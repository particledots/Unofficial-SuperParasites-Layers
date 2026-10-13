// Tests of the Layers feature of Unofficial SuperParasites Layers (headless, offline).
//
// What this checks: that the six layers record what is fed in, keep it, play it back, survive layer switching and trips
// through the other modes, clear and re-record independently, are emptied when the format changes, and that nothing
// overwrites a playing layer. Tones are used so that "which layer is this?" can be answered by measuring the pitch.
// What it does NOT check: how anything sounds, clicks you can hear, or any behaviour inside Logic.
#include "SuperParasitesEngine.h"
#include "stmlib/utils/random.h"
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
    std::printf ("\nlayers_test: %d checks, %d failed\n", gChecks, gFailed);
    return gFailed == 0 ? 0 : 1;
}
