cmake_minimum_required(VERSION 3.20)
project(prisma
    VERSION 1.0.0
    DESCRIPTION "Controle de cor em hardware para GPUs AMD no Linux"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)
option(PRISMA_BUILD_LEGACY_DRM "Build the experimental DRM LD_PRELOAD library" OFF)
option(PRISMA_BUILD_LEGACY_VULKAN "Build the experimental Vulkan layer and shader" OFF)

add_library(prisma_cli STATIC
    src/cli/device.cpp
    src/cli/telemetry.cpp
    src/cli/profiles.cpp
    src/cli/doctor.cpp
)
target_include_directories(prisma_cli PUBLIC src kernel)

add_executable(prisma-ctl src/cli/main.cpp)
target_link_libraries(prisma-ctl PRIVATE prisma_cli)

include(GNUInstallDirs)
install(TARGETS prisma-ctl RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")

if(BUILD_TESTING)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_test(NAME check_ui COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_ui.py)

    find_package(Threads REQUIRED)
    add_executable(test_cli tests/test_cli.cpp)
    add_dependencies(test_cli prisma-ctl)
    target_link_libraries(test_cli PRIVATE prisma_cli Threads::Threads)
    target_compile_options(test_cli PRIVATE -UNDEBUG)
    add_test(NAME test_cli COMMAND test_cli)

    add_executable(test_color_math tests/test_color_math.cpp)
    target_compile_options(test_color_math PRIVATE -UNDEBUG)
    add_test(NAME test_color_math COMMAND test_color_math)

    add_executable(test_kernel_abi EXCLUDE_FROM_ALL tests/test_kernel_abi.cpp)
    target_include_directories(test_kernel_abi PRIVATE kernel)
    target_link_libraries(test_kernel_abi PRIVATE Threads::Threads)
endif()

set(LOCAL_BIN_DIR "$ENV{HOME}/.local/bin")
set(LOCAL_LIB_DIR "$ENV{HOME}/.local/lib")
set(LOCAL_LAYER_DIR "$ENV{HOME}/.local/share/vulkan/implicit_layer.d")

add_custom_target(install-local
    COMMAND ${CMAKE_COMMAND} -E make_directory "${LOCAL_BIN_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:prisma-ctl>" "${LOCAL_BIN_DIR}/prisma-ctl"
    DEPENDS prisma-ctl
    VERBATIM
)
add_custom_target(uninstall-local
    COMMAND ${CMAKE_COMMAND} -E rm -f "${LOCAL_BIN_DIR}/prisma-ctl"
    VERBATIM
)

if(PRISMA_BUILD_LEGACY_DRM)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(DRM REQUIRED IMPORTED_TARGET libdrm)

    add_library(chroma_drm SHARED legacy/drm_hook.cpp legacy/drm_ctm.cpp)
    target_include_directories(chroma_drm PRIVATE legacy)
    target_link_libraries(chroma_drm PRIVATE PkgConfig::DRM ${CMAKE_DL_LIBS})

    if(BUILD_TESTING)
        add_executable(test_ctm tests/test_ctm.cpp legacy/drm_ctm.cpp)
        target_include_directories(test_ctm PRIVATE legacy)
        target_link_libraries(test_ctm PRIVATE PkgConfig::DRM)
        target_compile_options(test_ctm PRIVATE -UNDEBUG)
        add_test(NAME test_ctm COMMAND test_ctm)

        add_executable(test_vkms_ctm EXCLUDE_FROM_ALL tests/test_vkms_ctm.cpp)
        target_include_directories(test_vkms_ctm PRIVATE legacy)
        target_link_libraries(test_vkms_ctm PRIVATE PkgConfig::DRM)
    endif()

    add_custom_target(install-legacy-drm
        COMMAND ${CMAKE_COMMAND} -E make_directory "${LOCAL_LIB_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:chroma_drm>" "${LOCAL_LIB_DIR}/libchroma_drm.so"
        DEPENDS chroma_drm
        VERBATIM
    )
    add_dependencies(install-local install-legacy-drm)
    add_custom_command(TARGET uninstall-local POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E rm -f "${LOCAL_LIB_DIR}/libchroma_drm.so"
        VERBATIM
    )
endif()

if(PRISMA_BUILD_LEGACY_VULKAN)
    find_path(VULKAN_INCLUDE_DIR vulkan/vulkan.h REQUIRED)
    find_program(GLSLANG_VALIDATOR NAMES glslangValidator)
    find_program(GLSLC NAMES glslc)
    set(SHADER_SRC "${CMAKE_CURRENT_SOURCE_DIR}/legacy/shader.comp")
    set(SHADER_SPV "${CMAKE_CURRENT_BINARY_DIR}/shader.spv")
    set(SHADER_H "${CMAKE_CURRENT_BINARY_DIR}/shader_spv.h")
    set(BIN2C "${CMAKE_CURRENT_SOURCE_DIR}/cmake/bin2c.cmake")

    if(GLSLANG_VALIDATOR)
        set(SHADER_COMPILER "${GLSLANG_VALIDATOR}" -V)
    elseif(GLSLC)
        set(SHADER_COMPILER "${GLSLC}")
    else()
        message(FATAL_ERROR "The Vulkan layer requires glslangValidator or glslc.")
    endif()

    add_custom_command(
        OUTPUT "${SHADER_SPV}"
        COMMAND ${SHADER_COMPILER} "${SHADER_SRC}" -o "${SHADER_SPV}"
        DEPENDS "${SHADER_SRC}"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${SHADER_H}"
        COMMAND "${CMAKE_COMMAND}" "-DINPUT_FILE=${SHADER_SPV}" "-DOUTPUT_FILE=${SHADER_H}" -P "${BIN2C}"
        DEPENDS "${SHADER_SPV}" "${BIN2C}"
        VERBATIM
    )
    add_custom_target(compile_shaders DEPENDS "${SHADER_SPV}" "${SHADER_H}")
    add_library(VkLayer_chroma SHARED legacy/layer.cpp "${SHADER_H}")
    add_dependencies(VkLayer_chroma compile_shaders)
    target_include_directories(VkLayer_chroma PRIVATE "${VULKAN_INCLUDE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}" legacy)

    set(CHROMA_LAYER_PATH "${LOCAL_LIB_DIR}/libVkLayer_chroma.so")
    configure_file(legacy/VkLayer_chroma.json.in VkLayer_chroma.json @ONLY)
    add_custom_target(install-legacy-vulkan
        COMMAND ${CMAKE_COMMAND} -E make_directory "${LOCAL_LIB_DIR}" "${LOCAL_LAYER_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:VkLayer_chroma>" "${CHROMA_LAYER_PATH}"
        COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_BINARY_DIR}/VkLayer_chroma.json" "${LOCAL_LAYER_DIR}/VkLayer_chroma.json"
        DEPENDS VkLayer_chroma
        VERBATIM
    )
    add_dependencies(install-local install-legacy-vulkan)
    add_custom_command(TARGET uninstall-local POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E rm -f "${CHROMA_LAYER_PATH}" "${LOCAL_LAYER_DIR}/VkLayer_chroma.json"
        VERBATIM
    )
