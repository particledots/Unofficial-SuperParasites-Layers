// Tests of the Layers feature of Unofficial SuperParasites Layers (headless, offline).
//
// What this checks: that the six layers record what is fed in, keep it, play it back, survive layer switching and trips
// through the other modes, clear and re-record independently, are emptied when the format changes, and that nothing
// overwrites a playing layer. Tones are used so that "which layer is this?" can be answered by measuring the pitch.
// What it does NOT check: how anything sounds, clicks you can hear, or any behaviour inside Logic.
#include "SuperParasitesEngine.h"
#include "stmlib/utils/random.h"
#include "supercell/dsp/mu_law.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

using Engine = SuperParasitesEngine;

static int gChecks = 0, gFailed = 0;
static void check (bool ok, const char* what, double a = 0.0, double b = 0.0)
{
    ++gChecks;
    if (! ok) { ++gFailed; std::printf ("  FAIL: %s  (%g, %g)\n", what, a, b); }
}

// Tone of each layer (Hz). Far enough apart that the strongest one is unmistakable.
static const double kTone[Engine::kNumLayers + 1] = { 3000.0, 440.0, 660.0, 880.0, 1100.0, 1320.0, 1540.0 };

// Power of one frequency in a block (Goertzel), as an amplitude estimate.
static double amplitudeAt (const std::vector<float>& x, size_t from, size_t count, double hz, double sr)
{
    const double w = 2.0 * 3.14159265358979 * hz / sr, c = 2.0 * std::cos (w);
    double s1 = 0, s2 = 0;
    for (size_t i = from; i < from + count; ++i) { const double s = x[i] + c * s1 - s2; s2 = s1; s1 = s; }
    const double power = s1 * s1 + s2 * s2 - c * s1 * s2;
    return 2.0 * std::sqrt (std::max (0.0, power)) / (double) count;
}

// Energy in a narrow band around a frequency (a loop join spreads a tone's energy a little, so one exact frequency is not enough).
static double bandEnergy (const std::vector<float>& x, size_t from, size_t count, double hz, double sr)
{
    double sum = 0;
    for (int k = -6; k <= 6; ++k)
    {
        const double a = amplitudeAt (x, from, count, hz * (1.0 + 0.005 * k), sr);
        sum += a * a;
    }
    return sum;
}

// Which of tones 0..6 clearly dominates the last `count` samples (at least 2x the amplitude of any other), or -1 if none does.
// `amp` gets the band's amplitude estimate.
static int strongestTone (const std::vector<float>& x, size_t count, double sr, double* amp = nullptr)
{
    count = std::min (count, x.size());
    int best = -1; double bestE = 0, secondE = 0;
    for (int t = 0; t <= Engine::kNumLayers; ++t)
    {
        const double e = bandEnergy (x, x.size() - count, count, kTone[t], sr);
        if (e > bestE) { secondE = bestE; bestE = e; best = t; }
        else if (e > secondE) secondE = e;
    }
    if (amp) *amp = std::sqrt (bestE);
    return bestE >= 4.0 * secondE && bestE > 1e-9 ? best : -1;
}

static double rms (const std::vector<float>& x, size_t count)
{
    count = std::min (count, x.size());
    double s = 0; for (size_t i = x.size() - count; i < x.size(); ++i) s += (double) x[i] * x[i];
    return std::sqrt (s / (double) std::max<size_t> (1, count));
}

static uint64_t hashOf (const std::vector<uint8_t>& v)
{
    uint64_t h = 1469598103934665603ull;
    for (uint8_t b : v) { h ^= b; h *= 1099511628211ull; }
    return h;
}

struct Rig
{
    Engine e;
    Engine::Params p;
    double hostRate;
    double phase = 0.0;
    bool finite = true;
    double maxStep = 0.0, lastSample = 0.0;
    double maxCallMs = 0.0;

    Rig (double host, int engineRate, int quality, int length, int mode) : hostRate (host)
    {
        e.prepare (host);
        p.engineRate = engineRate; p.quality = quality; p.bufferLength = length; p.mode = mode;
        p.dryWet = 1.0f;          // all wet, so what is heard is the engine's own playback
        p.size = 0.5f; p.position = 0.3f; p.density = 0.5f; p.texture = 0.5f;
        p.reverb = 0.0f; p.feedback = 0.0f;
        run (0.25, 0.0);          // let the first rebuild happen
    }

    // Feeds `seconds` of a tone (0 = silence) and returns the left output.
    std::vector<float> run (double seconds, double toneHz, float amp = 0.5f, double captureEvery = 0.0)
    {
        const int N = 512;
        const long total = (long) (seconds * hostRate);
        std::vector<float> in (N), outL (N), outR (N), result;
        result.reserve ((size_t) total);
        long done = 0; double nextCap = captureEvery;
        double tsec = 0;
        while (done < total)
        {
            const int n = (int) std::min<long> (N, total - done);
            for (int i = 0; i < n; ++i)
            {
                phase += toneHz / hostRate; if (phase >= 1.0) phase -= 1.0;
                in[i] = toneHz > 0 ? amp * (float) std::sin (2.0 * 3.14159265358979 * phase) : 0.0f;
            }
            if (captureEvery > 0 && tsec >= nextCap) { e.capture(); nextCap += captureEvery; }
            const auto t0 = std::chrono::steady_clock::now();
            e.process (in.data(), in.data(), outL.data(), outR.data(), n, p);
            const double ms = std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now() - t0).count();
            maxCallMs = std::max (maxCallMs, ms);
            for (int i = 0; i < n; ++i)
            {
                if (! std::isfinite (outL[i]) || ! std::isfinite (outR[i]) || std::fabs (outL[i]) > 8.0f) finite = false;
                maxStep = std::max (maxStep, std::fabs ((double) outL[i] - lastSample));
                lastSample = outL[i];
                result.push_back (outL[i]);
            }
            done += n; tsec += (double) n / hostRate;
        }
        return result;
    }

    void select (int layer) { p.layer = layer; run (0.15, 0.0); }

    // Records into the selected layer with `toneHz` until it stops by itself (or maxSeconds). Returns seconds fed.
    double recordFull (double toneHz, double maxSeconds = 12.0)
    {
        e.recordLayer();
        double fed = 0;
        while (fed < maxSeconds)
        {
            run (0.1, toneHz); fed += 0.1;
            if (e.layerStatus (p.layer).state != Engine::LayerRecording) break;
        }
        run (0.05, 0.0);
        return fed;
    }

    // Plays the selected layer with silent input; returns the dominant tone; lastRms = loudness of the last 0.3 s.
    double lastRms = 0.0;
    int listen (double* amp = nullptr, double seconds = 0.8, double captureEvery = 0.0)
    {
        auto out = run (seconds, 0.0, 0.5f, captureEvery);
        lastRms = rms (out, (size_t) (0.3 * hostRate));
        return strongestTone (out, (size_t) (0.3 * hostRate), hostRate, amp);
    }
};

static double storedRate (int engineRate, int quality)
{
    if (quality & 2) return 16000.0;
    return engineRate ? 48000.0 : 32000.0;
}

