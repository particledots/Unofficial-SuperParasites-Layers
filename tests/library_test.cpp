// Tests of the file references of Unofficial SuperParasites Layers (LayerLibrary): saving a layer to a file, a preset bringing it back, finding a moved
// or renamed file, missing and changed files, format changes, user edits dropping the link. Headless, with real files in a temporary folder.
// What it does NOT check: the file dialogs, the host's preset handling, or how anything sounds.
#include "SuperParasitesEngine.h"
#include "LayerLibrary.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <vector>
#include <unistd.h>

namespace fs = std::filesystem;
using Engine = SuperParasitesEngine;
using Lib = LayerLibrary;

static int gChecks = 0, gFailed = 0;
static void check (bool ok, const char* what, double a = 0.0, double b = 0.0)
{
    ++gChecks;
    if (! ok) { ++gFailed; std::printf ("  FAIL: %s  (%g, %g)\n", what, a, b); }
}
static void checkS (bool ok, const char* what, const std::string& s)
{
    ++gChecks;
    if (! ok) { ++gFailed; std::printf ("  FAIL: %s  ('%s')\n", what, s.c_str()); }
}

static const double kTone[7] = { 3000.0, 440.0, 660.0, 880.0, 1100.0, 1320.0, 1540.0 };

static double amplitudeAt (const std::vector<float>& x, size_t from, size_t count, double hz, double sr)
{
    const double w = 2.0 * 3.14159265358979 * hz / sr, c = 2.0 * std::cos (w);
    double s1 = 0, s2 = 0;
    for (size_t i = from; i < from + count; ++i) { const double s = x[i] + c * s1 - s2; s2 = s1; s1 = s; }
    return 2.0 * std::sqrt (std::max (0.0, s1 * s1 + s2 * s2 - c * s1 * s2)) / (double) count;
}
static int toneOf (const std::vector<float>& x, double sr)    // which of tones 1..6 dominates
{
    if (x.size() < 2000) return -1;
    int best = -1; double bestE = 0, second = 0;
    for (int t = 1; t <= 6; ++t)
    {
        double e = 0;
        for (int k = -6; k <= 6; ++k) { const double a = amplitudeAt (x, 500, x.size() - 1000, kTone[t] * (1.0 + 0.005 * k), sr); e += a * a; }
        if (e > bestE) { second = bestE; bestE = e; best = t; } else if (e > second) second = e;
    }
    return bestE > 4.0 * second && bestE > 1e-9 ? best : -1;
}

static LayerAudio makeTone (double rate, double seconds, double hz, float amp = 0.5f)
{
    LayerAudio a; a.channels = 2; a.sampleRate = rate;
    const size_t n = (size_t) (seconds * rate);
    a.ch[0].resize (n); a.ch[1].resize (n);
    for (size_t i = 0; i < n; ++i) a.ch[0][i] = a.ch[1][i] = amp * (float) std::sin (2.0 * 3.14159265358979 * hz * (double) i / rate);
    return a;
}

static uint64_t hashOf (const std::vector<uint8_t>& v) { return Lib::hashBytes (v.data(), v.size()); }

