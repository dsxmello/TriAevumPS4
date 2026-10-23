# TriAevum PS4 (Vulkan stack) - Phase 2 game host.
# Boots the statically linked whole-AOT closure (256 generated shards + module
# + services) on the DolphinPS4 runtime (dlmalloc malloc-replace heap, crash
# log, libc++ 21) and renders it with the PICA Vulkan backend: RADV on GNM
# (tools/build-mesa.sh) + glslang for on-console GLSL -> SPIR-V
# (tools/build-glslang.sh). Built by tools/build-game.sh.
cmake_minimum_required(VERSION 3.13)
project(triaevum_ps4_vk_game C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)  # gnu++20, like the previous Makefile build

if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif ()

if (DEFINED ENV{PS4_PORT_ROOT})
    set(PS4_PORT_ROOT $ENV{PS4_PORT_ROOT})
endif ()
if (DEFINED ENV{DOLPHINPS4})
    set(DOLPHINPS4 $ENV{DOLPHINPS4})
endif ()
if (NOT PS4_PORT_ROOT OR NOT DOLPHINPS4)
    message(FATAL_ERROR "source tools/env.sh first (PS4_PORT_ROOT/DOLPHINPS4 missing)")
endif ()

# Local checkout of the TriAevum source and the generated AOT shards (private,
# derived from the user's ROM - never in any repository).
set(TRIAEVUM_ROOT "${PS4_PORT_ROOT}/../TriAevum" CACHE PATH "TriAevum source tree")
set(TITLE_CONTENT_AOT "${PS4_PORT_ROOT}/../title-content/aot-cpp" CACHE PATH "Generated whole-AOT shards")

include(${DOLPHINPS4}/toolchain/ps4-love-style.cmake)

# ----------------------------------------------------------------- sources --
set(NGR "${TRIAEVUM_ROOT}/tools/oot3d/native_game_runtime")
set(NGM "${TRIAEVUM_ROOT}/tools/oot3d/native_game_module")
set(A32 "${TRIAEVUM_ROOT}/tools/oot3d/native_a32_runtime")
set(NPF "${TRIAEVUM_ROOT}/tools/oot3d/native_pica_frontend")
set(AZAHAR_PICA "${TRIAEVUM_ROOT}/tools/oot3d/third_party/azahar_pica")
set(AZAHAR_AUDIO "${TRIAEVUM_ROOT}/tools/oot3d/third_party/azahar_audio")

file(GLOB SRC_MODULE CONFIGURE_DEPENDS
     "${TRIAEVUM_ROOT}/runtime/triaevum_module/src/*.cpp")
file(GLOB SRC_UICONTRACT CONFIGURE_DEPENDS
     "${TRIAEVUM_ROOT}/tools/oot3d/ui_contract/oot3d_ui/*.cpp")
file(GLOB SRC_GENERATED CONFIGURE_DEPENDS
     "${TITLE_CONTENT_AOT}/oot3d_whole_aot_generated.cpp"
     "${TITLE_CONTENT_AOT}/oot3d_whole_aot_shard_*.cpp")

set(SRC_INPUT
    "${TRIAEVUM_ROOT}/tools/three_ds/input/three_ds_input.cpp"
    "${TRIAEVUM_ROOT}/tools/three_ds/input/three_ds_motion_composition.cpp")

set(SRC_TYPED
    "${TRIAEVUM_ROOT}/tools/oot3d/typed_gameplay/oot3d_gameplay_actor.cpp"
    "${TRIAEVUM_ROOT}/tools/oot3d/typed_gameplay/oot3d_gameplay_animation.cpp"
    "${TRIAEVUM_ROOT}/tools/oot3d/typed_gameplay/oot3d_gameplay_camera.cpp"
    "${TRIAEVUM_ROOT}/tools/oot3d/typed_gameplay/oot3d_gameplay_camera_animation.cpp"
    "${TRIAEVUM_ROOT}/tools/oot3d/typed_gameplay/oot3d_gameplay_cutscene.cpp"
    "${TRIAEVUM_ROOT}/tools/oot3d/typed_gameplay/oot3d_gameplay_math.cpp"
    "${TRIAEVUM_ROOT}/tools/oot3d/typed_gameplay/oot3d_gameplay_player.cpp"
    "${TRIAEVUM_ROOT}/tools/oot3d/typed_gameplay/oot3d_gameplay_quake.cpp"
    "${TRIAEVUM_ROOT}/tools/oot3d/typed_gameplay/oot3d_gameplay_skel_anime.cpp"
    "${TRIAEVUM_ROOT}/tools/oot3d/typed_gameplay/oot3d_gameplay_time.cpp")

