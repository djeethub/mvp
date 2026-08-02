#pragma once

//#define _VIDEO_CONVERTER_THREAD_

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
#include <future>

#include "ffmpeg.hpp"
#ifdef _VIDEO_CONVERTER_THREAD_
#include "thread.hpp"
#endif
#include "ass.hpp"
#include "readerwriterqueue.h"
#include "twowayqueue.hpp"
#include "gpu.hpp"

// Define a unique event ID for our frame ticker
Uint32 USEREVENT_NEXT_FRAME;
Uint32 USEREVENT_SUBTITLE_ASS;
#define NUM_USEREVENT   2

namespace fs = std::filesystem;

extern AssHandler ass;

using WindowPtr = std::unique_ptr<SDL_Window, decltype(&SDL_DestroyWindow)>;
using RendererPtr = std::unique_ptr<SDL_Renderer, decltype(&SDL_DestroyRenderer)>;
using TexturePtr = std::unique_ptr<SDL_Texture, decltype(&SDL_DestroyTexture)>;
using AudioStream = std::unique_ptr<SDL_AudioStream, decltype(&SDL_DestroyAudioStream)>;

uint32_t SDLCALL TimerCallback(void* userdata, SDL_TimerID timerID, uint32_t interval);

enum MediaMode {
    None = 0,
    Video,
    Image
};

struct Subtitle {
    std::string text;
    double pts;
    double duration;
};

struct DirData {
    std::string parent_dir;
    std::vector<std::string> list;
    int idx;
};

struct AppState {
    std::vector<std::string> image_files;
    std::size_t current_index = -1;
    std::string parent_dir;
    bool trigger_context_menu = false;

    WindowPtr window{nullptr, SDL_DestroyWindow};
    RendererPtr renderer{nullptr, SDL_DestroyRenderer};
    TexturePtr texture{nullptr, SDL_DestroyTexture};
    AudioStream audio_stream{nullptr, SDL_DestroyAudioStream};

    ff::VideoFile video;
    GPUPipeline gpu;
    std::atomic<AVFrame *> video_frame;
    TwowayQueue<Subtitle *> sub_queue;
    ff::AudioBuffer audio_buf;
    double tick_diff = 0;
    SDL_AudioDeviceID audio_device_id = 0;
    std::thread fetch_thread;
    float video_scale = 1.0;
    float video_pan_x = 0.0;
    float video_pan_y = 0.0;
    bool is_loop = true;
    bool is_seeking = true;
    double seek_time;
    std::mutex fetch_mutex;
    std::condition_variable fetch_cv;
    int fetch_status = 0;   // 0 = running, 1 = reset, -1 = shutdown
    std::vector<ff::ChapterData> chapter_list;
    bool is_paused = false;
#ifdef _VIDEO_CONVERTER_THREAD_
    VideoConverter video_converter;
#endif
    std::future<DirData *> dir_future;
    static inline const std::unordered_set<std::string> video_exts = { ".mp4", ".mkv", ".mov", ".flv", ".wmv", ".webm" };
    static inline const std::unordered_set<std::string> image_exts = { ".png", ".jpg", ".jpeg", ".bmp", ".webp", ".gif" };
    MediaMode media_mode;

    AppState() : sub_queue(32) {
#ifdef _VIDEO_CONVERTER_THREAD_
        video_converter.video = &video;
#endif
        auto n = SDL_RegisterEvents(NUM_USEREVENT);
        USEREVENT_NEXT_FRAME = n++;
        USEREVENT_SUBTITLE_ASS = n++;
    }

    ~AppState()
    {
        reset_runtime_state();
    }

