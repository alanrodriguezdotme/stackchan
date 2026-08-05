/*
 * SPDX-FileCopyrightText: 2026 Alan Rodriguez
 *
 * SPDX-License-Identifier: MIT
 *
 * ReactiveDanceModifier: the on-device glue for beat-reactive dancing.
 *
 * Each frame it pulls the most recent mic window from the HAL streaming tap,
 * runs the pure DSP core (FFT -> band energies -> beat detector -> choreography)
 * and applies the resulting DanceCommand to the head servos, the avatar face and
 * the neon lights. All the "brains" live in stackchan::audioreactive (pure, host
 * tested); this class is only I/O + application, mirroring how BreathModifier
 * drives the avatar.
 */
#pragma once
#include "../modifiable.h"
#include "../audioreactive/audio_analysis.h"
#include "../audioreactive/beat_detector.h"
#include "../audioreactive/dance_director.h"
#include "../avatar/avatar/elements/emotion.h"
#include <hal/hal.h>
#include <atomic>
#include <array>
#include <cstdint>

namespace stackchan {

class ReactiveDanceModifier : public Modifier {
public:
    // Lightweight snapshot for the on-screen debug view. Read from the UI thread;
    // written from the stackchan update task. Plain atomics keep it race-free
    // without a lock in the hot path.
    struct DebugState {
        std::atomic<float> bass{0.0f};
        std::atomic<float> mid{0.0f};
        std::atomic<float> treble{0.0f};
        std::atomic<float> level{0.0f};
        std::atomic<float> bpm{0.0f};
        std::atomic<bool> beat{false};
        std::atomic<bool> active{false};
        std::atomic<uint32_t> lastBeatMs{0};
    };

    ReactiveDanceModifier()
    {
        // Onset energy for the beat detector is bass-dominant with a little mid
        // so snare-driven tracks still register.
    }

    void _update(Modifiable& stackchan) override
    {
        if (!stackchan.hasAvatar()) {
            return;
        }

        const uint32_t now = GetHAL().millis();

        // Pull the most recent analysis window (non-blocking; false until warm).
        // ch0 = mic, ch1 = speaker reference (hardware loopback) for echo suppression.
        int16_t* ref = _aec_enabled ? _ref.data() : nullptr;
        if (!GetHAL().readAudioReactiveFrame(_frame.data(), ref, _frame.size())) {
            return;
        }

        const auto bands = _aec_enabled
                               ? audioreactive::analyzeFrameStereo(
                                     _frame.data(), _ref.data(), _frame.size(), _ref_gain)
                               : audioreactive::analyzeFrame(_frame.data(), _frame.size());
        const float onset = bands.bass + _onset_mid_mix * bands.mid;

        const auto beat = _detector.process(onset, now);
        const auto cmd  = _director.onFrame(beat.beat, bands, beat.level, now);

        apply(stackchan, cmd);

        // Publish debug snapshot.
        _debug.bass.store(bands.bass);
        _debug.mid.store(bands.mid);
        _debug.treble.store(bands.treble);
        _debug.level.store(beat.level);
        _debug.bpm.store(beat.bpm);
        _debug.beat.store(beat.beat);
        _debug.active.store(cmd.active);
        if (beat.beat) {
            _debug.lastBeatMs.store(now);
        }
    }

    const DebugState& debug() const
    {
        return _debug;
    }

    /* --------------------------------- Tuning --------------------------------- */
    void setBeatConfig(const audioreactive::BeatDetectorConfig& cfg)
    {
        _detector.setConfig(cfg);
    }
    audioreactive::BeatDetectorConfig beatConfig() const
    {
        return _detector.config();
    }
    void setDanceConfig(const audioreactive::DanceConfig& cfg)
    {
        _director.setConfig(cfg);
    }
    audioreactive::DanceConfig danceConfig() const
    {
        return _director.config();
    }
    void setOnsetMidMix(float mix)
    {
        _onset_mid_mix = mix;
    }
    void setNodSpring(float stiffness, float damping)
    {
        _nod_stiffness = stiffness;
        _nod_damping   = damping;
    }
    void setAecEnabled(bool enabled)
    {
        _aec_enabled = enabled;
    }
    bool aecEnabled() const
    {
        return _aec_enabled;
    }
    void setAecReferenceGain(float gain)
    {
        _ref_gain = gain;
    }
    float aecReferenceGain() const
    {
        return _ref_gain;
    }

