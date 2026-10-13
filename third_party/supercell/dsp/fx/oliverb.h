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
// Reverb (used in the dedicated "Oliverb" playback mode).

#ifndef CLOUDS_DSP_FX_OLIVERB_H_
#define CLOUDS_DSP_FX_OLIVERB_H_

#include "stmlib/stmlib.h"

#include "supercell/dsp/engine_rate.h"
#include "supercell/dsp/fx/fx_engine.h"
#include "supercell/dsp/random_oscillator.h"

namespace clouds {

// HD: HI = 1 has every delay length, offset and window x 1.5 (same times in seconds); the per-sample smoothing
// and filter coefficients are mapped to the same decay per second, and the random LFOs run at the same Hz.
template<int HI = 0>
class Oliverb {
 public:
  typedef Scale<HI> R;
  Oliverb() { }
  ~Oliverb() { }

  void Init(uint16_t* buffer) {
    engine_.Init(buffer, R::kBlock);
    diffusion_ = 0.625f;
    size_ = 1.0f;
    mod_amount_ = 0.0f;
    mod_rate_ = 0.0f;
    size_ = 0.5f;
    input_gain_ = 1.0f;
    decay_ = 0.5f;
    lp_ = R::Pole(1.0f);
    hp_= R::Pole(0.0f);
    phase_ = 0.0f;
    ratio_ = 0.0f;
    pitch_shift_amount_ = 1.0f;
    level_ = 0.0f;
    for (int i=0; i<9; i++)
      lfo_[i].Init(R::kHi ? 2.0f / 3.0f : 1.0f);
  }

  void Process(FloatFrame* in_out, size_t size) {
    // This is the Griesinger topology described in the Dattorro paper
    // (4 AP diffusers on the input, then a loop of 2x 2AP+1Delay).
    // Modulation is applied in the loop of the first diffuser AP for additional
    // smearing; and to the two long delays for a slow shimmer/chorus effect.
    typedef typename E::template Reserve<R::N(113),     /* ap1 */
      typename E::template Reserve<R::N(162),           /* ap2 */
      typename E::template Reserve<R::N(241),           /* ap3 */
      typename E::template Reserve<R::N(399),           /* ap4 */
      typename E::template Reserve<R::N(1253),          /* dap1a */
      typename E::template Reserve<R::N(1738),          /* dap1b */
      typename E::template Reserve<R::N(3411),          /* del1 */
      typename E::template Reserve<R::N(1513),          /* dap2a */
      typename E::template Reserve<R::N(1363),          /* dap2b */
      typename E::template Reserve<R::N(4782)> > > > > > > > > > Memory; /* del2 */
    typename E::template DelayLine<Memory, 0> ap1;
    typename E::template DelayLine<Memory, 1> ap2;
    typename E::template DelayLine<Memory, 2> ap3;
    typename E::template DelayLine<Memory, 3> ap4;
    typename E::template DelayLine<Memory, 4> dap1a;
    typename E::template DelayLine<Memory, 5> dap1b;
    typename E::template DelayLine<Memory, 6> del1;
    typename E::template DelayLine<Memory, 7> dap2a;
    typename E::template DelayLine<Memory, 8> dap2b;
    typename E::template DelayLine<Memory, 9> del2;
    typename E::Context c;

    const float kap = diffusion_;

    float lp_1 = lp_decay_1_;
    float lp_2 = lp_decay_2_;
    float hp_1 = hp_decay_1_;
    float hp_2 = hp_decay_2_;

    /* Smoothing of the room size, once per sample: same decay per second at either rate. */
    const float smooth_coefficient = R::Pole(0.01f);

    /* Set frequency of LFOs */
    float slope = mod_rate_ * mod_rate_;
    slope *= slope * slope;
    slope /= 200.0f;
    for (int i=0; i<9; i++)
      lfo_[i].set_slope(slope);

    while (size--) {
      engine_.Start(&c);

      // Smooth parameters to avoid delay glitches
      ONE_POLE(smooth_size_, size_, smooth_coefficient);

      // compute windowing info for the pitch shifter
      float ps_size = R::F(128.0f) + (R::F(3410.0f) - R::F(128.0f)) * smooth_size_;
      phase_ += (1.0f - ratio_) / ps_size;
      if (phase_ >= 1.0f) phase_ -= 1.0f;
      if (phase_ <= 0.0f) phase_ += 1.0f;
      float tri = 2.0f * (phase_ >= 0.5f ? 1.0f - phase_ : phase_);
      tri = Interpolate(lut_window, tri, LUT_WINDOW_SIZE-1);
      float phase = phase_ * ps_size;
      float half = phase + ps_size * 0.5f;
      if (half >= ps_size) half -= ps_size;

#define INTERPOLATE_LFO(del, lfo, gain)                                 \
      {                                                                 \
        float offset = (del.length - 1) * smooth_size_;                 \
        offset += lfo.Next() * mod_amount_;                      \
        CONSTRAIN(offset, 1.0f, del.length - 1);                        \
        c.InterpolateHermite(del, offset, gain);                        \
      }

#define INTERPOLATE(del, gain)                                          \
      {                                                                 \
        float offset = (del.length - 1) * smooth_size_;                 \
        CONSTRAIN(offset, 1.0f, del.length - 1);                        \
        c.InterpolateHermite(del, offset, gain);                        \
      }

      // Smear AP1 inside the loop.
      c.Interpolate(ap1, R::F(10.0f), LFO_1, R::F(60.0f), 1.0f);
      c.Write(ap1, R::N(100), 0.0f);

      c.Read(in_out->l + in_out->r, input_gain_);
      // Diffuse through 4 allpasses.
      INTERPOLATE_LFO(ap1, lfo_[1], kap);
      c.WriteAllPass(ap1, -kap);
      INTERPOLATE_LFO(ap2, lfo_[2], kap);
      c.WriteAllPass(ap2, -kap);
      INTERPOLATE_LFO(ap3, lfo_[3], kap);
      c.WriteAllPass(ap3, -kap);
      INTERPOLATE_LFO(ap4, lfo_[4], kap);
      c.WriteAllPass(ap4, -kap);

      float apout;
      c.Write(apout);

      INTERPOLATE_LFO(del2, lfo_[5], decay_ * (1.0f - pitch_shift_amount_));
      /* blend in the pitch shifted feedback */
      c.InterpolateHermite(del2, phase, tri * decay_ * pitch_shift_amount_);
      c.InterpolateHermite(del2, half, (1.0f - tri) * decay_ * pitch_shift_amount_);

      c.Lp(lp_1, lp_);
      c.Hp(hp_1, hp_);
      c.SoftLimit();
      INTERPOLATE_LFO(dap1a, lfo_[6], -kap);
      c.WriteAllPass(dap1a, kap);
      INTERPOLATE(dap1b, kap);
      c.WriteAllPass(dap1b, -kap);
      c.Write(del1, 2.0f);
      c.Write(in_out->l, 0.0f);

      c.Load(apout);

      INTERPOLATE_LFO(del1, lfo_[7], decay_ * (1.0f - pitch_shift_amount_));
      /* blend in the pitch shifted feedback */
      c.InterpolateHermite(del1, phase, tri * decay_ * pitch_shift_amount_);
      c.InterpolateHermite(del1, half, (1.0f - tri) * decay_ * pitch_shift_amount_);
      c.Lp(lp_2, lp_);
      c.Hp(hp_2, hp_);
      c.SoftLimit();
      INTERPOLATE_LFO(dap2a, lfo_[8], kap);
      c.WriteAllPass(dap2a, -kap);
      INTERPOLATE(dap2b, -kap);
      c.WriteAllPass(dap2b, kap);
      c.Write(del2, 2.0f);
      c.Write(in_out->r, 0.0f);

      ++in_out;
    }

    lp_decay_1_ = lp_1;
    lp_decay_2_ = lp_2;
    hp_decay_1_ = hp_1;
    hp_decay_2_ = hp_2;
  }

