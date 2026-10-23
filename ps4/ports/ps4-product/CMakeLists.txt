# TriAevum PS4 - Phase 3: the complete product (oot3d_native_game: the window
# session with TopScreen, the F1 settings GUI, free camera...) on the
# Vulkan stack, following the Nintendo Switch homebrew product
# (docs/OOT3D_SWITCH_WHOLE_AOT_PORT.md) with this port's Vulkan backend in
# place of the Switch's SDL2/OpenGL one.
#
# The TriAevum part of the closure is NOT listed by hand: tools/product/
# closure.py extracts it from a host configure of TriAevum's own CMake and
# tools/product/gen_cmake.py turns it into generated/product_sources.cmake
# (one OBJECT library per TriAevum target, with its own definitions and
# includes). Built by tools/build-product.sh.
cmake_minimum_required(VERSION 3.18)
project(triaevum_ps4_product C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)
set(CMAKE_C_STANDARD 17)
if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif ()

foreach (var PS4_PORT_ROOT DOLPHINPS4 PS4_SYSROOT)
    if (DEFINED ENV{${var}})
        set(${var} $ENV{${var}})
    endif ()
    if (NOT ${var})
        message(FATAL_ERROR "source tools/env.sh first (${var} missing)")
    endif ()
endforeach ()

set(TRIAEVUM_ROOT "${PS4_PORT_ROOT}/../TriAevum" CACHE PATH "TriAevum source tree")
set(TITLE_CONTENT_AOT "${PS4_PORT_ROOT}/../title-content/aot-cpp" CACHE PATH
    "Generated whole-AOT shards (private, derived from the user's ROM)")
set(PS4_DEPS "${PS4_PORT_ROOT}/src/deps")
set(PS4_SDL2 "${PS4_SYSROOT}/sdl2")
set(PS4_PRODUCT_GENERATED "${CMAKE_CURRENT_SOURCE_DIR}/generated")
set(SPDLOG_ROOT "${PS4_PORT_ROOT}/src/spdlog-1.14.1")

include(${DOLPHINPS4}/toolchain/ps4-love-style.cmake)

# Every TU of the product sees the port marker, the PS4 SDL2, spdlog as a
# compiled library and the third-party headers of this build root. fmt 10
# (bundled with spdlog) trips clang's consteval checks; its runtime-checked
# format strings are what the desktop gets with MSVC anyway.
set(TRIAEVUM_PS4_BUILD_VERSION "1.23")
add_compile_definitions(TRIAEVUM_PS4_HOST=1 SPDLOG_COMPILED_LIB=1 FMT_CONSTEVAL=)
include_directories(SYSTEM
    "${SPDLOG_ROOT}/include"
    "${PS4_SDL2}/include"
    "${PS4_SDL2}/include/SDL2"
    "${PS4_PORT_ROOT}/ports/ps4-vk-game/third_party"
    "${PS4_PORT_ROOT}/src/mesa/include"
    "${PS4_SYSROOT}/glslang/include"
    "${PS4_PORT_ROOT}/src/tinyxml2-10.0.0"
    "${PS4_SYSROOT}/libzip/include")

# Profile-guided optimization (tools/build-product.sh PGO=generate|use): the
# game thread is CPU-bound in thousands of generated AOT functions, the
# kind of branchy code PGO lays out best. `generate` instruments every TU and
# writes /data/triaevum/pgo/product.profraw while playing; `use` rebuilds
# from the merged .profdata.
set(PS4_PGO "" CACHE STRING "Profile-guided optimization: generate | use")
set(PS4_PGO_PROFILE "" CACHE FILEPATH "Merged .profdata (PS4_PGO=use)")
if (PS4_PGO STREQUAL "generate")
    add_compile_options(-fprofile-generate)
    add_compile_definitions(TRIAEVUM_PS4_PGO_GENERATE=1)
    # The profile sections are orphans the OpenOrbis script places past the
    # data segment's file image: on the console they start as zeros (1.19:
    # every record without name, hash or counters). Inside .data they keep
    # their contents; the runtime's __start_/__stop_ bounds come from here.
    file(READ "${PS4_LINK_SCRIPT}" PS4_PGO_LINK_SCRIPT)
    set(PS4_PGO_DATA_RULE "*(.data .data.*)")
    foreach (section __llvm_prf_cnts __llvm_prf_bits __llvm_prf_vals __llvm_prf_data __llvm_prf_vnds)
        string(APPEND PS4_PGO_DATA_RULE
            "\n\t\t. = ALIGN(32); __start_${section} = .; KEEP(*(${section})) __stop_${section} = .;")
    endforeach ()
    string(REPLACE "*(.data .data.*)" "${PS4_PGO_DATA_RULE}" PS4_PGO_LINK_SCRIPT "${PS4_PGO_LINK_SCRIPT}")
    file(WRITE "${PS4_LINK_SCRIPT}" "${PS4_PGO_LINK_SCRIPT}")
