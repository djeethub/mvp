#pragma once

#include <iostream>
#include <string>
#include <queue>
#include <unordered_set>
#include <format>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
#include <libavutil/dict.h>
}

#include "concurrentqueue.h"
#include "readerwriterqueue.h"
#include "twowayqueue.hpp"

namespace ff {

enum Status {
    Play = 0,
    Reset,
    Quit,
};

const auto LARGE_INTERVAL = 777777.7;
const double PRELOAD_TIME  = 0.2;

struct Subtitle {
    std::string text;
    double pts;
    double duration;
};

struct ChapterData {
    std::string title;
    double start_time; // in seconds
    double end_time;   // in seconds
};

struct AudioData {
    int idx;
    std::string lang;
    std::string title;
    AVCodecID codec_id;
};

struct AudioBuffer {
    uint8_t *buf = nullptr;
    int size = 0;
    int data_size = 0;

    AudioBuffer() = default;
    ~AudioBuffer() {
        av_freep(&buf);
    }
    void init(int n)
    {
        if (size < n) {
            av_freep(&buf);
            av_samples_alloc(&buf, nullptr, 2, n, AV_SAMPLE_FMT_S16, 0);
            size = n;
        }
    }
};

class FramePool {
protected:
    moodycamel::ConcurrentQueue<AVFrame *> recycle_queue;

public:
    ~FramePool() {
        AVFrame *frame;
        while (recycle_queue.try_dequeue(frame)) {
            av_frame_free(&frame);
        }
    }

    void recycle(AVFrame *frame) {
        if (!frame)
            return;
        av_frame_unref(frame);
        recycle_queue.enqueue(frame);
    }

    AVFrame *alloc() {
        AVFrame *frame;
        if (recycle_queue.try_dequeue(frame))
            return frame;
        return av_frame_alloc();
    }
};

template <typename T, typename Deleter>
class AvQueue {
protected:
    std::queue<T> queue;

public:
    ~AvQueue() {
        clear();
    }

    void clear() {
        while (!queue.empty())
        {
            Deleter{}(queue.front());
            queue.pop();
        }
    }

    T get() {
        T data_ptr = queue.front();
        queue.pop();
        return data_ptr;
    }

    T front() {
        return queue.front();
    }

    void pop() {
        queue.pop();
    }

    void push(T data_ptr) {
        if (data_ptr) {
            queue.push(data_ptr);
        }
    }

    auto size() {
        return queue.size();
    }

    auto empty() {
        return queue.empty();
    }
};

struct FrameDeleter {
    void operator()(AVFrame *frame) { av_frame_free(&frame); }
};

using FrameQueue = AvQueue<AVFrame *, FrameDeleter>;

FramePool frame_pool;

inline AVFrame *frame_alloc() {
    return frame_pool.alloc();
}

inline void frame_recycle(AVFrame *frame) {
    frame_pool.recycle(frame);
}

class VideoFile {
public:
    VideoFile() = default;
    ~VideoFile() {
        close();
        av_packet_free(&packet);
        av_frame_free(&frame);
    }

    VideoFile(const VideoFile&) = delete;
    VideoFile& operator=(const VideoFile&) = delete;
    VideoFile(VideoFile&&) = delete;
    VideoFile& operator=(VideoFile&&) = delete;

    auto get_duration() const { return duration; }
    auto get_video_time_base() const { return video_time_base; }
    auto get_audio_time_base() const { return audio_time_base; }
    auto get_subtitle_time_base() const { return subtitle_time_base; }
    auto get_audio_channels() const { return audio_codec_ctx->ch_layout.nb_channels; }
    auto get_audio_sample_rate() const { return audio_codec_ctx->sample_rate; }
    auto get_subtitle_tracks() const { return subtitle_list; }
    auto get_audio_tracks() const { return audio_list; }
    auto get_subtitle_index() const { return subtitle_stream_idx; }
    auto get_audio_index() const { return audio_stream_index; }
    auto get_subtitle_ctx() const { return subtitle_codec_ctx; }
    auto get_video_ctx() const { return video_codec_ctx; }
    auto get_format_ctx() const { return format_ctx; }
    auto is_audio() const { return audio_codec_ctx != nullptr; }
    auto is_video() const { return video_codec_ctx != nullptr; }

