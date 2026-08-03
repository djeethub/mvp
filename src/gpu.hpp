#pragma once

#include <SDL3/SDL.h>
#include "imgui_impl_sdlgpu3.h"

// #include "lanczos-3.frag.h"
#include "vert.vert.h"
#include "nv12.frag.h"
#include "rgb.frag.h"
#include "yuv.frag.h"

extern AssHandler ass;

enum ShaderType
{
	VERT,
	RGB_FRAG,
	NV12_FRAG,
	YUV_FRAG
};

struct alignas(16) Uniforms
{
	float tex_size[2]; // width, height of Y plane
	int32_t color_range; // 1 = full range, 0 = limited
	int32_t colorspace;	 // 1 = 709, 9,10 = 2090
};

class XferQueue {
public:
	~XferQueue() {
		clear();
	}

	struct Data {
		SDL_GPUTransferBuffer *buf;
		Uint32 size;
	};

	void init(SDL_GPUDevice *device) {
		this->device = device;
	}

	Data *alloc(Uint32 size) {
		if (!queue.empty()) {
			auto data = queue.front();
			if (data->size >= size) {
				queue.pop();
				return data;
			}
		}

		SDL_GPUTransferBufferCreateInfo tb_info = {
			.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
			.size = size,
		};
		SDL_GPUTransferBuffer *buf = SDL_CreateGPUTransferBuffer(device, &tb_info);
		if (!buf)
			return nullptr;
		auto data = new Data{.buf = buf, .size = size};
		return data;
	}

	void recycle(Data *data) {
		queue.push(data);
	}

	void recycle() {
		while (!in_use_queue.empty()) {
			queue.push(in_use_queue.front());
			in_use_queue.pop();
		}
	}

	void in_use(Data *data) {
		in_use_queue.push(data);
	}

	void clear() {
		recycle();
		while (!queue.empty()) {
			auto d = queue.front();
			SDL_ReleaseGPUTransferBuffer(device, d->buf);
			delete d;
			queue.pop();
		}
	}

private:
	std::queue<Data *> queue;
	std::queue<Data *> in_use_queue;
	SDL_GPUDevice *device;
};

