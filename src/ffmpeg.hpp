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

namespace ff {

struct ChapterData {
    std::string title;
    double start_time; // in seconds
    double end_time;   // in seconds
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

struct Subtitle {
    int idx;
    std::string lang;
    std::string title;
    AVCodecID codec_id;
};

template <typename DataPtr, DataPtr (*AllocFunc)(), void (*FreeFunc)(DataPtr*)>
struct AvQueue {
    std::queue<DataPtr> queue;

    ~AvQueue() {
        clear();
    }

    void clear() {
        while (!queue.empty())
        {
            FreeFunc(&queue.front());
            queue.pop();
        }
    }

    DataPtr get() {
        DataPtr data_ptr = queue.front();
        queue.pop();
        return data_ptr;
    }

    DataPtr front() {
        return queue.front();
    }

    void pop() {
        queue.pop();
    }

    DataPtr alloc() {
        return AllocFunc();
    }

    template <typename CustomAllocFunc>
    DataPtr alloc(CustomAllocFunc func) {
        return func();
    }

    void push(DataPtr data_ptr) {
        if (data_ptr) {
            queue.push(data_ptr);
        }
    }

    void clear(DataPtr data_ptr) {
        if (data_ptr) {
            FreeFunc(&data_ptr);
        }
    }

    auto size() {
        return queue.size();
    }

    auto empty() {
        return queue.empty();
    }
};

typedef AvQueue<AVPacket *, av_packet_alloc, av_packet_free> PacketQueue;
typedef AvQueue<AVFrame *, av_frame_alloc, av_frame_free> FrameQueue;

class VideoFile {
public:
    const AVPixelFormat pixel_format = AV_PIX_FMT_NV12;

    VideoFile() = default;
    ~VideoFile() {
        close();
    }

    VideoFile(const VideoFile&) = delete;
    VideoFile& operator=(const VideoFile&) = delete;
    VideoFile(VideoFile&&) = delete;
    VideoFile& operator=(VideoFile&&) = delete;

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
        return true;
    }

    std::string get_stream_metadata(const AVStream* stream, const char *key) {
        if (!stream || !stream->metadata) {
            return "Unknown";
        }
        
        // Look up the key in the stream's metadata dictionary
        AVDictionaryEntry* entry = av_dict_get(stream->metadata, key, nullptr, 0);
        if (entry && entry->value) {
            return std::string(entry->value);
        }
        
        return "Unknown";
    }    

    bool find_subtitle_stream()
    {
        for (const auto& val : sub_lang_pref) {
            for (unsigned int i = 0; i < format_ctx->nb_streams; i++) {
                auto stream = format_ctx->streams[i];
                if (stream->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE) {
                    auto lang = get_stream_metadata(stream, "language");
                    if (lang == val) {
                        auto subtitle = Subtitle();
                        subtitle.idx = i;
                        subtitle.lang = lang;
                        subtitle.title = get_stream_metadata(stream, "title");
                        subtitle.codec_id = stream->codecpar->codec_id;
                        subtitles.push_back(subtitle);
                        std::cout << std::format("Subtitle: {} ({})\n", subtitle.title, subtitle.lang);
                        break;
                    }
                }
            }
        }

        if (!subtitles.empty())
        {
            subtitle_stream_idx = subtitles.front().idx;
            return true;
        }
        return false;
    }

