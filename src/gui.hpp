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

            // 3. Load the fonts from your local system or project directory
            // Arguments: (Filepath, Font Size in pixels, Config Struct, Glyph Ranges)
            uiFont  = io.Fonts->AddFontFromFileTTF("/usr/share/fonts/open-sans/OpenSans-Regular.ttf", 16.0f);
            osdFont = io.Fonts->AddFontFromFileTTF("/usr/share/fonts/open-sans/OpenSans-Bold.ttf", 32.0f);
            subtitleFont = io.Fonts->AddFontFromFileTTF("/usr/share/fonts/open-sans/OpenSans-Bold.ttf", 52.0f);

            // 4. Fallback safeguard: If files are missing, default back to ProggyClean safely
            if (uiFont == nullptr)  uiFont  = io.Fonts->AddFontDefault();
            if (osdFont == nullptr) osdFont = io.Fonts->AddFontDefault();            
            if (subtitleFont == nullptr) subtitleFont = io.Fonts->AddFontDefault();            

            // Setup Platform/Renderer Backends
            ImGui_ImplSDL3_InitForSDLRenderer(state->window.get(), state->renderer.get());
            ImGui_ImplSDLRenderer3_Init(state->renderer.get());
        }

        void DrawTextWithOutline(ImDrawList* draw_list, ImFont* font, float font_size, ImVec2 screen_pos, const char* text, ImU32 text_color, ImU32 outline_color, float stroke_thickness) {
            // Draw the 8-directional shadow offset boundary
            for (float x = -stroke_thickness; x <= stroke_thickness; x += stroke_thickness) {
                for (float y = -stroke_thickness; y <= stroke_thickness; y += stroke_thickness) {
                    if (x == 0.0f && y == 0.0f) continue; // Skip the exact center
                    
                    draw_list->AddText(font, font_size, ImVec2(screen_pos.x + x, screen_pos.y + y), outline_color, text);
                }
            }

            // Overlay the pristine main text inside the middle slot
            draw_list->AddText(font, font_size, screen_pos, text_color, text);
        }        

        void DrawTextWithShadow(ImDrawList* draw_list, ImFont* font, float font_size, ImVec2 screen_pos, const char* text, ImU32 text_color, ImU32 outline_color, float stroke_thickness, float wrap_width = -1.0f) {
            draw_list->AddText(font, font_size, ImVec2(screen_pos.x + stroke_thickness, screen_pos.y + stroke_thickness), outline_color, text, nullptr, wrap_width);
            draw_list->AddText(font, font_size, screen_pos, text_color, text, nullptr, wrap_width);
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
                ImDrawList* draw_list = ImGui::GetForegroundDrawList();
                ImVec2 osd_pos = ImVec2(20.0f, 20.0f); // Top left screen alignment
//                ImGui::Text(noti_text.c_str());
                ImGui::PushFont(osdFont);
                DrawTextWithShadow(
                    draw_list, 
                    ImGui::GetFont(), 
                    ImGui::GetFontSize(), // Scaled slightly larger for OSD
                    osd_pos, 
                    noti_text.c_str(), 
                    IM_COL32(255, 255, 255, 255), // Pure White Text
                    IM_COL32(0, 0, 0, 200),       // Soft Transparent Black Outline
                    1.8f                          // outline stroke thickness
                );
                ImGui::PopFont();
            }

            if (subtitle_expires_at > curr_ticks)
            {
                ImDrawList* draw_list = ImGui::GetBackgroundDrawList();
                ImGui::PushFont(subtitleFont);
                ImVec2 text_size = ImGui::CalcTextSize(subtitle.c_str(), NULL, false, io.DisplaySize.x * 0.9);
                ImVec2 pos = ImVec2(
                        (io.DisplaySize.x - text_size.x) * 0.5f,           // center X
                        io.DisplaySize.y - text_size.y - io.DisplaySize.y * 0.08             // bottom with padding
                    );                

                DrawTextWithShadow(
                    draw_list, 
                    ImGui::GetFont(), 
                    ImGui::GetFontSize(),
                    pos, 
                    subtitle.c_str(), 
                    IM_COL32(255, 255, 255, 255),
                    IM_COL32(0, 0, 0, 200),
                    2, text_size.x
                );
                ImGui::PopFont();
            }

            ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5, io.DisplaySize.y * 0.9), 0, ImVec2(0.5, 0.5));
            auto size = ImVec2(io.DisplaySize.x * 0.9, io.DisplaySize.y * 0.1);
            ImGui::SetNextWindowSize(size);
            ImGui::Begin("Slider", nullptr,
                        ImGuiWindowFlags_NoDecoration |
                        ImGuiWindowFlags_NoSavedSettings |
                        ImGuiWindowFlags_NoBackground |
                        ImGuiWindowFlags_NoFocusOnAppearing |
                        ImGuiWindowFlags_NoNav |
                        ImGuiWindowFlags_NoMove);
            if (ImGui::IsWindowHovered()) {
                auto play_time = state->get_play_time();
                auto duration = state->video.get_duration();
                float v = play_time / duration;
                ImGui::SetNextItemWidth(-1.0f);
                if (ImGui::SliderFloat("##Seek", &v, 0.0f, 1.0f, std::format("{:.0f} / {:.0f}", play_time, duration - play_time).c_str())) {
                    state->seek_ratio(v);
                }
            }
            ImGui::End();

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

        void show_subtitle(const std::string &message, uint64_t duration_ms = 2000)
        {
            subtitle = message;
            subtitle_expires_at = SDL_GetTicks() + duration_ms;
        }

    private:
        AppState *state = nullptr;
        std::string noti_text;
        uint64_t text_expires_at = 0; // Expiration time in SDL ticks (milliseconds)
        std::string subtitle;
        uint64_t subtitle_expires_at = 0;
        uint64_t slider_expires_at = 0;
        ImFont* osdFont = nullptr;
        ImFont* uiFont  = nullptr;
        ImFont* subtitleFont  = nullptr;
};