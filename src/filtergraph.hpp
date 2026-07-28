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

class DynamicVAAPIScaler {
private:
    AVFilterGraph* filter_graph = nullptr;
    AVFilterContext* src_ctx = nullptr;
    AVFilterContext* sink_ctx = nullptr;
    
    // Constant identifier for sending commands to the scale filter node
    const std::string scale_filter_name = "vaapi_scaler";

public:
    ~DynamicVAAPIScaler() {
        if (filter_graph) {
            avfilter_graph_free(&filter_graph);
        }
    }

    void close() {
        if (filter_graph) {
            av_buffersrc_add_frame_flags(src_ctx, nullptr, 0); // Send EOF to filter
            
            // 2. Destroy old filtergraph completely
            avfilter_graph_free(&filter_graph); 
            src_ctx = nullptr;
            sink_ctx = nullptr;
        }
    }

    int init_pipeline(AVCodecContext* dec_ctx, int initial_w, int initial_h) {
        clse();

        int ret = 0;
        filter_graph = avfilter_graph_alloc();

        const AVFilter* buffer_src  = avfilter_get_by_name("buffer");
        const AVFilter* buffer_sink = avfilter_get_by_name("buffersink");

        // 1. Configure input buffer filter for VAAPI hardware surfaces
        char args[512];
        snprintf(args, sizeof(args),
            "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
            dec_ctx->width, dec_ctx->height, AV_PIX_FMT_VAAPI,
            dec_ctx->pkt_timebase.num, dec_ctx->pkt_timebase.den,
            dec_ctx->sample_aspect_ratio.num, dec_ctx->sample_aspect_ratio.den);

        ret = avfilter_graph_create_filter(&src_ctx, buffer_src, "in", args, nullptr, filter_graph);
        if (ret < 0) return ret;

        ret = avfilter_graph_create_filter(&sink_ctx, buffer_sink, "out", nullptr, nullptr, filter_graph);
        if (ret < 0) return ret;

        // 2. Attach the VAAPI hardware device context
        for (unsigned i = 0; i < filter_graph->nb_filters; i++) {
            filter_graph->filters[i]->hw_device_ctx = av_buffer_ref(dec_ctx->hw_device_ctx);
        }

        // 3. Define the scale_vaapi filter string and explicitly set an instance name using @name
        char filter_descr[256];
        snprintf(filter_descr, sizeof(filter_descr),
                 "scale_vaapi=w=%d:h=%d:mode=hq:format=nv12@%s", 
                 initial_w, initial_h, scale_filter_name.c_ptr());

        AVFilterInOut* outputs = avfilter_inout_alloc();
        AVFilterInOut* inputs  = avfilter_inout_alloc();

        outputs->name       = av_strdup("in");
        outputs->filter_ctx = src_ctx;
        outputs->pad_idx    = 0;
        outputs->next       = nullptr;

        inputs->name        = av_strdup("out");
        inputs->filter_ctx  = sink_ctx;
        inputs->pad_idx     = 0;
        inputs->next        = nullptr;

        ret = avfilter_graph_parse_ptr(filter_graph, filter_descr, &inputs, &outputs, nullptr);
        if (ret < 0) return ret;

        ret = avfilter_graph_config(filter_graph, nullptr);
        avfilter_inout_free(&inputs);
        avfilter_inout_free(&outputs);

        return ret;
    }

    // Dynamic resolution change method (Zero-Copy / Zero Graph Recreation)
    int change_resolution(int new_width, int new_height) {
        if (!filter_graph) return -1;

        char response_buffer[256] = {0};
        char size_args[64];

        // Format command arguments for updating parameters
        snprintf(size_args, sizeof(size_args), "w=%d:h=%d", new_width, new_height);

        // Send runtime command directly to the scale_vaapi filter by its assigned name
        int ret = avfilter_graph_send_command(
            filter_graph,
            scale_filter_name.c_str(),  // Target filter instance name
            "w",                        // Option to modify
            std::to_string(new_width).c_str(),
            response_buffer,
            sizeof(response_buffer),
            0
        );

        ret |= avfilter_graph_send_command(
            filter_graph,
            scale_filter_name.c_str(),
            "h",                        // Option to modify
            std::to_string(new_height).c_str(),
            response_buffer,
            sizeof(response_buffer),
            0
        );

        // Fallback: If individual parameter commands are rejected, issue a reinit command
        if (ret < 0) {
            ret = avfilter_graph_send_command(
                filter_graph,
                scale_filter_name.c_str(),
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
            std::cerr << "[VAAPI Filter] Failed to send command: " << response_buffer << std::endl;
        }

        return ret;
    }

    int process_frame(AVFrame* in_frame, AVFrame* out_frame) {
        int ret = av_buffersrc_add_frame_flags(src_ctx, in_frame, AV_BUFFERSRC_FLAG_KEEP_REF);
        if (ret < 0) return ret;

        return av_buffersink_get_frame(sink_ctx, out_frame);
    }
};