#include "SuperParasitesEngine.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <new>
#include "supercell/dsp/granular_processor.h"
#include "supercell/resources.h"
#include "stmlib/dsp/dsp.h"

namespace
{
    constexpr int kPrepareCallsPerBlock = 16;   // the firmware calls Prepare() in a tight main loop between blocks

    inline short toShort (float x)
    {
        return (short) std::lrintf (std::clamp (x, -1.0f, 1.0f) * 32767.0f);
    }
    inline void onePole (float& state, float target, float coefficient) { state += coefficient * (target - state); }

    // The firmware's processor lives in global memory, which the chip zeroes at start-up, and parts of the code
    // (for example the silence flag) are never initialised by the code itself. So hand it zeroed memory too.
    template <class T> T* newZeroed()
    {
        void* m = ::operator new (sizeof (T));
        std::memset (m, 0, sizeof (T));
        return new (m) T();
    }
}

namespace
{
    // The module's memory: a 65408-byte block (the CCM) and a 118784-byte block. In stereo each channel gets the small block
    // and the rest of the large block is the effects' workspace; in mono the large block is the sample memory and the small one
    // is the workspace. So the workspace (118784 - 65408 bytes) must stay, and the sample memory per channel can grow.
    constexpr size_t kModuleSmall = 65536 - 128;
    constexpr size_t kModuleLarge = 118784;
    constexpr size_t kWorkspace   = kModuleLarge - kModuleSmall;
    const float kSeconds[SuperParasitesEngine::kNumBufferLengths] = { 1.0f, 4.0f, 10.0f, 20.0f };

    size_t smallBytesFor (int index)
    {
        if (index <= 0) return kModuleSmall;                              // exactly the module's memory
        const double bytes = (double) kSeconds[index] * 32000.0 * 2.0;    // 16-bit at 32 kHz, per channel
        return ((size_t) bytes + 7) / 8 * 8;
    }

    // Memory for the 48 kHz engine. Recording memory is 1.5x the 32 kHz amounts, so that Buffer Length means the same number
    // of seconds. The effects need more workspace at 48 kHz (their delay lines are 1.5x longer) than 1.5x the module's, so the
    // blocks are made large enough for that, and the processor is told how much of it may be used for recording.
    constexpr size_t kWorkspaceHd = clouds::GranularProcessor::kMinWorkspaceHd + 256;
    size_t round8 (double v) { return ((size_t) v + 7) / 8 * 8; }
    size_t stereoCapHd (int index) { return round8 (1.5 * (double) smallBytesFor (index)); }
    size_t monoCapHd   (int index) { return round8 (1.5 * (double) (smallBytesFor (index) + kWorkspace)); }
    size_t smallBytesHd (int index) { return std::max (stereoCapHd (index), kWorkspaceHd); }   // in mono this block is the workspace
    size_t largeBytesHd (int index) { return smallBytesHd (index) + kWorkspaceHd; }
}

float SuperParasitesEngine::bufferSeconds (int index)
{
    return kSeconds[std::clamp (index, 0, kNumBufferLengths - 1)];
}

SuperParasitesEngine::SuperParasitesEngine()
    : largeBuf (std::max (smallBytesFor (kNumBufferLengths - 1) + kWorkspace, largeBytesHd (kNumBufferLengths - 1)) / 8, 0),
      smallBuf (std::max (smallBytesFor (kNumBufferLengths - 1), smallBytesHd (kNumBufferLengths - 1)) / 8, 0)
{
    // Layer storage: address space for the largest possible recording memory of each channel is reserved up front (pages are
    // only used when a layer is recorded), so recording a layer never has to allocate while audio is running.
    const size_t most = std::max ({ stereoCapHd (kNumBufferLengths - 1), monoCapHd (kNumBufferLengths - 1),
                                    smallBytesFor (kNumBufferLengths - 1) + kWorkspace }) + 64;
    for (auto& l : layers) for (auto& m : l.mem) m.reserve (most);
    for (int i = 0; i < kNumLayers; ++i) { uiState[i].store (LayerEmpty); uiSeconds[i].store (0.0f); }
}

