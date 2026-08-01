#pragma once

#include <SDL3/SDL.h>

// #include "lanczos-3.frag.h"
#include "vert.vert.h"
#include "common.frag.h"
#include "test.frag.h"

enum ShaderType
{
	VERT,
	COMMON_FLAG,
};

struct CommonUniforms
{
	float tex_size[2]; // width, height of Y plane
	float lobes;	   // 2.0 or 3.0
	float bit_depth;   // 8.0 or 10.0
	int is_rgb;		   // 1 = RGB path, 0 = YUV
	int is_full_range; // 1 = full range, 0 = limited
	int matrix_id;	   // 0 = BT.601, 1 = BT.709, 2 = BT.2020
	int chroma_offset; // optional, 0 or 1
	int planar;
};

class GPUPipeline
{
private:
	SDL_GPUDevice *device = nullptr;
	SDL_GPUGraphicsPipeline *pipeline = nullptr;
	SDL_GPUSampler *sampler = nullptr;
	SDL_Window *window = nullptr;
	SDL_GPUTexture *yTexture = nullptr;
	SDL_GPUTexture *uTexture = nullptr;
	SDL_GPUTexture *vTexture = nullptr;
	AVPixelFormat pixel_format = AV_PIX_FMT_NONE;
	int width = 0;
	int height = 0;
	AVFrame *frame = nullptr;

	SDL_GPUShader *load_shader(ShaderType type)
	{
		SDL_GPUShaderCreateInfo shader_info = {
			.entrypoint = "main",
			.format = SDL_GPU_SHADERFORMAT_SPIRV,
		};

		switch (type)
		{
		case VERT:
			shader_info.stage = SDL_GPU_SHADERSTAGE_VERTEX;
			shader_info.code = vert_vert;
			shader_info.code_size = vert_vert_len;
			break;

		case COMMON_FLAG:
			shader_info.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
			shader_info.num_samplers = 2;
			shader_info.num_uniform_buffers = 1;
			shader_info.code = test_frag;
			shader_info.code_size = test_frag_len;
			break;
		}

		SDL_GPUShader *shader = SDL_CreateGPUShader(device, &shader_info);
		if (shader == nullptr)
		{
			SDL_Log("Failed to create shader!");
			return nullptr;
		}
		return shader;
	}

	SDL_GPUTexture *create_plane_texture(int w, int h, SDL_GPUTextureFormat format)
	{
		SDL_GPUTextureCreateInfo info = {};
		info.type = SDL_GPU_TEXTURETYPE_2D;
		info.format = format;
		info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
		info.width = static_cast<Uint32>(w);
		info.height = static_cast<Uint32>(h);
		info.layer_count_or_depth = 1;
		info.num_levels = 1;

		return SDL_CreateGPUTexture(device, &info);
	}

	void destroy_textures()
	{
		if (yTexture)
			SDL_ReleaseGPUTexture(device, yTexture);
		if (uTexture)
			SDL_ReleaseGPUTexture(device, uTexture);
		if (vTexture)
			SDL_ReleaseGPUTexture(device, vTexture);
		yTexture = nullptr;
		uTexture = nullptr;
		vTexture = nullptr;
	}

	void create_texture(AVFrame *frame)
	{
		if (frame->format == pixel_format && frame->width == width || frame->height == height)
			return;

		destroy_textures();
		pixel_format = static_cast<AVPixelFormat>(frame->format);
		SDL_Log("pixel_format %i\n", pixel_format);
		width = frame->width;
		height = frame->height;

		switch (pixel_format)
		{
		case AV_PIX_FMT_YUV420P:
			yTexture = create_plane_texture(width, height, SDL_GPU_TEXTUREFORMAT_R8_UNORM);
			uTexture = create_plane_texture(width / 2, height / 2, SDL_GPU_TEXTUREFORMAT_R8_UNORM);
			vTexture = create_plane_texture(width / 2, height / 2, SDL_GPU_TEXTUREFORMAT_R8_UNORM);
			break;

		case AV_PIX_FMT_YUV420P10LE:
			yTexture = create_plane_texture(width, height, SDL_GPU_TEXTUREFORMAT_R16_UNORM);
			uTexture = create_plane_texture(width / 2, height / 2, SDL_GPU_TEXTUREFORMAT_R16_UNORM);
			vTexture = create_plane_texture(width / 2, height / 2, SDL_GPU_TEXTUREFORMAT_R16_UNORM);
			break;

		case AV_PIX_FMT_NV12:
			yTexture = create_plane_texture(width, height, SDL_GPU_TEXTUREFORMAT_R8_UNORM);
			uTexture = create_plane_texture(width / 2, height / 2, SDL_GPU_TEXTUREFORMAT_R8G8_UNORM);
			break;

		case AV_PIX_FMT_P010LE:
			yTexture = create_plane_texture(width, height, SDL_GPU_TEXTUREFORMAT_R16_UNORM);
			uTexture = create_plane_texture(width / 2, height / 2, SDL_GPU_TEXTUREFORMAT_R16G16_UNORM);
			break;
		}
	}

