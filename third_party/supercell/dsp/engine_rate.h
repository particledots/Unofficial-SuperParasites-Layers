// Unofficial SuperParasites HD: the DSP can run at the module's 32 kHz or natively at 48 kHz.
//
// Everything in the original code is written in "samples at 32 kHz": delay lengths, window sizes, one-pole smoothing
// coefficients, filter cutoffs given as a fraction of the sample rate, and so on. This header holds the few rules used
// to turn those into the same *seconds* and the same *Hz* at 48 kHz. At 32 kHz every rule returns its input unchanged,
// so the 32 kHz path is the original code.
//
//   samples (delay lengths, window sizes, counters)   x 1.5            Scale<HI>::N / ::F, EngineRate::N / ::F
//   cutoff given as cycles per sample                 x 32/48          EngineRate::Freq
//   one-pole coefficient applied every sample         same pole decay  Scale<HI>::Pole, EngineRate::Pole
//                                                     per second       (pole is raised to the power 32/48)
//
// Two flavours exist because the delay-line effects (reverb, diffuser, Oliverb, Resonestor, pitch shifter) need
// their delay lengths at compile time (template parameter HI: 0 = 32 kHz, 1 = 48 kHz), while everything else
// reads a run-time EngineRate.
#ifndef CLOUDS_DSP_ENGINE_RATE_H_
#define CLOUDS_DSP_ENGINE_RATE_H_

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace clouds {

constexpr float kBaseSampleRate = 32000.0f;
constexpr float kHdSampleRate = 48000.0f;

// Per-sample one-pole coefficient k (state += k * (input - state)) that decays at the same rate per second when the
// sample rate goes from 32 kHz to `hz`:  1 - (1 - k)^(32000 / hz).  Computed in double, because k can be 1e-5.
inline float PoleForRate(float k, float hz) {
  if (hz == kBaseSampleRate) return k;
  if (k <= 0.0f) return 0.0f;
  if (k >= 1.0f) return 1.0f;
  const double p = 1.0 - static_cast<double>(k);
  return static_cast<float>(-std::expm1(std::log(p) * (static_cast<double>(kBaseSampleRate) / hz)));
}

// Compile-time flavour, for delay lengths.
template<int HI>
struct Scale {
  static constexpr bool kHi = HI != 0;
  static constexpr float kRate = HI ? kHdSampleRate : kBaseSampleRate;
  static constexpr int32_t kBlock = HI ? 48 : 32;       // 1 ms
  // A count of samples: x 1.5, rounded to the nearest sample (halves round up).
  static constexpr int32_t N(int32_t n) { return HI ? (n * 3 + 1) / 2 : n; }
  static constexpr float F(float x) { return HI ? x * 1.5f : x; }
  // A cutoff given in cycles per sample at 32 kHz -> the same Hz at this rate.
  static constexpr float Freq(float f) { return HI ? f * (2.0f / 3.0f) : f; }
  static float Pole(float k) { return PoleForRate(k, kRate); }
};

// Run-time flavour.
struct EngineRate {
  float hz = kBaseSampleRate;
  float scale = 1.0f;     // hz / 32000
  float inv = 1.0f;       // 32000 / hz
  int32_t block = 32;     // frames per processing block, always 1 ms
  int32_t down = 2;       // low-fi decimation factor; gives 16 kHz
  bool hi = false;

  EngineRate() { }
  explicit EngineRate(bool high) { Set(high); }

  void Set(bool high) {
    hi = high;
    hz = high ? kHdSampleRate : kBaseSampleRate;
    scale = high ? 1.5f : 1.0f;
    inv = high ? (2.0f / 3.0f) : 1.0f;
    block = high ? 48 : 32;
    down = high ? 3 : 2;
  }

  int32_t N(int32_t n) const { return hi ? (n * 3 + 1) / 2 : n; }
  float F(float x) const { return hi ? x * 1.5f : x; }
  float Freq(float f) const { return hi ? f * (2.0f / 3.0f) : f; }
  float Pole(float k) const { return PoleForRate(k, hz); }
};

}  // namespace clouds

#endif  // CLOUDS_DSP_ENGINE_RATE_H_
