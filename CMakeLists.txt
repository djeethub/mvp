cmake_minimum_required(VERSION 3.24)
project(main LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIGURATION>")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIGURATION>")

find_package(PkgConfig REQUIRED)
find_package(SDL3 REQUIRED)
pkg_check_modules(AVCODEC REQUIRED libavcodec libavformat libswresample libavutil libswscale)
pkg_check_modules(LIBASS REQUIRED IMPORTED_TARGET libass)
#find_package(Fontconfig REQUIRED)

# Define ImGui static source group
set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/vendor/imgui)
set(IMGUI_SRCS
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_sdl3.cpp
    ${IMGUI_DIR}/backends/imgui_impl_sdlgpu3.cpp
)
# Create a static library target named 'my_imgui' entirely inside this scope
add_library(my_imgui STATIC ${IMGUI_SRCS})
set_target_properties(my_imgui PROPERTIES EXCLUDE_FROM_ALL TRUE)

# Shaders
set(SHADER_SOURCES
    shaders/vert.vert
    shaders/ass.vert
    shaders/ass.frag
    shaders/rgb.frag
    shaders/nv12.frag
    shaders/yuv.frag
    shaders/yuv_10.frag
    shaders/gray.frag
    shaders/pal8.frag
)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(GLSLC_FLAGS "-g")
else()
    set(GLSLC_FLAGS "-O")
endif()

set(SPIRV_HEADER_FILES "")

foreach(SHADER_SRC ${SHADER_SOURCES})
    get_filename_component(FILENAME ${SHADER_SRC} NAME)
    set(SPV_OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/shaders/${FILENAME}.spv")
    set(HEADER_OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/shaders/${FILENAME}.h")

    # Ensure output directory exists
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/shaders")

    add_custom_command(
        OUTPUT ${SPV_OUTPUT}
        COMMAND glslc ${CMAKE_CURRENT_SOURCE_DIR}/${SHADER_SRC} -o ${SPV_OUTPUT}
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${SHADER_SRC}
        COMMENT "Compiling shader ${FILENAME} -> ${FILENAME}.spv"
        VERBATIM
    )

    add_custom_command(
        OUTPUT ${HEADER_OUTPUT}
        COMMAND xxd -n ${FILENAME} -i ${SPV_OUTPUT} > ${HEADER_OUTPUT}
        DEPENDS ${SPV_OUTPUT}
        COMMENT "Embedding ${SPV_FILE} → ${HEADER_OUTPUT}"
        VERBATIM
    )    

    list(APPEND SPIRV_HEADER_FILES ${HEADER_OUTPUT})
endforeach()

add_custom_target(CompileShaders DEPENDS ${SPIRV_HEADER_FILES})

# FetchContent
include(FetchContent)

FetchContent_Declare(
  readerwriterqueue
  GIT_REPOSITORY    https://github.com/cameron314/readerwriterqueue
  GIT_TAG           master
)
FetchContent_MakeAvailable(readerwriterqueue)
FetchContent_GetProperties(readerwriterqueue SOURCE_DIR readerwriterqueue_dir)

FetchContent_Declare(
  concurrentqueue
  GIT_REPOSITORY    https://github.com/cameron314/concurrentqueue
  GIT_TAG           master
)
FetchContent_MakeAvailable(concurrentqueue)
FetchContent_GetProperties(concurrentqueue SOURCE_DIR concurrentqueue_dir)

# Configure includes for the 'my_imgui' target safely
target_include_directories(my_imgui PUBLIC 
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
)

add_executable(main
    src/main.cpp
)
add_dependencies(main CompileShaders)

target_compile_definitions(main PRIVATE SDL_MAIN_USE_CALLBACKS=1)

# Main project only needs to know about SDL headers (ImGui headers come automatically via link)
target_include_directories(main PRIVATE 
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor/imgui
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor/imgui/backends
    ${AVCODEC_INCLUDE_DIRS}
    ${CMAKE_CURRENT_BINARY_DIR}/shaders
    ${readerwriterqueue_dir}
    ${concurrentqueue_dir}
)

target_link_libraries(main PRIVATE
    SDL3::SDL3
    my_imgui
    ${AVCODEC_LIBRARIES}
    ${LIBASS_LIBRARIES}
#    Fontconfig::Fontconfig
)

message(STATUS "CMake Search Paths: ${CMAKE_PREFIX_PATH}")