class AppGpu
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
	int n_bindings = 0;
	XferQueue transfer_queue;
	SwsContext *sws_ctx = nullptr;

	bool setup_sws_context(AVPixelFormat src_fmt, AVPixelFormat dst_fmt) {
		sws_free_context(&sws_ctx);
		sws_ctx = sws_getContext(
			width, height, src_fmt,       // Source video specs
			width, height, dst_fmt,        // Destination specs (GPU friendly)
			SWS_BILINEAR,                          // Fast filter (since size is identical)
			NULL, NULL, NULL
		);
		return sws_ctx != nullptr;
	}

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

		case NV12_FRAG:
			shader_info.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
			shader_info.num_samplers = 2;
			shader_info.num_uniform_buffers = 1;
			shader_info.code = nv12_frag;
			shader_info.code_size = nv12_frag_len;
			break;

		case RGB_FRAG:
			shader_info.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
			shader_info.num_samplers = 1;
			shader_info.num_uniform_buffers = 1;
			shader_info.code = rgb_frag;
			shader_info.code_size = rgb_frag_len;
			break;

		case YUV_FRAG:
			shader_info.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
			shader_info.num_samplers = 3;
			shader_info.num_uniform_buffers = 1;
			shader_info.code = yuv_frag;
			shader_info.code_size = yuv_frag_len;
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

	bool upload_plane(SDL_GPUCopyPass *pass, const uint8_t *data, int linesize, SDL_GPUTexture *texture, Uint32 width, Uint32 height, int bytes_per_pixel)
	{
		Uint32 row_size = width * bytes_per_pixel;
		Uint32 data_size = row_size * height;

		auto buf_data = transfer_queue.alloc(data_size);
		if (!buf_data)
			return false;

		// Map and copy
		void *mapped = SDL_MapGPUTransferBuffer(device, buf_data->buf, false);
		if (!mapped)
		{
			transfer_queue.recycle(buf_data);
			return false;
		}

		if (pixel_format == AV_PIX_FMT_RGB24) {
			uint8_t* dst_data[4] = { static_cast<uint8_t *>(mapped), NULL, NULL, NULL };
			int dst_linesize[4]  = { static_cast<int>(width) * 4, 0, 0, 0 };
			sws_scale(sws_ctx, &data, &linesize, 0, height, dst_data, dst_linesize);
		} else {
			// Handle possible padding in linesize
			uint8_t *dst = (uint8_t *)mapped;
			const uint8_t *src = data;
			for (int y = 0; y < height; ++y)
			{
				SDL_memcpy(dst, src, row_size);
				dst += row_size;
				src += linesize;
			}
		}

		SDL_UnmapGPUTransferBuffer(device, buf_data->buf);

		SDL_GPUTextureTransferInfo src_info = {
			.transfer_buffer = buf_data->buf,
			.offset = 0,
		};

		SDL_GPUTextureRegion dst_region = {
			.texture = texture,
			.w = width,
			.h = height,
			.d = 1,
		};

		SDL_UploadToGPUTexture(pass, &src_info, &dst_region, false);

		transfer_queue.in_use(buf_data);

		return true;
	}

	void push_uniforms(SDL_GPUCommandBuffer *cmd, const AVFrame *frame)
	{
		Uniforms u = {0};

		u.tex_size[0] = (float)frame->width;
		u.tex_size[1] = (float)frame->height;

		// Range (simplified – you can improve this with frame->color_range)
		u.color_range = frame->color_range == AVCOL_RANGE_UNSPECIFIED ? AVCOL_RANGE_MPEG : frame->color_range;
		// Color matrix
		u.colorspace = frame->colorspace == AVCOL_SPC_UNSPECIFIED ? AVCOL_SPC_BT709 : frame->colorspace;

		// Push to fragment uniform slot 0
		SDL_PushGPUFragmentUniformData(cmd, 0, &u, sizeof(u));
	}

	void create_texture(AVFrame *frame)
	{
		if (frame->format == pixel_format && frame->width == width && frame->height == height)
			return;

		destroy_textures();
		pixel_format = static_cast<AVPixelFormat>(frame->format);
		init_pipeline(pixel_format);
		SDL_Log("pixel_format %i\n", pixel_format);
		width = frame->width;
		height = frame->height;

		switch (pixel_format)
		{
		case AV_PIX_FMT_YUV420P:
		case AV_PIX_FMT_YUVJ420P:
		case AV_PIX_FMT_YUVJ422P:
		case AV_PIX_FMT_YUVJ444P:
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

		case AV_PIX_FMT_RGBA:
		case AV_PIX_FMT_RGB24:
			yTexture = create_plane_texture(width, height, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM);
			break;

		case AV_PIX_FMT_BGRA:
		case AV_PIX_FMT_BGR24:
			yTexture = create_plane_texture(width, height, SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM);
			break;
		}

		if (pixel_format == AV_PIX_FMT_RGB24) {
			setup_sws_context(pixel_format, AV_PIX_FMT_RGBA);
		}
	}

	void prepare_texture_draw(SDL_GPUCopyPass *pass) {
		if (!frame)
			return;
		transfer_queue.recycle();
		
		int bpp = 1;
		switch (frame->format) {
			case AV_PIX_FMT_P010LE:
			case AV_PIX_FMT_YUV420P10LE:
				bpp = 2;
				break;
			case AV_PIX_FMT_RGBA:
			case AV_PIX_FMT_BGRA:
			case AV_PIX_FMT_RGB24:
			case AV_PIX_FMT_BGR24:
				bpp = 4;
				break;
		}

		create_texture(frame);

		switch (n_bindings)
		{
		case 3:
			upload_plane(pass, frame->data[0], frame->linesize[0], yTexture, width, height, bpp);
			upload_plane(pass, frame->data[1], frame->linesize[1], uTexture, width / 2, height / 2, bpp);
			upload_plane(pass, frame->data[2], frame->linesize[2], vTexture, width / 2, height / 2, bpp);
			break;

		case 2:
			upload_plane(pass, frame->data[0], frame->linesize[0], yTexture, width, height, bpp);
			upload_plane(pass, frame->data[1], frame->linesize[1], uTexture, width / 2, height / 2, bpp * 2);
			break;

		default:
			upload_plane(pass, frame->data[0], frame->linesize[0], yTexture, width, height, bpp);
		}
	}

	void draw_texture(SDL_GPUCommandBuffer *cmd, SDL_GPURenderPass *pass) {
		if (!frame || !pipeline)
			return;

		SDL_BindGPUGraphicsPipeline(pass, pipeline);

		// Bind textures + sampler
		SDL_GPUTextureSamplerBinding bindings[3] = {
			{.texture = yTexture, .sampler = sampler},
			{.texture = uTexture, .sampler = sampler},
			{.texture = vTexture, .sampler = sampler},
		};
		SDL_BindGPUFragmentSamplers(pass, 0, bindings, n_bindings);

		push_uniforms(cmd, frame);

		// Draw fullscreen triangle (3 vertices) or quad
		SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
	}

