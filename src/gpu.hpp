#pragma once

#include <SDL3/SDL.h>
#include <imgui_impl_sdlgpu3.h>

#include "subtitle.hpp"

#include "vert.vert.h"
#include "nv12.frag.h"
#include "rgb.frag.h"
#include "yuv.frag.h"
#include "gray.frag.h"
#include "pal8.frag.h"
#include "yuv_10.frag.h"

enum ShaderType
{
	VERT,
	RGB_FRAG,
	NV12_FRAG,
	YUV_FRAG,
	GRAY_FRAG,
	PAL8_FRAG,
	YUV_10_FRAG
};

struct alignas(16) Vertform {
    float position[2]; // x, y (in NDC: -1.0 to 1.0)
    float size[2];     // width, height (in NDC: 0.0 to 2.0)
};

struct alignas(16) Uniforms
{
	float tex_size[2]; // width, height of Y plane
	int32_t color_range; // 1 = full range, 0 = limited
	int32_t colorspace;	 // 1 = 709, 9,10 = 2090
};

struct XferData {
	SDL_GPUTransferBuffer *buf;
	Uint32 size;

	void destroy(SDL_GPUDevice *device) {
		if (buf)
			SDL_ReleaseGPUTransferBuffer(device, buf);
		delete this;
	}

	void reset() {}
};

class XferPool : public GPUPool<XferData> {
public:
	XferData *alloc(Uint32 size) {
		if (!list.empty()) {
			auto data = list.back();
			list.pop_back();
			if (data->size >= size) {
				return data;
			}
			data->destroy(device);
		}

		SDL_GPUTransferBufferCreateInfo tb_info = {
			.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
			.size = size,
		};
		SDL_GPUTransferBuffer *buf = SDL_CreateGPUTransferBuffer(device, &tb_info);
		if (!buf)
			return nullptr;
		auto data = new XferData{.buf = buf, .size = size};
		return data;
	}
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
	int de_width = 2;
	int de_height = 2;
	int wnd_w = 0;
	int wnd_h = 0;
	float base_scale = 0.0;
	AVFrame *frame = nullptr;
	int n_bindings = 0;
	XferPool xfer_pool;
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
			shader_info.num_uniform_buffers = 1;
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

		case YUV_10_FRAG:
			shader_info.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
			shader_info.num_samplers = 3;
			shader_info.num_uniform_buffers = 1;
			shader_info.code = yuv_10_frag;
			shader_info.code_size = yuv_10_frag_len;
			break;

		case GRAY_FRAG:
			shader_info.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
			shader_info.num_samplers = 1;
			shader_info.num_uniform_buffers = 1;
			shader_info.code = gray_frag;
			shader_info.code_size = gray_frag_len;
			break;

		case PAL8_FRAG:
			shader_info.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
			shader_info.num_samplers = 2;
			shader_info.num_uniform_buffers = 1;
			shader_info.code = pal8_frag;
			shader_info.code_size = pal8_frag_len;
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

		auto buf_data = xfer_pool.alloc(data_size);
		if (!buf_data)
			return false;

		// Map and copy
		void *mapped = SDL_MapGPUTransferBuffer(device, buf_data->buf, true);
		if (!mapped)
		{
			xfer_pool.recycle(buf_data);
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

		SDL_UploadToGPUTexture(pass, &src_info, &dst_region, true);

		xfer_pool.in_use(buf_data);

		return true;
	}

