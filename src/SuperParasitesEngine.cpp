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
    largeBufB.assign (largeBuf.size(), 0); smallBufB.assign (smallBuf.size(), 0);   // the Crossfade's second engine has its own memory
    procB.reset (newZeroed<clouds::GranularProcessor>());   // the Crossfade's second engine; created here so that no switch allocates
    // Layer storage: address space for the largest possible recording memory of each channel is reserved up front (pages are
    // only used when a layer is recorded), so recording a layer never has to allocate while audio is running.
    const size_t most = std::max ({ stereoCapHd (kNumBufferLengths - 1), monoCapHd (kNumBufferLengths - 1),
                                    smallBytesFor (kNumBufferLengths - 1) + kWorkspace }) + 64;
    for (auto& l : layers) for (auto& m : l.mem) m.reserve (most);
    for (auto& m : exportMem) m.reserve (most);     // hand-over copies for Save / Load, also allocated up front
    for (auto& m : importMem) m.reserve (most);
    for (int i = 0; i < kNumLayers; ++i) { uiState[i].store (LayerEmpty); uiSeconds[i].store (0.0f); uiRev[i].store (0); uiEdit[i].store (0); }
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
    loadedLayerIdx = 0; recording = false; recLayer = 0; resetFade(); freezeWasForced = false; forcedFreeze = false;
    lastGeneration = proc->buffers_generation();
    publishFormat();
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

        // Crossfade: the second engine gets the same input and knob values as the first, and plays the incoming layer frozen.
        std::vector<XfFrame>& outB = xfOut;
        if (xfState != 0)
        {
            Parameters* pb = procB->mutable_parameters();
            *pb = *prm;
            pb->freeze = (p.mode != BeatRepeat) ? true : p.freeze;
            pb->capture = false;
            procB->set_playback_mode ((PlaybackMode) std::clamp (p.mode, 0, (int) PLAYBACK_MODE_LAST - 1));
            procB->set_quality (std::clamp (p.quality, 0, 3));
            procB->set_hold_writes (true);
            xfLoadIfReady();
            outB.resize (B);
            procB->Process (in.data(), reinterpret_cast<ShortFrame*> (outB.data()), B);
            for (int k = 0; k < kPrepareCallsPerBlock; ++k) procB->Prepare();
        }

        proc->Process (in.data(), out.data(), B);
        layersAfterProcess();

        for (int k = 0; k < kPrepareCallsPerBlock; ++k)
            proc->Prepare();
        layersAfterPrepare (p);

        if (xfState == 2)
        {   // cross-fade: equal-power (the two layers are different audio, so their powers add)
            for (size_t i = 0; i < B; ++i)
            {
                const float x = ((float) xfFadeDone + (float) (i + 1) / (float) B) / (float) xfFadeBlocks;
                const float a = std::cos (1.57079632679f * x), b = std::sin (1.57079632679f * x);
                procOutL.push_back (((float) out[i].l * a + (float) outB[i].l * b) / 32768.0f);
                procOutR.push_back (((float) out[i].r * a + (float) outB[i].r * b) / 32768.0f);
            }
        }
        else if (gainPrev == 1.0f && gainNow == 1.0f)
        {
            for (size_t i = 0; i < B; ++i)
            {
                procOutL.push_back ((float) out[i].l / 32768.0f);
                procOutR.push_back ((float) out[i].r / 32768.0f);
            }
        }
        else
        {   // a soft layer switch is in progress: ramp the output gain across this block (Switch Time above 0)
            for (size_t i = 0; i < B; ++i)
            {
                const float g = gainPrev + (gainNow - gainPrev) * (float) (i + 1) / (float) B;
                procOutL.push_back ((float) out[i].l / 32768.0f * g);
                procOutR.push_back ((float) out[i].r / 32768.0f * g);
            }
        }
        if (xfState == 1) { if (--xfPrerollLeft <= 0 && xfLoaded) xfState = 2; }
        else if (xfState == 2 && ++xfFadeDone >= xfFadeBlocks) xfFinish (p);
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
    for (int i = 1; i <= kNumLayers; ++i) bump (i, false);
    for (auto& l : layers) { l.filled = false; l.seconds = 0.0f; l.frames = 0; l.head[0] = l.head[1] = 0; l.mem[0].clear(); l.mem[1].clear(); }
    recording = false; recLayer = 0; resetFade();
    fmtValid.store (false);
    loadedLayerIdx = 0;               // the processor is about to be rebuilt in the new format; the state machine reloads then
    if (proc) { proc->EndLayerRecording(); proc->set_mute_out (false); }
    publishLayers();
}

