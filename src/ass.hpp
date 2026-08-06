#pragma once

#include <ass/ass.h>
#include <SDL3/SDL.h>

#include "ffmpeg.hpp"
#include "ass.vert.h"
#include "ass.frag.h"

template <typename T>
class GPUPool {
protected:
    std::vector<T *> in_use_list;
    std::vector<T *> list;
    SDL_GPUDevice *device;

public:
    void init(SDL_GPUDevice *device) {
        this->device = device;
    }

    auto get_in_use() {
        return in_use_list;
    }

    void recycle(T *buf) {
        buf->reset();
        list.push_back(buf);
    }

    void in_use(T *buf) {
        in_use_list.push_back(buf);
    }

    void recycle() {
        while (!in_use_list.empty()) {
            auto data = in_use_list.back();
            data->reset();
            list.push_back(data);
            in_use_list.pop_back();
        }
    }

    void destroy(T *data) {
        data->destroy(device);
    }

    void clear() {
        recycle();
        for (auto data : list) {
            destroy(data);
        }
        list.clear();
    }
};

struct Vertex {
    float x, y;
    float u, v;
    float r, g, b, a;
};

struct AtlasRegion {
    float u0, v0; // Top-Left UV
    float u1, v1; // Bottom-Right UV
};

struct Atlas {
    SDL_GPUTexture *tex;
    Uint32 w;
    Uint32 h;
    SDL_GPUTransferBuffer *buf;
    std::vector<Vertex> vertices;

    SDL_GPUBuffer *vert_buf;

// Shelf packer state
    uint32_t current_x = 0;
    uint32_t current_y = 0;
    uint32_t current_shelf_height = 0;
    const uint32_t padding = 1; // 1px padding to avoid bilinear filtering artifacts
    Uint32 offset;

    // Allocate space for a new glyph inside the atlas
    bool alloc_region(uint32_t glyph_w, uint32_t glyph_h, uint32_t& out_x, uint32_t& out_y, AtlasRegion& out_uv) {
        uint32_t alloc_w = glyph_w + padding;
        uint32_t alloc_h = glyph_h + padding;

        // Check if glyph fits on the current shelf
        if (current_x + alloc_w > w) {
            // Move down to next shelf
            current_y += current_shelf_height;
            current_x = 0;
            current_shelf_height = 0;
        }

        // Check if atlas is completely full
        if (current_y + alloc_h > h) {
            return false; // Atlas full! Needs flush or clear.
        }

        out_x = current_x;
        out_y = current_y;

        // Calculate normalized UV coordinates
        out_uv.u0 = (float)out_x / (float)w;
        out_uv.v0 = (float)out_y / (float)h;
        out_uv.u1 = (float)(out_x + glyph_w) / (float)w;
        out_uv.v1 = (float)(out_y + glyph_h) / (float)h;

        // Update shelf trackers
        current_x += alloc_w;
        if (alloc_h > current_shelf_height) {
            current_shelf_height = alloc_h;
        }

        return true;
    }

    // Reset atlas state when cleared (or when seeking video)
    void reset() {
        vertices.clear();
        current_x = 0;
        current_y = 0;
        current_shelf_height = 0;
        offset = 0;
        vert_buf = nullptr;
    }

    void destroy(SDL_GPUDevice* gpu) {
        if (tex)
            SDL_ReleaseGPUTexture(gpu, tex);
        if (buf)
            SDL_ReleaseGPUTransferBuffer(gpu, buf);
        delete this;
    }
};

class AtlasPool : public GPUPool<Atlas> {
public:
    Atlas *alloc(Uint32 w, Uint32 h) {
        if (!list.empty()) {
            auto data = list.back();
            list.pop_back();
            return data;
        }

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

        SDL_GPUTransferBufferCreateInfo tb_info = {
			.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
			.size = w * h,
		};
		auto buf = SDL_CreateGPUTransferBuffer(device, &tb_info);
        if (!buf) {
            SDL_ReleaseGPUTexture(device, tex);
            return nullptr;
        }
 
        return new Atlas {
            .tex = tex,
            .w = w,
            .h = h,
            .buf = buf,
        };
    }
};

struct VertexBuf {
    SDL_GPUBuffer *buf;
    SDL_GPUTransferBuffer *xfer_buf;
    Uint32 size;

