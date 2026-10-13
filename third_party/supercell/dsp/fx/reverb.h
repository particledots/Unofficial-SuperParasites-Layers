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
// Reverb.

#ifndef CLOUDS_DSP_FX_REVERB_H_
#define CLOUDS_DSP_FX_REVERB_H_

#include "stmlib/stmlib.h"

#include "supercell/dsp/engine_rate.h"
#include "supercell/dsp/fx/fx_engine.h"

namespace clouds {

// HD: HI = 0 is the original at 32 kHz; HI = 1 has every delay length and offset x 1.5 (same times in seconds) and the
// one-pole filter coefficient mapped to the same decay per second.
template<int HI = 0>
class Reverb {
 public:
  typedef Scale<HI> R;
  Reverb() { }
  ~Reverb() { }

  void Init(uint16_t* buffer) {
    engine_.Init(buffer, R::kBlock);
    engine_.SetLFOFrequency(LFO_1, 0.5f / R::kRate);
    engine_.SetLFOFrequency(LFO_2, 0.3f / R::kRate);
    lp_ = R::Pole(0.7f);
    diffusion_ = 0.625f;
  }

  void Process(FloatFrame* in_out, size_t size) {
    // This is the Griesinger topology described in the Dattorro paper
    // (4 AP diffusers on the input, then a loop of 2x 2AP+1Delay).
    // Modulation is applied in the loop of the first diffuser AP for additional
    // smearing; and to the two long delays for a slow shimmer/chorus effect.
    typedef typename E::template Reserve<R::N(113),
      typename E::template Reserve<R::N(162),
      typename E::template Reserve<R::N(241),
      typename E::template Reserve<R::N(399),
      typename E::template Reserve<R::N(1653),
      typename E::template Reserve<R::N(2038),
      typename E::template Reserve<R::N(3411),
      typename E::template Reserve<R::N(1913),
      typename E::template Reserve<R::N(1663),
      typename E::template Reserve<R::N(4782)> > > > > > > > > > Memory;
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
    const float klp = lp_;
    const float krt = reverb_time_;
    const float amount = amount_;
    const float gain = input_gain_;

    float lp_1 = lp_decay_1_;
    float lp_2 = lp_decay_2_;

    while (size--) {
      float wet;
      float apout = 0.0f;
      engine_.Start(&c);

      // Smear AP1 inside the loop.
      c.Interpolate(ap1, R::F(10.0f), LFO_1, R::F(60.0f), 1.0f);
      c.Write(ap1, R::N(100), 0.0f);

      c.Read(in_out->l + in_out->r, gain);

      // Diffuse through 4 allpasses.
      c.Read(ap1 TAIL, kap);
      c.WriteAllPass(ap1, -kap);
      c.Read(ap2 TAIL, kap);
      c.WriteAllPass(ap2, -kap);
      c.Read(ap3 TAIL, kap);
      c.WriteAllPass(ap3, -kap);
      c.Read(ap4 TAIL, kap);
      c.WriteAllPass(ap4, -kap);
      c.Write(apout);

      // Main reverb loop.
      c.Load(apout);
      c.Interpolate(del2, R::F(4680.0f), LFO_2, R::F(100.0f), krt);
      c.Lp(lp_1, klp);
      c.Read(dap1a TAIL, -kap);
      c.WriteAllPass(dap1a, kap);
      c.Read(dap1b TAIL, kap);
      c.WriteAllPass(dap1b, -kap);
      c.Write(del1, 2.0f);
      c.Write(wet, 0.0f);

      in_out->l += (wet - in_out->l) * amount;

      c.Load(apout);
      // c.Interpolate(del1, R::F(4450.0f), LFO_1, R::F(50.0f), krt);
      c.Read(del1 TAIL, krt);
      c.Lp(lp_2, klp);
      c.Read(dap2a TAIL, kap);
      c.WriteAllPass(dap2a, -kap);
      c.Read(dap2b TAIL, -kap);
      c.WriteAllPass(dap2b, kap);
      c.Write(del2, 2.0f);
      c.Write(wet, 0.0f);

      in_out->r += (wet - in_out->r) * amount;

      ++in_out;
    }

    lp_decay_1_ = lp_1;
    lp_decay_2_ = lp_2;
  }

  inline void set_amount(float amount) {
    amount_ = amount;
  }

  inline void set_input_gain(float input_gain) {
    input_gain_ = input_gain;
  }

  inline void set_time(float reverb_time) {
    reverb_time_ = reverb_time;
  }

  inline void set_diffusion(float diffusion) {
    diffusion_ = diffusion;
  }

  inline void set_lp(float lp) {
    lp_ = R::Pole(lp);   // per-sample one-pole coefficient
  }

 private:
  typedef FxEngine<HI ? 32768 : 16384, FORMAT_12_BIT> E;
  E engine_;

  float amount_;
  float input_gain_;
  float reverb_time_;
  float diffusion_;
  float lp_;

  float lp_decay_1_;
  float lp_decay_2_;

  DISALLOW_COPY_AND_ASSIGN(Reverb);
};

}  // namespace clouds

#endif  // CLOUDS_DSP_FX_REVERB_H_
