/*
 * SPDX-FileCopyrightText: 2026 Alan Rodriguez
 *
 * SPDX-License-Identifier: MIT
 */
#include "beat_detector.h"
#include <algorithm>
#include <cmath>

namespace stackchan::audioreactive {

BeatDetector::BeatDetector(BeatDetectorConfig config)
{
    setConfig(config);
}

void BeatDetector::setConfig(const BeatDetectorConfig& config)
{
    _config = config;
    if (_config.historyLen < 4) {
        _config.historyLen = 4;
    }
    _history.assign(_config.historyLen, 0.0f);
    _history_pos   = 0;
    _history_count = 0;
}

void BeatDetector::reset()
{
    std::fill(_history.begin(), _history.end(), 0.0f);
    _history_pos   = 0;
    _history_count = 0;
    _agc_peak      = 0.0f;
    _has_beat      = false;
    _last_beat_ms  = 0;
    _bpm           = 0.0f;
}

void BeatDetector::push_history(float energy)
{
    _history[_history_pos] = energy;
    _history_pos           = (_history_pos + 1) % _history.size();
    if (_history_count < _history.size()) {
        ++_history_count;
    }
}

void BeatDetector::compute_stats(float& mean, float& stddev) const
{
    mean   = 0.0f;
    stddev = 0.0f;
    if (_history_count == 0) {
        return;
    }
    for (size_t i = 0; i < _history_count; ++i) {
        mean += _history[i];
    }
    mean /= static_cast<float>(_history_count);

    float var = 0.0f;
    for (size_t i = 0; i < _history_count; ++i) {
        const float d = _history[i] - mean;
        var += d * d;
    }
    var /= static_cast<float>(_history_count);
    stddev = std::sqrt(var);
}

BeatResult BeatDetector::process(float energy, uint32_t tMs)
{
    BeatResult result;
    result.energy = energy;

    // AGC peak tracking: jump up fast, decay down slow.
    if (energy > _agc_peak) {
        _agc_peak += (energy - _agc_peak) * _config.agcAttack;
    } else {
        _agc_peak += (energy - _agc_peak) * _config.agcRelease;
    }
    if (_agc_peak > 1e-6f) {
        result.level = std::min(1.0f, std::max(0.0f, energy / _agc_peak));
    }

    float mean   = 0.0f;
    float stddev = 0.0f;
    compute_stats(mean, stddev);

    const float threshold = std::max(mean + _config.sensitivity * stddev, mean * _config.meanFactor);

    const bool warm       = _history_count >= 4;
    const bool loud       = energy >= _config.noiseFloor;
    const bool overThresh = energy > threshold;
    const bool refractory = _has_beat && (tMs - _last_beat_ms) < _config.refractoryMs;

    if (warm && loud && overThresh && !refractory) {
        result.beat = true;

        if (_has_beat) {
            const uint32_t interval = tMs - _last_beat_ms;
            if (interval > 0) {
                float inst = 60000.0f / static_cast<float>(interval);
                inst       = std::min(std::max(inst, _config.bpmMin), _config.bpmMax);
                if (_bpm <= 0.0f) {
                    _bpm = inst;
                } else {
                    _bpm += (inst - _bpm) * _config.bpmSmoothing;
                }
            }
        }
        _has_beat     = true;
        _last_beat_ms = tMs;
    }

    // Always feed the running statistics so the baseline tracks the signal.
    push_history(energy);

    result.bpm = _bpm;
    return result;
}

}  // namespace stackchan::audioreactive
