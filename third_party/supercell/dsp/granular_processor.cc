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

#include "supercell/dsp/granular_processor.h"

#include <cstring>


#include "stmlib/dsp/parameter_interpolator.h"
#include "stmlib/utils/buffer_allocator.h"

#include "supercell/resources.h"

namespace clouds {

using namespace std;
using namespace stmlib;

void GranularProcessor::Init(
    void* large_buffer, size_t large_buffer_size,
    void* small_buffer, size_t small_buffer_size,
    const EngineRate& rate,
    size_t sample_memory_stereo,
    size_t sample_memory_mono) {
  rate_ = rate;
  base_rate_ = EngineRate(false);
  mute_k_ = rate_.Pole(0.01f);   // 0.01 at 32 kHz (the original value)
  sample_memory_stereo_ = sample_memory_stereo;
  sample_memory_mono_ = sample_memory_mono;
  buffer_[0] = large_buffer;
  buffer_[1] = small_buffer;
  buffer_size_[0] = large_buffer_size;
  buffer_size_[1] = small_buffer_size;

  num_channels_ = 2;
  low_fidelity_ = false;
  bypass_ = false;

  region_ptr_[0] = region_ptr_[1] = NULL;
  region_bytes_[0] = region_bytes_[1] = 0;
  samples_valid_ = false;
  buffers_generation_ = 0;
  hold_writes_ = false;
  layer_rec_ = false;
  layer_rec_done_ = false;
  layer_rec_frames_ = 0;

  src_down_.Init();
  src_up_.Init();
  src_down_hd_.Init();
  src_up_hd_.Init();

  phase_vocoder_.Init();

  ResetFilters();

  previous_playback_mode_ = PLAYBACK_MODE_LAST;
  reset_buffers_ = true;
  mute_in_ = false;
  mute_out_ = false;
  mute_in_fade_ = 0.0f;
  mute_out_fade_ = 0.0f;
  dry_wet_ = 0.0f;
}

void GranularProcessor::ResetFilters() {
  for (int32_t i = 0; i < 2; ++i) {
    fb_filter_[i].Init();
    lp_filter_[i].Init();
    hp_filter_[i].Init();
  }
}

void GranularProcessor::ProcessGranular(
    FloatFrame* input,
    FloatFrame* output,
    size_t size) {
  // At the exception of the spectral mode, all modes require the incoming
  // audio signal to be written to the recording buffer.
  if (playback_mode_ != PLAYBACK_MODE_SPECTRAL &&
      playback_mode_ != PLAYBACK_MODE_SPECTRAL_CLOUD &&
      playback_mode_ != PLAYBACK_MODE_RESONESTOR) {
    const float* input_samples = &input[0].l;
    bool play = !parameters_.freeze ||
      playback_mode_ == PLAYBACK_MODE_OLIVERB ||
      playback_mode_ == PLAYBACK_MODE_KAMMERL;
    int32_t frames = static_cast<int32_t>(size);
    int32_t room = 0;
    if (hold_writes_) {
      play = false;                       // a layer is playing: its audio must not be overwritten
    }
    if (layer_rec_) {
      // Layer recording: write one pass of the sample memory, then stop by itself.
      play = true;
      const int32_t head = resolution() == 8 ? buffer_8_[0].head() : buffer_16_[0].head();
      const int32_t cap = resolution() == 8 ? buffer_8_[0].size() : buffer_16_[0].size();
      room = cap - head;
      if (room < 0) room = 0;
      if (frames > room) frames = room;
    }
    for (int32_t i = 0; i < num_channels_; ++i) {
      if (resolution() == 8) {
        buffer_8_[i].WriteFade(&input_samples[i], frames, 2, play);
      } else {
        buffer_16_[i].WriteFade(&input_samples[i], frames, 2, play);
      }
    }
    if (layer_rec_) {
      layer_rec_frames_ += frames;
      if (frames >= room) {
        layer_rec_ = false;               // the pass is complete
        layer_rec_done_ = true;
      }
    }
  }

  switch (playback_mode_) {
    case PLAYBACK_MODE_GRANULAR:
      // In Granular mode, DENSITY is a meta parameter.
      parameters_.granular.use_deterministic_seed = parameters_.density < 0.5f;
      if (parameters_.density >= 0.53f) {
        parameters_.granular.overlap = (parameters_.density - 0.53f) * 2.12f;
      } else if (parameters_.density <= 0.47f) {
        parameters_.granular.overlap = (0.47f - parameters_.density) * 2.12f;
      } else {
        parameters_.granular.overlap = 0.0f;
      }

#ifdef CLOUDS_QUANTIZE_SEMITONES
      // Quantize pitch to closest semitone
      if (parameters_.pitch < 0.5f) parameters_.pitch -= 0.5f;
      parameters_.pitch = static_cast<int>(parameters_.pitch + 0.5f);
#endif

      // And TEXTURE too.
      parameters_.granular.window_shape = parameters_.texture < 0.75f
          ? parameters_.texture * 1.333f : 1.0f;

      if (resolution() == 8) {
        player_.Play(buffer_8_, parameters_, &output[0].l, size);
      } else {
        player_.Play(buffer_16_, parameters_, &output[0].l, size);
      }
      break;

    case PLAYBACK_MODE_STRETCH:
      if (resolution() == 8) {
        ws_player_.Play(buffer_8_, parameters_, &output[0].l, size);
      } else {
        ws_player_.Play(buffer_16_, parameters_, &output[0].l, size);
      }
      break;

    case PLAYBACK_MODE_LOOPING_DELAY:
      if (resolution() == 8) {
        looper_.Play(buffer_8_, parameters_, &output[0].l, size);
      } else {
        looper_.Play(buffer_16_, parameters_, &output[0].l, size);
      }
      break;

    case PLAYBACK_MODE_SPECTRAL:
      {
        parameters_.spectral.quantization = parameters_.texture;
        parameters_.spectral.refresh_rate = 0.01f + 0.99f * parameters_.density;
        float warp = parameters_.size - 0.5f;
        parameters_.spectral.warp = 4.0f * warp * warp * warp + 0.5f;

        float randomization = parameters_.density - 0.5f;
        randomization *= randomization * 4.2f;
        randomization -= 0.05f;
        CONSTRAIN(randomization, 0.0f, 1.0f);
        parameters_.spectral.phase_randomization = randomization;
        phase_vocoder_.Process(parameters_, input, output, size);
      }
      break;

    case PLAYBACK_MODE_SPECTRAL_CLOUD:
      {
        phase_vocoder_.Process(parameters_, input, output, size);

        if (num_channels_ == 1) {
          for (size_t i = 0; i < size; ++i) {
            output[i].r = output[i].l;
          }
        }
      }
      break;

    case PLAYBACK_MODE_OLIVERB:
      {

        // Pre-delay, controlled by position or tap tempo sync
        Parameters p = {
          ws_player_.synchronized() ?
          parameters_.position :
          parameters_.position * 0.25f, // position;
          0.1f, // size;
          0.0f, // pitch;
          0.0f, // density;
          0.5f, // texture;
          1.0f, // dry_wet;
          0.0f, // stereo_spread;
          0.0f, // feedback;
          0.0f, // reverb;
          0.0f, // freeze;
          parameters_.capture, // trigger;
          0.0f // gate;
        };

        if (resolution() == 8) {
          ws_player_.Play(buffer_8_, p, &output[0].l, size);
        } else {
          ws_player_.Play(buffer_16_, p, &output[0].l, size);
        }

        if (proc_rate().hi) {
          RunOliverb(oliverb_hd_, size, output);
        } else {
          RunOliverb(oliverb_, size, output);
        }
      }
      break;

  case PLAYBACK_MODE_RESONESTOR:
    if (proc_rate().hi) {
      RunResonestor(resonestor_hd_, size, input, output);
    } else {
      RunResonestor(resonestor_, size, input, output);
    }
    break;

  case PLAYBACK_MODE_KAMMERL:
    if (resolution() == 8) {
      kammerl_.Play(buffer_8_, parameters_, &output[0].l, size);
    } else {
      kammerl_.Play(buffer_16_, parameters_, &output[0].l, size);
    }
    break;

    default:
      break;
  }
}

template<class OL>
void GranularProcessor::RunOliverb(OL& oliverb, size_t size, FloatFrame* output) {
  {
        // Settings of the reverb
        oliverb.set_diffusion(0.3f + 0.5f * parameters_.stereo_spread);
        oliverb.set_size(0.05f + 0.94f * parameters_.size);
        oliverb.set_mod_rate(parameters_.feedback);
        oliverb.set_mod_amount(parameters_.reverb * 300.0f);
        oliverb.set_ratio(SemitonesToRatio(parameters_.pitch));

        float x = parameters_.pitch;
        const float limit = 0.7f;
        const float slew = 0.4f;

        float wet =
          x < -limit ? 1.0f :
          x < -limit + slew ? 1.0f - (x + limit) / slew:
          x < limit - slew ? 0.0f :
          x < limit ? 1.0f + (x - limit) / slew:
          1.0f;
        oliverb.set_pitch_shift_amount(wet);

        if (parameters_.freeze) {
          oliverb.set_input_gain(0.0f);
          oliverb.set_decay(1.0f);
          oliverb.set_lp(1.0f);
          oliverb.set_hp(0.0f);
        } else {
          oliverb.set_decay(parameters_.density * 1.3f
                           + 0.15f * abs(parameters_.pitch) / 24.0f);
          oliverb.set_input_gain(0.5f);
          float lp = parameters_.texture < 0.5f ?
            parameters_.texture * 2.0f : 1.0f;
          float hp = parameters_.texture > 0.5f ?
            (parameters_.texture - 0.5f) * 2.0f : 0.0f;
          oliverb.set_lp(0.03f + 0.9f * lp);
          oliverb.set_hp(0.01f + 0.2f * hp); // the small offset
                                                  // gets rid of
                                                  // feedback of large
                                                  // DC offset.
        }
        oliverb.Process(output, size);
  }
}

template<class RS>
void GranularProcessor::RunResonestor(
    RS& resonestor, size_t size, FloatFrame* input, FloatFrame* output) {
  {
      copy(&input[0], &input[size], &output[0]);

      resonestor.set_pitch(parameters_.pitch);
      resonestor.set_chord(parameters_.size);
      resonestor.set_trigger(parameters_.capture);
      resonestor.set_burst_damp(parameters_.position);
      resonestor.set_burst_comb((1.0f - parameters_.position));
      resonestor.set_burst_duration((1.0f - parameters_.position));
      resonestor.set_spread_amount(parameters_.reverb);
      resonestor.set_stereo(parameters_.stereo_spread < 0.5f ? 0.0f :
        (parameters_.stereo_spread - 0.5f) * 2.0f);
      resonestor.set_separation(parameters_.stereo_spread > 0.5f ? 0.0f :
                                (0.5f - parameters_.stereo_spread) * 2.0f);
      resonestor.set_freeze(parameters_.freeze);
      resonestor.set_harmonicity(1.0f - (parameters_.feedback * 0.5f));
      resonestor.set_distortion(parameters_.dry_wet);

      float t = parameters_.texture;
      if (t < 0.5f) {
        resonestor.set_narrow(0.001f);
        float l = 1.0f - (0.5f - t) / 0.5f;
        l = l * (1.0f - 0.08f) + 0.08f;
        resonestor.set_damp(l * l);
      } else {
        resonestor.set_damp(1.0f);
        float n = (t - 0.5f) / 0.5f * 1.35f;
        n *= n * n * n;
        resonestor.set_narrow(0.001f + n * n * 0.6f);
      }

      float d = (parameters_.density - 0.05f) / 0.9f;
      if (d < 0.0f) d = 0.0f;
      d *= d * d;
      d *= d * d;
      d *= d * d;
      resonestor.set_feedback(d * 20.0f);

      resonestor.Process(output, size);
  }
}

template<class PS>
void GranularProcessor::RunPitchShifter(PS& pitch_shifter, size_t size) {
  {
    pitch_shifter.set_ratio(SemitonesToRatio(parameters_.pitch));
    pitch_shifter.set_size(parameters_.size);
    if (PLAYBACK_MODE_SPECTRAL_CLOUD != playback_mode_) {
      // parasites
      float x = parameters_.pitch;
      const float limit = 0.7f;
      const float slew = 0.4f;
      float wet =
        x < -limit ? 1.0f :
        x < -limit + slew ? 1.0f - (x + limit) / slew:
        x < limit - slew ? 0.0f :
        x < limit ? 1.0f + (x - limit) / slew:
        1.0f;
      pitch_shifter.set_dry_wet(wet);
    } else {
      // beat repeat
      pitch_shifter.set_dry_wet(1.f);
    }
    pitch_shifter.Process(out_, size);
  }
}

template<class RV>
void GranularProcessor::RunReverb(RV& reverb, size_t size, float feedback) {
  {
    float reverb_amount = parameters_.reverb;

    reverb.set_amount(reverb_amount * 0.54f);
    reverb.set_diffusion(0.7f);
    reverb.set_time(0.35f + 0.63f * reverb_amount);
    reverb.set_input_gain(0.2f);
    reverb.set_lp(0.6f + 0.37f * feedback);

    reverb.Process(out_, size);
  }
}

void GranularProcessor::WarmDistortion(float* in, float parameter) {
	if (parameter < 0.1) {
		return;
	}
	static const float kMaxDistf = 2.0f;
	const float fac = kMaxDistf * parameter;
	const float amp = 1.0f - parameter * 0.45f;

	float smp = *in;
	smp = (1.0f + fac) * smp - fac * smp * smp * smp;

	float sign = 1.0f;
	if (smp < 0) {
		sign = -1.0;
	}
	float tanh_loopup =  std::max(0.0f, std::min(1.0f, (smp / 2.0f) * sign));
	float inv_tanh_smp = Interpolate(lut_inv_tanh, tanh_loopup,
			static_cast<float>(LUT_INV_TANH_SIZE-1)) * sign;

	smp = smp + (inv_tanh_smp - smp) * fac;
	smp *= amp;
	smp = std::max(-1.0f, std::min(1.0f, smp));
	*in = smp;
}


void GranularProcessor::Process(
    ShortFrame* input,
    ShortFrame* output,
    size_t size) {
  // TIC
  if (bypass_) {
    copy(&input[0], &input[size], &output[0]);
    return;
  }

  if (silence_ || reset_buffers_ ||
      previous_playback_mode_ != playback_mode_) {
    short* output_samples = &output[0].l;
    fill(&output_samples[0], &output_samples[size << 1], 0);
    return;
  }

  // Convert input buffers to float, and mixdown for mono processing.
  // SUPERCELL Handle Mute In separately
  // TODO[pld] These loops look inefficient, we should be able to avoid unnecesary float/short conversions
  float mute_level_in;
  for (size_t i = 0; i < size; i++) {
    if (mute_out_) {
        mute_level_in = 0.0f;
    } else {
        mute_level_in = mute_in_ ? 0.0f : 1.0f;
    }
    mute_in_fade_ += mute_k_ * (mute_level_in - mute_in_fade_);
    input[i].l = input[i].l * mute_in_fade_;
    input[i].r = input[i].r * mute_in_fade_;
  }
  for (size_t i = 0; i < size; ++i) {
    in_[i].l = static_cast<float>(input[i].l) / 32768.0f;
    in_[i].r = static_cast<float>(input[i].r) / 32768.0f;
  }

  if (num_channels_ == 1) {
    for (size_t i = 0; i < size; ++i) {
      float xfade = 0.5f;
      // in mono delay modes, stereo spread controls input crossfade
      if (playback_mode_ == PLAYBACK_MODE_LOOPING_DELAY ||
          playback_mode_ == PLAYBACK_MODE_STRETCH)
        xfade = parameters_.stereo_spread;

      in_[i].l = in_[i].l * (1.0f - xfade) + in_[i].r * xfade;
      in_[i].r = in_[i].l;
    }
  }

  // Apply feedback, with high-pass filtering to prevent build-ups at very
  // low frequencies (causing large DC swings).
  float feedback =
		  (playback_mode_ == PLAYBACK_MODE_KAMMERL
				  && kammerl_.isSlicePlaybackActive()) ?
				  parameters_.reverb : 0.0f; // Map reverb parameter to feedback in PLAYBACK_MODE_KAMMERL.
  if (playback_mode_ != PLAYBACK_MODE_OLIVERB &&
      playback_mode_ != PLAYBACK_MODE_RESONESTOR &&
      playback_mode_ != PLAYBACK_MODE_KAMMERL &&
      playback_mode_ != PLAYBACK_MODE_SPECTRAL_CLOUD) {
	ONE_POLE(freeze_lp_, parameters_.freeze ? 1.0f : 0.0f, 0.0005f)
	feedback = parameters_.feedback;
	// (the original expresses this relative to sample_rate(); Freq() keeps the same Hz at the engine rate)
	float cutoff = rate_.Freq((20.0f + 100.0f * feedback * feedback) / sample_rate());
	fb_filter_[0].set_f_q<FREQUENCY_FAST>(cutoff, 1.0f);
	fb_filter_[1].set(fb_filter_[0]);
	fb_filter_[0].Process<FILTER_MODE_HIGH_PASS>(&fb_[0].l, &fb_[0].l, size, 2);
	fb_filter_[1].Process<FILTER_MODE_HIGH_PASS>(&fb_[0].r, &fb_[0].r, size, 2);
  }
  float fb_gain = feedback * (1.0f - freeze_lp_);
  for (size_t i = 0; i < size; ++i) {
	in_[i].l += fb_gain * (
		SoftLimit(fb_gain * 1.4f * fb_[i].l + in_[i].l) - in_[i].l);
	in_[i].r += fb_gain * (
		SoftLimit(fb_gain * 1.4f * fb_[i].r + in_[i].r) - in_[i].r);
  }

  if (low_fidelity_) {
    size_t downsampled_size = size / rate_.down;
    if (rate_.hi) {
      src_down_hd_.Process(in_, in_downsampled_, size);
      ProcessGranular(in_downsampled_, out_downsampled_, downsampled_size);
      src_up_hd_.Process(out_downsampled_, out_, downsampled_size);
    } else {
      src_down_.Process(in_, in_downsampled_,size);
      ProcessGranular(in_downsampled_, out_downsampled_, downsampled_size);
      src_up_.Process(out_downsampled_, out_, downsampled_size);
    }
  } else {
    ProcessGranular(in_, out_, size);
  }

  // Diffusion and pitch-shifting post-processings.
  if (playback_mode_ != PLAYBACK_MODE_SPECTRAL &&
      playback_mode_ != PLAYBACK_MODE_SPECTRAL_CLOUD &&
      playback_mode_ != PLAYBACK_MODE_OLIVERB &&
      playback_mode_ != PLAYBACK_MODE_RESONESTOR &&
      playback_mode_ != PLAYBACK_MODE_KAMMERL) {
    float texture = parameters_.texture;
    float diffusion = playback_mode_ == PLAYBACK_MODE_GRANULAR
        ? texture > 0.75f ? (texture - 0.75f) * 4.0f : 0.0f
        : parameters_.density;
    if (rate_.hi) {
      diffuser_hd_.set_amount(diffusion);
      diffuser_hd_.Process(out_, size);
    } else {
      diffuser_.set_amount(diffusion);
      diffuser_.Process(out_, size);
    }
  }

  if (((playback_mode_ == PLAYBACK_MODE_LOOPING_DELAY)
      && (!parameters_.freeze || looper_.synchronized()))
      || (playback_mode_ == PLAYBACK_MODE_SPECTRAL_CLOUD)) {
    if (rate_.hi) {
      RunPitchShifter(pitch_shifter_hd_, size);
    } else {
      RunPitchShifter(pitch_shifter_, size);
    }
  }

  // Apply filters.
  if (playback_mode_ == PLAYBACK_MODE_LOOPING_DELAY ||
      playback_mode_ == PLAYBACK_MODE_STRETCH) {
    float cutoff = parameters_.texture;
    float lp_cutoff = 0.5f * SemitonesToRatio(
        (cutoff < 0.5f ? cutoff - 0.5f : 0.0f) * 216.0f);
    float hp_cutoff = 0.25f * SemitonesToRatio(
        (cutoff < 0.5f ? -0.5f : cutoff - 1.0f) * 216.0f);
    CONSTRAIN(lp_cutoff, 0.0f, 0.499f);
    CONSTRAIN(hp_cutoff, 0.0f, 0.499f);
    // Same Hz at the engine rate (the original cutoffs are fractions of 32 kHz).
    lp_cutoff = rate_.Freq(lp_cutoff);
    hp_cutoff = rate_.Freq(hp_cutoff);

    lp_filter_[0].set_f_q<FREQUENCY_FAST>(lp_cutoff, 0.9f);
    lp_filter_[0].Process<FILTER_MODE_LOW_PASS>(
        &out_[0].l, &out_[0].l, size, 2);

    lp_filter_[1].set(lp_filter_[0]);
    lp_filter_[1].Process<FILTER_MODE_LOW_PASS>(
        &out_[0].r, &out_[0].r, size, 2);

    hp_filter_[0].set_f_q<FREQUENCY_FAST>(hp_cutoff, 0.9f);
    hp_filter_[0].Process<FILTER_MODE_HIGH_PASS>(
        &out_[0].l, &out_[0].l, size, 2);

    hp_filter_[1].set(hp_filter_[0]);
    hp_filter_[1].Process<FILTER_MODE_HIGH_PASS>(
        &out_[0].r, &out_[0].r, size, 2);
  }

  // SUPERCELL Added Pre-Reverb Muting.
  float mute_level_out;
  for (size_t i = 0; i < size; i++) {
      if (mute_out_) {
        //mute_level_in = 0.0f;
        mute_level_out = 0.0f;
      } else {
        mute_level_out = 1.0f;
        //mute_level_in = mute_in_ ? 0.0f : 1.0f;
      }
      mute_out_fade_ += mute_k_ * (mute_level_out - mute_out_fade_);
      //mute_in_fade_ += 0.01f * (mute_level_in - mute_in_fade_);
      out_[i].l = out_[i].l * mute_out_fade_;
      out_[i].r = out_[i].r * mute_out_fade_;
      //input[i].l = input[i].l * mute_in_fade_;
      //input[i].r = input[i].r * mute_in_fade_;
  }
  
  // This is what is fed back. Reverb is not fed back.
  copy(&out_[0], &out_[size], &fb_[0]);

  const float post_gain = 1.2f;

  if (playback_mode_ != PLAYBACK_MODE_RESONESTOR) {
    ParameterInterpolator dry_wet_mod(&dry_wet_, parameters_.dry_wet, size);
    for (size_t i = 0; i < size; ++i) {
      float dry_wet = dry_wet_mod.Next();
      if (playback_mode_ == PLAYBACK_MODE_KAMMERL) {
        dry_wet = 1.0f;
      }
      float fade_in = Interpolate(lut_xfade_in, dry_wet, 16.0f);
      float fade_out = Interpolate(lut_xfade_out, dry_wet, 16.0f);
      float l = static_cast<float>(input[i].l) / 32768.0f;
      float r = static_cast<float>(input[i].r) / 32768.0f;
      out_[i].l = l * fade_out + out_[i].l * post_gain * fade_in;
      out_[i].r = r * fade_out + out_[i].r * post_gain * fade_in;
    }
  }

  // Apply the simple post-processing reverb.
  if (playback_mode_ != PLAYBACK_MODE_OLIVERB &&
      playback_mode_ != PLAYBACK_MODE_RESONESTOR &&
      playback_mode_ != PLAYBACK_MODE_KAMMERL) {
    if (rate_.hi) {
      RunReverb(reverb_hd_, size, feedback);
    } else {
      RunReverb(reverb_, size, feedback);
    }
  }

  for (size_t i = 0; i < size; ++i) {
    if (playback_mode_ == PLAYBACK_MODE_SPECTRAL_CLOUD) {
	    WarmDistortion(&out_[i].l, parameters_.kammerl.pitch_mode);
	    WarmDistortion(&out_[i].r, parameters_.kammerl.pitch_mode);
    }

    output[i].l = SoftConvert(out_[i].l);
    output[i].r = SoftConvert(out_[i].r);
  }

  // TOC
}

void GranularProcessor::PreparePersistentData() {
  persistent_state_.write_head[0] = low_fidelity_ ?
      buffer_8_[0].head() : buffer_16_[0].head();
  persistent_state_.write_head[1] = low_fidelity_ ?
      buffer_8_[1].head() : buffer_16_[1].head();
  persistent_state_.quality = quality();
  if (playback_mode_ == PLAYBACK_MODE_SPECTRAL ||
      playback_mode_ == PLAYBACK_MODE_SPECTRAL_CLOUD)
    persistent_state_.spectral = playback_mode_;
  else
    persistent_state_.spectral = 0;
}

void GranularProcessor::GetPersistentData(
      PersistentBlock* block, size_t *num_blocks) {
  PersistentBlock* first_block = block;

  block->tag = FourCC<'S', 't', 'a', 't'>::value; // NOTE modified for Überclouds ('S' vs 's')
  block->data = &persistent_state_;
  block->size = sizeof(PersistentState);
  ++block;

  // Create save block holding the audio buffers.
  for (int32_t i = 0; i < num_channels_; ++i) {
    block->tag = FourCC<'b', 'u', 'f', 'f'>::value;
    block->data = buffer_[i];
    block->size = buffer_size_[num_channels_ - 1];
    ++block;
  }
  *num_blocks = block - first_block;
}

bool GranularProcessor::LoadPersistentData(const uint32_t* data) {
  // Force a silent output while the swapping of buffers takes place.
  silence_ = true;

  PersistentBlock block[4];
  size_t num_blocks;
  GetPersistentData(block, &num_blocks);

  for (size_t i = 0; i < num_blocks; ++i) {
    // Check that the format is correct.
    if (block[i].tag != data[0] || block[i].size != data[1]) {
      silence_ = false;
      return false;
    }

    // All good. Load the data. 2 words have already been used for the block tag
    // and the block size.
    data += 2;
    memcpy(block[i].data, data, block[i].size);
    data += block[i].size / sizeof(uint32_t);

    if (i == 0) {
      // We now know from which mode the data was saved.
      uint8_t currently_spectral =
          playback_mode_ == PLAYBACK_MODE_SPECTRAL ||
          playback_mode_ == PLAYBACK_MODE_SPECTRAL_CLOUD
          ? playback_mode_ : 0;
      uint8_t requires_spectral = persistent_state_.spectral;
      if (currently_spectral ^ requires_spectral) {
        set_playback_mode(requires_spectral
            ? static_cast<PlaybackMode>(requires_spectral)
            : PLAYBACK_MODE_GRANULAR);
      }
      set_quality(persistent_state_.quality);

      // We can force a switch to this mode, and once everything has been
      // initialized for this mode, we continue with the loop to copy the
      // actual buffer data - with all state variables correctly initialized.
      Prepare();
      GetPersistentData(block, &num_blocks);
    }
  }

  // We can finally reset the position of the write heads.
  if (low_fidelity_) {
    buffer_8_[0].Resync(persistent_state_.write_head[0]);
    buffer_8_[1].Resync(persistent_state_.write_head[1]);
  } else {
    buffer_16_[0].Resync(persistent_state_.write_head[0]);
    buffer_16_[1].Resync(persistent_state_.write_head[1]);
  }
  parameters_.freeze = true;
  silence_ = false;
  return true;
}

void GranularProcessor::Prepare() {
  bool playback_mode_changed = previous_playback_mode_ != playback_mode_;
  bool benign_change = playback_mode_ != PLAYBACK_MODE_SPECTRAL
    && previous_playback_mode_ != PLAYBACK_MODE_SPECTRAL
    && playback_mode_ != PLAYBACK_MODE_SPECTRAL_CLOUD
    && previous_playback_mode_ != PLAYBACK_MODE_SPECTRAL_CLOUD
    && playback_mode_ != PLAYBACK_MODE_RESONESTOR
    && previous_playback_mode_ != PLAYBACK_MODE_RESONESTOR
    && playback_mode_ != PLAYBACK_MODE_OLIVERB
    && previous_playback_mode_ != PLAYBACK_MODE_OLIVERB
    && previous_playback_mode_ != PLAYBACK_MODE_LAST;

  if (!reset_buffers_ && playback_mode_changed && benign_change) {
    ResetFilters();
    if (rate_.hi) {
      pitch_shifter_hd_.Clear();
    } else {
      pitch_shifter_.Clear();
    }
    previous_playback_mode_ = playback_mode_;
  }

  if ((playback_mode_changed && !benign_change) || reset_buffers_) {
    parameters_.freeze = false;
  }

  if (reset_buffers_ || (playback_mode_changed && !benign_change)) {
    void* buffer[2];
    size_t buffer_size[2];
    void* workspace;
    size_t workspace_size;
    if (num_channels_ == 1) {
      // Large buffer: 120k of sample memory.
      // small buffer: fully allocated to FX workspace.
      buffer[0] = buffer_[0];
      buffer_size[0] = buffer_size_[0];
      buffer[1] = NULL;
      buffer_size[1] = 0;
      workspace = buffer_[1];
      workspace_size = buffer_size_[1];
      if (sample_memory_mono_ && buffer_size[0] > sample_memory_mono_) {
        buffer_size[0] = sample_memory_mono_;   // HD: keeps the recording length in seconds the same as at 32 kHz
      }
    } else {
      // Large buffer: 64k of sample memory + FX workspace.
      // small buffer: 64k of sample memory.
      buffer_size[0] = buffer_size[1] = buffer_size_[1];
      buffer[0] = buffer_[0];
      buffer[1] = buffer_[1];

      workspace_size = buffer_size_[0] - buffer_size_[1];
      workspace = static_cast<uint8_t*>(buffer[0]) + buffer_size[0];
      if (sample_memory_stereo_ && buffer_size[0] > sample_memory_stereo_) {
        buffer_size[0] = buffer_size[1] = sample_memory_stereo_;
      }
    }
    if (rate_.hi && low_fidelity_) {
      // Low-fi samples are stored at 16 kHz whatever the engine rate, so 2/3 of the (1.5x) memory holds the same seconds.
      for (int32_t i = 0; i < num_channels_; ++i) {
        buffer_size[i] = (buffer_size[i] * 2 / 3) & ~static_cast<size_t>(7);
      }
    }
    // Layers: remember where the sample memory is (it is only audio in the modes that allocate AudioBuffers below).
    samples_valid_ = false;
    layer_rec_ = false;
    layer_rec_done_ = false;
    layer_rec_frames_ = 0;
    for (int32_t i = 0; i < 2; ++i) {
      region_ptr_[i] = (i < num_channels_) ? buffer[i] : NULL;
      region_bytes_[i] = (i < num_channels_) ? (buffer_size[i] & ~static_cast<size_t>(1)) : 0;
    }
    // The rate the stored audio, and the mode processing, run at (16 kHz in low-fi, otherwise the engine rate).
    const EngineRate& pr = proc_rate();
    // Rate handed to the spectral modes: only the 48 kHz hi-fi path uses the 6144-point FFT.
    float sr = (rate_.hi && !low_fidelity_) ? rate_.hz : sample_rate();

    BufferAllocator allocator(workspace, workspace_size);
    if (rate_.hi) {
      diffuser_hd_.Init(allocator.Allocate<float>(4096));
    } else {
      diffuser_.Init(allocator.Allocate<float>(2048));
    }

    uint16_t* reverb_buffer = allocator.Allocate<uint16_t>(rate_.hi ? 32768 : 16384);
    if (playback_mode_ == PLAYBACK_MODE_OLIVERB) {
      if (pr.hi) {
        oliverb_hd_.Init(reverb_buffer);
      } else {
        oliverb_.Init(reverb_buffer);
      }
    } else {
      if (rate_.hi) {
        reverb_hd_.Init(reverb_buffer);
      } else {
        reverb_.Init(reverb_buffer);
      }
    }

    size_t correlator_block_size = (pr.N(kMaxWSOLASize) / 32) + 2;
    uint32_t* correlator_data = allocator.Allocate<uint32_t>(
        correlator_block_size * 3);
    correlator_.Init(
        &correlator_data[0],
        &correlator_data[correlator_block_size]);
    if (rate_.hi) {
      pitch_shifter_hd_.Init((uint16_t*)correlator_data);
    } else {
      pitch_shifter_.Init((uint16_t*)correlator_data);
    }

    if (playback_mode_ == PLAYBACK_MODE_SPECTRAL) {
      phase_vocoder_.Init(
          PhaseVocoder::TRANSFORMATION_TYPE_FRAME,
          buffer, buffer_size,
          lut_sine_window_4096, 4096,
          num_channels_, resolution(), sr);
    } else if (playback_mode_ == PLAYBACK_MODE_SPECTRAL_CLOUD) {
      phase_vocoder_.Init(
          PhaseVocoder::TRANSFORMATION_TYPE_SPECTRAL_CLOUD,
          buffer, buffer_size,
          lut_sine_window_4096, 4096,
          num_channels_, resolution(), sr);
    } else if (playback_mode_ == PLAYBACK_MODE_RESONESTOR) {
      float* buf = (float*)buffer[0];
      if (pr.hi) {
        resonestor_hd_.Init(buf);
      } else {
        resonestor_.Init(buf);
      }
    } else {
      for (int32_t i = 0; i < num_channels_; ++i) {
        if (resolution() == 8) {
          buffer_8_[i].Init(
              buffer[i],
              (buffer_size[i]),
              tail_buffer_[i],
              pr.N(kCrossFadeSize));
        } else {
          buffer_16_[i].Init(
              buffer[i],
              ((buffer_size[i]) >> 1),
              tail_buffer_[i],
              pr.N(kCrossFadeSize));
        }
      }
      int32_t num_grains = (num_channels_ == 1 ? 32 : 26) * \
          (low_fidelity_ ? 20 : 16) >> 4;
      player_.Init(num_channels_, num_grains, pr);
      ws_player_.Init(&correlator_, num_channels_, pr);
      looper_.Init(num_channels_, pr);
      kammerl_.Init(num_channels_);
      samples_valid_ = true;
    }
    ++buffers_generation_;
    reset_buffers_ = false;
    previous_playback_mode_ = playback_mode_;
  }

  if (playback_mode_ == PLAYBACK_MODE_SPECTRAL ||
      playback_mode_ == PLAYBACK_MODE_SPECTRAL_CLOUD) {
    phase_vocoder_.Buffer();
  } else if (playback_mode_ == PLAYBACK_MODE_STRETCH ||
             playback_mode_ == PLAYBACK_MODE_OLIVERB) {
    if (resolution() == 8) {
      ws_player_.LoadCorrelator(buffer_8_);
    } else {
      ws_player_.LoadCorrelator(buffer_16_);
    }
    correlator_.EvaluateSomeCandidates();
  }
}

// ---- Unofficial SuperParasites Layers ----------------------------------------------------------------------------

bool GranularProcessor::sample_region(int32_t ch, void** data, size_t* bytes) const {
  if (!samples_valid_ || ch < 0 || ch >= num_channels_ || region_ptr_[ch] == NULL) return false;
  *data = region_ptr_[ch];
  *bytes = region_bytes_[ch];
  return true;
}

int32_t GranularProcessor::write_head(int32_t ch) const {
  if (ch < 0 || ch > 1) return 0;
  return low_fidelity_ ? buffer_8_[ch].head() : buffer_16_[ch].head();
}

void GranularProcessor::ResyncWriteHeads(int32_t head0, int32_t head1) {
  if (!samples_valid_) return;
  if (low_fidelity_) {
    buffer_8_[0].Resync(head0);
    buffer_8_[1].Resync(head1);
  } else {
    buffer_16_[0].Resync(head0);
    buffer_16_[1].Resync(head1);
  }
}

void GranularProcessor::ClearSamples() {
  if (!samples_valid_) return;
  for (int32_t i = 0; i < num_channels_; ++i) {
    if (region_ptr_[i] == NULL) continue;
    // Same silence the buffers start with: 0 for 16-bit, 127 for the mu-law low-fi samples.
    memset(region_ptr_[i], low_fidelity_ ? 127 : 0, region_bytes_[i]);
  }
  ResyncWriteHeads(0, 0);
}

void GranularProcessor::BeginLayerRecording() {
  if (!samples_valid_) return;
  ClearSamples();
  layer_rec_frames_ = 0;
  layer_rec_done_ = false;
  layer_rec_ = true;
}

void GranularProcessor::EndLayerRecording() {
  layer_rec_ = false;
}

void GranularProcessor::GetPositionView(PositionView* v) const {
  *v = PositionView();
  if (reset_buffers_ || previous_playback_mode_ != playback_mode_ || !samples_valid_) {
    v->hidden = VIEW_REBUILDING;
    if (playback_mode_ == PLAYBACK_MODE_SPECTRAL || playback_mode_ == PLAYBACK_MODE_SPECTRAL_CLOUD ||
        playback_mode_ == PLAYBACK_MODE_RESONESTOR) {
      v->hidden = VIEW_NO_BUFFER;
    }
    return;
  }
  if (playback_mode_ == PLAYBACK_MODE_OLIVERB) {
    v->hidden = VIEW_NOT_A_TIME;   // Position is the reverb's pre-delay here
    return;
  }
  v->hidden = VIEW_SHOWN;
  v->channels = num_channels_;
  v->low_fi = low_fidelity_;
  v->ring_frames = low_fidelity_ ? buffer_8_[0].size() : buffer_16_[0].size();
  v->head = low_fidelity_ ? buffer_8_[0].head() : buffer_16_[0].head();
  // The same rule as ProcessGranular(): is the incoming audio being written into the buffer right now?
  bool writing = !parameters_.freeze || playback_mode_ == PLAYBACK_MODE_KAMMERL;
  if (hold_writes_) writing = false;
  if (layer_rec_) writing = true;
  v->writing = writing;
  switch (playback_mode_) {
    case PLAYBACK_MODE_GRANULAR:
      player_.Describe(parameters_, v->ring_frames, v);
      break;
    case PLAYBACK_MODE_STRETCH:
      ws_player_.Describe(v->ring_frames, v);
      break;
    case PLAYBACK_MODE_LOOPING_DELAY:
      looper_.Describe(parameters_, v->ring_frames, v);
      break;
    case PLAYBACK_MODE_KAMMERL:
      kammerl_.Describe(v->ring_frames, v->head, v);
      break;
    default:
      v->hidden = VIEW_NO_BUFFER;
      break;
  }
}

}  // namespace clouds