void SuperParasitesEngine::rebuildProcessor (int bufferLength, int engineRate)
{
    bufferLength = std::clamp (bufferLength, 0, kNumBufferLengths - 1);
    const bool hd = engineRate != 0;
    const size_t small = hd ? smallBytesHd (bufferLength) : smallBytesFor (bufferLength);
    const size_t large = hd ? largeBytesHd (bufferLength) : small + kWorkspace;
    std::fill (largeBuf.begin(), largeBuf.begin() + (long) (large / 8), 0);
    std::fill (smallBuf.begin(), smallBuf.begin() + (long) (small / 8), 0);
    proc.reset (newZeroed<clouds::GranularProcessor>());   // fresh, zeroed state
    proc->Init (largeBuf.data(), large, smallBuf.data(), small, clouds::EngineRate (hd),
                hd ? stereoCapHd (bufferLength) : 0, hd ? monoCapHd (bufferLength) : 0);
    *proc->mutable_parameters() = clouds::Parameters();
    proc->Prepare();   // allocate the working buffers (the module does this in its main loop)
    currentBufferLength = bufferLength;
    currentEngineRate = engineRate;
    lastFreeze = false;
    // Layers: a new processor starts with silent memory, so no layer is loaded and nothing is recording. (The stored layers stay.)
    loadedLayerIdx = 0; recording = false; recLayer = 0; switching = false; freezeWasForced = false; forcedFreeze = false;
    lastGeneration = proc->buffers_generation();
    publishLayers();
}

void SuperParasitesEngine::setEngineRate (int engineRate)
{
    procRate = engineRateHz (engineRate);
    kBlock = engineRate == 0 ? 32 : 48;
    lastEngineRateSeen = engineRate;
    rsInL.init (hostRate, procRate);  rsInR.init (hostRate, procRate);
    rsOutL.init (procRate, hostRate); rsOutR.init (procRate, hostRate);
    inQueue.clear(); inQueueHead = 0;
    hostQL.clear(); hostQR.clear(); hostQHead = 0;
    const size_t prime = kBlock * 2 + SincResampler::kTaps;
    procOutL.assign (prime, 0.0f); procOutR.assign (prime, 0.0f);
    smoothInit = false; lastFreeze = false; capturePending = false;
}

SuperParasitesEngine::~SuperParasitesEngine() = default;

float SuperParasitesEngine::pitchPotToSemitones (float pot)
{
    return stmlib::Interpolate (clouds::lut_quantized_pitch, std::clamp (pot, 0.0f, 1.0f), 1024.0f);
}

void SuperParasitesEngine::prepare (double sr)
{
    hostRate = sr;
    setEngineRate (0);          // starts at the original 32 kHz; the first process() call switches to the chosen rate
    rebuildProcessor (0, 0);
    currentBufferLength = -1;   // the first runBlocks() builds it at the chosen length
}

int SuperParasitesEngine::latencyInHostSamples() const
{
    const double procFrames = (double) kBlock * 2 + SincResampler::kTaps;
    if (std::abs (hostRate - procRate) < 1e-6) return (int) procFrames;
    return (int) std::ceil (procFrames * hostRate / procRate);
}

