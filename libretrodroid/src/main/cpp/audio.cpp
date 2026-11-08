/*
 *     Copyright (C) 2019  Filippo Scognamiglio
 *
 *     This program is free software: you can redistribute it and/or modify
 *     it under the terms of the GNU General Public License as published by
 *     the Free Software Foundation, either version 3 of the License, or
 *     (at your option) any later version.
 *
 *     This program is distributed in the hope that it will be useful,
 *     but WITHOUT ANY WARRANTY; without even the implied warranty of
 *     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *     GNU General Public License for more details.
 *
 *     You should have received a copy of the GNU General Public License
 *     along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "log.h"

#include "audio.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>

namespace libretrodroid {

static constexpr int64_t AUDIO_STATE_TIMEOUT_NANOS = 500 * oboe::kNanosPerMillisecond;
static constexpr int32_t AUDIO_UNDERRUN_FADE_FRAMES = 64;
static constexpr int32_t AUDIO_FIFO_CAPACITY_MULTIPLIER = 4;
/* Extra capacity absorbs extended bursts after remote-link stalls. Express the
 * targets relative to the original FIFO so capacity can grow without adding
 * playback latency: about two video frames normally and three during WLAN. */
static constexpr double AUDIO_NORMAL_TARGET_FILL =
    0.5 / AUDIO_FIFO_CAPACITY_MULTIPLIER;
static constexpr double AUDIO_NETWORK_TARGET_FILL =
    0.75 / AUDIO_FIFO_CAPACITY_MULTIPLIER;

static void fadeAudio(int16_t *buffer, int32_t frames, bool fadeIn) {
    int32_t fadeFrames = std::min(frames, AUDIO_UNDERRUN_FADE_FRAMES);
    if (!buffer || fadeFrames <= 0) {
        return;
    }
    int32_t startFrame = fadeIn ? 0 : frames - fadeFrames;
    for (int32_t frame = 0; frame < fadeFrames; ++frame) {
        int32_t numerator = fadeIn ? frame + 1 : fadeFrames - frame;
        int32_t offset = (startFrame + frame) * 2;
        buffer[offset] = static_cast<int16_t>(
            static_cast<int32_t>(buffer[offset]) * numerator / (fadeFrames + 1));
        buffer[offset + 1] = static_cast<int16_t>(
            static_cast<int32_t>(buffer[offset + 1]) * numerator / (fadeFrames + 1));
    }
}

Audio::Audio(int32_t sampleRate, double refreshRate, bool preferLowLatencyAudio) {
    LOGI("Audio initialization has been called with input sample rate %d", sampleRate);

    contentRefreshRate = refreshRate;
    inputSampleRate = sampleRate;
    audioLatencySettings = findBestLatencySettings(preferLowLatencyAudio);
    initializeStream();
}

bool Audio::initializeStream() {
    LOGI("Using low latency stream: %d", audioLatencySettings->useLowLatencyStream);

    int32_t audioBufferSize = computeAudioBufferSize();
    int32_t fifoBufferSize = audioBufferSize * AUDIO_FIFO_CAPACITY_MULTIPLIER;

    oboe::AudioStreamBuilder builder;
    builder.setChannelCount(2);
    builder.setDirection(oboe::Direction::Output);
    builder.setFormat(oboe::AudioFormat::I16);
    builder.setDataCallback(this);
    builder.setErrorCallback(this);

    if (audioLatencySettings->useLowLatencyStream) {
        builder.setPerformanceMode(oboe::PerformanceMode::LowLatency);
    } else {
        builder.setFramesPerCallback(audioBufferSize / 10);
    }

    oboe::Result result = builder.openManagedStream(stream);
    if (result == oboe::Result::OK) {
        baseConversionFactor = (double) inputSampleRate / stream->getSampleRate();
        fifoBuffer = std::make_unique<oboe::FifoBuffer>(2, fifoBufferSize);
        temporaryAudioBuffer = std::unique_ptr<int16_t[]>(new int16_t[fifoBufferSize]);
        latencyTuner = std::make_unique<oboe::LatencyTuner>(*stream);
        return true;
    } else {
        LOGE("Failed to create stream. Error: %s", oboe::convertToText(result));
        stream = nullptr;
        latencyTuner = nullptr;
        return false;
    }
}