// ---- A: record all six, play each, in random order ------------------------------------------------------------------
static void testRecordAndPlayAll (double host, int engineRate)
{
    std::printf ("[A] record six layers, play them back (host %.0f Hz, engine %s)\n", host, engineRate ? "48k" : "32k");
    Rig r (host, engineRate, 0, 0, Engine::LoopingDelay);
    uint64_t hash[Engine::kNumLayers + 1] = {};
    for (int k = 1; k <= Engine::kNumLayers; ++k)
    {
        r.select (k);
        check (r.e.loadedLayer() == k, "layer is loaded after selecting it", r.e.loadedLayer(), k);
        const double fed = r.recordFull (kTone[k]);
        const auto st = r.e.layerStatus (k);
        check (st.state == Engine::LayerFilled, "recording stops by itself and the layer is filled", st.state);
        const size_t bytes = r.e.layerBytes (k, 0).size();
        const double expectSec = ((double) bytes / 2.0 - 8.0) / storedRate (engineRate, 0);
        check (std::fabs (st.seconds - expectSec) < 0.001, "reported length matches the memory", st.seconds, expectSec);
        check (fed < 2.0, "a 1 s layer fills in about a second of input", fed);
        // content: the stored samples hold the tone that was fed in
        const auto& m = r.e.layerBytes (k, 0);
        std::vector<float> x (m.size() / 2);
        for (size_t i = 0; i < x.size(); ++i) x[i] = (float) (int16_t) (m[2 * i] | (m[2 * i + 1] << 8)) / 32768.0f;
        double a = 0; const int t = strongestTone (x, x.size() - 64, storedRate (engineRate, 0), &a);
        check (t == k, "stored audio has the layer's tone", t, k);
        check (a > 0.35 && a < 0.65, "stored tone level is about what was fed (0.5)", a);
        hash[k] = hashOf (m);
    }
    for (int i = 1; i <= Engine::kNumLayers; ++i)
        for (int j = i + 1; j <= Engine::kNumLayers; ++j)
            check (hash[i] != hash[j], "layers hold different audio", i, j);

    // Play them back, many switches in an odd order, silent input.
    const int order[] = { 3, 1, 6, 2, 5, 4, 4, 1, 6, 3, 2, 5 };
    for (int k : order)
    {
        r.select (k);
        double amp = 0; const int t = r.listen (&amp);
        check (t == k, "playing a layer gives that layer's tone", t, k);
        check (r.lastRms > 0.05, "playing a layer is not silent", r.lastRms);
    }
    for (int k = 1; k <= Engine::kNumLayers; ++k)
        check (hashOf (r.e.layerBytes (k, 0)) == hash[k], "stored audio unchanged after all the switching", k);
    check (r.finite, "output finite and bounded");
}

// ---- B: trips through the modes that use the memory as workspace ------------------------------------------------------
static void testModeRoundTrip (int engineRate)
{
    std::printf ("[B] mode round trips (engine %s)\n", engineRate ? "48k" : "32k");
    Rig r (48000.0, engineRate, 0, 0, Engine::LoopingDelay);
    r.select (3); r.recordFull (kTone[3]);
    r.select (5); r.recordFull (kTone[5]);
    const uint64_t h3 = hashOf (r.e.layerBytes (3, 0)), h5 = hashOf (r.e.layerBytes (5, 0));
    r.select (3);
    double amp = 0; check (r.listen (&amp) == 3, "layer 3 plays before the trip", amp);

    const int trip[] = { Engine::Spectral, Engine::Resonestor, Engine::Oliverb, Engine::SpectralClouds };
    for (int m : trip)
    {
        r.p.mode = m;
        r.run (0.5, 0.0);
        r.e.recordLayer();            // must be ignored in these modes
        r.run (0.3, 700.0);
        check (r.e.layerStatus (3).state == Engine::LayerFilled, "record is ignored in a mode that can't hold layers", m);
        check (r.e.loadedLayer() == 0, "no layer is loaded in a mode that can't play layers", m);
        r.p.mode = Engine::LoopingDelay;
        double a2 = 0; const int t = r.listen (&a2, 1.0);
        check (t == 3, "layer 3 is back after going through the other mode", m, t);
        check (r.lastRms > 0.05, "layer 3 is audible after the trip", r.lastRms);
    }
    // switch layer while in another mode, come back: the new selection plays
    r.p.mode = Engine::Spectral; r.run (0.3, 0.0);
    r.p.layer = 5; r.run (0.3, 0.0);
    r.p.mode = Engine::LoopingDelay;
    double a5 = 0; check (r.listen (&a5, 1.0) == 5, "layer chosen while in another mode plays on return", a5);
    check (hashOf (r.e.layerBytes (3, 0)) == h3 && hashOf (r.e.layerBytes (5, 0)) == h5, "stored layers unchanged by the trips");

    // benign mode changes (no rebuild inside the processor) keep the layer loaded and playing
    for (int m : { (int) Engine::Granular, (int) Engine::PitchShifterStretch, (int) Engine::BeatRepeat, (int) Engine::LoopingDelay })
    {
        r.p.mode = m;
        auto out = r.run (1.0, 0.0, 0.5f, 0.25);
        check (r.e.loadedLayer() == 5, "layer stays loaded across Granular / Stretch / Beat repeat / Looping delay", m);
        check (rms (out, out.size() / 2) > 0.01, "something plays from the layer in this mode", m, rms (out, out.size() / 2));
    }
    check (r.finite, "output finite and bounded");
}

// ---- C: switching while sound plays, and going back to Live -----------------------------------------------------------
static void testSwitchingAndLive (int engineRate)
{
    std::printf ("[C] switching while playing, Live (engine %s)\n", engineRate ? "48k" : "32k");
    Rig r (48000.0, engineRate, 0, 0, Engine::LoopingDelay);
    r.select (1); r.recordFull (kTone[1]);
    r.select (2); r.recordFull (kTone[2]);
    const uint64_t h1 = hashOf (r.e.layerBytes (1, 0)), h2 = hashOf (r.e.layerBytes (2, 0));
    r.select (1);
    r.run (0.6, 0.0);
    r.maxStep = 0.0;
    std::vector<float> all;
    for (int i = 0; i < 6; ++i)
    {
        r.p.layer = (i % 2 == 0) ? 2 : 1;
        auto o = r.run (0.5, 0.0); all.insert (all.end(), o.begin(), o.end());
        double amp = 0; const int t = strongestTone (o, (size_t) (0.25 * 48000), 48000.0, &amp);
        check (t == r.p.layer, "after a switch the new layer plays", t, r.p.layer);
    }
    // A pure tone at 660 Hz and 0.5 amplitude never steps by more than about 0.043 between samples; the engine's output
    // gain and the loop joins add some. The number is reported; the limit only catches gross jumps.
    std::printf ("    largest sample-to-sample step while switching: %.3f\n", r.maxStep);
    check (r.maxStep < 0.4, "no huge jump in the output while switching", r.maxStep);

    // Live: the always-recording buffer, fed a tone no layer has; layers are untouched
    r.p.layer = 0;
    r.run (0.3, 0.0);
    check (r.e.loadedLayer() == 0, "Live loads no layer");
    auto live = r.run (1.5, kTone[0]);
    double amp = 0; const int t = strongestTone (live, (size_t) (0.4 * 48000), 48000.0, &amp);
    check (t == 0 && rms (live, 19200) > 0.05, "Live plays the live input, not a layer", t, rms (live, 19200));
    check (hashOf (r.e.layerBytes (1, 0)) == h1 && hashOf (r.e.layerBytes (2, 0)) == h2, "layers untouched while Live runs");
    // and back to a layer: the layer's sound returns, not the Live sound
    r.p.layer = 2; r.run (0.1, 0.0);
    double a2 = 0; check (r.listen (&a2) == 2, "going back to a layer from Live plays the layer", a2);
    check (r.finite, "output finite and bounded");
}

