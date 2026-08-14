#pragma once

#include <list>

#include "subtitle.hpp"
#include "bitmap.vert.h"
#include "bitmap.frag.h"

struct alignas(16) BitmapForm {
    float position[2]; // x, y (in NDC: -1.0 to 1.0)
    float size[2];     // width, height (in NDC: 0.0 to 2.0)
    float uv_size[2];
};

struct BitmapData {
    SDL_GPUTexture *tex;
    SDL_GPUTexture *tex_pal;
    Uint32 w;
    Uint32 h;
    SDL_GPUTransferBuffer *buf;

    BitmapForm form;

    void reset() {
    }

    void destroy(SDL_GPUDevice* gpu) {
        if (tex)
            SDL_ReleaseGPUTexture(gpu, tex);
        if (tex_pal)
            SDL_ReleaseGPUTexture(gpu, tex_pal);
        if (buf)
            SDL_ReleaseGPUTransferBuffer(gpu, buf);
        delete this;
    }    
};

class BitmapPool : public GPUPool<BitmapData> {
public:
    BitmapData *alloc(Uint32 w, Uint32 h) {
        while (!list.empty()) {
            auto data = list.back();
            list.pop_back();
            if (data->w >= w && data->h >= h)
                return data;
            data->destroy(device);
        }

        w += 64;
        h += 32;

        SDL_GPUTextureCreateInfo tex_info = {
            .type = SDL_GPU_TEXTURETYPE_2D,
            .format = SDL_GPU_TEXTUREFORMAT_R8_UNORM,
            .usage = SDL_GPU_TEXTUREUSAGE_SAMPLER,
            .width = w,
            .height = h,
            .layer_count_or_depth = 1,
            .num_levels = 1
        };
        auto tex = SDL_CreateGPUTexture(device, &tex_info);
        if (!tex)
            return nullptr;

        tex_info = {
            .type = SDL_GPU_TEXTURETYPE_2D,
            .format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
            .usage = SDL_GPU_TEXTUREUSAGE_SAMPLER,
            .width = 256,
            .height = 1,
            .layer_count_or_depth = 1,
            .num_levels = 1
        };
        auto tex_pal = SDL_CreateGPUTexture(device, &tex_info);
        if (!tex_pal) {
            SDL_ReleaseGPUTexture(device, tex);
            return nullptr;
        }

        SDL_GPUTransferBufferCreateInfo tb_info = {
			.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
			.size = w * h + 256 * 4,
		};
		auto buf = SDL_CreateGPUTransferBuffer(device, &tb_info);
        if (!buf) {
            SDL_ReleaseGPUTexture(device, tex);
            SDL_ReleaseGPUTexture(device, tex_pal);
            return nullptr;
        }
 
        return new BitmapData {
            .tex = tex,
            .tex_pal = tex_pal,
            .w = w,
            .h = h,
            .buf = buf,
        };
    }
};

class SubBitmap : public AppSubtitle {
private:
    BitmapPool tex_pool;
    std::list<ff::AVSubtitle_ *> sub_list;

