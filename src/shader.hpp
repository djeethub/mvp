#pragma once

#include "lanczos-3.frag.h"
#include "test.frag.h"

SDL_GPURenderState *create_render_state(SDL_Renderer *renderer) {
    SDL_GPUDevice *device = SDL_GetGPURendererDevice(renderer);

    SDL_GPUShaderCreateInfo shader_info = {0};
    shader_info.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
    shader_info.entrypoint = "main";
    shader_info.num_samplers = 1;               // usually 1 (the texture being drawn)
//    shader_info.num_uniform_buffers = 1;        // if you need uniforms

    // Choose format based on what the device supports
    SDL_GPUShaderFormat formats = SDL_GetGPUShaderFormats(device);

    if (formats & SDL_GPU_SHADERFORMAT_SPIRV) {
        shader_info.format = SDL_GPU_SHADERFORMAT_SPIRV;
//        shader_info.code = lanczos_3_frag;
//        shader_info.code_size = lanczos_3_frag_len;
        shader_info.code = test_frag;
        shader_info.code_size = test_frag_len;
    } else if (formats & SDL_GPU_SHADERFORMAT_DXIL) {
        // DXIL version...
    } else if (formats & SDL_GPU_SHADERFORMAT_MSL) {
        // MSL version...
    }

    SDL_GPUShader* frag_shader = SDL_CreateGPUShader(device, &shader_info);
    if (!frag_shader) {
        SDL_Log("SDL_CreateGPUShader failed: %s", SDL_GetError());
        return nullptr;
    }

    SDL_GPURenderStateCreateInfo state_info = {0};
    state_info.fragment_shader = frag_shader;

    SDL_GPURenderState* state = SDL_CreateGPURenderState(renderer, &state_info);
    if (!state) {
        SDL_Log("SDL_CreateGPURenderState failed: %s", SDL_GetError());
        SDL_ReleaseGPUShader(device, state_info.fragment_shader);
        return nullptr;
    }

    return state;
}