    void destroy(SDL_GPUDevice *device) {
        if (buf)
            SDL_ReleaseGPUBuffer(device, buf);
        if (xfer_buf)
            SDL_ReleaseGPUTransferBuffer(device, xfer_buf);
        delete this;
    }

    void reset() {}
};

class VertexPool : public GPUPool<VertexBuf> {
public:
    VertexBuf *alloc(Uint32 size) {
        while (!list.empty()) {
            auto data = list.back();
            list.pop_back();
            if (data->size >= size)
                return data;
            data->destroy(device);
        }

        size *= 3;
        SDL_GPUBufferCreateInfo vb_info = {
            .usage = SDL_GPU_BUFFERUSAGE_VERTEX,
            .size = size
        };
        auto buf = SDL_CreateGPUBuffer(device, &vb_info);
        if (!buf)
            return nullptr;

        SDL_GPUTransferBufferCreateInfo tb_info = {
			.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
			.size = size,
		};
		auto xfer_buf = SDL_CreateGPUTransferBuffer(device, &tb_info);
        if (!xfer_buf) {
            SDL_ReleaseGPUBuffer(device, buf);
            return nullptr;
        }
 
        return new VertexBuf {
            .buf = buf,
            .xfer_buf = xfer_buf,
            .size = size
        };
    }
};

class AssHandler {
public:
    void shutdown() {
        SDL_ReleaseGPUGraphicsPipeline(device, pipeline);
        pipeline = nullptr;
        SDL_ReleaseGPUSampler(device, sampler);
        sampler = nullptr;
        atlas_pool.clear();
        vertex_pool.clear();
    }

    void init_once(SDL_GPUDevice *device) {
        if (this->device)
            return;
        this->device = device;
        atlas_pool.init(device);
        vertex_pool.init(device);

		SDL_GPUSamplerCreateInfo samp_info = {
			.min_filter = SDL_GPU_FILTER_LINEAR,
			.mag_filter = SDL_GPU_FILTER_LINEAR,
			.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST,
			.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
			.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
		};
		sampler = SDL_CreateGPUSampler(device, &samp_info);

        ass_library.reset(ass_library_init());
    }

