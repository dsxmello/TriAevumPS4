// TriAevum PS4 product - the VideoOut owner shared by the Fast3D window
// backend (flips) and the rendering API (registers its three linear scanout
// VkImages). One process-wide instance; everything runs on the render thread.

#pragma once

#include <cstdint>

namespace triaevum::ps4 {

class Ps4Display {
  public:
    static Ps4Display& Instance();

    // sceVideoOutOpen + the flip event queue. Idempotent.
    bool Open();
    // Registers the scanout buffers (2 MiB aligned, linear A8B8G8R8).
    bool Register(void* const* buffers, uint32_t count, uint32_t width, uint32_t height,
                  uint32_t pitchPixels);

    uint32_t Width() const { return mWidth; }
    uint32_t Height() const { return mHeight; }
    // The buffer the next flip shows.
    uint32_t NextBuffer() const { return mNextBuffer; }
    // Blocks until NextBuffer() is neither on screen nor queued: with three
    // buffers one flip may still wait for its vblank while the next frame
    // renders (double buffering would wait for it here every frame).
    void WaitPendingFlip();
    // Queues NextBuffer() for the next vsync without waiting for it, then
    // moves on to the following buffer.
    bool Flip();
    uint64_t Flips() const { return mFlips; }

  private:
    Ps4Display() = default;
    int mHandle = -1;
    uint64_t mQueue = 0;
    bool mRegistered = false;
    uint32_t mBufferCount = 2;
    uint32_t mNextBuffer = 0;
    uint32_t mWidth = 1920;
    uint32_t mHeight = 1080;
    uint64_t mFlips = 0;
};

} // namespace triaevum::ps4
