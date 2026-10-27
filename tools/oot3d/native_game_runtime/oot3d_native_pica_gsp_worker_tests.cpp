// Stress test of the GSP worker (oot3d_native_pica_gsp_worker.h) and the
// device paths of NativeA32Memory (oot3d_native_a32_memory_device.h), on the PC.
// Built by hand (the tri-host tree does not configure), from the repo root:
//   A=tools/oot3d/native_a32_runtime/upstream/recomp
//   clang++ -std=c++23 -O1 -g -fsanitize=address,undefined -pthread \
//     -DOOT3D_GSP_WORKER_TEST_STANDALONE \
//     -Itools/oot3d/native_game_runtime -Itools/oot3d/native_pica_frontend \
//     -Itools/oot3d/native_a32_runtime -Itools/oot3d/native_a32_runtime/upstream -I$A \
//     -Ips4/ports/ps4-vk-game/third_party -Iruntime/three_ds_recomp/include -Itools/oot3d \
//     tools/oot3d/native_game_runtime/oot3d_native_pica_gsp_worker_tests.cpp \
//     tools/oot3d/native_game_runtime/oot3d_native_a32_memory.cpp $A/a32_runtime.cpp \
//     -o gsp_worker_test && ./gsp_worker_test 300000
// (-fsanitize=thread reports only the intended races: page generations
// written inline by the game thread against the worker's atomic reads.)
//
// The game thread plays the guest: it publishes jobs, serves the worker at
// "SVCs", drains it at "guest waits", and writes guest memory. Jobs capture
// ranges like the submission queue's snapshot caches do (write generation,
// then bytes), write as the device (DMA/fills), observe ranges and write
// observed pages (game-thread calls), and raise one interrupt each.
//
// Checked:
// - certificate: a capture whose generation equals the one kept with the
//   previous snapshot of the same range finds the same bytes (guest writes
//   to those ranges happen only while the worker is idle, device writes in
//   job order, escaped pointers write without a generation step);
// - interrupts arrive once each, in job order;
// - device writes land, guest writes to observed pages reach the queue
//   callback, layout changes wait for the worker, no deadlock;
// - after a drain and a service every page of the region has a generation.

#include "oot3d_native_a32_memory.h"
#include "oot3d_native_a32_memory_device.h"
#include "oot3d_native_pica_gsp_worker.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <random>

#if defined(OOT3D_GSP_WORKER_TEST_STANDALONE)
// a32_runtime.cpp links the interpreter, which the test never runs.
#include "a32_core.h"
#include "a32_core_internal.h"
#include "a32_vfp_scalar.h"
#include "a32_vfp_transport.h"
namespace oot3d::recomp::a32 {
ExecutionResult ExecuteCore(std::uint32_t, std::uint32_t, GuestState&, MemoryBus&) { std::abort(); }
ExecutionResult ExecuteVfpScalar(std::uint32_t, std::uint32_t, GuestState&) { std::abort(); }
ExecutionResult ExecuteVfpTransport(std::uint32_t, std::uint32_t, GuestState&, MemoryBus&) { std::abort(); }
namespace internal {
ExecutionResult ExecuteCoreAlu(std::uint32_t, std::uint32_t, GuestState&, MemoryBus&) { std::abort(); }
ExecutionResult ExecuteCoreMemory(std::uint32_t, std::uint32_t, GuestState&, MemoryBus&) { std::abort(); }
ExecutionResult ExecuteCoreSystem(std::uint32_t, std::uint32_t, GuestState&) noexcept { std::abort(); }
}
}
#endif

using namespace Oot3dNativeGame;

namespace {

constexpr uint32_t kBase = 0x20000000U;
constexpr uint32_t kSize = 0x40000U;  // 64 pages
constexpr uint32_t kOther = 0x30000000U;  // the guest's own region, never the device's
constexpr uint32_t kObserved = kBase + 0x3F000U;  // the last page: observed

enum Op : uint32_t { Capture = 1, DeviceWrite = 2, Observe = 3, ObservedWrite = 4, DeviceFill = 5 };

[[noreturn]] void Fail(const char* what, uint64_t detail = 0) {
    std::fprintf(stderr, "FAIL: %s (%llu)\n", what, static_cast<unsigned long long>(detail));
    std::abort();
}

void Require(bool condition, const char* what, uint64_t detail = 0) {
    if (!condition) Fail(what, detail);
}

struct Snapshot {
    std::optional<uint64_t> Generation;
    std::vector<uint8_t> Bytes;
};

} // namespace