void SuperParasitesEngine::loadLayerInto (clouds::GranularProcessor& pr, int layer)
{
    if (! pr.samples_valid()) return;
    const bool wantAudio = layer >= 1 && layer <= kNumLayers && layers[layer - 1].filled;
    if (wantAudio)
    {
        const Layer& L = layers[layer - 1];
        bool fits = true;
        for (int ch = 0; ch < 2; ++ch)
        {
            void* d = nullptr; size_t bytes = 0;
            if (pr.sample_region (ch, &d, &bytes)) { if (L.mem[ch].size() != bytes) fits = false; }
        }
        if (fits)
        {
            for (int ch = 0; ch < 2; ++ch)
            {
                void* d = nullptr; size_t bytes = 0;
                if (pr.sample_region (ch, &d, &bytes)) std::memcpy (d, L.mem[ch].data(), bytes);
            }
            pr.ResyncWriteHeads (L.head[0], L.head[1]);
            return;
        }
    }
    pr.ClearSamples();   // an empty layer, Live, or (never expected) a layer that doesn't fit: silence
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
    L.frames = ok ? frames : 0;
    L.filled = ok;
    bump (recLayer, false);
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
        L.filled = false; L.seconds = 0.0f; L.frames = 0; L.mem[0].clear(); L.mem[1].clear();
        bump (recLayer, false);
    }
    proc->EndLayerRecording();
    recording = false;
    publishLayers();
}

// ---- Crossfade: the incoming layer runs on a second processor (pre-roll), then the two outputs are cross-faded ------------
void SuperParasitesEngine::xfAbort()
{
    xfState = 0; xfTarget = 0; xfPrerollLeft = 0; xfFadeDone = 0; xfLoaded = false;
}

void SuperParasitesEngine::xfStart (const Params& p, int target)
{
    const int bl = currentBufferLength, hd = currentEngineRate != 0;
    const size_t small = hd ? smallBytesHd (bl) : smallBytesFor (bl);
    const size_t large = hd ? largeBytesHd (bl) : small + kWorkspace;
    std::fill (largeBufB.begin(), largeBufB.begin() + (long) (large / 8), 0);
    std::fill (smallBufB.begin(), smallBufB.begin() + (long) (small / 8), 0);
    procB->~GranularProcessor();                        // re-create in place: fresh zeroed state, no allocation
    std::memset ((void*) procB.get(), 0, sizeof (clouds::GranularProcessor));
    new ((void*) procB.get()) clouds::GranularProcessor();
    procB->Init (largeBufB.data(), large, smallBufB.data(), small, clouds::EngineRate (hd),
                 hd ? stereoCapHd (bl) : 0, hd ? monoCapHd (bl) : 0);
    *procB->mutable_parameters() = clouds::Parameters();
    procB->Prepare();
    xfGeneration = procB->buffers_generation();
    xfLoaded = false;
    xfTarget = target;
    xfState = 1;
    xfPrerollLeft = std::max (1, (int) std::lround (kCrossfadePrerollMs * 1000.0 / 1000.0));   // blocks are 1 ms
    xfFadeBlocks = std::max (2, (int) std::lround (std::clamp (p.switchTimeMs, 0.0f, kMaxSwitchMs)));
    xfFadeDone = 0;
}