elseif (PS4_PGO STREQUAL "use")
    add_compile_options("-fprofile-use=${PS4_PGO_PROFILE}"
        -Wno-profile-instr-unprofiled -Wno-profile-instr-out-of-date -Wno-profile-instr-missing)
elseif (NOT PS4_PGO STREQUAL "")
    message(FATAL_ERROR "PS4_PGO must be generate, use or empty")
endif ()

include(generated/product_sources.cmake)
# zstd's optional tracing hooks are weak undefined symbols; create-fself
# rejects any import without a module.
target_compile_definitions(tri_libzstd_static PRIVATE ZSTD_TRACE=0)

# spdlog (TriAevum pins v1.14.1; tools/build-product.sh verifies the tarball).
add_library(ps4_spdlog OBJECT
    "${SPDLOG_ROOT}/src/spdlog.cpp"
    "${SPDLOG_ROOT}/src/stdout_sinks.cpp"
    "${SPDLOG_ROOT}/src/color_sinks.cpp"
    "${SPDLOG_ROOT}/src/file_sinks.cpp"
    "${SPDLOG_ROOT}/src/async.cpp"
    "${SPDLOG_ROOT}/src/cfg.cpp"
    "${SPDLOG_ROOT}/src/bundled_fmtlib_format.cpp")

# The title: 256 generated whole-AOT shards + the plugin query, linked
# statically (the Switch path of oot3d_native_direct_aot.cpp calls
# triaevum_title_whole_aot_query directly). Exact floating point, as the
# desktop plugin (-ffp-model=strict).
file(GLOB TITLE_SOURCES CONFIGURE_DEPENDS
     "${TITLE_CONTENT_AOT}/oot3d_whole_aot_generated.cpp"
     "${TITLE_CONTENT_AOT}/oot3d_whole_aot_shard_*.cpp")
add_library(ps4_title OBJECT ${TITLE_SOURCES}
    "${TRIAEVUM_ROOT}/tools/oot3d/native_game_runtime/triaevum_title_whole_aot_plugin.cpp")
target_compile_definitions(ps4_title PRIVATE OOT3D_NATIVE_GENERATED_WHOLE_AOT=1
    OOT3D_WHOLE_AOT_PRODUCT_MODE=1
    # In the desktop product these live in the separate title DLL and the
    # host's oot3d_native_direct_aot.cpp defines the same names as forwarders
    # over the plugin query; linked into one eboot, the title's copies get
    # their own names (reached only through triaevum_title_whole_aot_query).
    Oot3dWholeAotEntryPoints=TitleOot3dWholeAotEntryPoints
    ExecuteOot3dWholeAotFunction=TitleExecuteOot3dWholeAotFunction)
target_compile_options(ps4_title PRIVATE -fno-fast-math -ffp-contract=off)
target_include_directories(ps4_title PRIVATE
    "${TITLE_CONTENT_AOT}"
    "${TRIAEVUM_ROOT}/tools/oot3d/native_game_runtime"
    "${TRIAEVUM_ROOT}/tools/oot3d/native_a32_runtime"
    "${TRIAEVUM_ROOT}/tools/oot3d/native_a32_runtime/upstream"
    "${TRIAEVUM_ROOT}/tools/oot3d/native_a32_runtime/upstream/recomp"
    "${TRIAEVUM_ROOT}/tools/oot3d/native_a32_runtime/mass_cpp"
    "${TRIAEVUM_ROOT}/runtime/triaevum_module/include")

# The PS4 platform layer (fast/backends/gfx_orbis.h): VideoOut, the Fast3D
# window backend, the ImGui platform side and the rendering API, plus the
# Vulkan PICA backend shared with the module host (ports/ps4-vk-game). They
# compile against the product target's own definitions and includes.
set(PS4_VK_GAME_SRC "${PS4_PORT_ROOT}/ports/ps4-vk-game/src")
add_library(ps4_platform OBJECT
    src/ps4_display.cpp
    src/ps4_process_init.cpp
    src/ps4_paths.cpp
    src/ps4_throw_trace.cpp
    src/gfx_orbis_window.cpp
    src/orbis_imgui_platform.cpp
    src/gfx_orbis_rendering.cpp
    "${PS4_VK_GAME_SRC}/ps4_vk_pica_backend.cpp"
    "${PS4_DEPS}/imgui/backends/imgui_impl_vulkan.cpp")
target_compile_definitions(ps4_platform PRIVATE ${tri_oot3d_native_game_DEFINES}
    TRIAEVUM_PS4_BUILD_VERSION="${TRIAEVUM_PS4_BUILD_VERSION}"
    IMGUI_IMPL_VULKAN_NO_PROTOTYPES=1)