public:
	~AppGpu()
	{
		shutdown();
	}

	void shutdown() {
		if (!device)
			return;
		transfer_queue.clear();
		destroy_textures();
		if (sampler)
			SDL_ReleaseGPUSampler(device, sampler);
		sampler = nullptr;
		if (pipeline)
			SDL_ReleaseGPUGraphicsPipeline(device, pipeline);
		pipeline = nullptr;
		if (device)
			SDL_DestroyGPUDevice(device);
		device = nullptr;
		av_frame_free(&frame);
		sws_free_context(&sws_ctx);
	}

	bool init(SDL_Window *window)
	{
#ifdef NDEBUG
		device = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV, false, nullptr);
#else
		device = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV, true, nullptr);
#endif
		if (!device)
			return false;
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
		transfer_queue.init(device);
		return true;
	}

	SDL_GPUDevice *get_device()
	{
		return device;
	}

	SDL_GPUSampler *get_sampler() {
		return sampler;
	}

	void reset_pipeline() {
		SDL_ReleaseGPUGraphicsPipeline(device, pipeline);
		pipeline = nullptr;
	}

	bool init_pipeline(AVPixelFormat fmt)
	{
		reset_pipeline();
		ShaderType frag;
		switch (fmt)
		{
		case AV_PIX_FMT_YUV420P:
		case AV_PIX_FMT_YUV420P10LE:
		case AV_PIX_FMT_YUVJ420P:
		case AV_PIX_FMT_YUVJ422P:
		case AV_PIX_FMT_YUVJ444P:
			n_bindings = 3;
			frag = YUV_FRAG;
			break;

		case AV_PIX_FMT_NV12:
		case AV_PIX_FMT_P010LE:
			n_bindings = 2;
			frag = NV12_FRAG;
			break;

		default:
			n_bindings = 1;
			frag = RGB_FRAG;
			break;
		}
		SDL_GPUShader *vs = load_shader(VERT);
		SDL_GPUShader *fs = load_shader(frag);

		SDL_GPUColorTargetDescription color_target = {
			.format = SDL_GetGPUSwapchainTextureFormat(device, window)
		};
		SDL_GPUGraphicsPipelineCreateInfo pipe_info = {
			.vertex_shader = vs,
			.fragment_shader = fs,
			.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST,
			.target_info = {
				.color_target_descriptions = &color_target,
				.num_color_targets = 1,
			},
		};
		// add blend state, rasterizer, etc. if needed
		pipeline = SDL_CreateGPUGraphicsPipeline(device, &pipe_info);

		SDL_ReleaseGPUShader(device, vs);
		SDL_ReleaseGPUShader(device, fs);
		transfer_queue.clear();
		return true;
	}

	void set_frame(AVFrame *frame, double play_time) {
		if (this->frame)
			ff::frame_recycle(this->frame);
		this->frame = frame;

		SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(device);
		SDL_GPUCopyPass *copy_pass = SDL_BeginGPUCopyPass(cmd);

		prepare_texture_draw(copy_pass);
		ass.prepare_draw(copy_pass, play_time);
		
		SDL_EndGPUCopyPass(copy_pass);
		if (!SDL_SubmitGPUCommandBuffer(cmd))
		{
			SDL_Log("SDL_SubmitGPUCommandBuffer failed: %s", SDL_GetError());
		}
	}

	void render()
	{
		SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(device);
		if (!cmd)
			return;

		// MANDATORY: Upload ImGui vertex/index buffers prior to the render pass
		ImDrawData* draw_data = ImGui::GetDrawData();
		ImGui_ImplSDLGPU3_PrepareDrawData(draw_data, cmd);

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

			draw_texture(cmd, pass);
			ass.draw(cmd, pass);
			ImGui_ImplSDLGPU3_RenderDrawData(draw_data, cmd, pass);

			SDL_EndGPURenderPass(pass);
		}

		if (!SDL_SubmitGPUCommandBuffer(cmd))
		{
			SDL_Log("SDL_SubmitGPUCommandBuffer failed: %s", SDL_GetError());
		}
	}
};
