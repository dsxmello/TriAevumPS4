#include "oot3d_native_a32_dsp_hle.h"

#include "audio_core/hle/mixers.h"
#include "audio_core/hle/source.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <exception>
#include <limits>
#include <mutex>
#include <nlohmann/json.hpp>
#include <span>
#include <thread>
#include <utility>

namespace Oot3dNativeGame {
namespace {

constexpr uint32_t kDspRegion0Address = 0x1FF50000U;
constexpr uint32_t kDspRegion1Address = 0x1FF70000U;

void SetError(std::string* error, const char* message) {
    if (error != nullptr) {
        *error = message;
    }
}

size_t CurrentRegionIndex(const AudioCore::HLE::SharedMemory& region0,
                          const AudioCore::HLE::SharedMemory& region1) {
    const uint16_t frame0 = region0.frame_counter;
    const uint16_t frame1 = region1.frame_counter;
    if (frame0 == 0xFFFFU && frame1 != 0xFFFEU) {
        return 1;
    }
    if (frame1 == 0xFFFFU && frame0 != 0xFFFEU) {
        return 0;
    }
    return frame0 > frame1 ? 0U : 1U;
}

} // namespace

// With the source worker, the sources of a frame are shared out between the
// caller and the worker. Each source reads only its own configuration and
// writes only its own status and frame, and each one's contribution to the
// intermediate mixes is the same integer whichever thread adds it, so the
// result does not depend on who ticked what. Like the PICA plan helpers
// (1.63), this takes no lock and makes no system call per frame: a frame is
// published with one store of (generation << 8 | next position), positions
// are claimed with a compare-exchange, and the worker spins with yields
// during the bursts of frames. The positions list the sources that played in
// the previous frame (or were just enabled) first: those carry nearly all the
// work, and the two threads take them in turns. Between bursts the worker
// waits on a condition variable that the caller notifies once before each
// burst (WakeSourceWorker, ~60/s) or when it finds the worker asleep; the
// wait has a 1 ms timeout, so a lost notification (they get lost on the PS4)
// only delays it. Guest memory is resolved under a spinlock (a resolution can
// materialize deferred writes, which is not thread-safe).
struct NativeA32DspHle::Impl {
    using Mixes = std::array<AudioCore::QuadFrame32, 3>;
    static constexpr uint64_t kSourceBits = 8;
    static_assert(AudioCore::HLE::num_sources < (1U << kSourceBits));

    explicit Impl(std::vector<NativeA32DspPhysicalRegion> regions)
        : PhysicalRegions(std::move(regions)) {
        Sources.reserve(AudioCore::HLE::num_sources);
        for (size_t index = 0; index < AudioCore::HLE::num_sources; ++index) {
            Sources.emplace_back(index);
            Sources.back().SetMemoryResolver(
                [this](uint32_t address, size_t size) { return Resolve(address, size); });
        }
    }

    ~Impl() {
        if (Worker.joinable()) {
            Stop.store(true, std::memory_order_relaxed);
            Wake();
            Worker.join();
        }
    }

    void Wake() {
        {
            std::lock_guard lock(WakeMutex);
            ++WakeRequests;
        }
        WakeCondition.notify_one();
    }