target_include_directories(ps4_platform PRIVATE
    src "${PS4_VK_GAME_SRC}"
    ${tri_oot3d_native_game_INCLUDES}
    ${tri_three_ds_recomp_runtime_INCLUDES}
    "${PS4_DEPS}/imgui" "${PS4_DEPS}/imgui/backends")
# Every TU that includes imgui_impl_vulkan.h must agree on the prototypes.
set_source_files_properties(
    "${TRIAEVUM_ROOT}/runtime/three_ds_recomp/src/fast/Fast3dGui.cpp"
    TARGET_DIRECTORY tri_three_ds_recomp_runtime
    PROPERTIES COMPILE_OPTIONS "-DIMGUI_IMPL_VULKAN_NO_PROTOTYPES=1")

# Sources the desktop product only links with its Vulkan/NRI renderer on, yet
# referenced from the closure's common code (the host configure used for the
# closure had Vulkan off, so it never had to link): the diffuse-GI pass
# planner and the NRI fragment-shader contract are portable code.
file(GLOB PS4_DIFFUSE_GI_SOURCES
     "${TRIAEVUM_ROOT}/runtime/three_ds_recomp/src/fast/renderer/diffuse_gi_*.cpp")
list(FILTER PS4_DIFFUSE_GI_SOURCES EXCLUDE REGEX "diffuse_gi_pass\\.cpp$")  # Vulkan executor
add_library(ps4_closure_extra OBJECT
    ${PS4_DIFFUSE_GI_SOURCES}
    "${TRIAEVUM_ROOT}/runtime/three_ds_recomp/src/fast/renderer3ds/pica_nri_shader_contract.cpp"
    # tinyxml2 10.0.0 (TriAevum's pin), one translation unit.
    "${PS4_PORT_ROOT}/src/tinyxml2-10.0.0/tinyxml2.cpp")
target_compile_definitions(ps4_closure_extra PRIVATE ${tri_three_ds_recomp_runtime_DEFINES})
target_include_directories(ps4_closure_extra PRIVATE ${tri_three_ds_recomp_runtime_INCLUDES})

set(PRODUCT_OBJECTS)
foreach (lib ${TRIAEVUM_PRODUCT_OBJECT_LIBRARIES} ps4_spdlog ps4_title ps4_platform
        ps4_closure_extra)
    list(APPEND PRODUCT_OBJECTS $<TARGET_OBJECTS:${lib}>)
endforeach ()
add_executable(triaevum_product ${PRODUCT_OBJECTS} ${PS4_RUNTIME_SOURCES})
ps4_link_mesa(triaevum_product)
target_link_libraries(triaevum_product PRIVATE
    "${PS4_SYSROOT}/glslang/lib/libglslang.a"
    "${PS4_SYSROOT}/glslang/lib/libglslang-default-resource-limits.a"
    "${PS4_SYSROOT}/glslang/lib/libglslang.a"
    "${PS4_SDL2}/lib/libSDL2.a"
    "${PS4_SYSROOT}/libzip/lib/libzip.a"
    z SceSysmodule SceSystemService SceUserService SceVideoOut ScePad SceAudioOut pthread)
# The kernel's path calls only take absolute paths (src/ps4_paths.cpp).
target_link_options(triaevum_product PRIVATE
    "LINKER:--wrap=open,--wrap=stat,--wrap=lstat,--wrap=mkdir,--wrap=rmdir"
    "LINKER:--wrap=unlink,--wrap=rename,--wrap=access,--wrap=chmod,--wrap=truncate"
    "LINKER:--wrap=readlink,--wrap=link,--wrap=statvfs,--wrap=pathconf"
    "LINKER:--wrap=utimensat,--wrap=fchmodat"
    # Throw sites in the boot log (src/ps4_throw_trace.cpp).
    "LINKER:--wrap=__cxa_throw")
if (PS4_PGO STREQUAL "generate")
    # The host clang's (Linux) profile runtime; ps4_pgo_runtime.c supplies
    # the few glibc-only entry points it calls.
    execute_process(COMMAND ${CMAKE_CXX_COMPILER} -print-resource-dir
                    OUTPUT_VARIABLE PS4_CLANG_RESOURCE_DIR OUTPUT_STRIP_TRAILING_WHITESPACE)
    target_sources(triaevum_product PRIVATE src/ps4_pgo_runtime.c)
    target_link_libraries(triaevum_product PRIVATE
        "${PS4_CLANG_RESOURCE_DIR}/lib/linux/libclang_rt.profile-x86_64.a")
endif ()
ps4_add_eboot(triaevum_product)