    bool find_audio_stream()
    {
        for (const auto& val : audio_lang_pref) {
            for (unsigned int i = 0; i < format_ctx->nb_streams; i++) {
                auto stream = format_ctx->streams[i];
                if (stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
                    auto lang = get_stream_metadata(stream, "language");
                    if (lang == val) {
                        audio_stream_index = i;
                        auto title = get_stream_metadata(stream, "title");
                        std::cout << std::format("Audio: {} ({})\n", title, lang);
                        return true;
                    }
                }
            }
        }

        for (unsigned int i = 0; i < format_ctx->nb_streams; i++)
        {
            if (format_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
            {
                audio_stream_index = i;
                return true;
            }
        }
        std::cerr << "Could not find an audio stream.\n";
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
        std::cerr << "Could not find a video stream.\n";
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
//        init_vaapi_device();
        AVCodecParameters *codec_params = format_ctx->streams[video_stream_index]->codecpar;
        const AVCodec *codec = avcodec_find_decoder(codec_params->codec_id);
        video_codec_ctx = avcodec_alloc_context3(codec);
        if (hw_device_ctx) {
            video_codec_ctx->hw_device_ctx = av_buffer_ref(hw_device_ctx);
            video_codec_ctx->get_format = get_vaapi_format;
        }
        video_codec_ctx->thread_count = 0; 
        video_codec_ctx->thread_type = FF_THREAD_FRAME; // Or FF_THREAD_SLICE
        avcodec_parameters_to_context(video_codec_ctx, codec_params);
        avcodec_open2(video_codec_ctx, codec, nullptr);
        video_time_base = av_q2d(format_ctx->streams[video_stream_index]->time_base);
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

    bool setup_sws_context(AVFrame *frame)
    {
        sws_free_context(&sws_ctx);
        sws_ctx = sws_getContext(
            frame->width, frame->height, static_cast<AVPixelFormat>(frame->format), // True source format
            frame->width, frame->height, pixel_format,       // True target format
            SWS_BILINEAR, nullptr, nullptr, nullptr);
        return sws_ctx != nullptr;
    }

    template <typename AudioFeedFunc, typename VideoFeedFunc, typename SubtitleFeedFunc>
    int feed_frame(AudioFeedFunc audio_feed, VideoFeedFunc video_feed, SubtitleFeedFunc subtitle_feed)
    {
        if (!packet) packet = av_packet_alloc();
        if (!frame) frame = av_frame_alloc();

        auto read_result = av_read_frame(format_ctx, packet);
        if (read_result < 0 && read_result != AVERROR_EOF)
            return read_result;

        if (packet->stream_index == audio_stream_index)
        {
            if (avcodec_send_packet(audio_codec_ctx, packet) >= 0)
            {
                while (avcodec_receive_frame(audio_codec_ctx, frame) >= 0)
                {
                    if (frame->pts != AV_NOPTS_VALUE)
                        audio_feed(frame);
                    av_frame_unref(frame);
                }
            }
        }
        else if (packet->stream_index == video_stream_index)
        {
            video_feed(packet);
        }
        else if (packet->stream_index == subtitle_stream_idx) {
            int got_subtitle = 0;
            AVSubtitle subtitle;
            // avcodec_decode_subtitle2 is old but still the standard way to handle subtitles in modern FFmpeg
            if (avcodec_decode_subtitle2(subtitle_codec_ctx, &subtitle, &got_subtitle, packet) >= 0) {
                if (got_subtitle) {
                    subtitle_feed(subtitle, packet);
                    // Free the allocated subtitle struct memory
                    avsubtitle_free(&subtitle);
                }
            }
        }
        av_packet_unref(packet);

        return read_result;
    }

    template <typename VideoFeedFunc>
    void feed_video_frame(AVPacket *packet, VideoFeedFunc video_feed) {
        if (!video_frame) video_frame = av_frame_alloc();
        if (avcodec_send_packet(video_codec_ctx, packet) >= 0)
        {
            while (avcodec_receive_frame(video_codec_ctx, video_frame) >= 0)
            {
                if (video_frame->pts != AV_NOPTS_VALUE) {
                    video_feed(video_frame);
                }
                av_frame_unref(video_frame);
            }
        }
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

    void scale_video_frame(AVFrame *frame, AVFrame *converted_frame)
    {
        if (!sws_ctx || frame->format != sws_ctx->src_format) {
            setup_sws_context(frame);
        }
        sws_scale(sws_ctx, frame->data, frame->linesize, 0,
                  video_codec_ctx->height, converted_frame->data, converted_frame->linesize);
        converted_frame->format = pixel_format;
    }

    void scale_video_frame(AVFrame *frame, uint8_t **data, int *linesize)
    {
        sws_scale(sws_ctx, frame->data, frame->linesize, 0,
                  video_codec_ctx->height, data, linesize);
    }

    AVFrame *alloc_converted_frame()
    {
        AVFrame *frame = av_frame_alloc();
        frame->format = pixel_format;
        frame->width  = video_codec_ctx->width;
        frame->height = video_codec_ctx->height;

        if (!converted_pool) {
            int buffer_size = av_image_get_buffer_size(static_cast<AVPixelFormat>(frame->format), frame->width, frame->height, 32);
            converted_pool = av_buffer_pool_init(buffer_size, NULL);
        }

        // 2. Instead of av_frame_get_buffer, grab a buffer from your pool
        frame->buf[0] = av_buffer_pool_get(converted_pool);
            
        // 3. Link the frame's data pointers to the pool buffer
        av_image_fill_arrays(frame->data, frame->linesize, 
                            frame->buf[0]->data, static_cast<AVPixelFormat>(frame->format), 
                            frame->width, frame->height, 32);

        return frame;
    }

    void close() {
        avcodec_free_context(&audio_codec_ctx);
        avcodec_free_context(&video_codec_ctx);
        avcodec_free_context(&subtitle_codec_ctx);
        avformat_close_input(&format_ctx);
        av_frame_free(&frame);
        av_frame_free(&video_frame);
        av_buffer_unref(&hw_device_ctx);
        swr_free(&swr_ctx);
        sws_free_context(&sws_ctx);
        audio_stream_index = -1;
        video_stream_index = -1;
        subtitle_stream_idx = -1;
        av_buffer_pool_uninit(&converted_pool);
        subtitles.clear();
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

    bool is_audio() const {
        return audio_stream_index >= 0;
    }

    bool is_video() const {
        return video_stream_index >= 0;
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

    std::vector<ChapterData> ReadChapters() {
        std::vector<ChapterData> chapter_list;

        // 1. Check if the file actually contains any chapters
        if (format_ctx->nb_chapters == 0) {
            return chapter_list;
        }

        // 2. Iterate through the chapters array
        for (unsigned int i = 0; i < format_ctx->nb_chapters; i++) {
            AVChapter* chapter = format_ctx->chapters[i];
            ChapterData data;

            // 3. Convert timestamps from the chapter's unique timebase into seconds
            double timebase_factor = av_q2d(chapter->time_base);
            data.start_time = chapter->start * timebase_factor;
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
        }

        return chapter_list;
    }

    double get_start_time() {
        return static_cast<double>(format_ctx->start_time) * AV_TIME_BASE;
    }

    static enum AVPixelFormat get_vaapi_format(AVCodecContext *ctx, const enum AVPixelFormat *pix_fmts) {
        const enum AVPixelFormat *p;

        for (p = pix_fmts; *p != -1; p++) {
            if (*p == AV_PIX_FMT_VAAPI) {
                return *p;
            }
        }

        std::cerr << "Failed to get HW surface format.\n";
        return AV_PIX_FMT_NONE;
    }

    int init_vaapi_device() {
        // This looks up the default DRI device (e.g., /dev/dri/renderD128)
        int err = av_hwdevice_ctx_create(&hw_device_ctx, AV_HWDEVICE_TYPE_VAAPI, NULL, NULL, 0);
        if (err < 0) {
            std::cerr << "Failed to create VA-API hardware device.\n";
            return err;
        }
        return 0;
    }

    void print_error_str(int err) {
        char err_buf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(err, err_buf, sizeof(err_buf));
        printf("%i: %s\n", err, err_buf);
    }

    AVCodecContext *get_subtitle_ctx() {
        return subtitle_codec_ctx;
    }

private:
    AVFormatContext* format_ctx = nullptr;
    AVCodecContext* audio_codec_ctx = nullptr;
    SwrContext* swr_ctx = nullptr;
    int audio_stream_index = -1;

    AVCodecContext* video_codec_ctx = nullptr;
    SwsContext* sws_ctx = nullptr;
    int video_stream_index = -1;

    AVPacket* packet = nullptr;
    AVFrame* frame = nullptr;
    AVFrame* video_frame = nullptr;
    AVBufferRef *hw_device_ctx = NULL;
    std::vector<Subtitle> subtitles;
    std::vector<std::string> sub_lang_pref = {"en", "eng", "ja", "jpn"};
    std::vector<std::string> audio_lang_pref = {"ja", "jpn", "en", "eng"};
    int subtitle_stream_idx = -1;
    AVCodecContext* subtitle_codec_ctx = nullptr;
    double duration;
    double video_time_base = 0.0;
    double audio_time_base = 0.0;
    double subtitle_time_base = 0.0;
    AVBufferPool *converted_pool = nullptr;

public:
    double get_duration() const { return duration; }
    double get_video_time_base() const { return video_time_base; }
    double get_audio_time_base() const { return audio_time_base; }
    double get_subtitle_time_base() const { return subtitle_time_base; }
#ifdef _VIDEO_CONVERTER_THREAD_
    PacketQueue video_packet_queue;
#endif
    FrameQueue video_frame_queue;
};
} // namespace ff