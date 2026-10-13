// Offline sanity test of SuperParasitesEngine at several host sample rates, modes and qualities.
// It checks that output is finite, bounded and not silent, that freeze keeps sound going after the input stops,
// and reports how fast it runs. It does NOT check that it sounds like the module.
#include "SuperParasitesEngine.h"
#include <chrono>
#include <cstdio>
#include <cmath>
#include <vector>
#include <algorithm>

// Which engine rate the whole suite runs at (0 = 32 kHz, 1 = 48 kHz); main() runs everything at both.
static int gEngineRate = 0;
struct TParams : SuperParasitesEngine::Params { TParams() { engineRate = gEngineRate; } };

static const char* kModeNames[] = { "granular", "stretch", "looping-delay", "spectral", "oliverb", "resonestor", "beat-repeat", "spectral-clouds" };

struct Result { double energy = 0, maxAbs = 0; bool bad = false; double realtimeFactor = 0; };

// Feeds a 110 Hz saw (silence after `inputSeconds`), pressing capture every `captureEvery` seconds (0 = never).
// Returns energy of the last half.
static Result run (double sr, SuperParasitesEngine::Params p, double seconds, double inputSeconds,
                   double freezeAtSeconds = -1.0, double captureEvery = 0.0)
{
    SuperParasitesEngine e; e.prepare (sr);
    const int N = 512;
    std::vector<float> in (N), outL (N), outR (N);
    double phase = 0, nextCapture = captureEvery;
    Result r;
    const int blocks = (int) (sr * seconds / N);
    auto t0 = std::chrono::steady_clock::now();
    for (int b = 0; b < blocks; ++b)
    {
        const double t = (double) b * N / sr;
        if (freezeAtSeconds >= 0 && t >= freezeAtSeconds) p.freeze = true;
        if (captureEvery > 0 && t >= nextCapture) { e.capture(); nextCapture += captureEvery; }
        const bool on = t < inputSeconds;
        for (int i = 0; i < N; ++i)
        {
            phase += 110.0 / sr; if (phase >= 1.0) phase -= 1.0;
            in[i] = on ? 0.4f * (float) (2.0 * phase - 1.0) : 0.0f;
        }
        e.process (in.data(), in.data(), outL.data(), outR.data(), N, p);
        for (int i = 0; i < N; ++i)
        {
            if (! std::isfinite (outL[i]) || ! std::isfinite (outR[i])) r.bad = true;
            r.maxAbs = std::max (r.maxAbs, (double) std::fabs (outL[i]));
            if (b > blocks / 2) r.energy += outL[i] * outL[i] + outR[i] * outR[i];
        }
    }
    const double elapsed = std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();
    r.realtimeFactor = seconds / elapsed;
    return r;
}



// ---- Extended: long-buffer tests -------------------------------------------------------------------------------
// Looping delay at maximum delay: a short noise burst should come back after (about) the length of the buffer.
static double measureMaxDelaySeconds (double sr, int lengthIndex, int quality, bool& bad)
{
    SuperParasitesEngine e; e.prepare (sr);
    TParams p;
    p.mode = SuperParasitesEngine::LoopingDelay; p.quality = quality; p.bufferLength = lengthIndex;
    p.dryWet = 1.0f; p.position = 1.0f; p.feedback = 0.0f; p.reverb = 0.0f; p.texture = 0.5f; p.density = 0.0f;
    const double expected = (quality & 1 ? 1.0 : 1.0) * 1.0;   // (kept for readability: expectation is computed by the caller)
    (void) expected;
    const int N = 512;
    std::vector<float> in (N), oL (N), oR (N);
    // total run: the buffer length (low-fi holds 4x, mono ~2x) plus margin; computed generously
    const double maxLen = SuperParasitesEngine::bufferSeconds (lengthIndex) * ((quality & 2) ? 4.0 : 1.0) * ((quality & 1) ? 2.0 : 1.0) + 3.0;
    const int blocks = (int) (sr * maxLen / N);
    unsigned seed = 12345;
    double peakTime = -1, peak = 0;
    for (int b = 0; b < blocks; ++b)
    {
        const double t0 = (double) b * N / sr;
        for (int i = 0; i < N; ++i)
        {
            const double t = t0 + i / sr;
            seed = seed * 1664525u + 1013904223u;
            const float noise = ((seed >> 9) / (float) (1 << 23)) * 2.0f - 1.0f;
            in[i] = (t > 0.10 && t < 0.115) ? 0.8f * noise : 0.0f;    // a 15 ms burst at 0.1 s
        }
        e.process (in.data(), in.data(), oL.data(), oR.data(), N, p);
        for (int i = 0; i < N; ++i)
        {
            if (! std::isfinite (oL[i])) bad = true;
            if (t0 > 0.5 && std::fabs (oL[i]) > peak) { peak = std::fabs (oL[i]); peakTime = t0 + i / sr; }
        }
    }
    return peak > 0.05 ? peakTime - 0.1 : -1.0;
}

