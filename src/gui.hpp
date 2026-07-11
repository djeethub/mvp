#pragma once

#include <string>
#include <format>

// Include Dear ImGui Headers
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"

#include <SDL3/SDL.h>

#include "appstate.hpp"

namespace
{
    std::string shell_quote(const std::string &value)
    {
        std::string escaped = "'";
        for (char c : value)
        {
            if (c == '\'')
            {
                escaped += "'\\''";
            }
            else
            {
                escaped += c;
            }
        }
        escaped += "'";
        return escaped;
    }

    bool open_file_location(const fs::path &file_path)
    {
        const fs::path parent_dir = file_path.parent_path();
        const std::string quoted_file = shell_quote(file_path.string());
        const std::string quoted_dir = shell_quote(parent_dir.string());

        const std::vector<std::string> commands = {
            "nautilus --select " + quoted_file,
            "nemo --select " + quoted_file,
            "caja --select " + quoted_file,
            "dolphin --select " + quoted_file,
            "thunar --select " + quoted_file,
            "xdg-open " + quoted_dir};

        for (const auto &command : commands)
        {
            if (std::system(command.c_str()) == 0)
            {
                return true;
            }
        }

        return false;
    }
}

class AppGui {
    public:
        AppGui() = default;
        ~AppGui()
        {
            if (state) {
                ImGui_ImplSDLRenderer3_Shutdown();
                ImGui_ImplSDL3_Shutdown();
                ImGui::DestroyContext();
            }
        }

        void init(AppState *state)
        {
            AppGui::state = state;

            // Initialize Dear ImGui Context
            IMGUI_CHECKVERSION();
            ImGui::CreateContext();
            ImGuiIO &io = ImGui::GetIO();
            io.IniFilename = NULL; // Disable default ini handling
            ImGui::StyleColorsDark();

            // Setup Platform/Renderer Backends
            ImGui_ImplSDL3_InitForSDLRenderer(state->window.get(), state->renderer.get());
            ImGui_ImplSDLRenderer3_Init(state->renderer.get());
        }

        SDL_AppResult draw()
        {
            auto curr_ticks = SDL_GetTicks();
            ImGuiIO &io = ImGui::GetIO();

            // Start ImGui Frame Rendering Chain
            ImGui_ImplSDLRenderer3_NewFrame();
            ImGui_ImplSDL3_NewFrame();
            ImGui::NewFrame();

            if (text_expires_at > curr_ticks)
            {
                ImGui::SetNextWindowPos(ImVec2(20, 20));
                ImGui::Begin("Text", nullptr,
                             ImGuiWindowFlags_NoTitleBar |
                                 ImGuiWindowFlags_NoResize |
                                 ImGuiWindowFlags_NoMove |
                                 ImGuiWindowFlags_NoScrollbar |
                                 ImGuiWindowFlags_NoBackground |
                                 ImGuiWindowFlags_NoBringToFrontOnFocus |
                                 ImGuiWindowFlags_AlwaysAutoResize);
                ImGui::Text(noti_text.c_str());
                ImGui::End();
            }

            if (slider_expires_at > curr_ticks) {
                ImGui::SetNextWindowPos(ImVec2(20, 0), 0, ImVec2(0.5, 0.9));
                auto size = ImVec2(io.DisplaySize.x * 0.8, io.DisplaySize.y * 0.05);
                ImGui::SetNextWindowSize(size);
                ImGui::Begin("Slider", nullptr,
                             ImGuiWindowFlags_NoTitleBar |
                                 ImGuiWindowFlags_NoResize |
                                 ImGuiWindowFlags_NoMove |
                                 ImGuiWindowFlags_NoScrollbar |
                                 ImGuiWindowFlags_NoBackground |
                                 ImGuiWindowFlags_NoBringToFrontOnFocus |
                                 ImGuiWindowFlags_AlwaysAutoResize);
                float v = 0.3;
                ImGui::PushItemWidth(size.x);
                ImGui::SliderFloat("##threshold", &v, 0.0f, 1.0f, "Threshold: %.3f");
                ImGui::End();
            }

            if (state->trigger_context_menu)
            {
                ImGui::OpenPopup("mymenu");
                state->trigger_context_menu = false;
            }

            if (ImGui::BeginPopup("mymenu"))
            {
                if (ImGui::MenuItem("Next Image", "Space", false, state->image_files.size() > 1))
                {
                    state->current_index = (state->current_index + 1) % state->image_files.size();
                    state->load_image_at_index();
                }
                if (ImGui::MenuItem("Previous Image", "Backspace", false, state->image_files.size() > 1))
                {
                    state->current_index = (state->current_index + state->image_files.size() - 1) % state->image_files.size();
                    state->load_image_at_index();
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Open File Location", "Ctrl+L"))
                {
                    const auto &filename = state->image_files[state->current_index];
                    const fs::path full = fs::path(state->parent_dir) / filename;
                    open_file_location(full);
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Exit", "Esc"))
                {
                    return SDL_APP_SUCCESS;
                }
                ImGui::EndPopup();
            }

            ImGui::Render();
            ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), state->renderer.get());
            return SDL_APP_CONTINUE;
        }

        void show_noti(const std::string &message, uint64_t duration_ms = 2000)
        {
            noti_text = message;
            text_expires_at = SDL_GetTicks() + duration_ms;
        }

    private:
        AppState *state = nullptr;
        std::string noti_text = "";
        uint64_t text_expires_at = 0; // Expiration time in SDL ticks (milliseconds)
        uint64_t slider_expires_at = 0;
};