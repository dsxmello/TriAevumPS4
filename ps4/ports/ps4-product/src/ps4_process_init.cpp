// TriAevum PS4 product - process start, before oot3d_native_game's main does
// any work (fast/backends/gfx_orbis.h OrbisProcessInit).

#include "fast/backends/gfx_orbis.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <SDL2/SDL.h>
#include <orbis/Sysmodule.h>
#include <orbis/libkernel.h>

// SDL's PS4 joystick driver calls the module loader of SDL's PS4 video
// driver, which this build leaves out (no Piglet): OrbisProcessInit has
// already loaded every module SDL needs.
extern "C" void PS4_LoadModules(void) {}

#if defined(TRIAEVUM_PS4_PGO_GENERATE)
extern "C" void TriaevumPs4StartProfileWriter(void);  // ps4_pgo_runtime.c
#endif

// DolphinPS4 runtime (ps4_sampler.cpp).
extern "C" void ps4_sampler_register_thread(const char* name);
extern "C" void ps4_sampler_start();

// libkernel; orbis/libkernel.h declares it without parameters.
extern "C" int TriaevumPthreadSetaffinity(OrbisPthread thread, uint64_t mask) __asm__(
    "scePthreadSetaffinity");


namespace Fast {

void OrbisProcessInit() {
    // stdout/stderr carry the boot log (spdlog's console sink and every
    // printf diagnostic), the evidence channel since phase 1.
    const int bootLog = open("/data/triaevum_boot.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (bootLog >= 0) {
        dup2(bootLog, STDOUT_FILENO);
        dup2(bootLog, STDERR_FILENO);
        if (bootLog > STDERR_FILENO) close(bootLog);
    }
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    std::printf("[triaevum] TriAevum PS4 product (oot3d_native_game) - build %s\n",
                TRIAEVUM_PS4_BUILD_VERSION);

    // Calling into a system library whose module is not loaded kills the
    // process (PRX_NOT_RESOLVED_FUNCTION).
    sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_SYSTEM_SERVICE);
    sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_USER_SERVICE);
    sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_VIDEO_OUT);
    sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_PAD);
    sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_AUDIOOUT);

    for (const char* directory :
         {"/data/triaevum", "/data/triaevum/config", "/data/triaevum/savedata",
          "/data/triaevum/mods", "/data/triaevum/mods/topscreen", "/data/triaevum/logs"}) {
        mkdir(directory, 0777);
    }
    // Relative paths (logs/, ./...) resolve against this directory
    // (ps4_paths.cpp; the kernel itself only takes absolute paths).
    if (chdir("/data/triaevum") != 0) {
        std::printf("[triaevum] chdir(/data/triaevum) failed\n");
    }

    // SDL's PS4 joystick driver names the pad "Sony DualShock 4 V2" (its
    // GUID is the first 16 bytes of the name) and maps OPTIONS to back and
    // the touch-pad click to start. Give the game controller the console's
    // convention: OPTIONS is start, the touch pad is back (3DS SELECT).
    SDL_SetHint(SDL_HINT_GAMECONTROLLERCONFIG,
                "536f6e79204475616c53686f636b2034,PS4 Controller,"
                "a:b0,b:b1,x:b2,y:b3,start:b4,guide:b5,back:b6,leftstick:b7,"
                "rightstick:b8,leftshoulder:b9,rightshoulder:b10,dpup:b11,"
                "dpdown:b12,dpleft:b13,dpright:b14,leftx:a0,lefty:a1,rightx:a2,"
                "righty:a3,lefttrigger:a4,righttrigger:a5,");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");

    // The game (guest code, AOT) runs on this thread.
    OrbisRegisterThread("main");
#if defined(TRIAEVUM_PS4_PGO_GENERATE)
    TriaevumPs4StartProfileWriter();
#endif
}

void OrbisRegisterThread(const char* name) {
    // The Jaguar CPU is two clusters of four cores, each with its own L2;
    // a line passed between clusters costs a trip through memory. The game
    // thread hands every frame's plans to the render worker, and the plan
    // helpers hand theirs to the game thread, so all of them stay on the
    // first cluster (Dusklight keeps its main, FIFO and render threads
    // there; DolphinPS4 pins its CPU and video threads to cores 2 and 3).
    // Off unless /data/triaevum/affinity exists: on the console (1.21) the
    // pinned game thread ran worse than letting the system place it, so the
    // system keeps the placement by default.
    static const bool pin = access("/data/triaevum/affinity", F_OK) == 0;
    if (pin) {
        uint64_t mask = 0;
        if (std::strcmp(name, "main") == 0) {
            mask = 1ULL << 2;
        } else if (std::strcmp(name, "render") == 0) {
            mask = 1ULL << 3;
        } else if (std::strcmp(name, "pica plan") == 0) {
            mask = (1ULL << 0) | (1ULL << 1);
        }
        if (mask != 0) {
            const int result = TriaevumPthreadSetaffinity(scePthreadSelf(), mask);
            std::printf("[triaevum] thread '%s': core mask 0x%llx (result 0x%x)\n", name,
                        static_cast<unsigned long long>(mask), result);
        }
    }
    static const bool profile = access("/data/triaevum/profile", F_OK) == 0;
    if (!profile) return;
    ps4_sampler_register_thread(name);
    ps4_sampler_start();
    std::printf("[triaevum] profiler: sampling thread '%s' -> /data/DolphinPS4/samples.log\n", name);
}

} // namespace Fast