struct Inst
{
    Engine e;
    Lib lib;
    Engine::Params p;
    double phase = 0;
    Inst (bool prepared = true) : lib (e)
    {
        p.engineRate = 1; p.quality = 0; p.bufferLength = 0; p.mode = Engine::LoopingDelay;
        p.dryWet = 1.0f; p.size = 0.5f; p.position = 0.3f; p.density = 0.5f; p.texture = 0.5f;
        if (prepared) { e.prepare (48000.0); run (0.3, 0.0); }
    }
    void run (double seconds, double hz)
    {
        const int N = 480; std::vector<float> in (N), oL (N), oR (N);
        for (long done = 0; done < (long) (seconds * 48000.0); done += N)
        {
            for (int i = 0; i < N; ++i) { phase += hz / 48000.0; if (phase >= 1.0) phase -= 1.0; in[i] = hz > 0 ? 0.5f * (float) std::sin (2.0 * 3.14159265358979 * phase) : 0.0f; }
            e.process (in.data(), in.data(), oL.data(), oR.data(), N, p);
        }
    }
    // audio runs 0.1 s, then the library ticks (the plug-in's timer ticks 10 times a second)
    void settle (double maxSeconds = 20.0)
    {
        for (double t = 0; t < maxSeconds; t += 0.1) { run (0.1, 0.0); lib.tick(); if (! lib.busy()) { run (0.1, 0.0); lib.tick(); return; } }
    }
    void select (int layer) { p.layer = layer; run (0.15, 0.0); lib.tick(); }
    void record (int layer, double hz)
    {
        select (layer); e.recordLayer();
        for (int i = 0; i < 40; ++i) { run (0.1, hz); lib.tick(); if (e.layerStatus (layer).state != Engine::LayerRecording && i > 0) break; }
        run (0.1, 0.0); lib.tick();
    }
    std::vector<float> decoded (int layer)
    {
        const auto& m = e.layerBytes (layer, 0);
        std::vector<float> x (m.size() / 2 > 8 ? m.size() / 2 - 8 : 0);
        for (size_t i = 0; i < x.size(); ++i) x[i] = (float) (int16_t) (m[2 * i] | (m[2 * i + 1] << 8)) / 32768.0f;
        return x;
    }
};

static std::string slurp (const fs::path& p) { std::ifstream f (p, std::ios::binary); return { std::istreambuf_iterator<char> (f), {} }; }
static void spit (const fs::path& p, const std::vector<uint8_t>& b) { fs::create_directories (p.parent_path()); std::ofstream f (p, std::ios::binary | std::ios::trunc); f.write ((const char*) b.data(), (std::streamsize) b.size()); }

static fs::path freshHome (const char* name)
{
    const fs::path h = fs::temp_directory_path() / ("splib_test_" + std::to_string ((long) getpid()) + "_" + name);
    fs::remove_all (h); fs::create_directories (h);
    return h;
}

// ---- 1. save binds the layer to the file --------------------------------------------------------------------------------
static void testSave ()
{
    std::printf ("[1] save a layer: file written, layer bound\n");
    const fs::path home = freshHome ("save");
    Inst a; a.lib.setHomeFolder (home.string());
    a.record (2, kTone[2]);
    check (a.lib.info (2).unsaved, "a recorded layer with no file is flagged unsaved");
    std::string why;
    check (! a.lib.saveLayer (3, (home / "x.wav").string(), why) && ! why.empty(), "saving an empty layer is refused");
    check (a.lib.saveLayer (2, (home / "Drone A.wav").string(), why), "save accepted");
    check (! a.lib.saveLayer (2, (home / "other.wav").string(), why), "a second save of the same layer while busy is refused");
    a.settle();
    check (fs::exists (home / "Drone A.wav"), "the file exists");
    const auto info = a.lib.info (2);
    check (info.bound && info.status == Lib::Ok && ! info.unsaved, "layer 2 is bound and no longer unsaved");
    checkS (info.fileName == "Drone A.wav", "file name shown", info.fileName);
    const auto b = a.lib.bindings();
    check (b.size() == 1 && b[0].first == 2, "one binding, for layer 2", (double) b.size());
    checkS (b.size() == 1 && b[0].second.path == "Drone A.wav", "stored path is relative to the home folder", b.empty() ? "" : b[0].second.path);
    const std::string bytes = slurp (home / "Drone A.wav");
    check (b.size() == 1 && b[0].second.size == bytes.size() && b[0].second.hash == Lib::hashBytes ((const uint8_t*) bytes.data(), bytes.size()), "size and fingerprint are those of the file");
    // saving into a folder that doesn't exist yet creates it
    a.record (4, kTone[4]);
    check (a.lib.saveLayer (4, (home / "new folder" / "deeper" / "L4.wav").string(), why), "save into new folders accepted");
    a.settle();
    check (fs::exists (home / "new folder" / "deeper" / "L4.wav") && a.lib.info (4).bound, "folders created, layer bound");
    // a file outside the home folder is stored with its full path
    const fs::path other = freshHome ("save_other");
    a.record (5, kTone[5]);
    a.lib.saveLayer (5, (other / "elsewhere.wav").string(), why); a.settle();
    bool found = false; for (auto& [l, bd] : a.lib.bindings()) if (l == 5) found = fs::path (bd.path).is_absolute();
    check (found, "a file outside the home folder is stored with its full path");
    // unwritable destination: the parent is a file
    spit (home / "iamafile", { 1, 2, 3 });
    a.record (6, kTone[6]);
    a.lib.saveLayer (6, (home / "iamafile" / "x.wav").string(), why); a.settle();
    check (! a.lib.info (6).bound && a.lib.info (6).status == Lib::Failed && a.lib.info (6).unsaved, "a destination that can't be written is reported, layer stays unsaved");
    fs::remove_all (home); fs::remove_all (other);
}

