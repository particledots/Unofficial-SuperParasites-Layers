# Sample-rate audit: everything in the DSP that depends on the sample rate

Done before any code was changed, by reading every file under `third_party/supercell/dsp/` for hard-coded rates, lengths in samples,
per-sample coefficients, normalised cutoffs and FFT sizes. "32 kHz" is the original engine; "48 kHz" is the HD engine.
Rules (all in `engine_rate.h`, and every rule returns the original number unchanged at 32 kHz):

* **Count of samples** (delay length, window length, counter) x 1.5, rounded to the nearest sample (halves round up) -> same seconds.
* **Cutoff given as a fraction of the sample rate** x 32/48 -> same Hz.
* **Per-sample one-pole coefficient k** -> 1 - (1 - k)^(32/48) -> same decay per second.
* **Per-block quantities** (knob smoothing, `freeze_lp_`, once-per-block coefficients) unchanged: a block is 1 ms at both rates (32 frames / 48 frames).

| # | Where | What it is at 32 kHz | What happens at 48 kHz |
|---|---|---|---|
| 1 | `frame.h` | `kMaxBlockSize` 32 | 48 (block = 48 frames = 1 ms). The engine's block length is chosen at run time |
| 2 | `fx/reverb.h` | 10 delay lengths (113 ... 4782 samples: 4 input all-passes, 2 loops of 2 all-passes + 1 delay), modulated reads at 10/60 and 4680/100, write offset 100 | each length x 1.5 (`Scale<1>::N/F`); buffer 16384 -> 32768 samples |
| 3 | `fx/reverb.h` | LFO frequencies `0.5/32000`, `0.3/32000` (the hard-coded 32000) | divided by 48000; FxEngine's LFO update interval 32 -> 48 samples so it stays once per ms |
| 4 | `fx/reverb.h` | one-pole low-pass (`set_lp`, initial 0.7) in the loop, per sample | pole mapped (same decay per second) |
| 5 | `fx/diffuser.h` | 8 all-pass lengths (126, 180, 269, 444, 151, 205, 245, 405) | x 1.5; buffer 2048 -> 4096 floats |
| 6 | `fx/oliverb.h` | 10 delay lengths (113 ... 4782), pitch-shifter window 128..3410, smear 10/60, write offset 100 | x 1.5; buffer 16384 -> 32768 |
| 7 | `fx/oliverb.h` | per-sample room-size smoothing 0.01; lp/hp one-poles; modulation depth in samples; 9 random LFOs (slope per sample) | pole mapped; depth x 1.5; LFO slopes x 32/48 |
| 8 | `resonestor.h` | comb delay `32000 / BASE_PITCH / ratio`, limit `MAX_COMB`, burst delays 200/4000, spread delay 3999 | `48000 / ...`, limits and delays x 1.5; buffer 16384 -> 32768 floats (131072 bytes) |
| 9 | `resonestor.h` | `powf(feedback, period / 32000)`, hp `10/32000`, `rand_hp 1/32000`, filter cutoffs given as fractions of the rate (`lp_freq`, `distortion*0.4`, `burst_damp^2*0.5`) | `/ 48000`; cutoffs through `Freq()`. **In low-fi the original runs this at 16 kHz with the 32000 constants unchanged (a quirk of the original); that is kept, so low-fi Resonestor at 48 kHz is exactly the 32 kHz low-fi one** |
| 10 | `fx/pitch_shifter.h` | window 2047 (two delay lines), size range 128..2047 | x 1.5; buffer 4096 -> 8192 |
| 11 | `granular_sample_player.h` | grain size table (in samples), size hint 1024, per-sample gain smoothing 0.01 | grain sizes x 1.5, hint x 1.5, pole mapped |
| 12 | `grain.h`, `window.h` | 16.16 fixed-point position was `int32` and can overflow for long grains | widened to `int64` (see "Source changes") |
| 13 | `wsola_sample_player.h` | `kMaxWSOLASize` 4096 (window), tap threshold 128, hysteresis 64, correlator stride `window/2048` | x 1.5; stride scaled so the correlator still looks at the same number of samples-of-bits (2048) |
| 14 | `looping_sample_player.h` | cross-fade 64 samples, tap threshold 128, delay smoothing 0.0005, tap smoothing 0.00001 | x 1.5, x 1.5, poles mapped |
| 15 | `audio_buffer.h` | end-of-buffer cross-fade 256 samples | x 1.5 (384); tail buffer enlarged |
| 16 | `random_oscillator.h` | slope per sample | x 32/48 |
| 17 | `kammerl_player.h` (Beat repeat) | **nothing found**: slice lengths come from the time between capture pulses, in samples of the buffer; loop positions are fractions of the slice | unchanged |
| 18 | `granular_processor.cc` | feedback high-pass cutoff `(20 + 100 fb^2) / sample_rate()`, where `sample_rate()` is 32000 or 16000 (low-fi), but the filter runs at the engine rate | same Hz via `Freq()` |
| 19 | `granular_processor.cc` | looping delay / stretch low-pass and high-pass cutoffs (fractions of 32 kHz, clamped to 0.499) | clamp as before, then `Freq()` |
| 20 | `granular_processor.cc` | mute fades `0.01` per sample (twice) | pole mapped |
| 21 | `granular_processor.cc` | low-fi: 2:1 decimation with a 45-tap filter, so the processing runs at 16 kHz | 3:1 with a new 69-tap filter (`src_filter_1x_3_69`) so it still runs at 16 kHz. The rest of the low-fi path (grains, delay, Oliverb, Resonestor, ...) is the unscaled 16 kHz code, and its memory is 2/3 of the 1.5x amount so low-fi holds the same seconds |
| 22 | `pvoc/stft.cc`, `phase_vocoder.cc` | FFT 4096 points (128 ms window, hop 1024 = 32 ms, bins 7.8125 Hz apart), window = `lut_sine_window_4096` | FFT **6144** points (same 128 ms, 32 ms, 7.8125 Hz) using the new `HdFft` (3 x 2^k); the window is the 4096 table resampled to 6144 and renormalised so that w(j)^2 + w(j+3072)^2 = 1 |
| 23 | `pvoc/frame_transformation.cc` | `WarpMagnitudes`: polynomial curves in fractions of the whole spectrum (0..16 kHz) | applied over the first 2032 bins (0..16 kHz), bins above are passed through; "spectral shift up" glitch wraps at the same bin. Bin-based things (high-frequency truncation 16 bins, quantise, hold, "nasty high-pass" `i/16`) are already in Hz because the bin spacing is the same |
| 24 | `pvoc/spectral_clouds_transformation.cc` | band layout counted in bins | unchanged (same Hz) |
| 25 | all `FREQUENCY_FAST` filters (`stmlib/dsp/filter.h`) | the tangent is a polynomial that is inaccurate at high cutoffs | not replaced (replacing it would change the 32 kHz output). The same Hz at a lower fraction of the sample rate is slightly more accurate at 48 kHz, so filter shapes near the top of the range differ a little between the rates |
| 26 | `GranularProcessor::Prepare` | workspace: diffuser 8192 B + reverb 32768 B + correlator, the pitch shifter reuses the correlator's memory | 16384 + 65536 + (correlator) with the pitch shifter 16384 -> **at least 98304 bytes** of workspace; recording memory x 1.5 (hi-fi) so Buffer Length means the same seconds; the wrapper caps recording memory per channel / in mono so the length is exactly the 32 kHz length |

## Things that are not rate-dependent and were checked
Grain count (voices, not time), `kMaxNumChannels`, all LUT tables indexed by a 0..1 parameter, the knob smoothing in the wrapper (per 1 ms block), SoftLimit/SoftConvert, and the 16-bit / 8-bit storage formats.

## What cannot be made identical
* Interpolation filters (Hermite reads, linear grain reads, the 4-point reads in Beat repeat and the Oliverb loop) roll off at a fixed fraction of the sample rate, so at 48 kHz they reach higher in Hz. Measured: with Beat repeat at half speed the top octave (6.4-12.8 kHz) is 5.6 dB brighter, in Oliverb 3.8 - 4 dB.
* The sinc filters that turn the host rate into 32 kHz (and back) do not exist at 48 kHz with a 48 kHz host: that path is a straight copy.
* Everything above 16 kHz: the 32 kHz engine removes it, the 48 kHz one passes it (on purpose).