    const uint8_t* Resolve(uint32_t address, size_t size) {
        if (CurrentMemory == nullptr || !CurrentMemory->ResolvePhysical) {
            return nullptr;
        }
        while (ResolveLock.test_and_set(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        const uint8_t* const memory = CurrentMemory->ResolvePhysical(address, size);
        ResolveLock.clear(std::memory_order_release);
        return memory;
    }

    void TickSource(size_t index, Mixes& mixes) {
        auto& source = Sources[index];
        JobWrite->source_statuses.status[index] =
            source.Tick(JobRead->source_configurations.config[index],
                        JobRead->adpcm_coefficients.coeff[index]);
        for (size_t mix = 0; mix < mixes.size(); ++mix) {
            source.MixInto(mixes[mix], mix);
        }
    }

    bool Claim(uint64_t generation, size_t& index) {
        uint64_t word = Next.load(std::memory_order_relaxed);
        for (;;) {
            const uint64_t source = word & ((1U << kSourceBits) - 1U);
            if ((word >> kSourceBits) != generation ||
                source >= AudioCore::HLE::num_sources) {
                return false;
            }
            if (Next.compare_exchange_weak(word, word + 1, std::memory_order_acquire,
                                           std::memory_order_relaxed)) {
                index = Order[static_cast<size_t>(source)];
                return true;
            }
        }
    }

    // Caller: ticks the sources the worker has not claimed, waits for the
    // ones it has, and adds the worker's mixes to its own.
    void TickSources(AudioCore::HLE::SharedMemory& read,
                     AudioCore::HLE::SharedMemory& write, Mixes& mixes) {
        const uint64_t generation = ++Generation;
        JobRead = &read;
        JobWrite = &write;
        size_t active = 0;
        size_t idle = AudioCore::HLE::num_sources;
        for (size_t source = 0; source < AudioCore::HLE::num_sources; ++source) {
            const auto& config = read.source_configurations.config[source];
            const bool playing = Playing[source] || (config.enable_dirty && config.enable != 0);
            Order[playing ? active++ : --idle] = static_cast<uint8_t>(source);
        }
        Finished.store(0, std::memory_order_relaxed);
        Next.store(generation << kSourceBits, std::memory_order_release);
        if (WorkerSleeping.load(std::memory_order_relaxed)) {
            Wake();
        }
        size_t index = 0;
        uint32_t ticked = 0;
        // The worker may still be reading read/write: wait for it even when a
        // source fails here.
        std::exception_ptr failure;
        while (Claim(generation, index)) {
            try {
                TickSource(index, mixes);
            } catch (...) {
                if (failure == nullptr) {
                    failure = std::current_exception();
                }
            }
            ++ticked;
        }
        CallerTicks += ticked;
        const uint32_t workerTicks =
            static_cast<uint32_t>(AudioCore::HLE::num_sources) - ticked;
        if (Finished.load(std::memory_order_acquire) != workerTicks) {
            const auto waitStart = std::chrono::steady_clock::now();
            while (Finished.load(std::memory_order_acquire) != workerTicks) {
                std::this_thread::yield();
            }
            CallerWaitNanoseconds += static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - waitStart).count());
        }
        for (size_t source = 0; source < AudioCore::HLE::num_sources; ++source) {
            Playing[source] = write.source_statuses.status[source].is_enabled != 0;
        }
        if (failure != nullptr) {
            WorkerException = nullptr;
            std::rethrow_exception(failure);
        }
        if (WorkerException != nullptr) {
            std::rethrow_exception(std::exchange(WorkerException, nullptr));
        }
        if (workerTicks != 0U) {
            for (size_t mix = 0; mix < mixes.size(); ++mix) {
                for (size_t sample = 0; sample < mixes[mix].size(); ++sample) {
                    for (size_t channel = 0; channel < mixes[mix][sample].size(); ++channel) {
                        mixes[mix][sample][channel] += WorkerMixes[mix][sample][channel];
                    }
                }
            }
        }
    }

    void WorkerLoop(void (*onStart)()) {
        if (onStart != nullptr) {
            onStart();
        }
        constexpr auto kSpinAfterWork = std::chrono::milliseconds(3);
        uint64_t mixesGeneration = 0;
        uint64_t wakeRequests = 0;
        auto lastWork = std::chrono::steady_clock::now();
        while (!Stop.load(std::memory_order_relaxed)) {
            const uint64_t generation =
                Next.load(std::memory_order_acquire) >> kSourceBits;
            size_t index = 0;
            if (generation != 0U && Claim(generation, index)) {
                if (mixesGeneration != generation) {
                    WorkerMixes = {};
                    mixesGeneration = generation;
                }
                // The worker must always count the source as finished: the
                // caller waits for it.
                try {
                    TickSource(index, WorkerMixes);
                } catch (...) {
                    if (WorkerException == nullptr) {
                        WorkerException = std::current_exception();
                    }
                }
                WorkerTicks.fetch_add(1, std::memory_order_relaxed);
                Finished.fetch_add(1, std::memory_order_release);
                lastWork = std::chrono::steady_clock::now();
            } else if (std::chrono::steady_clock::now() - lastWork < kSpinAfterWork) {
                std::this_thread::yield();
            } else {
                std::unique_lock lock(WakeMutex);
                WorkerSleeping.store(true, std::memory_order_relaxed);
                WakeCondition.wait_for(lock, std::chrono::milliseconds(1), [&] {
                    const uint64_t word = Next.load(std::memory_order_relaxed);
                    return WakeRequests != wakeRequests ||
                           Stop.load(std::memory_order_relaxed) ||
                           ((word >> kSourceBits) != 0U &&
                            (word & ((1U << kSourceBits) - 1U)) < AudioCore::HLE::num_sources);
                });
                WorkerSleeping.store(false, std::memory_order_relaxed);
                if (WakeRequests != wakeRequests) {
                    wakeRequests = WakeRequests;
                    lastWork = std::chrono::steady_clock::now();
                }
            }
        }
    }

