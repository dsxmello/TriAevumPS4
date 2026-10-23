# TriAevum PS4 Vulkan probe: RADV (tools/build-mesa.sh) on GNM.
# Built by tools/build-vk-probe.sh; plain homebrew identity (4.5 GiB direct memory, no Piglet).
cmake_minimum_required(VERSION 3.13)
project(triaevum_ps4_vk_probe C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif ()

# tools/env.sh exports PS4_PORT_ROOT and DOLPHINPS4; make them CMake-visible.
if (DEFINED ENV{PS4_PORT_ROOT})
    set(PS4_PORT_ROOT $ENV{PS4_PORT_ROOT})
endif ()
if (DEFINED ENV{DOLPHINPS4})
    set(DOLPHINPS4 $ENV{DOLPHINPS4})
endif ()
if (NOT PS4_PORT_ROOT OR NOT DOLPHINPS4)
    message(FATAL_ERROR "source tools/env.sh first (PS4_PORT_ROOT/DOLPHINPS4 missing)")
endif ()

include(${DOLPHINPS4}/toolchain/ps4-love-style.cmake)

set(MESA_SRC "${PS4_PORT_ROOT}/src/mesa" CACHE PATH "Mesa source (headers)")

add_executable(vk_probe main.c ${PS4_RUNTIME_SOURCES})
target_include_directories(vk_probe PRIVATE ${MESA_SRC}/include)
target_compile_options(vk_probe PRIVATE -Wall -Wno-format)
ps4_link_mesa(vk_probe)  # RADV on GNM (DolphinPS4 toolchain/ps4-love-style.cmake)
target_link_libraries(vk_probe PRIVATE
                      z SceSysmodule SceSystemService SceUserService SceVideoOut pthread)
ps4_add_eboot(vk_probe)
