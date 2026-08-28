#pragma once

#include <string>
#include <vector>
#include <filesystem>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <future>
#include <variant>

#include "ffmpeg.hpp"
#include "subtitle.hpp"
#include "ass.hpp"
#include "sub_bitmap.hpp"
#include "gpu.hpp"
#include "readerwriterqueue.h"

const auto LARGE_INTERVAL = 777777.7;

namespace fs = std::filesystem;

enum MediaMode {
    None = 0,
    Video,
    Image,
    Sound
};

enum NavMode {
    First,
    Next,
    Prev,
    Last
};

struct DirData {
    std::string parent_dir;
    std::vector<std::string> list;
    int idx;
};

class AppState {
private:
    SDL_AudioStream *audio_stream = nullptr;
    static inline const std::unordered_set<std::string> video_exts = { ".mp4", ".mkv", ".mov", ".flv", ".wmv", ".webm", ".avi" };
    static inline const std::unordered_set<std::string> image_exts = { ".png", ".jpg", ".jpeg", ".bmp", ".webp", ".gif", ".jfif" };
    static inline const std::unordered_set<std::string> sound_exts = { ".mp3", ".flac", ".ape", ".opus" };
    std::future<DirData *> dir_future;
    double seek_time;
    AVSubtitleType sub_type;
    SDL_AudioSpec audio_spec;
    bool is_loopable;

public:
    std::vector<std::string> image_files;
    std::size_t current_index = -1;
    std::string parent_dir;
    SDL_Window *window = nullptr;
    ff::VideoFile video;
    AppGpu gpu;
    AppSub app_sub;
    std::vector<ff::ChapterData> chapter_list;
    bool is_seeking = false;
    bool is_loop = true;
    MediaMode media_mode;
    
    ~AppState() {
        gpu.shutdown();
        if (window)
            SDL_DestroyWindow(window);
    }

