#pragma once

#include <SDL3/SDL.h>

//#include "lanczos-3.frag.h"
#include "common.frag.h"
#include "test.frag.h"

enum ShaderType {
	COMMON,
};

struct CommonUniforms {
    float tex_size[2];      // width, height of Y plane
    float lobes;            // 2.0 or 3.0
    float bit_depth;        // 8.0 or 10.0
    int   is_rgb;           // 1 = RGB path, 0 = YUV
    int   is_full_range;    // 1 = full range, 0 = limited
    int   matrix_id;        // 0 = BT.601, 1 = BT.709, 2 = BT.2020
    int   chroma_offset;    // optional, 0 or 1
};

class GPUPipeline {
private:
	SDL_GPUDevice *device = nullptr;
	SDL_Renderer *renderer = nullptr;
	SDL_GPURenderState *state = nullptr;
	SDL_Texture *texture = nullptr;

	SDL_GPUShader* load_shader(ShaderType type) {
		SDL_GPUShaderCreateInfo shader_info = {
			.entrypoint = "main",
			.format = SDL_GPU_SHADERFORMAT_SPIRV,
			.stage = SDL_GPU_SHADERSTAGE_FRAGMENT,
		};

		switch (type) {
			case COMMON:
				shader_info.num_samplers = 1;
				shader_info.num_uniform_buffers = 1;
				shader_info.code = test_frag;
				shader_info.code_size = test_frag_len;
				break;
		}

		SDL_GPUShader* shader = SDL_CreateGPUShader(device, &shader_info);
		if (shader == nullptr)
		{
			SDL_Log("Failed to create shader!");
			return nullptr;
		}
		return shader;
	}

    void create_texture(AVFrame *frame) {
        if (!texture || texture->w != frame->width || texture->h != frame->height) {
			SDL_DestroyTexture(texture);
            auto sdl_format = ff::VideoScaler::av_to_sdl(static_cast<AVPixelFormat>(frame->format));
            printf("texture format: %x\n", sdl_format);
            texture = SDL_CreateTexture(
                renderer,
                sdl_format,
                SDL_TEXTUREACCESS_STREAMING, 
                frame->width,
                frame->height
            );
        }
    }

	enum SDL_UpdateKind {
		SDL_UPDATE_TEXTURE,   /* SDL_UpdateTexture  (packed / RGB) */
		SDL_UPDATE_YUV,       /* SDL_UpdateYUVTexture (planar YUV) */
		SDL_UPDATE_NV,        /* SDL_UpdateNVTexture  (NV12/NV21/P010) */
		SDL_UPDATE_NONE
	};

	SDL_UpdateKind get_update_kind(SDL_PixelFormat format) {
		switch (format) {
			case SDL_PIXELFORMAT_IYUV:
				return SDL_UPDATE_YUV;

			case SDL_PIXELFORMAT_NV12:
			case SDL_PIXELFORMAT_NV21:
			case SDL_PIXELFORMAT_P010:
				return SDL_UPDATE_NV;

			default:
				return SDL_UPDATE_TEXTURE;
		}
	}

	void push_uniforms(AVFrame *frame)
	{
		CommonUniforms u = {0};

		u.tex_size[0] = (float)frame->width;
		u.tex_size[1] = (float)frame->height;
		u.lobes       = 3;

		// Bit depth
		switch (frame->format) {
			case AV_PIX_FMT_YUV420P10LE:
			case AV_PIX_FMT_P010LE:
			case AV_PIX_FMT_YUV420P10BE:
				u.bit_depth = 10.0f;
				break;
			default:
				u.bit_depth = 8.0f;
				break;
		}

		// RGB or YUV
		u.is_rgb = (frame->format == AV_PIX_FMT_RGBA ||
					frame->format == AV_PIX_FMT_RGB24 ||
					frame->format == AV_PIX_FMT_BGRA) ? 1 : 0;

		// Range (simplified – you can improve this with frame->color_range)
		u.is_full_range = (frame->color_range == AVCOL_RANGE_JPEG) ? 1 : 0;

		// Color matrix
		switch (frame->colorspace) {
			case AVCOL_SPC_BT709:
				u.matrix_id = 1;
				break;
			case AVCOL_SPC_BT2020_NCL:
			case AVCOL_SPC_BT2020_CL:
				u.matrix_id = 2;
				break;
			default:
				u.matrix_id = 0; // BT.601
				break;
		}

		u.chroma_offset = 0; // or detect from frame->chroma_location

		SDL_SetGPURenderStateFragmentUniforms(state, 0, &u, sizeof(u));
	}		

public:
	~GPUPipeline() {
		if (texture)
			SDL_DestroyTexture(texture);
		if (state)
			SDL_DestroyGPURenderState(state);
	}

