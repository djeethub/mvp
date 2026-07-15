#pragma once

#define _VIDEO_CONVERTER_THREAD_

#include <string>
#include <vector>
#include <memory>
#include <filesystem>
#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <queue>
#include <mutex>
#include <condition_variable>

#include "utils.hpp"
#include "ffmpeg.hpp"
#ifdef _VIDEO_CONVERTER_THREAD_
#include "thread.hpp"
#endif

// Define a unique event ID for our frame ticker
#define USEREVENT_NEXT_FRAME (SDL_EVENT_USER + 1)
#define USEREVENT_DECODE_TICK (SDL_EVENT_USER + 2)
#define USEREVENT_QUIT (SDL_EVENT_USER + 3)

namespace fs = std::filesystem;

using WindowPtr = std::unique_ptr<SDL_Window, decltype(&SDL_DestroyWindow)>;
using RendererPtr = std::unique_ptr<SDL_Renderer, decltype(&SDL_DestroyRenderer)>;
using TexturePtr = std::unique_ptr<SDL_Texture, decltype(&SDL_DestroyTexture)>;
using AudioStream = std::unique_ptr<SDL_AudioStream, decltype(&SDL_DestroyAudioStream)>;

uint32_t SDLCALL TimerCallback(void* userdata, SDL_TimerID timerID, uint32_t interval);

struct AppState {
    std::vector<std::string> image_files;
    std::size_t current_index = 0;
    std::string parent_dir;
    bool trigger_context_menu = false;
    float image_aspect = 1.0f;

    WindowPtr window{nullptr, SDL_DestroyWindow};
    RendererPtr renderer{nullptr, SDL_DestroyRenderer};
    TexturePtr texture{nullptr, SDL_DestroyTexture};
    AudioStream audio_stream{nullptr, SDL_DestroyAudioStream};

    ff::VideoFile video;
    std::atomic<AVFrame *> video_frame;
    ff::AudioBuffer audio_buf;
    double tick_diff = 0;
    int64_t last_video_pts = SDL_MAX_SINT64;
    int64_t last_audio_pts = SDL_MAX_SINT64;
    SDL_AudioDeviceID audio_device_id = 0;
    std::thread fetch_thread;
    float video_scale = 1.0;
    float video_pan_x = 0.0;
    float video_pan_y = 0.0;
    bool is_loop = true;
    bool is_looping = false;
    std::mutex fetch_mutex;
    std::condition_variable fetch_cv;
    int fetch_status = 0;   // 0 = running, 1 = reset, -1 = shutdown
    std::vector<ff::ChapterData> chapter_list;
    double pause_time;
    bool is_paused = false;
#ifdef _VIDEO_CONVERTER_THREAD_
    VideoConverter video_converter;
#endif

    AppState() = default;
    ~AppState()
    {
        reset_runtime_state();
    }