void SuperParasitesEngine::runBlocks (const Params& p)
{
    using namespace clouds;
    // Layers are stored in the current format, so changing quality, buffer length or engine rate empties them.
    if (p.quality != keyQuality || p.bufferLength != keyBufferLength || p.engineRate != keyEngineRate)
    {
        invalidateAllLayers();
        keyQuality = p.quality; keyBufferLength = p.bufferLength; keyEngineRate = p.engineRate;
    }
    // A different buffer length rebuilds the processor (and so clears the recorded audio, like a quality change does).
    if (p.bufferLength != currentBufferLength || p.engineRate != currentEngineRate)
        rebuildProcessor (p.bufferLength, p.engineRate);
    const size_t B = kBlock;

    // Mode and quality: set through the same calls the module's UI uses. The processor mutes itself while it rebuilds.
    proc->set_playback_mode ((PlaybackMode) std::clamp (p.mode, 0, (int) PLAYBACK_MODE_LAST - 1));
    proc->set_quality (std::clamp (p.quality, 0, 3));

    std::vector<ShortFrame> in (B), out (B);
    while (inQueue.size() - inQueueHead >= B)
    {
        for (size_t i = 0; i < B; ++i)
        {
            const Frame& f = inQueue[inQueueHead + i];
            in[i].l = toShort (f.l);
            in[i].r = toShort (f.r);
        }
        inQueueHead += B;

        // Per-block knob smoothing with the firmware's own coefficients (supercell/cv_scaler.cc).
        if (! smoothInit)
        {
            sPosition = p.position; sSize = p.size; sDensity = p.density; sTexture = p.texture; sPitchPot = p.pitchPot;
            sNote = p.pitchOffsetSemitones;
            sDryWet = p.dryWet; sSpread = p.stereoSpread; sFeedback = p.feedback; sReverb = p.reverb;
            smoothInit = true;
        }
        onePole (sPosition, p.position, 0.05f);
        onePole (sDensity,  p.density,  0.01f);
        onePole (sSize,     p.size,     0.01f);
        onePole (sTexture,  p.texture,  0.01f);
        onePole (sPitchPot, p.pitchPot, 0.01f);
        sDryWet = p.dryWet;                                  // the BLEND pot is not smoothed on the module (the processor interpolates)
        onePole (sSpread,   p.stereoSpread, 0.05f);
        onePole (sFeedback, p.feedback, 0.05f);
        onePole (sReverb,   p.reverb,   0.05f);
        if (std::fabs (p.pitchOffsetSemitones - sNote) > 0.5f) sNote = p.pitchOffsetSemitones;   // big steps jump, small ones glide
        else onePole (sNote, p.pitchOffsetSemitones, 0.2f);

        Parameters* prm = proc->mutable_parameters();
        prm->position = std::clamp (sPosition, 0.0f, 1.0f);
        prm->texture  = std::clamp (sTexture, 0.0f, 1.0f);
        prm->density  = std::clamp (sDensity, 0.0f, 1.0f);
        prm->size     = std::clamp (sSize, 0.0f, 1.0f);
        const float dryWet = std::clamp (sDryWet * 1.05f - 0.025f, 0.0f, 1.0f);   // the module's end-stop trim
        prm->dry_wet  = dryWet;
        prm->reverb   = std::clamp (sReverb, 0.0f, 1.0f);
        prm->feedback = std::clamp (sFeedback, 0.0f, 1.0f);
        prm->stereo_spread = std::clamp (sSpread, 0.0f, 1.0f);
        prm->pitch    = std::clamp (pitchPotToSemitones (sPitchPot) + sNote, -48.0f, 48.0f);

        // The Beat Repeat / Spectral Clouds parameters, filled in the way cv_scaler.cc does with nothing patched.
        prm->kammerl.slice_selection  = 0.0f;                              // texture CV
        prm->kammerl.slice_modulation = prm->texture;                      // texture pot
        prm->kammerl.size_modulation  = prm->density;
        prm->kammerl.probability      = dryWet;
        prm->kammerl.clock_divider    = prm->stereo_spread;
        prm->kammerl.pitch_mode       = prm->feedback;
        prm->kammerl.distortion       = prm->reverb;
        prm->kammerl.pitch            = std::clamp (sPitchPot, 0.0f, 1.0f); // pitch pot (+ V/OCT - 0.5 on the module)

        // Freeze: only written when the plug-in's switch changes, like the module's gate edges. (The processor itself
        // clears freeze when it rebuilds its buffers after a mode/quality change, as on the module.)
        // Layers: requests, switching, and (while a layer plays) holding the recording still and playing it as frozen audio.
        layersBeforeBlock (p);
        if (forcedFreeze) { prm->freeze = true; freezeWasForced = true; }
        else if (freezeWasForced) { prm->freeze = p.freeze; lastFreeze = p.freeze; freezeWasForced = false; }
        else if (p.freeze != lastFreeze) { prm->freeze = p.freeze; lastFreeze = p.freeze; }

        prm->granular.reverse = p.reverse;
        prm->capture = capturePending;
        capturePending = false;
        prm->gate = p.gate;

        proc->Process (in.data(), out.data(), B);
        layersAfterProcess();

        for (int k = 0; k < kPrepareCallsPerBlock; ++k)
            proc->Prepare();
        layersAfterPrepare (p);

        for (size_t i = 0; i < B; ++i)
        {
            procOutL.push_back ((float) out[i].l / 32768.0f);
            procOutR.push_back ((float) out[i].r / 32768.0f);
        }
    }
    if (inQueueHead > 4096)
    {
        inQueue.erase (inQueue.begin(), inQueue.begin() + (long) inQueueHead);
        inQueueHead = 0;
    }
}