std::unique_ptr<Audio::AudioLatencySettings> Audio::findBestLatencySettings(bool preferLowLatencyAudio) {
    if (oboe::AudioStreamBuilder::isAAudioRecommended() && preferLowLatencyAudio) {
        return std::make_unique<AudioLatencySettings>(LOW_LATENCY_SETTINGS);
    } else {
        return std::make_unique<AudioLatencySettings>(DEFAULT_LATENCY_SETTINGS);
    }
}

int32_t Audio::computeAudioBufferSize() {
    double maxLatency = computeMaximumLatency();
    LOGI("Average audio latency set to: %f ms", maxLatency * 0.5);
    double sampleRateDivisor = 500.0 / maxLatency;
    return roundToEven(inputSampleRate / sampleRateDivisor);
}

double Audio::computeMaximumLatency() const {
    double maxLatency = (audioLatencySettings->bufferSizeInVideoFrames / contentRefreshRate) * 1000;
    return std::max(maxLatency, 32.0);
}

void Audio::start() {
    if (startRequested.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    std::lock_guard<std::mutex> lock(streamStateMutex);
    if (!startRequested.load(std::memory_order_acquire)) {
        return;
    }
    if (stream != nullptr) {
        oboe::Result result = stream->start(AUDIO_STATE_TIMEOUT_NANOS);
        if (result != oboe::Result::OK) {
            LOGW("Failed to start audio stream: %s", oboe::convertToText(result));
        }
    }
}

void Audio::stop() {
    if (!startRequested.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    std::lock_guard<std::mutex> lock(streamStateMutex);
    if (startRequested.load(std::memory_order_acquire)) {
        return;
    }
    if (stream != nullptr) {
        oboe::Result result = stream->stop(AUDIO_STATE_TIMEOUT_NANOS);
        if (result != oboe::Result::OK) {
            LOGW("Failed to stop audio stream: %s", oboe::convertToText(result));
        }
    }
}

bool Audio::resetForContent(int32_t sampleRate, double refreshRate) {
    std::lock_guard<std::mutex> lock(streamStateMutex);
    if (inputSampleRate != sampleRate ||
        std::abs(contentRefreshRate - refreshRate) > 0.000001) {
        return false;
    }

    /* Reloading another ROM from the same core does not change the audio
     * format. Keep the existing Oboe stream instead of closing and reopening
     * one for every console rebuild; asynchronous stream teardown can otherwise
     * outlive the Audio instance after many rapid rebuilds. The caller pauses
     * emulation before reload, so it is safe to discard the old content's FIFO
     * and controller history here. */
    if (fifoBuffer) {
        fifoBuffer->setReadCounter(fifoBuffer->getWriteCounter());
    }
    framesToSubmit = 0.0;
    errorIntegral = 0.0;
    audioUnderrunActive = true;
    bufferResetRequested.store(false, std::memory_order_release);
    audioCallbackCount.store(0, std::memory_order_release);
    audioUnderrunCount.store(0, std::memory_order_release);
    audioOverflowCount.store(0, std::memory_order_release);
    return true;
}

void Audio::resetBuffer() {
    /* The Oboe callback is the FIFO consumer, so let it advance the read
     * counter and reset its controller state. This avoids racing a callback
     * that may still be completing while emulation leaves link mode. */
    if (!bufferResetRequested.exchange(true, std::memory_order_acq_rel)) {
        uint32_t callbacks = audioCallbackCount.exchange(0, std::memory_order_acq_rel);
        uint32_t underruns = audioUnderrunCount.exchange(0, std::memory_order_acq_rel);
        uint32_t overflows = audioOverflowCount.exchange(0, std::memory_order_acq_rel);
        /* LINK_DIAGNOSTIC_DISABLED: LOGI("Audio buffer reset after %u callbacks, underruns=%u, overflows=%u, networkJitter=%d",
             callbacks, underruns, overflows,
             networkJitterBuffering.load(std::memory_order_relaxed)) */;
    }
}

void Audio::setNetworkJitterBuffering(bool enabled) {
    networkJitterBuffering.store(enabled, std::memory_order_release);
}

void Audio::write(const int16_t *data, size_t frames) {
    int32_t requestedSamples = static_cast<int32_t>(frames * 2);
    int32_t writtenSamples = fifoBuffer->write(data, requestedSamples);
    if (writtenSamples < requestedSamples) {
        audioOverflowCount.fetch_add(1, std::memory_order_relaxed);
    }
}

void Audio::setPlaybackSpeed(const double newPlaybackSpeed) {
    playbackSpeed.store(newPlaybackSpeed, std::memory_order_relaxed);
}

oboe::DataCallbackResult Audio::onAudioReady(oboe::AudioStream *oboeStream, void *audioData, int32_t numFrames) {
    audioCallbackCount.fetch_add(1, std::memory_order_relaxed);
    auto outputArray = reinterpret_cast<int16_t *>(audioData);
    if (bufferResetRequested.exchange(false, std::memory_order_acq_rel)) {
        fifoBuffer->setReadCounter(fifoBuffer->getWriteCounter());
        framesToSubmit = 0.0;
        errorIntegral = 0.0;
        audioUnderrunActive = true;
        memset(outputArray, 0, sizeof(int16_t) * static_cast<size_t>(numFrames) * 2);
        latencyTuner->tune();
        return oboe::DataCallbackResult::Continue;
    }

    const int32_t outputSampleRate = oboeStream ? oboeStream->getSampleRate() : 0;
    const double callbackSeconds = outputSampleRate > 0
        ? static_cast<double>(numFrames) / outputSampleRate
        : 0.0;
    double dynamicBufferFactor = computeDynamicBufferConversionFactor(callbackSeconds);
    double finalConversionFactor =
        baseConversionFactor * dynamicBufferFactor * playbackSpeed.load(std::memory_order_relaxed);

    // When using low-latency stream, numFrames is very low (~100) and the dynamic buffer scaling doesn't work with rounding.
    // By keeping track of the "fractional" frames we can keep the error smaller.
    framesToSubmit += numFrames * finalConversionFactor;

    // FIFO stores mono int16 "frames" in this pipeline, so stereo libretro frames are half capacity.
    const int32_t maxInputFrames = static_cast<int32_t>(fifoBuffer->getBufferCapacityInFrames() / 2);
    int32_t currentFramesToSubmit = std::round(framesToSubmit);
    currentFramesToSubmit = std::clamp(currentFramesToSubmit, 0, maxInputFrames);
    framesToSubmit -= currentFramesToSubmit;

    // Avoid unbounded growth when requested speed is much higher than available audio production.
    framesToSubmit = std::clamp(framesToSubmit, -static_cast<double>(maxInputFrames), static_cast<double>(maxInputFrames));

    if (currentFramesToSubmit <= 0) {
        memset(outputArray, 0, sizeof(int16_t) * static_cast<size_t>(numFrames) * 2);
        errorIntegral = 0.0;
        if (!audioUnderrunActive) {
            audioUnderrunCount.fetch_add(1, std::memory_order_relaxed);
        }
        audioUnderrunActive = true;
        latencyTuner->tune();
        return oboe::DataCallbackResult::Continue;
    }

    /* Do not restart from a handful of newly produced samples after an
     * underrun. Refill to the controller target so remote-link jitter cannot
     * immediately empty the FIFO again. */
    const int32_t requestedInputSamples = currentFramesToSubmit * 2;
    const int32_t fifoCapacitySamples =
        static_cast<int32_t>(fifoBuffer->getBufferCapacityInFrames());
    const double targetFill = networkJitterBuffering.load(std::memory_order_acquire)
        ? AUDIO_NETWORK_TARGET_FILL
        : AUDIO_NORMAL_TARGET_FILL;
    const int32_t recoveryInputSamples = std::min(fifoCapacitySamples,
        std::max(requestedInputSamples,
                 static_cast<int32_t>(std::ceil(fifoCapacitySamples * targetFill))));
    if (audioUnderrunActive &&
        static_cast<int32_t>(fifoBuffer->getFullFramesAvailable()) < recoveryInputSamples) {
        memset(outputArray, 0, sizeof(int16_t) * static_cast<size_t>(numFrames) * 2);
        errorIntegral = 0.0;
        latencyTuner->tune();
        return oboe::DataCallbackResult::Continue;
    }

    int32_t samplesRead = fifoBuffer->readNow(
        temporaryAudioBuffer.get(), requestedInputSamples);
    int32_t inputFramesRead = std::max(0, samplesRead / 2);
    int32_t outputFramesWithAudio = inputFramesRead >= currentFramesToSubmit
        ? numFrames
        : static_cast<int32_t>(std::floor(
            static_cast<double>(numFrames) * inputFramesRead / currentFramesToSubmit));

    if (outputFramesWithAudio > 0) {
        resampler.resample(temporaryAudioBuffer.get(), inputFramesRead,
                           outputArray, outputFramesWithAudio);
        if (audioUnderrunActive) {
            fadeAudio(outputArray, outputFramesWithAudio, true);
        }
    }
    if (outputFramesWithAudio < numFrames) {
        if (outputFramesWithAudio > 0) {
            fadeAudio(outputArray, outputFramesWithAudio, false);
        }
        memset(outputArray + outputFramesWithAudio * 2, 0,
               sizeof(int16_t) * static_cast<size_t>(numFrames - outputFramesWithAudio) * 2);
        /* A prolonged empty FIFO must not leave the integral correction at an
         * audible pitch offset after standalone emulation resumes. */
        errorIntegral = 0.0;
        if (!audioUnderrunActive) {
            audioUnderrunCount.fetch_add(1, std::memory_order_relaxed);
        }
        audioUnderrunActive = true;
    } else {
        audioUnderrunActive = false;
    }

    latencyTuner->tune();

    return oboe::DataCallbackResult::Continue;
}

// To prevent audio buffer overruns or underruns we set up a PI controller. The idea is to run the
// audio slower when the buffer is empty and faster when it's full.
double Audio::computeDynamicBufferConversionFactor(double dt) {
    double framesCapacityInBuffer = fifoBuffer->getBufferCapacityInFrames();
    double framesAvailableInBuffer = fifoBuffer->getFullFramesAvailable();

    double targetFill = networkJitterBuffering.load(std::memory_order_acquire)
        ? AUDIO_NETWORK_TARGET_FILL
        : AUDIO_NORMAL_TARGET_FILL;
    double targetFramesInBuffer = framesCapacityInBuffer * targetFill;
    // Normalize against the ordinary target. Doubling capacity therefore does
    // not weaken the controller compared with the original half-full FIFO.
    double errorMeasure = std::clamp(
        (targetFramesInBuffer - framesAvailableInBuffer) /
            (framesCapacityInBuffer * AUDIO_NORMAL_TARGET_FILL),
        -1.0, 1.0);

    errorIntegral += errorMeasure * dt;

    // Wikipedia states that human ear resolution is around 3.6 Hz within the octave of 1000–2000 Hz.
    // This changes continuously, so we should try to keep it a very low value.
    double proportionalAdjustment = std::clamp(kp * errorMeasure, -maxp, maxp);

    // Ki is a lot lower, so it's safe if it exceeds the ear threshold. Hopefully convergence will
    // be slow enough to be not perceptible. We need to battle test this value.
    double integralAdjustment = std::clamp(ki * errorIntegral, -maxi, maxi);

    double finalAdjustment = proportionalAdjustment + integralAdjustment;

    LOGD("Audio speed adjustments (p: %f) (i: %f)", proportionalAdjustment, integralAdjustment);

    return 1.0 - (finalAdjustment);
}

int32_t Audio::roundToEven(int32_t x) {
    return (x / 2) * 2;
}

void Audio::onErrorAfterClose(oboe::AudioStream* oldStream, oboe::Result result) {
    AudioStreamErrorCallback::onErrorAfterClose(oldStream, result);
    LOGI("Stream error in oboe::onErrorAfterClose %s", oboe::convertToText(result));

    if (result != oboe::Result::ErrorDisconnected)
        return;

    std::lock_guard<std::mutex> lock(streamStateMutex);
    initializeStream();
    if (startRequested.load(std::memory_order_acquire) && stream != nullptr) {
        oboe::Result startResult = stream->start(AUDIO_STATE_TIMEOUT_NANOS);
        if (startResult != oboe::Result::OK) {
            LOGW("Failed to restart audio stream: %s", oboe::convertToText(startResult));
        }
    }
}

} //namespace libretrodroid