// ---- 2. a preset brings the layers back -------------------------------------------------------------------------------------
static void testPresetRoundTrip ()
{
    std::printf ("[2] preset round trip: restore loads the layers from their files\n");
    const fs::path home = freshHome ("preset");
    Inst a; a.lib.setHomeFolder (home.string());
    std::string why;
    uint64_t srcHash[7] = {};
    for (int k : { 1, 3, 4 })
    {
        a.record (k, kTone[k]);
        a.lib.saveLayer (k, (home / ("L" + std::to_string (k) + ".wav")).string(), why); a.settle();
        srcHash[k] = hashOf (a.e.layerBytes (k, 0));
    }
    const auto state = a.lib.bindings();
    check (state.size() == 3, "three bindings saved in the 'preset'", (double) state.size());
    if (state.size() != 3) { fs::remove_all (home); return; }

    Inst b; b.lib.setHomeFolder (home.string());            // a second instance (another Logic track), empty
    b.lib.restore (state);
    check (b.lib.info (3).status == Lib::Waiting, "restore queued: waiting");
    b.settle();
    for (int k : { 1, 3, 4 })
    {
        check (b.e.layerStatus (k).state == Engine::LayerFilled, "layer filled from its file", k);
        check (hashOf (b.e.layerBytes (k, 0)) == srcHash[k], "identical audio to what was saved", k);
        check (b.lib.info (k).bound && b.lib.info (k).status == Lib::Ok, "and bound", k);
    }
    check (b.e.layerStatus (2).state == Engine::LayerEmpty && ! b.lib.info (2).bound, "layers without a reference stay empty");
    // the restored layer plays its tone
    b.select (3);
    b.run (0.8, 0.0);
    // restoring the same state again changes nothing (hosts do this)
    const uint32_t rev = b.e.layerRevision (3);
    b.lib.restore (state); b.settle();
    check (b.e.layerRevision (3) == rev, "restoring the same state again doesn't reload the audio");
    // a preset with fewer layers: unlisted layers keep their audio but lose the binding
    std::vector<std::pair<int, Lib::Binding>> fewer; fewer.push_back (state[0]);
    b.lib.restore (fewer); b.settle();
    check (b.e.layerStatus (4).state == Engine::LayerFilled && ! b.lib.info (4).bound && b.lib.info (4).unsaved, "an unlisted layer keeps its audio, now unsaved");
    fs::remove_all (home);
}

