#pragma once

#include <SDL3/SDL.h>

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
    SDL_GPUGraphicsPipeline *pipeline = nullptr;
    SDL_GPUSampler *sampler = nullptr;
    int wnd_w = 0;
    int wnd_h = 0;

public:
    AppSubtitle(SDL_GPUDevice *gpu) : device(gpu) {}
};

class SubAss;
class SubBitmap;
using AppSub = std::variant<SubAss *, SubBitmap *>;
