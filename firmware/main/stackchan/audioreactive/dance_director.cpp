/*
 * SPDX-FileCopyrightText: 2026 Alan Rodriguez
 *
 * SPDX-License-Identifier: MIT
 */
#include "dance_director.h"
#include <algorithm>
#include <cmath>

namespace stackchan::audioreactive {

namespace {
constexpr float kPi = 3.14159265358979323846f;

float clamp01(float v)
{
    return std::min(1.0f, std::max(0.0f, v));
}
}  // namespace

void hsvToRgb(float h, float s, float v, uint8_t& r, uint8_t& g, uint8_t& b)
{
    h = std::fmod(h, 360.0f);
    if (h < 0.0f) {
        h += 360.0f;
    }
    s = clamp01(s);
    v = clamp01(v);

    const float c = v * s;
    const float x = c * (1.0f - std::fabs(std::fmod(h / 60.0f, 2.0f) - 1.0f));
    const float m = v - c;

    float rf = 0.0f, gf = 0.0f, bf = 0.0f;
    if (h < 60.0f) {
        rf = c; gf = x; bf = 0.0f;
    } else if (h < 120.0f) {
        rf = x; gf = c; bf = 0.0f;
    } else if (h < 180.0f) {
        rf = 0.0f; gf = c; bf = x;
    } else if (h < 240.0f) {
        rf = 0.0f; gf = x; bf = c;
    } else if (h < 300.0f) {
        rf = x; gf = 0.0f; bf = c;
    } else {
        rf = c; gf = 0.0f; bf = x;
    }

    r = static_cast<uint8_t>(std::lround((rf + m) * 255.0f));
    g = static_cast<uint8_t>(std::lround((gf + m) * 255.0f));
    b = static_cast<uint8_t>(std::lround((bf + m) * 255.0f));
}

DanceDirector::DanceDirector(DanceConfig config) : _config(config)
{
}

void DanceDirector::reset()
{
    _sway_dir     = 1;
    _hit          = 0.0f;
    _hue          = 0.0f;
    _last_ms      = 0;
    _has_last     = false;
    _level_smooth = 0.0f;
    _mouth_smooth = 0.0f;
}

DanceCommand DanceDirector::onFrame(bool beat, const BandEnergies& bands, float level, uint32_t tMs)
{
    float dt = 0.0f;
    if (_has_last && tMs >= _last_ms) {
        dt = static_cast<float>(tMs - _last_ms) / 1000.0f;
    }
    _last_ms  = tMs;
    _has_last = true;
    // Guard against pathological gaps (first frame, long stalls).
    dt = std::min(dt, 0.1f);

    // Smooth the loudness so idle motion + brightness don't jitter.
    _level_smooth += (clamp01(level) - _level_smooth) * 0.25f;

    // Beat -> flip sway direction and re-arm the hit envelope.
    if (beat) {
        _sway_dir = -_sway_dir;
        _hit      = 1.0f;
        _hue += _config.hueStepPerBeat;
    }

    // Relax the hit envelope exponentially between beats.
    _hit -= _hit * _config.decayPerSecond * dt;
    _hit = clamp01(_hit);

    DanceCommand cmd;
    cmd.beat = beat;

    // Head: a sharp beat "hit" toward the current sway side, plus a gentle idle
    // sway that keeps it alive between beats. Both scale with loudness.
    const float idle = std::sin(2.0f * kPi * _config.idleSwayHz * (static_cast<float>(tMs) / 1000.0f)) *
                       0.35f * _level_smooth;
    const float hitYaw = static_cast<float>(_sway_dir) * _config.yawAmplitude * _hit * (0.4f + 0.6f * _level_smooth);
    cmd.yaw            = std::min(1.0f, std::max(-1.0f, hitYaw + idle * (1.0f - _hit)));

    // Pitch bobs down on the hit (nod into the beat) driven by bass.
    const float bassDrive = clamp01(bands.bass * 8.0f);
    cmd.pitch = -_config.pitchAmplitude * _hit * (0.3f + 0.7f * bassDrive);
    cmd.pitch = std::min(1.0f, std::max(-1.0f, cmd.pitch));

    cmd.speed = _hit > 0.5f ? _config.beatSpeed : _config.idleSpeed;

    // Mouth follows mid+treble (vocals / hats) so it "sings along".
    const float mouthTarget = clamp01((bands.mid + bands.treble) * _config.mouthGain);
    _mouth_smooth += (mouthTarget - _mouth_smooth) * 0.4f;
    cmd.mouthOpen = _mouth_smooth;

    // Expression: happy while there's real energy, neutral when it dies down.
    cmd.emotion = (_level_smooth > _config.expressionOnLevel) ? 1 /*Happy*/ : 0 /*Neutral*/;

    // Color: hue rides the beat wheel; brightness tracks loudness with a floor so
    // the lights never go fully dark while dancing.
    const float bright = 0.25f + 0.75f * _level_smooth;
    hsvToRgb(_hue, 1.0f, bright, cmd.r, cmd.g, cmd.b);

    return cmd;
}

}  // namespace stackchan::audioreactive