// ---- 3. finding files that have moved ---------------------------------------------------------------------------------------
static void testFindMoved ()
{
    std::printf ("[3] moved / renamed / missing / changed files\n");
    const fs::path home = freshHome ("find");
    Inst a; a.lib.setHomeFolder (home.string());
    std::string why;
    a.record (2, kTone[2]);
    a.lib.saveLayer (2, (home / "a.wav").string(), why); a.settle();
    const uint64_t srcHash = hashOf (a.e.layerBytes (2, 0));
    const auto state = a.lib.bindings();
    check (state.size() == 1, "one binding to start from", (double) state.size());
    if (state.size() != 1) { fs::remove_all (home); return; }

    // moved into a sub-folder and renamed: found by size + fingerprint
    fs::create_directories (home / "projects" / "autumn");
    fs::rename (home / "a.wav", home / "projects" / "autumn" / "renamed drone.wav");
    // decoys: same name as the original in other folders, different audio
    spit (home / "decoy" / "a.wav", [] { auto w = writeWav16 (makeTone (48000.0, 0.5, 1540.0)); return w; }());
    Inst b; b.lib.setHomeFolder (home.string()); b.lib.restore (state); b.settle();
    check (b.e.layerStatus (2).state == Engine::LayerFilled && hashOf (b.e.layerBytes (2, 0)) == srcHash, "found the renamed file in a sub-folder and loaded the right audio");
    const auto nb = b.lib.bindings();
    checkS (nb.size() == 1 && nb[0].second.path == "projects/autumn/renamed drone.wav", "the binding now points at the new place", nb.empty() ? "" : nb[0].second.path);
    check (b.lib.info (2).status == Lib::Ok, "status ok");

    // missing: only a decoy with the same name remains
    fs::remove (home / "projects" / "autumn" / "renamed drone.wav");
    Inst c; c.lib.setHomeFolder (home.string()); c.lib.restore (state); c.settle();
    check (c.e.layerStatus (2).state == Engine::LayerEmpty, "a same-named file with different audio is NOT loaded");
    check (c.lib.info (2).status == Lib::Missing && c.lib.info (2).message.find ("missing") == 0, "reported as missing");
    check (c.lib.bindings().size() == 1, "the reference is kept (the file may come back)");

    // changed: the file is still at its path but has different audio
    spit (home / "a.wav", writeWav16 (makeTone (48000.0, 1.0, kTone[5])));
    Inst d; d.lib.setHomeFolder (home.string()); d.lib.restore (state); d.settle();
    check (d.e.layerStatus (2).state == Engine::LayerFilled, "the changed file is loaded");
    check (d.lib.info (2).status == Lib::Changed, "and flagged as changed");
    check (toneOf (d.decoded (2), 48000.0) == 5, "with the new audio");
    const auto nd = d.lib.bindings();
    std::string bytes = slurp (home / "a.wav");
    check (nd.size() == 1 && nd[0].second.hash == Lib::hashBytes ((const uint8_t*) bytes.data(), bytes.size()), "the fingerprint was updated to the new file");

    // no home folder set and a relative path: missing with a clear message
    Inst e2; e2.lib.restore (state); e2.settle();
    check (e2.lib.info (2).status == Lib::Missing && e2.lib.info (2).message.find ("home folder") != std::string::npos, "no home folder: missing, and it says why");

    // the file is garbage (same size, different bytes)
    {
        std::string orig = slurp (home / "a.wav");
        std::vector<uint8_t> junk (orig.size(), 0x41);
        spit (home / "a.wav", junk);
        Lib::Binding bd = state[0].second; bd.size = junk.size(); bd.hash = Lib::hashBytes (junk.data(), junk.size());
        Inst g; g.lib.setHomeFolder (home.string()); g.lib.restore ({ { 2, bd } }); g.settle();
        check (g.lib.info (2).status == Lib::Failed && g.e.layerStatus (2).state == Engine::LayerEmpty, "a file that is not a WAV fails cleanly");
    }
    // absolute path outside the home folder, no home set
    const fs::path other = freshHome ("find_other");
    spit (other / "outside.wav", writeWav16 (makeTone (44100.0, 0.8, kTone[3])));
    {
        const std::string bytes2 = slurp (other / "outside.wav");
        Lib::Binding bd; bd.path = (other / "outside.wav").string(); bd.size = bytes2.size(); bd.hash = Lib::hashBytes ((const uint8_t*) bytes2.data(), bytes2.size());
        Inst h; h.lib.restore ({ { 4, bd } }); h.settle();
        check (h.e.layerStatus (4).state == Engine::LayerFilled && toneOf (h.decoded (4), 48000.0) == 3, "an absolute path works without a home folder (and a 44.1 kHz file loads)");
    }
    fs::remove_all (home); fs::remove_all (other);
}

