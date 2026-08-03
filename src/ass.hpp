#pragma once

#include <ass/ass.h>
#include <SDL3/SDL.h>

#include "ffmpeg.hpp"
#include "ass.vert.h"
#include "ass.frag.h"

#define N_EXTRA_SIZE 32

struct CopyData {
    SDL_GPUTexture *glyph_tex;
    SDL_GPUBuffer* vert_buf;
    SDL_GPUTransferBuffer* v_xfer;
    uint32_t color;
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

struct BufBuf {
    SDL_GPUBuffer *buf;
    Uint32 size;
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

class BufPool : public GPUPool<BufBuf> {
protected:
    void destroy(BufBuf *buf) {
        SDL_ReleaseGPUBuffer(device, buf->buf);
        delete buf;
    }

    int n_extra = 0;

public:
    void clear() {
        recycle();
        while (!queue.empty()) {
            destroy(queue.front());
            queue.pop();
        }
    }

    BufBuf *alloc(Uint32 size) {
        while (!queue.empty()) {
            auto d = queue.front();
            queue.pop();
            if (d->size >= size)
                return d;
            else
                destroy(d);
        }

        SDL_GPUBufferCreateInfo vb_info = {
            .usage = SDL_GPU_BUFFERUSAGE_VERTEX,
            .size = size
        };
        auto buf = SDL_CreateGPUBuffer(device, &vb_info);
        if (!buf)
            return nullptr;
		auto data = new BufBuf{.buf = buf, .size = vb_info.size};
		return data;
    }
};

class CopyDataQueue {
protected:
    SDL_GPUDevice *device;
public:
    std::vector<CopyData *> list;

    void init(SDL_GPUDevice *device) {
        this->device = device;
    }

    void clear() {
        for (auto data : list) {
            destroy(data);
        }
        list.clear();
    }

    void destroy(CopyData *data) {
        delete data;
    }

    bool empty() {
        return list.empty();
    }

    void push(CopyData *data) {
        list.push_back(data);
    }
};

class AssHandler {
public:
    void shutdown() {
        if (pipeline) {
            SDL_ReleaseGPUGraphicsPipeline(device, pipeline);
            pipeline = nullptr;
        }
        xfer_pool.clear();
        tex_pool.clear();
        copy_queue.clear();
        vert_pool.clear();
        v_xfer_pool.clear();
    }

    void init(SDL_GPUDevice *device, SDL_GPUSampler *sampler) {
        this->device = device;
        this->sampler = sampler;
        xfer_pool.init(device);
        tex_pool.init(device);
        copy_queue.init(device);
        vert_pool.init(device);
        v_xfer_pool.init(device);
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
        vert_pool.recycle();
        v_xfer_pool.recycle();
        copy_queue.clear();

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

            CopyData *data = new CopyData{
                .glyph_tex = glyph->buf
            };

            // --- B. Convert Screen Coordinates to Normalized Device Coordinates (NDC) ---
            // libass gives pixel coordinates top-left (0,0) to bottom-right (screen_w, screen_h)
            float x0 = (2.0f * img->dst_x / wnd_w) - 1.0f;
            float y0 = 1.0f - (2.0f * img->dst_y / wnd_h);
            float x1 = (2.0f * (img->dst_x + img->w) / wnd_w) - 1.0f;
            float y1 = 1.0f - (2.0f * (img->dst_y + img->h) / wnd_h);

            float max_u = (float)img->w / (float)glyph->w;
            float max_v = (float)img->h / (float)glyph->h;

            Vertex vertices[4] = {
                { x0, y0, 0.0f, 0.0f }, // Top-Left
                { x1, y0, max_u, 0.0f }, // Top-Right
                { x0, y1, 0.0f, max_v }, // Bottom-Left
                { x1, y1, max_u, max_v }  // Bottom-Right
            };

            // Create dynamic Vertex Buffer for this quad
            auto vert = vert_pool.alloc(sizeof(vertices));

            // Write vertices directly using Transfer Buffer or SDL_UploadToGPUBuffer
            // (For brevity, uploading vertex quad to GPU)
            auto v_xfer = v_xfer_pool.alloc(sizeof(vertices));
            
            void* v_map = SDL_MapGPUTransferBuffer(device, v_xfer->buf, false);
            SDL_memcpy(v_map, vertices, sizeof(vertices));
            SDL_UnmapGPUTransferBuffer(device, v_xfer->buf);

            SDL_GPUTransferBufferLocation v_transfer = { v_xfer->buf, 0 };
            SDL_GPUBufferRegion v_region = { vert->buf, 0, sizeof(vertices) };
            
            SDL_UploadToGPUBuffer(pass, &v_transfer, &v_region, false);

            xfer_pool.in_use(xfer);
            tex_pool.in_use(glyph);
            vert_pool.in_use(vert);
            v_xfer_pool.in_use(v_xfer);            
            data->vert_buf = vert->buf;
            data->v_xfer = v_xfer->buf;
            data->color = img->color;
            copy_queue.push(data);
        }
    }

    void draw(SDL_GPUCommandBuffer *cmd, SDL_GPURenderPass *pass) {
        if (copy_queue.empty())
            return;

        SDL_BindGPUGraphicsPipeline(pass, pipeline);   // the one with blending enabled            

        for (auto data : copy_queue.list) {
            uint32_t c = data->color;
            float r = ((c >> 24) & 0xFF) / 255.0f;
            float g = ((c >> 16) & 0xFF) / 255.0f;
            float b = ((c >> 8)  & 0xFF) / 255.0f;
            float a = (255 - (c & 0xFF)) / 255.0f; // libass opacity is inverted! (0 = opaque, 255 = transparent)

            float color_uniform[4] = { r, g, b, a };

            // --- D. Bind & Render Quad ---
            SDL_GPUBufferBinding v_binding = { data->vert_buf, 0 };
            SDL_BindGPUVertexBuffers(pass, 0, &v_binding, 1);

            SDL_GPUTextureSamplerBinding t_binding = { data->glyph_tex, sampler };
            SDL_BindGPUFragmentSamplers(pass, 0, &t_binding, 1);

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

        // 1. Vertex Layout Setup (Pos: float2, UV: float2)
        SDL_GPUVertexAttribute attrs[2] = {};
        attrs[0].location = 0;
        attrs[0].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
        attrs[0].offset = 0;

        attrs[1].location = 1;
        attrs[1].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
        attrs[1].offset = sizeof(float) * 2;

        SDL_GPUVertexBufferDescription buffer_desc = {};
        buffer_desc.slot = 0;
        buffer_desc.pitch = sizeof(float) * 4;

        pipeline_info.vertex_input_state.vertex_attributes = attrs;
        pipeline_info.vertex_input_state.num_vertex_attributes = 2;
        pipeline_info.vertex_input_state.vertex_buffer_descriptions = &buffer_desc;
        pipeline_info.vertex_input_state.num_vertex_buffers = 1;

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
    BufPool vert_pool;
    XferPool v_xfer_pool;
    SDL_GPUDevice *device = nullptr;
    SDL_GPUTexture *glyph_tex = nullptr;
    int wnd_w = 0;
    int wnd_h = 0;
    CopyDataQueue copy_queue;
    SDL_GPUSampler *sampler = nullptr;
};