  inline void set_input_gain(float input_gain) {
    input_gain_ = input_gain;
  }

  inline void set_decay(float decay) {
    decay_ = decay;
  }

  inline void set_diffusion(float diffusion) {
    diffusion_ = diffusion;
  }

  inline void set_lp(float lp) {
    lp_ = R::Pole(lp);   // per-sample one-pole coefficient
  }

  inline void set_hp(float hp) {
    hp_ = R::Pole(hp);
  }

  inline void set_size(float size) {
    size_ = size;
  }

  inline void set_mod_amount(float mod_amount) {
    mod_amount_ = R::F(mod_amount);   // a depth in samples
  }

  inline void set_mod_rate(float mod_rate) {
    mod_rate_ = mod_rate;
  }

  inline void set_ratio(float ratio) {
    ratio_ = ratio;
  }

  inline void set_pitch_shift_amount(float pitch_shift) {
    pitch_shift_amount_ = pitch_shift;
  }

 private:
  typedef FxEngine<HI ? 32768 : 16384, FORMAT_16_BIT> E;
  E engine_;

  float input_gain_;
  float decay_;
  float diffusion_;
  float lp_;
  float hp_;
  float size_, smooth_size_;
  float mod_amount_;
  float mod_rate_;
  float pitch_shift_amount_;

  float lp_decay_1_;
  float lp_decay_2_;
  float hp_decay_1_;
  float hp_decay_2_;

  float phase_;
  float ratio_;
  float level_;

  RandomOscillator lfo_[9];

  DISALLOW_COPY_AND_ASSIGN(Oliverb);
};

}  // namespace clouds

#endif  // CLOUDS_DSP_FX_OLIVERB_H_
