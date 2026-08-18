cmake_minimum_required(VERSION 3.16)
project(autohdr-vk VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

find_package(Vulkan REQUIRED)
find_program(GLSLANG_VALIDATOR glslangValidator REQUIRED)

set(SHADER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/shaders)
set(SHADER_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${SHADER_GEN_DIR})
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/implicit_layer.d)

set(BLUE_NOISE_ATLAS_H ${SHADER_GEN_DIR}/blue_noise_atlas.h)

add_executable(bake_blue_noise tools/bake_blue_noise.cpp)

add_custom_command(
    OUTPUT ${BLUE_NOISE_ATLAS_H}
    COMMAND bake_blue_noise ${BLUE_NOISE_ATLAS_H}
    DEPENDS bake_blue_noise
    COMMENT "Baking blue-noise atlas"
    VERBATIM
)

add_custom_target(blue_noise_assets DEPENDS ${BLUE_NOISE_ATLAS_H})

set(OVERLAY_FONT_ATLAS_H ${SHADER_GEN_DIR}/overlay_font_atlas.h)
set(OVERLAY_FONT_METRICS_GLSL ${SHADER_GEN_DIR}/overlay_font_metrics.glsl)

add_executable(bake_overlay_font tools/bake_overlay_font.cpp)
target_include_directories(bake_overlay_font PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party)

add_custom_command(
    OUTPUT ${OVERLAY_FONT_ATLAS_H} ${OVERLAY_FONT_METRICS_GLSL}
    COMMAND bake_overlay_font
        ${CMAKE_CURRENT_SOURCE_DIR}/assets/DejaVuSans.ttf
        ${OVERLAY_FONT_ATLAS_H}
        ${OVERLAY_FONT_METRICS_GLSL}
    DEPENDS bake_overlay_font ${CMAKE_CURRENT_SOURCE_DIR}/assets/DejaVuSans.ttf
    COMMENT "Baking overlay font atlas"
    VERBATIM
)

add_custom_target(overlay_font_assets DEPENDS ${OVERLAY_FONT_ATLAS_H} ${OVERLAY_FONT_METRICS_GLSL})

set(SHADER_HEADERS "")

function(compile_shader NAME VAR_NAME)
    set(SRC ${SHADER_DIR}/${NAME})
    set(OUT ${SHADER_GEN_DIR}/${NAME}.spv.h)
    set(EXTRA_DEPS ${ARGN})

    add_custom_command(
        OUTPUT ${OUT}
        COMMAND ${GLSLANG_VALIDATOR}
            -V
            --target-env vulkan1.2
            -I${SHADER_DIR}
            -I${SHADER_GEN_DIR}
            ${SRC}
            -o ${OUT}
            -x
            --vn ${VAR_NAME}
        DEPENDS ${SRC}
            ${SHADER_DIR}/tonemap_color.glsl
            ${SHADER_DIR}/ictcp.glsl
            ${SHADER_DIR}/bt2446.glsl
            ${EXTRA_DEPS}
        COMMENT "Compiling ${NAME}"
        VERBATIM
    )
    set(SHADER_HEADERS ${SHADER_HEADERS} ${OUT} PARENT_SCOPE)
endfunction()

compile_shader(tonemap.comp tonemap_comp_spv ${SHADER_DIR}/deband.glsl ${SHADER_DIR}/tonemap_dither.glsl)
compile_shader(histogram_pass1.comp histogram_pass1_comp_spv)
compile_shader(histogram_pass2.comp histogram_pass2_comp_spv)
compile_shader(ui_cluster.comp ui_cluster_comp_spv)
compile_shader(base_blur.comp base_blur_comp_spv)
compile_shader(overlay.comp overlay_comp_spv ${SHADER_DIR}/overlay_font.glsl ${OVERLAY_FONT_METRICS_GLSL})
compile_shader(present_rgb10.comp present_rgb10_comp_spv ${SHADER_DIR}/dither.glsl)
compile_shader(present_unorm.comp present_unorm_comp_spv ${SHADER_DIR}/dither.glsl)

add_library(VkLayer_AUTOHDR_tonemap SHARED
    layer/entry.cpp
    layer/pipeline.cpp
    layer/config.cpp
    layer/input.cpp
    layer/input_x11.cpp
    layer/input_wayland.cpp
    layer/runtime_ui.cpp
    common/tone_curve.cpp
    common/tone_curve_presets.cpp
    ${SHADER_HEADERS}
    ${OVERLAY_FONT_ATLAS_H}
    ${BLUE_NOISE_ATLAS_H}
)

add_dependencies(VkLayer_AUTOHDR_tonemap overlay_font_assets blue_noise_assets)

find_package(X11 REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(WAYLAND_CLIENT REQUIRED wayland-client)
pkg_check_modules(XKBCOMMON REQUIRED xkbcommon)
pkg_check_modules(WAYLAND_PROTOCOLS REQUIRED wayland-protocols)
find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)

