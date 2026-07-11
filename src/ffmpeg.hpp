#pragma once

#include <iostream>
#include <string>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

namespace ff {
class VideoFile {
public:
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
        return true;
    }

    bool find_audio_stream()
    {
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
        return true;
    }

    bool open_video_decoder()
    {
        AVCodecParameters *codec_params = format_ctx->streams[video_stream_index]->codecpar;
        const AVCodec *codec = avcodec_find_decoder(codec_params->codec_id);
        video_codec_ctx = avcodec_alloc_context3(codec);
        avcodec_parameters_to_context(video_codec_ctx, codec_params);
        avcodec_open2(video_codec_ctx, codec, nullptr);
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

    bool setup_sws_context()
    {
        sws_ctx = sws_getContext(
            video_codec_ctx->width, video_codec_ctx->height, video_codec_ctx->pix_fmt, // True source format
            video_codec_ctx->width, video_codec_ctx->height, AV_PIX_FMT_YUV420P,       // True target format
            SWS_BILINEAR, nullptr, nullptr, nullptr);
        return sws_ctx != nullptr;
    }

    template <typename AudioFeedFunc, typename VideoFeedFunc>
    int feed_frame(AudioFeedFunc audio_feed, VideoFeedFunc video_feed)
    {
        if (!packet)
            packet = av_packet_alloc();
        if (!frame)
            frame = av_frame_alloc();

        auto read_result = av_read_frame(format_ctx, packet);
        if (read_result < 0)
            return read_result;

        // Is this data slice an audio packet?
        if (packet->stream_index == audio_stream_index)
        {
            if (avcodec_send_packet(audio_codec_ctx, packet) >= 0)
            {
                while (avcodec_receive_frame(audio_codec_ctx, frame) >= 0)
                {
                    // 1. Calculate max potential samples we will get after conversion
                    int out_samples = av_rescale_rnd(
                        swr_get_delay(swr_ctx, 44100) + frame->nb_samples,
                        44100,
                        audio_codec_ctx->sample_rate,
                        AV_ROUND_UP);

                    // 2. Allocate pointers for the destination buffer (2 channels, S16 format)
                    uint8_t *output_buffer = nullptr;
                    av_samples_alloc(&output_buffer, nullptr, 2, out_samples, AV_SAMPLE_FMT_S16, 0);

                    // 3. Perform the actual conversion safely
                    int converted_samples = swr_convert(
                        swr_ctx,
                        &output_buffer,
                        out_samples,
                        (const uint8_t **)frame->data,
                        frame->nb_samples);

                    if (converted_samples > 0)
                    {
                        // Calculate the exact size of the resulting packed bytes
                        // 2 channels * number of converted samples * 2 bytes per sample (S16)
                        int buffer_size = converted_samples * 2 * sizeof(int16_t);

                        // 4. Feed the clean, packed stereo bytes to PipeWire via SDL3
                        audio_feed(output_buffer, buffer_size);
                    }

                    // Free the temporary buffer array
                    av_freep(&output_buffer);
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
                    video_feed(frame);
                    av_frame_unref(frame);
                }
            }
        }
        av_packet_unref(packet); // Recycle memory node cleanly

        return read_result;
    }

    void scale_video_frame(AVFrame *frame, AVFrame *converted_frame)
    {
        sws_scale(sws_ctx, frame->data, frame->linesize, 0,
                  video_codec_ctx->height, converted_frame->data, converted_frame->linesize);
    }

    AVFrame *alloc_converted_frame()
    {
        AVFrame *converted_frame = av_frame_alloc();
        converted_frame->format = AV_PIX_FMT_YUV420P;
        converted_frame->width = video_codec_ctx->width;
        converted_frame->height = video_codec_ctx->height;
        av_frame_get_buffer(converted_frame, 0); // Allocate the internal pixel memory arrays
        return converted_frame;
    }

    void close() {
        swr_free(&swr_ctx);
        avcodec_free_context(&audio_codec_ctx);
        avcodec_free_context(&video_codec_ctx);
        avformat_close_input(&format_ctx);
        av_packet_free(&packet);
        av_frame_free(&frame);
        audio_stream_index = -1;
        video_stream_index = -1;
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

    double get_audio_time_base() const
    {
        return audio_codec_ctx ? av_q2d(format_ctx->streams[audio_stream_index]->time_base) : 0.0;
    }

    double get_video_time_base() const
    {
        return video_codec_ctx ? av_q2d(format_ctx->streams[video_stream_index]->time_base) : 0.0;
    }

    int64_t seek(int64_t ts)
    {
        int seek_result = avformat_seek_file(
                format_ctx,
                -1,
                INT64_MIN, ts, INT64_MAX,
                AVSEEK_FLAG_BACKWARD);
        if (seek_result >= 0) {
            avcodec_flush_buffers(audio_codec_ctx);
            avcodec_flush_buffers(video_codec_ctx);
        }
        return seek_result;
    }

    double get_duration() {
        return format_ctx->duration / (double)AV_TIME_BASE;
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
};
} // namespace ff