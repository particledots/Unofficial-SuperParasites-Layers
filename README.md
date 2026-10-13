# Unofficial SuperParasites HD — AU / VST3 (experiment)

A copy of [Unofficial SuperParasites Extended](https://github.com/particledots/Unofficial-SuperParasites-Extended) (SuperParasites with the long
**Buffer Length** menu) with one new menu: **Engine Rate**.

* **32 kHz (original)** — the DSP runs at 32 kHz exactly as in Extended (16 kHz in the low-fi qualities). This setting is the default.
* **48 kHz (HD)** — the same DSP running natively at 48 kHz. Delay lengths, window lengths, filter cutoffs and smoothing speeds are rescaled so that
  times in seconds and pitches in Hz match the 32 kHz version, and the spectral modes use a 6144-point FFT instead of 4096. You can switch between the two
  in one plug-in and compare them by ear.

It is a separate plug-in (code `SPHD`, "particledots: Unofficial SuperParasites HD"), so it installs next to Extended and the original.
Not a Mutable Instruments product, and not made or endorsed by any of the module makers.

**I have not listened to it, and nothing here claims the 48 kHz engine sounds the same as, or better than, the 32 kHz one.** The tests below measure times,
pitches, decay times and spectra. Whether the difference is audible, and which you prefer, is for you to say.

## How the two settings relate

| | 32 kHz (original) | 48 kHz (HD) |
|---|---|---|
| Processing rate / block | 32 kHz, 32 frames (1 ms) | 48 kHz, 48 frames (1 ms) |
| Host at 48 kHz | resampled 48 -> 32 -> 48 kHz (sinc) | **no resampling at all** (straight copy) |
| Highest frequency passed | about 15.5 kHz | the host's Nyquist (24 kHz at 48 kHz) |
| Delay times, reverb decay, pitch in Hz, grain/window lengths in ms | the module's | the same (rescaled, see `docs/RATE_AUDIT.md`) |
| Buffer Length (1 / 4 / 10 / 20 s) | seconds as in Extended | the same seconds (1.5x the bytes) |
| Spectral modes (Spectral madness, Spectral clouds) | FFT 4096, window 128 ms, bins 7.8125 Hz apart | FFT **6144**, window 128 ms, bins 7.8125 Hz apart |
| Low-fi qualities (8-bit) | processing at 16 kHz (2:1 filter) | processing at **16 kHz** (new 3:1 filter); same seconds of memory |
| Reverb / Oliverb / Resonestor delay lines | 16384 samples | 32768 samples (longer delay lines in samples, same seconds) |
| Output if the engine rate is 32 kHz | bit-for-bit the same samples as Extended (checked, see below) | — |

Switching the Engine Rate rebuilds the processor and clears the recorded audio (like changing Buffer Length or Quality), and the plug-in's reported
latency changes slightly.

### What does *not* come out the same (expected, not bugs)

These are consequences of running at a different sample rate, not of the rescaling:

1. **Interpolation brightness.** Reads at fractional positions (Hermite in Beat repeat and the Oliverb loop, linear in grains, pitch shifting) roll off
   at a fixed fraction of the sample rate, so at 48 kHz the roll-off reaches higher in Hz. Measured with noise in: the top octave (6.4 - 12.8 kHz) is up
   to about 4 dB brighter in Oliverb and 5.6 dB brighter in Beat repeat played at half speed; below 6.4 kHz every octave band agrees within the 3 dB the test allows. (Beat
   repeat at normal speed, which has no interpolation, differs by 0.0 dB.)
2. **Content above 16 kHz** exists only in the 48 kHz setting.
3. **Filter shapes near the top of their range.** The module's `FREQUENCY_FAST` filters use a polynomial that is inaccurate at high cutoffs; the same Hz
   is a lower fraction of 48 kHz, so the filter is slightly more accurate there and differs a little from the 32 kHz one. I did not change the polynomial,
   because that would change the 32 kHz output.
