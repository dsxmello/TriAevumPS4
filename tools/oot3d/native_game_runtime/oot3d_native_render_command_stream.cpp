#if defined(__ORBIS__)
#include "fast/backends/gfx_orbis.h"
#endif
#include "oot3d_native_render_command_stream.h"

#include <chrono>
#include <stdexcept>
#include <utility>

namespace Oot3dNativeGame {
namespace {

thread_local bool tInCommand = false;
thread_local bool tSynchronous = false;

struct CommandScope {
    explicit CommandScope(bool synchronous) {
        tInCommand = true;
        tSynchronous = synchronous;
    }
    ~CommandScope() {
        tInCommand = false;
        tSynchronous = false;
    }
};

} // namespace

NativeRenderCommandStream::NativeRenderCommandStream(Mode mode,
                                                     size_t maxFramesInFlight)
    : mMode(mode),
      mMaxFramesInFlight(maxFramesInFlight == 0U ? 1U : maxFramesInFlight) {}

// On the PS4 condition-variable notifications get lost (the sampler's signals
// interrupt the waits, 1.57), so every wait there rechecks its condition each
// millisecond: a lost notification costs 1 ms instead of a stalled loop.
template <typename Ready>
void NativeRenderCommandStream::Wait(std::condition_variable& condition,
                                     std::unique_lock<std::mutex>& lock, Ready ready) {
#if defined(__ORBIS__)
    while (!ready()) {
        if (condition.wait_for(lock, std::chrono::milliseconds(1)) == std::cv_status::timeout &&
            ready()) {
            ++mWaitsRecovered;
        }
    }
#else
    condition.wait(lock, ready);
#endif
}

NativeRenderCommandStream::~NativeRenderCommandStream() {
    StopWorker();
}

void NativeRenderCommandStream::SetMode(Mode mode) {
    Drain();
    if (mode == mMode) return;
    if (mode == Mode::Inline) StopWorker();
    mMode = mode;
}

bool NativeRenderCommandStream::InCommand() const noexcept {
    return tInCommand;
}

bool NativeRenderCommandStream::InSynchronousCommand() const noexcept {
    return tInCommand && tSynchronous;
}

uint64_t NativeRenderCommandStream::ExecutedCommands() const noexcept {
    std::lock_guard lock(mMutex);
    return mExecuted;
}

void NativeRenderCommandStream::Submit(Command command, bool endsFrame) {
    Enqueue(std::move(command), endsFrame, false);
}

void NativeRenderCommandStream::Enqueue(Command command, bool endsFrame,
                                        bool synchronous) {
    if (mMode == Mode::Inline) {
        {
            CommandScope scope(true);
            command();
        }
        std::lock_guard lock(mMutex);
        ++mSubmitted;
        ++mExecuted;
        return;
    }
    StartWorker();
    std::unique_lock lock(mMutex);
    RethrowPendingFailureLocked(lock);
    if (endsFrame) {
#if defined(__ORBIS__)
        Fast::OrbisWaitScope wait("the render worker (frames in flight)");
#endif
        Wait(mProgress, lock, [&] {
            return mFailure != nullptr || mFramesQueued < mMaxFramesInFlight;
        });
        RethrowPendingFailureLocked(lock);
        ++mFramesQueued;
    }
    mQueue.push_back({std::move(command), endsFrame, synchronous});
    ++mSubmitted;
    mWork.notify_one();
}

void NativeRenderCommandStream::Run(Command command) {
    Drain();
    {
        CommandScope scope(true);
        command();
    }
    std::lock_guard lock(mMutex);
    ++mSubmitted;
    ++mExecuted;
}

void NativeRenderCommandStream::Drain() {
    if (mMode == Mode::Inline || !mWorker.joinable()) return;
    std::unique_lock lock(mMutex);
#if defined(__ORBIS__)
    Fast::OrbisWaitScope wait("the render worker (drain)");
#endif
    Wait(mProgress, lock, [&] {
        return mFailure != nullptr || (mQueue.empty() && !mBusy);
    });
    RethrowPendingFailureLocked(lock);
}

void NativeRenderCommandStream::WaitForFramesInFlight(size_t frames) {
    if (mMode == Mode::Inline || !mWorker.joinable()) return;
    std::unique_lock lock(mMutex);
    Wait(mProgress, lock, [&] {
        return mFailure != nullptr || mFramesQueued <= frames;
    });
    RethrowPendingFailureLocked(lock);
}

void NativeRenderCommandStream::SetMaxFramesInFlight(size_t frames) {
    Drain();
    std::lock_guard lock(mMutex);
    mMaxFramesInFlight = frames == 0U ? 1U : frames;
}

uint64_t NativeRenderCommandStream::WaitsRecoveredByTimeout() const noexcept {
    std::lock_guard lock(mMutex);
    return mWaitsRecovered;
}

void NativeRenderCommandStream::RethrowPendingFailureLocked(
    std::unique_lock<std::mutex>& lock) {
    if (mFailure == nullptr) return;
    // Report the failure once; the stream is empty and usable afterwards.
    auto failure = std::exchange(mFailure, nullptr);
    mQueue.clear();
    mFramesQueued = 0U;
    mExecuted = mSubmitted;
    lock.unlock();
    std::rethrow_exception(failure);
}

void NativeRenderCommandStream::StartWorker() {
    if (mWorker.joinable()) return;
    mStop = false;
    mWorker = std::thread([this] { WorkerLoop(); });
}

void NativeRenderCommandStream::StopWorker() {
    if (!mWorker.joinable()) return;
    {
        std::lock_guard lock(mMutex);
        mStop = true;
    }
    mWork.notify_all();
    mWorker.join();
}

void NativeRenderCommandStream::WorkerLoop() {
#if defined(__ORBIS__)
    Fast::OrbisRegisterThread("render");
#endif
    std::unique_lock lock(mMutex);
    for (;;) {
        Wait(mWork, lock, [&] { return mStop || !mQueue.empty(); });
        if (mQueue.empty()) return;  // stopping with nothing left to run
        Entry entry = std::move(mQueue.front());
        mQueue.pop_front();
        mBusy = true;
        lock.unlock();
        std::exception_ptr failure;
        try {
            CommandScope scope(entry.Synchronous);
            entry.Work();
        } catch (...) {
            failure = std::current_exception();
        }
        lock.lock();
        mBusy = false;
        if (entry.EndsFrame && mFramesQueued != 0U) --mFramesQueued;
        if (failure != nullptr) {
            mFailure = failure;
            mQueue.clear();
            mFramesQueued = 0U;
        } else {
            ++mExecuted;
        }
        mProgress.notify_all();
    }
}

} // namespace Oot3dNativeGame
