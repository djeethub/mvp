#pragma once

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
    AVFrame *video_frame = nullptr;
    std::queue<AVFrame *> frame_queue;
    std::queue<AVFrame *> frame_trash;
    double tick_diff = 0;
    int64_t first_video_pts = AV_NOPTS_VALUE;
    double video_time_base = 0.0;
    double audio_time_base = 0.0;
    SDL_AudioDeviceID audio_device_id = 0;
    std::thread fetch_thread;
    float video_scale = 1.0;
    float video_pan_x = 0.0;
    float video_pan_y = 0.0;
    bool is_loop = true;
    bool need_play_time_update = true;
    std::mutex fetch_mutex;
    std::condition_variable fetch_cv;
    int fetch_status = 0;   // 0 = running, 1 = reset, -1 = shutdown

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
        while (!frame_queue.empty()) {
            av_frame_free(&frame_queue.front());
            frame_queue.pop();
        }
        while (!frame_trash.empty()) {
            av_frame_free(&frame_trash.front());
            frame_trash.pop();
        }
        if (video_frame)
            av_frame_free(&video_frame);
    }

    void trash_frame_buffers()
    {
        if (video_frame) {
            frame_trash.push(video_frame);
            video_frame = nullptr;
        }
        while (!frame_queue.empty())
        {
            frame_trash.push(frame_queue.front());
            frame_queue.pop();
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
        clear_frame_buffers();
        video.close();
        first_video_pts = AV_NOPTS_VALUE;
    }

    bool shutdown() {
        {
            std::lock_guard<std::mutex> lock(fetch_mutex);
            fetch_status = -1;
        }
        if (fetch_thread.joinable())
            fetch_thread.join();
        SDL_FlushEvents(SDL_EVENT_FIRST, SDL_EVENT_LAST);
        return true;
    }

    bool load_image_at_index() {
        if (image_files.empty() || !renderer) return false;

        std::lock_guard<std::mutex> lock(fetch_mutex);
        reset_runtime_state();
        trash_frame_buffers();

        const auto &filename = image_files[current_index];
        fs::path full = fs::path(parent_dir) / filename;

        if (!video.open(full.string())) {
            return false;
        }

        if (video.find_audio_stream()) {
            if (video.open_audio_decoder()) {
                video.setup_swr_context();

                audio_time_base = video.get_audio_time_base();
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
                
                video_time_base = video.get_video_time_base();
                int w, h;
                video.get_video_dimensions(w, h);

                // Create an SDL3 texture that matches the video dimensions
                SDL_Texture* tex = SDL_CreateTexture(
                    renderer.get(), 
                    SDL_PIXELFORMAT_IYUV,   // Super fast native YUV streaming
                    SDL_TEXTUREACCESS_STREAMING, 
                    w, 
                    h
                );
                if (!tex) return false;
                SDL_GetTextureSize(tex, &img_w, &img_h);
                texture.reset(tex);
            }
        }

        read_next_frame();
        image_aspect = img_h > 0.0f ? img_w / img_h : 1.0f;
        resize_window();
        SDL_SetWindowTitle(window.get(), filename.c_str());

        fetch_status = 1;
        if (!fetch_thread.joinable())
            fetch_thread = std::thread(fetch_thread_worker, this);
        return true;
    }

    bool read_next_frame() {
        bool done = false;
        while (!done) {
            auto read_result = video.feed_frame([&](uint8_t *out_data, int out_size) -> void {
                // Feed the raw sound bytes to SDL3's background mixer
                if (audio_stream && !SDL_PutAudioStreamData(audio_stream.get(), out_data, out_size)) {
                    SDL_Log("Audio Stream Error: %s", SDL_GetError());
                }
                if (!video.is_video()) {
                    done = true;
                }
            },[&](AVFrame *frame) -> void {
                AVFrame *new_frame;
                if (!frame_trash.empty()) {
                    new_frame = frame_trash.front();
                    frame_trash.pop();
                } else
                    new_frame = video.alloc_converted_frame();
                video.scale_video_frame(frame, new_frame);
                new_frame->pts = frame->pts;
                if (first_video_pts == AV_NOPTS_VALUE && frame->pts != AV_NOPTS_VALUE)
                {
                    first_video_pts = frame->pts;
                }
                frame_queue.push(new_frame);
                done = true;
            });
            if (read_result < 0) {
                if (read_result == AVERROR_EOF && is_loop) {
                    seek(0, false);
                    continue;
                }
                return false;
            }
        }
        return true;
    }

    uint32_t time_next_frame(uint32_t interval = 200)
    {
        if (video.is_video() && !frame_queue.empty()) {
            if (video_frame)
                frame_trash.push(video_frame); // Move the current frame to the trash queue
            video_frame = frame_queue.front();
            frame_queue.pop(); // Remove the frame from the queue
            if (need_play_time_update || video_frame->pts == first_video_pts)
            {
                set_play_time(video_frame->pts * video_time_base);
            }
            SDL_Event event;
            SDL_zero(event);
            event.type = USEREVENT_NEXT_FRAME;
            // Push it to the main event loop
            SDL_PushEvent(&event);
        }
        read_next_frame(); // Preload the next frame
        if (video.is_video()) {
            if (frame_queue.empty()) {
                return 0; // Stop the timer if no more frames are available
            }
            auto curr_ticks = get_ticks();
            while (true) {
                auto frame = frame_queue.front();
                if (frame->pts != AV_NOPTS_VALUE) {
                    if (frame->pts == first_video_pts) {
                        break;
                    }
                    auto frame_time = frame->pts * video_time_base + tick_diff; // Convert to milliseconds
                    if (frame_time > curr_ticks)
                    {
                        interval = static_cast<uint32_t>((frame_time - curr_ticks) * 1000.0);
                        break;
                    } else {
                        if (video_frame)
                            frame_trash.push(video_frame);
                        video_frame = frame_queue.front();
                        frame_queue.pop();
                    }
                } else {
                    frame_trash.push(frame);
                    frame_queue.pop();
                }
                read_next_frame();
                if (frame_queue.empty()) {
                    interval = 0; // Stop the timer if no more frames are available
                    break;
                }
            }
        } else {
            while (!audio_stream || SDL_GetAudioStreamQueued(audio_stream.get()) < 44100 * 2) {
                if (!read_next_frame()) {
                    break;
                }
            }
        }
        return interval;
    }

    bool seek(double ts, bool reset) {
        if (reset)
        {
            std::lock_guard<std::mutex> lock(fetch_mutex);
            if (video.seek(static_cast<int64_t>(ts * AV_TIME_BASE)) >= 0)
            {
                trash_frame_buffers();
                need_play_time_update = true;
                read_next_frame();
                fetch_status = 1;
                return true;
            }
        } else if (video.seek(ts) >= 0)
        {
            return true;
        }
        return false;
    }

    bool seek_relative(double t) {
        auto target_time = get_play_time();
        auto duration = video.get_duration();
        target_time += t;
        if (target_time < 0.0)
            target_time = 0.0;
        else if (target_time > duration)
            target_time = duration;
        return seek(target_time, true);
    }

    static void fetch_thread_worker(AppState *state)
    {
        uint32_t interval = 200;
        while (true)
        {
            std::unique_lock<std::mutex> lock(state->fetch_mutex);
            if (state->fetch_status < 0)
                break;
            state->fetch_status = 0;
            interval = state->time_next_frame(interval);
            if (interval == 0)
                break;
            state->fetch_cv.wait_for(lock, std::chrono::milliseconds(interval), [state]{ return state->fetch_status != 0; });
        }
    }

    static double get_ticks() {
        return static_cast<double>(SDL_GetTicks()) / 1000.0;
    }

    void set_play_time(double play_time)
    {
        tick_diff = get_ticks() - play_time;
        need_play_time_update = false;
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
};
