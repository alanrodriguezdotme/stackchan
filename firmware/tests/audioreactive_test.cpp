/*
 * SPDX-FileCopyrightText: 2026 Alan Rodriguez
 *
 * SPDX-License-Identifier: MIT
 *
 * Host-side unit tests for the reactive-dance DSP core: FFT + band energies,
 * the beat detector, and the choreography mapping. No ESP-IDF involved.
 */
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <vector>

#include <stackchan/audioreactive/audio_analysis.h>
#include <stackchan/audioreactive/beat_detector.h>
#include <stackchan/audioreactive/dance_director.h>

namespace {

using namespace stackchan::audioreactive;

int g_failures = 0;

void check(bool cond, const char* label)
{
    if (!cond) {
        std::cerr << "FAIL: " << label << '\n';
        ++g_failures;
    }
}

void checkNear(float actual, float expected, float tol, const char* label)
{
    if (std::fabs(actual - expected) > tol) {
        std::cerr << "FAIL: " << label << " expected ~" << expected << " got " << actual << '\n';
        ++g_failures;
    }
}

constexpr float kPi = 3.14159265358979323846f;

std::vector<float> makeSine(float freqHz, size_t n, float amp = 1.0f)
{
    std::vector<float> s(n);
    for (size_t i = 0; i < n; ++i) {
        s[i] = amp * std::sin(2.0f * kPi * freqHz * static_cast<float>(i) / kSampleRate);
    }
    return s;
}

/* ------------------------------- FFT / bands ------------------------------ */

void testFftPeakBin()
{
    // A pure tone should put nearly all energy in its bin.
    const size_t n     = kFftSize;
    const float binHz  = kSampleRate / static_cast<float>(n);
    const size_t k     = 20;               // arbitrary bin well inside the band
    const float freq   = k * binHz;
    auto samples       = makeSine(freq, n, 0.8f);

    std::vector<float> mags(n / 2, 0.0f);
    computeMagnitudeSpectrum(samples.data(), n, mags.data());

    size_t peak = 0;
    for (size_t i = 1; i < mags.size(); ++i) {
        if (mags[i] > mags[peak]) {
            peak = i;
        }
    }
    check(peak == k, "fft peak lands in the tone's bin");
    // Windowed amplitude recovery is approximate; just assert it's substantial.
    check(mags[peak] > 0.2f, "fft peak magnitude is substantial");
}

void testBandSeparation()
{
    const size_t n = kFftSize;

    auto bassSig   = makeSine(90.0f, n, 0.8f);    // in bass band
    auto trebleSig = makeSine(4000.0f, n, 0.8f);  // in treble band

    std::vector<float> mags(n / 2, 0.0f);

    computeMagnitudeSpectrum(bassSig.data(), n, mags.data());
    auto bBands = computeBandEnergies(mags.data(), n / 2, kSampleRate, n);
    check(bBands.bass > bBands.treble, "bass tone -> bass band dominates");

    computeMagnitudeSpectrum(trebleSig.data(), n, mags.data());
    auto tBands = computeBandEnergies(mags.data(), n / 2, kSampleRate, n);
    check(tBands.treble > tBands.bass, "treble tone -> treble band dominates");
}

void testSilenceIsQuiet()
{
    const size_t n = kFftSize;
    std::vector<float> zeros(n, 0.0f);
    std::vector<float> mags(n / 2, 0.0f);
    computeMagnitudeSpectrum(zeros.data(), n, mags.data());
    auto bands = computeBandEnergies(mags.data(), n / 2, kSampleRate, n);
    checkNear(bands.total, 0.0f, 1e-6f, "silence has ~zero energy");
}

/* ----------------------------- AEC (reference) ---------------------------- */

std::vector<int16_t> makeSine16(float freqHz, size_t n, float amp)
{
    std::vector<int16_t> s(n);
    for (size_t i = 0; i < n; ++i) {
        float v = amp * std::sin(2.0f * kPi * freqHz * static_cast<float>(i) / kSampleRate);
        s[i]    = static_cast<int16_t>(v * 30000.0f);
    }
    return s;
}

void testAecCancelsSelfAudio()
{
    // Mic hears exactly the speaker reference (pure self-audio) -> suppressed.
    const size_t n = kFftSize;
    auto sig       = makeSine16(90.0f, n, 0.8f);  // bass-band self-audio
    auto bands     = analyzeFrameStereo(sig.data(), sig.data(), n, 1.0f);
    check(bands.bass < 0.01f, "identical mic+ref cancels to near zero (bass)");
    check(bands.total < 0.01f, "identical mic+ref cancels to near zero (total)");
}

void testAecKeepsExternalAudio()
{
    // Speaker (ref) plays a bass tone; mic hears that PLUS an external treble
    // tone the speaker isn't producing. AEC should suppress the shared bass but
    // preserve the external treble. Use half-scale tones so the mix doesn't clip.
    const size_t n = kFftSize;
    auto refSig    = makeSine16(90.0f, n, 0.4f);
    auto micBass   = makeSine16(90.0f, n, 0.4f);
    auto micTreble = makeSine16(4000.0f, n, 0.4f);
    std::vector<int16_t> mic(n);
    for (size_t i = 0; i < n; ++i) {
        mic[i] = static_cast<int16_t>(micBass[i] + micTreble[i]);
    }

    const auto micOnly = analyzeFrame(mic.data(), n);
    const auto aec     = analyzeFrameStereo(mic.data(), refSig.data(), n, 1.0f);

    check(aec.bass < micOnly.bass * 0.5f, "shared bass self-audio is suppressed");
    check(aec.treble > micOnly.treble * 0.9f, "external treble is preserved");
    check(aec.treble > aec.bass, "after AEC the external band dominates");
}

void testSubtractReferenceClamps()
{
    BandEnergies mic;
    mic.bass = 0.1f;
    BandEnergies ref;
    ref.bass    = 0.5f;
    auto out    = subtractReference(mic, ref, 1.0f);
    check(out.bass >= 0.0f, "reference subtraction never goes negative");
}

/* ------------------------------ Beat detector ----------------------------- */

// Simulate a stream of energy hops at ~94 Hz (10.6 ms). A "beat" is a spike.
void testBeatDetectsSteadyTempo()
{
    BeatDetector det;  // defaults
    const uint32_t hopMs   = 11;
    const uint32_t beatGap = 500;  // 120 BPM
    const uint32_t total   = 8000;

    int beats           = 0;
    uint32_t nextBeatAt = 1000;  // let the baseline warm up first
    float lastBpm       = 0.0f;

    for (uint32_t t = 0; t <= total; t += hopMs) {
        float energy = 0.02f;  // quiet baseline hum
        if (t >= nextBeatAt) {
            energy = 0.5f;  // spike
            nextBeatAt += beatGap;
        }
        auto r = det.process(energy, t);
        if (r.beat) {
            ++beats;
        }
        lastBpm = r.bpm;
    }

    // 14 spikes scheduled from 1000..7500 ms.
    check(beats >= 12 && beats <= 14, "steady tempo -> most beats detected");
    checkNear(lastBpm, 120.0f, 12.0f, "bpm estimate near 120");
}

void testBeatSilenceNoFalsePositives()
{
    BeatDetector det;
    int beats = 0;
    for (uint32_t t = 0; t <= 5000; t += 11) {
        auto r = det.process(0.0f, t);  // dead silence
        if (r.beat) {
            ++beats;
        }
    }
    check(beats == 0, "silence -> no beats");
}

void testBeatRefractory()
{
    BeatDetectorConfig cfg;
    cfg.refractoryMs = 260;
    BeatDetector det(cfg);

    // Warm up the baseline low.
    for (uint32_t t = 0; t < 600; t += 11) {
        det.process(0.02f, t);
    }
    // Fire spikes every 50 ms - refractory should collapse them.
    int beats = 0;
    for (uint32_t t = 600; t < 1600; t += 50) {
        auto r = det.process(0.6f, t);
        if (r.beat) {
            ++beats;
        }
    }
    // 1000 ms window / 260 ms refractory -> at most ~4 beats.
    check(beats <= 4, "refractory limits rapid re-triggering");
    check(beats >= 3, "refractory still lets beats through at the allowed rate");
}

void testBeatLevelNormalized()
{
    BeatDetector det;
    float maxLevel = 0.0f;
    for (uint32_t t = 0; t <= 2000; t += 11) {
        float e = (t % 500 < 20) ? 0.9f : 0.05f;
        auto r  = det.process(e, t);
        maxLevel = std::max(maxLevel, r.level);
        check(r.level >= 0.0f && r.level <= 1.0f, "level stays within 0..1");
    }
    check(maxLevel > 0.8f, "loud passage drives level near 1");
}

/* ----------------------------- Dance director ----------------------------- */

void testHsvPrimaries()
{
    uint8_t r, g, b;
    hsvToRgb(0.0f, 1.0f, 1.0f, r, g, b);
    check(r == 255 && g == 0 && b == 0, "hsv red");
    hsvToRgb(120.0f, 1.0f, 1.0f, r, g, b);
    check(r == 0 && g == 255 && b == 0, "hsv green");
    hsvToRgb(240.0f, 1.0f, 1.0f, r, g, b);
    check(r == 0 && g == 0 && b == 255, "hsv blue");
    hsvToRgb(0.0f, 0.0f, 0.0f, r, g, b);
    check(r == 0 && g == 0 && b == 0, "hsv black");
}

void testDanceBeatFlipsSway()
{
    DanceDirector dir;
    BandEnergies bands;
    bands.bass = 0.1f;

    auto c1 = dir.onFrame(true, bands, 1.0f, 0);
    auto c2 = dir.onFrame(true, bands, 1.0f, 400);
    // Two beats -> sway direction should have flipped between them.
    check((c1.yaw > 0.0f) != (c2.yaw > 0.0f), "consecutive beats flip sway side");
    check(std::fabs(c1.yaw) > 0.1f, "beat produces real head movement");
    check(c1.speed >= 900, "beat uses fast servo speed");
}

void testDanceRelaxesBetweenBeats()
{
    DanceDirector dir;
    BandEnergies bands;
    bands.bass = 0.1f;

    auto hit = dir.onFrame(true, bands, 1.0f, 0);
    float peak = std::fabs(hit.yaw);

    // No further beats for a while; the hit envelope should decay the amplitude.
    DanceCommand later;
    for (uint32_t t = 50; t <= 1200; t += 50) {
        later = dir.onFrame(false, bands, 0.2f, t);
    }
    check(std::fabs(later.yaw) < peak, "yaw amplitude decays without new beats");
    check(later.speed < 900, "idle uses slower servo speed");
}

void testDanceMouthTracksHighs()
{
    DanceDirector dir;
    BandEnergies quiet;   // all zero
    BandEnergies bright;
    bright.mid    = 0.15f;
    bright.treble = 0.15f;

    DanceCommand loud;
    for (uint32_t t = 0; t <= 300; t += 30) {
        loud = dir.onFrame(false, bright, 0.8f, t);
    }
    DanceCommand silent;
    for (uint32_t t = 400; t <= 700; t += 30) {
        silent = dir.onFrame(false, quiet, 0.0f, t);
    }
    check(loud.mouthOpen > silent.mouthOpen, "mouth opens more with mid/treble energy");
    check(loud.emotion == 1, "loud passage -> happy expression");
}

void testDanceColorAdvancesOnBeat()
{
    DanceDirector dir;
    BandEnergies bands;
    bands.bass = 0.1f;

    auto a = dir.onFrame(true, bands, 1.0f, 0);
    auto b = dir.onFrame(true, bands, 1.0f, 400);
    const bool changed = (a.r != b.r) || (a.g != b.g) || (a.b != b.b);
    check(changed, "color wheel advances across beats");
}

}  // namespace

int main()
{
    testFftPeakBin();
    testBandSeparation();
    testSilenceIsQuiet();
    testAecCancelsSelfAudio();
    testAecKeepsExternalAudio();
    testSubtractReferenceClamps();

    testBeatDetectsSteadyTempo();
    testBeatSilenceNoFalsePositives();
    testBeatRefractory();
    testBeatLevelNormalized();

    testHsvPrimaries();
    testDanceBeatFlipsSway();
    testDanceRelaxesBetweenBeats();
    testDanceMouthTracksHighs();
    testDanceColorAdvancesOnBeat();

    if (g_failures != 0) {
        std::cerr << g_failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "all audioreactive tests passed\n";
    return 0;
}