	bool upload_plane(SDL_GPUCommandBuffer *cmd, const uint8_t *data, int linesize, SDL_GPUTexture *texture, Uint32 width, Uint32 height, int bytes_per_pixel)
	{
		Uint32 row_size = width * bytes_per_pixel;
		Uint32 data_size = row_size * height;

		// Create a temporary transfer buffer
		SDL_GPUTransferBufferCreateInfo tb_info = {
			.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
			.size = data_size,
		};
		SDL_GPUTransferBuffer *transfer = SDL_CreateGPUTransferBuffer(device, &tb_info);
		if (!transfer)
			return false;

		// Map and copy
		void *mapped = SDL_MapGPUTransferBuffer(device, transfer, false);
		if (!mapped)
		{
			SDL_ReleaseGPUTransferBuffer(device, transfer);
			return false;
		}

		// Handle possible padding in linesize
		uint8_t *dst = (uint8_t *)mapped;
		const uint8_t *src = data;
		for (int y = 0; y < height; ++y)
		{
			SDL_memcpy(dst, src, row_size);
			dst += row_size;
			src += linesize;
		}

		SDL_UnmapGPUTransferBuffer(device, transfer);

		// Copy to texture
		SDL_GPUCopyPass *copy_pass = SDL_BeginGPUCopyPass(cmd);

		SDL_GPUTextureTransferInfo src_info = {
			.transfer_buffer = transfer,
			.offset = 0,
		};

		SDL_GPUTextureRegion dst_region = {
			.texture = texture,
			.w = width,
			.h = height,
			.d = 1,
		};

		SDL_UploadToGPUTexture(copy_pass, &src_info, &dst_region, false);
		SDL_EndGPUCopyPass(copy_pass);

		// Transfer buffer can be released after submit,
		// but for simplicity we release here (or keep a pool)
		SDL_ReleaseGPUTransferBuffer(device, transfer);

		return true;
	}

	void push_uniforms(
		SDL_GPUCommandBuffer *cmd,
		const AVFrame *frame,
		float lobes)
	{
		CommonUniforms u = {0};

		u.tex_size[0] = (float)frame->width;
		u.tex_size[1] = (float)frame->height;
		u.lobes = lobes;

		// Bit depth
		switch (frame->format)
		{
		case AV_PIX_FMT_YUV420P10LE:
		case AV_PIX_FMT_P010LE:
			u.bit_depth = 10.0f;
			break;
		default:
			u.bit_depth = 8.0f;
			break;
		}

		// RGB or YUV
		u.is_rgb = (frame->format == AV_PIX_FMT_RGBA ||
					frame->format == AV_PIX_FMT_RGB24 ||
					frame->format == AV_PIX_FMT_BGRA)
					   ? 1
					   : 0;

		// Range (simplified – you can improve this with frame->color_range)
		u.is_full_range = (frame->color_range == AVCOL_RANGE_JPEG) ? 1 : 0;

		// Color matrix
		switch (frame->colorspace)
		{
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

		// Bit depth
		switch (frame->format)
		{
		case AV_PIX_FMT_YUV420P10LE:
		case AV_PIX_FMT_YUV420P:
			u.planar = 0;
			break;

		case AV_PIX_FMT_P010LE:
		case AV_PIX_FMT_NV12:
			u.planar = 1;
			break;
		}

		// Push to fragment uniform slot 0
		SDL_PushGPUFragmentUniformData(cmd, 0, &u, sizeof(u));
	}

	bool is_10bit()
	{
		return frame->format == AV_PIX_FMT_YUV420P10LE ||
			   frame->format == AV_PIX_FMT_P010LE;
	}

public:
	~GPUPipeline()
	{
		destroy_textures();
		if (sampler)
			SDL_ReleaseGPUSampler(device, sampler);
		if (pipeline)
			SDL_ReleaseGPUGraphicsPipeline(device, pipeline);
		if (device)
			SDL_DestroyGPUDevice(device);
		if (frame)
			av_frame_free(&frame);
	}

	bool init(SDL_Window *window)
	{
		device = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV, true, nullptr);
		if (!device)
		{
			return false;
		}
		if (!SDL_ClaimWindowForGPUDevice(device, window))
		{
			SDL_Log("ClaimWindow failed: %s", SDL_GetError());
			return false;
		}

		SDL_GPUSamplerCreateInfo samp_info = {
			.min_filter = SDL_GPU_FILTER_LINEAR,
			.mag_filter = SDL_GPU_FILTER_LINEAR,
			.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST,
			.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
			.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
		};
		sampler = SDL_CreateGPUSampler(device, &samp_info);
		this->window = window;
		return true;
	}