// ---- 4. user edits drop the link; format changes reload ----------------------------------------------------------------------
static void testEditsAndFormatChange ()
{
    std::printf ("[4] recording over / clearing drops the link; a format change reloads from the files\n");
    const fs::path home = freshHome ("edits");
    Inst a; a.lib.setHomeFolder (home.string());
    std::string why;
    a.record (1, kTone[1]);
    a.lib.saveLayer (1, (home / "one.wav").string(), why); a.settle();
    a.record (2, kTone[2]);
    a.lib.saveLayer (2, (home / "two.wav").string(), why); a.settle();
    a.record (3, kTone[3]);                                        // never saved
    check (a.lib.info (1).bound && a.lib.info (2).bound && ! a.lib.info (3).bound && a.lib.info (3).unsaved, "two saved, one unsaved");

    // recording over layer 1 drops its binding
    a.record (1, kTone[4]);
    a.lib.tick(); a.run (0.2, 0.0); a.lib.tick();
    check (! a.lib.info (1).bound && a.lib.info (1).unsaved, "recording over a bound layer drops the link (layer now unsaved)");
    check (a.lib.bindings().size() == 1 && a.lib.bindings()[0].first == 2, "only layer 2 stays in the preset");
    // clearing drops it
    a.select (2); a.e.clearLayer(); a.run (0.2, 0.0); a.lib.tick(); a.run (0.1, 0.0); a.lib.tick();
    check (! a.lib.info (2).bound && a.e.layerStatus (2).state == Engine::LayerEmpty, "clearing a bound layer drops the link");
    check (a.lib.bindings().empty(), "no references left");

    // load a file into a layer: bound
    spit (home / "loaded.wav", writeWav16 (makeTone (44100.0, 0.7, kTone[6])));
    check (a.lib.loadLayer (5, (home / "loaded.wav").string(), why), "load accepted");
    a.settle();
    check (a.lib.info (5).bound && a.e.layerStatus (5).state == Engine::LayerFilled && a.lib.bindings().size() == 1, "loading a file binds the layer to it");
    check (! a.lib.loadLayer (5, (home / "nonexistent.wav").string(), why) && ! why.empty(), "a missing file is refused up front");
    spit (home / "bad.wav", std::vector<uint8_t> (100, 7));
    check (! a.lib.loadLayer (5, (home / "bad.wav").string(), why) && ! why.empty(), "a file that isn't a WAV is refused with a message");

    // format change: bound layers come back from their files, unbound ones are lost
    a.lib.restore ({}); a.lib.unbind (5);
    Inst f; f.lib.setHomeFolder (home.string());
    f.record (1, kTone[1]); f.lib.saveLayer (1, (home / "f1.wav").string(), why); f.settle();
    f.record (2, kTone[2]);                                         // unsaved
    f.record (3, kTone[3]); f.lib.saveLayer (3, (home / "f3.wav").string(), why); f.settle();
    f.p.quality = 1;                                                // stereo -> mono: all layers emptied by the engine
    f.settle (30.0); f.run (0.5, 0.0); f.lib.tick(); f.settle();
    check (f.e.layerStatus (1).state == Engine::LayerFilled && f.e.layerStatus (3).state == Engine::LayerFilled, "bound layers were reloaded in the new format");
    check (f.e.layerStatus (2).state == Engine::LayerEmpty, "the unsaved layer was lost with the format change");
    check (f.lib.info (1).bound && f.lib.info (3).bound && f.lib.info (1).status == Lib::Ok, "still bound");
    check (f.e.layerBytes (1, 1).empty(), "layer 1 is now a mono layer");
    {
        std::vector<float> x; const auto& m = f.e.layerBytes (1, 0);
        for (size_t i = 0; i + 1 < m.size() && x.size() < m.size() / 2 - 8; i += 2) x.push_back ((float) (int16_t) (m[i] | (m[i + 1] << 8)) / 32768.0f);
        check (toneOf (x, 48000.0) == 1, "with its own audio");
    }
    f.p.bufferLength = 1; f.p.quality = 2;                          // 4 s buffer, low-fi stereo
    f.settle (30.0); f.run (0.5, 0.0); f.lib.tick(); f.settle();
    check (f.e.layerStatus (1).state == Engine::LayerFilled && f.e.layerStatus (3).state == Engine::LayerFilled, "reloaded again after a second format change (low-fi, longer buffer)");
    check (f.e.layerStatus (1).seconds > 0.9 && f.e.layerStatus (1).seconds < 1.1, "a 1 s file is a 1 s layer in the longer buffer", f.e.layerStatus (1).seconds);
    // a bound file that vanished before the format change: missing, no loop
    fs::remove (home / "f3.wav");
    f.p.quality = 0; f.settle (30.0); f.run (0.5, 0.0); f.lib.tick(); f.settle();
    check (f.e.layerStatus (1).state == Engine::LayerFilled, "layer 1 reloaded");
    check (f.e.layerStatus (3).state == Engine::LayerEmpty && f.lib.info (3).status == Lib::Missing, "layer 3's file is gone: empty and reported missing");
    check (! f.lib.busy(), "nothing left retrying");
    fs::remove_all (home);
}