    moodycamel::ReaderWriterQueue<AVFrame *> audio_frame_queue;
    moodycamel::ReaderWriterQueue<AVFrame *> video_frame_queue;
    TwowayQueue<Subtitle *> sub_queue{32};
    std::mutex mutex;
    std::condition_variable cv;
    Status status = Play;
    bool is_loop = true;
    bool is_paused = false;
    bool is_seeking = false;
    double seek_time;

    bool open(const std::string &filename)
    {
        close();

        if (avformat_open_input(&format_ctx, filename.c_str(), nullptr, nullptr) < 0)
        {
            std::cerr << "Could not open video file.\n";
            return false;
        }
        if (avformat_find_stream_info(format_ctx, nullptr) < 0)
        {
            std::cerr << "Could not find stream information.\n";
            close();
            return false;
        }
        duration = static_cast<double>(format_ctx->duration) / AV_TIME_BASE;
        if (!packet) packet = av_packet_alloc();
        if (!frame) frame = frame_pool.alloc();
        return true;
    }

    std::string get_stream_metadata(const AVStream* stream, const char *key) {
        if (!stream || !stream->metadata) {
            return "";
        }
        
        // Look up the key in the stream's metadata dictionary
        AVDictionaryEntry* entry = av_dict_get(stream->metadata, key, nullptr, 0);
        if (entry && entry->value) {
            return std::string(entry->value);
        }
        
        return "";
    }    

    bool find_subtitle_stream()
    {
        for (unsigned int i = 0; i < format_ctx->nb_streams; i++) {
            auto stream = format_ctx->streams[i];
            if (stream->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE) {
                auto subtitle = AudioData();
                subtitle.idx = i;
                subtitle.lang = get_stream_metadata(stream, "language");
                subtitle.title = get_stream_metadata(stream, "title");
                if (subtitle.title.empty())
                    subtitle.title = std::format("Track {}", i);
                subtitle.codec_id = stream->codecpar->codec_id;
                subtitle_list.push_back(subtitle);
            }
        }

        for (const auto& val : sub_lang_pref) {
            for (const auto& sub : subtitle_list) {
                if (sub.lang == val) {
                    subtitle_stream_idx = sub.idx;
                    return true;
                }
            }
        }
        return false;
    }

    bool find_audio_stream()
    {
        for (unsigned int i = 0; i < format_ctx->nb_streams; i++) {
            auto stream = format_ctx->streams[i];
            if (stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
                auto data = AudioData();
                data.idx = i;
                data.lang = get_stream_metadata(stream, "language");
                data.title = get_stream_metadata(stream, "title");
                if (data.title.empty())
                    data.title = std::format("Track {}", i);
                data.codec_id = stream->codecpar->codec_id;
                audio_list.push_back(data);
            }
        }

        for (const auto& val : audio_lang_pref) {
            for (const auto& data : audio_list) {
                if (data.lang == val) {
                    audio_stream_index = data.idx;
                    return true;
                }
            }
        }

        if (!audio_list.empty()) {
            audio_stream_index = audio_list.front().idx;
            return true;
        }
//        std::cerr << "Could not find an audio stream.\n";
        return false;
    }