    static bool is_supported_image(const fs::path &p) {
        if (!p.has_extension()) return false;
        auto ext = p.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c){ return std::tolower(c); });
        return (ext == ".mp4" || ext == ".mkv" || ext == ".avi" || ext == ".mov" || ext == ".flv" || ext == ".wmv" || ext == ".webm");
    }

    void clear_frame_buffers() {
        video_frame = nullptr;
        {
#ifdef _VIDEO_CONVERTER_THREAD_
            std::lock_guard<std::mutex> lock(video_converter.mtx_);
            video.video_frame_queue.clear();
#endif
            video.video_converted_queue.clear();
        }
    }

    void recycle_frame_buffers()
    {
        video_frame = nullptr;
        {
#ifdef _VIDEO_CONVERTER_THREAD_
            std::lock_guard<std::mutex> lock(video_converter.mtx_);
            video.video_frame_queue.recycle();
#endif
            video.video_converted_queue.recycle(false);
        }
        if (audio_stream)
            SDL_ClearAudioStream(audio_stream.get());
    }

    void reset_runtime_state() {
        if (audio_device_id != 0) {
            SDL_CloseAudioDevice(audio_device_id);
            audio_device_id = 0;
        }
        audio_stream.reset();
        texture.reset();
        video.close();
        clear_frame_buffers();
        last_video_pts = SDL_MAX_SINT64;
        last_audio_pts = SDL_MAX_SINT64;
        is_looping = false;
    }

    bool shutdown() {
        {
            std::lock_guard<std::mutex> lock(fetch_mutex);
            fetch_status = -1;
        }
        fetch_cv.notify_all();
        if (fetch_thread.joinable())
            fetch_thread.join();
#ifdef _VIDEO_CONVERTER_THREAD_
        video_converter.stop();
#endif
        SDL_FlushEvents(SDL_EVENT_FIRST, SDL_EVENT_LAST);
        return true;
    }

    bool load_image_at_index() {
        if (image_files.empty() || !renderer) return false;

        std::lock_guard<std::mutex> lock(fetch_mutex);
        reset_runtime_state();

        const auto &filename = image_files[current_index];
        fs::path full = fs::path(parent_dir) / filename;

        if (!video.open(full.string())) {
            return false;
        }

        if (video.find_audio_stream()) {
            if (video.open_audio_decoder()) {
                video.setup_swr_context();

                int count = 0;
                auto *devices = SDL_GetAudioPlaybackDevices(&count);
                if (count > 0) {
                    SDL_AudioSpec target_spec = { SDL_AUDIO_S16LE, 2, 44100 }; // Standard CD quality format
                    auto stream = SDL_CreateAudioStream(&target_spec, &target_spec);
                    if (!stream) {
                        SDL_Log("Failed to create audio stream: %s", SDL_GetError());
                        return false;
                    }
                    // Open a real logical connection to the system soundcard
                    SDL_AudioDeviceID dev_id = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, nullptr);
                    if (dev_id == 0) {
                        SDL_Log("Failed to open physical audio device: %s", SDL_GetError());
                        return false;
                    }
                    audio_stream.reset(stream);
                    audio_device_id = dev_id;
                    SDL_BindAudioStream(dev_id, audio_stream.get());
                    SDL_ResumeAudioDevice(dev_id);
                }
                else
                    SDL_Log("Audio Error: %s", SDL_GetError());
                SDL_free(devices);
            }
        }

        float img_w = 640, img_h = 480;
        if (video.find_video_stream()) {
            if (video.open_video_decoder()) {
                video.setup_sws_context();
                
                int w, h;
                video.get_video_dimensions(w, h);

                // Create an SDL3 texture that matches the video dimensions
                SDL_Texture* tex = SDL_CreateTexture(
                    renderer.get(), 
                    SDL_PIXELFORMAT_NV12,   // Super fast native YUV streaming
                    SDL_TEXTUREACCESS_STREAMING, 
                    w, 
                    h
                );
                if (!tex) return false;
                SDL_GetTextureSize(tex, &img_w, &img_h);
                texture.reset(tex);
            }
        }

        chapter_list = video.ReadChapters();

        image_aspect = img_h > 0.0f ? img_w / img_h : 1.0f;
        resize_window();
        SDL_SetWindowTitle(window.get(), filename.c_str());

        fetch_status = 1;
        fetch_cv.notify_all();
        if (!fetch_thread.joinable()) {
            fetch_thread = std::thread(fetch_thread_worker, this);
            pthread_setname_np(fetch_thread.native_handle(), "fetch");
        }
#ifdef _VIDEO_CONVERTER_THREAD_
        video_converter.video = &video;
        video_converter.start();