// ---- 5. timing: the engine not ready, audio not running --------------------------------------------------------------------
static void testNotReady ()
{
    std::printf ("[5] restore before the audio is running; save while the audio is stopped\n");
    const fs::path home = freshHome ("ready");
    spit (home / "w.wav", writeWav16 (makeTone (48000.0, 0.8, kTone[2])));
    const std::string bytes = slurp (home / "w.wav");
    Lib::Binding bd; bd.path = "w.wav"; bd.size = bytes.size(); bd.hash = Lib::hashBytes ((const uint8_t*) bytes.data(), bytes.size());
    {
        Inst a (false);                                              // engine not prepared yet (the host sets the state before starting audio)
        a.lib.setHomeFolder (home.string());
        a.lib.restore ({ { 2, bd } });
        for (int i = 0; i < 20; ++i) a.lib.tick();
        check (a.lib.info (2).status == Lib::Waiting && a.lib.busy(), "waits while the engine isn't ready");
        a.e.prepare (48000.0); a.run (0.3, 0.0);
        a.settle();
        check (a.e.layerStatus (2).state == Engine::LayerFilled && a.lib.info (2).status == Lib::Ok, "loads as soon as the audio runs");
    }
    {
        Inst a; a.lib.setHomeFolder (home.string());
        a.record (3, kTone[3]);
        std::string why; a.lib.saveLayer (3, (home / "stopped.wav").string(), why);
        for (int i = 0; i < 40; ++i) a.lib.tick();                   // no audio is running
        check (a.lib.busy() && ! fs::exists (home / "stopped.wav"), "a save waits while no audio runs");
        check (a.lib.info (3).message.find ("waiting for audio") != std::string::npos, "and says so", 0);
        a.settle();
        check (fs::exists (home / "stopped.wav") && a.lib.info (3).bound, "completes when the audio runs");
    }
    {   // four saves at once
        Inst a; a.lib.setHomeFolder (home.string());
        std::string why;
        for (int k = 1; k <= 4; ++k) a.record (k, kTone[k]);
        for (int k = 1; k <= 4; ++k) check (a.lib.saveLayer (k, (home / ("q" + std::to_string (k) + ".wav")).string(), why), "queued", k);
        a.settle();
        int ok = 0; for (int k = 1; k <= 4; ++k) ok += a.lib.info (k).bound && fs::exists (home / ("q" + std::to_string (k) + ".wav"));
        check (ok == 4 && a.lib.bindings().size() == 4, "four queued saves all complete", ok);
        std::set<uint64_t> hs; for (int k = 1; k <= 4; ++k) { std::string b = slurp (home / ("q" + std::to_string (k) + ".wav")); hs.insert (Lib::hashBytes ((const uint8_t*) b.data(), b.size())); }
        check (hs.size() == 4, "each file holds a different layer");
    }
    // suggested names
    Inst s; const std::string n = s.lib.suggestedFileName (3);
    check (n.rfind ("Layer 3 ", 0) == 0 && n.size() > 8 && n.substr (n.size() - 4) == ".wav", "suggested file name looks right");
    fs::remove_all (home);
}

int main()
{
    testSave();
    testPresetRoundTrip();
    testFindMoved();
    testEditsAndFormatChange();
    testNotReady();
    std::printf ("\nlibrary_test: %d checks, %d failed\n", gChecks, gFailed);
    return gFailed == 0 ? 0 : 1;
}
