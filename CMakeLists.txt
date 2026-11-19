cmake_minimum_required(VERSION 3.20)
project(prisma
    VERSION 1.0.0
    DESCRIPTION "Controle de cor em hardware para GPUs AMD no Linux"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Find libdrm via pkg-config for the DRM/KMS hardware backend
find_package(PkgConfig REQUIRED)
pkg_check_modules(DRM REQUIRED libdrm)

# Vulkan Headers (locate on system or fetch from official Khronos repository)
find_package(VulkanHeaders QUIET)
if(NOT VulkanHeaders_FOUND)
    find_package(Vulkan QUIET)
endif()

if(TARGET Vulkan::Headers)
    set(VULKAN_TARGET Vulkan::Headers)
elseif(TARGET Vulkan::Vulkan)
    set(VULKAN_TARGET Vulkan::Vulkan)
else()
    include(FetchContent)
    message(STATUS "Vulkan Headers not found locally. Downloading official Khronos headers...")
    set(VULKAN_HEADERS_ENABLE_MODULE OFF CACHE BOOL "" FORCE)
    set(VULKAN_HEADERS_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
    set(VULKAN_HEADERS_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
    set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
    FetchContent_Declare(
        VulkanHeaders
        GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
        GIT_TAG v1.3.296
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(VulkanHeaders)
    set(VULKAN_TARGET Vulkan::Headers)
endif()

# Locate SPIR-V shader compiler
find_program(GLSLANG_VALIDATOR NAMES glslangValidator)
find_program(GLSLC NAMES glslc)

set(SHADER_SRC "${CMAKE_CURRENT_SOURCE_DIR}/src/shader.comp")
set(SHADER_SPV "${CMAKE_CURRENT_BINARY_DIR}/shader.spv")
set(SHADER_H "${CMAKE_CURRENT_BINARY_DIR}/shader_spv.h")

if(GLSLANG_VALIDATOR)
    message(STATUS "Selected GLSL compiler: glslangValidator (${GLSLANG_VALIDATOR})")
    add_custom_command(
        OUTPUT "${SHADER_SPV}" "${SHADER_H}"
        COMMAND "${GLSLANG_VALIDATOR}" -V "${SHADER_SRC}" -o "${SHADER_SPV}"
        COMMAND "${GLSLANG_VALIDATOR}" -V --vn chroma_shader_spv "${SHADER_SRC}" -o "${SHADER_H}"
        DEPENDS "${SHADER_SRC}"
        COMMENT "Compiling compute shader (${SHADER_SRC}) -> SPIR-V and C++ header"
        VERBATIM
    )
elseif(GLSLC)
    message(STATUS "Selected GLSL compiler: glslc (${GLSLC})")
    add_custom_command(
        OUTPUT "${SHADER_SPV}"
        COMMAND "${GLSLC}" "${SHADER_SRC}" -o "${SHADER_SPV}"
        DEPENDS "${SHADER_SRC}"
        COMMENT "Compiling compute shader (${SHADER_SRC}) -> SPIR-V"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${SHADER_H}"
        COMMAND ${CMAKE_COMMAND}
            -DINPUT_FILE="${SHADER_SPV}"
            -DOUTPUT_FILE="${SHADER_H}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/bin2c.cmake"
        DEPENDS "${SHADER_SPV}"
        COMMENT "Generating C++ header from SPIR-V shader"
        VERBATIM
    )
else()
    message(FATAL_ERROR "No shader compiler found! Please install glslang (glslangValidator) or shaderc (glslc).")
endif()

# Target to force shader compilation
add_custom_target(compile_shaders DEPENDS "${SHADER_SPV}" "${SHADER_H}")

# ==============================================================================
# 1. Vulkan Implicit Layer (libVkLayer_chroma.so)
# ==============================================================================
add_library(VkLayer_chroma SHARED
    src/layer.cpp
    "${SHADER_H}"
)

add_dependencies(VkLayer_chroma compile_shaders)

target_include_directories(VkLayer_chroma PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_BINARY_DIR}"
)

target_link_libraries(VkLayer_chroma PRIVATE
    ${VULKAN_TARGET}
)

set_target_properties(VkLayer_chroma PROPERTIES
    OUTPUT_NAME "VkLayer_chroma"
    PREFIX "lib"
)

# ==============================================================================
# 2. Interceptor Library (libchroma_drm.so) for DRM/KMS Hardware CTM
# ==============================================================================
add_library(chroma_drm SHARED
    src/drm_hook.cpp
    src/drm_ctm.cpp
)

target_include_directories(chroma_drm PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    ${DRM_INCLUDE_DIRS}
)

target_link_libraries(chroma_drm PRIVATE
    ${DRM_LIBRARIES}
    ${CMAKE_DL_LIBS}
)

set_target_properties(chroma_drm PROPERTIES
    OUTPUT_NAME "chroma_drm"
    PREFIX "lib"
)

# ==============================================================================
# 3. Unit Tests & Verification Binaries (test_ctm, test_cli)
# ==============================================================================
add_executable(test_ctm
    tests/test_ctm.cpp
    src/drm_ctm.cpp
)

target_include_directories(test_ctm PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    ${DRM_INCLUDE_DIRS}
)

target_link_libraries(test_ctm PRIVATE
    ${DRM_LIBRARIES}
)

# CLI & Telemetry Unit Tests
add_executable(test_cli
    tests/test_cli.cpp
    src/cli/device.cpp
    src/cli/telemetry.cpp
    src/cli/profiles.cpp
)

target_include_directories(test_cli PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

enable_testing()
add_test(NAME test_ctm COMMAND test_ctm)
add_test(NAME test_cli COMMAND test_cli)

# ==============================================================================
# 4. CLI Tool (prisma-ctl)
# ==============================================================================
add_executable(prisma-ctl
    src/cli/main.cpp
    src/cli/device.cpp
    src/cli/telemetry.cpp
    src/cli/profiles.cpp
)

target_include_directories(prisma-ctl PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

# ==============================================================================
# User Local Installation
# ==============================================================================
set(LOCAL_BIN_DIR "$ENV{HOME}/.local/bin")
set(LOCAL_LIB_DIR "$ENV{HOME}/.local/lib")
set(LOCAL_LAYER_DIR "$ENV{HOME}/.local/share/vulkan/implicit_layer.d")
set(PRISMA_CTL_PATH "${LOCAL_BIN_DIR}/prisma-ctl")
set(CHROMA_LAYER_PATH "${LOCAL_LIB_DIR}/libVkLayer_chroma.so")
set(CHROMA_DRM_PATH "${LOCAL_LIB_DIR}/libchroma_drm.so")
set(CHROMA_JSON_PATH "${LOCAL_LAYER_DIR}/VkLayer_chroma.json")

# Configure JSON manifest with absolute path to installed library
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/VkLayer_chroma.json.in"
    "${CMAKE_CURRENT_BINARY_DIR}/VkLayer_chroma.json"
    @ONLY
)

# Target: install-local
add_custom_target(install-local
    COMMAND ${CMAKE_COMMAND} -E make_directory "${LOCAL_BIN_DIR}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${LOCAL_LIB_DIR}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${LOCAL_LAYER_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:prisma-ctl>" "${PRISMA_CTL_PATH}"
    COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:VkLayer_chroma>" "${CHROMA_LAYER_PATH}"
    COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:chroma_drm>" "${CHROMA_DRM_PATH}"
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_BINARY_DIR}/VkLayer_chroma.json" "${CHROMA_JSON_PATH}"
    DEPENDS VkLayer_chroma chroma_drm prisma-ctl
    COMMENT "Installing prisma-ctl to ${LOCAL_BIN_DIR}, libraries to ${LOCAL_LIB_DIR} and manifest to ${CHROMA_JSON_PATH}"
    VERBATIM
)

# Target: uninstall-local
add_custom_target(uninstall-local
    COMMAND ${CMAKE_COMMAND} -E remove -f "${PRISMA_CTL_PATH}"
    COMMAND ${CMAKE_COMMAND} -E remove -f "${CHROMA_LAYER_PATH}"
    COMMAND ${CMAKE_COMMAND} -E remove -f "${CHROMA_DRM_PATH}"
    COMMAND ${CMAKE_COMMAND} -E remove -f "${CHROMA_JSON_PATH}"
    COMMENT "Removing local installation of prisma binaries, libraries and manifest"
    VERBATIM
)

# ==============================================================================
# 5. Noctalia Shell Plugin (Prisma Control Center)
# ==============================================================================
set(NOCTALIA_PLUGIN_DIR "$ENV{HOME}/.local/state/noctalia/plugins/materialized/deyvideguilherme/prisma")

# Target: install-ui
add_custom_target(install-ui
    COMMAND ${CMAKE_COMMAND} -E make_directory "${NOCTALIA_PLUGIN_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/ui/prisma" "${NOCTALIA_PLUGIN_DIR}"
    COMMENT "Installing Prisma Control Center plugin to ${NOCTALIA_PLUGIN_DIR}"
    VERBATIM
)

# Target: uninstall-ui
add_custom_target(uninstall-ui
    COMMAND ${CMAKE_COMMAND} -E remove_directory "${NOCTALIA_PLUGIN_DIR}"
    COMMENT "Removing Prisma Control Center plugin from ${NOCTALIA_PLUGIN_DIR}"
    VERBATIM
)