// ---- D / E / F: clear, re-record, partial recording ------------------------------------------------------------------
static void testClearRerecordPartial (int engineRate)
{
    std::printf ("[D/E/F] clear, re-record, partial recording (engine %s)\n", engineRate ? "48k" : "32k");
    Rig r (48000.0, engineRate, 0, 0, Engine::LoopingDelay);
    for (int k = 1; k <= 3; ++k) { r.select (k); r.recordFull (kTone[k]); }
    uint64_t h[4]; for (int k = 1; k <= 3; ++k) h[k] = hashOf (r.e.layerBytes (k, 0));

    // D: clear layer 2
    r.select (2); r.e.clearLayer(); r.run (0.2, 0.0);
    check (r.e.layerStatus (2).state == Engine::LayerEmpty, "clearing empties the layer");
    check (r.e.layerStatus (1).state == Engine::LayerFilled && r.e.layerStatus (3).state == Engine::LayerFilled, "the other layers stay filled");
    check (hashOf (r.e.layerBytes (1, 0)) == h[1] && hashOf (r.e.layerBytes (3, 0)) == h[3], "the other layers' audio is unchanged");
    auto out = r.run (0.8, 0.0);
    check (rms (out, out.size() / 2) < 0.005, "an empty layer plays silence", rms (out, out.size() / 2));
    r.select (1); check (r.listen() == 1, "layer 1 still plays after layer 2 was cleared");
    r.select (2);

    // E: re-record layer 2 with a different tone: it is replaced
    r.recordFull (kTone[5]);
    check (r.e.layerStatus (2).state == Engine::LayerFilled, "an emptied layer can be recorded again");
    check (hashOf (r.e.layerBytes (2, 0)) != h[2], "re-recording replaces the audio");
    double a = 0; check (r.listen (&a) == 5, "the replacement tone plays", a);
    // re-record a filled layer without clearing: it is replaced as well
    r.select (3); r.recordFull (kTone[6]);
    check (hashOf (r.e.layerBytes (3, 0)) != h[3], "recording over a filled layer replaces it");
    check (r.listen (&a) == 6, "the new recording plays, not the old one", a);
    check (hashOf (r.e.layerBytes (1, 0)) == h[1], "layer 1 untouched by all that");

    // F: stop a recording early: what was recorded is kept, the rest is silent
    r.select (4);
    r.e.recordLayer(); r.run (0.3, kTone[4]);
    check (r.e.layerStatus (4).state == Engine::LayerRecording, "recording shows as recording");
    r.e.recordLayer(); r.run (0.1, 0.0);
    const auto st = r.e.layerStatus (4);
    check (st.state == Engine::LayerFilled, "pressing record again stops and keeps the recording");
    check (st.seconds > 0.24 && st.seconds < 0.34, "a partial layer reports about the time recorded (0.3 s)", st.seconds);
    r.p.position = 0.0f;   // loop ends at the write head, i.e. inside the part that was recorded
    double ap = 0; check (r.listen (&ap) == 4 && r.lastRms > 0.03, "a partial layer plays its tone", r.lastRms);
    r.p.position = 0.3f;
    // changing layer while recording keeps what was recorded
    r.select (6);
    r.e.recordLayer(); r.run (0.2, kTone[6]);
    r.p.layer = 1; r.run (0.3, 0.0);
    check (r.e.layerStatus (6).state == Engine::LayerFilled, "switching layer during a recording keeps the recording", r.e.layerStatus (6).state);
    check (r.e.layerStatus (6).seconds > 0.12 && r.e.layerStatus (6).seconds < 0.3, "...and its length", r.e.layerStatus (6).seconds);
    check (r.finite, "output finite and bounded");
}

// ---- G: format changes empty the layers ------------------------------------------------------------------------------
static void testFormatChange ()
{
    std::printf ("[G] changing quality / buffer length / engine rate empties the layers\n");
    Rig r (48000.0, 1, 0, 0, Engine::LoopingDelay);
    auto fill = [&] { for (int k = 1; k <= 2; ++k) { r.select (k); r.recordFull (kTone[k]); } };
    auto allEmpty = [&] { for (int k = 1; k <= Engine::kNumLayers; ++k) if (r.e.layerStatus (k).state != Engine::LayerEmpty) return false; return true; };
    fill(); check (r.e.layerStatus (1).state == Engine::LayerFilled, "filled before the change");
    r.p.quality = 1; r.run (0.4, 0.0);
    check (allEmpty() && r.e.layerStoredBytes() == 0, "quality change empties all layers");
    r.p.quality = 0; r.run (0.4, 0.0);
    fill(); r.p.bufferLength = 1; r.run (0.4, 0.0);
    check (allEmpty() && r.e.layerStoredBytes() == 0, "buffer length change empties all layers");
    r.p.bufferLength = 0; r.run (0.4, 0.0);
    fill(); r.p.engineRate = 0; r.run (0.4, 0.0);
    check (allEmpty() && r.e.layerStoredBytes() == 0, "engine rate change empties all layers");
    // and recording works in the new format
    r.select (1); r.recordFull (kTone[1]);
    check (r.e.layerStatus (1).state == Engine::LayerFilled, "recording works after a format change");
    check (r.listen() == 1, "and plays");
    // a re-prepare (the host restarting the plug-in at a new sample rate) keeps the layers
    r.e.prepare (44100.0); r.hostRate = 44100.0; r.run (0.5, 0.0);
    check (r.e.layerStatus (1).state == Engine::LayerFilled, "a re-prepare keeps the layers");
    r.p.layer = 1; double a = 0; check (r.listen (&a, 1.0) == 1, "and layer 1 still plays after it", a);
    check (r.finite, "output finite and bounded");
}

// ---- H: every quality, both rates ------------------------------------------------------------------------------------
static void testQualities (int engineRate)
{
    std::printf ("[H] every quality (engine %s)\n", engineRate ? "48k" : "32k");
    const char* names[] = { "stereo 16-bit", "mono 16-bit", "stereo 8-bit low-fi", "mono 8-bit low-fi" };
    for (int q = 0; q < 4; ++q)
    {
        Rig r (48000.0, engineRate, q, 0, Engine::LoopingDelay);
        r.select (2);
        const double fed = r.recordFull (kTone[2], 14.0);
        const auto st = r.e.layerStatus (2);
        check (st.state == Engine::LayerFilled, "layer fills in every quality", q, st.state);
        const size_t b0 = r.e.layerBytes (2, 0).size(), b1 = r.e.layerBytes (2, 1).size();
        check (b0 > 0 && ((q & 1) ? b1 == 0 : b1 == b0), "stereo layers hold two channels, mono one", q);
        std::printf ("    %-20s: %.2f s long, %zu bytes\n", names[q], st.seconds, b0 + b1);
        (void) fed;
        double a = 0; const int t = r.listen (&a, 1.0);
        check (t == 2, "the layer plays its tone in every quality", q, t);
        check (r.lastRms > 0.03, "...audibly", q, r.lastRms);
        check (r.finite, "output finite and bounded", q);
    }
}

