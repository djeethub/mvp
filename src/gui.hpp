#pragma once

#include <string>
#include <format>

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlgpu3.h>

//#include <fontconfig/fontconfig.h>

#include "appstate.hpp"

class AppGui {
    public:
        AppGui() = default;

        void shutdown() {
            if (state) {
                state = nullptr;
                ImGui_ImplSDLGPU3_Shutdown();
                ImGui_ImplSDL3_Shutdown();
                ImGui::DestroyContext();
            }
        }
/*
        static std::string search_font_linux(const char* family_name, const char* style_name) {
            std::string out_path;

            // 1. Initialize Fontconfig
            FcConfig* config = FcInitLoadConfigAndFonts();

            // 2. Build a pattern describing what you want
            FcPattern* pattern = FcPatternBuild(
                nullptr, 
                FC_FAMILY, FcTypeString, family_name,
                FC_STYLE, FcTypeString, style_name,
                (char*)0
            );

            // 3. Perform standard config substitution (enables aliases & system fallbacks)
            FcConfigSubstitute(config, pattern, FcMatchPattern);
            FcDefaultSubstitute(pattern);

            // 4. Match against installed system fonts
            FcResult result;
            FcPattern* font = FcFontMatch(config, pattern, &result);

            if (font) {
                FcChar8* file = nullptr;
                FcChar8* matched_family = nullptr;

                // Extract font file path and matched family name
                if (FcPatternGetString(font, FC_FILE, 0, &file) == FcResultMatch &&
                    FcPatternGetString(font, FC_FAMILY, 0, &matched_family) == FcResultMatch) {
//                    std::cout << "Requested Family: " << family_name << "\n";
//                    std::cout << "Matched Family:   " << matched_family << "\n";
//                    std::cout << "Font Path:        " << file << "\n";
                    out_path = (char *) file;
                }
                FcPatternDestroy(font);
            } else {
                SDL_Log("No matching font found.\n");
            }

            // Cleanup
            FcPatternDestroy(pattern);
            FcConfigDestroy(config);
            FcFini();
            return out_path;
        }        
*/
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
/*            auto font_path = search_font_linux("Sans", "Regular");
            auto bold_font_path = search_font_linux("Sans", "Bold");
            if (!font_path.empty()) {
                uiFont  = io.Fonts->AddFontFromFileTTF(font_path.c_str(), 17.0f);
//                osdFont = io.Fonts->AddFontFromFileTTF(font_path.c_str(), 32.0f);
//                subtitleFont = io.Fonts->AddFontFromFileTTF(font_path.c_str(), 52.0f);
            }
            if (!bold_font_path.empty()) {
                osdFont = io.Fonts->AddFontFromFileTTF(bold_font_path.c_str(), 32.0f);
                subtitleFont = io.Fonts->AddFontFromFileTTF(bold_font_path.c_str(), 52.0f);
            } else if (!font_path.empty()) {
                osdFont = io.Fonts->AddFontFromFileTTF(font_path.c_str(), 32.0f);
                subtitleFont = io.Fonts->AddFontFromFileTTF(font_path.c_str(), 52.0f);
            }*/

            // 4. Fallback safeguard: If files are missing, default back to ProggyClean safely
            if (uiFont == nullptr)  uiFont  = io.Fonts->AddFontDefaultVector();
            if (osdFont == nullptr) osdFont = io.Fonts->AddFontDefaultVector();
//            if (subtitleFont == nullptr) subtitleFont = io.Fonts->AddFontDefault();            