void SuperParasitesEngine::process (const float* inL, const float* inR, float* outL, float* outR, int n, const Params& p)
{
    // 0. a different engine rate: new block size, resamplers and queues (the processor itself is rebuilt in runBlocks)
    if (p.engineRate != lastEngineRateSeen)
        setEngineRate (p.engineRate);

    // 1. host rate -> engine rate
    tmpL.clear(); tmpR.clear();
    rsInL.process (inL, (size_t) n, tmpL);
    rsInR.process (inR, (size_t) n, tmpR);
    const size_t produced = std::min (tmpL.size(), tmpR.size());
    for (size_t i = 0; i < produced; ++i)
        inQueue.push_back ({ tmpL[i], tmpR[i] });

    // 2. run the module DSP
    runBlocks (p);

    // 3. 32 kHz -> host rate
    rsOutL.process (procOutL.data(), procOutL.size(), hostQL);
    rsOutR.process (procOutR.data(), procOutR.size(), hostQR);
    procOutL.clear(); procOutR.clear();

    const size_t avail = std::min (hostQL.size(), hostQR.size()) - hostQHead;
    const size_t take = std::min (avail, (size_t) n);
    for (size_t i = 0; i < take; ++i) { outL[i] = hostQL[hostQHead + i]; outR[i] = hostQR[hostQHead + i]; }
    for (size_t i = take; i < (size_t) n; ++i) { outL[i] = 0.0f; outR[i] = 0.0f; }   // underrun guard
    hostQHead += take;
    if (hostQHead > 8192)
    {
        hostQL.erase (hostQL.begin(), hostQL.begin() + (long) hostQHead);
        hostQR.erase (hostQR.begin(), hostQR.begin() + (long) hostQHead);
        hostQHead = 0;
    }
}


// ======================================================================================================================
// Layers
// ======================================================================================================================
namespace
{
    constexpr int kSwitchFadeBlocks = 24;   // 24 ms of fade-out (the processor's mute fades in about 3 ms per time constant)
}

bool SuperParasitesEngine::modeUsesLayers (int mode)
{
    return mode == Granular || mode == PitchShifterStretch || mode == LoopingDelay || mode == BeatRepeat;
}

SuperParasitesEngine::LayerStatus SuperParasitesEngine::layerStatus (int layer) const
{
    LayerStatus s;
    if (layer < 1 || layer > kNumLayers) return s;
    s.state = uiState[layer - 1].load();
    s.seconds = uiSeconds[layer - 1].load();
    return s;
}

size_t SuperParasitesEngine::layerStoredBytes() const
{
    size_t total = 0;
    for (const auto& l : layers) if (l.filled) total += l.mem[0].size() + l.mem[1].size();
    return total;
}

void SuperParasitesEngine::publishLayers()
{
    for (int i = 0; i < kNumLayers; ++i)
    {
        const bool rec = recording && recLayer == i + 1;
        uiState[i].store (rec ? LayerRecording : (layers[i].filled ? LayerFilled : LayerEmpty));
        uiSeconds[i].store (layers[i].filled ? layers[i].seconds : 0.0f);
    }
    uiLoaded.store (loadedLayerIdx);
}

void SuperParasitesEngine::invalidateAllLayers()
{
    for (auto& l : layers) { l.filled = false; l.seconds = 0.0f; l.head[0] = l.head[1] = 0; l.mem[0].clear(); l.mem[1].clear(); }
    recording = false; recLayer = 0; switching = false;
    loadedLayerIdx = 0;               // the processor is about to be rebuilt in the new format; the state machine reloads then
    if (proc) { proc->EndLayerRecording(); proc->set_mute_out (false); }
    publishLayers();
}

void SuperParasitesEngine::loadLayerIntoProcessor (int layer)
{
    if (! proc->samples_valid()) return;
    const bool wantAudio = layer >= 1 && layer <= kNumLayers && layers[layer - 1].filled;
    if (wantAudio)
    {
        const Layer& L = layers[layer - 1];
        bool fits = true;
        for (int ch = 0; ch < 2; ++ch)
        {
            void* d = nullptr; size_t bytes = 0;
            if (proc->sample_region (ch, &d, &bytes)) { if (L.mem[ch].size() != bytes) fits = false; }
        }
        if (fits)
        {
            for (int ch = 0; ch < 2; ++ch)
            {
                void* d = nullptr; size_t bytes = 0;
                if (proc->sample_region (ch, &d, &bytes)) std::memcpy (d, L.mem[ch].data(), bytes);
            }
            proc->ResyncWriteHeads (L.head[0], L.head[1]);
            return;
        }
    }
    proc->ClearSamples();   // an empty layer, Live, or (never expected) a layer that doesn't fit: silence
}