// ---- I: nothing overwrites a playing layer ---------------------------------------------------------------------------
static void testHoldWrites (int engineRate)
{
    std::printf ("[I] a playing layer is not overwritten by live input (engine %s)\n", engineRate ? "48k" : "32k");
    for (int mode : { (int) Engine::Granular, (int) Engine::LoopingDelay, (int) Engine::PitchShifterStretch, (int) Engine::BeatRepeat })
    {
        Rig r (48000.0, engineRate, 0, 0, Engine::LoopingDelay);
        r.select (1); r.recordFull (kTone[1]);
        r.p.mode = mode;
        r.p.density = (mode == Engine::BeatRepeat) ? 0.5f : 0.8f;   // (in Beat repeat Density changes how slices play)
        r.p.dryWet = 1.0f;
        r.run (0.3, 0.0, 0.5f, 0.25);
        // loud live input of another pitch, for long enough to overwrite the whole 1 s memory twice
        auto out = r.run (2.5, kTone[0], 0.8f, 0.25);
        double a = 0;
        const size_t n = (size_t) (0.8 * 48000);
        const int t = strongestTone (out, n, 48000.0, &a);
        // how much of the output's power is the live input's tone (a pure tone of amplitude A has power A^2/2)
        const double liveAmp = std::sqrt (bandEnergy (out, out.size() - n, n, kTone[0], 48000.0));
        const double liveShare = (liveAmp * liveAmp / 2.0) / std::max (1e-12, rms (out, n) * rms (out, n));
        if (mode != Engine::BeatRepeat)   // (Beat repeat plays its slices at its own pitch, so its tone can't be compared)
            check (t == 1, "the layer's tone plays, not the live input's", mode, t);
        check (liveShare < 0.05, "none of the live input's tone is in the output", mode, liveShare);
        check (rms (out, n) > 0.03, "...and the layer is audible", mode, rms (out, n));
        check (r.e.loadedLayer() == 1, "the layer stays loaded", mode);
    }
}

// Beat repeat records its input even when frozen, so only the layer "hold" keeps a playing layer safe there. Run the same
// thing twice, once with silent live input and once with loud live input: if the layer is protected the output is identical.
static std::vector<float> beatRepeatWithLayer (int engineRate, double liveTone)
{
    stmlib::Random::Seed (1);
    Rig r (48000.0, engineRate, 0, 0, Engine::LoopingDelay);
    r.select (1); r.recordFull (kTone[1]);
    r.p.mode = Engine::BeatRepeat; r.p.density = 0.5f; r.p.dryWet = 1.0f;
    r.run (0.3, 0.0, 0.5f, 0.25);
    return r.run (2.0, liveTone, 0.8f, 0.25);
}

static void testBeatRepeatHold (int engineRate)
{
    std::printf ("[I2] Beat repeat: live input makes no difference while a layer plays (engine %s)\n", engineRate ? "48k" : "32k");
    const auto quiet = beatRepeatWithLayer (engineRate, 0.0);
    const auto loud  = beatRepeatWithLayer (engineRate, kTone[0]);
    double d = 0, s = 0;
    for (size_t i = quiet.size() / 2; i < quiet.size() && i < loud.size(); ++i) { d += (double) (quiet[i] - loud[i]) * (quiet[i] - loud[i]); s += (double) quiet[i] * quiet[i]; }
    const double rel = std::sqrt (d / std::max (1e-12, s));
    std::printf("    difference between the two outputs, relative to the output: %.6f\n", rel);
    check (s > 1e-3, "Beat repeat plays the layer", s);
    check (rel < 0.01, "Beat repeat output does not depend on the live input while a layer plays", rel);
}

// ---- J: record does nothing in Live ---------------------------------------------------------------------------------
static void testRecordIgnoredInLive ()
{
    std::printf ("[J] record does nothing in Live\n");
    Rig r (48000.0, 1, 0, 0, Engine::LoopingDelay);
    r.p.layer = 0;
    r.e.recordLayer(); r.run (0.5, 500.0);
    bool any = false; for (int k = 1; k <= Engine::kNumLayers; ++k) any |= r.e.layerStatus (k).state != Engine::LayerEmpty;
    check (! any, "no layer is touched by Record while Live is selected");
}

// ---- K: memory ---------------------------------------------------------------------------------------------------------
static void testMemory (int engineRate)
{
    std::printf ("[K] memory of six 10 s layers (engine %s)\n", engineRate ? "48k" : "32k");
    Rig r (48000.0, engineRate, 0, 2, Engine::LoopingDelay);   // 10 s
    for (int k = 1; k <= Engine::kNumLayers; ++k) { r.select (k); r.recordFull (kTone[k], 14.0); }
    const size_t bytes = r.e.layerStoredBytes();
    const double mb = (double) bytes / 1.0e6;
    const double expectMb = 6.0 * 2.0 * 640000.0 * (engineRate ? 1.5 : 1.0) / 1.0e6;
    std::printf ("    six 10 s stereo layers: %.2f MB stored (calculated %.2f MB)\n", mb, expectMb);
    check (std::fabs (mb - expectMb) < 0.02 * expectMb, "stored size matches the calculation", mb, expectMb);
    for (int k = 1; k <= Engine::kNumLayers; ++k)
        check (r.e.layerStatus (k).seconds > 9.9 && r.e.layerStatus (k).seconds < 10.1, "each layer is about 10 s", k, r.e.layerStatus (k).seconds);
    r.p.mode = Engine::LoopingDelay;
    for (int k : { 2, 6, 1 }) { r.select (k); double a = 0; check (r.listen (&a) == k, "long layers play too", k, a); }
}

// ---- L: how long the layer swap takes (the memory copy) -------------------------------------------------------------------
static void testSwitchCost ()
{
    std::printf ("[L] cost of a layer switch\n");
    for (int len : { 2, 3 })
    {
        Rig r (48000.0, 1, 0, len, Engine::LoopingDelay);
        r.select (1); r.recordFull (kTone[1], 25.0);
        r.select (2); r.recordFull (kTone[2], 25.0);
        r.maxCallMs = 0;
        for (int i = 0; i < 6; ++i) { r.p.layer = 1 + (i % 2); r.run (0.2, 0.0); }
        std::printf ("    %.0f s layers, 48 kHz stereo: slowest process() call during switching: %.2f ms (512-sample calls)\n",
                     Engine::bufferSeconds (len), r.maxCallMs);
        check (r.maxCallMs < 100.0, "a switch does not stall for long on this machine", r.maxCallMs);
    }
}


// ======================================================================================================================
// Switch Time and WAV Save / Load
// ======================================================================================================================
#include "LayerAudio.h"
#include <string>

// RMS of consecutive windows of `ms` milliseconds.
static std::vector<double> envelope (const std::vector<float>& x, double sr, double ms)
{
    const size_t w = (size_t) (sr * ms / 1000.0);
    std::vector<double> e;
    for (size_t i = 0; i + w <= x.size(); i += w)
    {
        double s = 0; for (size_t k = 0; k < w; ++k) s += (double) x[i + k] * x[i + k];
        e.push_back (std::sqrt (s / (double) w));
    }
    return e;
}

// How long (ms) the envelope stays below `frac` of `ref`, counted from the first window that drops below it.
static double quietMs (const std::vector<double>& e, double ms, double ref, double frac)
{
    int n = 0; for (double v : e) if (v < frac * ref) ++n;
    return n * ms;
}