	bool init(SDL_Renderer *renderer) {
		device = SDL_GetGPURendererDevice(renderer);
		if (!device)
			return false;
		this->renderer = renderer;
		return true;
	}

	bool create_render_state(ShaderType type) {
		if (state) {
			SDL_DestroyGPURenderState(state);
			state = nullptr;
		}

		// Choose format based on what the device supports
		SDL_GPUShaderFormat formats = SDL_GetGPUShaderFormats(device);

		if (formats & SDL_GPU_SHADERFORMAT_SPIRV) {
		} else if (formats & SDL_GPU_SHADERFORMAT_DXIL) {
			// DXIL version...
			return false;
		} else if (formats & SDL_GPU_SHADERFORMAT_MSL) {
			return false;
		}

		SDL_GPUShader* frag_shader = load_shader(type);
		if (!frag_shader) {
			SDL_Log("SDL_CreateGPUShader failed: %s", SDL_GetError());
			return false;
		}

		SDL_GPURenderStateCreateInfo state_info = {0};
		state_info.fragment_shader = frag_shader;

		state = SDL_CreateGPURenderState(renderer, &state_info);
		if (!state) {
			SDL_Log("SDL_CreateGPURenderState failed: %s", SDL_GetError());
			SDL_ReleaseGPUShader(device, state_info.fragment_shader);
			return false;
		}

		return true;
	}

	void set_frame(AVFrame *frame) {
		create_texture(frame);
		switch (get_update_kind(texture->format)) {
			case SDL_UPDATE_NV:
				SDL_UpdateNVTexture(texture, nullptr, frame->data[0], frame->linesize[0], frame->data[1], frame->linesize[1]);
				break;
			case SDL_UPDATE_YUV:
				SDL_UpdateYUVTexture(texture, nullptr, frame->data[0], frame->linesize[0], frame->data[1], frame->linesize[1], frame->data[2], frame->linesize[2]);
				break;
			default:
				SDL_UpdateTexture(texture, nullptr, frame->data[0], frame->linesize[0]);
				break;
		}
		push_uniforms(frame);
	}

	void render() {
		if (!texture)
			return;

		int window_w = 0, window_h = 0;
		float texture_w = 0, texture_h = 0;
		SDL_FRect dst_rect;
		SDL_GetRenderOutputSize(renderer, &window_w, &window_h);
	//        dst_rect.w = state->target_w;
	//        dst_rect.h = state->target_h;
	//        dst_rect.x = (window_w - dst_rect.w) / 2 + state->video_pan_x;
	//        dst_rect.y = (window_h - dst_rect.h) / 2 + state->video_pan_y;
		dst_rect.x = 0;
		dst_rect.y = 0;
		dst_rect.w = window_w;
		dst_rect.h = window_h;

//        SDL_SetTextureScaleMode(state->texture.get(), SDL_SCALEMODE_LINEAR);
        SDL_SetGPURenderState(renderer, state);
        SDL_RenderTexture(renderer, texture, NULL, &dst_rect);
        SDL_SetGPURenderState(renderer, nullptr);
	}
};
