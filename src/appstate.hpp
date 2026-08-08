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
#include <future>

#include "ffmpeg.hpp"
#include "ass.hpp"
#include "readerwriterqueue.h"
#include "twowayqueue.hpp"
#include "gpu.hpp"

// Define a unique event ID for our frame ticker
Uint32 USEREVENT_NEXT_FRAME;
Uint32 USEREVENT_SUBTITLE_ASS;
const auto NUM_USEREVENT = 2;
const auto LARGE_INTERVAL = 777777.7;

namespace fs = std::filesystem;
extern AssHandler ass;

enum Status {
    Running,
    Reset,
    Quit
};

using WindowPtr = std::unique_ptr<SDL_Window, decltype(&SDL_DestroyWindow)>;
using RendererPtr = std::unique_ptr<SDL_Renderer, decltype(&SDL_DestroyRenderer)>;
using TexturePtr = std::unique_ptr<SDL_Texture, decltype(&SDL_DestroyTexture)>;
using AudioStream = std::unique_ptr<SDL_AudioStream, decltype(&SDL_DestroyAudioStream)>;

enum MediaMode {
    None = 0,
    Video,
    Image
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
    AppGpu gpu;
    std::atomic<AVFrame *> video_frame;
    ff::AudioBuffer audio_buf;
    SDL_AudioDeviceID audio_device_id = 0;
    std::thread thread;
    double seek_time;
    std::mutex mutex;
    std::condition_variable cv;
    Status status = Running;
    std::vector<ff::ChapterData> chapter_list;
    std::future<DirData *> dir_future;
    static inline const std::unordered_set<std::string> video_exts = { ".mp4", ".mkv", ".mov", ".flv", ".wmv", ".webm" };
    static inline const std::unordered_set<std::string> image_exts = { ".png", ".jpg", ".jpeg", ".bmp", ".webp", ".gif" };
    MediaMode media_mode;
    bool is_seeking = false;
    bool is_loopable = false;
    SDL_AudioSpec audio_spec;

    AppState() {
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
        if (audio_stream)
            SDL_ClearAudioStream(audio_stream.get());
        ass.flush();

        AVFrame *frame;
        while (video.video_frame_queue.try_dequeue(frame))
            ff::frame_recycle(frame);
        while (video.audio_frame_queue.try_dequeue(frame))
            ff::frame_recycle(frame);
        video.sub_queue.recycle_all();
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
        is_loopable = false;
    }

