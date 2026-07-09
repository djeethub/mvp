cmake_minimum_required(VERSION 3.24)
project(main LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIGURATION>")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIGURATION>")

find_package(PkgConfig REQUIRED)
pkg_check_modules(AVCODEC REQUIRED libavcodec libavformat libswresample libavutil libswscale)

add_subdirectory(vendor/SDL EXCLUDE_FROM_ALL)

# Define ImGui static source group
set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/vendor/imgui)
set(IMGUI_SRCS
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_sdl3.cpp
    ${IMGUI_DIR}/backends/imgui_impl_sdlrenderer3.cpp
)

# Create a static library target named 'my_imgui' entirely inside this scope
add_library(my_imgui STATIC ${IMGUI_SRCS})
set_target_properties(my_imgui PROPERTIES EXCLUDE_FROM_ALL TRUE)

# Configure includes for the 'my_imgui' target safely
target_include_directories(my_imgui PUBLIC 
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
)
target_include_directories(my_imgui PRIVATE 
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor/SDL/include
)

add_executable(main
    src/main.cpp
)

target_compile_definitions(main PRIVATE SDL_MAIN_USE_CALLBACKS=1)

# Main project only needs to know about SDL headers (ImGui headers come automatically via link)
target_include_directories(main PRIVATE 
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor/SDL/include
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor/imgui
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor/imgui/backends
    ${AVCODEC_INCLUDE_DIRS}
)

target_link_libraries(main PRIVATE
    SDL3::SDL3
    my_imgui
    ${AVCODEC_LIBRARIES}
)