    std::vector<NativeA32DspPhysicalRegion> PhysicalRegions;
    std::vector<AudioCore::HLE::Source> Sources;
    AudioCore::HLE::Mixers Mixers;
    const NativeDspMemoryAccess* CurrentMemory = nullptr;
    std::atomic_flag ResolveLock = ATOMIC_FLAG_INIT;

    std::thread Worker;
    std::atomic<bool> Stop{false};
    std::mutex WakeMutex;
    std::condition_variable WakeCondition;
    uint64_t WakeRequests = 0;  // under WakeMutex
    std::atomic<bool> WorkerSleeping{false};
    // The caller alone writes Generation, JobRead and JobWrite (before the
    // release store to Next); the worker writes WorkerMixes and
    // WorkerException before its release increment of Finished.
    uint64_t Generation = 0;
    std::array<uint8_t, AudioCore::HLE::num_sources> Order{};
    std::array<bool, AudioCore::HLE::num_sources> Playing{};  // caller only
    AudioCore::HLE::SharedMemory* JobRead = nullptr;
    AudioCore::HLE::SharedMemory* JobWrite = nullptr;
    std::atomic<uint64_t> Next{0};
    std::atomic<uint32_t> Finished{0};
    Mixes WorkerMixes{};
    std::exception_ptr WorkerException;
    std::atomic<uint64_t> WorkerTicks{0};
    uint64_t CallerTicks = 0;
    uint64_t CallerWaitNanoseconds = 0;
};

NativeA32DspHle::NativeA32DspHle(
    std::vector<NativeA32DspPhysicalRegion> physicalRegions)
    : mImpl(std::make_unique<Impl>(std::move(physicalRegions))) {}

NativeA32DspHle::~NativeA32DspHle() = default;

std::span<const NativeA32DspPhysicalRegion>
NativeA32DspHle::PhysicalRegions() const {
    return mImpl->PhysicalRegions;
}

void NativeA32DspHle::StartSourceWorker(void (*onStart)()) {
    if (!mImpl->Worker.joinable()) {
        mImpl->Worker = std::thread([impl = mImpl.get(), onStart] { impl->WorkerLoop(onStart); });
    }
}

NativeA32DspHle::SourceTickCounts NativeA32DspHle::TakeSourceTickCounts() {
    SourceTickCounts counts{mImpl->WorkerTicks.exchange(0, std::memory_order_relaxed),
                            mImpl->CallerTicks, mImpl->CallerWaitNanoseconds};
    mImpl->CallerTicks = 0;
    mImpl->CallerWaitNanoseconds = 0;
    return counts;
}

void NativeA32DspHle::WakeSourceWorker() {
    if (mImpl->Worker.joinable()) {
        mImpl->Wake();
    }
}

bool NativeA32DspHle::ProcessFrame(
    const NativeDspMemoryAccess& memory,
    std::vector<int16_t>& interleavedStereo, std::string* error) {
    if (!memory.Read || !memory.Write || !memory.ResolvePhysical) {
        SetError(error, "native DSP memory contract is incomplete");
        return false;
    }
    AudioCore::HLE::SharedMemory region0{};
    AudioCore::HLE::SharedMemory region1{};
    auto region0Bytes = std::span<uint8_t>(
        reinterpret_cast<uint8_t*>(&region0), sizeof(region0));
    auto region1Bytes = std::span<uint8_t>(
        reinterpret_cast<uint8_t*>(&region1), sizeof(region1));
    if (!memory.Read(kDspRegion0Address, region0Bytes) ||
        !memory.Read(kDspRegion1Address, region1Bytes)) {
        SetError(error, "native DSP shared memory is not mapped");
        return false;
    }

    mImpl->CurrentMemory = &memory;

    const size_t readIndex = CurrentRegionIndex(region0, region1);
    auto& read = readIndex == 0U ? region0 : region1;
    auto& write = readIndex == 0U ? region1 : region0;
    Impl::Mixes intermediateMixes{};
    if (mImpl->Worker.joinable()) {
        mImpl->TickSources(read, write, intermediateMixes);
    } else {
        mImpl->JobRead = &read;
        mImpl->JobWrite = &write;
        for (size_t sourceIndex = 0; sourceIndex < mImpl->Sources.size();
             ++sourceIndex) {
            mImpl->TickSource(sourceIndex, intermediateMixes);
        }
    }

    write.dsp_status = mImpl->Mixers.Tick(
        read.dsp_configuration, read.intermediate_mix_samples,
        write.intermediate_mix_samples, intermediateMixes);
    const auto output = mImpl->Mixers.GetOutput();
    interleavedStereo.reserve(interleavedStereo.size() + output.size() * 2U);
    for (size_t sample = 0; sample < output.size(); ++sample) {
        for (size_t channel = 0; channel < output[sample].size(); ++channel) {
            write.final_samples.pcm16[sample][channel] =
                output[sample][channel];
            interleavedStereo.push_back(output[sample][channel]);
        }
    }

    const auto readBytes = std::span<const uint8_t>(
        reinterpret_cast<const uint8_t*>(&read), sizeof(read));
    const auto writeBytes = std::span<const uint8_t>(
        reinterpret_cast<const uint8_t*>(&write), sizeof(write));
    const uint32_t readAddress =
        readIndex == 0U ? kDspRegion0Address : kDspRegion1Address;
    const uint32_t writeAddress =
        readIndex == 0U ? kDspRegion1Address : kDspRegion0Address;
    if (!memory.Write(readAddress, readBytes) ||
        !memory.Write(writeAddress, writeBytes)) {
        SetError(error, "native DSP shared memory is not writable");
        return false;
    }
    return true;
}

nlohmann::json NativeA32DspHle::CaptureState() const {
    nlohmann::json regions = nlohmann::json::array();
    for (const auto& region : mImpl->PhysicalRegions) {
        regions.push_back({
            {"physical_address", region.PhysicalAddress},
            {"virtual_address", region.VirtualAddress},
            {"size", region.Size},
        });
    }
    nlohmann::json sources = nlohmann::json::array();
    for (const auto& source : mImpl->Sources) {
        sources.push_back(source.CaptureState());
    }
    return {
        {"schema", "oot3d_native_dsp_hle_v1"},
        {"physical_regions", std::move(regions)},
        {"sources", std::move(sources)},
        {"mixers", mImpl->Mixers.CaptureState()},
    };
}

bool NativeA32DspHle::RestoreState(const nlohmann::json& state,
                                   std::string* error) {
    try {
        if (!state.is_object() ||
            state.value("schema", std::string{}) !=
                "oot3d_native_dsp_hle_v1") {
            SetError(error, "invalid native DSP HLE state schema");
            return false;
        }
        const auto& regions = state.at("physical_regions");
        const auto& sources = state.at("sources");
        if (!regions.is_array() ||
            regions.size() != mImpl->PhysicalRegions.size() ||
            !sources.is_array() ||
            sources.size() != AudioCore::HLE::num_sources) {
            SetError(error, "native DSP HLE state layout mismatch");
            return false;
        }
        for (size_t index = 0; index < regions.size(); ++index) {
            const auto& encoded = regions[index];
            const auto& expected = mImpl->PhysicalRegions[index];
            if (encoded.at("physical_address").get<uint32_t>() !=
                    expected.PhysicalAddress ||
                encoded.at("virtual_address").get<uint32_t>() !=
                    expected.VirtualAddress ||
                encoded.at("size").get<size_t>() != expected.Size) {
                SetError(error,
                         "native DSP HLE physical-memory layout mismatch");
                return false;
            }
        }

        std::vector<AudioCore::HLE::Source> restoredSources;
        restoredSources.reserve(AudioCore::HLE::num_sources);
        for (size_t index = 0; index < AudioCore::HLE::num_sources; ++index) {
            restoredSources.emplace_back(index);
            restoredSources.back().SetMemoryResolver(
                [impl = mImpl.get()](uint32_t address, size_t size) {
                    return impl->Resolve(address, size);
                });
            if (!restoredSources.back().RestoreState(sources[index], error)) {
                return false;
            }
        }
        AudioCore::HLE::Mixers restoredMixers;
        if (!restoredMixers.RestoreState(state.at("mixers"), error)) {
            return false;
        }

        mImpl->Sources.swap(restoredSources);
        mImpl->Mixers = std::move(restoredMixers);
        mImpl->CurrentMemory = nullptr;
        return true;
    } catch (const std::exception& exception) {
        if (error != nullptr) {
            *error = std::string("invalid native DSP HLE state: ") +
                     exception.what();
        }
        return false;
    }
}

} // namespace Oot3dNativeGame