// ---- M: Switch Time ---------------------------------------------------------------------------------------------------------
static void testSwitchTime (int engineRate)
{
    std::printf ("[M] Switch Time: fade out, swap, fade in (engine %s)\n", engineRate ? "48k" : "32k");
    const double sr = 48000.0;
    Rig r (sr, engineRate, 0, 0, Engine::LoopingDelay);
    r.select (1); r.recordFull (kTone[1]);
    r.select (2); r.recordFull (kTone[2]);
    r.select (3); r.recordFull (kTone[3]);
    r.select (1);
    double steady = 0; check (r.listen (&steady) == 1, "layer 1 plays before the switch");
    const double ref = r.lastRms;
    check (ref > 0.05, "layer 1 is audible", ref);

    // Instant (Switch Time 0): the gap is short (the 24 ms mute).
    r.p.switchTimeMs = 0.0f;
    r.p.layer = 2; auto o0 = r.run (0.6, 0.0);
    const double gap0 = quietMs (envelope (o0, sr, 5.0), 5.0, ref, 0.2);
    check (gap0 < 90.0, "Switch Time 0 keeps the short swap", gap0);
    check (strongestTone (o0, (size_t) (0.2 * sr), sr) == 2, "...and lands on layer 2");

    // 800 ms: a long dip, the first part is layer 2's tone, the last part is layer 1's, and nothing steps.
    r.p.switchTimeMs = 800.0f;
    r.maxStep = 0.0;
    r.p.layer = 1; auto o1 = r.run (1.6, 0.0);
    const double gap1 = quietMs (envelope (o1, sr, 5.0), 5.0, ref, 0.2);
    check (gap1 > 120.0 && gap1 < 450.0 && gap1 > 2.0 * gap0, "Switch Time 800 ms gives a much longer fade through quiet than 0", gap1, gap0);
    const auto e1 = envelope (o1, sr, 20.0);
    // the quietest point is near the middle of the 800 ms (swap time), within a generous window around 400 ms (+ engine latency)
    size_t qi = 0; for (size_t i = 0; i < e1.size(); ++i) if (e1[i] < e1[qi]) qi = i;
    const double qms = (double) qi * 20.0;
    check (qms > 250.0 && qms < 650.0, "the quietest moment is mid-way through the fade", qms);
    check (e1[qi] < 0.1 * ref, "it fades almost to silence", e1[qi] / ref);
    std::vector<float> head (o1.begin(), o1.begin() + (long) (0.18 * sr)), tail (o1.end() - (long) (0.3 * sr), o1.end());
    check (strongestTone (tail, tail.size(), sr) == 1, "after the fade the new layer is playing");
    check (r.maxStep < 0.12, "no step in the output during a long fade", r.maxStep);
    check (r.finite, "output stays finite");
    check (r.e.loadedLayer() == 1, "the engine reports the new layer as playing", r.e.loadedLayer());

    // The fade is smooth: no 20 ms window jumps up or down by more than half of the reference in one step (a click/step would).
    double worst = 0; for (size_t i = 1; i < e1.size(); ++i) worst = std::max (worst, std::fabs (e1[i] - e1[i - 1]));
    check (worst < 0.5 * ref, "envelope changes gradually", worst / ref);

    // Switch asked for again while fading in: ends on the last one asked for, without getting stuck quiet.
    r.p.switchTimeMs = 400.0f;
    r.p.layer = 2; r.run (0.3, 0.0);
    r.p.layer = 3; r.run (0.05, 0.0);
    r.p.layer = 1; r.run (0.1, 0.0);
    r.p.layer = 3; r.run (1.5, 0.0);
    double a3 = 0; check (r.listen (&a3) == 3, "rapid switching ends on the last layer asked for", a3);
    check (r.lastRms > 0.7 * ref, "and it is back at full level", r.lastRms / ref);

    // Switch Time changed back to 0 half-way through a fade: still finishes, at full level, right layer.
    r.p.switchTimeMs = 1000.0f; r.p.layer = 1; r.run (0.3, 0.0);
    r.p.switchTimeMs = 0.0f;    r.run (1.5, 0.0);
    check (r.listen() == 1, "changing Switch Time mid-fade still lands on the layer");
    check (r.lastRms > 0.7 * ref, "at full level", r.lastRms / ref);

    // Going to Live and back with a fade.
    r.p.switchTimeMs = 200.0f;
    r.p.layer = 0; r.run (0.8, 0.0);
    check (r.e.loadedLayer() == 0, "Live is selected after the fade");
    r.p.layer = 2; r.run (0.8, 0.0);
    check (r.listen() == 2, "and back to a layer");
}

// ---- N: WAV files --------------------------------------------------------------------------------------------------------------
static std::vector<uint8_t> wavBytes (int format, int ch, int bits, int rate, const std::vector<double>& interleaved, bool extensible = false)
{
    std::vector<uint8_t> v;
    auto w32 = [&] (uint32_t x) { for (int i = 0; i < 4; ++i) v.push_back ((uint8_t) (x >> (8 * i))); };
    auto w16 = [&] (uint16_t x) { v.push_back ((uint8_t) x); v.push_back ((uint8_t) (x >> 8)); };
    auto tag = [&] (const char* s) { for (int i = 0; i < 4; ++i) v.push_back ((uint8_t) s[i]); };
    const size_t bytes = interleaved.size() * (size_t) (bits / 8);
    tag ("RIFF"); w32 ((uint32_t) (4 + (8 + (extensible ? 40 : 16)) + 8 + bytes)); tag ("WAVE");
    tag ("fmt "); w32 (extensible ? 40 : 16); w16 (extensible ? 0xFFFE : (uint16_t) format); w16 ((uint16_t) ch); w32 ((uint32_t) rate);
    w32 ((uint32_t) (rate * ch * bits / 8)); w16 ((uint16_t) (ch * bits / 8)); w16 ((uint16_t) bits);
    if (extensible) { w16 (22); w16 ((uint16_t) bits); w32 (0); w16 ((uint16_t) format); for (int i = 0; i < 14; ++i) v.push_back (0); }
    tag ("data"); w32 ((uint32_t) bytes);
    for (double x : interleaved)
    {
        if (format == 3 && bits == 32) { float f = (float) x; uint32_t u; std::memcpy (&u, &f, 4); w32 (u); }
        else if (format == 3) { double f = x; uint64_t u; std::memcpy (&u, &f, 8); w32 ((uint32_t) u); w32 ((uint32_t) (u >> 32)); }
        else if (bits == 8)  v.push_back ((uint8_t) (int) std::lround (x * 127.0 + 128.0));
        else if (bits == 16) w16 ((uint16_t) (int16_t) std::lround (x * 32767.0));
        else if (bits == 24) { const int s = (int) std::lround (x * 8388607.0); v.push_back ((uint8_t) s); v.push_back ((uint8_t) (s >> 8)); v.push_back ((uint8_t) (s >> 16)); }
        else                 w32 ((uint32_t) (int32_t) std::lround (x * 2147483647.0));
    }
    return v;
}