    bool init() {
        Uint32 window_flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_BORDERLESS | SDL_WINDOW_HIDDEN;
        window = SDL_CreateWindow("mm", 640, 320, window_flags);
        if (!window) { return false; }

        return gpu.init(window);
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
                if (sound_exts.contains(ext))
                    return Sound;
                break;

            case Video:
                if (video_exts.contains(ext))
                    return Video;

            case Image:
                if (image_exts.contains(ext))
                    return Image;

            case Sound:
                if (sound_exts.contains(ext))
                    return Image;
        }
        return None;
    }

    void clear_frame_buffers() {
        if (audio_stream)
            SDL_ClearAudioStream(audio_stream);
        std::visit([](auto&& sub){
            if (sub)
                sub->flush();
        }, app_sub);

        AVFrame *frame;
        while (video.video_frame_queue.try_dequeue(frame))
            ff::frame_recycle(frame);
        while (video.audio_frame_queue.try_dequeue(frame))
            ff::frame_recycle(frame);
        ff::AVSubtitle_ *sub;
        while (video.sub_queue.try_dequeue(sub))
            ff::subtitle_recycle(sub);
    }

    void reset_runtime_state() {
        video.close();
        if (audio_stream) {
            SDL_DestroyAudioStream(audio_stream);
            audio_stream = nullptr;
        }
        clear_frame_buffers();
        is_loopable = false;
        sub_type = SUBTITLE_NONE;
    }

    bool shutdown() {
        video.stop_thead();
        reset_runtime_state();
        std::visit([](auto&& sub){
            if (sub) {
                delete sub;
                sub = nullptr;
            }
        }, app_sub);
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
            SDL_Log("Not supported file: %s\n", file_path);
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

    void check_dir_future() {
        if (dir_future.valid())
        {
            auto *data = dir_future.get();
            image_files = std::move(data->list);
            parent_dir = std::move(data->parent_dir);
            current_index = data->idx;
            delete data;
        }
    }

    bool open_next_file(NavMode mode) {
        check_dir_future();

        switch (mode) {
            case Next:
                if (current_index < image_files.size() - 1) {
                    current_index++;
                } else
                    return false;
                break;
            case Prev:
                if (current_index > 0) {
                    current_index--;
                } else
                    return false;
                break;
            case First:
                if (current_index != 0) {
                    current_index = 0;
                } else
                    return false;
                break;
            case Last:
                if (current_index < image_files.size() - 1) {
                    current_index = image_files.size() - 1;
                } else
                    return false;
                break;
        }

        return open_video((fs::path(parent_dir) / image_files[current_index]).string());
    }

    bool open_video(const std::string& file_path) {
        {
            std::lock_guard lock(video.mutex);
            reset_runtime_state();

            if (!video.open(file_path)) {
                return false;
            }

            if (video.find_audio_stream()) {
                if (video.open_audio_decoder()) {
    //                video.setup_swr_context();
                    auto audio_ctx = video.get_audio_ctx();
//                    SDL_Log("in_sample_fmt: %s\n", av_get_sample_fmt_name(audio_ctx->sample_fmt));

                    SDL_AudioFormat sdl_fmt = SDL_AUDIO_UNKNOWN;
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
                    
                    audio_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &audio_spec, nullptr, nullptr);
                    if (!audio_stream) {
                        SDL_Log("Failed to create audio stream: %s", SDL_GetError());
                    } else {
                        if (!video.is_paused)
                            SDL_ResumeAudioStreamDevice(audio_stream);
                        is_loopable = true;
                    }
                }
            }

            if (video.find_video_stream()) {
                if (video.open_video_decoder()) {
//                    SDL_Log("in_pix_fmt: %s\n", av_get_pix_fmt_name(video.get_video_ctx()->pix_fmt));
                }
            }

            if (video.find_subtitle_stream()) {
                if (video.open_subtitle_decoder()) {
                }
            }

            chapter_list = video.read_chapters();

            resize_window();
            SDL_SetWindowTitle(window, file_path.c_str());
            
            seek_time = video.get_start_time();
            video.seek_time = seek_time;
            is_seeking = true;
            video.is_seeking = true;
            video.status = ff::Reset;
//            video.read_next_frame(seek_time);
            video.shared_tick.store(get_ticks() - seek_time, std::memory_order_relaxed);
        }
        video.cv.notify_one();
        video.start_thread();
        set_video_play(true);
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

    bool seek(double ts) {
        {
            std::lock_guard lock(video.mutex);
            if (video.seek(ts))
            {
                clear_frame_buffers();
                seek_time = ts;
                video.seek_time = ts;
                video.is_seeking = true;
                is_seeking = true;
                video.status = ff::Reset;
//                video.read_next_frame(ts);
                video.shared_tick.store(get_ticks() - ts, std::memory_order_relaxed);
            } else
                return false;
        }
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

    void check_audio_frame() {
        AVFrame *frame;
        while (video.audio_frame_queue.try_dequeue(frame))
        {
            auto frame_time = frame->pts * video.get_audio_time_base();
            if (is_seeking) {
                is_seeking = false;
                set_play_time(frame_time);
            }
            if (av_sample_fmt_is_planar(static_cast<AVSampleFormat>(frame->format))) {
                // Perfect for FLTP (extracts from any standard video file container)
                SDL_PutAudioStreamPlanarData(audio_stream, (const void * const *)frame->data, frame->ch_layout.nb_channels, frame->nb_samples);
            } else {
                // Perfect for packed/interleaved layouts (like FLT, S16, S32)
                int size_in_bytes = frame->nb_samples * frame->ch_layout.nb_channels * av_get_bytes_per_sample(static_cast<AVSampleFormat>(frame->format));
                SDL_PutAudioStreamData(audio_stream, frame->data[0], size_in_bytes);
            }
            ff::frame_recycle(frame);
        }
    }

    AVFrame *check_video_frame(double play_time) {
        AVFrame *frame = nullptr;
        AVFrame *frame_to_display = nullptr;
        while (auto pp = video.video_frame_queue.peek())
        {
            frame = *pp;
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
                is_loopable = true;
                break;
            }
        }
        if (!frame && video.is_eof.load(std::memory_order_acquire)) {
            if (is_loopable) {
                if (seek(video.get_start_time())) {
                    return nullptr;
                }
            }
            set_video_play(false);
        }
        return frame_to_display;
    }

    void init_subtitle(AVSubtitleType type) {
        if (sub_type != SUBTITLE_NONE)
            return;

        sub_type = type;

        switch (type) {
            case SUBTITLE_ASS:
            {
                int target_w, target_h;
                video.get_video_dimensions(target_w, target_h);

                SubAss *sub;
                auto pp = std::get_if<SubAss *>(&app_sub);
                if (pp && *pp) {
                    sub = *pp;
                } else {
                    std::visit([](auto&& sub){
                        if (sub)
                            delete sub;
                    }, app_sub);
                    sub = new SubAss(gpu.get_device());
                    app_sub = sub;
                }
                sub->init(target_w, target_h, video.get_subtitle_ctx(), video.get_format_ctx(), window);
            }
                break;

            case SUBTITLE_BITMAP:
            {
                SubBitmap *sub;
                auto pp = std::get_if<SubBitmap *>(&app_sub);
                if (pp && *pp) {
                    sub = *pp;
                } else {
                    std::visit([](auto&& sub){
                        if (sub)
                            delete sub;
                    }, app_sub);
                    sub = new SubBitmap(gpu.get_device());
                    app_sub = sub;
                }
                sub->init(video.get_subtitle_ctx(), window);
            }
                break;

            default:
                return;
        }
    }

    void check_subtitle() {
        while (auto pp = video.sub_queue.peek()) {
            auto sub = *pp;
            if (sub_type == SUBTITLE_BITMAP) {
                std::get<SubBitmap *>(app_sub)->add_sub(sub);
                video.sub_queue.pop();
                continue;
            }
            bool done = false;
            for (auto i = 0; i < sub->num_rects && !done; i++) {
                AVSubtitleRect* rect = sub->rects[i];
                init_subtitle(rect->type);
                switch (rect->type) {
                    case SUBTITLE_ASS:
                        if (rect->ass) {
                            std::get<SubAss *>(app_sub)->add_ass(rect->ass, static_cast<long long>(sub->frame_time * 1000), static_cast<long long>(sub->duration * 1000));
                        }
                        break;
                    case SUBTITLE_BITMAP:
                        std::get<SubBitmap *>(app_sub)->add_sub(sub);
                        done = true;
                        break;
                }
            }
            video.sub_queue.pop();
            if (done)
                continue;
            ff::subtitle_recycle(sub);
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
        return (is_seeking || video.is_paused) ? seek_time : (get_ticks() - video.shared_tick.load(std::memory_order_relaxed));
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
        SDL_GetWindowPosition(window, &current_x, &current_y);
        SDL_GetWindowSize(window, &current_w, &current_h);
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

        SDL_SetWindowSize(window, target_w, target_h);
        SDL_SetWindowPosition(window, new_x, new_y);
    }

    void pause(bool pause_only = false) {
        {
            std::lock_guard lock(video.mutex);
            if (video.is_paused) {
                if (pause_only)
                    return;
                video.is_paused = false;
                video.status = ff::Reset;
                video.shared_tick.store(get_ticks() - seek_time, std::memory_order_relaxed);
            } else {
                SDL_PauseAudioStreamDevice(audio_stream);
                seek_time = get_play_time();
                video.is_paused = true;
                set_video_play(false);
                return;
            }
        }
        video.cv.notify_one();
        SDL_ResumeAudioStreamDevice(audio_stream);
        set_video_play(true);
    }

    auto get_file_name() {
        return current_index >= 0 ? image_files[current_index] : nullptr;
    }

    void select_subtitle(int idx) {
        std::lock_guard<std::mutex> lock(video.mutex);
        if (video.get_subtitle_index() != idx) {
            std::visit([](auto&& sub){
                if (sub)
                    sub->flush();
            }, app_sub);
            video.select_subtitle(idx);
        }
    }

    void select_audio(int idx) {
        std::lock_guard<std::mutex> lock(video.mutex);
        if (video.get_audio_index() != idx) {
//            if (audio_stream)
//                SDL_FlushAudioStream(audio_stream.get());
            video.select_audio(idx);
        }
    }

    static void set_video_play(bool play) {
        static bool is_playing = false;
        if (is_playing != play) {
            is_playing = play;
            SDL_SetHint(SDL_HINT_MAIN_CALLBACK_RATE, play ? 0 : "waitevent");
        }
    }

    void render() {
        if (!video.is_paused) {
            check_audio_frame();
            check_subtitle();
            auto play_time = get_play_time();
            auto video_frame = check_video_frame(play_time);
            if (video_frame) {
                gpu.set_frame(video_frame, play_time, app_sub);
            }    
        }

        gpu.render(app_sub);
    }

    bool open_file_location()
    {
        check_dir_future();
        const auto &filename = image_files[current_index];
        const auto &file_path = (fs::path(parent_dir) / filename).string();

#ifdef __linux__
        const char * const commands[][4] = {
            { "nautilus", "--select",  file_path.c_str(), nullptr },
            { "nemo", "--select",  file_path.c_str(), nullptr },
            { "caja", "--select",  file_path.c_str(), nullptr },
            { "dolphin", "--select",  file_path.c_str(), nullptr },
            { "thunar", "--select",  file_path.c_str(), nullptr },
            { "xdg-open", parent_dir.c_str(), nullptr }
        };

        for (const auto &command : commands) {
            auto proc = SDL_CreateProcess(command, false);
            if (proc != nullptr) {
                SDL_DestroyProcess(proc);
                return true;
            }
        }
#else
        const char* commandArgs[] = {
            "explorer.exe",
            "/select,",
            file_path.c_str(),
            nullptr
        };
        auto proc = SDL_CreateProcess(commandArgs, false);
        if (proc) {
            SDL_DestroyProcess(proc);
            return true;
        }
#endif
        return false;
    }
};
