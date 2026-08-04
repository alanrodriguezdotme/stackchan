/*
 * SPDX-FileCopyrightText: 2026 Alan Rodriguez
 *
 * SPDX-License-Identifier: MIT
 *
 * Pure, host-testable beat/onset detector.
 *
 * Feed it one "onset energy" value per analysis hop (typically the bass-band
 * mean magnitude) plus a timestamp. It maintains a rolling statistical model of
 * recent energy and fires a beat when the instantaneous energy jumps well above
 * that baseline, subject to a refractory period. It also tracks an AGC-style
 * level (0..1) so downstream choreography can scale motion by loudness, and a
 * smoothed BPM estimate from inter-beat intervals.
 *
 * Approach is inspired by the classic "energy vs. local average" beat detector
 * (Simple Beat Detection, and WLED's audioreactive usermod) but is an original
 * implementation - no GPL code vendored.
 */
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace stackchan::audioreactive {

struct BeatDetectorConfig {
    // Beat fires when energy > localMean + sensitivity * localStdDev.
    float sensitivity = 1.6f;

    // ...and also energy > localMean * meanFactor (guards against firing on tiny
    // fluctuations when the signal is nearly flat / silent).
    float meanFactor = 1.3f;

    // Absolute noise floor. Energy below this can never be a beat.
    float noiseFloor = 0.0008f;

    // Minimum gap between beats. 300 ms ~= 200 BPM ceiling; rejects double-triggers.
    uint32_t refractoryMs = 260;

    // Length of the rolling history window used for the local statistics.
    // ~43 hops ~= 0.45 s at a 256-sample hop / 24 kHz.
    size_t historyLen = 43;

    // AGC peak tracker: fast attack toward new peaks, slow release when quiet.
    float agcAttack  = 0.6f;
    float agcRelease = 0.02f;

    // BPM smoothing (exponential) and clamp range.
    float bpmSmoothing = 0.2f;
    float bpmMin       = 50.0f;
    float bpmMax       = 200.0f;
};

struct BeatResult {
    bool beat    = false;  // true on the hop a beat is detected
    float level  = 0.0f;   // AGC-normalized loudness, 0..1
    float energy = 0.0f;   // raw energy passed in
    float bpm    = 0.0f;   // smoothed running estimate (0 until enough data)
};

class BeatDetector {
public:
    explicit BeatDetector(BeatDetectorConfig config = {});

    // Process one onset-energy sample captured at tMs. Returns detection state.
    BeatResult process(float energy, uint32_t tMs);

    void reset();

    void setConfig(const BeatDetectorConfig& config);
    const BeatDetectorConfig& config() const
    {
        return _config;
    }

private:
    BeatDetectorConfig _config;

    std::vector<float> _history;
    size_t _history_pos   = 0;
    size_t _history_count = 0;

    float _agc_peak   = 0.0f;
    bool _has_beat    = false;
    uint32_t _last_beat_ms = 0;
    float _bpm        = 0.0f;

    void push_history(float energy);
    void compute_stats(float& mean, float& stddev) const;
};

}  // namespace stackchan::audioreactive
