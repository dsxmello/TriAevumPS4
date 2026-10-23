#include "ship/audio/SDLAudioPlayer.h"

#include <algorithm>
#include <cmath>
#include <spdlog/spdlog.h>

namespace Ship {

SDLAudioPlayer::~SDLAudioPlayer() {
    SPDLOG_TRACE("destruct SDL audio player");
    DoClose();
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

void SDLAudioPlayer::DoClose() {
    if (mDevice != 0) {
        // Pause playback first
        SDL_PauseAudioDevice(mDevice, 1);
        // Clear any queued audio to prevent glitches when reopening
        SDL_ClearQueuedAudio(mDevice);
        SDL_CloseAudioDevice(mDevice);
        mDevice = 0;
    }
}

bool SDLAudioPlayer::DoInit() {
    if (SDL_Init(SDL_INIT_AUDIO) != 0) {
        SPDLOG_ERROR("SDL init error: {}", SDL_GetError());
        return false;
    }

    // Always open with the correct number of output channels
    mNumChannels = this->GetNumOutputChannels();

    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq = this->GetSampleRate();
    want.format = AUDIO_S16SYS;
    want.channels = mNumChannels;
    want.samples = this->GetSampleLength();
    want.callback = NULL;

    mDevice = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (mDevice == 0) {
        SPDLOG_ERROR("SDL_OpenAudio error: {}", SDL_GetError());
        return false;
    }

    SPDLOG_INFO("SDL Audio initialized: {} channels, {} Hz", mNumChannels, this->GetSampleRate());

#if defined(__ORBIS__)
    // DoPlay starts the output once the first cushion is queued.
    mRefilling = true;
#else
    SDL_PauseAudioDevice(mDevice, 0);
#endif
    return true;
}

int SDLAudioPlayer::Buffered() {
    return SDL_GetQueuedAudioSize(mDevice) / (sizeof(int16_t) * mNumChannels);
}

void SDLAudioPlayer::DoPlay(const uint8_t* buf, size_t len) {
#if defined(__ORBIS__)
    // The PS4 device drains the queue in 5 ms periods while the game queues
    // a whole frame of audio at once: with no cushion, every late frame is a
    // gap (the queue was empty at every submission). An empty queue pauses
    // the output until DesiredBuffered is queued again, so a gap is one
    // short pause instead of constant crackling.
    if (!mRefilling && Buffered() == 0) {
        SDL_PauseAudioDevice(mDevice, 1);
        mRefilling = true;
    }
#endif
#if defined(__ORBIS__)
    // Dynamic rate control: the game makes its audio as fast as it runs, a
    // few percent under real time when a frame is slow. Stretching (or
    // compressing) each submission by up to ~6% towards DesiredBuffered
    // keeps the queue fed without gaps; the pause above remains the last
    // resort.
    {
        const int32_t desired = std::max(1, GetDesiredBuffered());
        const float error = static_cast<float>(desired - Buffered()) / static_cast<float>(desired);
        // Smoothed over ~20 submissions: a per-chunk jump is audible.
        mRateRatio += 0.05F * (1.0F + 0.06F * std::clamp(error, -0.5F, 1.0F) - mRateRatio);
        const float ratio = mRateRatio;
        const size_t channels = static_cast<size_t>(mNumChannels);
        const size_t inputFrames = len / (sizeof(int16_t) * channels);
        if (inputFrames > 1U) {
            const size_t outputFrames =
                std::max<size_t>(1U, static_cast<size_t>(static_cast<float>(inputFrames) * ratio));
            const auto* input = reinterpret_cast<const int16_t*>(buf);
            mStretched.resize(outputFrames * channels);
            const float step = static_cast<float>(inputFrames - 1U) /
                               static_cast<float>(std::max<size_t>(1U, outputFrames - 1U));
            for (size_t frame = 0; frame < outputFrames; ++frame) {
                const float position = static_cast<float>(frame) * step;
                const size_t index = std::min(static_cast<size_t>(position), inputFrames - 2U);
                const float fraction = position - static_cast<float>(index);
                for (size_t channel = 0; channel < channels; ++channel) {
                    const float a = input[index * channels + channel];
                    const float b = input[(index + 1U) * channels + channel];
                    mStretched[frame * channels + channel] =
                        static_cast<int16_t>(std::lround(a + (b - a) * fraction));
                }
            }
            buf = reinterpret_cast<const uint8_t*>(mStretched.data());
            len = mStretched.size() * sizeof(int16_t);
        }
    }
#endif
    if (Buffered() < 6000) {
        // Don't fill the audio buffer too much in case this happens
        SDL_QueueAudio(mDevice, buf, len);
    }
#if defined(__ORBIS__)
    if (mRefilling && Buffered() >= GetDesiredBuffered()) {
        SDL_PauseAudioDevice(mDevice, 0);
        mRefilling = false;
    }
#endif
}


void SDLAudioPlayer::DoFlush() {
    if (mDevice != 0) {
        SDL_ClearQueuedAudio(mDevice);
    }
}
} // namespace Ship
