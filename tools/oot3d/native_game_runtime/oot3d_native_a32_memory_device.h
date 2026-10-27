#pragma once

// Guest memory seen from the GSP worker (PS4 1.70). While the guest runs, a
// worker thread executes the GPU's commands (oot3d_native_pica_gsp_worker.h),
// concurrently with the guest on the game thread. The worker reaches guest
// memory through the same out-of-line members as everyone else (ReadBytes,
// WriteBytes, SynchronousReadView, RangeWriteGeneration, IsMapped,
// ObserveWrites...); those see that the calling thread is the device's and
// take a path that never changes the memory's bookkeeping, which belongs to
// the game thread (the generated code updates it inline, unsynchronized):
// - a span whose pages all have a fast entry (mapped whole, not deferred, not
//   watched; for a write also writable and not observed) is read or written
//   in place, as the console's GPU does. Mapping and page-table changes wait
//   for the worker first (the layout hook), so regions and their bytes stay
//   where they are while it runs.
// - a device write leaves the write generation to the game thread: it zeroes
//   the pages' generation (a range with a zero page has no certificate, so a
//   snapshot of it compares bytes) and owes one generation step for the
//   range, which the game thread takes at its next service (Service).
// - escaped write pointers are checked against a copy the game thread makes
//   when one escapes; until the copy is fresh, ranges have no certificate.
// - everything else (deferred bytes, observed or watched pages, observing a
//   range, typed accesses) runs on the game thread, which serves the worker
//   at every SVC and while it waits for it (the guest runner).
// None of this changes NativeA32Memory's layout or its inline paths: the
// generated code compiled against the header is the same.

#include <cstddef>
#include <cstdint>
#include <optional>

namespace Oot3dNativeGame {

class NativeA32Memory;

class NativeA32DeviceAccess final {
  public:
    // Runs call(argument) on the game thread and returns once it has.
    using GuestRunner = void (*)(void (*call)(void*), void* argument);

    // Device thread: marks it as the device's for the scope's lifetime.
    class ThreadScope final {
      public:
        explicit ThreadScope(GuestRunner runner) noexcept;
        ~ThreadScope();
        ThreadScope(const ThreadScope&) = delete;
        ThreadScope& operator=(const ThreadScope&) = delete;
    };

    static bool OnDeviceThread() noexcept;

    // Game thread: called before any change of the mapping or the page
    // table (and before an image is replaced), to let the worker finish.
    static void SetLayoutHook(void (*hook)()) noexcept;

    // Game thread: the generation steps owed for device writes, and a fresh
    // copy of the escaped write ranges when the worker asked for one.
    static void Service(NativeA32Memory& memory);

    // Per-process totals, for the five-second log.
    struct Counts {
        uint64_t DirectReads = 0;   // spans read in place by the device
        uint64_t DirectWrites = 0;  // spans written in place by the device
        uint64_t GuestCalls = 0;    // accesses the game thread ran for it
    };
    static Counts ReadCounts() noexcept;

  private:
    friend class NativeA32Memory;
    // The bytes of [address, address + size) when every page of it has a
    // fast read (or write) entry, else null: the caller then asks the game
    // thread.
    static uint8_t* InPlace(const NativeA32Memory& memory, uint32_t address,
                            size_t size, bool write) noexcept;
    // After an in-place write: zero generations, one step owed.
    static void NoteWritten(NativeA32Memory& memory, uint32_t address,
                            size_t size);
    static std::optional<uint64_t> RangeWriteGeneration(
        const NativeA32Memory& memory, uint32_t address, size_t size);
    // ObserveWrites' answer without the game thread, when the range is
    // already observed (true), else false: then ask the game thread.
    static bool AlreadyObserved(const NativeA32Memory& memory, uint32_t address,
                                size_t size);
    static void LayoutChanging();
    // Escaped write pointers (game thread): their copy is stale.
    static void EscapesChanged() noexcept;
    template <typename Function>
    static auto OnGuestThread(Function&& function);
};

} // namespace Oot3dNativeGame
