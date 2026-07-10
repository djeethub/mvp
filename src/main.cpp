#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

// Include Dear ImGui Headers
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"

#include "appstate.hpp"

constexpr int BORDER_SIZE = 5;

namespace {
std::string shell_quote(const std::string &value) {
    std::string escaped = "'";
    for (char c : value) {
        if (c == '\'') {
            escaped += "'\\''";
        } else {
            escaped += c;
        }
    }
    escaped += "'";
    return escaped;
}

bool open_file_location(const fs::path &file_path) {
    const fs::path parent_dir = file_path.parent_path();
    const std::string quoted_file = shell_quote(file_path.string());
    const std::string quoted_dir = shell_quote(parent_dir.string());

    const std::vector<std::string> commands = {
        "nautilus --select " + quoted_file,
        "nemo --select " + quoted_file,
        "caja --select " + quoted_file,
        "dolphin --select " + quoted_file,
        "thunar --select " + quoted_file,
        "xdg-open " + quoted_dir
    };

    for (const auto &command : commands) {
        if (std::system(command.c_str()) == 0) {
            return true;
        }
    }

    return false;
}
}

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
    if (!state->window) { delete state; return SDL_APP_FAILURE; }

    state->renderer.reset(SDL_CreateRenderer(state->window.get(), nullptr));
    if (!state->renderer) { delete state; return SDL_APP_FAILURE; }

    SDL_SetWindowHitTest(state->window.get(), WindowHitTest, nullptr);

    if (!state->load_image_at_index()) { delete state; return SDL_APP_FAILURE; }
    SDL_ShowWindow(state->window.get());

    // Initialize Dear ImGui Context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = NULL; // Disable default ini handling
    ImGui::StyleColorsDark();

    // Setup Platform/Renderer Backends
    ImGui_ImplSDL3_InitForSDLRenderer(state->window.get(), state->renderer.get());
    ImGui_ImplSDLRenderer3_Init(state->renderer.get());

    SDL_ShowWindow(state->window.get());
    return SDL_APP_CONTINUE;
}

SDL_AppResult quit(SDL_AppResult rlt, AppState *state) {
    state->stop();
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    return rlt;
}

SDL_AppResult SDL_AppEvent(void *appstate, SDL_Event *event) {
    auto *state = static_cast<AppState*>(appstate);
    ImGui_ImplSDL3_ProcessEvent(event);

    if (event->type == SDL_EVENT_QUIT) return quit(SDL_APP_SUCCESS, state);

    if (event->type == USEREVENT_NEXT_FRAME) {
        auto video_frame = state->video_frame.load(std::memory_order_acquire);
        SDL_UpdateYUVTexture(state->texture.get(), nullptr, video_frame->data[0], video_frame->linesize[0], video_frame->data[1], video_frame->linesize[1], video_frame->data[2], video_frame->linesize[2]);
        return SDL_APP_CONTINUE;
    }

    if (event->type == SDL_EVENT_WINDOW_RESIZED && state->window && event->window.windowID == SDL_GetWindowID(state->window.get()) && state->image_aspect > 0.0f) {
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

    if (event->type == SDL_EVENT_MOUSE_WHEEL && !ImGui::GetIO().WantCaptureMouse) {
        if (event->wheel.y < 0) {
            if (state->image_files.size() > 1) {
                state->current_index = (state->current_index + 1) % state->image_files.size();
                state->load_image_at_index();
            }
        } else if (event->wheel.y > 0) {
            if (state->image_files.size() > 1) {
                state->current_index = (state->current_index + state->image_files.size() - 1) % state->image_files.size();
                state->load_image_at_index();
            }
        }
    }

    if (event->type == SDL_EVENT_MOUSE_BUTTON_DOWN && event->button.button == SDL_BUTTON_RIGHT) {
        state->trigger_context_menu = true;
    }

    if (event->type == SDL_EVENT_KEY_DOWN) {
        switch (event->key.key) {
            case SDLK_ESCAPE: return quit(SDL_APP_SUCCESS, state);
            case SDLK_SPACE:
                if (state->image_files.size() > 1) {
                    state->current_index = (state->current_index + 1) % state->image_files.size();
                    state->load_image_at_index();
                }
                break;
            case SDLK_BACKSPACE:
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
        }
    }

    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppIterate(void *appstate) {
    auto *state = static_cast<AppState*>(appstate);

    int window_w = 0, window_h = 0;
    float texture_w = 0, texture_h = 0;
    SDL_FRect dst_rect{};

    SDL_GetRenderOutputSize(state->renderer.get(), &window_w, &window_h);
    if (state->texture) {
        SDL_GetTextureSize(state->texture.get(), &texture_w, &texture_h);
        float scale = SDL_min(static_cast<float>(window_w) / texture_w, static_cast<float>(window_h) / texture_h);
        dst_rect.w = texture_w * scale; dst_rect.h = texture_h * scale;
        dst_rect.x = (static_cast<float>(window_w) - dst_rect.w) / 2.0f; dst_rect.y = (static_cast<float>(window_h) - dst_rect.h) / 2.0f;
    }

    // Start ImGui Frame Rendering Chain
    ImGui_ImplSDLRenderer3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();

    if (state->trigger_context_menu) {
        ImGui::OpenPopup("mymenu");
        state->trigger_context_menu = false;
    }

    if (ImGui::BeginPopup("mymenu")) {
        if (ImGui::MenuItem("Next Image", "Space", false, state->image_files.size() > 1)) {
            state->current_index = (state->current_index + 1) % state->image_files.size();
            state->load_image_at_index();
        }
        if (ImGui::MenuItem("Previous Image", "Backspace", false, state->image_files.size() > 1)) {
            state->current_index = (state->current_index + state->image_files.size() - 1) % state->image_files.size();
            state->load_image_at_index();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Open File Location", "Ctrl+L")) {
            const auto &filename = state->image_files[state->current_index];
            const fs::path full = fs::path(state->parent_dir) / filename;
            open_file_location(full);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Exit", "Esc")) {
            return quit(SDL_APP_SUCCESS, state);
        }
        ImGui::EndPopup();
    }

    // Render hardware elements
    SDL_SetRenderDrawColor(state->renderer.get(), 30, 30, 30, 255);
    SDL_RenderClear(state->renderer.get());
    if (state->texture) {
        SDL_RenderTexture(state->renderer.get(), state->texture.get(), NULL, &dst_rect);
    }

    ImGui::Render();
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), state->renderer.get());

    SDL_RenderPresent(state->renderer.get());
    return SDL_APP_CONTINUE;
}

void SDL_AppQuit(void *appstate, SDL_AppResult result) {
    auto *state = static_cast<AppState*>(appstate);
    if (state) delete state;
}
