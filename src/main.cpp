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

uint32_t SDLCALL TimerCallback(void* userdata, SDL_TimerID timerID, uint32_t interval) {
    auto *state = static_cast<AppState*>(userdata);
    return state->time_next_frame();
}

AppGui gui;

SDL_AppResult SDL_AppInit(void **appstate, int argc, char *argv[]) {
    if (argc < 2 || !argv[1]) return SDL_APP_FAILURE;

    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        SDL_Log("Failed to initialize SDL Audio: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }
    // Print out WHICH driver SDL actually loaded so we can verify it
    SDL_Log("Using Audio Driver: %s", SDL_GetCurrentAudioDriver());
    SDL_SetHint(SDL_HINT_MAIN_CALLBACK_RATE, "waitevent");

    auto state = new AppState();
    *appstate = state;

    fs::path argpath = argv[1];
    if (argpath.has_parent_path()) {
        state->parent_dir = argpath.parent_path().string();
        if (state->parent_dir.back() != fs::path::preferred_separator) state->parent_dir.push_back(fs::path::preferred_separator);
        state->image_files.push_back(argpath.filename().string());
    } else {
        state->parent_dir = std::string("./");
        state->image_files.push_back(argpath.filename().string());
    }

    // Enumerate directory for supported images
    try {
        for (auto &entry : fs::directory_iterator(state->parent_dir)) {
            if (!entry.is_regular_file()) continue;
            if (AppState::is_supported_image(entry.path())) {
                state->image_files.push_back(entry.path().filename().string());
            }
        }
        // remove duplicates and sort
        std::sort(state->image_files.begin(), state->image_files.end());
        state->image_files.erase(std::unique(state->image_files.begin(), state->image_files.end()), state->image_files.end());

        // find initial file index
        auto it = std::find(state->image_files.begin(), state->image_files.end(), argpath.filename().string());
        if (it != state->image_files.end()) state->current_index = static_cast<std::size_t>(std::distance(state->image_files.begin(), it));
    } catch (...) {
        // filesystem errors -> failure
    }

    Uint32 window_flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_BORDERLESS | SDL_WINDOW_HIDDEN;
    state->window.reset(SDL_CreateWindow("miv", 800, 600, window_flags));
    if (!state->window) { return SDL_APP_FAILURE; }

    state->renderer.reset(SDL_CreateRenderer(state->window.get(), nullptr));
    if (!state->renderer) { return SDL_APP_FAILURE; }

    SDL_SetWindowHitTest(state->window.get(), WindowHitTest, nullptr);

    if (!state->load_image_at_index()) { return SDL_APP_FAILURE; }
    SDL_ShowWindow(state->window.get());

    gui.init(state);

    SDL_ShowWindow(state->window.get());
    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppEvent(void *appstate, SDL_Event *event) {
    auto *state = static_cast<AppState*>(appstate);
    ImGui_ImplSDL3_ProcessEvent(event);

    if (event->type == SDL_EVENT_QUIT) return SDL_APP_SUCCESS;

    if (event->type == USEREVENT_NEXT_FRAME) {
        auto video_frame = state->video_frame.load(std::memory_order_acquire);
        if (video_frame)
            SDL_UpdateYUVTexture(state->texture.get(), nullptr, video_frame->data[0], video_frame->linesize[0], video_frame->data[1], video_frame->linesize[1], video_frame->data[2], video_frame->linesize[2]);
        return SDL_APP_CONTINUE;
    }

/*     if (event->type == SDL_EVENT_WINDOW_RESIZED && state->window && event->window.windowID == SDL_GetWindowID(state->window.get()) && state->image_aspect > 0.0f) {
        int new_w = event->window.data1;
        int new_h = event->window.data2;
        if (new_w > 0 && new_h > 0) {
            int adjusted_h = static_cast<int>(std::round(new_w / state->image_aspect));
            int adjusted_w = static_cast<int>(std::round(new_h * state->image_aspect));
            int target_w, target_h;
            if (std::abs(adjusted_h - new_h) < std::abs(adjusted_w - new_w)) {
                target_w = new_w;
                target_h = adjusted_h;
            } else {
                target_w = adjusted_w;
                target_h = new_h;
            }
            if (target_w > 0 && target_h > 0 && (target_w != new_w || target_h != new_h)) {
                SDL_SetWindowSize(state->window.get(), target_w, target_h);
            }
        }
    }
 */
    if (event->type == SDL_EVENT_MOUSE_WHEEL && !ImGui::GetIO().WantCaptureMouse) {
        if (event->wheel.y < 0) {
        } else if (event->wheel.y > 0) {
        }
    }

    if (event->type == SDL_EVENT_MOUSE_BUTTON_DOWN && event->button.button == SDL_BUTTON_RIGHT) {
        state->trigger_context_menu = true;
    }

    if (event->type == SDL_EVENT_KEY_DOWN) {
        switch (event->key.key) {
            case SDLK_ESCAPE: return SDL_APP_SUCCESS;
            case SDLK_RETURN:
            case SDLK_PERIOD:
                if (state->image_files.size() > 1) {
                    state->current_index = (state->current_index + 1) % state->image_files.size();
                    state->load_image_at_index();
                }
                break;
            case SDLK_COMMA:
                if (state->image_files.size() > 1) {
                    state->current_index = (state->current_index + state->image_files.size() - 1) % state->image_files.size();
                    state->load_image_at_index();
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
                state->video_scale += SCALE_N;
                gui.show_noti(std::format("Video Scale: {:.2f}", state->video_scale));
                break;
            case SDLK_KP_1:
                state->video_scale -= SCALE_N;
                gui.show_noti(std::format("Video Scale: {:.2f}", state->video_scale));
                break;
            case SDLK_KP_5:
                state->video_scale = 1;
                state->video_pan_x = 0;
                state->video_pan_y = 0;
                gui.show_noti("Video Reset");
                break;
            case SDLK_KP_4:
                state->video_pan_x -= PAN_N;
                gui.show_noti(std::format("Video Pan X: {}", state->video_pan_x));
                break;
            case SDLK_KP_6:
                state->video_pan_x += PAN_N;
                gui.show_noti(std::format("Video Pan X: {}", state->video_pan_x));
                break;
            case SDLK_KP_8:
                state->video_pan_y -= PAN_N;
                gui.show_noti(std::format("Video Pan Y: {}", state->video_pan_y));
                break;
            case SDLK_KP_2:
                state->video_pan_y += PAN_N;
                gui.show_noti(std::format("Video Pan Y: {}", state->video_pan_y));
                break;
            case SDLK_1:
                if (event->key.mod & SDL_KMOD_ALT) {
                    state->resize_window(0.5);
                    gui.show_noti("Window Scale: 0.5");
                }
                break;
            case SDLK_2:
                if (event->key.mod & SDL_KMOD_ALT)
                {
                    state->resize_window(1);
                    gui.show_noti("Window Scale: 1");
                }
                break;
            case SDLK_3:
                if (event->key.mod & SDL_KMOD_ALT)
                {
                    state->resize_window(2);
                    gui.show_noti("Window Scale: 2");
                }
                break;
            case SDLK_RIGHT:
                state->seek_relative(event->key.mod & SDL_KMOD_ALT ? 15 : 5);
                break;
            case SDLK_LEFT:
                state->seek_relative(event->key.mod & SDL_KMOD_ALT ? -15 : -5);
                break;

            case SDLK_PAGEDOWN:
            case SDLK_PAGEUP:
            {
                auto id = state->get_relative_chapter(event->key.key == SDLK_PAGEDOWN ? 1 : -1);
                if (id >= 0) {
                    if (!state->seek_to_chapter(id))
                        state->seek_relative(event->key.key == SDLK_PAGEDOWN ? 30 : -30);
                    else
                        gui.show_noti(state->chapter_list[id].title);
                }
            }
                break;

            case SDLK_SPACE:
                state->pause();
                gui.show_noti(state->is_paused ? "Paused" : "Resumed");
                break;
            }
    }

    return SDL_APP_CONTINUE;
}

/* inline void DrawOSD(AppState *state)
{
    // 1. Check if the OSD has expired
    if (SDL_GetTicks() > state->osd_noti.expires_at)
    {
        return; // Stale message, don't render anything
    }

    // 2. Position the OSD in the top-right corner of the viewport
    // (You can change this to center or bottom depending on preference)
    const float PADDING = 20.0f;
    const ImGuiViewport *viewport = ImGui::GetMainViewport();
    ImVec2 target_pos = ImVec2(
        viewport->WorkPos.x + PADDING,
        viewport->WorkPos.y + PADDING);

    // Set pivot point to top-right (1.0f, 0.0f) so padding scales correctly
    ImGui::SetNextWindowPos(target_pos, ImGuiCond_Always, ImVec2(0.0f, 0.0f));

    // Make the background semi-transparent black
    ImGui::SetNextWindowBgAlpha(0.0f);

    // 3. Configure window flags to make it completely non-interactive
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration |
                             ImGuiWindowFlags_AlwaysAutoResize |
                             ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoFocusOnAppearing |
                             ImGuiWindowFlags_NoNav |
                             ImGuiWindowFlags_NoMove;

    // 4. Render the window
    if (ImGui::Begin("##OSD_Overlay", nullptr, flags))
    {
        // Optional: Make text larger or use a bold font if loaded
        ImGui::SetWindowFontScale(2.0f);

        ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "%s", state->osd_noti.text.c_str());

        ImGui::End();
    }
}
 */
SDL_AppResult SDL_AppIterate(void *appstate) {
    auto *state = static_cast<AppState*>(appstate);

    int window_w = 0, window_h = 0;
    float texture_w = 0, texture_h = 0;
    SDL_FRect dst_rect{};

    SDL_GetRenderOutputSize(state->renderer.get(), &window_w, &window_h);
    if (state->texture) {
        SDL_GetTextureSize(state->texture.get(), &texture_w, &texture_h);
        float scale = SDL_max(static_cast<float>(window_w) / texture_w, static_cast<float>(window_h) / texture_h) * state->video_scale;
        dst_rect.w = texture_w * scale; dst_rect.h = texture_h * scale;
        dst_rect.x = (static_cast<float>(window_w) - dst_rect.w) / 2.0f + state->video_pan_x; dst_rect.y = (static_cast<float>(window_h) - dst_rect.h) / 2.0f + state->video_pan_y;
    }

    // Render hardware elements
    SDL_SetRenderDrawColor(state->renderer.get(), 50, 50, 50, 255);
    SDL_RenderClear(state->renderer.get());
    if (state->texture) {
        SDL_RenderTexture(state->renderer.get(), state->texture.get(), NULL, &dst_rect);
    }

    auto app_result = gui.draw();
    if (app_result != SDL_APP_CONTINUE)
        return app_result;

    SDL_RenderPresent(state->renderer.get());
    return SDL_APP_CONTINUE;
}

void SDL_AppQuit(void *appstate, SDL_AppResult result) {
    auto *state = static_cast<AppState*>(appstate);
    if (state) {
        state->shutdown();
        delete state;
    }
}