	void push_uniforms(SDL_GPUCommandBuffer *cmd, const AVFrame *frame)
	{
		auto scale = base_scale * video_scale;
		float w = 2.0f * scale * frame->width / wnd_w;
		float h = 2.0f * scale * frame->height / wnd_h;
		Vertform tf = {
			.position = { video_pan_x / wnd_w, video_pan_y / wnd_h },
			.size = { w, h }
		};
		SDL_PushGPUVertexUniformData(cmd, 0, &tf, sizeof(tf));

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
		SDL_Log("out_pix_fmt: %s\n", av_get_pix_fmt_name(pixel_format));
		init_pipeline(pixel_format);
		width = frame->width;
		height = frame->height;
		reset_scale();

		switch (pixel_format)
		{
		case AV_PIX_FMT_YUV420P:
		case AV_PIX_FMT_YUVJ420P:
		case AV_PIX_FMT_YUVJ422P:
		case AV_PIX_FMT_YUVJ444P:
			yTexture = create_plane_texture(width, height, SDL_GPU_TEXTUREFORMAT_R8_UNORM);
			uTexture = create_plane_texture(width / de_width, height / de_height, SDL_GPU_TEXTUREFORMAT_R8_UNORM);
			vTexture = create_plane_texture(width / de_width, height / de_height, SDL_GPU_TEXTUREFORMAT_R8_UNORM);
			break;

		case AV_PIX_FMT_YUV420P10LE:
		case AV_PIX_FMT_YUV422P10LE:
		case AV_PIX_FMT_YUV444P10LE:
			yTexture = create_plane_texture(width, height, SDL_GPU_TEXTUREFORMAT_R16_UNORM);
			uTexture = create_plane_texture(width / de_width, height / de_height, SDL_GPU_TEXTUREFORMAT_R16_UNORM);
			vTexture = create_plane_texture(width / de_width, height / de_height, SDL_GPU_TEXTUREFORMAT_R16_UNORM);
			break;

		case AV_PIX_FMT_NV12:
			yTexture = create_plane_texture(width, height, SDL_GPU_TEXTUREFORMAT_R8_UNORM);
			uTexture = create_plane_texture(width / de_width, height / de_height, SDL_GPU_TEXTUREFORMAT_R8G8_UNORM);
			break;

		case AV_PIX_FMT_P010LE:
			yTexture = create_plane_texture(width, height, SDL_GPU_TEXTUREFORMAT_R16_UNORM);
			uTexture = create_plane_texture(width / de_width, height / de_height, SDL_GPU_TEXTUREFORMAT_R16G16_UNORM);
			break;

		case AV_PIX_FMT_BGRA:
		case AV_PIX_FMT_BGR24:
			yTexture = create_plane_texture(width, height, SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM);
			break;

		case AV_PIX_FMT_GRAY8:
			yTexture = create_plane_texture(width, height, SDL_GPU_TEXTUREFORMAT_R8_UNORM);
			break;

		case AV_PIX_FMT_PAL8:
			yTexture = create_plane_texture(width, height, SDL_GPU_TEXTUREFORMAT_R8_UNORM);
			uTexture = create_plane_texture(256, 1, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM);
			break;

		case AV_PIX_FMT_RGBA:
		case AV_PIX_FMT_RGB24:
		default:
			yTexture = create_plane_texture(width, height, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM);
			break;
		}

		if (pixel_format == AV_PIX_FMT_RGB24) {
			setup_sws_context(pixel_format, AV_PIX_FMT_RGBA);
		}
	}

	void prepare_texture_draw(SDL_GPUCopyPass *pass) {
		if (!frame)
			return;
		xfer_pool.recycle();
		
		int bpp;
		switch (frame->format) {
			case AV_PIX_FMT_P010LE:
			case AV_PIX_FMT_YUV420P10LE:
			case AV_PIX_FMT_YUV422P10LE:
			case AV_PIX_FMT_YUV444P10LE:
				bpp = 2;
				break;
			case AV_PIX_FMT_RGBA:
			case AV_PIX_FMT_BGRA:
			case AV_PIX_FMT_RGB24:
			case AV_PIX_FMT_BGR24:
				bpp = 4;
				break;
			default:
				bpp = 1;
		}

		create_texture(frame);

		switch (n_bindings)
		{
		case 3:
			upload_plane(pass, frame->data[0], frame->linesize[0], yTexture, width, height, bpp);
			upload_plane(pass, frame->data[1], frame->linesize[1], uTexture, width / de_width, height / de_height, bpp);
			upload_plane(pass, frame->data[2], frame->linesize[2], vTexture, width / de_width, height / de_height, bpp);
			break;

		case 2:
			upload_plane(pass, frame->data[0], frame->linesize[0], yTexture, width, height, bpp);
			if (frame->format == AV_PIX_FMT_PAL8)
				upload_plane(pass, frame->data[1], frame->linesize[1], uTexture, 256, 1, 4);
			else
				upload_plane(pass, frame->data[1], frame->linesize[1], uTexture, width / de_width, height / de_height, bpp * 2);
			break;

		default:
			upload_plane(pass, frame->data[0], frame->linesize[0], yTexture, width, height, bpp);
		}
	}

	void draw_texture(SDL_GPUCommandBuffer *cmd, SDL_GPURenderPass *pass) {
		if (!pipeline)
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

		SDL_DrawGPUPrimitives(pass, 4, 1, 0, 0);
	}

public:
    float video_scale = 1.0;
    float video_pan_x = 0.0;
    float video_pan_y = 0.0;

	~AppGpu()
	{
		shutdown();
	}

	void shutdown() {
		if (!device)
			return;
		xfer_pool.clear();
		destroy_textures();
		SDL_ReleaseGPUSampler(device, sampler);
		sampler = nullptr;
		SDL_ReleaseGPUGraphicsPipeline(device, pipeline);
		pipeline = nullptr;
		SDL_ReleaseWindowFromGPUDevice(device, window);
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

		this->window = window;
		xfer_pool.init(device);
		return true;
	}