void SuperParasitesEngine::finishRecording()
{
    // Called when the pass is complete or the user stopped it: copy the recorded memory into the layer.
    Layer& L = layers[recLayer - 1];
    const int32_t frames = proc->layer_record_frames();
    proc->EndLayerRecording();
    bool ok = frames > 0;
    for (int ch = 0; ch < 2; ++ch)
    {
        void* d = nullptr; size_t bytes = 0;
        if (proc->sample_region (ch, &d, &bytes))
        {
            L.mem[ch].assign (static_cast<const uint8_t*> (d), static_cast<const uint8_t*> (d) + bytes);
            L.head[ch] = proc->write_head (ch);
        }
        else
        {
            L.mem[ch].clear();
            L.head[ch] = 0;
            if (ch == 0) ok = false;
        }
    }
    L.filled = ok;
    L.seconds = ok ? (float) ((double) frames / (double) proc->stored_sample_rate()) : 0.0f;
    recording = false;
    publishLayers();
}

void SuperParasitesEngine::discardRecording()
{
    // The recording was cut off (the memory was rebuilt, or the layer was cleared): the layer ends up empty.
    if (recLayer >= 1)
    {
        Layer& L = layers[recLayer - 1];
        L.filled = false; L.seconds = 0.0f; L.mem[0].clear(); L.mem[1].clear();
    }
    proc->EndLayerRecording();
    recording = false;
    publishLayers();
}

void SuperParasitesEngine::layersBeforeBlock (const Params& p)
{
    const int wanted = std::clamp (p.layer, 0, kNumLayers);
    const int mode = std::clamp (p.mode, 0, (int) NumModes - 1);
    const int target = (wanted > 0 && modeUsesLayers (mode)) ? wanted : 0;   // the layer that should be playing now
    const bool valid = proc->samples_valid();

    // Clear request: empties the selected layer.
    if (clearRequested.exchange (false) && wanted > 0)
    {
        Layer& L = layers[wanted - 1];
        if (recording && recLayer == wanted) discardRecording();
        L.filled = false; L.seconds = 0.0f; L.mem[0].clear(); L.mem[1].clear();
        if (loadedLayerIdx == wanted && valid) proc->ClearSamples();
        publishLayers();
    }

    // Record request: toggles recording into the layer that is playing.
    if (recordRequested.exchange (false))
    {
        if (recording)
        {
            finishRecording();                      // stopped by the user: keep what was recorded
        }
        else if (target > 0 && loadedLayerIdx == target && ! switching && valid)
        {
            proc->BeginLayerRecording();            // clears the memory, records one pass from the start
            recording = true; recLayer = target;
            layers[target - 1].filled = false; layers[target - 1].seconds = 0.0f;
            layers[target - 1].mem[0].clear(); layers[target - 1].mem[1].clear();
            publishLayers();
        }
    }

    // Switching layer (or going to / from Live): fade out, swap the memory, fade in. Only once the memory holds audio.
    if (switching)
    {
        if (--fadeLeft <= 0)
        {
            loadLayerIntoProcessor (target);
            loadedLayerIdx = target;
            switching = false;
            proc->set_mute_out (false);
            publishLayers();
        }
    }
    else if (target != loadedLayerIdx && valid)
    {
        if (recording) finishRecording();           // changing layer while recording keeps what was recorded
        switching = true;
        fadeLeft = kSwitchFadeBlocks;
        proc->set_mute_out (true);
    }

    // While a layer plays (and isn't being recorded) nothing may be written into it, and it plays as frozen audio
    // (Beat repeat keeps its own behaviour: freeze would make every slice fire).
    const bool hold = loadedLayerIdx > 0 && ! recording;
    proc->set_hold_writes (hold);
    forcedFreeze = hold && mode != BeatRepeat;
}

void SuperParasitesEngine::layersAfterProcess()
{
    if (recording && proc->layer_record_done())
        finishRecording();
}

void SuperParasitesEngine::layersAfterPrepare (const Params& p)
{
    const int32_t gen = proc->buffers_generation();
    if (gen == lastGeneration) return;
    lastGeneration = gen;
    // The processor rebuilt its sample memory (a mode change into or out of Spectral / Spectral clouds / Resonestor / Oliverb,
    // or a quality change): it is fresh silence now, and a recording in progress is cut off.
    if (recording) discardRecording();
    switching = false;
    proc->set_mute_out (false);
    loadedLayerIdx = 0;
    const int wanted = std::clamp (p.layer, 0, kNumLayers);
    const int mode = std::clamp (p.mode, 0, (int) NumModes - 1);
    if (wanted > 0 && modeUsesLayers (mode) && proc->samples_valid())
    {
        loadLayerIntoProcessor (wanted);      // freshly rebuilt processor: no sound is playing, so no fade is needed
        loadedLayerIdx = wanted;
    }
    publishLayers();
}
