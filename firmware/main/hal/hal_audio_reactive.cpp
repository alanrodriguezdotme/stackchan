/*
 * SPDX-FileCopyrightText: 2026 Alan Rodriguez
 *
 * SPDX-License-Identifier: MIT
 *
 * Streaming mic capture for the beat-reactive dance mode.
 *
 * A dedicated FreeRTOS task reads the audio codec continuously and deinterleaves
 * the mic channel (ch0) and the speaker-reference channel (ch1, used later for
 * self-audio rejection) into two ring buffers. Consumers grab the most recent N
 * samples with readAudioReactiveFrame(); this never blocks on the codec the way
 * getMicWaveformFrame() does, so the UI/analysis loop stays responsive.
 */
#include "hal.h"
#include <algorithm>
#include <atomic>
#include <mutex>
#include <vector>
#include <board.h>
#include <audio/audio_codec.h>
#include <hal/board/config.h>
#include <mooncake_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const std::string_view _tag = "HAL-AudioRx";

namespace {

// Power-of-two ring so the analysis window (512) plus a little slack always fits.
constexpr size_t kRingSize      = 2048;
constexpr size_t kCaptureFrames = 256;  // frames per codec read (== analysis hop)

struct ReactiveCaptureState {
    std::mutex mutex;
    int16_t mic_ring[kRingSize] = {0};
    int16_t ref_ring[kRingSize] = {0};
    size_t write_pos            = 0;
    size_t total_written        = 0;

    std::atomic<bool> running{false};
    TaskHandle_t task = nullptr;
};

ReactiveCaptureState& state()
{
    static ReactiveCaptureState s;
    return s;
}

void capture_task(void* /*arg*/)
{
    auto& s          = state();
    auto& board      = Board::GetInstance();
    auto audio_codec = board.GetAudioCodec();

    if (!audio_codec) {
        mclog::tagError(_tag, "audio codec unavailable, capture task exiting");
        s.running = false;
        s.task    = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    const size_t channels = std::max(audio_codec->input_channels(), 1);
    if (!audio_codec->input_enabled()) {
        audio_codec->EnableInput(true);
    }

    std::vector<int16_t> chunk(kCaptureFrames * channels);

    while (s.running.load()) {
        if (!audio_codec->InputData(chunk)) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        std::lock_guard<std::mutex> lock(s.mutex);
        for (size_t f = 0; f < kCaptureFrames; ++f) {
            const size_t base   = f * channels;
            s.mic_ring[s.write_pos] = chunk[base];
            s.ref_ring[s.write_pos] = (channels > 1) ? chunk[base + 1] : chunk[base];
            s.write_pos             = (s.write_pos + 1) & (kRingSize - 1);
            ++s.total_written;
        }
    }

    s.task = nullptr;
    vTaskDelete(nullptr);
}

}  // namespace

bool Hal::startAudioReactiveCapture()
{
    auto& s = state();
    if (s.running.load()) {
        return true;
    }

    {
        std::lock_guard<std::mutex> lock(s.mutex);
        s.write_pos     = 0;
        s.total_written = 0;
    }

    s.running = true;
    // Pin to core 1 alongside the other sensor tasks; keep priority modest.
    if (xTaskCreatePinnedToCore(capture_task, "audio_rx", 4096, nullptr, 3, &s.task, 1) != pdPASS) {
        mclog::tagError(_tag, "failed to create capture task");
        s.running = false;
        return false;
    }
    mclog::tagInfo(_tag, "reactive audio capture started");
    return true;
}

void Hal::stopAudioReactiveCapture()
{
    auto& s = state();
    if (!s.running.load()) {
        return;
    }
    s.running = false;
    // Let the task observe the flag and self-delete.
    for (int i = 0; i < 40 && s.task != nullptr; ++i) {
        vTaskDelay(pdMS_TO_TICKS(5));
    }

    auto& board      = Board::GetInstance();
    auto audio_codec = board.GetAudioCodec();
    if (audio_codec && audio_codec->input_enabled()) {
        audio_codec->EnableInput(false);
    }
    mclog::tagInfo(_tag, "reactive audio capture stopped");
}

bool Hal::isAudioReactiveCapturing()
{
    return state().running.load();
}

bool Hal::readAudioReactiveFrame(int16_t* mic, int16_t* ref, size_t count)
{
    if (count == 0 || count > kRingSize) {
        return false;
    }
    auto& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.total_written < count) {
        return false;
    }

    // Oldest of the last `count` samples: write_pos - count (mod ring).
    size_t start = (s.write_pos + kRingSize - count) & (kRingSize - 1);
    for (size_t i = 0; i < count; ++i) {
        const size_t idx = (start + i) & (kRingSize - 1);
        if (mic) {
            mic[i] = s.mic_ring[idx];
        }
        if (ref) {
            ref[i] = s.ref_ring[idx];
        }
    }
    return true;
}
