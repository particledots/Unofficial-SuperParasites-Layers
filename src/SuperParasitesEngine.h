// JUCE-free wrapper around the SuperParasites DSP (MIT licensed: Emilie Gillet's Clouds, Matthias Puech's Parasites,
// Julius Kammerl's Beat Repeat / Spectral Clouds, merged by Patrick Dowling for Supercell/Microcell hardware).
// Unofficial SuperParasites Layers = HD plus six stored layers (see the "Layers" section below). With Params::layer = 0 (Live)
// and no layer calls the output is the HD plug-in's, sample for sample.
// Unofficial SuperParasites HD: the DSP can run at 32 kHz (the original; 16 kHz in the low-fidelity qualities) in 32-frame
// blocks, or natively at 48 kHz (still 16 kHz in the low-fidelity qualities) in 48-frame blocks, both 1 ms, with 16-bit
// samples; this wrapper converts to and from the host rate (and does not resample at all when the host runs at the engine rate).
#pragma once
#include <atomic>
#include <memory>
#include <vector>
#include <cstdint>
#include "Resampler.h"
#include "LayerAudio.h"
#include "WaveformPicture.h"

namespace clouds { class GranularProcessor; }

class SuperParasitesEngine
{
public:
    // The module's eight modes, in the order its mode button steps through them.
    enum Mode { Granular = 0, PitchShifterStretch, LoopingDelay, Spectral, Oliverb, Resonestor, BeatRepeat, SpectralClouds, NumModes };

    struct Params
    {
        // Same meaning as the module's knobs (all 0..1 unless noted).
        float position = 0.5f, size = 0.5f, density = 0.7f, texture = 0.5f;
        float pitchPot = 0.5f;                               // the PITCH knob position, 0..1 (the module maps it through a detented curve)
        float pitchOffsetSemitones = 0.0f;                   // added on top, like the V/OCT input (used for MIDI follow)
        int pitchSnap = 0;                                   // our addition: 0 Off, 1 Steps (octaves and fifths), 2 Semitones - snaps the PITCH knob's value
        // The module's four BLEND roles, each as its own value:
        float dryWet = 0.5f, stereoSpread = 0.5f, feedback = 0.0f, reverb = 0.0f;
        bool freeze = false;
        bool reverse = false;    // granular reverse (the module's reverse toggle)
        bool gate = false;       // the module's gate input; in Spectral madness it glitches the spectrum
        int mode = Granular;
        int quality = 0;     // 0 stereo 16-bit, 1 mono 16-bit, 2 stereo 8-bit low-fi, 3 mono 8-bit low-fi (the module's order)
        int bufferLength = 0; // index into bufferSeconds(): how much sample memory the recording buffer gets (Extended)
        int engineRate = 0;   // 0 = 32 kHz (the original), 1 = 48 kHz (HD)
        int layer = 0;        // 0 = Live (the always-recording buffer, as in HD); 1..kNumLayers = a stored layer
        int layerB = 0;              // a second layer played together with the first (0 = none, 1..kNumLayers); needs a layer-playing mode, and is ignored if it is the same as `layer`
        float layerBPan = 0.0f;      // balance of the second layer: -1 left .. 0 centre (unchanged) .. +1 right
        float layerBLevel = 0.8f;    // its level, 0..1
        int switchStyle = 0;         // with Switch Time above 0: 0 = Dip (fade out, swap, fade in), 1 = Crossfade (the new layer is started on a second engine and the two are cross-faded)
        float switchTimeMs = 0.0f;   // layer switching: 0 = the short instant swap (24 ms mute); above 0 = fade out then fade in over this many ms in total
    };

    // Recording-buffer sizes offered by the Extended build. Index 0 is the module's own memory (about 1 second per channel at
    // 16-bit); the others are longer. The low-fi qualities hold 4x as long (8-bit at 16 kHz), mono holds a little over 2x as long
    // as stereo, exactly as on the module. The numbers are the stereo 16-bit length per channel in seconds.
    static constexpr int kNumBufferLengths = 4;
    static float bufferSeconds (int index);

    SuperParasitesEngine();
    ~SuperParasitesEngine();