int main(int argc, char** argv) {
    const uint64_t iterations = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 200000;
    NativeA32Memory memory;
    std::string error;
    Require(memory.MapRegion({"linear", kBase, kSize, true, false, {}}, &error), "map");
    Require(memory.MapRegion({"other", kOther, 0x10000U, true, false, {}}, &error), "map other");

    // Observer: as the window's (the queue takes it in order with the jobs).
    std::atomic<uint64_t> queueWrites{0};  // both threads
    uint64_t observedNotes = 0;     // game thread
    PicaGspWorker* workerPointer = nullptr;
    memory.SetWriteObserver([&](uint32_t address, size_t size) {
        ++observedNotes;
        if (workerPointer == nullptr || !workerPointer->PostGuestWrite(address, size)) ++queueWrites;
    });

    std::map<uint32_t, Snapshot> snapshots;  // worker thread only (by range key)
    uint64_t certified = 0, compared = 0;    // worker thread
    PicaGspWorker worker(
        [&](const PicaGspWorker::Job& job, std::vector<uint8_t>& interrupts, std::string& jobError) {
            const uint32_t op = job.Packet.Control;
            const uint32_t address = job.Packet.Parameters[0];
            const uint32_t size = job.Packet.Parameters[1];
            const uint32_t seed = job.Packet.Parameters[2];
            if (op == Capture) {
                // As CaptureGeometryResource: view, generation, then the bytes.
                const auto view = memory.SynchronousReadView(address, size);
                const auto generation = memory.RangeWriteGeneration(address, size);
                Require(view.size() == size, "capture view");
                auto& snapshot = snapshots[address * 4U + size];
                if (generation && snapshot.Generation == generation) {
                    ++certified;
                    if (std::memcmp(snapshot.Bytes.data(), view.data(), size) != 0)
                        Fail("certified snapshot differs", address);
                } else {
                    ++compared;
                }
                snapshot.Generation = generation;
                snapshot.Bytes.assign(view.begin(), view.end());
            } else if (op == DeviceWrite || op == ObservedWrite) {
                std::vector<uint8_t> bytes(size);
                for (uint32_t i = 0; i < size; ++i) bytes[i] = static_cast<uint8_t>(seed + i * 7U);
                if (!memory.WriteBytes(address, bytes)) {
                    jobError = "device write";
                    return false;
                }
                std::vector<uint8_t> back(size);
                Require(memory.ReadBytes(address, back) && back == bytes, "device read back");
            } else if (op == DeviceFill) {
                Require(memory.Fill(address, size, static_cast<uint8_t>(seed)), "device fill");
            } else if (op == Observe) {
                Require(memory.ObserveWrites(address, size), "observe");
            }
            interrupts.push_back(static_cast<uint8_t>(seed));
            return true;
        },
        [&](uint32_t, size_t) { ++queueWrites; },
        nullptr);
    workerPointer = &worker;

    std::mt19937_64 random(12345);
    uint8_t nextInterrupt = 0;   // the next job's
    uint64_t published = 0;
    uint8_t expected = 0;        // the next delivery's
    uint64_t delivered = 0;
    const PicaGspWorker::Deliver deliver = [&](uint8_t interrupt) {
        if (interrupt != expected) Fail("interrupt order", interrupt);
        ++expected;
        ++delivered;
        return true;
    };
    const auto publish = [&](uint32_t op, uint32_t address, uint32_t size) {
        auto& job = worker.Reserve();
        job.Packet.Control = op;
        job.Packet.Parameters[0] = address;
        job.Packet.Parameters[1] = size;
        job.Packet.Parameters[2] = nextInterrupt++;
        ++published;
        worker.Publish();
    };
    // Capture ranges: a fixed set, so snapshots recur (some cross pages).
    const std::array<std::pair<uint32_t, uint32_t>, 8> ranges{{{kBase, 64},
        {kBase + 0x1000U, 0x1000U}, {kBase + 0x1F80U, 0x100U}, {kBase + 0x5000U, 0x3000U},
        {kBase + 0x9000U, 16}, {kBase + 0x20000U, 0x800U}, {kBase + 0x30010U, 0x2000U},
        {kObserved, 0x100U}}};
    publish(Observe, kObserved, 0x1000U);
    std::vector<uint8_t*> escaped;
    uint64_t layoutChanges = 0, escapes = 0, drains = 0;
    for (uint64_t iteration = 0; iteration < iterations; ++iteration) {
        const uint32_t pick = static_cast<uint32_t>(random() % 100U);
        if (pick < 40) {
            const auto& [address, size] = ranges[random() % ranges.size()];
            publish(Capture, address, size);
        } else if (pick < 50) {
            const uint32_t offset = static_cast<uint32_t>(random() % (kSize - 0x2000U));
            publish(random() % 4U ? DeviceWrite : DeviceFill, kBase + offset,
                    1U + static_cast<uint32_t>(random() % 0x1800U));
        } else if (pick < 53) {
            publish(ObservedWrite, kObserved + static_cast<uint32_t>(random() % 0x700U), 64U);
        } else if (pick < 75) {
            // An SVC.
            std::string serviceError;
            worker.Service(memory, deliver, serviceError);
            Require(serviceError.empty(), "service error");
        } else if (pick < 78) {
            // The guest writes an observed page while the worker runs (away
            // from the captured bytes): the queue gets it as a job, in order.
            uint8_t bytes[8] = {static_cast<uint8_t>(random())};
            Require(memory.WriteBytes(kObserved + 0x800U, bytes), "observed while busy");
        } else if (pick < 90) {
            // The guest's own memory while the worker runs: generation steps race the device's.
            for (int i = 0; i < 64; ++i) {
                memory.WriteFast<uint32_t>(kOther + static_cast<uint32_t>(random() % 0x3FF0U) & ~3U,
                                           static_cast<uint32_t>(random()));
            }
        } else if (pick < 99) {
            // A guest wait: drain, then the guest writes the device's region
            // (typed, bytes, an observed page, through escaped pointers).
            std::string drainError;
            worker.Drain(memory, deliver, PicaGspWorker::WaitSite::GuestWait, drainError);
            Require(drainError.empty(), "drain error");
            Require(worker.Idle(), "idle after drain");
            ++drains;
            for (int i = 0; i < 8; ++i) {
                const uint32_t address = kBase + (static_cast<uint32_t>(random() % (kSize - 8U)) & ~3U);
                memory.WriteFast<uint32_t>(address, static_cast<uint32_t>(random()));
            }
            uint8_t bytes[32];
            for (auto& byte : bytes) byte = static_cast<uint8_t>(random());
            Require(memory.WriteBytes(kBase + static_cast<uint32_t>(random() % (kSize - 32U)), bytes),
                    "guest bytes");
            Require(memory.WriteBytes(kObserved + 8U, std::span<const uint8_t>(bytes, 8)), "observed");
            if (random() % 64U == 0U && escapes < 4) {
                // An escaped pointer: later writes through it take no step.
                const uint32_t address = kBase + 0x9000U;
                escaped.push_back(memory.GetWritePointer(address, 16));
                ++escapes;
            }
            for (auto* pointer : escaped) pointer[random() % 16U] = static_cast<uint8_t>(random());
        } else {
            // A layout change with the worker busy (ControlMemory).
            publish(Capture, ranges[1].first, ranges[1].second);
            Require(memory.MapRegion({"heap" + std::to_string(layoutChanges),
                                      0x40000000U + static_cast<uint32_t>(layoutChanges) * 0x1000U,
                                      0x1000U, true, false, {}},
                                     &error),
                    "layout change");
            Require(worker.Idle(), "layout waited for the worker");
            ++layoutChanges;
        }
    }
    std::string finalError;
    worker.Drain(memory, deliver, PicaGspWorker::WaitSite::GuestWait, finalError);
    worker.Service(memory, deliver, finalError);
    Require(finalError.empty(), "final error");
    Require(delivered == published, "every interrupt delivered", delivered);
    // Every page has a generation again (the owed steps were taken), except
    // the escaped one, which the game thread never certifies.
    Require(memory.RangeWriteGeneration(kBase, 0x9000U).has_value(), "generation after service");
    Require(!memory.RangeWriteGeneration(kBase + 0x9000U, 16).has_value() || escapes == 0,
            "escaped range certified");
    const auto stats = worker.ReadStats();
    const auto device = NativeA32DeviceAccess::ReadCounts();
    workerPointer = nullptr;
    std::printf("ok: %llu jobs, %llu interrupts, %llu certified, %llu compared, %llu drains,"
                " %llu layout changes, %llu escapes | guest calls %llu, in place %llu reads"
                " %llu writes | observed notes %llu, queue writes %llu (%llu as jobs)\n",
                static_cast<unsigned long long>(stats.Commands + stats.GuestWrites),
                static_cast<unsigned long long>(delivered),
                static_cast<unsigned long long>(certified), static_cast<unsigned long long>(compared),
                static_cast<unsigned long long>(drains), static_cast<unsigned long long>(layoutChanges),
                static_cast<unsigned long long>(escapes),
                static_cast<unsigned long long>(device.GuestCalls),
                static_cast<unsigned long long>(device.DirectReads),
                static_cast<unsigned long long>(device.DirectWrites),
                static_cast<unsigned long long>(observedNotes),
                static_cast<unsigned long long>(queueWrites.load()),
                static_cast<unsigned long long>(stats.GuestWrites));
    return 0;
}