4. **Spectral modes**: the window is the original 4096 window re-sampled to 6144 points (the original's analytic form is not known) and re-normalised so that
   overlapping windows still add up exactly. The "warp" curves are applied over the first 0 - 16 kHz of the spectrum (the same Hz as before); bins above 16 kHz
   pass through unwarped.
5. **Delay lengths are whole samples.** A length of n samples at 32 kHz becomes round(1.5 n) at 48 kHz: the time is right to within 10 microseconds.
6. **Random and chaotic parts** (granular jitter, Spectral clouds' random band gains, feedback loops) cannot be compared sample by sample between rates; only
   their statistics can.
7. **CPU.** Roughly 1.0 - 1.7x the cost of the 32 kHz setting in my Linux timing of the headless test (Spectral madness the most). Not measured on a Mac.

### Source changes compared with Extended
Details in `docs/RATE_AUDIT.md`. In short: a small header (`engine_rate.h`) holds the rescaling rules; the delay-line effects (Reverb, Diffuser, Oliverb,
Resonestor, PitchShifter) became templates so their delay lengths are compile-time constants at either rate; players, buffers, filters and smoothing take the
rate at run time; `HdFft` (a 3 x 2^k real FFT) and a 3:1 low-fi converter were added; and the wrapper chooses block size, memory and resamplers per rate.
One change affects **both** rates: `grain.h` and `window.h` keep their 16.16 fixed-point position in a 64-bit integer instead of a 32-bit one (the same kind
of overflow fix Extended made in `looping_sample_player.h` and `kammerl_player.h`). It only changes the output when the old 32-bit value would have overflowed;
in the 64 combinations checked below the 32 kHz output is identical.

## Install
Download `SuperParasites-HD-macOS` from the latest passing run in the Actions tab and follow `INSTALL.txt`
(universal arm64 + x86_64, macOS 11 or later). Controls are the same as in the other plug-ins, plus the Engine Rate menu next to Buffer Length.

## Verified and not verified

**Verified here (Linux, g++, October 2026)**
* *32 kHz setting equals Extended.* `tests/bit_exact_dump.cpp` hashes the output of every mode x quality (4) x Buffer Length (1 s, 4 s), 64 combinations, 6 s each,
  with a fixed input and moving knobs. The hashes from this code at 32 kHz and from Extended at commit `66d2161` are identical. The GitHub workflow repeats this
  with the macOS compiler.
* *The 6144-point FFT* matches a plain DFT (error about 1e-7 relative) at 12, 48, 384 and 6144 points and round-trips; the 6144 window is power-complementary
  to 1e-7 and within 0.0003 of the original window's shape.
* *Rescaling rules*: every delay length at 48 kHz is within half a sample of the 32 kHz time; one-pole coefficients give the same decay per second; cutoffs the same Hz.
* *The old test suite* (`engine_test`: all modes x qualities, freeze, mode switching, extreme knobs, Buffer Length 1 / 4 / 10 / 20 s including mono and low-fi,
  44.1 / 48 / 96 kHz hosts) now runs at both engine rates: 254 checks, all pass. (One bound for extreme knob settings was loosened from 1.5 to 1.7 because the
  host-rate resampler overshoots a hard-clipped signal more with a 96 kHz host and the 48 kHz engine; the engine itself never exceeds 1.0.)
* *`rate_compare_test`* (same input and knobs at both rates, measured, 32 kHz vs 48 kHz):
  * delay times: looping delay at nine positions (16-bit and low-fi) agrees within 1.5 ms, which is the resampling latency of the 32 kHz path; granular and
    time-stretch start/peak times agree within 5 ms; Beat repeat's loop period is 0.2500 s at both rates;
  * pitch (+12, -5, +7, -7 semitones etc.): granular, stretch, looping-delay pitch shifter, spectral, spectral clouds, Oliverb and Resonestor agree between the
    rates (limit 0.5 %; 1.2 % for granular and 1.5 % for Oliverb because their output is random or smeared; the largest difference seen was 0.9 %). In some modes **both** rates are off the plain-arithmetic pitch (for example
    the looping-delay pitch shifter gives 702 Hz for +7 semitones from 440 Hz at both rates): that is the original's behaviour, not a rate effect;
  * reverb decay (60 dB time): the post reverb at three settings agrees within 5.4 %, Oliverb at six settings within 3 % (averaged over three noise bursts,
    whose own spread is about 2 - 4 %);
  * spectrum with white noise in, all eight modes, hi-fi and low-fi: each octave band 100 Hz - 12.8 kHz agrees in shape within 3 dB (6 dB in the top octave, see item 1
    above); total level within 2 dB.
* *GitHub Actions on macOS (run 38476158548, commit `69a40f7`, passed):* the universal arm64 + x86_64 AU and VST3 build, the ad-hoc signing and packaging, `engine_test` at
  both engine rates, `rate_compare_test`, and the bit-for-bit comparison of the 32 kHz output with Extended built with the macOS compiler. (I can only see that the steps
  exited successfully, not their log: the log host is not reachable from where I work.) The first run failed because AppleClang wants the `template` keyword in
  `resonestor.h`; fixed.

**Not verified**
* Loading in Logic on the M4 Mac mini or the Intel Mac; whether the Engine Rate switch behaves in a host (it rebuilds the processor on the audio thread, like Buffer Length).
* **How it sounds**, and whether 48 kHz sounds the same as, better than or worse than 32 kHz in any mode. I have not listened. Likewise no comparison with a hardware module.
* CPU use on a Mac.
* Any host sample rate other than 44.1 / 48 / 96 kHz; 48 kHz engine with a 44.1 kHz host uses the same sinc resampler as the 32 kHz one.
* The accuracy of the replaced low-fi converter (69 taps, 3:1) beyond the spectrum test above.

**Found in the original code and left unchanged** (changing them would break the bit-exact match with Extended)
* `correlator.cc`: `x >> (32 - offset_bits)` is an undefined shift when `offset_bits` is 0. On the module's CPU this gives 0; on Intel and Apple CPUs it gives `x`, so 1 in 32
  candidate positions is scored wrongly in Pitch shifter / time stretch and Oliverb's pre-delay.
* A few 4-byte reads just past the end of small tables (`slope_response`, `lut_xfade_in/out`) when a knob is at an end stop.

## Planned experiments
1. Done: long buffers at 32 kHz (Extended).
2. Done here, unlistened: native 48 kHz. Possible follow-ups depending on what you hear: 44.1 kHz, or 96 kHz.
3. Later, maybe: several captured layers (the Instruō Arbhar idea).

## Source and licence
Same as Extended: `third_party/supercell/` is `supercell/dsp/*` and `supercell/resources.*` from `patrickdowling/superparasites` (commit `2e9ed7c`) with the HD
changes above, `third_party/stmlib/` is a subset of `mqtthiqs/stmlib` at `8ab2aaee77cbacb47b646d46d22ee5d358effe2d`. MIT per that project's README, with MIT notices
in the source files (Emilie Gillet, Matthias Puech, Julius Kammerl, Patrick Dowling); keep them if you redistribute. The wrapper code in `src/` has no licence assigned
yet. JUCE is used under its own terms (AGPLv3 / commercial).
