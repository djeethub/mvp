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

#include "ffmpeg.hpp"
#ifdef _VIDEO_CONVERTER_THREAD_
#include "thread.hpp"
#endif
#include "ass.hpp"

// Define a unique event ID for our frame ticker
#define USEREVENT_NEXT_FRAME (SDL_EVENT_USER + 1)
#define USEREVENT_SUBTITLE (SDL_EVENT_USER + 2)
#define USEREVENT_SUBTITLE_ASS (SDL_EVENT_USER + 4)
#define USEREVENT_QUIT (SDL_EVENT_USER + 3)

namespace fs = std::filesystem;

using WindowPtr = std::unique_ptr<SDL_Window, decltype(&SDL_DestroyWindow)>;
using RendererPtr = std::unique_ptr<SDL_Renderer, decltype(&SDL_DestroyRenderer)>;
using TexturePtr = std::unique_ptr<SDL_Texture, decltype(&SDL_DestroyTexture)>;
using AudioStream = std::unique_ptr<SDL_AudioStream, decltype(&SDL_DestroyAudioStream)>;

uint32_t SDLCALL TimerCallback(void* userdata, SDL_TimerID timerID, uint32_t interval);

struct Subtitle {
    std::string text;
    double pts;
    double duration;
};

struct AppState {
    std::vector<std::string> image_files;
    std::size_t current_index = 0;
    std::string parent_dir;
    bool trigger_context_menu = false;

    WindowPtr window{nullptr, SDL_DestroyWindow};
    RendererPtr renderer{nullptr, SDL_DestroyRenderer};
    TexturePtr texture{nullptr, SDL_DestroyTexture};
    AudioStream audio_stream{nullptr, SDL_DestroyAudioStream};

    ff::VideoFile video;
    std::atomic<AVFrame *> video_frame;
    std::atomic<std::shared_ptr<Subtitle>> subtitle;
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
    bool is_seeking = false;
    double seek_time;
    std::mutex fetch_mutex;
    std::condition_variable fetch_cv;
    int fetch_status = 0;   // 0 = running, 1 = reset, -1 = shutdown
    std::vector<ff::ChapterData> chapter_list;
    double pause_time;
    bool is_paused = false;
    int last_pixfmt = AV_PIX_FMT_NONE;
#ifdef _VIDEO_CONVERTER_THREAD_
    VideoConverter video_converter;
#endif
    AssHandler ass;