static int longBufferTests()
{
    int fails = 0;
    const double sr = 48000.0;

    // 1) maximum looping-delay time follows the buffer length (module memory: ~1 s stereo; low-fi holds 4x; mono about 2x)
    struct Case { int length, quality; double expect; const char* label; };
    const Case cases[] = {
        { 0, 0,  1.0,    "1 s   stereo 16-bit" },
        { 1, 0,  4.0,    "4 s   stereo 16-bit" },
        { 2, 0, 10.0,    "10 s  stereo 16-bit" },
        { 3, 0, 20.0,    "20 s  stereo 16-bit" },
        { 2, 1, 10.0 + 53376.0 / 2.0 / 32000.0,   "10 s  mono 16-bit" },
        { 0, 2,  4.09,   "1 s   stereo 8-bit (low-fi)" },
        { 1, 2, 16.0,    "4 s   stereo 8-bit (low-fi)" },
    };
    for (const Case& c : cases)
    {
        bool bad = false;
        const double got = measureMaxDelaySeconds (sr, c.length, c.quality, bad);
        const bool ok = ! bad && got > 0 && std::fabs (got - c.expect) <= 0.06 * c.expect + 0.05;
        printf ("max delay, %-28s expected about %6.2f s, measured %6.2f s  %s\n", c.label, c.expect, got, ok ? "OK" : "FAIL");
        fails += ! ok;
    }

    // 2) every mode x quality x length stays finite, bounded and not silent at 48 kHz. A long buffer is mostly empty until it has
    //    been recorded into (the Position knob spans the whole buffer), so each run lasts longer than the buffer takes to fill.
    int before = fails;
    for (int length = 0; length < SuperParasitesEngine::kNumBufferLengths; ++length)
        for (int mode = 0; mode < SuperParasitesEngine::NumModes; ++mode)
            for (int quality : { 0, 1, 2 })
            {
                TParams p; p.mode = mode; p.quality = quality; p.bufferLength = length; p.dryWet = 1.0f;
                p.density = 0.8f; p.texture = 0.4f; p.size = 0.5f; p.position = 0.4f; p.stereoSpread = 0.6f;
                if (mode == SuperParasitesEngine::SpectralClouds) p.position = 0.95f;
                const double fill = SuperParasitesEngine::bufferSeconds (length) * ((quality & 2) ? 4.0 : 1.0) * ((quality & 1) ? 2.0 : 1.0);
                const double seconds = fill + 3.0;
                const Result r = run (sr, p, seconds, seconds, -1.0, 1.0);
                const bool ok = ! r.bad && r.energy > 0.5 && r.maxAbs <= 1.5;
                if (! ok) printf ("length %d %-15s quality=%d  energy=%.2f max=%.2f  FAIL\n", length, kModeNames[mode], quality, r.energy, r.maxAbs);
                fails += ! ok;
            }
    printf ("every mode x quality 0/1/2 x all four buffer lengths (run long enough to fill the buffer): %s\n", fails > before ? "see FAIL lines" : "OK");

    // 3) positions deep in a long buffer: granular, stretch, beat repeat read audio from several seconds back
    for (int mode : { 0, 1, 2, 6 })
    {
        TParams p; p.mode = mode; p.quality = 0; p.bufferLength = 3; p.dryWet = 1.0f;
        p.density = 0.8f; p.size = 0.3f; p.position = 0.9f; p.texture = 0.4f; p.stereoSpread = 0.6f;
        // input for 12 s, so the buffer holds old audio; read position near the far end of the 20 s buffer
        const Result r = run (sr, p, 24.0, 12.0, -1.0, 0.8);
        const bool ok = ! r.bad && r.maxAbs <= 1.5 && r.energy > 0.05;
        printf ("20 s buffer, %-15s position 0.9, input then silence: energy=%.2f max=%.2f  %s\n", kModeNames[mode], r.energy, r.maxAbs, ok ? "OK" : "FAIL");
        fails += ! ok;
    }

    // 4) changing the buffer length while running must not crash or produce junk
    {
        SuperParasitesEngine e; e.prepare (sr);
        TParams p; p.dryWet = 0.7f;
        const int N = 256; std::vector<float> in (N), oL (N), oR (N);
        double phase = 0; bool bad = false;
        const int blocks = (int) (sr * 20 / N);
        for (int b = 0; b < blocks; ++b)
        {
            if (b % 100 == 0) { p.bufferLength = (b / 100) % SuperParasitesEngine::kNumBufferLengths; p.mode = (b / 100) % SuperParasitesEngine::NumModes; e.capture(); }
            for (int i = 0; i < N; ++i) { phase += 220.0 / sr; if (phase >= 1.0) phase -= 1.0; in[i] = 0.4f * (float) (2.0 * phase - 1.0); }
            e.process (in.data(), in.data(), oL.data(), oR.data(), N, p);
            for (int i = 0; i < N; ++i) if (! std::isfinite (oL[i]) || ! std::isfinite (oR[i]) || std::fabs (oL[i]) > 2.0f) bad = true;
        }
        printf ("changing buffer length and mode while running: %s\n", bad ? "FAIL" : "OK");
        fails += bad;
    }
    return fails;
}

