#pragma once

#include <iostream>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/opt.h>
}

namespace ff {

const extern AVPixelFormat finalPixelFormat;

class DynamicVAAPIScaler {
private:
    AVFilterGraph* filter_graph = nullptr;
    AVFilterContext* src_ctx = nullptr;
    AVFilterContext* sink_ctx = nullptr;
    int target_w = 0;
    int target_h = 0;

public:
    ~DynamicVAAPIScaler() {
        if (filter_graph) {
            avfilter_graph_free(&filter_graph);
        }
    }

    void clear() {
        if (filter_graph) {
//            av_buffersrc_add_frame_flags(src_ctx, nullptr, 0); // Send EOF to filter
            
            // 2. Destroy old filtergraph completely
            avfilter_graph_free(&filter_graph); 
            src_ctx = nullptr;
            sink_ctx = nullptr;
        }
    }

    bool is_init() {
        return filter_graph != nullptr;
    }

    int init_pipeline(AVFrame* frame, int out_w, int out_h) {
        clear();
        int ret = 0;
        filter_graph = avfilter_graph_alloc();

        // 1. Allocate and configure Source Filter ("in")
        const AVFilter* buffer_src = avfilter_get_by_name("buffer");
        src_ctx = avfilter_graph_alloc_filter(filter_graph, buffer_src, "in");

        AVBufferSrcParameters* params = av_buffersrc_parameters_alloc();
        memset(params, 0, sizeof(*params));
        params->width  = frame->width;
        params->height = frame->height;
        params->format = frame->format;

        AVRational tb = frame->time_base;
        if (tb.num == 0 || tb.den == 0) tb = {1, 1000}; // Timebase fallback
        params->time_base = tb;

        if (frame->hw_frames_ctx) {
            params->hw_frames_ctx = av_buffer_ref(frame->hw_frames_ctx);
        }

        ret = av_buffersrc_parameters_set(src_ctx, params);
        av_free(params);
        if (ret < 0) return ret;

        ret = avfilter_init_str(src_ctx, nullptr);
        if (ret < 0) return ret;

        // 2. Create Sink Filter ("out")
        const AVFilter* buffer_sink = avfilter_get_by_name("buffersink");
        sink_ctx = nullptr;
        ret = avfilter_graph_create_filter(&sink_ctx, buffer_sink, "out", nullptr, nullptr, filter_graph);
        if (ret < 0) return ret;
/*
        // 3. Set HW device context across all graph nodes
        for (unsigned i = 0; i < filter_graph->nb_filters; i++) {
            if (!filter_graph->filters[i]->hw_device_ctx && frame->hw_device_ctx) {
                filter_graph->filters[i]->hw_device_ctx = av_buffer_ref(frame->hw_device_ctx);
            }
        }
*/
        // 4. Clean Filter Description String (NO [in] or [out] brackets inside string!)
        char filter_descr[256];
        snprintf(filter_descr, sizeof(filter_descr),
                "scale_vaapi=w=%d:h=%d:mode=hq", out_w, out_h);

        // 5. Prepare In/Out linking structures
        AVFilterInOut* outputs = avfilter_inout_alloc();
        AVFilterInOut* inputs  = avfilter_inout_alloc();

        if (!outputs || !inputs) {
            avfilter_inout_free(&inputs);
            avfilter_inout_free(&outputs);
            return AVERROR(ENOMEM);
        }

        // Connects output of "in" (src_ctx) to the start of scale_vaapi
        outputs->name       = av_strdup("in");
        outputs->filter_ctx = src_ctx;
        outputs->pad_idx    = 0;
        outputs->next       = nullptr;

        // Connects output of scale_vaapi to the input of "out" (sink_ctx)
        inputs->name        = av_strdup("out");
        inputs->filter_ctx  = sink_ctx;
        inputs->pad_idx     = 0;
        inputs->next        = nullptr;

        // 6. Parse and link the graph
        ret = avfilter_graph_parse_ptr(filter_graph, filter_descr, &inputs, &outputs, nullptr);
        
        // Always free inputs/outputs after parse_ptr
        avfilter_inout_free(&inputs);
        avfilter_inout_free(&outputs);

        if (ret < 0) {
            std::cerr << "Failed to parse graph string!" << std::endl;
            return ret;
        }

        // 7. Validate full graph connectivity
        ret = avfilter_graph_config(filter_graph, nullptr);
        if (ret < 0) {
            std::cerr << "Failed to configure graph!" << std::endl;
            return ret;
        }

        target_w = out_w;
        target_h = out_h;

        return 0;
    }