    bool init_pipeline(SDL_Window *window) {
        if (pipeline)
            return true;

		SDL_GPUShaderCreateInfo shader_info = {
            .code_size = bitmap_vert_len,
            .code = bitmap_vert,
			.entrypoint = "main",
			.format = SDL_GPU_SHADERFORMAT_SPIRV,
            .stage = SDL_GPU_SHADERSTAGE_VERTEX,
            .num_uniform_buffers = 1,
		};
        SDL_GPUShader *vert_shader = SDL_CreateGPUShader(device, &shader_info);
        shader_info.code_size = bitmap_frag_len,
        shader_info.code = bitmap_frag;
        shader_info.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
		shader_info.num_samplers = 2;
    	shader_info.num_uniform_buffers = 0;
        SDL_GPUShader *frag_shader = SDL_CreateGPUShader(device, &shader_info);

        auto color_desc = SDL_GPUColorTargetDescription{
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
        SDL_GPUGraphicsPipelineCreateInfo pipeline_info = {
            .vertex_shader = vert_shader,
            .fragment_shader = frag_shader,
            .primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLESTRIP,
            .target_info = {
                .color_target_descriptions = &color_desc,
                .num_color_targets = 1,
            },
        };

        pipeline = SDL_CreateGPUGraphicsPipeline(device, &pipeline_info);
        SDL_ReleaseGPUShader(device, vert_shader);
		SDL_ReleaseGPUShader(device, frag_shader);
        return pipeline != nullptr;
    }

public:
    SubBitmap(SDL_GPUDevice *gpu) : AppSubtitle(gpu) {
        init_once(gpu);
    }
    ~SubBitmap() {
        shutdown();
    }

    void shutdown() {
        SDL_ReleaseGPUGraphicsPipeline(device, pipeline);
        pipeline = nullptr;
        SDL_ReleaseGPUSampler(device, sampler);
        sampler = nullptr;
        tex_pool.clear();
        flush();
    }

    void init_once(SDL_GPUDevice *gpu) {
        device = gpu;
        tex_pool.init(device);

		SDL_GPUSamplerCreateInfo samp_info = {
			.min_filter = SDL_GPU_FILTER_NEAREST,
			.mag_filter = SDL_GPU_FILTER_NEAREST,
			.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST,
			.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
			.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
		};
		sampler = SDL_CreateGPUSampler(device, &samp_info);
    }

    bool init(SDL_Window *window) {
        flush();
        SDL_GetWindowSizeInPixels(window, &wnd_w, &wnd_h);

        return init_pipeline(window);
    }

    void flush() {
        for (auto sub : sub_list) {
            ff::subtitle_recycle(sub);
        }
        sub_list.clear();
    }

    void add_sub(ff::AVSubtitle_ *sub) {
        sub_list.push_back(sub);
    }

    void prepare_draw(SDL_GPUCopyPass *pass, double play_time) {
        tex_pool.recycle();

        for (auto it = sub_list.begin(); it != sub_list.end(); ) {
            if ((*it)->frame_time > play_time)
                break;
            while (true) {
                auto next = std::next(it);
                if (next == sub_list.end() || (*next)->frame_time > play_time)
                    break;
                ff::subtitle_recycle((*it));
                it = sub_list.erase(it);
            }
            auto sub = (*it);
            if (sub->frame_time + sub->duration < play_time) {
                ff::subtitle_recycle(sub);
                it = sub_list.erase(it);
                break;
            }

            for (auto i = 0; i < sub->num_rects; i++) {
                auto rect = sub->rects[i];
                if (rect->type != SUBTITLE_BITMAP)
                    continue;
                if (rect->w == 0 || rect->h == 0)
                    continue;

                auto tex = tex_pool.alloc(rect->w, rect->h);
                uint8_t* dst = (uint8_t*)SDL_MapGPUTransferBuffer(device, tex->buf, false);
                Uint32 pal_offset = rect->nb_colors * 4;
                SDL_memcpy(dst, rect->data[1], pal_offset);
                dst += pal_offset;

                const uint8_t* src = rect->data[0];
                for (int y = 0; y < rect->h; ++y) {
                    SDL_memcpy(dst + (y * rect->w), src + (y * rect->linesize[0]), rect->w);
                }
                SDL_UnmapGPUTransferBuffer(device, tex->buf);

                SDL_GPUTextureTransferInfo transfer_info = {
                    .transfer_buffer = tex->buf,
                    .offset = pal_offset,
                };
                SDL_GPUTextureRegion region = {
                    .texture = tex->tex,
                    .w = (Uint32) rect->w,
                    .h = (Uint32) rect->h,
                    .d = 1
                };
                SDL_UploadToGPUTexture(pass, &transfer_info, &region, false);
                transfer_info.transfer_buffer = tex->buf;
                transfer_info.offset = 0;
                region.texture = tex->tex_pal;
                region.w = rect->nb_colors;
                region.h = 1;
                SDL_UploadToGPUTexture(pass, &transfer_info, &region, false);

                tex->form = {
                    2.0f * rect->x / wnd_w - 1.0f, 1.0f - 2.0f * rect->y / wnd_h,
                    2.0f * rect->w / wnd_w, 2.0f * rect->h / wnd_h,
                    (float) rect->w / tex->w, (float) rect->h / tex->h
                };
                tex_pool.in_use(tex);
            }
            break;
        }
    }

    void draw(SDL_GPUCommandBuffer *cmd, SDL_GPURenderPass *pass) {
        auto list = tex_pool.get_in_use();
        if (list.empty())
            return;

        SDL_BindGPUGraphicsPipeline(pass, pipeline);   // the one with blending enabled            

        for (auto data : list) {
            SDL_PushGPUVertexUniformData(cmd, 0, &data->form, sizeof(data->form));
            SDL_GPUTextureSamplerBinding t_binding[2] = {
                { data->tex, sampler },
                { data->tex_pal, sampler },
            };
            SDL_BindGPUFragmentSamplers(pass, 0, t_binding, 2);
            SDL_DrawGPUPrimitives(pass, 4, 1, 0, 0);
        }
    }
};