    // Coarse sensitivity presets so the beat detector can be retuned on-device
    // without a rebuild. 0 = Low (fewest false beats), 1 = Med, 2 = High.
    static constexpr int kNumSensitivityPresets = 3;
    void setSensitivityPreset(int preset)
    {
        preset      = std::max(0, std::min(kNumSensitivityPresets - 1, preset));
        _sens_preset = preset;
        auto cfg     = _detector.config();
        // Lower sensitivity value = lower threshold multiplier = more beats. Also
        // widen the refractory at low sensitivity so it locks onto strong beats only.
        switch (preset) {
            case 0:  // Low - only strong, obvious beats; fewest false triggers.
                cfg.sensitivity  = 2.6f;
                cfg.meanFactor   = 1.6f;
                cfg.refractoryMs = 360;
                break;
            case 1:  // Med
                cfg.sensitivity  = 1.8f;
                cfg.meanFactor   = 1.35f;
                cfg.refractoryMs = 320;
                break;
            case 2:  // High - twitchier, picks up subtle beats.
                cfg.sensitivity  = 1.3f;
                cfg.meanFactor   = 1.2f;
                cfg.refractoryMs = 280;
                break;
        }
        _detector.setConfig(cfg);
    }
    int sensitivityPreset() const
    {
        return _sens_preset;
    }
    int cycleSensitivityPreset()
    {
        setSensitivityPreset((_sens_preset + 1) % kNumSensitivityPresets);
        return _sens_preset;
    }
    static const char* sensitivityPresetName(int preset)
    {
        switch (preset) {
            case 0:
                return "LOW";
            case 2:
                return "HIGH";
            default:
                return "MED";
        }
    }

private:
    void apply(Modifiable& stackchan, const audioreactive::DanceCommand& cmd)
    {
        auto& avatar = stackchan.avatar();

        // Idle: hold still. Only act on the transition into idle so we don't spam
        // the servos with home commands (which would keep them buzzing / noisy).
        if (!cmd.active) {
            if (_was_active) {
                stackchan.motion().goHome(cmd.speed);
                if (_last_emotion != avatar::Emotion::Neutral) {
                    avatar.setEmotion(avatar::Emotion::Neutral);
                    _last_emotion = avatar::Emotion::Neutral;
                }
                avatar.mouth().setWeight(0);
                stackchan.leftNeonLight().setColor(0, 0, 0);
                stackchan.rightNeonLight().setColor(0, 0, 0);
                _was_active = false;
            }
            return;
        }

        // Active: nod with a slightly-underdamped spring so the travel is close to
        // linear with a tiny bounce at the end, instead of a critically-damped snap.
        stackchan.motion().lookAtNormalizedSpring(cmd.yaw, cmd.pitch, _nod_stiffness, _nod_damping);

        const auto emotion =
            (cmd.emotion == 1) ? avatar::Emotion::Happy : avatar::Emotion::Neutral;
        if (emotion != _last_emotion) {
            avatar.setEmotion(emotion);
            _last_emotion = emotion;
        }
        avatar.mouth().setWeight(static_cast<int>(cmd.mouthOpen * 100.0f));

        // Neon lights.
        stackchan.leftNeonLight().setColor(cmd.r, cmd.g, cmd.b);
        stackchan.rightNeonLight().setColor(cmd.r, cmd.g, cmd.b);

        _was_active = true;
    }

    audioreactive::BeatDetector _detector;
    audioreactive::DanceDirector _director;
    std::array<int16_t, audioreactive::kFftSize> _frame{};
    std::array<int16_t, audioreactive::kFftSize> _ref{};
    // Onset for the beat detector is bass-dominant with only a touch of mid, so the
    // robot's own broadband servo/mechanical noise contributes little and can't
    // easily self-trigger a beat.
    float _onset_mid_mix          = 0.15f;
    bool _aec_enabled             = true;
    float _ref_gain               = 1.0f;
    int _sens_preset              = 1;
    // Nod spring: slightly under 2*sqrt(stiffness) critical damping for a small bounce.
    float _nod_stiffness          = 150.0f;
    float _nod_damping            = 21.0f;
    bool _was_active              = false;
    avatar::Emotion _last_emotion = avatar::Emotion::Neutral;
    DebugState _debug;
};

}  // namespace stackchan