    bool find_video_stream()
    {
        for (unsigned int i = 0; i < format_ctx->nb_streams; i++)
        {
            if (format_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
            {
                video_stream_index = i;
                return true;
            }
        }
//        std::cerr << "Could not find a video stream.\n";
        return false;
    }

    bool open_audio_decoder()
    {
        AVCodecParameters *codec_params = format_ctx->streams[audio_stream_index]->codecpar;
        const AVCodec *codec = avcodec_find_decoder(codec_params->codec_id);
        audio_codec_ctx = avcodec_alloc_context3(codec);
        avcodec_parameters_to_context(audio_codec_ctx, codec_params);
        avcodec_open2(audio_codec_ctx, codec, nullptr);
        audio_time_base = av_q2d(format_ctx->streams[audio_stream_index]->time_base);
        return true;
    }

    bool open_video_decoder()
    {
        AVCodecParameters *codec_params = format_ctx->streams[video_stream_index]->codecpar;
        const AVCodec *codec = avcodec_find_decoder(codec_params->codec_id);
        video_codec_ctx = avcodec_alloc_context3(codec);
        AVBufferRef *hw_device_ctx = nullptr;
        if (av_hwdevice_ctx_create(&hw_device_ctx, AV_HWDEVICE_TYPE_VAAPI, NULL, NULL, 0) == 0) {
            video_codec_ctx->hw_device_ctx = av_buffer_ref(hw_device_ctx);
            av_buffer_unref(&hw_device_ctx);
        }
        video_codec_ctx->thread_count = 0;
        video_codec_ctx->thread_type = FF_THREAD_FRAME; // Or FF_THREAD_SLICE
        avcodec_parameters_to_context(video_codec_ctx, codec_params);
        avcodec_open2(video_codec_ctx, codec, nullptr);
        video_time_base = av_q2d(format_ctx->streams[video_stream_index]->time_base);
//        printf("av format: %i -> %i\n", video_codec_ctx->pix_fmt, finalPixelFormat);
        return true;
    }

    bool open_subtitle_decoder() {
        AVCodecParameters *codec_params = format_ctx->streams[subtitle_stream_idx]->codecpar;
        const AVCodec *codec = avcodec_find_decoder(codec_params->codec_id);
        subtitle_codec_ctx = avcodec_alloc_context3(codec);
        avcodec_parameters_to_context(subtitle_codec_ctx, codec_params);
        avcodec_open2(subtitle_codec_ctx, codec, nullptr);
        subtitle_time_base = av_q2d(format_ctx->streams[subtitle_stream_idx]->time_base);
        return true;
    }

    bool setup_swr_context()
    {
        // Explicitly define a 2-channel Stereo Layout for the destination
        AVChannelLayout stereo_layout;
        av_channel_layout_default(&stereo_layout, 2);

        // Setup SwrContext to convert whatever the video has into raw packed S16 Stereo PCM
        int swr_err = swr_alloc_set_opts2(&swr_ctx,
                                          &stereo_layout, AV_SAMPLE_FMT_S16, 44100,                                               // Destination: Packed 16-bit, 44100Hz Stereo
                                          &audio_codec_ctx->ch_layout, audio_codec_ctx->sample_fmt, audio_codec_ctx->sample_rate, // Source: Movie Native Settings
                                          0, nullptr);

        if (swr_err < 0 || swr_init(swr_ctx) < 0)
        {
            SDL_Log("Failed to initialize SwrContext conversion engine!");
            return SDL_APP_FAILURE;
        }
        return true;
    }

    void convert_audio_frame(AVFrame *frame, AudioBuffer *audio_buf)
    {
        // 1. Calculate max potential samples we will get after conversion
        int out_samples = av_rescale_rnd(
            swr_get_delay(swr_ctx, 44100) + frame->nb_samples,
            44100,
            audio_codec_ctx->sample_rate,
            AV_ROUND_UP);

        // 2. Allocate pointers for the destination buffer (2 channels, S16 format)
        audio_buf->init(out_samples);

        // 3. Perform the actual conversion safely
        int converted_samples = swr_convert(
            swr_ctx,
            &audio_buf->buf,
            out_samples,
            (const uint8_t **)frame->data,
            frame->nb_samples);

        if (converted_samples > 0)
        {
            // Calculate the exact size of the resulting packed bytes
            // 2 channels * number of converted samples * 2 bytes per sample (S16)
            audio_buf->data_size = converted_samples * 2 * sizeof(int16_t);
        }
        else
            audio_buf->data_size = 0;
    }

    void close() {
        avcodec_free_context(&audio_codec_ctx);
        avcodec_free_context(&video_codec_ctx);
        avcodec_free_context(&subtitle_codec_ctx);
        avformat_close_input(&format_ctx);
        swr_free(&swr_ctx);
        audio_stream_index = -1;
        video_stream_index = -1;
        subtitle_stream_idx = -1;
        subtitle_list.clear();
        audio_list.clear();
        is_loopable = false;
        is_seeking = false;
        last_video_time = 0.0;
        last_audio_time = 0.0;
    }

    void get_video_dimensions(int& width, int& height) const {
        if (video_codec_ctx) {
            width = video_codec_ctx->width;
            height = video_codec_ctx->height;
        } else {
            width = 0;
            height = 0;
        }
    }

    int64_t seek(int64_t ts)
    {
        int seek_result = avformat_seek_file(
                format_ctx,
                -1,
                INT64_MIN, ts, ts,
                AVSEEK_FLAG_BACKWARD);
        if (seek_result >= 0) {
            if (audio_codec_ctx)
                avcodec_flush_buffers(audio_codec_ctx);
            if (video_codec_ctx)
                avcodec_flush_buffers(video_codec_ctx);
            if (subtitle_codec_ctx)
                avcodec_flush_buffers(subtitle_codec_ctx);
        }
        return seek_result;
    }

    bool seek(double ts) {
        if (seek(static_cast<int64_t>(ts * AV_TIME_BASE)) >= 0) {
            set_seeking(true, ts);
            last_video_time = ts;
            last_audio_time = ts;
            return true;
        } else
            return false;
    }

    void set_seeking(bool set, double ts) {
        if (set) {
            is_seeking = true;
            seek_time = ts;
            set_skip(AVDISCARD_NONREF, AVDISCARD_ALL, AVDISCARD_ALL);
//            clear_frame_buffers();
        } else {
            is_seeking = false;
            seek_time = ts;
            set_skip(AVDISCARD_DEFAULT, AVDISCARD_DEFAULT, AVDISCARD_DEFAULT);
        }
    }    

    std::vector<ChapterData> read_chapters() {
        std::vector<ChapterData> chapter_list;

        // 1. Check if the file actually contains any chapters
        if (format_ctx->nb_chapters == 0) {
            return chapter_list;
        }

        double start_time = get_start_time();
        // 2. Iterate through the chapters array
        for (unsigned int i = 0; i < format_ctx->nb_chapters; i++) {
            AVChapter* chapter = format_ctx->chapters[i];
            ChapterData data;

            // 3. Convert timestamps from the chapter's unique timebase into seconds
            double timebase_factor = av_q2d(chapter->time_base);
            data.start_time = start_time;
            data.end_time   = chapter->end * timebase_factor;

            // 4. Extract the chapter title string from the metadata dictionary
            AVDictionaryEntry* title_tag = av_dict_get(chapter->metadata, "title", nullptr, 0);
            if (title_tag && title_tag->value) {
                data.title = title_tag->value;
            } else {
                // Fallback string if the chapter is unnamed (e.g., "Chapter 1")
                data.title = "Chapter " + std::to_string(i + 1);
            }

            chapter_list.push_back(data);
            start_time = data.end_time;
        }

        return chapter_list;
    }

    double get_start_time() {
        return static_cast<double>(format_ctx->start_time * AV_TIME_BASE);
    }

    void print_error_str(int err) {
        char err_buf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(err, err_buf, sizeof(err_buf));
        printf("%i: %s\n", err, err_buf);
    }

    void select_subtitle(int idx) {
        if (subtitle_stream_idx != idx) {
            avcodec_free_context(&subtitle_codec_ctx);
            subtitle_stream_idx = idx;
            if (idx >= 0)
                open_subtitle_decoder();
        }
    }

    void select_audio(int idx) {
        if (audio_stream_index != idx) {
            avcodec_free_context(&audio_codec_ctx);
            audio_stream_index = idx;
            if (idx >= 0)
                open_audio_decoder();
        }
    }

    void set_skip(AVDiscard frame, AVDiscard loop_filter, AVDiscard idct) {
        video_codec_ctx->skip_frame = frame;
        video_codec_ctx->skip_loop_filter = loop_filter;
        video_codec_ctx->skip_idct = idct;
    }

    void add_subtitle(const std::string& text, AVPacket *packet) {
        auto data = sub_queue.alloc();
        data->text = text;
        data->pts = packet->pts * get_subtitle_time_base();
        data->duration = packet->duration * get_subtitle_time_base();
        sub_queue.enqueue(std::move(data));
    }

    int read_next_frame(double play_time, bool preload = false) {
        int read_result;
        double target_play_time = play_time + PRELOAD_TIME;

        while ((is_video() && last_video_time < target_play_time) || (is_audio() && last_audio_time < target_play_time)) {
            read_result = av_read_frame(format_ctx, packet);
            if (read_result < 0 && read_result != AVERROR_EOF)
                return read_result;

            if (packet->stream_index == audio_stream_index)
            {
                last_audio_time = packet->pts * get_audio_time_base();
                if ((!is_seeking || last_audio_time >= play_time) && avcodec_send_packet(audio_codec_ctx, packet) >= 0)
                {
                    while (avcodec_receive_frame(audio_codec_ctx, frame) >= 0)
                    {
                        if (frame->pts != AV_NOPTS_VALUE) {
                            if (is_seeking) {
                                play_time = last_audio_time;
                                set_seeking(false, play_time);
                                set_play_time(play_time);
                            }
                            auto new_frame = frame_alloc();
                            av_frame_move_ref(new_frame, frame);
                            audio_frame_queue.enqueue(new_frame);
                        }
                        av_frame_unref(frame);
                    }
                }
            }
            else if (packet->stream_index == video_stream_index)
            {
                if (avcodec_send_packet(video_codec_ctx, packet) >= 0)
                {
                    while (avcodec_receive_frame(video_codec_ctx, frame) >= 0)
                    {
                        if (frame->pts != AV_NOPTS_VALUE) {
                            last_video_time = frame->pts * get_video_time_base();
                            if (!is_seeking || last_video_time >= play_time) {
                                if (is_seeking) {
                                    play_time = last_video_time;
                                    set_seeking(false, seek_time);
                                    set_play_time(play_time);
                                }
                                auto new_frame = frame_alloc();
                                if (frame->format == AV_PIX_FMT_VAAPI) {
                                    auto err = av_hwframe_transfer_data(new_frame, frame, 0);
                                    if (err != 0) SDL_Log("av_hwframe_transfer_data failed: %s\n", av_err2str(err));
                                    new_frame->pts = frame->pts;
                                    new_frame->duration = frame->duration;
                                }
                                else {
                                    av_frame_move_ref(new_frame, frame);
                                }
                                video_frame_queue.enqueue(new_frame);
                            }
                        }
                        av_frame_unref(frame);
                    }
                }
            }
            else if (packet->stream_index == subtitle_stream_idx) {
                int got_subtitle = 0;
                AVSubtitle subtitle;
                // avcodec_decode_subtitle2 is old but still the standard way to handle subtitles in modern FFmpeg
                if (avcodec_decode_subtitle2(subtitle_codec_ctx, &subtitle, &got_subtitle, packet) >= 0) {
                    if (got_subtitle) {
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

                        avsubtitle_free(&subtitle);
                    }
                }
            }
            av_packet_unref(packet);

            if (!preload && last_video_time > play_time)
                break;
        }

        return read_result;
    }

    double time_next_frame() {
        if (is_paused && !is_seeking)
            return LARGE_INTERVAL;
        double play_time = is_seeking ? seek_time : get_play_time();
        if (is_video()) {
            auto rlt = read_next_frame(play_time, true);
//            SDL_Log("%i %i\n", audio_frame_queue.size_approx(), video_frame_queue.size_approx());
            if (rlt < 0) {
                if (rlt == AVERROR_EOF) {
                    if (is_loopable && is_loop) {
                        if (seek(get_start_time())) {
                            return 0.0;
                        }
                    }
                }
                return LARGE_INTERVAL;
            }
            if (is_seeking)
                return 0.0;
            return 0.1;
        } else {
            auto rlt = read_next_frame(play_time, true);
            if (rlt < 0) {
                if (rlt == AVERROR_EOF && is_loop) {
                    if (seek(get_start_time())) {
                        return 0.0;
                    }
                }
                return LARGE_INTERVAL;
            }
            return 0.1;
        }
    }

    static void thread_worker(VideoFile *video)
    {
        while (true)
        {
            std::unique_lock<std::mutex> lock(video->mutex);
            if (video->status == Quit)
                break;
            video->status = Play;
            double interval = video->time_next_frame();
            video->cv.wait_for(lock, std::chrono::microseconds(static_cast<int64_t>(interval * 1000000)), [video]{ return video->status != Play; });
        }
    }

    void start_thread() {
        if (!thread.joinable()) {
            thread = std::thread(thread_worker, this);
            pthread_setname_np(thread.native_handle(), "media");
        }
    }

    void stop_thead() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            status = Quit;
        }
        cv.notify_one();
        if (thread.joinable())
            thread.join();
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

private:
    AVFormatContext* format_ctx = nullptr;
    AVCodecContext* audio_codec_ctx = nullptr;
    SwrContext* swr_ctx = nullptr;
    int audio_stream_index = -1;

    AVCodecContext* video_codec_ctx = nullptr;
    int video_stream_index = -1;

    AVPacket* packet = nullptr;
    AVFrame* frame = nullptr;
    std::vector<AudioData> subtitle_list;
    std::vector<AudioData> audio_list;
    std::vector<std::string> sub_lang_pref = {"en", "eng", "ja", "jpn"};
    std::vector<std::string> audio_lang_pref = {"ja", "jpn", "en", "eng"};
    int subtitle_stream_idx = -1;
    AVCodecContext* subtitle_codec_ctx = nullptr;
    double duration;
    double video_time_base = 0.0;
    double audio_time_base = 0.0;
    double subtitle_time_base = 0.0;
    std::thread thread;
    bool is_loopable;
    double tick_diff;
    double last_video_time;
    double last_audio_time;
};
} // namespace ff