            // Setup Platform/Renderer Backends
            ImGui_ImplSDL3_InitForSDLGPU(state->window);
            ImGui_ImplSDLGPU3_InitInfo init_info = {
                .Device = state->gpu.get_device(),
                .ColorTargetFormat = SDL_GetGPUSwapchainTextureFormat(state->gpu.get_device(), state->window),
                .MSAASamples = SDL_GPU_SAMPLECOUNT_1,
            };
            ImGui_ImplSDLGPU3_Init(&init_info);
        }

        static void DrawTextWithOutline(ImDrawList* draw_list, ImFont* font, float font_size, ImVec2 screen_pos, const char* text, ImU32 text_color, ImU32 outline_color, float stroke_thickness) {
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

        static void DrawTextWithShadow(ImDrawList* draw_list, ImFont* font, float font_size, ImVec2 screen_pos, const char* text, ImU32 text_color, ImU32 outline_color, float stroke_thickness, float wrap_width = -1.0f) {
            draw_list->AddText(font, font_size, ImVec2(screen_pos.x + stroke_thickness, screen_pos.y + stroke_thickness), outline_color, text, nullptr, wrap_width);
            draw_list->AddText(font, font_size, screen_pos, text_color, text, nullptr, wrap_width);
        }

        static std::string time_str(auto time) {
            auto n = static_cast<int>(time);
            auto m = n / 60;
            return m > 0 ? std::format("{}:{:02}", m, n % 60) : std::to_string(n);
        }

        std::string lang_string(const std::string& title, const std::string& lang) {
            if (lang.empty()) {
                return title;
            } else
                return std::format("{} ({})", title, lang);
        }

        SDL_AppResult draw()
        {
            auto curr_ticks = SDL_GetTicks();
            ImGuiIO &io = ImGui::GetIO();

            // Start ImGui Frame Rendering Chain
            ImGui_ImplSDLGPU3_NewFrame();
            ImGui_ImplSDL3_NewFrame();
            ImGui::NewFrame();

            if (text_expires_at > curr_ticks)
            {
                ImDrawList* draw_list = ImGui::GetForegroundDrawList();
                ImVec2 osd_pos = ImVec2(20.0f, 20.0f); // Top left screen alignment
//                ImGui::Text(noti_text.c_str());
                ImGui::PushFont(osdFont);
                DrawTextWithOutline(
                    draw_list, 
                    ImGui::GetFont(), 
                    ImGui::GetFontSize() * 1.5f, // Scaled slightly larger for OSD
                    osd_pos, 
                    noti_text.c_str(), 
                    IM_COL32(255, 255, 255, 255), // Pure White Text
                    IM_COL32(0, 0, 0, 200),       // Soft Transparent Black Outline
                    1.2f                          // outline stroke thickness
                );
                ImGui::PopFont();
            }
/*
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
            }*/

            ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5, io.DisplaySize.y * 0.9), 0, ImVec2(0.5, 0.5));
            auto size = ImVec2(io.DisplaySize.x * 0.92, io.DisplaySize.y * 0.1);
            ImGui::SetNextWindowSize(size);
            ImGui::Begin("Slider", nullptr,
                        ImGuiWindowFlags_NoDecoration |
                        ImGuiWindowFlags_NoSavedSettings |
                        ImGuiWindowFlags_NoBackground |
                        ImGuiWindowFlags_NoFocusOnAppearing |
                        ImGuiWindowFlags_NoNav |
                        ImGuiWindowFlags_NoMove);
            auto duration = state->video.get_duration();
            if (ImGui::IsWindowHovered() && duration > 0.05) {
                auto play_time = state->get_play_time();
                float v = play_time / duration;
                ImGui::SetNextItemWidth(-1.0f);
                if (ImGui::SliderFloat("##Seek", &v, 0.0f, 1.0f, std::format("{} / {}", time_str(play_time), time_str(duration - play_time)).c_str())) {
                    state->seek_ratio(v);
                }
            }
            ImGui::End();

            if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                ImGui::OpenPopup("mymenu");
            }
            if (ImGui::BeginPopup("mymenu"))
            {
/*                
                if (ImGui::MenuItem("Next File", ".", false, state->image_files.size() > 1))
                {
                    if (state->open_next_file(true))
                        show_noti(state->get_file_name());
                }
                if (ImGui::MenuItem("Previous File", ",", false, state->image_files.size() > 1))
                {
                    if (state->open_next_file(false))
                        show_noti(state->get_file_name());
                }*/
                if (ImGui::MenuItem(state->video.is_paused ? "Resume" : "Pause", "Space", false, true))
                {
                    state->pause();
                    show_noti(state->video.is_paused ? "Paused" : "Resumed");
                    if (state->video.is_paused) {
                        SDL_Event event{ .type = SDL_EVENT_FIRST };
                        SDL_PushEvent(&event);
                    }
                }
                if (ImGui::MenuItem("Minimize", "F9", false, true))
                {
                    SDL_MinimizeWindow(state->window);
                    state->pause(true);
                }
                if (ImGui::BeginMenu("Audio"))
                {
                    auto idx = state->video.get_audio_index();
                    if (ImGui::MenuItem("None", nullptr, idx < 0, true))
                    {
                        state->select_audio(-1);
                    }
                    ImGui::Separator();
                    auto tracks = state->video.get_audio_tracks();
                    for (const auto& data : tracks) {
                        if (ImGui::MenuItem(lang_string(data.title, data.lang).c_str(), nullptr, data.idx == idx, true))
                        {
                            state->select_audio(data.idx);
                        }
                    }
                    ImGui::EndMenu();
                }
                if (ImGui::BeginMenu("Subtitles"))
                {
                    auto idx = state->video.get_subtitle_index();
                    if (ImGui::MenuItem("None", nullptr, idx < 0, true))
                    {
                        state->select_subtitle(-1);
                    }
                    ImGui::Separator();
                    auto tracks = state->video.get_subtitle_tracks();
                    for (const auto& data : tracks) {
                        if (ImGui::MenuItem(lang_string(data.title, data.lang).c_str(), nullptr, data.idx == idx, true))
                        {
                            state->select_subtitle(data.idx);
                        }
                    }
                    ImGui::EndMenu();
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Open File Location", "Ctrl+L")) {
                    state->open_file_location();
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Exit", "Esc")) {
                    return SDL_APP_SUCCESS;
                }
                ImGui::EndPopup();
            }

            ImGui::Render();
//            ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), state->renderer.get());
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
//        ImFont* subtitleFont  = nullptr;
};