set(SRC_UITOP
    "${TRIAEVUM_ROOT}/tools/oot3d/ui_topscreen/oot3d_native_font_coverage.cpp"
    "${TRIAEVUM_ROOT}/tools/oot3d/ui_topscreen/oot3d_top_screen_config.cpp"
    "${TRIAEVUM_ROOT}/tools/oot3d/ui_topscreen/oot3d_top_screen_controls.cpp"
    "${TRIAEVUM_ROOT}/tools/oot3d/ui_topscreen/oot3d_top_screen_gameplay_actions.cpp"
    "${TRIAEVUM_ROOT}/tools/oot3d/ui_topscreen/oot3d_top_screen_dpad_presentation.cpp"
    "${TRIAEVUM_ROOT}/tools/oot3d/ui_topscreen/oot3d_top_screen_items_hint.cpp"
    "${TRIAEVUM_ROOT}/tools/oot3d/ui_topscreen/oot3d_top_screen_camera.cpp"
    "${TRIAEVUM_ROOT}/tools/oot3d/ui_topscreen/oot3d_top_screen_camera_guest.cpp"
    "${TRIAEVUM_ROOT}/tools/oot3d/ui_topscreen/oot3d_top_screen_item_guest.cpp"
    "${TRIAEVUM_ROOT}/tools/oot3d/ui_topscreen/oot3d_top_screen_item_dispatch.cpp"
    "${TRIAEVUM_ROOT}/tools/oot3d/ui_topscreen/oot3d_top_screen_mod_profile.cpp"
    "${TRIAEVUM_ROOT}/tools/oot3d/ui_topscreen/oot3d_top_screen_mod_profile_items.cpp"
    "${TRIAEVUM_ROOT}/tools/oot3d/ui_topscreen/oot3d_top_screen_mod_profile_hud.cpp"
    "${TRIAEVUM_ROOT}/tools/oot3d/ui_topscreen/oot3d_top_screen_mod_profile_quest.cpp"
    "${TRIAEVUM_ROOT}/tools/oot3d/ui_topscreen/oot3d_top_screen_mod_profile_pause.cpp"
    "${TRIAEVUM_ROOT}/tools/oot3d/ui_topscreen/oot3d_top_screen_ocarina.cpp"
    "${TRIAEVUM_ROOT}/tools/oot3d/ui_topscreen/oot3d_top_screen_texture_overrides.cpp")

set(SRC_AUDIO
    "${AZAHAR_AUDIO}/audio_core/codec.cpp"
    "${AZAHAR_AUDIO}/audio_core/interpolate.cpp"
    "${AZAHAR_AUDIO}/audio_core/hle/filter.cpp"
    "${AZAHAR_AUDIO}/audio_core/hle/mixers.cpp"
    "${AZAHAR_AUDIO}/audio_core/hle/source.cpp")

set(SRC_CTRHOST
    "${NGR}/oot3d_native_a32_ctr_host.cpp"
    "${NGR}/oot3d_native_a32_ctr_host_system.cpp"
    "${NGR}/oot3d_native_a32_ctr_host_hid.cpp"
    "${NGR}/oot3d_native_a32_ctr_host_gsp.cpp"
    "${NGR}/oot3d_native_a32_ctr_host_dsp.cpp"
    "${NGR}/oot3d_native_a32_ctr_host_fs.cpp"
    "${NGR}/oot3d_native_a32_ctr_host_state.cpp"
    "${NGR}/oot3d_native_a32_dsp_hle.cpp"
    "${NGR}/oot3d_native_a32_dsp_memory_adapter.cpp")

set(SRC_PROCESS
    "${NGR}/oot3d_native_a32_memory.cpp"
    "${NGR}/oot3d_native_a32_process.cpp"
    "${NGR}/oot3d_native_a32_registry.cpp"
    "${NGR}/oot3d_native_owner_call_adapter.cpp"
    "${NGR}/oot3d_native_compiled_functions.cpp"
    "${NGR}/oot3d_native_mass_aot.cpp"
    "${NGR}/oot3d_typed_actor_lifecycle.cpp"
    "${NGR}/oot3d_typed_actor_update_records.cpp"
    "${NGR}/oot3d_typed_audio_request_callback.cpp"
    "${NGR}/oot3d_typed_dyna_interaction_reset.cpp"
    "${NGR}/oot3d_typed_game_state.cpp"
    "${NGR}/oot3d_typed_pause_ui_alpha.cpp"
    "${NGR}/oot3d_typed_player_lock_on.cpp"
    "${NGR}/oot3d_typed_record_initializer.cpp"
    "${NGR}/oot3d_typed_gameplay_bridge.cpp"
    "${NGR}/oot3d_native_true_aot_blocks.cpp"
    "${NGR}/oot3d_native_whole_aot_runtime.cpp"
    "${NGR}/oot3d_memory_bus_abi.cpp")