void SuperParasitesEngine::xfLoadIfReady()
{
    const int32_t gen = procB->buffers_generation();
    if (gen != xfGeneration) { xfGeneration = gen; xfLoaded = false; }   // B rebuilt its memory (mode / quality settled)
    if (! xfLoaded && procB->samples_valid())
    {
        loadLayerInto (*procB, xfTarget);
        xfLoaded = true;
    }
}

void SuperParasitesEngine::xfFinish (const Params& p)
{
    std::swap (proc, procB);                  // B (the incoming layer) becomes the engine; the old one is idle until the next Crossfade
    largeBuf.swap (largeBufB);
    smallBuf.swap (smallBufB);
    lastGeneration = proc->buffers_generation();
    loadedLayerIdx = xfTarget;
    const int mode = std::clamp (p.mode, 0, (int) NumModes - 1);
    freezeWasForced = (mode != BeatRepeat);   // B was frozen by force (not in Beat repeat): continue that way
    forcedFreeze = freezeWasForced;
    lastFreeze = p.freeze;
    xfAbort();
    publishLayers();
}

void SuperParasitesEngine::layersBeforeBlock (const Params& p)
{
    const int wanted = std::clamp (p.layer, 0, kNumLayers);
    const int mode = std::clamp (p.mode, 0, (int) NumModes - 1);
    const int target = (wanted > 0 && modeUsesLayers (mode)) ? wanted : 0;   // the layer that should be playing now
    const bool valid = proc->samples_valid();
    serviceTransfers();     // Save / Load hand-overs with the UI thread

    // A Crossfade is abandoned (the old layer simply carries on) if its target is emptied or the mode stops using layers.
    if (xfState != 0 && (! modeUsesLayers (mode) || xfTarget < 1 || ! layers[xfTarget - 1].filled)) xfAbort();

    // Clear request: empties the selected layer.
    if (clearRequested.exchange (false) && wanted > 0)
    {
        Layer& L = layers[wanted - 1];
        if (recording && recLayer == wanted) discardRecording();
        L.filled = false; L.seconds = 0.0f; L.frames = 0; L.mem[0].clear(); L.mem[1].clear();
        bump (wanted, true);
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
        else if (target > 0 && loadedLayerIdx == target && ! switching && xfState == 0 && ! reloadNeeded && valid)
        {
            proc->BeginLayerRecording();            // clears the memory, records one pass from the start
            recording = true; recLayer = target;
            layers[target - 1].filled = false; layers[target - 1].seconds = 0.0f; layers[target - 1].frames = 0;
            bump (target, true);
            layers[target - 1].mem[0].clear(); layers[target - 1].mem[1].clear();
            publishLayers();
        }
    }

    // Switching layer (or going to / from Live), only once the memory holds audio.
    //  Switch Time = 0: mute (about 24 ms), swap, unmute (the processor's own mute fade).
    //  Switch Time > 0: the output fades out over half the time, the layer is swapped, and it fades back in over the other half.
    const int switchMs = (int) std::lround (std::clamp (p.switchTimeMs, 0.0f, kMaxSwitchMs));
    if (switching)
    {
        if (softFade)
        {
            fadePos -= fadeStep;
            if (fadePos <= 0.0f)
            {
                fadePos = 0.0f;
                loadLayerIntoProcessor (target);
                loadedLayerIdx = target;
                reloadNeeded = false;
                switching = false;
                fadingIn = true;
                publishLayers();
            }
        }
        else if (--fadeLeft <= 0)
        {
            loadLayerIntoProcessor (target);
            loadedLayerIdx = target;
            reloadNeeded = false;
            switching = false;
            proc->set_mute_out (false);
            publishLayers();
        }
    }
    else if (xfState != 0)
    {
        // a Crossfade is under way (advanced after the blocks are processed, see runBlocks)
    }
    else if ((target != loadedLayerIdx || reloadNeeded) && valid)
    {
        if (recording) finishRecording();           // changing layer while recording keeps what was recorded
        if (p.switchStyle == 1 && switchMs > 0 && target > 0 && layers[target - 1].filled && ! reloadNeeded && fadePos >= 1.0f
            && ! fadingIn)
        {
            xfStart (p, target);                    // the incoming layer starts playing on the second engine; the fade comes after the pre-roll
        }
        else
        {
        switching = true;
        softFade = switchMs > 0 || fadePos < 1.0f;  // (a switch asked for while a soft fade is still finishing continues smoothly)
        if (softFade)
        {
            const int half = std::max (1, (switchMs > 0 ? switchMs : 24) / 2);
            fadeStep = 1.0f / (float) half;
            fadingIn = false;
        }
        else
        {
            fadeLeft = kSwitchFadeBlocks;
            proc->set_mute_out (true);
        }
        }
    }
    else if (fadingIn)
    {
        fadePos += fadeStep;
        if (fadePos >= 1.0f) { fadePos = 1.0f; fadingIn = false; softFade = false; }
    }
    gainPrev = gainNow;
    gainNow = fadePos >= 1.0f ? 1.0f : 0.5f - 0.5f * std::cos (3.14159265358979f * fadePos);   // raised-cosine fade

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
    resetFade();
    proc->set_mute_out (false);
    loadedLayerIdx = 0;
    const int wanted = std::clamp (p.layer, 0, kNumLayers);
    const int mode = std::clamp (p.mode, 0, (int) NumModes - 1);
    if (wanted > 0 && modeUsesLayers (mode) && proc->samples_valid())
    {
        loadLayerIntoProcessor (wanted);      // freshly rebuilt processor: no sound is playing, so no fade is needed
        loadedLayerIdx = wanted;
    }
    publishFormat();
    publishLayers();
}


