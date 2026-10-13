// Unofficial SuperParasites HD: a real FFT of size 3 * 2^k (6144 = 3 * 2048), for the spectral modes at 48 kHz.
//
// Why it exists: at 32 kHz the spectral modes use a 4096-point FFT (128 ms window, 32 ms hop, 7.8125 Hz between bins).
// To get the same window length in seconds and the same spacing between bins at 48 kHz the FFT has to be 1.5 times
// longer, 6144 points, and the original FFT (ShyFFT) only does powers of two. This one splits the signal into three
// interleaved parts, runs a radix-2 FFT on each, and combines them with a radix-3 step. It is written for clarity, not
// speed (a 6144-point transform takes well under a millisecond, and one is needed every 32 ms per channel).
//
// It reproduces ShyFFT's conventions exactly (checked against ShyFFT at 4096 in hd_fft_test):
//   Direct():  out[0] = DC, out[N/2] = Nyquist, out[k] = Re X[k], out[N/2 + k] = -Im X[k]   (k = 1 .. N/2 - 1),
//              where X[k] = sum x[n] exp(-2 pi i n k / N).
//   Inverse(): takes that layout, returns N * x (not divided by N).
// Unlike ShyFFT it does not use its input as scratch space; the input is left alone.
#ifndef CLOUDS_DSP_PVOC_HD_FFT_H_
#define CLOUDS_DSP_PVOC_HD_FFT_H_

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace clouds {

class HdFft {
 public:
  static constexpr size_t kMaxSize = 6144;   // 3 * 2^11

  HdFft() : size_(0) { }
  ~HdFft() { }

  // n must be 3 * 2^k with 4 <= n <= kMaxSize. No allocation; safe to call again.
  bool Init(size_t n) {
    if (n == size_ && n != 0) return true;
    if (n < 12 || n > kMaxSize || n % 3 != 0) return false;
    const size_t m = n / 3;
    if ((m & (m - 1)) != 0) return false;
    size_ = n;
    third_ = m;
    const double pi = 3.14159265358979323846;
    for (size_t k = 0; k < n; ++k) {
      const double a = -2.0 * pi * static_cast<double>(k) / static_cast<double>(n);
      tw_re_[k] = static_cast<float>(std::cos(a));
      tw_im_[k] = static_cast<float>(std::sin(a));
    }
    // bit reversal table for the radix-2 part
    size_t bits = 0;
    while ((static_cast<size_t>(1) << bits) < m) ++bits;
    for (size_t i = 0; i < m; ++i) {
      size_t r = 0;
      for (size_t b = 0; b < bits; ++b)
        if (i & (static_cast<size_t>(1) << b)) r |= static_cast<size_t>(1) << (bits - 1 - b);
      bitrev_[i] = static_cast<uint16_t>(r);
    }
    return true;
  }

  size_t size() const { return size_; }

  // in[0..N): real signal. out[0..N): packed spectrum (see top of file).
  void Direct(const float* in, float* out) {
    const size_t n = size_;
    for (size_t i = 0; i < n; ++i) { re_[i] = in[i]; im_[i] = 0.0f; }
    Transform(false);
    out[0] = re_[0];
    out[n / 2] = re_[n / 2];
    for (size_t k = 1; k < n / 2; ++k) {
      out[k] = re_[k];
      out[n / 2 + k] = -im_[k];
    }
  }

  // in[0..N): packed spectrum. out[0..N): N times the original signal.
  void Inverse(const float* in, float* out) {
    const size_t n = size_;
    re_[0] = in[0]; im_[0] = 0.0f;
    re_[n / 2] = in[n / 2]; im_[n / 2] = 0.0f;
    for (size_t k = 1; k < n / 2; ++k) {
      re_[k] = in[k];
      im_[k] = -in[n / 2 + k];
      re_[n - k] = in[k];
      im_[n - k] = in[n / 2 + k];   // complex conjugate
    }
    Transform(true);
    for (size_t i = 0; i < n; ++i) out[i] = re_[i];
  }

 private:
  // In-place complex DFT of (re_, im_), N points. inverse = true uses exp(+i ...) and no scaling.
  void Transform(bool inverse) {
    const size_t n = size_;
    const size_t m = third_;
    const float sign = inverse ? -1.0f : 1.0f;   // twiddle imaginary parts are stored for the forward direction

    // 1. three interleaved sub-sequences x[3j + r], each transformed with a radix-2 FFT of size m.
    for (size_t r = 0; r < 3; ++r) {
      float* sr = sub_re_[r];
      float* si = sub_im_[r];
      for (size_t j = 0; j < m; ++j) {
        const size_t src = 3 * j + r;
        const size_t dst = bitrev_[j];
        sr[dst] = re_[src];
        si[dst] = im_[src];
      }
      for (size_t len = 2; len <= m; len <<= 1) {
        const size_t half = len >> 1;
        const size_t step = n / len;                 // twiddle for e^{-2 pi i t / len} is tw[t * n / len]
        for (size_t start = 0; start < m; start += len) {
          for (size_t t = 0; t < half; ++t) {
            const float wr = tw_re_[t * step];
            const float wi = sign * tw_im_[t * step];
            const size_t a = start + t;
            const size_t b = a + half;
            const float xr = sr[b] * wr - si[b] * wi;
            const float xi = sr[b] * wi + si[b] * wr;
            sr[b] = sr[a] - xr;
            si[b] = si[a] - xi;
            sr[a] += xr;
            si[a] += xi;
          }
        }
      }
    }

    // 2. radix-3 combine: X[k] = Y0[k mod m] + W^k Y1[k mod m] + W^{2k} Y2[k mod m], with W = exp(-2 pi i / n).
    for (size_t k = 0; k < n; ++k) {
      const size_t j = k >= 2 * m ? k - 2 * m : (k >= m ? k - m : k);
      const size_t k1 = k;
      const size_t k2 = (2 * k) % n;
      const float w1r = tw_re_[k1], w1i = sign * tw_im_[k1];
      const float w2r = tw_re_[k2], w2i = sign * tw_im_[k2];
      const float y1r = sub_re_[1][j], y1i = sub_im_[1][j];
      const float y2r = sub_re_[2][j], y2i = sub_im_[2][j];
      re_[k] = sub_re_[0][j] + (y1r * w1r - y1i * w1i) + (y2r * w2r - y2i * w2i);
      im_[k] = sub_im_[0][j] + (y1r * w1i + y1i * w1r) + (y2r * w2i + y2i * w2r);
    }
  }

  size_t size_;
  size_t third_;
  float tw_re_[kMaxSize];
  float tw_im_[kMaxSize];
  uint16_t bitrev_[kMaxSize / 3];
  float re_[kMaxSize];
  float im_[kMaxSize];
  float sub_re_[3][kMaxSize / 3];
  float sub_im_[3][kMaxSize / 3];

  HdFft(const HdFft&);
  HdFft& operator=(const HdFft&);
};

}  // namespace clouds

#endif  // CLOUDS_DSP_PVOC_HD_FFT_H_
