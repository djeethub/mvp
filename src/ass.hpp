#pragma once

#include <ass/ass.h>
#include <SDL3/SDL.h>

#include "ffmpeg.hpp"
#include "ass.vert.h"
#include "ass.frag.h"

#define N_EXTRA_SIZE 32

struct alignas(16) TransformData {
    float position[2]; // x, y (in NDC: -1.0 to 1.0)
    float size[2];     // width, height (in NDC: 0.0 to 2.0)
    float uv[2];
    float padding[2];
};

struct CopyData {
    SDL_GPUTexture *glyph_tex;
    uint32_t color;
    TransformData transform;
};

struct XferBuf {
    SDL_GPUTransferBuffer *buf;
    Uint32 size;
};

struct TexBuf {
    SDL_GPUTexture *buf;
    Uint32 w;
    Uint32 h;
};

template <typename T>
class GPUPool {
protected:
    std::queue<T *> queue;
    std::queue<T *> in_use_queue;
    SDL_GPUDevice *device;

public:
    void init(SDL_GPUDevice *device) {
        this->device = device;
    }

    void recycle(T *buf) {
        queue.push(buf);
    }

    void in_use(T *buf) {
        in_use_queue.push(buf);
    }

    void recycle() {
        while (!in_use_queue.empty()) {
            queue.push(in_use_queue.front());
            in_use_queue.pop();
        }
    }
};

class XferPool : public GPUPool<XferBuf> {
protected:
    void destroy(XferBuf *buf) {
        SDL_ReleaseGPUTransferBuffer(device, buf->buf);
        delete buf;
    }

    int n_extra = 0;

public:
    XferPool(int extra = 0) : n_extra(extra) {}

    void clear() {
        recycle();
        while (!queue.empty()) {
            destroy(queue.front());
            queue.pop();
        }
    }

    XferBuf *alloc(Uint32 size) {
        while (!queue.empty()) {
            auto d = queue.front();
            queue.pop();
            if (d->size >= size)
                return d;
            else
                destroy(d);
        }

		SDL_GPUTransferBufferCreateInfo tb_info = {
			.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
			.size = size + n_extra,
		};
		auto buf = SDL_CreateGPUTransferBuffer(device, &tb_info);
        if (!buf)
            return nullptr;
		auto data = new XferBuf{.buf = buf, .size = tb_info.size};
		return data;
    }
};

class TexPool : public GPUPool<TexBuf> {
protected:
    void destroy(TexBuf *buf) {
        SDL_ReleaseGPUTexture(device, buf->buf);
        delete buf;
    }

public:
    void clear() {
        recycle();
        while (!queue.empty()) {
            destroy(queue.front());
            queue.pop();
        }
    }

    TexBuf *alloc(Uint32 w, Uint32 h) {
        while (!queue.empty()) {
            auto d = queue.front();
            queue.pop();
            if (d->w >= w && d->h >= h)
                return d;
            else
                destroy(d);
        }

        w += N_EXTRA_SIZE; h += N_EXTRA_SIZE;
        SDL_GPUTextureCreateInfo tex_info = {};
        tex_info.type = SDL_GPU_TEXTURETYPE_2D;
        tex_info.format = SDL_GPU_TEXTUREFORMAT_R8_UNORM;
        tex_info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
        tex_info.width = w;
        tex_info.height = h;
        tex_info.layer_count_or_depth = 1;
        tex_info.num_levels = 1;
        auto tex = SDL_CreateGPUTexture(device, &tex_info);
        if (!tex)
            return nullptr;
		auto data = new TexBuf{.buf = tex, .w = w, .h = h };
		return data;
    }
};

class CopyDataPool {
protected:
    std::vector<CopyData *> in_use_list;
    std::vector<CopyData *> list;

public:
    auto get_in_use() {
        return in_use_list;
    }

    void recycle(CopyData *buf) {
        list.push_back(buf);
    }

    void in_use(CopyData *buf) {
        in_use_list.push_back(buf);
    }

    void recycle() {
        list.insert(list.end(), in_use_list.begin(), in_use_list.end());
        in_use_list.clear();
    }

    void clear() {
        recycle();
        for (auto data : list) {
            delete data;
        }
        list.clear();
    }

    CopyData *alloc() {
        if (!list.empty()) {
            auto data = list.back();
            list.pop_back();
            return data;
        }

        return new CopyData;
    }
};

class AssHandler {
public:
    void shutdown() {
        SDL_ReleaseGPUGraphicsPipeline(device, pipeline);
        pipeline = nullptr;
        SDL_ReleaseGPUSampler(device, sampler);
        sampler = nullptr;
        xfer_pool.clear();
        tex_pool.clear();
        copy_pool.clear();
    }

