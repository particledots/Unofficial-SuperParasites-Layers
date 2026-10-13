// Copyright 2014 Olivier Gillet.
//
// Author: Olivier Gillet (ol.gillet@gmail.com)
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
// 
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
// 
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
// 
// See http://creativecommons.org/licenses/MIT/ for more information.
//
// -----------------------------------------------------------------------------
//
// Main processing class.

#ifndef CLOUDS_DSP_GRANULAR_PROCESSOR_H_
#define CLOUDS_DSP_GRANULAR_PROCESSOR_H_

#include "stmlib/stmlib.h"
#include "stmlib/dsp/filter.h"

#include "supercell/dsp/correlator.h"
#include "supercell/dsp/engine_rate.h"
#include "supercell/dsp/frame.h"
#include "supercell/dsp/fx/diffuser.h"
#include "supercell/dsp/fx/pitch_shifter.h"
#include "supercell/dsp/fx/reverb.h"
#include "supercell/dsp/resonestor.h"
#include "supercell/dsp/fx/oliverb.h"
#include "supercell/dsp/granular_processor.h"
#include "supercell/dsp/granular_sample_player.h"
#include "supercell/dsp/kammerl_player.h"
#include "supercell/dsp/looping_sample_player.h"
#include "supercell/dsp/pvoc/phase_vocoder.h"
#include "supercell/dsp/sample_rate_converter.h"
#include "supercell/dsp/wsola_sample_player.h"

namespace clouds {

// Low-fidelity qualities run the processing at 16 kHz: the engine rate (32 or 48 kHz) is decimated by 2 or 3.
const int32_t kDownsamplingFactor = 2;
const int32_t kDownsamplingFactorHd = 3;
// Largest number of frames per processing block at the low-fidelity processing rate (always 16 = 1 ms).
const int32_t kMaxLowFiBlock = 16;

enum PlaybackMode {
  PLAYBACK_MODE_GRANULAR,
  PLAYBACK_MODE_STRETCH,
  PLAYBACK_MODE_LOOPING_DELAY,
  PLAYBACK_MODE_SPECTRAL,
  PLAYBACK_MODE_OLIVERB,
  PLAYBACK_MODE_RESONESTOR,
  PLAYBACK_MODE_KAMMERL,
  PLAYBACK_MODE_SPECTRAL_CLOUD,
  PLAYBACK_MODE_LAST
};

// State of the recording buffer as saved in one of the 4 sample memories.
struct PersistentState {
  int32_t write_head[2];
  uint8_t quality;
  uint8_t spectral;
};

// Data block as saved in one of the 4 sample memories.
struct PersistentBlock {
  uint32_t tag;
  uint32_t size;
  void* data;
};

class GranularProcessor {
 public:
  GranularProcessor() { }
  ~GranularProcessor() { }
  
  // `rate` selects the engine rate: 32 kHz (default, the original) or 48 kHz. At 48 kHz the buffers must be sized for
  // it: the effects' workspace (large minus small in stereo, small in mono) needs at least kMinWorkspaceHd bytes.
  // sample_memory_stereo / sample_memory_mono, when non-zero, cap the recording memory per channel (stereo) or in total
  // (mono) so that the recording length in seconds can be set independently of the workspace size.
  void Init(
      void* large_buffer,
      size_t large_buffer_size,
      void* small_buffer,
      size_t small_buffer_size,
      const EngineRate& rate = EngineRate(),
      size_t sample_memory_stereo = 0,
      size_t sample_memory_mono = 0);

  // Bytes of effects workspace the 48 kHz engine needs (diffuser 16384 + reverb 65536 + pitch shifter 16384).
  static const size_t kMinWorkspaceHd = 98304;
  // Frames per processing block at the engine rate (1 ms).
  inline size_t block_size() const { return static_cast<size_t>(rate_.block); }

  void Process(ShortFrame* input, ShortFrame* output, size_t size);
  void Prepare();
  
  inline Parameters* mutable_parameters() {
    return &parameters_;
  }

  inline const Parameters& parameters() const {
    return parameters_;
  }
  
  inline void ToggleFreeze() {
    parameters_.freeze = !parameters_.freeze;
  }

  inline void set_freeze(bool freeze) {
    parameters_.freeze = freeze;
  }

  inline bool frozen() const {
    return parameters_.freeze;
  }


  inline void ToggleReverse() {
    parameters_.granular.reverse = !parameters_.granular.reverse;
  }

  inline bool reversed() const {
    return parameters_.granular.reverse;
  }