    // ---- Layers --------------------------------------------------------------------------------------------------
    // Six stored recordings that outlive the processor's own buffer (which Spectral madness, Spectral clouds, Resonestor and
    // Oliverb rebuild). One plays at a time: Params::layer picks it (0 = Live). A layer plays like the module's frozen buffer
    // (the recording is held still), in the modes that play recorded audio: Granular, Pitch shifter / time stretch,
    // Looping delay and Beat repeat. In the other four modes the selected layer is kept safe and the engine works on the live
    // input as in HD. Layers are cleared when the quality, Buffer Length or Engine Rate changes (they are stored in that format).
    static constexpr int kNumLayers = 6;
    enum LayerState { LayerEmpty = 0, LayerRecording = 1, LayerFilled = 2 };
    struct LayerStatus { int state = LayerEmpty; float seconds = 0.0f; };

    // Toggle recording into the selected layer: the first call clears the layer and records from its start until it is full
    // (then it stops by itself); calling it while recording stops and keeps what was recorded. Ignored in Live and in
    // the modes that don't play recorded audio. Safe to call from the UI thread; acted on in the next process() call.
    void recordLayer() { recordRequested.store (true); }
    // Empty the selected layer (stops its recording too). Same threading.
    void clearLayer() { clearRequested.store (true); }
    LayerStatus layerStatus (int layer) const;   // layer is 1..kNumLayers
    // Change counters (for tying a layer to a file). The revision goes up whenever the audio in a layer changes for any reason (a recording
    // ends, a load, a clear, a format change emptying it); the edit count only when the USER changed it (clear, start of a recording).
    // So "revision changed, edit count not" on an empty layer means it was emptied by a Quality / Buffer Length / Engine Rate change.
    uint32_t layerRevision (int layer) const { return uiRev[layer - 1].load(); }
    uint32_t layerEditCount (int layer) const { return uiEdit[layer - 1].load(); }
    int loadedLayer() const { return uiLoaded.load(); }   // which layer's audio the engine is playing (0 = Live)
    static constexpr float kMaxSwitchMs = 2000.0f;
    static constexpr int kCrossfadePrerollMs = 200;   // a Crossfade starts the incoming layer this long before the fade, so it is already playing
    bool crossfading() const { return xfState != 0; }  // audio thread / tests

    // ---- Saving and loading a layer's audio as WAV (UI thread; the audio thread does the hand-over, so nothing here blocks it) ----
    // The format layers are stored in right now (depends on Quality, Buffer Length and Engine Rate).
    struct LayerFormat { bool valid = false; int channels = 2; bool lowFi = false; double sampleRate = 48000.0; int capFrames = 0; };
    LayerFormat layerFormat() const;
    enum IoResult { IoNone = 0, ExportReady, ExportEmpty, ImportOk, ImportRejected };
    // Save: ask for a copy of a filled layer; the audio thread supplies it within a few ms of running. Then poll until it is ready.
    bool requestExport (int layer);                  // false if a request is already in progress
    IoResult pollExport (LayerAudio& out);           // IoNone while pending; ExportReady fills `out` (at the layer's own sample rate)
    // After ExportReady: the revision / edit count the exported snapshot had (to tie the file to exactly that audio).
    uint32_t exportedRevision() const { return exportRev; }
    uint32_t exportedEditCount() const { return exportEdit; }
    // Load: convert the audio to the layer format (resampling, mono/stereo, 16-bit or 8-bit low-fi), truncated to the layer length,
    // and hand it to the audio thread. Returns false (with a reason) if it can't be queued right now. Then poll for the outcome.
    bool submitImport (int layer, const LayerAudio& audio, std::string& whyNot);
    IoResult pollImport();                           // IoNone while pending; ImportOk / ImportRejected when done (once)
    static bool modeUsesLayers (int mode);
    // For tests: the stored bytes of a layer's channel, and the bytes all layers hold at the moment.
    const std::vector<uint8_t>& layerBytes (int layer, int channel) const { return layers[layer - 1].mem[channel]; }
    size_t layerStoredBytes() const;

