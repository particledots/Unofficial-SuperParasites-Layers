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

#ifndef CLOUDS_DSP_PVOC_PHASE_VOCODER_H_
#define CLOUDS_DSP_PVOC_PHASE_VOCODER_H_

#include "stmlib/stmlib.h"

#include "stmlib/fft/shy_fft.h"

#include "supercell/dsp/frame.h"
#include "supercell/dsp/pvoc/hd_fft.h"
#include "supercell/dsp/pvoc/stft.h"
#include "supercell/dsp/pvoc/spectral_clouds_transformation.h"
#include "supercell/dsp/pvoc/frame_transformation.h"

namespace clouds {

struct Parameters;

class PhaseVocoder {
 public:
  PhaseVocoder() { }
  ~PhaseVocoder() { }

  void Init();

  enum TransformationType {
    TRANSFORMATION_TYPE_FRAME,
    TRANSFORMATION_TYPE_SPECTRAL_CLOUD
  };
  
  void Init(
      TransformationType transformation_type,
      void** buffer, size_t* buffer_size,
      const float* large_window_lut, size_t largest_fft_size,
      int32_t num_channels,
      int32_t resolution,
      float sample_rate);
  // sample_rate is the rate the spectral processing runs at. Above 32 kHz the 6144-point FFT (HdFft) and a 6144-point
  // window are used; this gives the same window length in seconds and the same bin spacing in Hz as the 4096-point
  // FFT at 32 kHz. At 32 kHz nothing changes.

  void Process(
      const Parameters& parameters,
      const FloatFrame* input,
      FloatFrame* output,
      size_t size);
  void Buffer();
  
 private:
  FFT fft_;
  HdFft fft_hd_;
  
  STFT stft_[2];
  FrameTransformation frame_transformation_[2];
  SpectralCloudsTransformation spectral_clouds_transformation_[2];

  int32_t num_channels_;

  DISALLOW_COPY_AND_ASSIGN(PhaseVocoder);
};

}  // namespace clouds

#endif  // CLOUDS_DSP_PVOC_PHASE_VOCODER_H_
