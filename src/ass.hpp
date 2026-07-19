#pragma once

#include <ass/ass.h>
#include <SDL3/SDL.h>

#include "ffmpeg.hpp"

class AssHandler {
public:
    bool init(int width, int height, AVCodecContext *subtitle_codec_ctx) {
        ass_track.reset();
        ass_renderer.reset();

        // 1. Initialize your standard libass environment
        if (!ass_library)
            ass_library.reset(ass_library_init());
        ass_renderer.reset(ass_renderer_init(ass_library.get()));
        ass_set_fonts(ass_renderer.get(), nullptr, "Sans", 1, nullptr, 1);
        ass_set_storage_size(ass_renderer.get(), width, height);
        ass_set_frame_size(ass_renderer.get(), width, height); // Match your window canvas size        

        // 2. Create an EMPTY, blank track that you will feed packets manually
        ass_track.reset(ass_new_track(ass_library.get()));

        if (subtitle_codec_ctx->subtitle_header_size > 0) {
            ass_process_codec_private(
                ass_track.get(), 
                (char*)subtitle_codec_ctx->subtitle_header, 
                subtitle_codec_ctx->subtitle_header_size
            );

            return true;
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

    void draw(SDL_Renderer *renderer,  double play_time) {
        if (!ass_track)
            return;

        // 2. Set alpha blend settings for overlay layers
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);            

        int changed = 0;
        // Ask libass to process the track at this specific millisecond frame marker
        ASS_Image* img = ass_render_frame(ass_renderer.get(), ass_track.get(), play_time * 1000, &changed);
        // 3. Draw the active text lines over the frame canvas
        while (img) {
//            printf("SUCCESS: libass generated image chunks! w=%d, h=%d at position x=%d, y=%d\n", img->w, img->h, img->dst_x, img->dst_y);
            if (img->w > 0 && img->h > 0) {
                // 1. Create your texture
                if (!sub_texture || sub_texture.get()->w < img->w || sub_texture.get()->h < img->h)
                    sub_texture.reset(SDL_CreateTexture(
                        renderer, 
                        SDL_PIXELFORMAT_RGBA8888, 
                        SDL_TEXTUREACCESS_STREAMING, 
                        img->w + 32, 
                        img->h + 32
                    ));

                if (sub_texture) {
                    uint8_t r = (img->color >> 24) & 0xFF;
                    uint8_t g = (img->color >> 16) & 0xFF;
                    uint8_t b = (img->color >> 8)  & 0xFF;
                    uint8_t a = (img->color)       & 0xFF;
                    
                    float alpha_multiplier = (255.0f - a) / 255.0f;

                    // DECLARE AS BYTE POINTER (uint8_t*) TO MATCH PITCH VALUE BYTES
                    SDL_Rect lock_rect = { 0, 0, img->w, img->h };
                    uint8_t* raw_pixels = nullptr;
                    int pitch = 0;

                    if (SDL_LockTexture(sub_texture.get(), &lock_rect, (void**)&raw_pixels, &pitch) == true) { // SDL3 returns true on success
                        
                        for (int y = 0; y < img->h; ++y) {
                            // Find the precise hardware pointer start address for this specific row
                            uint32_t* row_pixels = reinterpret_cast<uint32_t*>(raw_pixels + (y * pitch));

                            for (int x = 0; x < img->w; ++x) {
                                uint8_t coverage = img->bitmap[y * img->stride + x];
                                
                                if (coverage > 0) {
                                    uint8_t final_alpha = static_cast<uint8_t>(coverage * alpha_multiplier);
                                    
                                    // Safe, unpadded index write
                                    row_pixels[x] = (r << 24) | (g << 16) | (b << 8) | final_alpha;
                                } else {
                                    // Fully transparent clear space background 
                                    row_pixels[x] = 0x00000000;
                                }
                            }
                        }
                        SDL_UnlockTexture(sub_texture.get());
                    }

                    SDL_FRect src_rect = { 0.0f, 0.0f, static_cast<float>(img->w), static_cast<float>(img->h) };
                    SDL_FRect dst_rect = {
                        .x = static_cast<float>(img->dst_x),
                        .y = static_cast<float>(img->dst_y),
                        .w = static_cast<float>(img->w),
                        .h = static_cast<float>(img->h)
                    };
                    SDL_RenderTexture(renderer, sub_texture.get(), &src_rect, &dst_rect);
                }
            }

            img = img->next; // Progress to next overlay cluster segment (e.g., border outlines)
        }
    }

private:
    std::unique_ptr<ASS_Library, decltype(&ass_library_done)> ass_library{nullptr, ass_library_done};
    std::unique_ptr<ASS_Renderer, decltype(&ass_renderer_done)> ass_renderer{nullptr, ass_renderer_done};
    std::unique_ptr<ASS_Track, decltype(&ass_free_track)> ass_track{nullptr, ass_free_track};
    std::unique_ptr<SDL_Texture, decltype(&SDL_DestroyTexture)> sub_texture{nullptr, SDL_DestroyTexture};
};