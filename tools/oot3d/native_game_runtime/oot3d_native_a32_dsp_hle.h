#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>
#include <nlohmann/json_fwd.hpp>

namespace Oot3dNativeGame {

class NativeA32Memory;

struct NativeA32DspPhysicalRegion {
    uint32_t PhysicalAddress = 0;
    uint32_t VirtualAddress = 0;
    size_t Size = 0;
};

struct NativeDspMemoryAccess {
    std::function<bool(uint32_t, std::span<uint8_t>)> Read;
    std::function<bool(uint32_t, std::span<const uint8_t>)> Write;
    std::function<const uint8_t*(uint32_t, size_t)> ResolvePhysical;
};

class NativeA32DspHle {
  public:
    static constexpr uint32_t NativeSampleRate = 32728;
    static constexpr uint32_t SamplesPerFrame = 160;

    explicit NativeA32DspHle(
        std::vector<NativeA32DspPhysicalRegion> physicalRegions);
    ~NativeA32DspHle();

    NativeA32DspHle(const NativeA32DspHle&) = delete;
    NativeA32DspHle& operator=(const NativeA32DspHle&) = delete;

    bool ProcessFrame(NativeA32Memory& memory,
                      std::vector<int16_t>& interleavedStereo,
                      std::string* error = nullptr);
    bool ProcessFrame(const NativeDspMemoryAccess& memory,
                      std::vector<int16_t>& interleavedStereo,
                      std::string* error = nullptr);

    // Starts a thread that ticks part of the sources of every frame while
    // ProcessFrame's caller ticks the rest (the sources are independent; the
    // output and the state are identical to ticking them all in order).
    // onStart runs first on the new thread. Call once, before ProcessFrame.
    void StartSourceWorker(void (*onStart)() = nullptr);

    // Wakes the worker ahead of a burst of ProcessFrame calls (it sleeps
    // between bursts). Without a worker it does nothing.
    void WakeSourceWorker();

    // Since the last call: sources ticked by the worker and by the caller,
    // and the caller's time waiting for the worker's sources.
    struct SourceTickCounts {
        uint64_t Worker = 0;
        uint64_t Caller = 0;
        uint64_t CallerWaitNanoseconds = 0;
    };
    SourceTickCounts TakeSourceTickCounts();

    nlohmann::json CaptureState() const;
    bool RestoreState(const nlohmann::json& state,
                      std::string* error = nullptr);

    // The emulated memory's physical-to-virtual map (also used by savestates
    // to place render targets in VRAM).
    std::span<const NativeA32DspPhysicalRegion> PhysicalRegions() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> mImpl;
};

} // namespace Oot3dNativeGame