  inline void set_silence(bool silence) {
    silence_ = silence;
  }

  inline void set_bypass(bool bypass) {
    bypass_ = bypass;
  }
  
  inline bool bypass() const {
    return bypass_;
  }

  inline void set_mute_out(bool mute) {
    mute_out_ = mute;
  }

  inline bool mute_out() const {
    return mute_out_;
  }

  inline void set_mute_in(bool mute) {
    mute_in_ = mute;
  }

  inline bool mute_in() const {
    return mute_in_;
  }
  
  inline void set_playback_mode(PlaybackMode playback_mode) {
    playback_mode_ = playback_mode;
  }
  
  inline PlaybackMode playback_mode() const { return playback_mode_; }
  
  inline void set_quality(int32_t quality) {
    set_num_channels(quality & 1 ? 1 : 2);
    set_low_fidelity(quality >> 1 ? true : false);
  }
  
  inline void set_num_channels(int32_t num_channels) {
    reset_buffers_ = reset_buffers_ || num_channels_ != num_channels;
    num_channels_ = num_channels;
  }
  
  inline void set_low_fidelity(bool low_fidelity) {
    reset_buffers_ = reset_buffers_ || low_fidelity != low_fidelity_;
    low_fidelity_ = low_fidelity;
  }
  
  inline int32_t quality() const {
    int32_t quality = 0;
    if (num_channels_ == 1) quality |= 1;
    if (low_fidelity_) quality |= 2;
    return quality;
  }
  
  void GetPersistentData(PersistentBlock* block, size_t *num_blocks);
  bool LoadPersistentData(const uint32_t* data);
  void PreparePersistentData();

  // ---- Unofficial SuperParasites Layers --------------------------------------------------------------------------
  // The plug-in keeps its layers outside the processor (switching to Spectral, Resonestor or Oliverb rebuilds the
  // processor's sample memory and reuses it as workspace) and copies a layer in and out through these calls.
  // None of them changes anything unless it is called, so the Live behaviour is the original one.

  // Counts how many times the sample memory has been rebuilt (mode change into/out of Spectral, Spectral clouds,
  // Resonestor or Oliverb, quality change, first Prepare). After a change the memory holds fresh silence.
  inline int32_t buffers_generation() const { return buffers_generation_; }

  // True when the sample memory currently holds recorded audio (Granular, Pitch shifter/stretch, Looping delay, Oliverb,
  // Beat repeat); false in the modes that use that memory as workspace.
  inline bool samples_valid() const { return samples_valid_; }

  // The sample memory of channel ch (0 or 1): where it is and how many bytes. Returns false if it isn't in use.
  bool sample_region(int32_t ch, void** data, size_t* bytes) const;

  // Where the write head of channel ch is, and moving both write heads.
  int32_t write_head(int32_t ch) const;
  void ResyncWriteHeads(int32_t head0, int32_t head1);

  // Sets the sample memory to silence (the right silence for 16-bit or low-fi 8-bit) and the write heads to 0.
  void ClearSamples();

  // While true, nothing is written into the sample memory (Beat repeat and Oliverb normally record even when frozen).
  inline void set_hold_writes(bool hold) { hold_writes_ = hold; }

  // Clears the sample memory, puts the write heads at 0 and records exactly one pass of the memory, then stops
  // writing by itself (layer_record_done() becomes true). layer_record_frames() = frames written so far per channel.
  void BeginLayerRecording();
  void EndLayerRecording();
  inline bool layer_recording() const { return layer_rec_; }
  inline bool layer_record_done() const { return layer_rec_done_; }
  inline int32_t layer_record_frames() const { return layer_rec_frames_; }
  // The stored sample rate: 16 kHz in low-fi, otherwise the engine rate.
  inline float stored_sample_rate() const { return low_fidelity_ ? kBaseSampleRate / kDownsamplingFactor : rate_.hz; }

 private:
  void WarmDistortion(float* in, float parameter);

  inline int32_t resolution() const {
    return low_fidelity_ ? 8 : 16;
  }

  // The original's notion of the sample rate: 32 kHz, or 16 kHz in low-fi. (Used for the feedback filter cutoff, which
  // the original expresses relative to this value; rate_.Freq() then converts the result to the engine rate.)
  inline float sample_rate() const {
    return 32000.0f / \
        (low_fidelity_ ? kDownsamplingFactor : 1);
  }