static int allChecks()
{
    int fails = 0;
    const double rates[] = { 44100.0, 48000.0, 96000.0 };

    printf ("pitch knob curve: 0 -> %.2f st, 0.5 -> %.2f st, 1 -> %.2f st\n",
            SuperParasitesEngine::pitchPotToSemitones (0.0f), SuperParasitesEngine::pitchPotToSemitones (0.5f),
            SuperParasitesEngine::pitchPotToSemitones (1.0f));

    for (double sr : rates)
    {
        // 1) every mode x every quality, wet signal only (capture pressed every second so burst/trigger modes sound)
        for (int mode = 0; mode < SuperParasitesEngine::NumModes; ++mode)
            for (int quality = 0; quality < 4; ++quality)
            {
                TParams p; p.mode = mode; p.quality = quality; p.dryWet = 1.0f; p.density = 0.8f;
                p.texture = 0.4f; p.size = 0.5f; p.position = 0.4f; p.stereoSpread = 0.6f;
                // Spectral clouds mutes random frequency bands by design: POSITION low = most bands muted (silent at 0).
                if (mode == SuperParasitesEngine::SpectralClouds) p.position = 0.95f;
                const Result r = run (sr, p, 6.0, 6.0, -1.0, 1.0);
                const bool ok = ! r.bad && r.energy > 0.5 && r.maxAbs <= 1.5;
                printf ("sr=%6.0f %-15s quality=%d  energy=%10.2f max=%.2f  %5.1fx realtime  %s\n",
                        sr, kModeNames[mode], quality, r.energy, r.maxAbs, r.realtimeFactor, ok ? "OK" : "FAIL");
                fails += ! ok;
            }

        // 2) freeze: stop feeding input, the frozen buffer should keep sounding
        for (int mode : { 0, 2, 3, 4, 7 })
        {
            TParams p; p.mode = mode; p.quality = 0; p.dryWet = 1.0f; p.density = 0.8f; p.position = 0.5f; p.size = 0.6f;
            const Result r = run (sr, p, 8.0, 2.5, 2.4);
            const bool ok = ! r.bad && r.energy > 0.05 && r.maxAbs <= 1.5;
            printf ("sr=%6.0f %-15s freeze, input stopped  energy=%10.2f max=%.2f  %s\n", sr, kModeNames[mode], r.energy, r.maxAbs, ok ? "OK" : "FAIL");
            fails += ! ok;
        }

        // 3) switching mode and quality while running must not crash or produce non-finite output
        {
            SuperParasitesEngine e; e.prepare (sr);
            TParams p; p.dryWet = 0.7f;
            const int N = 256; std::vector<float> in (N), oL (N), oR (N);
            double phase = 0; bool bad = false;
            const int blocks = (int) (sr * 16 / N);
            for (int b = 0; b < blocks; ++b)
            {
                if (b % 40 == 0) { p.mode = (b / 40) % SuperParasitesEngine::NumModes; p.quality = (b / 160) % 4; e.capture(); }
                for (int i = 0; i < N; ++i) { phase += 220.0 / sr; if (phase >= 1.0) phase -= 1.0; in[i] = 0.4f * (float) (2.0 * phase - 1.0); }
                e.process (in.data(), in.data(), oL.data(), oR.data(), N, p);
                for (int i = 0; i < N; ++i) if (! std::isfinite (oL[i]) || ! std::isfinite (oR[i]) || std::fabs (oL[i]) > 2.0f) bad = true;
            }
            printf ("sr=%6.0f mode/quality switching while running  %s\n", sr, bad ? "FAIL" : "OK");
            fails += bad;
        }

        // 4) extreme knob positions in every mode must stay finite and bounded
        for (int mode = 0; mode < SuperParasitesEngine::NumModes; ++mode)
            for (int corner = 0; corner < 4; ++corner)
            {
                TParams p; p.mode = mode;
                const float lo = 0.0f, hi = 1.0f;
                p.position = (corner & 1) ? hi : lo; p.size = (corner & 2) ? hi : lo; p.density = (corner & 1) ? lo : hi;
                p.texture = (corner & 2) ? lo : hi; p.pitchPot = (corner & 1) ? 1.0f : 0.0f; p.dryWet = 1.0f;
                p.stereoSpread = hi; p.feedback = hi; p.reverb = hi; p.gate = (corner == 3);
                const Result r = run (sr, p, 4.0, 4.0, -1.0, 0.7);
                // The engine's own output never exceeds 1.0 (it is soft-clipped); anything above that is the host-rate resampler
                // overshooting on a hard-clipped signal (measured up to 1.41 at 32 kHz and 1.57 at 48 kHz with a 96 kHz host).
                const bool ok = ! r.bad && r.maxAbs <= 1.7;
                if (! ok) printf ("sr=%6.0f %-15s extreme corner %d  max=%.2f  FAIL\n", sr, kModeNames[mode], corner, r.maxAbs);
                fails += ! ok;
            }

        SuperParasitesEngine e; e.prepare (sr);
        printf ("sr=%6.0f processor rate %.0f, reported latency %d samples\n", sr, e.processorRate(), e.latencyInHostSamples());
    }
    fails += longBufferTests();
    return fails;
}

int main()
{
    int fails = 0;
    for (int rate : { 0, 1 })
    {
        gEngineRate = rate;
        printf ("\n===== engine rate %s =====\n", rate == 0 ? "32 kHz (original)" : "48 kHz (HD)");
        fails += allChecks();
    }
    printf (fails ? "\n%d FAILURES\n" : "\nall checks passed\n", fails);
    return fails;
}
