#pragma once

// The GSP worker (PS4 1.70). The game thread is the console's bottleneck, and
// ~3 ms of each host frame went to executing the guest's GSP commands (the
// command-list walk, the draw packets and their geometry/texture capture,
// fills, DMA, transfers) inline in its TriggerCmdReqQueue. 1.69 simulated
// moving all of it to another thread, in order ("design B"): it hid ~all of
// it, the guest almost never waiting for it. This is that thread.
//
// The game thread reads the command packets and their lists from the queue
// (and the composition spans the hooks recorded for each list), hands each
// command to the worker as a job, and goes on running the guest. The worker
// executes the jobs in submission order, as the console's GPU does, and
// leaves the interrupts they raise for the game thread, which queues them to
// the guest at its next SVC (Service) or when it waits for the worker:
// - whenever the guest stops (RunUntilGuestWait), so that every command it
//   submitted is done when the frame code drains the queue, exactly as
//   before; if that delivered an interrupt, the guest runs again;
// - before any other GSP request (register writes and reads see every
//   earlier command) and before a change of the guest memory layout.
// Guest memory is reached through the device paths of
// oot3d_native_a32_memory_device.h; what they cannot do in place the game
// thread runs for the worker (Service), never the other way round.
//
// Guest writes to observed memory (raw copies the GPU owns) reach the
// submission queue in order with the commands: as a job while the worker has
// any, or at the end of the job a game-thread call was made for. The game
// thread never takes the queue's lock while the worker may hold it.
//
// Like the DSP worker (1.64) and the plan helpers (1.63), nothing per job
// takes a lock or makes a system call: the jobs sit in fixed slots, published
// with one release store, and the worker spins with yields during the bursts.
// Between bursts it waits on a condition variable with a 1 ms timeout (lost
// notifications only delay it). Game thread and worker only.

#include "oot3d_native_a32_memory_device.h"
#include "oot3d_native_pica_frontend.h"