    void init_gpu(SDL_GPUDevice *device) {
        if (this->device)
            return;
        this->device = device;
        xfer_pool.init(device);
        tex_pool.init(device);

		SDL_GPUSamplerCreateInfo samp_info = {
			.min_filter = SDL_GPU_FILTER_LINEAR,
			.mag_filter = SDL_GPU_FILTER_LINEAR,
			.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST,
			.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
			.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
		};
		sampler = SDL_CreateGPUSampler(device, &samp_info);
    }

    bool init(int width, int height, AVCodecContext *subtitle_codec_ctx, SDL_Window *window) {
        ass_track.reset();
        ass_renderer.reset();

        SDL_GetWindowSizeInPixels(window, &wnd_w, &wnd_h);

        // 1. Initialize your standard libass environment
        if (!ass_library)
            ass_library.reset(ass_library_init());
        ass_renderer.reset(ass_renderer_init(ass_library.get()));
        ass_set_fonts(ass_renderer.get(), nullptr, "Sans", 1, nullptr, 1);
        ass_set_storage_size(ass_renderer.get(), width, height);
        ass_set_frame_size(ass_renderer.get(), wnd_w, wnd_h); // Match your window canvas size
//        ass_set_hinting(ass_renderer.get(), ASS_HINTING_NONE);

        // 2. Create an EMPTY, blank track that you will feed packets manually
        ass_track.reset(ass_new_track(ass_library.get()));

        if (subtitle_codec_ctx->subtitle_header_size > 0) {
            ass_process_codec_private(
                ass_track.get(), 
                (char*)subtitle_codec_ctx->subtitle_header, 
                subtitle_codec_ctx->subtitle_header_size
            );

            return init_pipeline(window);
        }
        return false;
    }

    void flush() {
        if (ass_track)
            ass_flush_events(ass_track.get());
    }

    void add_ass(const std::string& text, long long pts, long long duration) {
        ass_process_chunk(
                        ass_track.get(), 
                        text.c_str(),          // The raw time-stripped ASS string payload
                        text.length(),  // String length
                        pts,             // Explicit start time (ms)
                        duration         // Explicit duration length (ms)
        );
    }

    struct Vertex {
        float x, y;
        float u, v;
    };

    void prepare_draw(SDL_GPUCopyPass *pass, double play_time) {
        if (!ass_track)
            return;

        xfer_pool.recycle();
        tex_pool.recycle();
        copy_pool.recycle();

        int changed = 0;
        // Ask libass to process the track at this specific millisecond frame marker
        ASS_Image* img = ass_render_frame(ass_renderer.get(), ass_track.get(), play_time * 1000, &changed);
        // 3. Draw the active text lines over the frame canvas
        for (; img; img = img->next) {
//            printf("SUCCESS: libass generated image chunks! w=%d, h=%d at position x=%d, y=%d\n", img->w, img->h, img->dst_x, img->dst_y);
            if (img->w == 0 || img->h == 0)
                continue;

            auto xfer = xfer_pool.alloc(img->w * img->h);
            uint8_t* dst = (uint8_t*)SDL_MapGPUTransferBuffer(device, xfer->buf, false);
            const uint8_t* src = img->bitmap;
            for (int y = 0; y < img->h; ++y) {
                SDL_memcpy(dst + (y * img->w), src + (y * img->stride), img->w);
            }
            SDL_UnmapGPUTransferBuffer(device, xfer->buf);

            auto glyph = tex_pool.alloc(img->w, img->h);
            SDL_GPUTextureTransferInfo transfer_info = {
                .transfer_buffer = xfer->buf
            };
            SDL_GPUTextureRegion region = {
                .texture = glyph->buf,
                .w = static_cast<Uint32>(img->w),
                .h = static_cast<Uint32>(img->h),
                .d = 1
            };
            SDL_UploadToGPUTexture(pass, &transfer_info, &region, false);

            // --- B. Convert Screen Coordinates to Normalized Device Coordinates (NDC) ---
            // libass gives pixel coordinates top-left (0,0) to bottom-right (screen_w, screen_h)
            float x0 = (2.0f * img->dst_x / wnd_w) - 1.0f;
            float y0 = 1.0f - (2.0f * img->dst_y / wnd_h);
            float w1 = 2.0f * img->w / wnd_w;
            float h1 = 2.0f * img->h / wnd_h;
            float max_u = (float)img->w / (float)glyph->w;
            float max_v = (float)img->h / (float)glyph->h;

            CopyData *data = copy_pool.alloc();
            *data = {
                .glyph_tex = glyph->buf,
                .color = img->color,
                .transform = {
                    .position = { x0, y0 },
                    .size = { w1, h1 },
                    .uv = { max_u, max_v }
                }
            };

            xfer_pool.in_use(xfer);
            tex_pool.in_use(glyph);
            copy_pool.in_use(data);
        }
    }