set(SRC_IMAGE "${NGR}/oot3d_native_a32_process_image.cpp")

set(SRC_BRIDGE
    "${NGR}/triaevum_oot3d_pica_client_bridge.cpp"
    "${NGR}/triaevum_ctr_filesystem_client_bridge.cpp"
    "${NGR}/triaevum_forge_content_filesystem.cpp"
    "${NPF}/oot3d_native_pica_transfer.cpp")

set(SRC_GAMEMOD
    "${NGM}/oot3d_game_module_runtime.cpp"
    "${NGM}/oot3d_game_module_ui_profile.cpp"
    "${NGM}/oot3d_game_module_exports.cpp")

set(SRC_PICAFRONTEND
    "${NPF}/oot3d_native_pica_draw_state.cpp"
    "${NPF}/oot3d_native_pica_fragment_lighting_gen.cpp"
    "${NPF}/oot3d_native_pica_fragment_shader_gen.cpp"
    "${NPF}/oot3d_native_pica_frontend.cpp"
    "${NPF}/oot3d_native_pica_proctex.cpp"
    "${NPF}/oot3d_native_pica_program_descriptor.cpp"
    "${NPF}/oot3d_native_pica_raw_draw_record.cpp"
    "${NPF}/oot3d_native_pica_shader_gen.cpp"
    "${NPF}/oot3d_native_vertex_binaries.cpp"
    "${AZAHAR_PICA}/video_core/shader/generator/glsl_shader_decompiler.cpp")

set(SRC_PICAHOST
    "${NGR}/triaevum_oot3d_pica_host.cpp"
    "${NGR}/triaevum_oot3d_pica_service.cpp"
    "${NGR}/triaevum_oot3d_pica_memory.cpp"
    "${NGR}/triaevum_oot3d_pica_batch.cpp"
    "${NGR}/oot3d_native_pica_submission.cpp"
    "${NGR}/oot3d_native_pica_a32_memory_view.cpp"
    "${NGR}/triaevum_oot3d_pica_renderer.cpp"
    "${NGR}/oot3d_native_pica_vulkan_bridge.cpp"
    "${NGR}/oot3d_native_pica_vulkan_plan.cpp")

set(SRC_RENDERER
    "${TRIAEVUM_ROOT}/runtime/three_ds_recomp/src/fast/oot3d/pica_fragment_lighting.cpp"
    "${TRIAEVUM_ROOT}/runtime/three_ds_recomp/src/fast/renderer3ds/pica_tev_program.cpp"
    "${TRIAEVUM_ROOT}/runtime/three_ds_recomp/src/fast/oot3d/pica_texture_decode.cpp")

set(SRC_PLUGIN "${NGR}/triaevum_title_whole_aot_plugin.cpp")

# Architectural support shared with the generated shards; exact FP like the
# Makefile (a32_vfp_binary64/transport + vfp_ops under -fno-fast-math
# -ffp-contract=off). oot3d_native_a32_memory_bus_defaults.cpp is skipped on
# purpose: oot3d_memory_bus_abi.cpp already provides the identical MemoryBus
# base-class defaults in this static link.
set(SRC_SUPPORT
    "${A32}/upstream/recomp/a32_vfp_binary64.cpp"
    "${A32}/oot3d_native_a32_vfp_ops.cpp"
    "${A32}/upstream/recomp/a32_vfp_transport.cpp")

add_executable(triaevum_game
    src/ps4_vk_host.cpp
    src/ps4_vk_pica_backend.cpp
    ${PS4_RUNTIME_SOURCES}
    ${SRC_MODULE} ${SRC_INPUT} ${SRC_TYPED} ${SRC_UICONTRACT} ${SRC_UITOP}
    ${SRC_AUDIO} ${SRC_CTRHOST} ${SRC_PROCESS} ${SRC_IMAGE} ${SRC_BRIDGE}
    ${SRC_GAMEMOD} ${SRC_PICAFRONTEND} ${SRC_PICAHOST} ${SRC_RENDERER}
    ${SRC_PLUGIN} ${SRC_SUPPORT} ${SRC_GENERATED})

target_include_directories(triaevum_game SYSTEM PRIVATE
    third_party/compat
    third_party)
