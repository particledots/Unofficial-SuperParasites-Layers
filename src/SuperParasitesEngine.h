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
    int loadedLayer() const { return uiLoaded.load(); }   // which layer's audio the engine is playing (0 = Live)
    static bool modeUsesLayers (int mode);
    // For tests: the stored bytes of a layer's channel, and the bytes all layers hold at the moment.
    const std::vector<uint8_t>& layerBytes (int layer, int channel) const { return layers[layer - 1].mem[channel]; }
    size_t layerStoredBytes() const;

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
        std::vector<uint8_t> mem[2];     // raw copy of the processor's sample memory (16-bit, or low-fi 8-bit)
    };
    Layer layers[kNumLayers];
    std::atomic<bool> recordRequested { false }, clearRequested { false };
    std::atomic<int> uiState[kNumLayers];
    std::atomic<float> uiSeconds[kNumLayers];
    std::atomic<int> uiLoaded { 0 };
    int loadedLayerIdx = 0;              // layer whose audio is in the processor's sample memory (0 = Live)
    int recLayer = 0;
    bool recording = false, switching = false, freezeWasForced = false, forcedFreeze = false;
    int fadeLeft = 0;
    int32_t lastGeneration = 0;
    int keyQuality = -1, keyEngineRate = -1, keyBufferLength = -1;
    void layersBeforeBlock (const Params& p);      // requests, layer switching, hold/freeze flags (once per 1 ms block)
    void layersAfterProcess();                     // notices a finished recording
    void layersAfterPrepare (const Params& p);     // notices that the processor rebuilt its memory and puts the layer back
    void loadLayerIntoProcessor (int layer);       // copy a stored layer in (an empty layer or Live = silence)
    void finishRecording();
    void discardRecording();
    void invalidateAllLayers();
    void publishLayers();

    std::unique_ptr<clouds::GranularProcessor> proc;
    std::vector<uint64_t> largeBuf, smallBuf;   // the module's 118784-byte main block and 65408-byte CCM block

    double hostRate = 48000.0, procRate = 32000.0;
    size_t kBlock = 32;                         // frames per block: 1 ms (32 at 32 kHz, 48 at 48 kHz)
    int currentEngineRate = 0, lastEngineRateSeen = 0;

    // smoothed copies of the knob values (the firmware low-passes each knob; same coefficients)
    float sPosition = 0.5f, sSize = 0.5f, sDensity = 0.7f, sTexture = 0.5f, sPitchPot = 0.5f, sNote = 0.0f;
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