static void testWavFiles ()
{
    std::printf ("[N1] WAV reading and writing\n");
    // write -> read is exact for 16-bit values
    LayerAudio a; a.channels = 2; a.sampleRate = 44100.0;
    for (int i = 0; i < 1000; ++i) { a.ch[0].push_back ((float) ((i * 37) % 65536 - 32768) / 32768.0f); a.ch[1].push_back ((float) ((i * 91) % 65536 - 32768) / 32768.0f); }
    auto bytes = writeWav16 (a);
    LayerAudio b; std::string err;
    check (readWav (bytes.data(), bytes.size(), b, err), "reads what it wrote", 0, 0);
    check (b.channels == 2 && b.sampleRate == 44100.0 && b.frames() == 1000, "format and length survive", b.channels, (double) b.frames());
    bool same = true; for (size_t i = 0; i < 1000; ++i) same &= (b.ch[0][i] == a.ch[0][i] && b.ch[1][i] == a.ch[1][i]);
    check (same, "16-bit samples survive exactly");
    LayerAudio m; m.channels = 1; m.sampleRate = 16000.0; m.ch[0] = { 0.0f, 0.5f, -0.5f, 0.25f };
    auto mb = writeWav16 (m); LayerAudio m2;
    check (readWav (mb.data(), mb.size(), m2, err) && m2.channels == 1 && m2.frames() == 4 && m2.sampleRate == 16000.0, "mono file");

    // other formats
    const std::vector<double> s = { 0.5, -0.5, 0.25, -0.25, 1.0, -1.0 };    // 3 stereo frames
    struct Case { int fmt, bits; bool ext; double tol; const char* name; };
    for (const Case c : { Case { 1, 8, false, 0.01, "8-bit" }, Case { 1, 16, false, 1e-4, "16-bit" }, Case { 1, 24, false, 1e-6, "24-bit" },
                          Case { 1, 32, false, 1e-6, "32-bit int" }, Case { 3, 32, false, 1e-6, "32-bit float" }, Case { 3, 64, false, 1e-9, "64-bit float" },
                          Case { 1, 24, true, 1e-6, "24-bit extensible" }, Case { 3, 32, true, 1e-6, "float extensible" } })
    {
        auto wb = wavBytes (c.fmt, 2, c.bits, 48000, s, c.ext);
        LayerAudio r;
        const bool ok = readWav (wb.data(), wb.size(), r, err);
        check (ok && r.frames() == 3 && r.channels == 2, c.name, ok, (double) r.frames());
        if (ok) { double worst = 0; for (size_t i = 0; i < 3; ++i) { worst = std::max (worst, std::fabs ((double) r.ch[0][i] - s[2 * i])); worst = std::max (worst, std::fabs ((double) r.ch[1][i] - s[2 * i + 1])); }
                  check (worst < c.tol + 1e-3 || (c.bits == 32 && c.fmt == 1 && worst < 2e-3), c.name, worst, c.tol); }
    }
    // more than two channels keeps the first two
    auto six = wavBytes (1, 6, 16, 48000, std::vector<double> { 0.1, 0.2, 0.3, 0.4, 0.5, 0.6 });
    LayerAudio s6; check (readWav (six.data(), six.size(), s6, err) && s6.channels == 2 && s6.frames() == 1 && std::fabs (s6.ch[1][0] - 0.2f) < 1e-3, "six-channel file keeps the first two");
    // a data chunk that claims more than the file holds (streamed files)
    auto cut = wavBytes (1, 1, 16, 48000, std::vector<double> (100, 0.1)); cut.resize (cut.size() - 60);
    LayerAudio c2; check (readWav (cut.data(), cut.size(), c2, err) && c2.frames() == 70, "a cut-off data chunk reads what is there", (double) c2.frames());

    // things that must be refused, each with a message
    std::vector<uint8_t> junk (200, 0x41);
    check (! readWav (junk.data(), junk.size(), b, err) && ! err.empty(), "garbage is refused");
    check (! readWav (bytes.data(), 20, b, err) && ! err.empty(), "a truncated header is refused");
    check (! readWav (bytes.data(), 0, b, err), "an empty file is refused");
    auto adpcm = wavBytes (1, 1, 16, 48000, std::vector<double> (10, 0.1)); adpcm[20] = 2;   // format tag 2 = ADPCM
    check (! readWav (adpcm.data(), adpcm.size(), b, err) && ! err.empty(), "compressed WAV is refused");
    auto nodata = wavBytes (1, 1, 16, 48000, {}); check (! readWav (nodata.data(), nodata.size(), b, err), "a WAV with no samples is refused");
    auto badRate = wavBytes (1, 1, 16, 5, std::vector<double> (10, 0.1)); check (! readWav (badRate.data(), badRate.size(), b, err), "a silly sample rate is refused");
    // random garbage with a valid header start must never crash
    uint32_t rng = 12345; int survived = 0;
    for (int k = 0; k < 300; ++k)
    {
        auto g = wavBytes (1, 2, 16, 48000, std::vector<double> (64, 0.1));
        for (int j = 0; j < 6; ++j) { rng = rng * 1664525u + 1013904223u; g[rng % g.size()] = (uint8_t) (rng >> 24); }
        if (k % 3 == 0) g.resize (rng % g.size());
        LayerAudio q; std::string e; readWav (g.data(), g.size(), q, e); ++survived;
    }
    check (survived == 300, "300 damaged files never crash the reader");
}

static Engine::IoResult waitExport (Rig& r, LayerAudio& out)
{
    for (int i = 0; i < 40; ++i) { r.run (0.02, 0.0); const auto s = r.e.pollExport (out); if (s != Engine::IoNone) return s; }
    return Engine::IoNone;
}
static Engine::IoResult waitImport (Rig& r)
{
    for (int i = 0; i < 40; ++i) { r.run (0.02, 0.0); const auto s = r.e.pollImport(); if (s != Engine::IoNone) return s; }
    return Engine::IoNone;
}

static LayerAudio makeTone (double rate, double seconds, double hzL, double hzR, float amp = 0.5f, bool mono = false)
{
    LayerAudio a; a.channels = mono ? 1 : 2; a.sampleRate = rate;
    const size_t n = (size_t) (seconds * rate);
    a.ch[0].resize (n); if (! mono) a.ch[1].resize (n);
    for (size_t i = 0; i < n; ++i)
    {
        a.ch[0][i] = amp * (float) std::sin (2.0 * 3.14159265358979 * hzL * (double) i / rate);
        if (! mono) a.ch[1][i] = amp * (float) std::sin (2.0 * 3.14159265358979 * hzR * (double) i / rate);
    }
    return a;
}

// The audio stored in a layer, decoded (16-bit or low-fi), frames = the layer length.
static std::vector<float> storedAudio (Rig& r, int layer, int ch, bool lowFi)
{
    const auto& m = r.e.layerBytes (layer, ch);
    const size_t n = lowFi ? m.size() : m.size() / 2;
    std::vector<float> x (n > 8 ? n - 8 : 0);
    for (size_t i = 0; i < x.size(); ++i)
        x[i] = lowFi ? (float) clouds::MuLaw2Lin (m[i]) / 32768.0f : (float) (int16_t) (m[2 * i] | (m[2 * i + 1] << 8)) / 32768.0f;
    return x;
}
static size_t framesOf (Rig& r, int layer) { return (size_t) std::lround (r.e.layerStatus (layer).seconds * storedRate (r.p.engineRate, r.p.quality)); }

// Which of the layer tones is in a stretch of audio, and how loud (amplitude)
static int toneIn (const std::vector<float>& x, size_t from, size_t count, double sr, double* amp = nullptr)
{
    std::vector<float> seg (x.begin() + (long) from, x.begin() + (long) std::min (x.size(), from + count));
    return strongestTone (seg, seg.size(), sr, amp);
}