    bool shutdown() {
        video.stop_thead();
        {
            std::lock_guard<std::mutex> lock(mutex);
            status = Quit;
        }
        cv.notify_one();
        if (thread.joinable())
            thread.join();
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
        {
            std::scoped_lock lock(video.mutex, mutex);
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
                        SDL_AudioFormat sdl_fmt = SDL_AUDIO_UNKNOWN;
                        auto audio_ctx = video.get_audio_ctx();
                        switch (audio_ctx->sample_fmt) {
                            case AV_SAMPLE_FMT_U8:
                            case AV_SAMPLE_FMT_U8P:
                                sdl_fmt = SDL_AUDIO_U8;
                                break;
                            case AV_SAMPLE_FMT_S16:
                            case AV_SAMPLE_FMT_S16P:
                                sdl_fmt = SDL_AUDIO_S16;
                                break;
                            case AV_SAMPLE_FMT_S32:
                            case AV_SAMPLE_FMT_S32P:
                                sdl_fmt = SDL_AUDIO_S32;
                                break;
                            case AV_SAMPLE_FMT_FLT:
                            case AV_SAMPLE_FMT_FLTP:
                                sdl_fmt = SDL_AUDIO_F32;
                                break;
                            case AV_SAMPLE_FMT_DBL:
                            case AV_SAMPLE_FMT_DBLP:
                            case AV_SAMPLE_FMT_S64:
                            case AV_SAMPLE_FMT_S64P:
                                sdl_fmt = SDL_AUDIO_UNKNOWN;
                        }
                        audio_spec = { sdl_fmt, audio_ctx->ch_layout.nb_channels, audio_ctx->sample_rate };
                        auto stream = SDL_CreateAudioStream(&audio_spec, NULL);
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
                    is_loopable = true;
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
            
            auto subtitle_ctx = video.get_subtitle_ctx();
            if (subtitle_ctx) {
                int target_w, target_h;
                video.get_video_dimensions(target_w, target_h);
                ass.init_once(gpu.get_device());
                ass.init(target_w, target_h, subtitle_ctx, video.get_format_ctx(), window.get());
            }

            seek_time = video.get_start_time();
            video.is_seeking = true;
            is_seeking = true;
            video.status = ff::Reset;
            status = Reset;
            video.read_next_frame(seek_time);
            video.shared_tick.store(get_ticks() - seek_time, std::memory_order_relaxed);
        }
        cv.notify_one();
        video.cv.notify_one();
        if (!thread.joinable()) {
            thread = std::thread(thread_worker, this);
            pthread_setname_np(thread.native_handle(), "timer");
        }
        video.start_thread();
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

    bool loop() {
        {
            auto ts = video.get_start_time();
            std::lock_guard<std::mutex> lock(video.mutex);
            if (video.seek(ts))
            {
                clear_frame_buffers();
                seek_time = ts;
                video.is_seeking = true;
                is_seeking = true;
                video.status = ff::Reset;
                video.read_next_frame(ts);
                video.shared_tick.store(get_ticks() - ts, std::memory_order_relaxed);
            } else
                return false;
        }
        video.cv.notify_one();
        return true;
    }

    bool seek(double ts) {
        {
            std::scoped_lock lock(video.mutex, mutex);
            if (video.seek(ts))
            {
                clear_frame_buffers();
                seek_time = ts;
                video.is_seeking = true;
                is_seeking = true;
                video.status = ff::Reset;
                status = Reset;
                video.read_next_frame(ts);
                video.shared_tick.store(get_ticks() - ts, std::memory_order_relaxed);
            } else
                return false;
        }
        cv.notify_one();
        video.cv.notify_one();
        return true;
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
        return seek(target_time);
    }

    bool seek_ratio(double t) {
        return seek(t * video.get_duration());
    }

    bool seek_to_chapter(int id) {
        auto chapter = chapter_list[id];
        return seek(chapter.start_time);
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

    double check_next_frame(double play_time) {
        double ret = 0.0;

        if (video.is_audio()) {
            AVFrame *frame;
            while (video.audio_frame_queue.try_dequeue(frame))
            {
                auto frame_time = frame->pts * video.get_audio_time_base();
                if (is_seeking) {
                    play_time = frame_time;
                    is_seeking = false;
                    set_play_time(play_time);
                }
                if (av_sample_fmt_is_planar(static_cast<AVSampleFormat>(frame->format))) {
                    // Perfect for FLTP (extracts from any standard video file container)
                    SDL_PutAudioStreamPlanarData(audio_stream.get(), (const void * const *)frame->data, frame->ch_layout.nb_channels, frame->nb_samples);
                } else {
                    // Perfect for packed/interleaved layouts (like FLT, S16, S32)
                    int size_in_bytes = frame->nb_samples * frame->ch_layout.nb_channels * av_get_bytes_per_sample(static_cast<AVSampleFormat>(frame->format));
                    SDL_PutAudioStreamData(audio_stream.get(), frame->data[0], size_in_bytes);
                }
                ff::frame_recycle(frame);
            }
        }
        if (video.is_video()) {
            AVFrame *frame_to_display = nullptr;
            while (auto pp = video.video_frame_queue.peek())
            {
                AVFrame *frame = *pp;
                auto frame_time = frame->pts * video.get_video_time_base();
                if (is_seeking && !video.is_audio()) {
                    play_time = frame_time;
                    is_seeking = false;
                    set_play_time(play_time);
                }
                if (frame_time <= play_time) {
                    if (frame_to_display)
                        ff::frame_recycle(frame_to_display);
                    frame_to_display = frame;
                    video.video_frame_queue.pop();
                } else {
                    ret = frame_time;
                    is_loopable = true;
                    break;
                }
            }
            if (frame_to_display)
            {
                if (ret == 0.0)
                    ret = (frame_to_display->pts + frame_to_display->duration) * video.get_video_time_base();
                auto duration = frame_to_display->duration;
                auto old_frame = video_frame.exchange(frame_to_display, std::memory_order_release);
                ff::frame_recycle(old_frame);

                SDL_Event event;
                SDL_zero(event);
                event.type = USEREVENT_NEXT_FRAME;
                SDL_PushEvent(&event);
            }
        }

        return ret;
    }

    double time_from_audio_bytes(int bytes) {
        int bps;
        switch (audio_spec.format) {
            case SDL_AUDIO_U8:
                bps = 1;
                break;
            case SDL_AUDIO_S16:
                bps = 2;
                break;
            case SDL_AUDIO_S32:
            case SDL_AUDIO_F32:
            default:
                bps = 4;
        }
        return (double) bytes / (audio_spec.freq * audio_spec.channels * bps);
    }

    double time_next_frame()
    {
        if (video.is_paused)
            return LARGE_INTERVAL;
        double play_time = get_play_time();
        auto frame_time = check_next_frame(play_time);
        if (frame_time > 0.0)
            return frame_time - get_play_time();
        else if (is_loopable) {
            if (audio_stream) {
                auto bytes = SDL_GetAudioStreamQueued(audio_stream.get());
                frame_time = time_from_audio_bytes(bytes);
                if (frame_time >= 0.02)
                    return SDL_min(frame_time, 0.1);
            }
            if (loop())
                return 0.0;
        }
        return LARGE_INTERVAL;
    }
    
    static void thread_worker(AppState *state)
    {
        while (true)
        {
            std::unique_lock<std::mutex> lock(state->mutex);
            if (state->status == Quit)
                break;
            state->status = Running;
            double interval = state->time_next_frame();
            state->cv.wait_for(lock, std::chrono::microseconds(static_cast<int64_t>(interval * 1000000)), [state]{ return state->status != Running; });
        }
    }

    static double get_ticks() {
        return static_cast<double>(SDL_GetPerformanceCounter()) / SDL_GetPerformanceFrequency();
    }

    void set_play_time(double play_time)
    {
        video.shared_tick.store(get_ticks() - play_time, std::memory_order_release);
    }

    double get_play_time() const {
        return video.is_paused ? seek_time : (get_ticks() - video.shared_tick.load(std::memory_order_relaxed));
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
        {
            std::scoped_lock lock(video.mutex, mutex);
            if (video.is_paused) {
                video.is_paused = false;
                video.status = ff::Reset;
                status = Reset;
                video.shared_tick.store(get_ticks() - seek_time, std::memory_order_relaxed);
            } else {
                SDL_PauseAudioStreamDevice(audio_stream.get());
                seek_time = get_play_time();
                video.is_paused = true;
                return;
            }
        }
        cv.notify_one();
        video.cv.notify_one();
        SDL_ResumeAudioStreamDevice(audio_stream.get());
    }

    auto get_file_name() {
        return current_index >= 0 ? image_files[current_index] : nullptr;
    }

    void select_subtitle(int idx) {
        std::lock_guard<std::mutex> lock(mutex);
        if (video.get_subtitle_index() != idx) {
            ass.flush();
            video.select_subtitle(idx);
        }
    }

    void select_audio(int idx) {
        std::lock_guard<std::mutex> lock(mutex);
        if (video.get_audio_index() != idx) {
//            if (audio_stream)
//                SDL_FlushAudioStream(audio_stream.get());
            video.select_audio(idx);
        }
    }
};
