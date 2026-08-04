/*
 * SPDX-FileCopyrightText: 2026 Alan Rodriguez
 *
 * SPDX-License-Identifier: MIT
 */
#include "audio_analysis.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace stackchan::audioreactive {

namespace {

constexpr float kPi = 3.14159265358979323846f;

size_t binForFreq(float hz, float sampleRate, size_t fftSize)
{
    const float binHz = sampleRate / static_cast<float>(fftSize);
    long bin          = std::lround(hz / binHz);
    if (bin < 0) {
        bin = 0;
    }
    return static_cast<size_t>(bin);
}

float meanMagnitude(const float* mags, size_t lo, size_t hi, size_t numBins)
{
    lo = std::min(lo, numBins);
    hi = std::min(hi, numBins);
    if (hi <= lo) {
        return 0.0f;
    }
    float sum = 0.0f;
    for (size_t k = lo; k < hi; ++k) {
        sum += mags[k];
    }
    return sum / static_cast<float>(hi - lo);
}

}  // namespace

void applyHannWindow(const float* in, float* out, size_t n)
{
    if (n == 0) {
        return;
    }
    if (n == 1) {
        out[0] = in[0];
        return;
    }
    const float denom = static_cast<float>(n - 1);
    for (size_t i = 0; i < n; ++i) {
        const float w = 0.5f * (1.0f - std::cos(2.0f * kPi * static_cast<float>(i) / denom));
        out[i]        = in[i] * w;
    }
}

void fftRadix2(float* real, float* imag, size_t n)
{
    if (n < 2) {
        return;
    }

    // Bit-reversal permutation.
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) {
            j &= ~bit;
        }
        j |= bit;
        if (i < j) {
            std::swap(real[i], real[j]);
            std::swap(imag[i], imag[j]);
        }
    }

    // Cooley-Tukey butterflies.
    for (size_t len = 2; len <= n; len <<= 1) {
        const float ang = -2.0f * kPi / static_cast<float>(len);
        const float wr  = std::cos(ang);
        const float wi  = std::sin(ang);
        for (size_t i = 0; i < n; i += len) {
            float curr = 1.0f;
            float curi = 0.0f;
            for (size_t k = 0; k < len / 2; ++k) {
                const size_t a = i + k;
                const size_t b = i + k + len / 2;
                const float ur = real[a];
                const float ui = imag[a];
                const float vr = real[b] * curr - imag[b] * curi;
                const float vi = real[b] * curi + imag[b] * curr;
                real[a]        = ur + vr;
                imag[a]        = ui + vi;
                real[b]        = ur - vr;
                imag[b]        = ui - vi;
                const float ncurr = curr * wr - curi * wi;
                curi              = curr * wi + curi * wr;
                curr              = ncurr;
            }
        }
    }
}

void computeMagnitudeSpectrum(const float* samples, size_t n, float* mags)
{
    if (n < 2) {
        if (n == 1) {
            mags[0] = std::fabs(samples[0]);
        }
        return;
    }

    std::vector<float> re(n);
    std::vector<float> im(n, 0.0f);
    applyHannWindow(samples, re.data(), n);

    fftRadix2(re.data(), im.data(), n);

    const float norm     = 2.0f / static_cast<float>(n);
    const size_t numBins = n / 2;
    for (size_t k = 0; k < numBins; ++k) {
        mags[k] = std::sqrt(re[k] * re[k] + im[k] * im[k]) * norm;
    }
}

BandEnergies computeBandEnergies(const float* mags, size_t numBins, float sampleRate, size_t fftSize)
{
    BandEnergies out;
    out.bass = meanMagnitude(mags, binForFreq(kBassLoHz, sampleRate, fftSize),
                             binForFreq(kBassHiHz, sampleRate, fftSize), numBins);
    out.mid  = meanMagnitude(mags, binForFreq(kMidLoHz, sampleRate, fftSize),
                            binForFreq(kMidHiHz, sampleRate, fftSize), numBins);
    out.treble = meanMagnitude(mags, binForFreq(kTrebleLoHz, sampleRate, fftSize),
                               binForFreq(kTrebleHiHz, sampleRate, fftSize), numBins);
    // Skip DC bin for the broadband total.
    out.total = meanMagnitude(mags, 1, numBins, numBins);
    return out;
}

BandEnergies analyzeFrame(const int16_t* samples, size_t n)
{
    std::vector<float> f(n);
    for (size_t i = 0; i < n; ++i) {
        f[i] = static_cast<float>(samples[i]) / 32768.0f;
    }
    std::vector<float> mags(n / 2, 0.0f);
    computeMagnitudeSpectrum(f.data(), n, mags.data());
    return computeBandEnergies(mags.data(), n / 2, kSampleRate, n);
}

BandEnergies subtractReference(const BandEnergies& mic, const BandEnergies& ref, float refGain)
{
    BandEnergies out;
    out.bass   = std::max(0.0f, mic.bass - refGain * ref.bass);
    out.mid    = std::max(0.0f, mic.mid - refGain * ref.mid);
    out.treble = std::max(0.0f, mic.treble - refGain * ref.treble);
    out.total  = std::max(0.0f, mic.total - refGain * ref.total);
    return out;
}

BandEnergies analyzeFrameStereo(const int16_t* mic, const int16_t* ref, size_t n, float refGain)
{
    const BandEnergies micBands = analyzeFrame(mic, n);
    const BandEnergies refBands = analyzeFrame(ref, n);
    return subtractReference(micBands, refBands, refGain);
}

}  // namespace stackchan::audioreactive
