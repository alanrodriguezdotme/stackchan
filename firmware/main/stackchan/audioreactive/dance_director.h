/*
 * SPDX-FileCopyrightText: 2026 Alan Rodriguez
 *
 * SPDX-License-Identifier: MIT
 *
 * Pure, host-testable choreography: turns beat + band-energy state into concrete
 * head / expression / RGB targets. Keeping this side-effect free means the "feel"
 * of the dance (amplitude, decay, color cycling, mouth response) is unit-tested
 * and tunable without a device in the loop. The firmware modifier just applies
 * whatever DanceCommand this produces.
 */
#pragma once
#include "audio_analysis.h"
#include <cstdint>

namespace stackchan::audioreactive {

struct DanceConfig {
    float yawAmplitude   = 0.85f;  // normalized [-1,1] head sway at full level
    float pitchAmplitude = 0.45f;  // normalized head bob at full level
    float idleSwayHz     = 0.6f;   // gentle sway when no beats are landing
    float decayPerSecond = 3.5f;   // how fast a beat "hit" relaxes back toward idle
    int beatSpeed        = 950;    // servo speed on a beat hit (0..1000)
    int idleSpeed        = 350;    // servo speed for smooth idle motion
    float hueStepPerBeat = 47.0f;  // degrees the color wheel advances per beat
    float mouthGain      = 6.0f;   // scales mid+treble energy into mouth openness
    float expressionOnLevel = 0.35f;  // level above which the face goes Happy
};

struct DanceCommand {
    float yaw   = 0.0f;  // normalized target for motion.lookAtNormalized (x)
    float pitch = 0.0f;  // normalized target for motion.lookAtNormalized (y)
    int speed   = 350;   // servo speed 0..1000
    float mouthOpen = 0.0f;  // 0..1 mouth openness
    int emotion = 0;         // stackchan::avatar::Emotion as int (0 = Neutral, 1 = Happy)
    uint8_t r = 0, g = 0, b = 0;  // neon light color
    bool beat = false;            // pass-through: true on the frame a beat hit
};

/**
 * @brief Convert HSV (h in degrees 0..360, s/v 0..1) to 8-bit RGB. Pure helper,
 *        exposed for testing the color mapping.
 */
void hsvToRgb(float h, float s, float v, uint8_t& r, uint8_t& g, uint8_t& b);

class DanceDirector {
public:
    explicit DanceDirector(DanceConfig config = {});

    /**
     * @brief Advance one frame.
     *
     * @param beat   Whether a beat was detected this frame.
     * @param bands  Current band energies (for mouth + color emphasis).
     * @param level  AGC loudness 0..1 from the beat detector.
     * @param tMs    Monotonic time in ms.
     */
    DanceCommand onFrame(bool beat, const BandEnergies& bands, float level, uint32_t tMs);

    void reset();

    void setConfig(const DanceConfig& config)
    {
        _config = config;
    }
    const DanceConfig& config() const
    {
        return _config;
    }

private:
    DanceConfig _config;

    int _sway_dir       = 1;      // +1 / -1, flips each beat
    float _hit          = 0.0f;   // 0..1 envelope of the most recent beat hit
    float _hue          = 0.0f;   // degrees
    uint32_t _last_ms   = 0;
    bool _has_last      = false;
    float _level_smooth = 0.0f;
    float _mouth_smooth = 0.0f;
};

}  // namespace stackchan::audioreactive