	SDL_GPUDevice *get_device()
	{
		return device;
	}

	bool init_pipeline(ShaderType type = COMMON_FLAG)
	{
		if (pipeline)
		{
			SDL_ReleaseGPUGraphicsPipeline(device, pipeline);
			pipeline = nullptr;
		}

		SDL_GPUShader *vs = load_shader(VERT);
		SDL_GPUShader *fs = load_shader(type);

		SDL_GPUGraphicsPipelineCreateInfo pipe_info = {0};
		pipe_info.vertex_shader = vs;
		pipe_info.fragment_shader = fs;
		pipe_info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
		pipe_info.target_info.num_color_targets = 1;
		pipe_info.target_info.color_target_descriptions = (SDL_GPUColorTargetDescription[]){{.format = SDL_GetGPUSwapchainTextureFormat(device, window)}};
		// add blend state, rasterizer, etc. if needed

		pipeline = SDL_CreateGPUGraphicsPipeline(device, &pipe_info);

		SDL_ReleaseGPUShader(device, vs);
		SDL_ReleaseGPUShader(device, fs);
		return true;
	}

	void render(AVFrame *frame)
	{
		SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(device);
		if (!cmd)
			return;

		if (this->frame)
			ff::frame_recycle(this->frame);
		this->frame = frame;

		int bpp = is_10bit() ? 2 : 1;
		create_texture(frame);

		Uint32 n_bindings = 3;
		switch (pixel_format)
		{
		case AV_PIX_FMT_YUV420P:
		case AV_PIX_FMT_YUV420P10LE:
			upload_plane(cmd, frame->data[0], frame->linesize[0], yTexture, width, height, bpp);
			upload_plane(cmd, frame->data[1], frame->linesize[1], uTexture, width / 2, height / 2, bpp);
			upload_plane(cmd, frame->data[2], frame->linesize[2], vTexture, width / 2, height / 2, bpp);
			n_bindings = 3;
			break;

		case AV_PIX_FMT_NV12:
		case AV_PIX_FMT_P010LE:
			upload_plane(cmd, frame->data[0], frame->linesize[0], yTexture, width, height, bpp);
			upload_plane(cmd, frame->data[1], frame->linesize[1], uTexture, width / 2, height / 2, bpp * 2);
			n_bindings = 2;
			break;
		}

		// ----- Get swapchain texture -----
		SDL_GPUTexture *swapchain_tex = NULL;
		Uint32 w, h;
		if (SDL_WaitAndAcquireGPUSwapchainTexture(cmd, window, &swapchain_tex, &w, &h))
		{
			SDL_GPUColorTargetInfo color_target = {
				.texture = swapchain_tex,
				.clear_color = {0.0f, 0.0f, 0.0f, 1.0f},
				.load_op = SDL_GPU_LOADOP_CLEAR,
				.store_op = SDL_GPU_STOREOP_STORE,
			};

			SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(cmd, &color_target, 1, nullptr);

			SDL_BindGPUGraphicsPipeline(pass, pipeline);

			// Bind textures + sampler
			SDL_GPUTextureSamplerBinding bindings[3] = {
				{.texture = yTexture, .sampler = sampler},
				{.texture = uTexture, .sampler = sampler},
				{.texture = vTexture, .sampler = sampler},
			};
			SDL_BindGPUFragmentSamplers(pass, 0, bindings, n_bindings);

			push_uniforms(cmd, frame, 3.0f);

			// Draw fullscreen triangle (3 vertices) or quad
			SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);

			SDL_EndGPURenderPass(pass);
		}

		if (!SDL_SubmitGPUCommandBuffer(cmd))
		{
			SDL_Log("SDL_SubmitGPUCommandBuffer failed: %s", SDL_GetError());
		}
	}
};