    // ---- Waveform display ------------------------------------------------------------------------------------------
    // A coarse picture of the recording buffer with the Position / Size markers (see WaveformPicture.h). It is only kept up to
    // date while at least one editor has it open; the audio thread never waits for the editor, and the picture is read-only for
    // the sound: with it on or off the output is the same sample for sample.
    const WaveformPicture& waveform() const { return picture; }
    void waveformViewerOpened() { viewers.fetch_add (1, std::memory_order_relaxed); }
    void waveformViewerClosed() { viewers.fetch_sub (1, std::memory_order_relaxed); }

    void prepare (double hostSampleRate);

    // Press the module's CAPTURE button / fire its capture input once (seeds a grain, taps tempo, triggers a burst...).
    void capture() { capturePending = true; }

    void process (const float* inL, const float* inR, float* outL, float* outR, int numSamples, const Params& p);

    int latencyInHostSamples() const;
    double processorRate() const { return procRate; }
    static double engineRateHz (int engineRate) { return engineRate == 0 ? 32000.0 : 48000.0; }
    size_t blockSize() const { return kBlock; }

    // The PITCH knob's detented curve: knob position 0..1 -> semitones (-24..+24), as in the module.
    static float pitchPotToSemitones (float pot);

    // Our addition: Pitch Snap applied to the knob's semitone value (before the MIDI note is added).
    //   0 Off       - unchanged (the module's own detented curve)
    //   1 Steps     - -24, -12, -7, 0, +7, +12, +24 (octaves and fifths)
    //   2 Semitones - whole semitones
    enum PitchSnap { SnapOff = 0, SnapSteps = 1, SnapSemitones = 2 };
    static float snapPitch (float semitones, int mode);

private:
    struct Frame { float l, r; };

    void runBlocks (const Params& p);
    void rebuildProcessor (int bufferLength, int engineRate);   // fresh processor with the chosen sample memory and engine rate
    void setEngineRate (int engineRate);       // switches the rate: new resamplers, empty queues