pkg_get_variable(WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)
set(RELATIVE_POINTER_XML
    "${WAYLAND_PROTOCOLS_DIR}/unstable/relative-pointer/relative-pointer-unstable-v1.xml")
set(RELATIVE_POINTER_CLIENT_H
    "${SHADER_GEN_DIR}/relative-pointer-unstable-v1-client-protocol.h")
set(RELATIVE_POINTER_CLIENT_C
    "${SHADER_GEN_DIR}/relative-pointer-unstable-v1-protocol.c")

add_custom_command(
    OUTPUT ${RELATIVE_POINTER_CLIENT_H}
    COMMAND ${WAYLAND_SCANNER} client-header ${RELATIVE_POINTER_XML} ${RELATIVE_POINTER_CLIENT_H}
    DEPENDS ${RELATIVE_POINTER_XML}
    COMMENT "Generating relative-pointer client header"
    VERBATIM
)
add_custom_command(
    OUTPUT ${RELATIVE_POINTER_CLIENT_C}
    COMMAND ${WAYLAND_SCANNER} private-code ${RELATIVE_POINTER_XML} ${RELATIVE_POINTER_CLIENT_C}
    DEPENDS ${RELATIVE_POINTER_XML}
    COMMENT "Generating relative-pointer protocol code"
    VERBATIM
)

target_sources(VkLayer_AUTOHDR_tonemap PRIVATE
    ${RELATIVE_POINTER_CLIENT_C}
    ${RELATIVE_POINTER_CLIENT_H}
)

target_include_directories(VkLayer_AUTOHDR_tonemap
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/layer
        ${CMAKE_CURRENT_SOURCE_DIR}/common
        ${SHADER_GEN_DIR}
        ${Vulkan_INCLUDE_DIRS}
        ${WAYLAND_CLIENT_INCLUDE_DIRS}
        ${XKBCOMMON_INCLUDE_DIRS}
)

target_link_libraries(VkLayer_AUTOHDR_tonemap PRIVATE
    Vulkan::Vulkan
    X11::X11
    ${WAYLAND_CLIENT_LIBRARIES}
    ${XKBCOMMON_LIBRARIES}
)

if(UNIX AND NOT APPLE)
    target_link_options(VkLayer_AUTOHDR_tonemap PRIVATE
        "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/layer/exports.map"
    )
endif()

set_target_properties(VkLayer_AUTOHDR_tonemap PROPERTIES
    PREFIX "lib"
    OUTPUT_NAME "VkLayer_AUTOHDR_tonemap"
)

add_executable(autohdr-vk-cli tools/autohdr-vk-cli.cpp layer/config.cpp
    common/tone_curve.cpp common/tone_curve_presets.cpp)
target_include_directories(autohdr-vk-cli PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/layer
    ${CMAKE_CURRENT_SOURCE_DIR}/common
    ${Vulkan_INCLUDE_DIRS})

include(GNUInstallDirs)

if(NOT DEFINED AUTOHDR_LAYER_LIBRARY_PATH OR AUTOHDR_LAYER_LIBRARY_PATH STREQUAL "")
    set(AUTOHDR_LAYER_LIBRARY_PATH
        "${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR}/libVkLayer_AUTOHDR_tonemap.so")
endif()

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/layer/VkLayer_AUTOHDR_tonemap.json.in
    ${CMAKE_CURRENT_BINARY_DIR}/VkLayer_AUTOHDR_tonemap.json
    @ONLY
)

file(GENERATE
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/implicit_layer.d/VkLayer_AUTOHDR_tonemap.json
    CONTENT
"{
  \"file_format_version\": \"1.0.0\",
  \"layer\": {
    \"name\": \"VK_LAYER_AUTOHDR_tonemap\",
    \"type\": \"GLOBAL\",
    \"library_path\": \"$<TARGET_FILE:VkLayer_AUTOHDR_tonemap>\",
    \"api_version\": \"1.4.0\",
    \"implementation_version\": \"1\",
    \"description\": \"AutoHDR SDR to HDR tone mapping at present time\",
    \"functions\": {
      \"vkGetInstanceProcAddr\": \"vkGetInstanceProcAddr\",
      \"vkGetDeviceProcAddr\": \"vkGetDeviceProcAddr\"
    },
    \"enable_environment\": {
      \"ENABLE_AUTOHDR\": \"1\"
    },
    \"disable_environment\": {
      \"DISABLE_AUTOHDR\": \"1\"
    }
  }
}
"
)

install(TARGETS VkLayer_AUTOHDR_tonemap
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(TARGETS autohdr-vk-cli
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/VkLayer_AUTOHDR_tonemap.json
    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/vulkan/implicit_layer.d)
install(FILES conf.example.toml
    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/autohdr-vk)