#if defined(__ORBIS__)
#include "fast/backends/gfx_orbis.h"
#endif

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace Oot3dNativeGame {

class NativeA32Memory;

class PicaGspWorker final {
  public:
    using Clock = std::chrono::steady_clock;

    struct Job {
        // A guest write the submission queue has to see in order (no
        // command): written by the guest after the jobs before it.
        bool GuestWrite = false;
        uint32_t WriteAddress = 0;
        size_t WriteSize = 0;
        Oot3dGspCommandPacket Packet{};
        std::vector<uint32_t> List;  // the command list, read at submission
        std::vector<Oot3dPicaCommandListCompositionSpan> Spans;
        bool HasSpans = false;
        std::optional<Oot3dPicaCompositionDomain> Domain;
    };

    // Worker thread: executes one command, appending the interrupts it
    // raises in order; false (with error) fails the guest at its next SVC.
    using Execute =
        std::function<bool(const Job&, std::vector<uint8_t>& interrupts, std::string& error)>;
    // Worker thread: a guest write to observed memory, for the queue.
    using GuestWrite = std::function<void(uint32_t address, size_t size)>;
    // Game thread: queues one interrupt to the guest.
    using Deliver = std::function<bool(uint8_t interrupt)>;

    // Where the game thread waited for the worker.
    enum class WaitSite : uint8_t { GuestWait, Request, Layout, Room, Count };

    struct Stats {
        uint64_t Commands = 0, Lists = 0, GuestWrites = 0;
        uint64_t BusyNanoseconds = 0;         // the worker executing jobs
        uint64_t GuestCallNanoseconds = 0;    // the worker waiting for game-thread calls
        std::array<uint64_t, static_cast<size_t>(WaitSite::Count)> WaitNanoseconds{};
    };

    PicaGspWorker(Execute execute, GuestWrite guestWrite, void (*onStart)())
        : mExecute(std::move(execute)), mGuestWrite(std::move(guestWrite)), mOnStart(onStart) {
        gWorker = this;
        NativeA32DeviceAccess::SetLayoutHook(&LayoutHook);
        mThread = std::thread([this] { Loop(); });
    }

    ~PicaGspWorker() {
        NativeA32DeviceAccess::SetLayoutHook(nullptr);
        mStop.store(true, std::memory_order_seq_cst);
        Wake();
        mThread.join();
        gWorker = nullptr;
    }

    PicaGspWorker(const PicaGspWorker&) = delete;
    PicaGspWorker& operator=(const PicaGspWorker&) = delete;

    // Game thread: the next job's slot (waiting for room), then Publish.
    Job& Reserve() {
        const uint64_t tail = mTail.load(std::memory_order_relaxed);
        if (tail - mHead.load(std::memory_order_acquire) == kSlots) {
            WaitFor(tail - kSlots + 1U, WaitSite::Room);
        }
        Job& job = Slot(tail);
        job.GuestWrite = false;
        job.HasSpans = false;
        job.Domain.reset();
        job.List.clear();
        job.Spans.clear();
        return job;
    }

    void Publish() {
        mTail.store(mTail.load(std::memory_order_relaxed) + 1U, std::memory_order_seq_cst);
        if (mSleeping.load(std::memory_order_seq_cst)) Wake();
    }

    // Game thread: no job outstanding, so the worker holds no lock and
    // reaches nothing until the next Publish.
    bool Idle() const noexcept {
        return mHead.load(std::memory_order_acquire) == mTail.load(std::memory_order_relaxed);
    }

    // Game thread, from the write observer: false when the queue can take
    // the write right away (the worker is idle).
    bool PostGuestWrite(uint32_t address, size_t size) {
        if (mServicing.load(std::memory_order_relaxed)) {
            // A call made for the worker wrote it: the worker passes it on
            // when that job ends, before the next command.
            mCallWrites.emplace_back(address, size);
            return true;
        }
        if (Idle()) return false;
        Job& job = Reserve();
        job.GuestWrite = true;
        job.WriteAddress = address;
        job.WriteSize = size;
        Publish();
        return true;
    }

    // Game thread, at every SVC: the calls the worker is waiting for, the
    // generation steps it owes, and the interrupts it raised (delivered in
    // order; true if any was). Its first error, if any, goes to error.
    bool Service(NativeA32Memory& memory, const Deliver& deliver, std::string& error) {
        ServiceCall();
        NativeA32DeviceAccess::Service(memory);
        if (!mHasOutput.load(std::memory_order_acquire)) return false;
        {
            OutputLock lock(mOutputLock);
            mDelivering.swap(mOutInterrupts);
            if (error.empty() && !mOutError.empty()) error = mOutError;
            mOutError.clear();
            mHasOutput.store(false, std::memory_order_relaxed);
        }
        bool delivered = false;
        for (const uint8_t interrupt : mDelivering) {
            if (!deliver(interrupt)) {
                if (error.empty()) error = "a GSP worker interrupt could not be queued";
                break;
            }
            delivered = true;
        }
        mDelivering.clear();
        return delivered;
    }

    // Game thread: waits for every published job, then Service.
    bool Drain(NativeA32Memory& memory, const Deliver& deliver, WaitSite site,
               std::string& error) {
        WaitFor(mTail.load(std::memory_order_relaxed), site);
        return Service(memory, deliver, error);
    }

    Stats ReadStats() const noexcept {
        Stats stats;
        stats.Commands = mCommands.load(std::memory_order_relaxed);
        stats.Lists = mLists.load(std::memory_order_relaxed);
        stats.GuestWrites = mGuestWrites.load(std::memory_order_relaxed);
        stats.BusyNanoseconds = mBusyNanoseconds.load(std::memory_order_relaxed);
        stats.GuestCallNanoseconds = mGuestCallNanoseconds.load(std::memory_order_relaxed);
        for (size_t site = 0; site < stats.WaitNanoseconds.size(); ++site) {
            stats.WaitNanoseconds[site] = mWaitNanoseconds[site].load(std::memory_order_relaxed);
        }
        return stats;
    }

  private:
    static constexpr uint64_t kSlots = 64;
    // After its last job the worker keeps polling this long before it sleeps
    // (the guest submits in bursts, a few per refresh).
    static constexpr auto kSpinAfterWork = std::chrono::milliseconds(2);

    struct Call {
        void (*Function)(void*) = nullptr;
        void* Argument = nullptr;
        std::atomic<bool> Done{false};
    };

    class OutputLock final {
      public:
        explicit OutputLock(std::atomic_flag& flag) noexcept : mFlag(flag) {
            while (mFlag.test_and_set(std::memory_order_acquire)) std::this_thread::yield();
        }
        ~OutputLock() { mFlag.clear(std::memory_order_release); }
        OutputLock(const OutputLock&) = delete;
        OutputLock& operator=(const OutputLock&) = delete;

      private:
        std::atomic_flag& mFlag;
    };

    static uint64_t Since(Clock::time_point start) {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
    }

    Job& Slot(uint64_t sequence) { return mSlots[sequence % kSlots]; }

    void Wake() {
        {
            std::lock_guard lock(mWakeMutex);
            ++mWakeRequests;
        }
        mWakeCondition.notify_one();
    }

    // Game thread: until the worker has taken every job before sequence
    // `until`, serving its calls meanwhile.
    void WaitFor(uint64_t until, WaitSite site) {
        if (mHead.load(std::memory_order_acquire) >= until) return;
        const auto start = Clock::now();
#if defined(__ORBIS__)
        Fast::OrbisWaitScope wait("the GSP worker");
#endif
        while (mHead.load(std::memory_order_acquire) < until) {
            if (!ServiceCall()) std::this_thread::yield();
        }
        mWaitNanoseconds[static_cast<size_t>(site)].fetch_add(Since(start),
                                                               std::memory_order_relaxed);
    }

    // Game thread: runs the call the worker is waiting for, if any.
    bool ServiceCall() {
        Call* call = mCall.load(std::memory_order_acquire);
        if (call == nullptr) return false;
        mServicing.store(true, std::memory_order_relaxed);
        call->Function(call->Argument);
        mServicing.store(false, std::memory_order_relaxed);
        mCall.store(nullptr, std::memory_order_relaxed);
        call->Done.store(true, std::memory_order_release);
        return true;
    }

    // Worker thread (NativeA32DeviceAccess::GuestRunner).
    static void RunOnGuest(void (*function)(void*), void* argument) {
        PicaGspWorker& self = *gWorker;
        Call call;
        call.Function = function;
        call.Argument = argument;
        const auto start = Clock::now();
#if defined(__ORBIS__)
        Fast::OrbisWaitScope wait("the game thread (GSP worker call)");
#endif
        self.mCall.store(&call, std::memory_order_release);
        while (!call.Done.load(std::memory_order_acquire)) std::this_thread::yield();
        self.mGuestCallNanoseconds.fetch_add(Since(start), std::memory_order_relaxed);
    }

    // Before the guest memory layout changes (any thread that changes it):
    // let the worker finish. Not while serving it a call: it is stopped.
    static void LayoutHook() {
        PicaGspWorker* self = gWorker;
        if (self == nullptr || self->mServicing.load(std::memory_order_relaxed)) return;
        self->WaitFor(self->mTail.load(std::memory_order_relaxed), WaitSite::Layout);
    }

    void Loop() {
        NativeA32DeviceAccess::ThreadScope device(&RunOnGuest);
        if (mOnStart != nullptr) mOnStart();
        std::vector<uint8_t> interrupts;
        std::string error;
        auto lastWork = Clock::now();
        while (!mStop.load(std::memory_order_relaxed)) {
            const uint64_t head = mHead.load(std::memory_order_relaxed);
            if (head == mTail.load(std::memory_order_acquire)) {
                if (Clock::now() - lastWork < kSpinAfterWork) {
                    std::this_thread::yield();
                    continue;
                }
                std::unique_lock lock(mWakeMutex);
                mSleeping.store(true, std::memory_order_seq_cst);
                if (head == mTail.load(std::memory_order_seq_cst) &&
                    !mStop.load(std::memory_order_relaxed)) {
                    const uint64_t requests = mWakeRequests;
                    mWakeCondition.wait_for(lock, std::chrono::milliseconds(1),
                                            [&] { return mWakeRequests != requests; });
                }
                mSleeping.store(false, std::memory_order_relaxed);
                lastWork = Clock::now();
                continue;
            }
            const auto start = Clock::now();
            const Job& job = Slot(head);
            if (job.GuestWrite) {
                mGuestWrite(job.WriteAddress, job.WriteSize);
                mGuestWrites.fetch_add(1, std::memory_order_relaxed);
            } else {
                interrupts.clear();
                error.clear();
                bool executed = false;
                try {
                    executed = mExecute(job, interrupts, error);
                } catch (const std::exception& exception) {
                    error = exception.what();
                } catch (...) {
                    error = "unknown exception";
                }
                if (!executed && error.empty()) error = "GSP command failed";
                // Writes made for it by the game thread, before the next command.
                for (const auto& [address, size] : mCallWrites) mGuestWrite(address, size);
                mCallWrites.clear();
                if (!interrupts.empty() || !executed) {
                    OutputLock lock(mOutputLock);
                    mOutInterrupts.insert(mOutInterrupts.end(), interrupts.begin(), interrupts.end());
                    if (!executed && mOutError.empty()) mOutError = "GSP worker: " + error;
                    mHasOutput.store(true, std::memory_order_release);
                }
                mCommands.fetch_add(1, std::memory_order_relaxed);
                if (!job.List.empty()) mLists.fetch_add(1, std::memory_order_relaxed);
            }
            mBusyNanoseconds.fetch_add(Since(start), std::memory_order_relaxed);
            mHead.store(head + 1U, std::memory_order_release);
            lastWork = Clock::now();
        }
    }

    // The process's worker (one: the layout hook and the runner are global).
    static inline PicaGspWorker* gWorker = nullptr;

    Execute mExecute;
    GuestWrite mGuestWrite;
    void (*mOnStart)() = nullptr;
    std::array<Job, kSlots> mSlots;
    std::atomic<uint64_t> mHead{0};  // jobs the worker finished
    std::atomic<uint64_t> mTail{0};  // jobs published
    std::atomic<bool> mStop{false};
    std::atomic<bool> mSleeping{false};
    std::mutex mWakeMutex;
    std::condition_variable mWakeCondition;
    uint64_t mWakeRequests = 0;
    std::atomic<Call*> mCall{nullptr};
    std::atomic<bool> mServicing{false};
    // Written by the game thread while it serves a call (the worker waits).
    std::vector<std::pair<uint32_t, size_t>> mCallWrites;
    std::atomic_flag mOutputLock = ATOMIC_FLAG_INIT;
    std::atomic<bool> mHasOutput{false};
    std::vector<uint8_t> mOutInterrupts;
    std::string mOutError;
    std::vector<uint8_t> mDelivering;  // game thread
    std::atomic<uint64_t> mCommands{0}, mLists{0}, mGuestWrites{0};
    std::atomic<uint64_t> mBusyNanoseconds{0}, mGuestCallNanoseconds{0};
    std::array<std::atomic<uint64_t>, static_cast<size_t>(WaitSite::Count)> mWaitNanoseconds{};
    std::thread mThread;
};

// RunUntilGuestWait (oot3d_native_a32_window.cpp), when the guest stopped:
// drains the worker; true if that queued an interrupt. Set while it runs.
inline std::function<bool()> gPicaGspWorkerAtGuestWait;

} // namespace Oot3dNativeGame