    AppState() {
#ifdef _VIDEO_CONVERTER_THREAD_
        video_converter.video = &video;
#endif
    }

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
#ifdef _VIDEO_CONVERTER_THREAD_
        video_converter.clear();
#else
        video.video_frame_queue.clear();
#endif
        ass.flush();
    }

    void recycle_frame_buffers()
    {
#ifdef _VIDEO_CONVERTER_THREAD_
        video_converter.clear();
#else
        video.video_frame_queue.clear();
#endif
        if (audio_stream)
            SDL_ClearAudioStream(audio_stream.get());
        ass.flush();
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
        last_pixfmt = AV_PIX_FMT_NONE;
        texture.reset(nullptr);
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

    bool open_file(const char *file_path) {
        if (!is_supported_image(file_path)) {
            std::cerr << "Not supported file: " << file_path << "\n";
            return false;
        }

        current_index = 0;
        image_files.clear();

        fs::path argpath = file_path;
        if (argpath.has_parent_path()) {
            parent_dir = argpath.parent_path().string();
            if (parent_dir.back() != fs::path::preferred_separator) parent_dir.push_back(fs::path::preferred_separator);
            image_files.push_back(argpath.filename().string());
        } else {
            parent_dir = std::string("./");
            image_files.push_back(argpath.filename().string());
        }

        // Enumerate directory for supported images
        try {
            for (auto &entry : fs::directory_iterator(parent_dir)) {
                if (!entry.is_regular_file()) continue;
                if (is_supported_image(entry.path())) {
                    image_files.push_back(entry.path().filename().string());
                }
            }
            // remove duplicates and sort
            std::sort(image_files.begin(), image_files.end());
            image_files.erase(std::unique(image_files.begin(), image_files.end()), image_files.end());

            // find initial file index
            auto it = std::find(image_files.begin(), image_files.end(), argpath.filename().string());
            if (it != image_files.end()) current_index = static_cast<std::size_t>(std::distance(image_files.begin(), it));
        } catch (...) {
            // filesystem errors -> failure
            return false;
        }

        return true;
    }

    bool load_image_at_index() {
        if (image_files.empty() || !renderer) return false;

        std::unique_lock<std::mutex> lock(fetch_mutex);
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

        int img_w = 640, img_h = 480;
        if (video.find_video_stream()) {
            if (video.open_video_decoder()) {
                video.get_video_dimensions(img_w, img_h);
            }
        }

        if (video.find_subtitle_stream()) {
            if (video.open_subtitle_decoder()) {

            }
        }

        chapter_list = video.ReadChapters();
        auto subtitle_ctx = video.get_subtitle_ctx();
        if (subtitle_ctx)
            ass.init(img_w, img_h, subtitle_ctx);

        resize_window();
        SDL_SetWindowTitle(window.get(), filename.c_str());

        fetch_status = 1;
        if (!fetch_thread.joinable()) {
            fetch_thread = std::thread(fetch_thread_worker, this);
            pthread_setname_np(fetch_thread.native_handle(), "fetch");
        }
#ifdef _VIDEO_CONVERTER_THREAD_
        video_converter.start();
#endif
        read_next_frame(video.get_start_time());
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

    void set_subtitle(const std::string& text, AVPacket *packet) {
        auto data = std::make_shared<Subtitle>();
        data->text = text;
        data->pts = packet->pts * video.subtitle_time_base;
        data->duration = packet->duration * video.subtitle_time_base;
        AppState::subtitle.store(data, std::memory_order_release);
        SDL_Event event;
        SDL_zero(event);
        event.type = USEREVENT_SUBTITLE_ASS;
        SDL_PushEvent(&event);
    }

    int read_next_frame(double play_time) {
        int read_result = 0;
        while (true) {
#ifdef _VIDEO_CONVERTER_THREAD_
            auto video_frame_count = video_converter.count_video_packet();
#else
            auto video_frame_count = video.video_frame_queue.size();
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
                video.feed_video_frame(packet, [&](AVFrame *frame){
                    auto new_frame = video.video_frame_queue.alloc();
                    if (frame->hw_frames_ctx) {
                        new_frame->format = AV_PIX_FMT_NV12;
                        av_hwframe_transfer_data(new_frame, frame, 0);
                        new_frame->pts = frame->pts;
                        new_frame->duration = frame->duration;
    /*                    if (av_hwframe_map(new_frame, frame, AV_HWFRAME_MAP_READ) < 0) {
                            fprintf(stderr, "Mapping to DRM PRIME failed!\n");
                            av_frame_free(&new_frame);
                        }*/
                    } else
                        av_frame_ref(new_frame, frame);
                    video.video_frame_queue.push(new_frame);
                });
#endif
            }, [&](AVSubtitle& subtitle, AVPacket *packet){
                // Iterate through the subtitle rectangles (lines/images)
                for (unsigned int i = 0; i < subtitle.num_rects; i++) {
                    AVSubtitleRect* rect = subtitle.rects[i];
                    if (rect->type == SUBTITLE_TEXT && rect->text) {
                        set_subtitle(rect->text, packet);
                    } 
                    else if (rect->type == SUBTITLE_ASS && rect->ass) {
                        // ASS subtitles contain formatting markers (e.g., {\an8}) alongside text
//                        std::cout << "<" << subtitle.start_display_time << "ms> " << rect->ass << "\n";
//                        set_subtitle(extract_dialogue_ass(rect->ass), packet->duration * video.subtitle_time_base);
//                        ass.add_ass(rect, packet->pts * video.subtitle_time_base * 1000, packet->duration * video.subtitle_time_base * 1000);
                        set_subtitle(rect->ass, packet);
                    }
                }
            });
            if (read_result < 0)
                break;
        }
        return read_result;
    }

    bool check_next_frame(double curr_ticks) {
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
                    auto frame_time = frame->pts * video.video_time_base;
                    if (is_seeking) {
                        if (frame_time < seek_time) {
                            video.video_frame_queue.pop();
                            av_frame_free(&frame);
                            need_fetch = true;
                            continue;
                        } else {
                            last_video_pts = frame->pts;
                            is_seeking = false;
                        }
                    }
                    if (is_looping || frame_time + tick_diff <= curr_ticks) {
//                        printf("pts: %i\n", frame->pts);
                        if (frame->pts < last_video_pts) {
                            is_looping = false;
                            set_play_time(frame_time);
                        }
                        last_video_pts = frame->pts;
                        if (frame_to_display)
                            av_frame_free(&frame_to_display);
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
                if (frame_to_display->format == AV_PIX_FMT_NV12) {
                    auto old_frame = video_frame.exchange(frame_to_display, std::memory_order_release);
                    av_frame_free(&old_frame);
                } else {
                    auto converted_frame = video.alloc_converted_frame();
                    video.scale_video_frame(frame_to_display, converted_frame);
                    converted_frame->duration = frame_to_display->duration;
                    av_frame_free(&frame_to_display);
                    auto old_frame = video_frame.exchange(converted_frame, std::memory_order_release);
                    av_frame_free(&old_frame);
                }

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
                    if (is_loop && video.video_frame_queue.empty()) {
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
            if (video.video_frame_queue.empty()) {
#endif
                return 0; // Stop the timer if no more frames are available
            }
            if (is_seeking)
                return 0.001;
            curr_ticks = get_ticks();
            curr_ticks -= tick_diff;
#ifdef _VIDEO_CONVERTER_THREAD_
            auto frame_time = video_converter.next_play_time();
#else
            auto frame_time = video.video_frame_queue.front()->pts * video.video_time_base;
#endif
            interval = frame_time - curr_ticks;
//            printf("interval: %f\n", interval);
            if (interval <= 0)
                interval = 0.02;
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
        if (is_seeking)
            return false;
        if (reset)
        {
            {
                std::lock_guard<std::mutex> lock(fetch_mutex);
                if (video.seek(static_cast<int64_t>(ts * AV_TIME_BASE)) >= 0)
                {
                    is_seeking = true;
                    seek_time = ts;
                    recycle_frame_buffers();
                    set_play_time(ts);
                    read_next_frame(ts);
                    fetch_status = 1;
                }
            }
            if (is_seeking)
                fetch_cv.notify_one();
        } else if (video.seek(ts) >= 0)
        {
            return true;
        }
        return false;
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

    void render_direct(AVFrame *frame) {
        // 1. Lock the texture to get SDL's internal pointers and pitches
        void* locked_pixels = nullptr;
        int locked_pitch = 0;

        // For NV12, SDL_LockTexture gives you a pointer to the start of the Y plane
        if (SDL_LockTexture(texture.get(), NULL, &locked_pixels, &locked_pitch) < 0) {
            // Handle error
            return;
        }

        // 2. Set up your destination arrays for sws_scale
        uint8_t* dst_data[4] = { nullptr };
        int dst_linesize[4] = { 0 };

        // NV12 Plane 0: Y Plane
        dst_data[0] = static_cast<uint8_t*>(locked_pixels);
        dst_linesize[0] = locked_pitch;

        // NV12 Plane 1: UV Plane 
        // In NV12, the UV plane starts immediately after the Y plane (width * height)
        // but scaled to the locked pitch!
        dst_data[1] = dst_data[0] + (locked_pitch * frame->height);
        dst_linesize[1] = locked_pitch; // NV12 UV pitch is identical to Y pitch in SDL

        video.scale_video_frame(frame, dst_data, dst_linesize);

        // 4. Unlock to commit the changes and prepare for rendering
        SDL_UnlockTexture(texture.get());
    }

    SDL_PixelFormat ffmpeg_to_sdl3_pix_fmt(auto av_fmt) {
        switch (av_fmt) {
            // --- Planar YUV Formats (8-bit) ---
            case AV_PIX_FMT_YUV420P:
                return SDL_PIXELFORMAT_IYUV; // Also known as I420 (Y, U, V planes)
            case AV_PIX_FMT_YUYV422:
                return SDL_PIXELFORMAT_YUY2; // Packed YUV 4:2:2
            case AV_PIX_FMT_UYVY422:
                return SDL_PIXELFORMAT_UYVY; // Packed YUV 4:2:2 (reversed chroma/luma byte order)

            // --- Semi-Planar YUV Formats (Hardware-friendly) ---
            case AV_PIX_FMT_NV12:
                return SDL_PIXELFORMAT_NV12; // Y plane + interleaved U/V plane
            case AV_PIX_FMT_NV21:
                return SDL_PIXELFORMAT_NV21; // Y plane + interleaved V/U plane

            case AV_PIX_FMT_P010LE:
                return SDL_PIXELFORMAT_P010;

            // --- Unpacked RGB/RGBA Formats ---
            case AV_PIX_FMT_RGB24:
                return SDL_PIXELFORMAT_RGB24;
            case AV_PIX_FMT_BGR24:
                return SDL_PIXELFORMAT_BGR24;
            case AV_PIX_FMT_RGBA:
                return SDL_PIXELFORMAT_RGBA32;
            case AV_PIX_FMT_BGRA:
                return SDL_PIXELFORMAT_BGRA32;
            case AV_PIX_FMT_ARGB:
                return SDL_PIXELFORMAT_ARGB32;
            case AV_PIX_FMT_ABGR:
                return SDL_PIXELFORMAT_ABGR32;

            // --- Packed RGB Formats ---
            case AV_PIX_FMT_RGB565LE:
                return SDL_PIXELFORMAT_RGB565;
            case AV_PIX_FMT_BGR565LE:
                return SDL_PIXELFORMAT_BGR565;

            // --- Monochrome / Grayscale ---
            case AV_PIX_FMT_GRAY8:
                return SDL_PIXELFORMAT_INDEX8; // SDL doesn't have a direct "LUMA8" but INDEX8 works for raw 8-bit gray blits

            // --- Unsupported Direct Maps ---
            // (10-bit HDR formats, hardware surfaces like CUDA/VAAPI/DXVA2,
            // and exotic formats must fallback to swscale or custom GPU shaders)
            default:
                return SDL_PIXELFORMAT_UNKNOWN;
        }
    }

    void draw_texture2(AVFrame *frame) {
        if (last_pixfmt != frame->format) {
            last_pixfmt = frame->format;
            SDL_Texture* tex = SDL_CreateTexture(
                renderer.get(), 
                ffmpeg_to_sdl3_pix_fmt(frame->format),
                SDL_TEXTUREACCESS_STREAMING, 
                frame->width, 
                frame->height
            );
            texture.reset(tex);
        }

//        SDL_UpdateTexture(texture.get(), nullptr, frame->data[0], frame->linesize[0]);
        SDL_UpdateNVTexture(texture.get(), nullptr, frame->data[0], frame->linesize[0], frame->data[1], frame->linesize[1]);
    }

    void draw_ass() {
        ass.draw(renderer.get(), get_play_time());
    }
};