static void testSaveLoad (int engineRate, int quality)
{
    const bool lowFi = quality >= 2, mono = (quality & 1) != 0;
    std::printf ("[N2] Save / Load a layer (engine %s, quality %d)\n", engineRate ? "48k" : "32k", quality);
    Rig r (48000.0, engineRate, quality, 0, Engine::LoopingDelay);
    const double rate = storedRate (engineRate, quality);
    const auto fmt = r.e.layerFormat();
    check (fmt.valid && fmt.lowFi == lowFi && fmt.channels == (mono ? 1 : 2) && fmt.sampleRate == rate, "layer format is published correctly", fmt.valid, fmt.sampleRate);
    check (fmt.capFrames > (int) (0.9 * rate), "layer is about a second long", fmt.capFrames);

    // ---- Save: record, export, check the audio
    r.select (3); r.recordFull (kTone[3]);
    LayerAudio ex;
    check (r.e.requestExport (3), "export request accepted");
    check (! r.e.requestExport (3), "a second request while one is pending is refused");
    check (waitExport (r, ex) == Engine::ExportReady, "export arrives");
    check (ex.channels == (mono ? 1 : 2) && ex.sampleRate == rate, "exported format", ex.channels, ex.sampleRate);
    check (ex.frames() == framesOf (r, 3) || std::llabs ((long long) ex.frames() - (long long) framesOf (r, 3)) <= 1, "exported length is the layer length", (double) ex.frames(), (double) framesOf (r, 3));
    double amp = 0; check (toneIn (ex.ch[0], 0, ex.frames() - 64, rate, &amp) == 3, "exported audio is the recorded tone", amp);
    check (amp > 0.35 && amp < 0.65, "at the recorded level", amp);
    LayerAudio none; r.e.requestExport (5);
    check (waitExport (r, none) == Engine::ExportEmpty, "exporting an empty layer is reported as empty");
    check (r.e.requestExport (3), "export works again after that");
    LayerAudio ex2; waitExport (r, ex2);

    // ---- full cycle like the plug-in does it: WAV bytes in between
    const auto wav = writeWav16 (ex);
    LayerAudio back; std::string err; check (readWav (wav.data(), wav.size(), back, err), "the saved WAV reads back");
    r.select (4);
    check (r.e.submitImport (4, back, err), "load accepted");
    check (waitImport (r) == Engine::ImportOk, "load applied");
    check (r.e.layerStatus (4).state == Engine::LayerFilled, "layer 4 is filled");
    check (std::fabs (r.e.layerStatus (4).seconds - r.e.layerStatus (3).seconds) < 0.002, "same length as layer 3", r.e.layerStatus (4).seconds, r.e.layerStatus (3).seconds);
    for (int ch = 0; ch < (mono ? 1 : 2); ++ch)
    {
        const auto A = storedAudio (r, 3, ch, lowFi), B = storedAudio (r, 4, ch, lowFi);
        double worst = 0; for (size_t i = 0; i < std::min (A.size(), B.size()); ++i) worst = std::max (worst, std::fabs ((double) A[i] - B[i]));
        check (A.size() == B.size() && worst < 1e-6, "save then load gives back the same samples", worst, (double) A.size());
    }
    check (r.e.layerBytes (3, 0).size() == r.e.layerBytes (4, 0).size(), "same memory size");
    double a4 = 0; check (r.listen (&a4) == 3, "layer 4 now plays layer 3's tone", a4);
    check (r.lastRms > 0.05, "audibly", r.lastRms);

    // ---- Load files at other rates, mono / stereo, longer than the layer
    struct F { double rate; bool monoFile; double secs; const char* what; };
    for (const F f : { F { 48000.0, false, 0.6, "48 kHz stereo" }, F { 44100.0, false, 0.6, "44.1 kHz stereo" }, F { 96000.0, true, 0.6, "96 kHz mono" },
                       F { 22050.0, true, 0.6, "22.05 kHz mono" }, F { 44100.0, false, 3.0, "3 s file (longer than the layer)" } })
    {
        const auto file = makeTone (f.rate, f.secs, kTone[2], kTone[5], 0.5f, f.monoFile);
        r.select (6);
        check (r.e.submitImport (6, file, err), f.what);
        check (waitImport (r) == Engine::ImportOk, f.what);
        const size_t expectFrames = (size_t) std::min<double> (f.secs * rate, (double) fmt.capFrames);
        check (std::llabs ((long long) framesOf (r, 6) - (long long) expectFrames) <= 2, "loaded length", (double) framesOf (r, 6), (double) expectFrames);
        const auto L = storedAudio (r, 6, 0, lowFi);
        double aL = 0; const int tl = toneIn (L, 400, framesOf (r, 6) - 800, rate, &aL);
        // the left channel: the file's left tone (a mono layer mixes both channels, so then either tone is expected)
        const bool stereoFileIntoMonoLayer = mono && ! f.monoFile;
        check (tl == 2 || (stereoFileIntoMonoLayer && tl == 5) || (stereoFileIntoMonoLayer && tl == -1), f.what, tl, aL);
        if (! mono)
        {
            const auto R = storedAudio (r, 6, 1, lowFi);
            double aR = 0; const int tr = toneIn (R, 400, framesOf (r, 6) - 800, rate, &aR);
            check (tr == (f.monoFile ? 2 : 5), "right channel", tr, aR);
            check (aR > 0.3 && aR < 0.65, "level kept", aR);
        }
        if (! stereoFileIntoMonoLayer) check (aL > 0.3 && aL < 0.65, "level kept (left)", aL);
        // the interpolation tail repeats the start of the audio
        const auto& mem = r.e.layerBytes (6, 0);
        bool tailOk = true;
        for (int k = 0; k < 8; ++k)
        {
            if (lowFi) tailOk &= mem[(size_t) fmt.capFrames + (size_t) k] == mem[(size_t) k];
            else       tailOk &= std::memcmp (&mem[((size_t) fmt.capFrames + (size_t) k) * 2], &mem[(size_t) k * 2], 2) == 0;
        }
        check (tailOk, "the 8-sample tail after the layer repeats the start");
    }

    // ---- A short file leaves the rest of the layer silent
    {
        const auto file = makeTone (44100.0, 0.25, kTone[1], kTone[1]);
        r.select (5); r.e.submitImport (5, file, err); waitImport (r);
        const auto L = storedAudio (r, 5, 0, lowFi);
        const size_t n = framesOf (r, 5);
        check (std::fabs ((double) n / rate - 0.25) < 0.005, "short file gives a short layer", (double) n / rate);
        double after = 0; for (size_t i = n + 16; i + 16 < L.size(); ++i) after = std::max (after, (double) std::fabs (L[i]));
        check (after < 1e-3, "the rest of the layer memory is silent", after);
    }
}

// Timing: a click at 0.1 s in the file lands at 0.1 s in the layer (the resampler's delay is taken out).
static void testImportAlignment ()
{
    std::printf ("[N3] a loaded file keeps its timing\n");
    Rig r (48000.0, 1, 0, 0, Engine::LoopingDelay);
    std::string err;
    for (double fileRate : { 44100.0, 96000.0, 22050.0, 48000.0 })
    {
        LayerAudio a; a.channels = 1; a.sampleRate = fileRate; a.ch[0].assign ((size_t) (0.5 * fileRate), 0.0f);
        const size_t at = (size_t) (0.1 * fileRate);
        a.ch[0][at] = 0.9f;
        r.select (1);
        check (r.e.submitImport (1, a, err), "queued");
        check (waitImport (r) == Engine::ImportOk, "applied");
        const auto L = storedAudio (r, 1, 0, false);
        size_t pk = 0; for (size_t i = 0; i < L.size(); ++i) if (std::fabs (L[i]) > std::fabs (L[pk])) pk = i;
        check (std::llabs ((long long) pk - 4800) <= 3, "click position", (double) pk, 4800.0);
        check (std::fabs (L[pk]) > 0.2, "click survives resampling (a one-sample click is low-passed)", L[pk]);
    }
}