endif()

find_program(NOCTALIA_BIN noctalia)
set(NOCTALIA_PLUGIN_DIR "$ENV{HOME}/.local/state/noctalia/plugins/materialized/deyvideguilherme/prisma")

if(NOCTALIA_BIN)
    add_custom_target(install-ui
        COMMAND "${NOCTALIA_BIN}" msg plugins source add prisma path "${CMAKE_CURRENT_SOURCE_DIR}/ui"
        COMMAND "${NOCTALIA_BIN}" msg plugins update prisma
        COMMAND "${NOCTALIA_BIN}" msg plugins enable deyvideguilherme/prisma
        VERBATIM
        COMMENT "Registering and enabling Prisma plugin in Noctalia Shell"
    )
    add_custom_target(uninstall-ui
        COMMAND "${NOCTALIA_BIN}" msg plugins disable deyvideguilherme/prisma
        COMMAND "${NOCTALIA_BIN}" msg plugins source remove prisma
        COMMAND ${CMAKE_COMMAND} -E rm -rf "${NOCTALIA_PLUGIN_DIR}"
        VERBATIM
        COMMENT "Disabling and removing Prisma plugin from Noctalia Shell"
    )
else()
    add_custom_target(install-ui
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/ui/prisma" "${NOCTALIA_PLUGIN_DIR}"
        VERBATIM
        COMMENT "Fallback: Noctalia CLI not found; copied plugin files to materialized directory"
    )
    add_custom_target(uninstall-ui
        COMMAND ${CMAKE_COMMAND} -E rm -rf "${NOCTALIA_PLUGIN_DIR}"
        VERBATIM
        COMMENT "Fallback: Removed materialized plugin directory"
    )
endif()