// ======================================================================================================================
// Saving and loading a layer's audio (WAV)
// ======================================================================================================================
namespace
{
    // Offline version of the plug-in's streaming resampler, with its own delay (17 input samples) taken out.
    std::vector<float> resampleOffline (const std::vector<float>& in, double inRate, double outRate)
    {
        if (std::abs (inRate - outRate) < 1e-6 || in.empty()) return in;
        SincResampler rs;
        rs.init (inRate, outRate);
        std::vector<float> padded (in);
        padded.insert (padded.end(), 64, 0.0f);
        std::vector<float> raw;
        raw.reserve ((size_t) ((double) padded.size() * outRate / inRate) + 8);
        rs.process (padded.data(), padded.size(), raw);
        const size_t drop = (size_t) std::lround (17.0 * outRate / inRate);
        const size_t want = (size_t) std::lround ((double) in.size() * outRate / inRate);
        std::vector<float> out;
        if (raw.size() > drop) out.assign (raw.begin() + (long) drop, raw.begin() + (long) std::min (raw.size(), drop + want));
        return out;
    }

    inline int16_t toS16 (float x)
    {
        const long v = std::lrintf (std::clamp (x, -1.0f, 1.0f) * 32768.0f);
        return (int16_t) std::clamp (v, -32768L, 32767L);
    }

    // The sample memory of one channel: `bytes` long, `cap` audio samples, then the 8-sample interpolation tail that repeats the start.
    void quantizeChannel (const float* x, size_t n, size_t cap, bool lowFi, size_t bytes, std::vector<uint8_t>& mem)
    {
        mem.assign (bytes, lowFi ? 127 : 0);
        const size_t tail = (size_t) ::kInterpolationTail;
        if (lowFi)
        {
            for (size_t i = 0; i < n; ++i) mem[i] = clouds::Lin2MuLaw (toS16 (x[i]));
            for (size_t k = 0; k < tail && cap + k < bytes; ++k) mem[cap + k] = mem[k];
        }
        else
        {
            int16_t* s = reinterpret_cast<int16_t*> (mem.data());
            for (size_t i = 0; i < n; ++i) s[i] = toS16 (x[i]);
            for (size_t k = 0; k < tail && (cap + k) * 2 + 1 < bytes; ++k) s[cap + k] = s[k];
        }
    }
}