    static MediaMode is_supported_format(const fs::path &p, MediaMode mode = None) {
        if (!p.has_extension()) return None;
        auto ext = p.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c){ return std::tolower(c); });
        switch (mode) {
            case None:
                if (video_exts.contains(ext))
                    return Video;
                if (image_exts.contains(ext))
                    return Image;
                break;

            case Video:
                if (video_exts.contains(ext))
                    return Video;

            case Image:
                if (image_exts.contains(ext))
                    return Image;
        }
        return None;
    }

    void clear_frame_buffers() {
#ifdef _VIDEO_CONVERTER_THREAD_
        video_converter.clear();
#else
        video.video_frame_queue.clear();
#endif
        if (audio_stream)
            SDL_ClearAudioStream(audio_stream.get());
        ass.flush();
        sub_queue.recycle_all();
    }

    void reset_runtime_state() {
        if (audio_device_id != 0) {
            SDL_CloseAudioDevice(audio_device_id);
            audio_device_id = 0;
        }
        audio_stream.reset();
        video.close();
        clear_frame_buffers();
        texture.reset();
        is_seeking = true;
    }

    bool shutdown() {
        {
            std::lock_guard<std::mutex> lock(fetch_mutex);
            fetch_status = -1;
        }
        fetch_cv.notify_one();
        if (fetch_thread.joinable())
            fetch_thread.join();
#ifdef _VIDEO_CONVERTER_THREAD_
        video_converter.stop();
#endif
        SDL_FlushEvents(SDL_EVENT_FIRST, SDL_EVENT_LAST);
        return true;
    }

    static DirData *dir_worker(const std::string file_path, MediaMode mode) {
        auto data = new DirData;
        fs::path argpath = file_path;
        if (argpath.has_parent_path()) {
            data->parent_dir = argpath.parent_path().string();
            if (data->parent_dir.back() != fs::path::preferred_separator) data->parent_dir.push_back(fs::path::preferred_separator);
        } else {
            data->parent_dir = std::string("./");
        }
        auto file_name = argpath.filename().string();

        auto& list = data->list;
        // Enumerate directory for supported images
        try {
            for (auto &entry : fs::directory_iterator(data->parent_dir)) {
                if (!entry.is_regular_file()) continue;
                if (is_supported_format(entry.path(), mode)) {
                    list.push_back(entry.path().filename().string());
                }
            }
            // remove duplicates and sort
            std::sort(list.begin(), list.end());
            list.erase(std::unique(list.begin(), list.end()), list.end());

            // find initial file index
            auto it = std::find(list.begin(), list.end(), file_name);
            if (it != list.end()) data->idx = static_cast<std::size_t>(std::distance(list.begin(), it));
        } catch (...) {
            // filesystem errors -> failure
        }
        
        return data;
    }

    bool open_file(const char *file_path) {
        auto mode = is_supported_format(file_path);
        if (mode == None) {
            std::cerr << "Not supported file: " << file_path << "\n";
            return false;
        }

        if (dir_future.valid())
            delete dir_future.get();

        if (open_video(file_path)) {
            media_mode = mode;
            dir_future = std::async(dir_worker, file_path, mode);
            image_files.clear();
            image_files.push_back(fs::path(file_path).filename().string());
            current_index = 0;
        }

        return true;
    }

    bool open_next_file(bool next) {
        if (dir_future.valid())
        {
            auto *data = dir_future.get();
            image_files = std::move(data->list);
            parent_dir = std::move(data->parent_dir);
            current_index = data->idx;
            delete data;
        }

        if (next) {
            if (current_index < image_files.size() - 1) {
                current_index++;
            } else
                return false;
        } else {
            if (current_index > 0) {
                current_index--;
            } else
                return false;
        }

        return open_video(fs::path(parent_dir) / image_files[current_index]);
    }

    bool open_video(const std::string& file_path) {
        std::unique_lock<std::mutex> lock(fetch_mutex);
        reset_runtime_state();

        if (!video.open(file_path)) {
            return false;
        }

        if (video.find_audio_stream()) {
            if (video.open_audio_decoder()) {
//                video.setup_swr_context();

                int count = 0;
                auto *devices = SDL_GetAudioPlaybackDevices(&count);
                if (count > 0) {
                    SDL_AudioSpec src_spec = { SDL_AUDIO_F32, video.get_audio_channels(), video.get_audio_sample_rate() };
                    auto stream = SDL_CreateAudioStream(&src_spec, NULL);
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
//                    SDL_ResumeAudioDevice(dev_id);
                }
                else
                    SDL_Log("Audio Error: %s", SDL_GetError());
                SDL_free(devices);
            }
        }

        if (video.find_video_stream()) {
            if (video.open_video_decoder()) {
            }
        }

        if (video.find_subtitle_stream()) {
            if (video.open_subtitle_decoder()) {
            }
        }

        chapter_list = video.read_chapters();

        resize_window();
        SDL_SetWindowTitle(window.get(), file_path.c_str());
        
        gpu.init_pipeline(COMMON_FLAG); // todo: format check
        auto subtitle_ctx = video.get_subtitle_ctx();
        if (subtitle_ctx) {
            int target_w, target_h;
            video.get_video_dimensions(target_w, target_h);
            ass.init(gpu.get_device(), gpu.get_sampler());
            ass.init(target_w, target_h, subtitle_ctx, window.get());
        }

#ifdef _VIDEO_CONVERTER_THREAD_
        video_converter.start();
#endif
        seek_time = video.get_start_time();
        read_next_frame(seek_time);
        fetch_status = 1;
        if (!fetch_thread.joinable()) {
            fetch_thread = std::thread(fetch_thread_worker, this);
            pthread_setname_np(fetch_thread.native_handle(), "fetch");
        }
        lock.unlock();
        fetch_cv.notify_one();
        return true;
    }

    // Helper to isolate the Dialogue payload and extract the text
    std::string extract_dialogue_ass(const std::string& line) {
        size_t pos = 0;
        int comma_count = 0;

        // In rect->ass, we need to skip exactly 8 commas to reach the text
        while (comma_count < 8 && pos != std::string::npos) {
            pos = line.find(',', pos);
            if (pos != std::string::npos) {
                comma_count++;
                pos++; // Move right past the found comma
            }
        }

        std::string str;
        // If we successfully skipped 8 commas, slice out the remaining string
        if (comma_count == 8 && pos < line.length()) {
            str = line.substr(pos);

            std::string to_find = "\\N";
            pos = str.find(to_find);
            while ((pos = str.find(to_find)) != std::string::npos) {
                str.erase(pos, to_find.length());
            }
            return str;
        }

        return line; // Fallback if string is unexpected or malformed
    }

    void add_subtitle(const std::string& text, AVPacket *packet) {
        auto data = sub_queue.alloc();
        data->text = text;
        data->pts = packet->pts * video.get_subtitle_time_base();
        data->duration = packet->duration * video.get_subtitle_time_base();
        sub_queue.enqueue(std::move(data));
    }

    int read_next_frame(double play_time) {
        int read_result = 0;
        while (true) {
#ifdef _VIDEO_CONVERTER_THREAD_
            auto video_frame_count = video_converter.count_video_packet();
#else
            auto video_frame_count = video.video_frame_queue.size();
#endif
            if (video_frame_count >= 2 && (!video.is_audio() || SDL_GetAudioStreamQueued(audio_stream.get()) > 22222))
                break;
            read_result = video.feed_frame(play_time, [&](AVFrame *frame) -> void {
                if (!audio_stream)
                    return;
//                    printf("pts: %f, play_time: %f, looping: %i\n", frame->pts * audio_time_base, play_time, is_looping);
                if (is_seeking) {
                    play_time = frame->pts * video.get_audio_time_base();
                    set_seeking(false, play_time);
                    set_play_time(play_time);
                }
//                    video.convert_audio_frame(frame, &audio_buf);
                // Feed the raw sound bytes to SDL3's background mixer
                if (av_sample_fmt_is_planar(static_cast<AVSampleFormat>(frame->format))) {
                    // Perfect for FLTP (extracts from any standard video file container)
                    SDL_PutAudioStreamPlanarData(audio_stream.get(), (const void * const *)frame->data, frame->ch_layout.nb_channels, frame->nb_samples);
                } else {
                    // Perfect for packed/interleaved layouts (like FLT, S16, S32)
                    int size_in_bytes = frame->nb_samples * frame->ch_layout.nb_channels * av_get_bytes_per_sample(static_cast<AVSampleFormat>(frame->format));
                    SDL_PutAudioStreamData(audio_stream.get(), frame->data[0], size_in_bytes);
                }
            }, [&](AVPacket *packet) -> void {
#ifdef _VIDEO_CONVERTER_THREAD_
                {
                    auto new_packet = av_packet_alloc();
                    av_packet_move_ref(new_packet, packet);
                    std::lock_guard<std::mutex> lock(video_converter.mtx_);
                    video.video_packet_queue.push(new_packet);
                }
                video_converter.cv_.notify_one();
#else
                video.feed_video_frame(packet, [&](AVFrame *frame) {
                    if (frame->pts * video.get_video_time_base() < play_time) {
                        return;
                    }
                    AVFrame *new_frame = video.scale_video_frame(frame, frame->width, frame->height);
                    video.video_frame_queue.push(new_frame);
                });
#endif
            }, [&](AVSubtitle& subtitle, AVPacket *packet) {
                // Iterate through the subtitle rectangles (lines/images)
                for (unsigned int i = 0; i < subtitle.num_rects; i++) {
                    AVSubtitleRect* rect = subtitle.rects[i];
                    if (rect->type == SUBTITLE_TEXT && rect->text) {
                        add_subtitle(rect->text, packet);
                    } 
                    else if (rect->type == SUBTITLE_ASS && rect->ass) {
                        // ASS subtitles contain formatting markers (e.g., {\an8}) alongside text
//                        std::cout << "<" << subtitle.start_display_time << "ms> " << rect->ass << "\n";
//                        set_subtitle(extract_dialogue_ass(rect->ass), packet->duration * video.subtitle_time_base);
//                        ass.add_ass(rect, packet->pts * video.subtitle_time_base * 1000, packet->duration * video.subtitle_time_base * 1000);
                        add_subtitle(rect->ass, packet);
                    }
                }
                SDL_Event event;
                SDL_zero(event);
                event.type = USEREVENT_SUBTITLE_ASS;
                SDL_PushEvent(&event);
            });
            if (read_result < 0)
                break;
        }
        return read_result;
    }

    bool check_next_frame(double play_time) {
        if (video.is_video()) {
            AVFrame *frame_to_display = nullptr;
            bool need_fetch = false;
            {
#ifdef _VIDEO_CONVERTER_THREAD_
                std::lock_guard<std::mutex> lock(video_converter.mtx_);
#endif
                while (!video.video_frame_queue.empty())
                {
                    auto frame = video.video_frame_queue.front();
                    auto frame_time = frame->pts * video.get_video_time_base();
                    if (is_seeking) {
                        if (frame_time < play_time) {
                            video.video_frame_queue.pop();
                            ff::frame_recycle(frame);
                            need_fetch = true;
                            continue;
                        } else {
                            play_time = frame_time;
                            set_seeking(false, play_time);
                            set_play_time(play_time);
                        }
                    }
                    if (frame_time <= play_time) {
                        if (frame_to_display)
                            ff::frame_recycle(frame_to_display);
                        frame_to_display = frame;
                        video.video_frame_queue.pop();
                        need_fetch = true;
                    } else
                        break;
                }
            }
#ifdef _VIDEO_CONVERTER_THREAD_
            if (need_fetch)
                video_converter.cv_.notify_one();
#endif
            if (frame_to_display)
            {
                auto old_frame = video_frame.exchange(frame_to_display, std::memory_order_release);
                ff::frame_recycle(old_frame);

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
        if (is_paused && !is_seeking)
            return 77777;
        double play_time = is_seeking ? seek_time : get_play_time();
        check_next_frame(play_time);
        if (video.is_video()) {
            auto rlt = read_next_frame(play_time);
            if (rlt < 0) {
                if (rlt == AVERROR_EOF) {
#ifdef _VIDEO_CONVERTER_THREAD_
                    if (is_loop && video_converter.empty()) {
#else
                    if (is_loop && video.video_frame_queue.empty()) {
#endif
                        if (seek(video.get_start_time(), false)) {
                            if (audio_stream) {
                                auto bytes = SDL_GetAudioStreamQueued(audio_stream.get());
                                if (bytes > 0) {
                                    return static_cast<double>(bytes) / (44100 * 2 * sizeof(int16_t));
                                }
                            }
                            auto frame = video_frame.load(std::memory_order_relaxed);
                            if (frame && frame->duration > 0) {
                                return frame->duration * video.get_video_time_base();
                            } else {
                                return 0.001;
                            }
                        }
                    }
                }
            }
#ifdef _VIDEO_CONVERTER_THREAD_
            if (video_converter.empty()) {
#else
            if (video.video_frame_queue.empty()) {
#endif
                return 0; // Stop the timer if no more frames are available
            }
            if (is_seeking)
                return 0.001;
#ifdef _VIDEO_CONVERTER_THREAD_
            auto frame_time = video_converter.next_play_time();
#else
            auto frame_time = video.video_frame_queue.front()->pts * video.get_video_time_base();
#endif
            interval = frame_time - get_play_time();
//            printf("interval: %f\n", interval);
            if (interval <= 0)
                interval = 0.01;
        } else {
            auto rlt = read_next_frame(play_time);
            if (rlt < 0) {
                if (rlt == AVERROR_EOF && is_loop) {
                    if (seek(0, false)) {
                        return 0.02;
                    }
                }
                return 0;
            }
        }
        return interval;
    }

    bool seek(double ts, bool reset) {
        if (reset)
        {
            if (is_seeking)
                return false;
            {
                std::lock_guard<std::mutex> lock(fetch_mutex);
                if (video.seek(static_cast<int64_t>(ts * AV_TIME_BASE)) >= 0)
                {
                    set_seeking(true, ts);
                    read_next_frame(ts);
                    fetch_status = 1;
                } else
                    return false;
            }
            fetch_cv.notify_one();
            return true;
        } else if (video.seek(ts) >= 0) {
            is_seeking = true;
            seek_time = ts;
            return true;
        }
        return false;
    }

    void set_seeking(bool set, double ts) {
        if (set) {
            is_seeking = true;
            seek_time = ts;
            video.set_skip(AVDISCARD_NONREF, AVDISCARD_ALL, AVDISCARD_ALL);
            clear_frame_buffers();
        } else {
            is_seeking = false;
            seek_time = ts;
            video.set_skip(AVDISCARD_DEFAULT, AVDISCARD_DEFAULT, AVDISCARD_DEFAULT);
        }
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
                    if (play_time < chapter.start_time + 5)
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
        return is_paused ? seek_time : (get_ticks() - tick_diff);
    }

    void resize_window(float window_scale = 1.0) {
        int img_w, img_h;
        video.get_video_dimensions(img_w, img_h);

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
        std::unique_lock<std::mutex> lock(fetch_mutex);
        if (is_paused) {
            is_paused = false;
            fetch_status = 2;
            set_play_time(seek_time);
            lock.unlock();
            fetch_cv.notify_one();
            SDL_ResumeAudioStreamDevice(audio_stream.get());
        } else {
            SDL_PauseAudioStreamDevice(audio_stream.get());
            seek_time = get_play_time();
            is_paused = true;
        }
    }

    auto get_file_name() {
        return current_index >= 0 ? image_files[current_index] : nullptr;
    }

    void select_subtitle(int idx) {
        std::lock_guard<std::mutex> lock(fetch_mutex);
        if (video.get_subtitle_index() != idx) {
            ass.flush();
            video.select_subtitle(idx);
        }
    }

    void select_audio(int idx) {
        std::lock_guard<std::mutex> lock(fetch_mutex);
        if (video.get_audio_index() != idx) {
//            if (audio_stream)
//                SDL_FlushAudioStream(audio_stream.get());
            video.select_audio(idx);
        }
    }

    void create_texture(AVFrame *frame) {
        if (!texture || texture->w != frame->width || texture->h != frame->height) {
            auto sdl_format = ff::VideoScaler::av_to_sdl(static_cast<AVPixelFormat>(frame->format));
            printf("texture format: %x\n", sdl_format);
            SDL_Texture* tex = SDL_CreateTexture(
                renderer.get(),
                sdl_format,
                SDL_TEXTUREACCESS_STREAMING, 
                frame->width,
                frame->height
            );
            texture.reset(tex);
        }
    }
};