    // Dynamic resolution change method (Zero-Copy / Zero Graph Recreation)
    int change_resolution(int new_width, int new_height) {
        if (!filter_graph) return -1;

        char response_buffer[256] = {0};
        char size_args[64];

        // Format command arguments for updating parameters
        snprintf(size_args, sizeof(size_args), "w=%d:h=%d", new_width, new_height);

    // 1. Send "width" command
        int ret = avfilter_graph_send_command(
            filter_graph,
            "out",  // Name of the scale_vaapi filter
            "w",                    // Correct option name
            std::to_string(new_width).c_str(),
            response_buffer,
            sizeof(response_buffer),
            0
        );

        // 2. Send "height" command
        ret |= avfilter_graph_send_command(
            filter_graph,
            "out",
            "h",                   // Correct option name
            std::to_string(new_height).c_str(),
            response_buffer,
            sizeof(response_buffer),
            0
        );

        // Fallback: If individual parameter commands are rejected, issue a reinit command
        if (ret < 0) {
            ret = avfilter_graph_send_command(
                filter_graph,
                "out",
                "reinit",
                size_args,
                response_buffer,
                sizeof(response_buffer),
                0
            );
        }

        if (ret >= 0) {
            std::cout << "[VAAPI Filter] Resolution updated successfully to " 
                      << new_width << "x" << new_height << std::endl;
        } else {
            std::cerr << "Reinit command failed (" << ret << "): " << response_buffer << std::endl;
        }

        return ret;
    }

    AVFrame *process_frame(AVFrame* frame, int width, int height) {
        if (!filter_graph || width != target_w || height != target_h) {
            init_pipeline(frame, width, height);
        }

        int ret = av_buffersrc_add_frame_flags(src_ctx, frame, AV_BUFFERSRC_FLAG_KEEP_REF);
        if (ret < 0) return nullptr;

        auto new_frame = av_frame_alloc();
        if (av_buffersink_get_frame(sink_ctx, new_frame) >= 0)
            return new_frame;
        av_frame_free(&new_frame);
        return nullptr;
    }
};

class VideoScaler {
    private:
        SwsContext *sws_ctx = nullptr;
        AVBufferPool *pool = nullptr;
        int pool_width = 0;
        int pool_height = 0;
        DynamicVAAPIScaler hw_scaler;

        bool setup_sws_context(AVFrame *frame, int width, int height)
        {
            sws_free_context(&sws_ctx);
            sws_ctx = sws_getContext(
                frame->width, frame->height, static_cast<AVPixelFormat>(frame->format), // True source format
                width, height, finalPixelFormat,       // True target format
                SWS_BICUBIC, nullptr, nullptr, nullptr);
            return sws_ctx != nullptr;
        }

        AVFrame *alloc(int width, int height) {
            if (!pool || width != pool_width || height != pool_height) {
                av_buffer_pool_uninit(&pool);
                pool_width = width;
                pool_height = height;
                int buffer_size = av_image_get_buffer_size(static_cast<AVPixelFormat>(finalPixelFormat), width, height, 32);
                pool = av_buffer_pool_init(buffer_size, NULL);
            }

            AVFrame *new_frame = av_frame_alloc();
            new_frame->format = finalPixelFormat;
            new_frame->width  = width;
            new_frame->height = height;

            // 2. Instead of av_frame_get_buffer, grab a buffer from your pool
            new_frame->buf[0] = av_buffer_pool_get(pool);
                
            // 3. Link the frame's data pointers to the pool buffer
            av_image_fill_arrays(new_frame->data, new_frame->linesize, 
                                new_frame->buf[0]->data, static_cast<AVPixelFormat>(new_frame->format), 
                                new_frame->width, new_frame->height, 32);

            return new_frame;
        }

    public:
        ~VideoScaler() {
            clear();
            av_buffer_pool_uninit(&pool);
        }

        void clear() {
            sws_free_context(&sws_ctx);
            hw_scaler.clear();
        }

        void set_target_size(AVCodecContext *dec_ctx, int width, int height) {
        }

        AVFrame *scale(AVFrame *frame, int width, int height) {
            if (frame->format == AV_PIX_FMT_VAAPI) {
                if (width == frame->width && height == frame->height)
                {
                    auto new_frame = av_frame_alloc();
                    av_frame_move_ref(new_frame, frame);
                    return new_frame;
                }
                
                auto new_frame = hw_scaler.process_frame(frame, width, height);
                new_frame->pts = frame->pts;
                new_frame->duration = frame->duration;
                return new_frame;
            } else {
                if (frame->format == finalPixelFormat && width == frame->width && height == frame->height)
                {
                    auto new_frame = av_frame_alloc();
                    av_frame_move_ref(new_frame, frame);
                    return new_frame;
                }

                if (!sws_ctx || width != sws_ctx->dst_w || height != sws_ctx->dst_h) {
                    setup_sws_context(frame, width, height);
                }
                auto new_frame = alloc(width, height);
                sws_scale(sws_ctx, frame->data, frame->linesize, 0,
                        frame->height, new_frame->data, new_frame->linesize);
                new_frame->pts = frame->pts;
                new_frame->duration = frame->duration;
                return new_frame;
            }
        }
};
}