target_include_directories(triaevum_game PRIVATE
    "${TRIAEVUM_ROOT}/runtime/triaevum_module/include"
    "${TRIAEVUM_ROOT}/tools/oot3d/native_a32_runtime"
    "${TRIAEVUM_ROOT}/tools/oot3d/native_a32_runtime/upstream"
    "${TRIAEVUM_ROOT}/tools/oot3d/native_a32_runtime/upstream/recomp"
    "${TRIAEVUM_ROOT}/tools/oot3d/native_a32_runtime/mass_cpp"
    "${TRIAEVUM_ROOT}/tools/oot3d/native_game_runtime"
    "${TRIAEVUM_ROOT}/tools/oot3d/native_game_module"
    "${NPF}"
    "${AZAHAR_PICA}"
    "${TRIAEVUM_ROOT}/tools/oot3d/ui_topscreen"
    "${TRIAEVUM_ROOT}/tools/oot3d/ui_contract"
    "${TRIAEVUM_ROOT}/tools/oot3d/typed_gameplay"
    "${TRIAEVUM_ROOT}/tools/three_ds/input"
    "${AZAHAR_AUDIO}"
    "${TRIAEVUM_ROOT}/runtime/three_ds_recomp/include"
    "${TITLE_CONTENT_AOT}")

# The vendored Azahar subtrees each carry a trimmed common/ (assert.h,
# common_types.h, ...). azahar_pica's variants come first on the target's
# include path (the shader decompiler wants them), but the audio_core TUs -
# and the DSP HLE host that consumes audio_core - include
# "common/assert.h" / "common/common_types.h" that must resolve to
# azahar_audio's own (the pica subsets lack PAddr and the UNREACHABLE_MSG
# family). Prepend azahar_audio for exactly those files - the per-file
# INCLUDE_DIRECTORIES are searched before the target's.
set(AZAHAR_AUDIO_CONSUMERS ${SRC_AUDIO}
    "${NGR}/oot3d_native_a32_dsp_hle.cpp")
set_source_files_properties(${AZAHAR_AUDIO_CONSUMERS} PROPERTIES
    INCLUDE_DIRECTORIES "${AZAHAR_AUDIO}")

# Port marker: sources guarded by TRIAEVUM_PS4_HOST keep their exact desktop
# behavior everywhere else. (With libc++ 21 this is the whole compatibility
# story: bit_cast, concepts and the floating-point ::abs overloads all exist,
# so the old force-included ps4_compat.h shims are gone.)
target_compile_definitions(triaevum_game PRIVATE
    TRIAEVUM_PS4_HOST=1
    TRIAEVUM_PS4_BUILD_VERSION="0.94")
# Whole-AOT product mode groups.
set_source_files_properties(${SRC_PROCESS} ${SRC_GENERATED} ${SRC_PLUGIN}
    PROPERTIES COMPILE_DEFINITIONS
    "OOT3D_NATIVE_GENERATED_WHOLE_AOT=1;OOT3D_WHOLE_AOT_PRODUCT_MODE=1")
set_source_files_properties(${SRC_CTRHOST}
    PROPERTIES COMPILE_DEFINITIONS "OOT3D_CTR_HOST_SERVICE_PICA_ONLY=1")
set_source_files_properties(${SRC_SUPPORT}
    PROPERTIES COMPILE_OPTIONS "-fno-fast-math;-ffp-contract=off")

# Vulkan renderer: Mesa's headers for vulkan_core.h, glslang (static,
# tools/build-glslang.sh installs it into the sysroot) and RADV itself.
set(MESA_SRC "${PS4_PORT_ROOT}/src/mesa" CACHE PATH "Mesa source (headers)")
set(GLSLANG_PREFIX "$ENV{PS4_SYSROOT}/glslang" CACHE PATH "glslang PS4 install")
set_source_files_properties(src/ps4_vk_pica_backend.cpp PROPERTIES
    INCLUDE_DIRECTORIES "${MESA_SRC}/include;${GLSLANG_PREFIX}/include")
set_source_files_properties(src/ps4_vk_host.cpp PROPERTIES
    INCLUDE_DIRECTORIES "${MESA_SRC}/include")
ps4_link_mesa(triaevum_game)
target_link_libraries(triaevum_game PRIVATE
    "${GLSLANG_PREFIX}/lib/libglslang.a"
    "${GLSLANG_PREFIX}/lib/libglslang-default-resource-limits.a"
    "${GLSLANG_PREFIX}/lib/libglslang.a")
target_link_libraries(triaevum_game PRIVATE
    z SceSysmodule SceSystemService SceUserService SceVideoOut ScePad pthread)
ps4_add_eboot(triaevum_game)
