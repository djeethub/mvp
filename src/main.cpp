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
    SDL_GetWindowSizeInPixels(win, &w, &h);
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

#ifdef __linux__
    return SDL_HITTEST_DRAGGABLE;
#else
    if (SDL_GetGlobalMouseState(nullptr, nullptr) & SDL_BUTTON_LMASK)
        return SDL_HITTEST_DRAGGABLE;
    return SDL_HITTEST_NORMAL;
#endif
}

AppGui gui;

SDL_AppResult SDL_AppInit(void **appstate, int argc, char *argv[]) {
    if (argc < 2 || !argv[1]) return SDL_APP_FAILURE;

    if (!SDL_Init(SDL_INIT_AUDIO | SDL_INIT_VIDEO)) {
        SDL_Log("Failed to initialize SDL Audio: %s", SDL_GetError());
    }
    SDL_SetHint(SDL_HINT_MAIN_CALLBACK_RATE, "waitevent");

    auto state = new AppState();
    *appstate = state;
    if (!state->init())
        return SDL_APP_FAILURE;

    if (!state->open_file(argv[1])) { return SDL_APP_FAILURE; }
    gui.init(state);
    SDL_ShowWindow(state->window);
    SDL_SetWindowHitTest(state->window, WindowHitTest, nullptr);
    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppEvent(void *appstate, SDL_Event *event) {
    auto *state = static_cast<AppState*>(appstate);
    ImGui_ImplSDL3_ProcessEvent(event);

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
            if (event->button.button == SDL_BUTTON_MIDDLE) {
                state->pause();
                gui.show_noti(state->video.is_paused ? "Paused" : "Resumed");
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
                        state->open_file_location();
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
                    gui.show_noti(state->video.is_paused ? "Paused" : "Resumed");
                    break;

                case SDLK_T:
                    gui.show_noti(state->get_file_name());
                    break;

                case SDLK_HOME:
                    state->seek(state->video.get_start_time());
                    break;

                case SDLK_F9:
                    SDL_MinimizeWindow(state->window);
                    state->pause(true);
                    break;
            }
            break;

        case SDL_EVENT_WINDOW_RESIZED:
            state->gpu.window_size_changed(event->window.data1, event->window.data2);
            break;

        case SDL_EVENT_DROP_FILE:
        {
            const char* dropped_file_path = event->drop.data;
            if (dropped_file_path) {
                SDL_Log("File dropped: %s\n", dropped_file_path);
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

    state->render();
    return app_result;
}

void SDL_AppQuit(void *appstate, SDL_AppResult result) {
    auto *state = static_cast<AppState*>(appstate);
    if (state) {
        SDL_SetWindowHitTest(state->window, nullptr, nullptr);
        state->shutdown();
        SDL_WaitForGPUIdle(state->gpu.get_device());
        gui.shutdown();
        delete state;
    }
}