#endif
        read_next_frame(0);
        return true;
    }

    int read_next_frame(double play_time) {
        int read_result = 0;
        while (true) {
#ifdef _VIDEO_CONVERTER_THREAD_
            auto video_frame_count = video_converter.count_video_frame();
#else
            auto video_frame_count = video.video_converted_queue.size();
#endif
            if (video_frame_count >= 2 && (!audio_stream || SDL_GetAudioStreamQueued(audio_stream.get()) > 22222))
                break;
            read_result = video.feed_frame([&](AVFrame *frame) -> void {
                if (audio_stream) {
//                    printf("pts: %f, play_time: %f, looping: %i\n", frame->pts * audio_time_base, play_time, is_looping);
                    if (!is_looping && frame->pts * video.audio_time_base < play_time)
                        return;
                    video.convert_audio_frame(frame, &audio_buf);
//                    if (frame->pts < last_audio_pts)
//                        set_play_time(frame->pts * audio_time_base);
                    last_audio_pts = frame->pts;
                    // Feed the raw sound bytes to SDL3's background mixer
                    if (!SDL_PutAudioStreamData(audio_stream.get(), audio_buf.buf, audio_buf.data_size)) {
                        SDL_Log("Audio Stream Error: %s", SDL_GetError());
                    }
//                    printf("SDL_GetAudioStreamQueued: %i\n", SDL_GetAudioStreamQueued(audio_stream.get()));
                }
            });
            if (video_frame_count < 2) {
                video.feed_video_frame([&](AVFrame *frame) -> void {
                    auto frame_time = frame->pts * video.video_time_base;
                    if (!is_looping && frame_time < play_time)
                        return;
#ifdef _VIDEO_CONVERTER_THREAD_
                    {
                        std::lock_guard<std::mutex> lock(video_converter.mtx_);
                        auto new_frame = video.video_frame_queue.alloc();
                        av_frame_ref(new_frame, frame);
                        video.video_frame_queue.push(new_frame);
                    }
                    video_converter.cv_.notify_one();
#else
                    AVFrame *new_frame = video.video_converted_queue.alloc([this]{ return video.alloc_converted_frame();});
                    video.scale_video_frame(frame, new_frame);
                    new_frame->pts = frame->pts;
                    video.video_converted_queue.push(new_frame);
#endif
                    if (frame->pts < last_video_pts) {
                        is_looping = false;
                        set_play_time(frame_time);
                    }
                });
            }
            if (read_result < 0)
                break;
        }
        return read_result;
    }

    bool check_next_frame(double curr_ticks) {
        if (video.is_video()) {
            AVFrame *frame_to_display = nullptr;
            {
#ifdef _VIDEO_CONVERTER_THREAD_
                std::unique_lock<std::mutex> lock(video_converter.mtx_);
#endif
                while (!video.video_converted_queue.empty()) {
                    auto frame = video.video_converted_queue.front();
                    if (frame->pts < last_video_pts) {
                        last_video_pts = frame->pts;
                        frame_to_display = frame;
                        video.video_converted_queue.pop();
                        video.video_converted_queue.recycle(frame, false);
                        break;
                    }
                    else
                    {
                        auto frame_time = frame->pts * video.video_time_base + tick_diff;
                        if (frame_time <= curr_ticks) {
                            last_video_pts = frame->pts;
                            frame_to_display = frame;
                            video.video_converted_queue.pop();
                            video.video_converted_queue.recycle(frame, false);
                        } else
                            break;
                    }
                }
            }
            if (frame_to_display)
            {
#ifdef _VIDEO_CONVERTER_THREAD_
                video_converter.cv_.notify_one();
#endif
                video_frame.store(frame_to_display, std::memory_order_release);
                SDL_Event event;
                SDL_zero(event);
                event.type = USEREVENT_NEXT_FRAME;
                SDL_PushEvent(&event);
                return true;
            }
        }

        return false;
    }

    double time_next_frame(double interval = 0.2)
    {
        if (is_paused)
            return 77777;
        auto curr_ticks = get_ticks();
        check_next_frame(curr_ticks);
        auto play_time = curr_ticks - tick_diff;
        if (video.is_video()) {
            auto rlt = read_next_frame(play_time);
            if (rlt < 0) {
                if (rlt == AVERROR_EOF) {
#ifdef _VIDEO_CONVERTER_THREAD_
                    if (is_loop && video_converter.empty()) {
#else
                    if (is_loop && video.video_converted_queue.empty()) {
#endif
                        if (seek(video.get_start_time(), false)) {
                            is_looping = true;
                            if (audio_stream) {
                                auto bytes = SDL_GetAudioStreamQueued(audio_stream.get());
                                if (bytes > 0) {
                                    return static_cast<double>(bytes) / (44100 * 2 * sizeof(int16_t));
                                }
                            }
                            auto frame = video_frame.load(std::memory_order_relaxed);
                            if (frame && frame->duration > 0)
                                return frame->duration * video.video_time_base;
                            else
                                return 0.001;
                        }
                    }
                }
            }
#ifdef _VIDEO_CONVERTER_THREAD_
            if (video_converter.empty()) {
#else
            if (video.video_converted_queue.empty()) {
#endif
                return 0; // Stop the timer if no more frames are available
            }
            curr_ticks -= tick_diff;
#ifdef _VIDEO_CONVERTER_THREAD_
            double frame_time = video_converter.next_play_time(curr_ticks);
#else
            auto frame_time = video.video_converted_queue.front()->pts * video.video_time_base;
#endif
            interval = frame_time - curr_ticks;
            if (interval <= 0)
                interval = 0.001;
        } else {
            auto rlt = read_next_frame(play_time);
            if (rlt < 0) {
                if (rlt == AVERROR_EOF && is_loop) {
                    if (seek(0, false)) {
                        return 0.01;
                    }
                }
                return 0;
            }
        }
        return interval;
    }

    bool seek(double ts, bool reset) {
        auto rlt = false;
        if (reset)
        {
            {
                std::lock_guard<std::mutex> lock(fetch_mutex);
                if (video.seek(static_cast<int64_t>(ts * AV_TIME_BASE)) >= 0)
                {
                    recycle_frame_buffers();
                    set_play_time(ts);
                    read_next_frame(ts);
                    fetch_status = 1;
                    rlt = true;
                }
            }
            if (rlt)
                fetch_cv.notify_one();
        } else if (video.seek(ts) >= 0)
        {
            rlt = true;
        }
        return rlt;
    }

    bool seek_relative(double t) {
        auto duration = video.get_duration();
        auto target_time = get_play_time();
        target_time += t;
        auto start_time = video.get_start_time();
        if (target_time < start_time)
            target_time = start_time;
        else if (target_time > duration)
            target_time = duration;
        return seek(target_time, true);
    }

    bool seek_ratio(double t) {
        return seek(t * video.get_duration(), true);
    }

    bool seek_to_chapter(int id) {
        auto chapter = chapter_list[id];
        return seek(chapter.start_time, true);
    }

    int get_relative_chapter(int n) {
        if (chapter_list.size() <= 1)
            return -1;

        auto play_time = get_play_time();
        for (int i = 0; i < chapter_list.size(); i++) {
            auto chapter = chapter_list[i];
            if (play_time >= chapter.start_time && play_time <= chapter.end_time) {
                auto id = i;
                if (n > 0)
                    id += n;
                else if (n < 0) {
                    if (play_time < chapter.start_time + 1)
                        id += n;
                    else
                        id += n + 1;
                }
                if (id >= 0 && id < chapter_list.size()) {
                    return id;
                }
                break;
            }
        }

        return -1;
    }
    
    static void fetch_thread_worker(AppState *state)
    {
        double interval = 0.2;
        while (true)
        {
            std::unique_lock<std::mutex> lock(state->fetch_mutex);
            if (state->fetch_status < 0)
                break;
            state->fetch_status = 0;
            interval = state->time_next_frame(interval);
            if (interval == 0)
                break;
            state->fetch_cv.wait_for(lock, std::chrono::microseconds(static_cast<int64_t>(interval * 1000000)), [state]{ return state->fetch_status != 0; });
        }
    }

    static double get_ticks() {
        return static_cast<double>(SDL_GetPerformanceCounter()) / SDL_GetPerformanceFrequency();
    }

    void set_play_time(double play_time)
    {
        tick_diff = get_ticks() - play_time;
    }

    double get_play_time() const {
        return get_ticks() - tick_diff;
    }

    void resize_window(float window_scale = 1.0) {
        if (!texture)
            return;
        float img_w, img_h;
        SDL_GetTextureSize(texture.get(), &img_w, &img_h);

        SDL_DisplayID primary_display = SDL_GetPrimaryDisplay();
        SDL_Rect display_bounds;
        if (!SDL_GetDisplayUsableBounds(primary_display, &display_bounds))
        {
            display_bounds.x = 0;
            display_bounds.y = 0;
            display_bounds.w = 1920;
            display_bounds.h = 1080;
        }

        int current_x = 0, current_y = 0;
        int current_w = 0, current_h = 0;
        SDL_GetWindowPosition(window.get(), &current_x, &current_y);
        SDL_GetWindowSize(window.get(), &current_w, &current_h);
        int center_x = current_x + current_w / 2;
        int center_y = current_y + current_h / 2;

        // Compute new target size with scaling to fit display
        int target_w = static_cast<int>(img_w) * window_scale;
        int target_h = static_cast<int>(img_h) * window_scale;
        if (target_w > display_bounds.w || target_h > display_bounds.h)
        {
            float scale = SDL_min(static_cast<float>(display_bounds.w) / img_w,
                                  static_cast<float>(display_bounds.h) / img_h);
            target_w = static_cast<int>(img_w * scale);
            target_h = static_cast<int>(img_h * scale);
        }

        int new_x = center_x - target_w / 2;
        int new_y = center_y - target_h / 2;
        if (new_x < display_bounds.x)
            new_x = display_bounds.x;
        if (new_y < display_bounds.y)
            new_y = display_bounds.y;
        if (new_x + target_w > display_bounds.x + display_bounds.w)
            new_x = display_bounds.x + display_bounds.w - target_w;
        if (new_y + target_h > display_bounds.y + display_bounds.h)
            new_y = display_bounds.y + display_bounds.h - target_h;

        SDL_SetWindowSize(window.get(), target_w, target_h);
        SDL_SetWindowPosition(window.get(), new_x, new_y);
    }

    void pause() {
        if (is_paused) {
            {
                std::lock_guard<std::mutex> lock(fetch_mutex);
                is_paused = false;
                fetch_status = 2;
                tick_diff += get_ticks() - pause_time;
                SDL_ResumeAudioStreamDevice(audio_stream.get());
            }
            fetch_cv.notify_one();
        }
        else
        {
            std::lock_guard<std::mutex> lock(fetch_mutex);
            SDL_PauseAudioStreamDevice(audio_stream.get());
            pause_time = get_ticks();
            is_paused = true;
        }
    }
};
