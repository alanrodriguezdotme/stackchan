/*
 * SPDX-FileCopyrightText: 2026 Alan Rodriguez
 *
 * SPDX-License-Identifier: MIT
 *
 * Pure, host-testable audio analysis for the reactive dance mode.
 *
 * Nothing in this header may depend on ESP-IDF, FreeRTOS or hardware. It is
 * compiled both into the firmware and into firmware/tests so the exact same
 * code path is unit-tested on the host against synthetic + recorded signals.
 */
#pragma once
#include <cstddef>
#include <cstdint>

namespace stackchan::audioreactive {

// Mic runs at 24 kHz (see hal/board/config.h AUDIO_INPUT_SAMPLE_RATE).
constexpr float kSampleRate = 24000.0f;

// 512-point FFT -> 46.875 Hz/bin, ~21.3 ms window. 256-sample hop = 50 % overlap
// -> ~94 analysis frames per second, plenty to catch beats without starving CPU.
constexpr size_t kFftSize = 512;
constexpr size_t kHopSize = 256;
constexpr size_t kNumBins = kFftSize / 2;

// Perceptual band edges (Hz). Bass carries kick/beat energy; mid ~ vocals/snare;
// treble ~ hats/cymbals. Tuned for a single tiny MEMS mic, not hi-fi analysis.
constexpr float kBassLoHz   = 40.0f;
constexpr float kBassHiHz   = 250.0f;
constexpr float kMidLoHz    = 250.0f;
constexpr float kMidHiHz    = 2000.0f;
constexpr float kTrebleLoHz = 2000.0f;
constexpr float kTrebleHiHz = 8000.0f;

struct BandEnergies {
    float bass   = 0.0f;  // mean magnitude across the bass band
    float mid    = 0.0f;
    float treble = 0.0f;
    float total  = 0.0f;  // mean magnitude across the full usable spectrum
};

/**
 * @brief Multiply a frame by a Hann window.
 *
 * @param in     Input samples, length n.
 * @param out    Output (may alias in), length n.
 * @param n      Frame length.
 */
void applyHannWindow(const float* in, float* out, size_t n);

/**
 * @brief In-place iterative radix-2 FFT (decimation in time).
 *
 * @param real   Real part, length n. Overwritten with result.
 * @param imag   Imag part, length n. Overwritten with result.
 * @param n      Transform length. MUST be a power of two.
 */
void fftRadix2(float* real, float* imag, size_t n);

/**
 * @brief Window a real frame, FFT it, and emit the magnitude spectrum.
 *
 * @param samples  Time-domain samples, length n.
 * @param n        Frame length (power of two).
 * @param mags     Output magnitudes, length n/2. mags[k] = |X[k]| / (n/2).
 */
void computeMagnitudeSpectrum(const float* samples, size_t n, float* mags);

/**
 * @brief Reduce a magnitude spectrum to bass/mid/treble mean energies.
 *
 * @param mags        Magnitude spectrum, length numBins.
 * @param numBins     Number of usable bins (n/2).
 * @param sampleRate  Sample rate in Hz.
 * @param fftSize     FFT size used to produce mags.
 */
BandEnergies computeBandEnergies(const float* mags, size_t numBins, float sampleRate, size_t fftSize);

/**
 * @brief Convenience: raw int16 mono frame -> band energies.
 *
 * @param samples  int16 PCM, length n.
 * @param n        Frame length (power of two).
 */
BandEnergies analyzeFrame(const int16_t* samples, size_t n);

/**
 * @brief Energy-domain acoustic echo suppression.
 *
 * Subtract a scaled copy of the reference (speaker) band energies from the mic
 * band energies, clamped at zero. Phase-insensitive and cheap - enough to stop
 * the robot's own TTS / sound effects from self-triggering beats, without a full
 * adaptive-filter AEC. refGain accounts for the mic picking up the speaker
 * louder/quieter than the electrical reference.
 */
BandEnergies subtractReference(const BandEnergies& mic, const BandEnergies& ref, float refGain);

/**
 * @brief Raw int16 mic + reference frames -> echo-suppressed band energies.
 *
 * @param mic      Mic channel (ch0), length n.
 * @param ref      Speaker reference channel (ch1), length n.
 * @param n        Frame length (power of two).
 * @param refGain  Scale applied to the reference before subtraction.
 */
BandEnergies analyzeFrameStereo(const int16_t* mic, const int16_t* ref, size_t n, float refGain);

}  // namespace stackchan::audioreactive