void SuperParasitesEngine::publishFormat()
{
    if (! proc || ! proc->samples_valid()) return;
    void* d = nullptr; size_t b0 = 0, b1 = 0;
    if (! proc->sample_region (0, &d, &b0)) return;
    const bool two = proc->sample_region (1, &d, &b1);
    const bool lowFi = proc->stored_sample_rate() < 20000.0f;
    const int samples = lowFi ? (int) b0 : (int) (b0 / 2);
    fmtLowFi.store (lowFi);
    fmtChannels.store (two ? 2 : 1);
    fmtRate.store ((double) proc->stored_sample_rate());
    fmtCap.store (std::max (0, samples - (int) ::kInterpolationTail));
    fmtBytes0.store ((int64_t) b0);
    fmtBytes1.store (two ? (int64_t) b1 : 0);
    fmtValid.store (true);
}

SuperParasitesEngine::LayerFormat SuperParasitesEngine::layerFormat() const
{
    LayerFormat f;
    f.valid = fmtValid.load();
    f.channels = fmtChannels.load();
    f.lowFi = fmtLowFi.load();
    f.sampleRate = fmtRate.load();
    f.capFrames = fmtCap.load();
    return f;
}

void SuperParasitesEngine::serviceTransfers()
{
    if (exportState.load() == 1)
    {
        const int ly = exportLayerReq.load();
        if (ly >= 1 && ly <= kNumLayers && layers[ly - 1].filled && layers[ly - 1].frames > 0 && fmtValid.load())
        {
            const Layer& L = layers[ly - 1];
            exportMem[0].assign (L.mem[0].begin(), L.mem[0].end());
            exportMem[1].assign (L.mem[1].begin(), L.mem[1].end());
            exportFrames = L.frames;
            exportRate = fmtRate.load();
            exportLowFi = fmtLowFi.load();
            exportRev = uiRev[ly - 1].load(); exportEdit = uiEdit[ly - 1].load();
            exportState.store (2);
        }
        else
            exportState.store (3);
    }

    if (importState.load() == 2)
    {
        const int ly = importLayerReq;
        const bool ok = ly >= 1 && ly <= kNumLayers && ! recording && fmtValid.load()
                        && importBytes0 == fmtBytes0.load() && importBytes1 == fmtBytes1.load() && importLowFi == fmtLowFi.load()
                        && (int64_t) importMem[0].size() == importBytes0 && (int64_t) importMem[1].size() == importBytes1;
        if (ok)
        {
            Layer& L = layers[ly - 1];
            L.mem[0].assign (importMem[0].begin(), importMem[0].end());
            L.mem[1].assign (importMem[1].begin(), importMem[1].end());
            L.frames = importFrames;
            const int cap = fmtCap.load();
            L.head[0] = L.head[1] = (importFrames >= cap ? 0 : importFrames);   // as if it had been recorded: the head wraps after a full pass
            L.seconds = (float) ((double) importFrames / fmtRate.load());
            L.filled = true;
            bump (ly, false);
            if (xfState != 0 && xfTarget == ly) xfAbort();   // a Crossfade towards this layer would play the old audio
            if (loadedLayerIdx == ly) reloadNeeded = true;    // it is playing now: swap the new audio in with the usual fade
            publishLayers();
            importOutcome.store (ImportOk);
        }
        else
            importOutcome.store (ImportRejected);
        importState.store (0);
    }
}

bool SuperParasitesEngine::requestExport (int layer)
{
    if (layer < 1 || layer > kNumLayers) return false;
    int s = exportState.load();
    if (s == 2 || s == 3) exportState.store (0);     // an old, never-collected result
    int expected = 0;
    if (! exportState.compare_exchange_strong (expected, 4)) return false;
    exportLayerReq.store (layer);
    exportState.store (1);
    return true;
}