    void draw(SDL_GPUCommandBuffer *cmd, SDL_GPURenderPass *pass) {
        auto list = copy_pool.get_in_use();
        if (list.empty())
            return;

        SDL_BindGPUGraphicsPipeline(pass, pipeline);   // the one with blending enabled            

        for (auto data : list) {
            uint32_t c = data->color;
            float r = ((c >> 24) & 0xFF) / 255.0f;
            float g = ((c >> 16) & 0xFF) / 255.0f;
            float b = ((c >> 8)  & 0xFF) / 255.0f;
            float a = (255 - (c & 0xFF)) / 255.0f; // libass opacity is inverted! (0 = opaque, 255 = transparent)

            float color_uniform[4] = { r, g, b, a };

            // --- D. Bind & Render Quad ---
            SDL_GPUTextureSamplerBinding t_binding = { data->glyph_tex, sampler };
            SDL_BindGPUFragmentSamplers(pass, 0, &t_binding, 1);
            SDL_PushGPUVertexUniformData(cmd, 0, &data->transform, sizeof(data->transform));
            SDL_PushGPUFragmentUniformData(cmd, 0, color_uniform, sizeof(color_uniform));
            SDL_DrawGPUPrimitives(pass, 4, 1, 0, 0);
        }
    }

    void set_target_size(int width, int height) {
        if (ass_renderer)
            ass_set_frame_size(ass_renderer.get(), width, height); // Match your window canvas size        
    }

private:
    bool init_pipeline(SDL_Window *window) {
        if (pipeline)
            return true;

		SDL_GPUShaderCreateInfo shader_info = {
            .code_size = ass_vert_len,
            .code = ass_vert,
			.entrypoint = "main",
			.format = SDL_GPU_SHADERFORMAT_SPIRV,
            .stage = SDL_GPU_SHADERSTAGE_VERTEX,
            .num_uniform_buffers = 1
		};
        SDL_GPUShader *vert_shader = SDL_CreateGPUShader(device, &shader_info);
        shader_info.code_size = ass_frag_len,
        shader_info.code = ass_frag;
        shader_info.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
		shader_info.num_samplers = 1;
    	shader_info.num_uniform_buffers = 1;
        SDL_GPUShader *frag_shader = SDL_CreateGPUShader(device, &shader_info);

        SDL_GPUGraphicsPipelineCreateInfo pipeline_info = {
            .vertex_shader = vert_shader,
            .fragment_shader = frag_shader,
            .primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLESTRIP
        };

        // 2. Enable Standard Alpha Blending
        SDL_GPUColorTargetDescription color_desc = {};
        color_desc.format = SDL_GetGPUSwapchainTextureFormat(device, window);
        color_desc.blend_state.enable_blend = true;
        color_desc.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
        color_desc.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        color_desc.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
        color_desc.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        color_desc.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        color_desc.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;

        pipeline_info.target_info.color_target_descriptions = &color_desc;
        pipeline_info.target_info.num_color_targets = 1;

        pipeline = SDL_CreateGPUGraphicsPipeline(device, &pipeline_info);
        SDL_ReleaseGPUShader(device, vert_shader);
		SDL_ReleaseGPUShader(device, frag_shader);
        return pipeline != nullptr;
    }

private:
    std::unique_ptr<ASS_Library, decltype(&ass_library_done)> ass_library{nullptr, ass_library_done};
    std::unique_ptr<ASS_Renderer, decltype(&ass_renderer_done)> ass_renderer{nullptr, ass_renderer_done};
    std::unique_ptr<ASS_Track, decltype(&ass_free_track)> ass_track{nullptr, ass_free_track};
    SDL_GPUGraphicsPipeline *pipeline = nullptr;
    XferPool xfer_pool{N_EXTRA_SIZE * N_EXTRA_SIZE};
    TexPool tex_pool;
    SDL_GPUDevice *device = nullptr;
    SDL_GPUTexture *glyph_tex = nullptr;
    int wnd_w = 0;
    int wnd_h = 0;
    CopyDataPool copy_pool;
    SDL_GPUSampler *sampler = nullptr;
};