  // The rate the mode-specific processing runs at: the engine rate, or 16 kHz in low-fi. In low-fi all of it is exactly
  // the original 16 kHz processing (same constants), whichever the engine rate is.
  inline const EngineRate& proc_rate() const {
    return low_fidelity_ ? base_rate_ : rate_;
  }
     
  void ResetFilters();
  void ProcessGranular(FloatFrame* input, FloatFrame* output, size_t size);

  // The mode and post-processing code that is the same for both rates; called with the <0> or <1> effect instance.
  template<class OL> void RunOliverb(OL& oliverb, size_t size, FloatFrame* output);
  template<class RS> void RunResonestor(RS& resonestor, size_t size, FloatFrame* input, FloatFrame* output);
  template<class PS> void RunPitchShifter(PS& pitch_shifter, size_t size);
  template<class RV> void RunReverb(RV& reverb, size_t size, float feedback);

  PlaybackMode playback_mode_;
  PlaybackMode previous_playback_mode_;
  int32_t num_channels_;
  bool low_fidelity_;
  
  bool silence_;
  bool bypass_;
  bool reset_buffers_;
  bool mute_in_;
  bool mute_out_;
  float mute_in_fade_;
  float mute_out_fade_;

  float freeze_lp_;
  float dry_wet_;
  
  void* buffer_[2];
  size_t buffer_size_[2];
  
  Correlator correlator_;
  
  GranularSamplePlayer player_;
  WSOLASamplePlayer ws_player_;
  LoopingSamplePlayer looper_;
  PhaseVocoder phase_vocoder_;
  KammerlPlayer kammerl_;
  
  // Delay-line effects need their lengths at compile time, so each exists in a 32 kHz (<0>) and a 48 kHz (<1>) version.
  // Post-processing (diffuser, reverb, pitch shifter) runs at the engine rate; Oliverb and Resonestor run inside the
  // mode processing, so in low-fi they use the 16 kHz (original, <0>) version at either engine rate.
  Diffuser<0> diffuser_;
  Reverb<0> reverb_;
  Oliverb<0> oliverb_;
  Resonestor<0> resonestor_;
  PitchShifter<0> pitch_shifter_;
  Diffuser<1> diffuser_hd_;
  Reverb<1> reverb_hd_;
  Oliverb<1> oliverb_hd_;
  Resonestor<1> resonestor_hd_;
  PitchShifter<1> pitch_shifter_hd_;
  stmlib::Svf fb_filter_[2];
  stmlib::Svf hp_filter_[2];
  stmlib::Svf lp_filter_[2];
  
  AudioBuffer<RESOLUTION_8_BIT_MU_LAW> buffer_8_[2];
  AudioBuffer<RESOLUTION_16_BIT> buffer_16_[2];
  
  FloatFrame in_[kMaxBlockSize];
  FloatFrame in_downsampled_[kMaxLowFiBlock];
  FloatFrame out_downsampled_[kMaxLowFiBlock];
  FloatFrame out_[kMaxBlockSize];
  FloatFrame fb_[kMaxBlockSize];
  
  int16_t tail_buffer_[2][384];   // HD: 1.5x the original 256 (the cross-fade at the end of the recording buffer)
  
  Parameters parameters_;
  
  SampleRateConverter<-kDownsamplingFactor, 45, src_filter_1x_2_45> src_down_;
  SampleRateConverter<+kDownsamplingFactor, 45, src_filter_1x_2_45> src_up_;
  SampleRateConverter<-kDownsamplingFactorHd, 69, src_filter_1x_3_69> src_down_hd_;
  SampleRateConverter<+kDownsamplingFactorHd, 69, src_filter_1x_3_69> src_up_hd_;

  EngineRate rate_;        // engine rate: 32 or 48 kHz
  EngineRate base_rate_;   // always the original 32 kHz constants (EngineRate(false)); used for 16 kHz low-fi processing
  float mute_k_;           // per-sample coefficient of the mute fades, same speed in seconds at either rate
  size_t sample_memory_stereo_;
  size_t sample_memory_mono_;

  // Layers (see the public calls above).
  void* region_ptr_[2];
  size_t region_bytes_[2];
  bool samples_valid_;
  int32_t buffers_generation_;
  bool hold_writes_;
  bool layer_rec_;
  bool layer_rec_done_;
  int32_t layer_rec_frames_;
  
  PersistentState persistent_state_;
  
  DISALLOW_COPY_AND_ASSIGN(GranularProcessor);
};

}  // namespace clouds

#endif  // CLOUDS_DSP_GRANULAR_PROCESSOR_H_
