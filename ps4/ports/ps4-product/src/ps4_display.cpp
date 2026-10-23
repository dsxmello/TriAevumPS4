#include "ps4_display.h"

#include <cstdio>

#include <orbis/UserService.h>
#include <orbis/VideoOut.h>
#include <orbis/libkernel.h>

namespace triaevum::ps4 {

Ps4Display& Ps4Display::Instance() {
    static Ps4Display display;
    return display;
}

bool Ps4Display::Open() {
    if (mHandle >= 0) return true;
    sceUserServiceInitialize(nullptr);
    mHandle = sceVideoOutOpen(0xFF, 0, 0, nullptr);
    if (mHandle < 0) {
        std::printf("[triaevum] sceVideoOutOpen = %#x\n", mHandle);
        return false;
    }
    OrbisKernelEqueue queue = 0;
    if (sceKernelCreateEqueue(&queue, "triaevum_flip") != 0 ||
        sceVideoOutAddFlipEvent(queue, mHandle, nullptr) != 0) {
        std::printf("[triaevum] flip event queue setup failed\n");
        return false;
    }
    mQueue = static_cast<uint64_t>(queue);
    return true;
}

bool Ps4Display::Register(void* const* buffers, uint32_t count, uint32_t width,
                          uint32_t height, uint32_t pitchPixels) {
    if (!Open()) return false;
    // A8B8G8R8_SRGB names channels MSB-first: byte 0 is red, i.e. the
    // R8G8B8A8 VkImages of the renderer (VK probe evidence).
    OrbisVideoOutBufferAttribute attribute;
    sceVideoOutSetBufferAttribute(&attribute, ORBIS_VIDEO_OUT_PIXEL_FORMAT_A8B8G8R8_SRGB,
                                  ORBIS_VIDEO_OUT_TILING_MODE_LINEAR, 0, width, height,
                                  pitchPixels);
    const int32_t registered = sceVideoOutRegisterBuffers(
        mHandle, 0, const_cast<void**>(buffers), static_cast<int32_t>(count), &attribute);
    std::printf("[triaevum] sceVideoOutRegisterBuffers = %d (%u buffers, %ux%u pitch %u)\n",
                registered, count, width, height, pitchPixels);
    mRegistered = registered >= 0;
    mBufferCount = count;
    mWidth = width;
    mHeight = height;
    return mRegistered;
}

void Ps4Display::WaitPendingFlip() {
    if (!mRegistered) return;
    // NextBuffer() was last shown by the flip mBufferCount flips ago; it is
    // free once at most mBufferCount - 2 flips are still queued. Flip events
    // coalesce, so the queue only wakes the check; the status counts.
    const int32_t allowed = static_cast<int32_t>(mBufferCount) - 2;
    for (;;) {
        OrbisVideoOutFlipStatus status{};
        if (sceVideoOutGetFlipStatus(mHandle, &status) != 0 || status.numFlipPending <= allowed)
            return;
        OrbisKernelEvent event;
        int count = 0;
        OrbisKernelUseconds timeout = 4000;
        sceKernelWaitEqueue(static_cast<OrbisKernelEqueue>(mQueue), &event, 1, &count, &timeout);
    }
}

bool Ps4Display::Flip() {
    if (!mRegistered) return false;
    WaitPendingFlip();
    const int flipped = sceVideoOutSubmitFlip(mHandle, static_cast<int32_t>(mNextBuffer),
                                              ORBIS_VIDEO_OUT_FLIP_VSYNC,
                                              static_cast<int64_t>(mFlips));
    if (flipped != 0) {
        std::printf("[triaevum] sceVideoOutSubmitFlip = %#x\n", flipped);
        return false;
    }
    ++mFlips;
    mNextBuffer = (mNextBuffer + 1U) % mBufferCount;
    return true;
}

} // namespace triaevum::ps4
