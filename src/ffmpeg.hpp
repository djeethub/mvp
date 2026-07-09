#pragma once

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
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

    bool open(const std::string& filename) {
        close();

        if (avformat_open_input(&format_ctx, filename.c_str(), nullptr, nullptr) < 0) {
            std::cerr << "Could not open video file: " << filename << '\n';
            return false;
        }

        if (avformat_find_stream_info(format_ctx, nullptr) < 0) {
            std::cerr << "Could not find stream information for: " << filename << '\n';
            close();
            return false;
        }

        audio_stream_index = -1;
        video_stream_index = -1;
        return true;
    }

    bool find_audio_stream() {
        if (!format_ctx) {
            std::cerr << "No format context available.\n";
            audio_stream_index = -1;
            return false;
        }

        for (unsigned int i = 0; i < format_ctx->nb_streams; ++i) {
            const auto* stream = format_ctx->streams[i];
            if (stream && stream->codecpar && stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
                audio_stream_index = static_cast<int>(i);
                return true;
            }
        }

        audio_stream_index = -1;
        std::cerr << "Could not find an audio stream.\n";
        return false;
    }

    bool find_video_stream() {
        if (!format_ctx) {
            std::cerr << "No format context available.\n";
            video_stream_index = -1;
            return false;
        }

        for (unsigned int i = 0; i < format_ctx->nb_streams; ++i) {
            const auto* stream = format_ctx->streams[i];
            if (stream && stream->codecpar && stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                video_stream_index = static_cast<int>(i);
                return true;
            }
        }

        video_stream_index = -1;
        std::cerr << "Could not find a video stream.\n";
        return false;
    }

    bool open_audio_decoder() {
        if (!format_ctx || audio_stream_index < 0) {
            std::cerr << "Audio stream is not available.\n";
            return false;
        }

        const auto* stream = format_ctx->streams[audio_stream_index];
        if (!stream || !stream->codecpar) {
            std::cerr << "Audio stream parameters are missing.\n";
            return false;
        }

        const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
        if (!codec) {
            std::cerr << "Could not find an audio decoder.\n";
            return false;
        }

        auto* codec_ctx = avcodec_alloc_context3(codec);
        if (!codec_ctx) {
            std::cerr << "Could not allocate audio decoder context.\n";
            return false;
        }

        if (avcodec_parameters_to_context(codec_ctx, stream->codecpar) < 0) {
            std::cerr << "Could not copy audio stream parameters to decoder context.\n";
            avcodec_free_context(&codec_ctx);
            return false;
        }

        if (avcodec_open2(codec_ctx, codec, nullptr) < 0) {
            std::cerr << "Could not open audio decoder.\n";
            avcodec_free_context(&codec_ctx);
            return false;
        }

        avcodec_free_context(&audio_codec_ctx);
        audio_codec_ctx = codec_ctx;
        return true;
    }

    bool open_video_decoder() {
        if (!format_ctx || video_stream_index < 0) {
            std::cerr << "Video stream is not available.\n";
            return false;
        }

        const auto* stream = format_ctx->streams[video_stream_index];
        if (!stream || !stream->codecpar) {
            std::cerr << "Video stream parameters are missing.\n";
            return false;
        }

        const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
        if (!codec) {
            std::cerr << "Could not find a video decoder.\n";
            return false;
        }

        auto* codec_ctx = avcodec_alloc_context3(codec);
        if (!codec_ctx) {
            std::cerr << "Could not allocate video decoder context.\n";
            return false;
        }

        if (avcodec_parameters_to_context(codec_ctx, stream->codecpar) < 0) {
            std::cerr << "Could not copy video stream parameters to decoder context.\n";
            avcodec_free_context(&codec_ctx);
            return false;
        }

        if (avcodec_open2(codec_ctx, codec, nullptr) < 0) {
            std::cerr << "Could not open video decoder.\n";
            avcodec_free_context(&codec_ctx);
            return false;
        }

        avcodec_free_context(&video_codec_ctx);
        video_codec_ctx = codec_ctx;
        return true;
    }

    bool setup_swr_context() {
        if (!audio_codec_ctx) {
            std::cerr << "Audio decoder context is not ready.\n";
            return false;
        }

        AVChannelLayout dst_layout{};
        AVChannelLayout src_layout{};
        av_channel_layout_default(&dst_layout, 2);

        if (av_channel_layout_copy(&src_layout, &audio_codec_ctx->ch_layout) < 0) {
            std::cerr << "Could not copy source channel layout.\n";
            av_channel_layout_uninit(&dst_layout);
            return false;
        }

        swr_free(&swr_ctx);
        int swr_err = swr_alloc_set_opts2(
            &swr_ctx,
            &dst_layout,
            AV_SAMPLE_FMT_S16,
            44100,
            &src_layout,
            audio_codec_ctx->sample_fmt,
            audio_codec_ctx->sample_rate,
            0,
            nullptr);

        av_channel_layout_uninit(&src_layout);
        av_channel_layout_uninit(&dst_layout);

        if (swr_err < 0 || !swr_ctx || swr_init(swr_ctx) < 0) {
            std::cerr << "Failed to initialize the resampling context.\n";
            swr_free(&swr_ctx);
            return false;
        }

        return true;
    }

    bool setup_sws_context() {
        if (!video_codec_ctx) {
            std::cerr << "Video decoder context is not ready.\n";
            return false;
        }

        sws_freeContext(sws_ctx);
        sws_ctx = sws_getContext(
            video_codec_ctx->width,
            video_codec_ctx->height,
            video_codec_ctx->pix_fmt,
            video_codec_ctx->width,
            video_codec_ctx->height,
            AV_PIX_FMT_YUV420P,
            SWS_BILINEAR,
            nullptr,
            nullptr,
            nullptr);
        return sws_ctx != nullptr;
    }

    template <typename AudioFeedFunc, typename VideoFeedFunc>
    bool feed_frame(AudioFeedFunc audio_feed, VideoFeedFunc video_feed) {
        if (!packet) {
            packet = av_packet_alloc();
        }
        if (!frame) {
            frame = av_frame_alloc();
        }
        if (!packet || !frame || !format_ctx) {
            std::cerr << "FFmpeg packet/frame state is not initialized.\n";
            return false;
        }

        if (av_read_frame(format_ctx, packet) < 0) {
            return false;
        }

        if (packet->stream_index == audio_stream_index) {
            if (audio_codec_ctx && swr_ctx && avcodec_send_packet(audio_codec_ctx, packet) >= 0) {
                while (avcodec_receive_frame(audio_codec_ctx, frame) >= 0) {
                    const int out_samples = av_rescale_rnd(
                        swr_get_delay(swr_ctx, 44100) + frame->nb_samples,
                        44100,
                        audio_codec_ctx->sample_rate,
                        AV_ROUND_UP);

                    std::vector<uint8_t> output_buffer(
                        av_samples_get_buffer_size(nullptr, 2, std::max(out_samples, 0), AV_SAMPLE_FMT_S16, 0));
                    auto* output_data = output_buffer.empty() ? nullptr : output_buffer.data();

                    const auto* const* input_data = reinterpret_cast<const uint8_t* const*>(frame->data);
                    const int converted_samples = swr_convert(
                        swr_ctx,
                        &output_data,
                        std::max(out_samples, 0),
                        input_data,
                        frame->nb_samples);

                    if (converted_samples > 0) {
                        const int buffer_size = av_samples_get_buffer_size(
                            nullptr,
                            2,
                            converted_samples,
                            AV_SAMPLE_FMT_S16,
                            1);
                        if (buffer_size > 0) {
                            audio_feed(output_data, buffer_size);
                        }
                    }

                    av_frame_unref(frame);
                }
            }
        } else if (packet->stream_index == video_stream_index) {
            if (video_codec_ctx && avcodec_send_packet(video_codec_ctx, packet) >= 0) {
                while (avcodec_receive_frame(video_codec_ctx, frame) >= 0) {
                    video_feed(frame);
                    av_frame_unref(frame);
                }
            }
        }

        av_packet_unref(packet);
        return true;
    }

    void scale_video_frame(AVFrame* frame, AVFrame* converted_frame) {
        if (!sws_ctx || !frame || !converted_frame || !video_codec_ctx) {
            return;
        }

        sws_scale(
            sws_ctx,
            frame->data,
            frame->linesize,
            0,
            video_codec_ctx->height,
            converted_frame->data,
            converted_frame->linesize);
    }

    AVFrame* alloc_converted_frame() {
        if (!video_codec_ctx) {
            return nullptr;
        }

        AVFrame* converted_frame = av_frame_alloc();
        if (!converted_frame) {
            return nullptr;
        }

        converted_frame->format = AV_PIX_FMT_YUV420P;
        converted_frame->width = video_codec_ctx->width;
        converted_frame->height = video_codec_ctx->height;
        if (av_frame_get_buffer(converted_frame, 0) < 0) {
            av_frame_free(&converted_frame);
            return nullptr;
        }
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

    double get_audio_time_base() const {
        if (!format_ctx || audio_stream_index < 0 || audio_stream_index >= format_ctx->nb_streams) {
            return 0.0;
        }
        const auto* stream = format_ctx->streams[audio_stream_index];
        return stream ? av_q2d(stream->time_base) : 0.0;
    }

    double get_video_time_base() const {
        if (!format_ctx || video_stream_index < 0 || video_stream_index >= format_ctx->nb_streams) {
            return 0.0;
        }
        const auto* stream = format_ctx->streams[video_stream_index];
        return stream ? av_q2d(stream->time_base) : 0.0;
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