    // ---- layer internals
    struct Layer
    {
        bool filled = false;
        int32_t head[2] = { 0, 0 };      // write head of each channel when the recording ended (playback is relative to it)
        float seconds = 0.0f;
        int32_t frames = 0;              // length in samples per channel
        std::vector<uint8_t> mem[2];     // raw copy of the processor's sample memory (16-bit, or low-fi 8-bit)
    };
    Layer layers[kNumLayers];
    std::atomic<bool> recordRequested { false }, clearRequested { false };
    std::atomic<int> uiState[kNumLayers];
    std::atomic<float> uiSeconds[kNumLayers];
    std::atomic<uint32_t> uiRev[kNumLayers], uiEdit[kNumLayers];
    uint32_t exportRev = 0, exportEdit = 0;
    void bump (int layer, bool user) { uiRev[layer - 1].fetch_add (1); if (user) uiEdit[layer - 1].fetch_add (1); }
    std::atomic<int> uiLoaded { 0 };
    int loadedLayerIdx = 0;              // layer whose audio is in the processor's sample memory (0 = Live)
    int recLayer = 0;
    bool recording = false, switching = false, freezeWasForced = false, forcedFreeze = false;
    int fadeLeft = 0;
    // soft switch (Switch Time > 0): own gain ramp on the output, fade out -> swap -> fade in
    bool softFade = false, fadingIn = false, reloadNeeded = false;
    float fadePos = 1.0f, fadeStep = 0.0f, gainPrev = 1.0f, gainNow = 1.0f;
    // Crossfade: a second processor (procB) plays the incoming layer; states 0 idle, 1 pre-roll (B runs, output still A), 2 fading
    std::unique_ptr<clouds::GranularProcessor> procB;
    std::vector<uint64_t> largeBufB, smallBufB;
    int xfState = 0, xfTarget = 0, xfPrerollLeft = 0, xfFadeBlocks = 1, xfFadeDone = 0;
    bool xfLoaded = false; int32_t xfGeneration = 0;
    struct XfFrame { short l, r; };
    std::vector<XfFrame> xfOut;           // same layout as the processor's ShortFrame
    void xfStart (const Params& p, int target);
    void xfAbort();
    void xfStartDual (int layer);
    void xfFinish (const Params& p);
    void xfLoadIfReady();
    // Second layer together with the first: procB plays it, frozen, and its output is added to the main output.
    int dualLayer = 0, dualWant = 0; float dualGain = 0.0f, dualLevel = 0.8f, dualPan = 0.0f, dualPanS = 0.0f; bool dualRestart = false;
    void resetFade() { xfAbort(); dualLayer = 0; dualGain = 0.0f; dualRestart = false; switching = false; softFade = false; fadingIn = false; reloadNeeded = false; fadePos = 1.0f; fadeStep = 0.0f; gainPrev = gainNow = 1.0f; }
    // format of the stored layers, published for the UI thread
    std::atomic<bool> fmtValid { false }, fmtLowFi { false };
    std::atomic<int> fmtChannels { 2 }, fmtCap { 0 };
    std::atomic<double> fmtRate { 48000.0 };
    std::atomic<int64_t> fmtBytes0 { 0 }, fmtBytes1 { 0 };
    // hand-over slots (UI thread <-> audio thread)
    std::atomic<int> exportState { 0 }, importState { 0 }, importOutcome { IoNone }, exportLayerReq { 0 };
    int exportFrames = 0; double exportRate = 0.0; bool exportLowFi = false;
    std::vector<uint8_t> exportMem[2];
    int importLayerReq = 0; int32_t importFrames = 0; int64_t importBytes0 = 0, importBytes1 = 0; bool importLowFi = false;
    std::vector<uint8_t> importMem[2];
    void publishFormat();
    void serviceTransfers();                       // audio thread: fulfils an export request / applies a staged import
    int32_t lastGeneration = 0;
    int keyQuality = -1, keyEngineRate = -1, keyBufferLength = -1;
    void layersBeforeBlock (const Params& p);      // requests, layer switching, hold/freeze flags (once per 1 ms block)
    void layersAfterProcess();                     // notices a finished recording
    void layersAfterPrepare (const Params& p);     // notices that the processor rebuilt its memory and puts the layer back
    void loadLayerIntoProcessor (int layer) { loadLayerInto (*proc, layer); }
    void loadLayerInto (clouds::GranularProcessor& pr, int layer);       // copy a stored layer in (an empty layer or Live = silence)
    void finishRecording();
    void discardRecording();
    void invalidateAllLayers();
    void publishLayers();

    // waveform display (audio thread writes the picture; see updateWaveform())
    WaveformPicture picture;
    std::atomic<int> viewers { 0 };
    int scanCursor = 0, scanRing = -1, scanSinceHeld = 0, scanIdleTick = 0;
    int32_t scanGeneration = -1;
    bool scanLowFi = false;
    const void* scanPtr = nullptr;
    void updateWaveform();

    std::unique_ptr<clouds::GranularProcessor> proc;
    std::vector<uint64_t> largeBuf, smallBuf;   // the module's 118784-byte main block and 65408-byte CCM block

    double hostRate = 48000.0, procRate = 32000.0;
    size_t kBlock = 32;                         // frames per block: 1 ms (32 at 32 kHz, 48 at 48 kHz)
    int currentEngineRate = 0, lastEngineRateSeen = 0;

    // smoothed copies of the knob values (the firmware low-passes each knob; same coefficients)
    float sPosition = 0.5f, sSize = 0.5f, sDensity = 0.7f, sTexture = 0.5f, sPitchPot = 0.5f, sNote = 0.0f, sSnapped = 0.0f;
    float sDryWet = 0.5f, sSpread = 0.5f, sFeedback = 0.0f, sReverb = 0.0f;
    bool lastFreeze = false, capturePending = false, smoothInit = false;
    int currentBufferLength = -1;

    SincResampler rsInL, rsInR, rsOutL, rsOutR;
    std::vector<float> tmpL, tmpR;
    std::vector<float> hostQL, hostQR;
    size_t hostQHead = 0;
    std::vector<Frame> inQueue;
    size_t inQueueHead = 0;
    std::vector<float> procOutL, procOutR;
};