	SDL_GPUDevice *get_device()
	{
		return device;
	}

	bool init_pipeline(AVPixelFormat fmt)
	{
		switch (fmt) {
			case AV_PIX_FMT_YUVJ444P:
			case AV_PIX_FMT_YUV444P10LE:
				de_width = 1;
				de_height = 1;
				break;
			case AV_PIX_FMT_YUVJ422P:
			case AV_PIX_FMT_YUV422P10LE:
				de_width = 2;
				de_height = 1;
				break;
			default:
				de_width = 2;
				de_height = 2;
		}
		ShaderType frag;
		switch (fmt)
		{
		case AV_PIX_FMT_YUV420P:
		case AV_PIX_FMT_YUVJ420P:
		case AV_PIX_FMT_YUVJ422P:
		case AV_PIX_FMT_YUVJ444P:
			n_bindings = 3;
			frag = YUV_FRAG;
			break;

		case AV_PIX_FMT_YUV420P10LE:
		case AV_PIX_FMT_YUV422P10LE:
		case AV_PIX_FMT_YUV444P10LE:
			n_bindings = 3;
			frag = YUV_10_FRAG;
			break;

		case AV_PIX_FMT_NV12:
		case AV_PIX_FMT_P010LE:
			n_bindings = 2;
			frag = NV12_FRAG;
			break;

		case AV_PIX_FMT_GRAY8:
			n_bindings = 1;
			frag = GRAY_FRAG;
			break;

		case AV_PIX_FMT_PAL8:
			n_bindings = 2;
			frag = PAL8_FRAG;
			break;

		default:
			n_bindings = 1;
			frag = RGB_FRAG;
			break;
		}

		SDL_ReleaseGPUGraphicsPipeline(device, pipeline);
		SDL_GPUShader *vs = load_shader(VERT);
		SDL_GPUShader *fs = load_shader(frag);

		SDL_GPUColorTargetDescription color_target = {
			.format = SDL_GetGPUSwapchainTextureFormat(device, window)
		};
		SDL_GPUGraphicsPipelineCreateInfo pipe_info = {
			.vertex_shader = vs,
			.fragment_shader = fs,
			.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLESTRIP,
			.target_info = {
				.color_target_descriptions = &color_target,
				.num_color_targets = 1,
			},
		};
		// add blend state, rasterizer, etc. if needed
		pipeline = SDL_CreateGPUGraphicsPipeline(device, &pipe_info);
		SDL_ReleaseGPUShader(device, vs);
		SDL_ReleaseGPUShader(device, fs);

		SDL_ReleaseGPUSampler(device, sampler);
		SDL_GPUSamplerCreateInfo samp_info = {
			.min_filter = SDL_GPU_FILTER_LINEAR,
			.mag_filter = SDL_GPU_FILTER_LINEAR,
			.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST,
			.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
			.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
		};
		if (fmt == AV_PIX_FMT_PAL8) {
			samp_info.min_filter = SDL_GPU_FILTER_NEAREST;
			samp_info.mag_filter = SDL_GPU_FILTER_NEAREST;
		}
		sampler = SDL_CreateGPUSampler(device, &samp_info);
		return true;
	}

	void set_frame(AVFrame *frame, double play_time, AppSub sub) {
		if (this->frame)
			ff::frame_recycle(this->frame);
		this->frame = frame;

		SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(device);
		SDL_GPUCopyPass *copy_pass = SDL_BeginGPUCopyPass(cmd);

		prepare_texture_draw(copy_pass);
		std::visit([&](auto&& sub){
			if (sub)
				sub->prepare_draw(copy_pass, play_time);
		}, sub);
		
		SDL_EndGPUCopyPass(copy_pass);
		if (!SDL_SubmitGPUCommandBuffer(cmd))
		{
			SDL_Log("SDL_SubmitGPUCommandBuffer failed: %s", SDL_GetError());
		}
	}

	void render(AppSub sub)
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
			std::visit([&](auto&& sub){
				if (sub)
					sub->draw(cmd, pass);
			}, sub);
			ImGui_ImplSDLGPU3_RenderDrawData(draw_data, cmd, pass);

			SDL_EndGPURenderPass(pass);
		}

		if (!SDL_SubmitGPUCommandBuffer(cmd))
		{
			SDL_Log("SDL_SubmitGPUCommandBuffer failed: %s", SDL_GetError());
		}
	}

	void window_size_changed(Sint32 w, Sint32 h) {
		wnd_w = w;
		wnd_h = h;
	}

	void reset_scale() {
		SDL_GetWindowSizeInPixels(window, &wnd_w, &wnd_h);
		base_scale = SDL_max((float) wnd_w / width, (float) wnd_h / height);
	}
};