    bool init(int width, int height, AVCodecContext *subtitle_codec_ctx, AVFormatContext *format_ctx, SDL_Window *window) {
        ass_track.reset();
        ass_renderer.reset();
        SDL_GetWindowSizeInPixels(window, &wnd_w, &wnd_h);

        // 1. Initialize your standard libass environment
        load_embedded_fonts(format_ctx);
        ass_renderer.reset(ass_renderer_init(ass_library.get()));
        ass_set_fonts(ass_renderer.get(), nullptr, "Sans", 1, nullptr, 0);
        ass_set_storage_size(ass_renderer.get(), width, height);
        ass_set_frame_size(ass_renderer.get(), wnd_w, wnd_h); // Match your window canvas size
        ass_set_hinting(ass_renderer.get(), ASS_HINTING_NATIVE);

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

    void load_embedded_fonts(AVFormatContext* format_ctx) {
        ass_clear_fonts(ass_library.get());

        // Loop through all tracks/streams in the container
        for (unsigned int i = 0; i < format_ctx->nb_streams; i++) {
            AVStream* stream = format_ctx->streams[i];
            
            // Look specifically for file attachments
            if (stream->codecpar->codec_type == AVMEDIA_TYPE_ATTACHMENT) {
                
                // Extract the font filename from metadata
                std::string font_name = "unknown_font";
                AVDictionaryEntry* tag = av_dict_get(stream->metadata, "filename", nullptr, 0);
                if (tag && tag->value) {
                    font_name = tag->value;
                }

                // Verify that the attachment contains data
                if (stream->codecpar->extradata && stream->codecpar->extradata_size > 0) {
                    
                    char* font_data = reinterpret_cast<char*>(stream->codecpar->extradata);
                    int font_data_size = stream->codecpar->extradata_size;

                    // Register the raw font buffer into libass's internal memory pool
                    ass_add_font(ass_library.get(), font_name.c_str(), font_data, font_data_size);
                    
                    std::cout << "Successfully matched and loaded font: " << font_name 
                            << " (" << font_data_size << " bytes)" << std::endl;
                }
            }
        }
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

    void upload_vertices(SDL_GPUCopyPass *pass, Atlas *atlas) {
        Uint32 size = atlas->vertices.size() * sizeof(Vertex);
        auto vert = vertex_pool.alloc(size);
        auto src = atlas->vertices.data();
        void* v_map = SDL_MapGPUTransferBuffer(device, vert->xfer_buf, false);
        SDL_memcpy(v_map, src, size);
        SDL_UnmapGPUTransferBuffer(device, vert->xfer_buf);

        SDL_GPUTransferBufferLocation v_transfer = { vert->xfer_buf, 0 };
        SDL_GPUBufferRegion v_region = { vert->buf, 0, size };
        SDL_UploadToGPUBuffer(pass, &v_transfer, &v_region, false);
        atlas->vert_buf = vert->buf;
        vertex_pool.in_use(vert);
    }

    void prepare_draw(SDL_GPUCopyPass *pass, double play_time) {
        if (!ass_track)
            return;

        atlas_pool.recycle();
        vertex_pool.recycle();

        int changed = 0;
        // Ask libass to process the track at this specific millisecond frame marker
        ASS_Image* img = ass_render_frame(ass_renderer.get(), ass_track.get(), play_time * 1000, &changed);
        // 3. Draw the active text lines over the frame canvas
        Atlas *atlas = nullptr;
        for (; img; img = img->next) {
//            printf("SUCCESS: libass generated image chunks! w=%d, h=%d at position x=%d, y=%d\n", img->w, img->h, img->dst_x, img->dst_y);
            if (img->w == 0 || img->h == 0)
                continue;

            uint32_t out_x, out_y;
            AtlasRegion out_uv;
            while (true) {
                if (!atlas) {
                    atlas = atlas_pool.alloc(wnd_w, wnd_h / 2);
                }
                if (atlas->alloc_region(img->w, img->h, out_x, out_y, out_uv))
                    break;
                if (atlas->vertices.empty()) {
                    atlas->destroy(device);
                } else {
                    upload_vertices(pass, atlas);
                    atlas_pool.in_use(atlas);
                }
                atlas = nullptr;
            }

            uint8_t* dst = (uint8_t*)SDL_MapGPUTransferBuffer(device, atlas->buf, false);
            dst += atlas->offset;
            const uint8_t* src = img->bitmap;
            for (int y = 0; y < img->h; ++y) {
                SDL_memcpy(dst + (y * img->w), src + (y * img->stride), img->w);
            }
            SDL_UnmapGPUTransferBuffer(device, atlas->buf);

            SDL_GPUTextureTransferInfo transfer_info = {
                .transfer_buffer = atlas->buf,
                .offset = atlas->offset
            };
            SDL_GPUTextureRegion region = {
                .texture = atlas->tex,
                .x = out_x,
                .y = out_y,
                .w = static_cast<Uint32>(img->w),
                .h = static_cast<Uint32>(img->h),
                .d = 1
            };
            SDL_UploadToGPUTexture(pass, &transfer_info, &region, false);
            atlas->offset += img->w * img->h;

            // --- B. Convert Screen Coordinates to Normalized Device Coordinates (NDC) ---
            // libass gives pixel coordinates top-left (0,0) to bottom-right (screen_w, screen_h)
            float x0 = (2.0f * img->dst_x / wnd_w) - 1.0f;
            float y0 = 1.0f - (2.0f * img->dst_y / wnd_h);
            float x1 = (2.0f * (img->dst_x + img->w) / wnd_w) - 1.0f;
            float y1 = 1.0f - (2.0f * (img->dst_y + img->h) / wnd_h);

            uint32_t c = img->color;
            float r = ((c >> 24) & 0xFF) / 255.0f;
            float g = ((c >> 16) & 0xFF) / 255.0f;
            float b = ((c >> 8)  & 0xFF) / 255.0f;
            float a = (255 - (c & 0xFF)) / 255.0f;

            atlas->vertices.push_back({ x0, y0, out_uv.u0, out_uv.v0, r, g, b, a });
            atlas->vertices.push_back({ x1, y0, out_uv.u1, out_uv.v0, r, g, b, a });
            atlas->vertices.push_back({ x0, y1, out_uv.u0, out_uv.v1, r, g, b, a });
//            atlas->vertices.push_back({ x1, y1, out_uv.u1, out_uv.v1, r, g, b, a });

            atlas->vertices.push_back({ x0, y1, out_uv.u0, out_uv.v1, r, g, b, a });
            atlas->vertices.push_back({ x1, y0, out_uv.u1, out_uv.v0, r, g, b, a });
            atlas->vertices.push_back({ x1, y1, out_uv.u1, out_uv.v1, r, g, b, a });
        }
        if (atlas) {
            upload_vertices(pass, atlas);
            atlas_pool.in_use(atlas);
        }
    }

    void draw(SDL_GPUCommandBuffer *cmd, SDL_GPURenderPass *pass) {
        auto list = atlas_pool.get_in_use();
        if (list.empty())
            return;

        SDL_BindGPUGraphicsPipeline(pass, pipeline);   // the one with blending enabled            

        for (auto data : list) {
            SDL_GPUTextureSamplerBinding t_binding = { data->tex, sampler };
            SDL_BindGPUFragmentSamplers(pass, 0, &t_binding, 1);
            SDL_GPUBufferBinding v_binding = { data->vert_buf, 0 };
            SDL_BindGPUVertexBuffers(pass, 0, &v_binding, 1);
            SDL_DrawGPUPrimitives(pass, data->vertices.size(), 1, 0, 0);
        }
    }

    void window_size_changed(Sint32 w, Sint32 h) {
        wnd_w = w;
        wnd_h = h;
        if (ass_renderer)
            ass_set_frame_size(ass_renderer.get(), wnd_w, wnd_h); // Match your window canvas size
    }

private:
    bool init_pipeline(SDL_Window *window) {
        if (pipeline)
            return true;

        // 1. Configure Vertex Attributes matching GLSL layout(...) input locations
        SDL_GPUVertexAttribute attrs[3] = {};

        // Location 0: inPosition (vec2)
        attrs[0].location = 0;
        attrs[0].buffer_slot = 0;
        attrs[0].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
        attrs[0].offset = offsetof(Vertex, x);

        // Location 1: inUV (vec2)
        attrs[1].location = 1;
        attrs[1].buffer_slot = 0;
        attrs[1].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
        attrs[1].offset = offsetof(Vertex, u);

        // Location 2: inColor (vec4)
        attrs[2].location = 2;
        attrs[2].buffer_slot = 0;
        attrs[2].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4;
        attrs[2].offset = offsetof(Vertex, r);

        SDL_GPUVertexBufferDescription buffer_desc = {
            .slot = 0,
            .pitch = sizeof(float) * 8,
        };

		SDL_GPUShaderCreateInfo shader_info = {
            .code_size = ass_vert_len,
            .code = ass_vert,
			.entrypoint = "main",
			.format = SDL_GPU_SHADERFORMAT_SPIRV,
            .stage = SDL_GPU_SHADERSTAGE_VERTEX,
            .num_uniform_buffers = 0
		};
        SDL_GPUShader *vert_shader = SDL_CreateGPUShader(device, &shader_info);
        shader_info.code_size = ass_frag_len,
        shader_info.code = ass_frag;
        shader_info.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
		shader_info.num_samplers = 1;
    	shader_info.num_uniform_buffers = 0;
        SDL_GPUShader *frag_shader = SDL_CreateGPUShader(device, &shader_info);

        SDL_GPUGraphicsPipelineCreateInfo pipeline_info = {
            .vertex_shader = vert_shader,
            .fragment_shader = frag_shader,
            .vertex_input_state = {
                .vertex_buffer_descriptions = &buffer_desc,
                .num_vertex_buffers = 1,
                .vertex_attributes = attrs,
                .num_vertex_attributes = 3,
            },
            .primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST,
        };

        // 2. Enable Standard Alpha Blending
        SDL_GPUColorTargetDescription color_desc = {
            .format = SDL_GetGPUSwapchainTextureFormat(device, window),
            .blend_state = {
                .src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA,
                .dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
                .color_blend_op = SDL_GPU_BLENDOP_ADD,
                .src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE,
                .dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
                .alpha_blend_op = SDL_GPU_BLENDOP_ADD,
                .enable_blend = true,
            }
        };

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
    SDL_GPUDevice *device = nullptr;
    SDL_GPUTexture *glyph_tex = nullptr;
    int wnd_w = 0;
    int wnd_h = 0;
    SDL_GPUSampler *sampler = nullptr;
    AtlasPool atlas_pool;
    VertexPool vertex_pool;
};