static void testImportEdgeCases ()
{
    std::printf ("[N4] loading into a playing layer, while recording, after a format change, and refusals\n");
    Rig r (48000.0, 1, 0, 0, Engine::LoopingDelay);
    std::string err;
    r.select (1); r.recordFull (kTone[1]);
    check (r.listen() == 1, "layer 1 plays its tone");
    const double ref = r.lastRms;

    // loading into the layer that is playing: swapped in (with the fade), then plays the new audio
    check (r.e.submitImport (1, makeTone (48000.0, 1.0, kTone[4], kTone[4]), err), "load into the playing layer");
    check (waitImport (r) == Engine::ImportOk, "applied");
    r.run (0.6, 0.0);
    double a = 0; check (r.listen (&a) == 4, "the playing layer now plays the loaded audio", a);
    check (r.lastRms > 0.5 * ref, "at a similar level", r.lastRms / ref);
    check (r.e.loadedLayer() == 1, "still layer 1");

    // loading into a layer that is not playing leaves the playing one alone
    check (r.e.submitImport (3, makeTone (48000.0, 1.0, kTone[6], kTone[6]), err), "load into another layer");
    check (waitImport (r) == Engine::ImportOk, "applied");
    check (r.listen() == 4, "layer 1 keeps playing");
    r.select (3); check (r.listen() == 6, "and layer 3 has the loaded audio");

    // refused while recording
    r.select (2); r.e.recordLayer(); r.run (0.2, kTone[2]);
    check (r.e.layerStatus (2).state == Engine::LayerRecording, "recording");
    check (r.e.submitImport (2, makeTone (48000.0, 1.0, kTone[5], kTone[5]), err), "queued");
    check (waitImport (r) == Engine::ImportRejected, "refused while recording");
    r.e.recordLayer(); r.run (0.2, 0.0);

    // format changed between queueing and applying
    check (r.e.submitImport (2, makeTone (48000.0, 1.0, kTone[5], kTone[5]), err), "queued");
    r.p.quality = 1;
    check (waitImport (r) == Engine::ImportRejected, "refused if the format changed meanwhile");
    check (r.e.layerStatus (2).state == Engine::LayerEmpty, "the layer is left empty");
    r.p.quality = 0; r.run (0.3, 0.0);

    // refusals up front
    check (! r.e.submitImport (0, makeTone (48000.0, 1.0, 440, 440), err) && ! err.empty(), "layer 0 (Live) is refused");
    check (! r.e.submitImport (7, makeTone (48000.0, 1.0, 440, 440), err), "layer 7 is refused");
    LayerAudio empty; empty.channels = 1; empty.sampleRate = 48000.0;
    check (! r.e.submitImport (1, empty, err) && ! err.empty(), "empty audio is refused");

    // clipping: a file louder than full scale must not wrap around
    LayerAudio loud = makeTone (48000.0, 0.5, 440, 440, 1.0f); for (auto& s : loud.ch[0]) s *= 3.0f;
    check (r.e.submitImport (5, loud, err), "loud file queued"); waitImport (r);
    const auto L = storedAudio (r, 5, 0, false);
    double mx = 0; bool sane = true; for (size_t i = 100; i + 100 < framesOf (r, 5); ++i) { mx = std::max (mx, (double) std::fabs (L[i])); }
    // a tone clipped at +-1 stays a clipped tone: its samples are never far from the +-1 rails (no wrap to the opposite sign near peaks)
    for (size_t i = 101; i + 100 < framesOf (r, 5); ++i) if (std::fabs (L[i] - L[i - 1]) > 0.5) sane = false;
    check (mx > 0.99 && sane, "overloaded file is clipped, not wrapped", mx);

    // all layers filled by loading, memory untouched by an empty request
    check (r.e.layerStoredBytes() > 0, "layers hold data");
}

// Layer length when the buffer is long
static void testImportLongBuffer ()
{
    std::printf ("[N5] loading into 10 s layers\n");
    Rig r (48000.0, 1, 0, 2, Engine::LoopingDelay);
    std::string err;
    r.select (2);
    check (r.e.submitImport (2, makeTone (44100.0, 4.0, kTone[3], kTone[3]), err), "4 s file queued");
    check (waitImport (r) == Engine::ImportOk, "applied");
    check (std::fabs (r.e.layerStatus (2).seconds - 4.0) < 0.01, "4 s layer", r.e.layerStatus (2).seconds);
    { const auto L = storedAudio (r, 2, 0, false); double am = 0;
      check (toneIn (L, 1000, framesOf (r, 2) - 2000, 48000.0, &am) == 3 && am > 0.3, "the 4 s of audio is stored (how a layer shorter than the buffer loops depends on Size and Position, as with a short recording)", am); }
    check (r.e.submitImport (2, makeTone (48000.0, 30.0, kTone[1], kTone[1]), err), "30 s file queued");
    check (waitImport (r) == Engine::ImportOk, "applied");
    check (std::fabs (r.e.layerStatus (2).seconds - 10.0) < 0.05, "30 s file is cut to the layer length", r.e.layerStatus (2).seconds);
    r.run (0.5, 0.0);
    check (r.listen() == 1, "a full-length loaded layer plays");
}

// ---- O: change counters (what lets a file link tell "the user changed it" from "a format change emptied it") -----------------------
static void testCounters ()
{
    std::printf ("[O] layer revision / edit counters\n");
    Rig r (48000.0, 1, 0, 0, Engine::LoopingDelay);
    std::string err;
    const auto rev = [&] (int k) { return r.e.layerRevision (k); };
    const auto edit = [&] (int k) { return r.e.layerEditCount (k); };
    r.select (2);
    const uint32_t r0 = rev (2), e0 = edit (2), r1b = rev (1), e1b = edit (1);
    r.e.recordLayer(); r.run (0.2, kTone[2]);
    check (rev (2) > r0 && edit (2) > e0, "starting a recording is a user edit");
    const uint32_t r1 = rev (2), e1 = edit (2);
    r.e.recordLayer(); r.run (0.2, 0.0);
    check (rev (2) > r1 && edit (2) == e1, "the recording ending changes the revision but is not a new edit", rev (2), edit (2));
    const uint32_t r2 = rev (2), e2 = edit (2);
    check (r.e.submitImport (2, makeTone (48000.0, 0.5, kTone[3], kTone[3]), err), "load");
    check (waitImport (r) == Engine::ImportOk && rev (2) > r2 && edit (2) == e2, "a load changes the revision, not the edit count");
    check (rev (1) == r1b && edit (1) == e1b, "other layers untouched");
    const uint32_t r3 = rev (2), e3 = edit (2);
    r.e.clearLayer(); r.run (0.2, 0.0);
    check (rev (2) > r3 && edit (2) > e3, "clearing is a user edit");
    check (r.e.submitImport (3, makeTone (48000.0, 0.5, kTone[3], kTone[3]), err) && waitImport (r) == Engine::ImportOk, "load into 3");
    const uint32_t r4 = rev (3), e4 = edit (3), r5 = rev (5), e5 = edit (5);
    r.p.quality = 1; r.run (0.5, 0.0);
    check (rev (3) > r4 && edit (3) == e4 && rev (5) > r5 && edit (5) == e5, "a format change bumps every revision but no edit count");
    // export snapshot
    check (r.e.submitImport (4, makeTone (48000.0, 0.5, kTone[4], kTone[4]), err) && waitImport (r) == Engine::ImportOk, "load into 4");
    LayerAudio ex; r.e.requestExport (4);
    check (waitExport (r, ex) == Engine::ExportReady && r.e.exportedRevision() == rev (4) && r.e.exportedEditCount() == edit (4), "the export reports the revision it copied");
}

int main()
{
    for (int rate = 0; rate < 2; ++rate)
    {
        testRecordAndPlayAll (48000.0, rate);
        testModeRoundTrip (rate);
        testSwitchingAndLive (rate);
        testClearRerecordPartial (rate);
        testQualities (rate);
        testHoldWrites (rate);
        testBeatRepeatHold (rate);
        testMemory (rate);
    }
    testRecordAndPlayAll (44100.0, 1);
    testRecordAndPlayAll (96000.0, 1);
    testFormatChange();
    testRecordIgnoredInLive();
    testSwitchCost();
    for (int rate = 0; rate < 2; ++rate) testSwitchTime (rate);
    testWavFiles();
    for (int rate = 0; rate < 2; ++rate) for (int q = 0; q < 4; ++q) testSaveLoad (rate, q);
    testImportAlignment();
    testImportEdgeCases();
    testImportLongBuffer();
    testCounters();
    std::printf ("\nlayers_test: %d checks, %d failed\n", gChecks, gFailed);
    return gFailed == 0 ? 0 : 1;
}
