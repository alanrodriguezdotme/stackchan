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
#include <vector>

namespace stackchan::audioreactive {

struct DanceConfig {
    float yawAmplitude   = 0.16f;  // subtle side sway on a beat (nod is the star)
    float pitchAmplitude = 0.34f;  // primary downward nod at full level
    float decayPerSecond = 4.5f;   // how fast a beat "hit" relaxes back toward still
    float hueStepPerBeat = 30.0f;  // degrees the color wheel advances per beat
    float mouthGain      = 6.0f;   // scales mid+treble energy into mouth openness
    float expressionOnLevel = 0.35f;  // level above which the face goes Happy

    // Arming gate: stay dead still until a real, sustained rhythm is heard, and go
    // still again when the music stops. This is what keeps it from dancing to
    // ambient room noise or to the sound of its own servos.
    int armBeatsRequired    = 3;     // beats needed to start dancing...
    uint32_t armWindowMs    = 2600;  // ...within this rolling window
    uint32_t disarmQuietMs  = 1500;  // no beats for this long -> stop, hold still

    int beatSpeed = 650;  // advisory servo speed on a beat hit (0..1000)
    int idleSpeed = 300;  // servo speed used to ease home when disarming
};

struct DanceCommand {
    float yaw   = 0.0f;  // normalized target for motion.lookAtNormalized (x)
    float pitch = 0.0f;  // normalized target for motion.lookAtNormalized (y)
    int speed   = 350;   // servo speed 0..1000
    float mouthOpen = 0.0f;  // 0..1 mouth openness
    int emotion = 0;         // stackchan::avatar::Emotion as int (0 = Neutral, 1 = Happy)
    uint8_t r = 0, g = 0, b = 0;  // neon light color
    bool beat   = false;          // pass-through: true on the frame a beat hit
    bool active = false;          // true while armed/dancing; false = hold still
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

    /**
     * @brief True while armed (a rhythm has been locked and we're dancing).
     */
    bool active() const
    {
        return _armed;
    }

private:
    DanceConfig _config;

    bool _armed             = false;  // arming state: still until real music
    std::vector<uint32_t> _beat_times;  // recent beat timestamps (for arming)
    uint32_t _last_beat_ms  = 0;
    bool _has_beat          = false;

    int _sway_dir       = 1;      // +1 / -1, flips each beat
    float _hit          = 0.0f;   // 0..1 envelope of the most recent beat hit
    float _hue          = 0.0f;   // degrees
    uint32_t _last_ms   = 0;
    bool _has_last      = false;
    float _level_smooth = 0.0f;
    float _mouth_smooth = 0.0f;
};

}  // namespace stackchan::audioreactive
