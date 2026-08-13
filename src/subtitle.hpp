#pragma once

#include <SDL3/SDL.h>

#include "ffmpeg.hpp"

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

class AppSubtitle {
protected:
    SDL_GPUDevice *device = nullptr;
    
    AppSubtitle(SDL_GPUDevice *gpu) : device(gpu) {}

public:
    virtual ~AppSubtitle() {}

    virtual void shutdown() = 0;
    virtual void flush() = 0;
    virtual void prepare_draw(SDL_GPUCopyPass *pass, double play_time) = 0;
    virtual void draw(SDL_GPUCommandBuffer *cmd, SDL_GPURenderPass *pass) = 0;
};