SuperParasitesEngine::IoResult SuperParasitesEngine::pollExport (LayerAudio& out)
{
    const int s = exportState.load();
    if (s == 3) { exportState.store (0); return ExportEmpty; }
    if (s != 2) return IoNone;
    const int nch = exportMem[1].empty() ? 1 : 2;
    const size_t n = (size_t) exportFrames;
    out = LayerAudio();
    out.channels = nch;
    out.sampleRate = exportRate;
    for (int c = 0; c < nch; ++c)
    {
        out.ch[c].assign (n, 0.0f);
        const auto& m = exportMem[c];
        if (exportLowFi)
        {
            for (size_t i = 0; i < n && i < m.size(); ++i) out.ch[c][i] = (float) clouds::MuLaw2Lin (m[i]) / 32768.0f;
        }
        else
        {
            for (size_t i = 0; i < n && i * 2 + 1 < m.size(); ++i)
            {
                int16_t v; std::memcpy (&v, &m[i * 2], 2);
                out.ch[c][i] = (float) v / 32768.0f;
            }
        }
    }
    exportState.store (0);
    return ExportReady;
}

bool SuperParasitesEngine::submitImport (int layer, const LayerAudio& audio, std::string& whyNot)
{
    if (layer < 1 || layer > kNumLayers) { whyNot = "Pick a layer (1-6) first."; return false; }
    if (audio.frames() == 0 || audio.channels < 1 || audio.sampleRate < 1000.0) { whyNot = "That audio is empty."; return false; }
    const LayerFormat f = layerFormat();
    if (! f.valid || f.capFrames <= 0)
    { whyNot = "The layer memory isn't ready - choose Granular, Pitch shifter, Looping delay or Beat repeat, let it run a moment, then try again."; return false; }
    int expected = 0;
    if (! importState.compare_exchange_strong (expected, 1)) { whyNot = "Still busy with the last load - try again in a moment."; return false; }

    const size_t cap = (size_t) f.capFrames;
    const size_t need = std::min (audio.frames(), (size_t) std::ceil ((double) cap * audio.sampleRate / f.sampleRate) + 128);
    auto take = [&] (int c) { return std::vector<float> (audio.ch[c].begin(), audio.ch[c].begin() + (long) need); };
    std::vector<float> a = resampleOffline (take (0), audio.sampleRate, f.sampleRate);
    std::vector<float> b;
    if (audio.channels >= 2) b = resampleOffline (take (1), audio.sampleRate, f.sampleRate);
    size_t n = std::min (a.size(), cap);
    if (! b.empty()) n = std::min (n, b.size());

    std::vector<float> L (a.begin(), a.begin() + (long) n), R;
    if (f.channels == 1)
    {
        if (! b.empty()) for (size_t i = 0; i < n; ++i) L[i] = 0.5f * (a[i] + b[i]);      // stereo file into a mono layer: the two channels are mixed
    }
    else
        R = b.empty() ? L : std::vector<float> (b.begin(), b.begin() + (long) n);          // mono file into a stereo layer: both sides

    const int64_t bytes0 = fmtBytes0.load(), bytes1 = fmtBytes1.load();
    quantizeChannel (L.data(), n, cap, f.lowFi, (size_t) bytes0, importMem[0]);
    if (f.channels == 2) quantizeChannel (R.data(), n, cap, f.lowFi, (size_t) bytes1, importMem[1]);
    else importMem[1].clear();
    importBytes0 = bytes0; importBytes1 = f.channels == 2 ? bytes1 : 0;
    importLowFi = f.lowFi;
    importFrames = (int32_t) n;
    importLayerReq = layer;
    importOutcome.store (IoNone);
    importState.store (2);
    return true;
}

SuperParasitesEngine::IoResult SuperParasitesEngine::pollImport()
{
    if (importState.load() != 0) return IoNone;
    return (IoResult) importOutcome.exchange (IoNone);
}
