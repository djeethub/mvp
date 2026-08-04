#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include "appstate.hpp"
#include "gui.hpp"

constexpr int BORDER_SIZE = 5;
#define PAN_N   5
#define SCALE_N 0.01

static SDL_HitTestResult SDLCALL WindowHitTest(SDL_Window *win, const SDL_Point *area, void *data) {
    if (ImGui::GetIO().WantCaptureMouse) return SDL_HITTEST_NORMAL;

    int w, h;
    SDL_GetWindowSize(win, &w, &h);
    bool top = (area->y <= BORDER_SIZE), bottom = (area->y >= h - BORDER_SIZE);
    bool left = (area->x <= BORDER_SIZE), right = (area->x >= w - BORDER_SIZE);

    if (top && left) return SDL_HITTEST_RESIZE_TOPLEFT;
    if (top && right) return SDL_HITTEST_RESIZE_TOPRIGHT;
    if (bottom && left) return SDL_HITTEST_RESIZE_BOTTOMLEFT;
    if (bottom && right) return SDL_HITTEST_RESIZE_BOTTOMRIGHT;
    if (top) return SDL_HITTEST_RESIZE_TOP;
    if (bottom) return SDL_HITTEST_RESIZE_BOTTOM;
    if (left) return SDL_HITTEST_RESIZE_LEFT;
    if (right) return SDL_HITTEST_RESIZE_RIGHT;

    return SDL_HITTEST_DRAGGABLE;
}

AppGui gui;
AssHandler ass;

