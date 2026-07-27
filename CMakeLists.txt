cmake_minimum_required(VERSION 3.16)
project(autohdr-vk VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

find_package(Vulkan REQUIRED)
find_program(GLSLANG_VALIDATOR glslangValidator REQUIRED)

set(SHADER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/shaders)
set(SHADER_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${SHADER_GEN_DIR})
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/implicit_layer.d)

set(SHADER_HEADERS "")

function(compile_shader NAME VAR_NAME)
    set(SRC ${SHADER_DIR}/${NAME})
    set(OUT ${SHADER_GEN_DIR}/${NAME}.spv.h)

    add_custom_command(
        OUTPUT ${OUT}
        COMMAND ${GLSLANG_VALIDATOR}
            -V
            --target-env vulkan1.2
            -I${SHADER_DIR}
            ${SRC}
            -o ${OUT}
            -x
            --vn ${VAR_NAME}
        DEPENDS ${SRC} ${SHADER_DIR}/tonemap_color.glsl
        COMMENT "Compiling ${NAME}"
        VERBATIM
    )
    set(SHADER_HEADERS ${SHADER_HEADERS} ${OUT} PARENT_SCOPE)
endfunction()

compile_shader(tonemap.vert tonemap_vert_spv)
compile_shader(tonemap.frag tonemap_frag_spv)

add_library(VkLayer_AUTOHDR_tonemap SHARED
    layer/entry.cpp
    layer/pipeline.cpp
    layer/config.cpp
    common/tone_curve.cpp
    common/tone_curve_presets.cpp
    ${SHADER_HEADERS}
)

target_include_directories(VkLayer_AUTOHDR_tonemap
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/layer
        ${CMAKE_CURRENT_SOURCE_DIR}/common
        ${SHADER_GEN_DIR}
        ${Vulkan_INCLUDE_DIRS}
)

target_link_libraries(VkLayer_AUTOHDR_tonemap PRIVATE Vulkan::Vulkan)

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
