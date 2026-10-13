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
// Naive phase vocoder.

#include "supercell/dsp/pvoc/phase_vocoder.h"

#include <algorithm>
#include <cmath>

#include "stmlib/utils/buffer_allocator.h"

#include "supercell/resources.h"

namespace clouds {

using namespace std;
using namespace stmlib;

// The analysis/synthesis window for the 6144-point FFT, derived from the 4096-point table of the original:
//  1. the original table is resampled to 6144 points (4-point cubic interpolation, the window is periodic), so the
//     window has the same shape over the same length in seconds;
//  2. each pair (j, j + 3072) is rescaled so that w(j)^2 + w(j + 3072)^2 == 1 exactly, the condition the original
//     table satisfies at 4096 points (hop = N/4, so overlap-added frames then sum to a constant).
// Built once, the first time it is needed (thread safe), then shared and read-only.
const float* HdWindow6144() {
  struct Table {
    float w[6144];
    Table() {
      const int n_src = LUT_SINE_WINDOW_4096_SIZE;
      const int n = 6144;
      double tmp[6144];
      for (int j = 0; j < n; ++j) {
        const double pos = static_cast<double>(j) * n_src / n;
        const int i1 = static_cast<int>(pos);
        const double t = pos - i1;
        const double p0 = lut_sine_window_4096[(i1 + n_src - 1) % n_src];
        const double p1 = lut_sine_window_4096[i1 % n_src];
        const double p2 = lut_sine_window_4096[(i1 + 1) % n_src];
        const double p3 = lut_sine_window_4096[(i1 + 2) % n_src];
        tmp[j] = p1 + 0.5 * t * (p2 - p0 + t * (2.0 * p0 - 5.0 * p1 + 4.0 * p2 - p3 + t * (3.0 * (p1 - p2) + p3 - p0)));
      }
      for (int j = 0; j < n / 2; ++j) {
        const double a = tmp[j], b = tmp[j + n / 2];
        const double norm = 1.0 / std::sqrt(a * a + b * b);
        w[j] = static_cast<float>(a * norm);
        w[j + n / 2] = static_cast<float>(b * norm);
      }
    }
  };
  static const Table table;
  return table.w;
}

void PhaseVocoder::Init() {
  // Le grand hack: To support both modes, Modifier is implemented with virtual
  // functions. However since we're running without C++ init_array sections in
  // the linker script, and no constructors are called (__libc_init_array), and
  // since this is part of static object we have to construct these ourselves
  // otherwise the vtables are 0, aka "Not Good" :[]
  // - Placement new _should_ be a NOP/inline function anyway.
  // - Alternatively, implement some thunks or similar.
  // - Using the assignment operator (frame_transformation_[0] = FrameTransformation()
  //   has some side effects, it breaks the DISALLOW_COPY_AND_ASSIGN as well as
  // throwing warnings because of uninitialized member variables, plus it actually
  // calls the destructors on the half-initialized objects.
  //
  // On the plus side, we could save some RAM and just instantiate both types
  // in shared block of memory.
  new(&frame_transformation_[0]) FrameTransformation();
  new(&frame_transformation_[1]) FrameTransformation();

  new(&spectral_clouds_transformation_[0]) SpectralCloudsTransformation();
  new(&spectral_clouds_transformation_[1]) SpectralCloudsTransformation();
}

void PhaseVocoder::Init(
    TransformationType transformation_type,
    void** buffer,
    size_t* buffer_size,
    const float* large_window_lut,
    size_t largest_fft_size,
    int32_t num_channels,
    int32_t resolution,
    float sample_rate) {
  num_channels_ = num_channels;

  const bool hd = sample_rate > 32500.0f;
  size_t fft_size = hd ? 6144 : largest_fft_size;
  size_t hop_ratio = 4;
  const float* window_lut = hd ? HdWindow6144() : large_window_lut;
  const size_t window_lut_size = hd ? 6144 : LUT_SINE_WINDOW_4096_SIZE;
  if (hd) {
    fft_hd_.Init(fft_size);
  }
  
  BufferAllocator allocator_0(buffer[0], buffer_size[0]);
  BufferAllocator allocator_1(buffer[1], buffer_size[1]);
  BufferAllocator* allocator[2] = { &allocator_0, &allocator_1 };
  float* fft_buffer = allocator[0]->Allocate<float>(fft_size);
  float* ifft_buffer = allocator[num_channels_ - 1]->Allocate<float>(fft_size);

  Modifier *modifiers[2];
  if (TRANSFORMATION_TYPE_FRAME == transformation_type) {
    modifiers[0] = &frame_transformation_[0];
    modifiers[1] = &frame_transformation_[1];
  } else {
    modifiers[0] = &spectral_clouds_transformation_[0];
    modifiers[1] = &spectral_clouds_transformation_[1];
  }

  size_t num_textures = modifiers[0]->num_textures();
  size_t texture_size = modifiers[0]->texture_size(fft_size);

  for (int32_t i = 0; i < num_channels_; ++i) {
    short* ana_syn_buffer = allocator[i]->Allocate<short>(
        (fft_size + (fft_size >> 1)) * 2);
    
    num_textures = min(
        allocator[i]->free() / (sizeof(float) * texture_size),
        num_textures);
    stft_[i].Init(
        &fft_,
        fft_size,
        fft_size / hop_ratio,
        fft_buffer,
        ifft_buffer,
        window_lut,
        ana_syn_buffer,
        modifiers[i],
        hd ? &fft_hd_ : NULL,
        window_lut_size);
  }
  for (int32_t i = 0; i < num_channels_; ++i) {
    float* texture_buffer = allocator[i]->Allocate<float>(
        num_textures * texture_size);
    modifiers[i]->Init(texture_buffer, fft_size, num_textures, sample_rate, &fft_);
  }
}

void PhaseVocoder::Process(
    const Parameters& parameters,
    const FloatFrame* input,
    FloatFrame* output, size_t size) {

  const float* input_samples = &input[0].l;
  float* output_samples = &output[0].l;
  for (int32_t i = 0; i < num_channels_; ++i) {
    stft_[i].Process(
        parameters,
        input_samples + i,
        output_samples + i,
        size,
        2);
  }
}

void PhaseVocoder::Buffer() {
  for (int32_t i = 0; i < num_channels_; ++i) {
    stft_[i].Buffer();
  }
}

// Test hooks (used by tests/rate_compare_test.cpp): the 6144-point window and the 4096-point table it was derived from.
const float* HdWindow6144_for_test() { return HdWindow6144(); }
const float* Window4096_for_test() { return lut_sine_window_4096; }

}  // namespace clouds