SDL_AppResult SDL_AppInit(void **appstate, int argc, char *argv[]) {
    if (argc < 2 || !argv[1]) return SDL_APP_FAILURE;

    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        SDL_Log("Failed to initialize SDL Audio: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }
    SDL_SetHint(SDL_HINT_MAIN_CALLBACK_RATE, "waitevent");

    auto state = new AppState();
    *appstate = state;

    Uint32 window_flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_BORDERLESS | SDL_WINDOW_HIDDEN;
    state->window.reset(SDL_CreateWindow("miv", 800, 600, window_flags));
    if (!state->window) { return SDL_APP_FAILURE; }

    state->gpu.init(state->window.get());

//    state->renderer.reset(state->gpu.create_renderer());
//    if (!state->renderer) { return SDL_APP_FAILURE; }


    if (!state->open_file(argv[1])) { return SDL_APP_FAILURE; }
    gui.init(state);
    SDL_ShowWindow(state->window.get());
    SDL_SetWindowHitTest(state->window.get(), WindowHitTest, nullptr);
    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppEvent(void *appstate, SDL_Event *event) {
    auto *state = static_cast<AppState*>(appstate);
    ImGui_ImplSDL3_ProcessEvent(event);

    if (event->type == USEREVENT_NEXT_FRAME) {
        auto video_frame = state->video_frame.exchange(nullptr, std::memory_order_acquire);
        if (video_frame) {
//            SDL_UpdateYUVTexture(state->texture.get(), nullptr, video_frame->data[0], video_frame->linesize[0], video_frame->data[1], video_frame->linesize[1], video_frame->data[2], video_frame->linesize[2]);
/*            state->create_texture(video_frame);
            switch (get_update_kind(state->texture->format)) {
                case SDL_UPDATE_NV:
                    SDL_UpdateNVTexture(state->texture.get(), nullptr, video_frame->data[0], video_frame->linesize[0], video_frame->data[1], video_frame->linesize[1]);
                    break;
                case SDL_UPDATE_YUV:
                    SDL_UpdateYUVTexture(state->texture.get(), nullptr, video_frame->data[0], video_frame->linesize[0], video_frame->data[1], video_frame->linesize[1], video_frame->data[2], video_frame->linesize[2]);
                    break;
                default:
                    SDL_UpdateTexture(state->texture.get(), nullptr, video_frame->data[0], video_frame->linesize[0]);
                    break;
            }*/
            state->gpu.set_frame(video_frame, state->get_play_time());
//            state->gpu.render();
        }
        return SDL_APP_CONTINUE;
    } else if (event->type == USEREVENT_SUBTITLE_ASS) {
        Subtitle *subtitle;
        while (state->sub_queue.dequeue(subtitle)) {
            ass.add_ass(subtitle->text, static_cast<long long>(subtitle->pts * 1000), static_cast<long long>(subtitle->duration * 1000));
//                printf("subtitle: %s, %f, %f\n", subtitle->text.c_str(), subtitle->pts, subtitle->duration);
            state->sub_queue.recycle(subtitle);
        }
        return SDL_APP_CONTINUE;
    }

    switch (event->type) {
        case SDL_EVENT_QUIT:
            return SDL_APP_SUCCESS;

        case SDL_EVENT_MOUSE_WHEEL:
            if (!ImGui::GetIO().WantCaptureMouse) {
                if (event->wheel.y < 0) {
                    if (state->media_mode == Image)
                        if (state->open_next_file(true)) {
                            gui.show_noti(state->get_file_name());
                        }
                } else if (event->wheel.y > 0) {
                    if (state->media_mode == Image)
                        if (state->open_next_file(false)) {
                            gui.show_noti(state->get_file_name());
                        }
                }
            }
            break;

        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            if (event->button.button == SDL_BUTTON_RIGHT)
                state->trigger_context_menu = true;
            else if (event->button.button == SDL_BUTTON_MIDDLE) {
                state->pause();
                gui.show_noti(state->is_paused ? "Paused" : "Resumed");
            }
            break;

        case SDL_EVENT_KEY_DOWN:
            switch (event->key.key) {
                case SDLK_ESCAPE: return SDL_APP_SUCCESS;
                case SDLK_RETURN:
                    if (state->open_next_file(true)) {
                        gui.show_noti(state->get_file_name());
                    }
                    break;
                case SDLK_BACKSPACE:
                    if (state->open_next_file(false)) {
                        gui.show_noti(state->get_file_name());                        
                    }
                    break;
                case SDLK_L:
                    if (event->key.mod & SDL_KMOD_CTRL) {
                        const auto &filename = state->image_files[state->current_index];
                        const fs::path full = fs::path(state->parent_dir) / filename;
                        open_file_location(full);
                    }
                    break;
                case SDLK_KP_9:
                    state->gpu.video_scale += SCALE_N;
                    gui.show_noti(std::format("Video Scale: {:.2f}", state->gpu.video_scale));
                    break;
                case SDLK_KP_1:
                    state->gpu.video_scale -= SCALE_N;
                    gui.show_noti(std::format("Video Scale: {:.2f}", state->gpu.video_scale));
                    break;
                case SDLK_KP_5:
                    state->gpu.video_pan_x = 0;
                    state->gpu.video_pan_y = 0;
                    state->gpu.video_scale = 1;
                    gui.show_noti("Video Reset");
                    break;
                case SDLK_KP_4:
                    state->gpu.video_pan_x -= PAN_N;
                    gui.show_noti(std::format("Video Pan X: {}", state->gpu.video_pan_x));
                    break;
                case SDLK_KP_6:
                    state->gpu.video_pan_x += PAN_N;
                    gui.show_noti(std::format("Video Pan X: {}", state->gpu.video_pan_x));
                    break;
                case SDLK_KP_8:
                    state->gpu.video_pan_y += PAN_N;
                    gui.show_noti(std::format("Video Pan Y: {}", state->gpu.video_pan_y));
                    break;
                case SDLK_KP_2:
                    state->gpu.video_pan_y -= PAN_N;
                    gui.show_noti(std::format("Video Pan Y: {}", state->gpu.video_pan_y));
                    break;
                case SDLK_1:
                    if (event->key.mod & SDL_KMOD_ALT) {
                        state->resize_window(0.5);
                        state->gpu.reset_scale();
                        gui.show_noti("Window Scale: 0.5");
                    }
                    break;
                case SDLK_2:
                    if (event->key.mod & SDL_KMOD_ALT)
                    {
                        state->resize_window(1);
                        state->gpu.reset_scale();
                        gui.show_noti("Window Scale: 1");
                    }
                    break;
                case SDLK_3:
                    if (event->key.mod & SDL_KMOD_ALT)
                    {
                        state->resize_window(2);
                        state->gpu.reset_scale();
                        gui.show_noti("Window Scale: 2");
                    }
                    break;
                case SDLK_RIGHT:
                    state->seek_relative(event->key.mod & SDL_KMOD_ALT ? 15 : 6);
                    break;
                case SDLK_LEFT:
                    state->seek_relative(event->key.mod & SDL_KMOD_ALT ? -15 : -6);
                    break;

                case SDLK_PAGEDOWN:
                case SDLK_PAGEUP:
                {
                    auto id = state->get_relative_chapter(event->key.key == SDLK_PAGEDOWN ? 1 : -1);
                    if (id >= 0) {
                        if (state->seek_to_chapter(id))
                            gui.show_noti(state->chapter_list[id].title);
                    } else {
                        state->seek_relative(event->key.key == SDLK_PAGEDOWN ? 40 : -40);
                    }
                }
                    break;

                case SDLK_SPACE:
                    state->pause();
                    gui.show_noti(state->is_paused ? "Paused" : "Resumed");
                    break;

                case SDLK_T:
                    gui.show_noti(state->get_file_name());
                    break;

                case SDLK_HOME:
                    state->seek(state->video.get_start_time(), true);
                    break;
            }
            break;

        case SDL_EVENT_WINDOW_RESIZED:
            state->gpu.window_size_changed();
            break;

        case SDL_EVENT_DROP_FILE:
        {
            const char* dropped_file_path = event->drop.data;
            if (dropped_file_path) {
                std::cout << "File dropped: " << dropped_file_path << std::endl;
                state->open_file(dropped_file_path);
            }
        }
            break;
    }

    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppIterate(void *appstate) {
    auto *state = static_cast<AppState*>(appstate);

    auto app_result = gui.draw();
    if (app_result != SDL_APP_CONTINUE)
        return app_result;
    state->gpu.render();
    return app_result;
}

void SDL_AppQuit(void *appstate, SDL_AppResult result) {
    auto *state = static_cast<AppState*>(appstate);
    if (state) {
        SDL_SetWindowHitTest(state->window.get(), nullptr, nullptr);
        state->shutdown();
        ass.shutdown();
        gui.shutdown();
//        SDL_WaitForGPUIdle(state->gpu.get_device());
        delete state;
    }
}
