# Unofficial SuperParasites Layers — AU / VST3 (experiment)

A copy of [Unofficial SuperParasites HD](https://github.com/particledots/Unofficial-SuperParasites-HD) (the **Engine Rate** and **Buffer Length** menus and
the eight SuperParasites modes) with one new feature: **six stored layers**. A layer is a separate recording that you can record into, switch between, and
play with the existing modes. Layers can be saved to and loaded from **WAV files**, and the layer switch has an optional **Switch Time** fade.

It is a separate plug-in (code `SPLy`, "particledots: Unofficial SuperParasites Layers"), so it installs next to HD, Extended and the original.
Not a Mutable Instruments product, and not made or endorsed by any of the module makers.

**I have not listened to it, and nothing here claims it sounds good, or better than anything else.** The tests below measure what is stored, what is
played back and how the memory is used. Whether it is musical is for you to say.

## How the layers work

There is a new **LAYER** menu with seven choices, plus **RECORD**, **CLEAR**, **SAVE WAV** and **LOAD WAV** buttons, a **SWITCH TIME** slider and a status line.

* **Live** — the plug-in as HD: one buffer that is always recording, the newest sound replacing the oldest. With Live selected (and no layer buttons pressed) the
  output is the HD plug-in's, sample for sample (checked, see below).
* **Layer 1 … Layer 6** — six separate recordings. One plays at a time; the one you pick.
  * **RECORD** (with a layer selected) *clears that layer* and records the audio coming in, from the start of the layer, until the layer is full; it then stops by
    itself. Pressing it again (**STOP**) stops early and keeps what was recorded; the rest of the layer is silent. While it records you hear it through the current mode.
  * **CLEAR** empties the selected layer.
  * Picking another layer swaps it in. With **Switch Time** at *Instant* (the default) that is a mute of about 24 ms, a swap, and an unmute. Switching never changes the audio
    stored in the layers.
  * **SWITCH TIME** (0 to 2 s, automatable): above *Instant*, the sound fades out over half the time, the layer is swapped, and it fades back in over the other half
    (raised-cosine fades, so a setting of 800 ms is a dip of 800 ms in total). It is a dip through quiet, not an overlap of the two layers: the old layer is gone before
    the new one starts. A true overlap needs a second engine playing at the same time and is not in this version. If you ask for another layer while a fade is running it
    carries on from where the level is.
  * **SAVE WAV** writes the selected layer to a 16-bit WAV file (mono or stereo as the layer is, at the layer's own sample rate: the engine rate, or 16 kHz in the low-fi
    qualities; low-fi layers are decoded to 16-bit first). **LOAD WAV** puts a WAV file into the selected layer: any sample rate (resampled to the layer's rate), mono or stereo
    (a mono file goes to both sides of a stereo layer; a stereo file is mixed down for a mono layer), 8/16/24/32-bit integer or 32/64-bit float. A file longer than the layer is
    cut to the layer length; a shorter file gives a shorter layer (the rest is silent), just like stopping a recording early. Louder than full scale is clipped. A loaded layer
    behaves exactly like a recorded one, including being replaced by RECORD. Loading into the layer that is playing swaps the new audio in with the usual fade. Both buttons ask
    for the file with the normal Mac file dialog. Save and Load hand the audio over through the audio thread, so they only complete while Logic is processing audio (if the
    transport is stopped and nothing is processing, the plug-in says it is waiting). LOAD is refused while a recording is running, or if Quality / Buffer Length / Engine Rate
    changed while the file was being read.
  * The length of a layer is the **Buffer Length** menu (default 10 s), in the current quality: mono holds a little over twice as long, low-fi (8-bit) four times.
    The status line shows how many seconds each layer holds.

### Which modes play layers
Granular, Pitch shifter / time stretch, Looping delay and Beat repeat. In **Oliverb, Spectral madness, Resonestor and Spectral clouds** the plug-in works on the live
input as HD does, and the layers are kept safe underneath. (Those four modes rebuild the plug-in's own recording memory and use it as workspace, which is why the layers are
stored separately. A layer you chose before going to one of them plays again when you come back to one of the four layer modes.)

### A playing layer is held, like Freeze
A layer plays the way the module plays a frozen buffer: nothing is written into it while it plays. In Granular, Pitch shifter / time stretch and Looping delay that means
the plug-in is in its Freeze state while a layer plays (so the FEEDBACK knob does not feed the output back into the input, as with Freeze; this is from reading the code,
not from listening). Beat repeat keeps its own behaviour (Freeze there would make every slice fire) and is only held against being overwritten. Beat repeat takes its slices
from just before the layer's write position, which for a full layer is the end of the layer (from reading the code; I have not checked how that sounds).

### What empties the layers, and what does not
* **Emptied:** changing **Quality**, **Buffer Length** or **Engine Rate** (the layers are stored in that format); RECORD on a layer (it is replaced); CLEAR.
* **Not emptied:** switching layers, going to Live and back, visiting any other mode, the host re-preparing the plug-in (for example a new sample rate or buffer size).
* **Not saved with the project:** layers live in memory only. Closing the Logic project, or Logic unloading the plug-in, loses them. Use **SAVE WAV** to keep a layer and
  **LOAD WAV** to bring it back. (Embedding the layers in the project / a plug-in preset, or remembering the WAV file locations in the project, are not in this version.)
* A layer *shorter* than the buffer (a short recording or a short loaded file) is played by the modes from a buffer that is mostly silent, so what you hear depends on SIZE and
  POSITION (in Looping delay the loop can land on the silent part). This is the same for a short recording as for a short loaded file.
* Going from a layer to **Live** starts Live from silence (Live's old content is not kept).

### Memory
Stored audio is raw 16-bit samples at the engine rate (8-bit at 16 kHz in low-fi), per channel:

| 6 layers, stereo 16-bit | 32 kHz engine | 48 kHz engine |
|---|---|---|
| 1 s each | 0.8 MB | 1.2 MB |
| 4 s each | 3.1 MB | 4.6 MB |
| 10 s each | 7.7 MB | 11.5 MB |
| 20 s each | 15.4 MB | 23.0 MB |

The 10 s rows were measured in the test (7.68 MB and 11.52 MB, equal to the calculation); the others are calculated. Mono is half. The plug-in also reserves address space
for the largest case (about 24 MB, so that recording never has to allocate while audio runs); the pages are only used when a layer is recorded. The processor's own
recording buffer and effect memory are as in HD.

## What changed in the source compared with HD
* `granular_processor.h/.cc`: accessors for the recording memory (where it is, the write heads), a "hold writes" switch, a "record exactly one pass" mode, a counter that
  changes when the memory is rebuilt, and a clear that uses the right silence (0 for 16-bit, 127 for the low-fi samples). None of it does anything unless called.
* `SuperParasitesEngine.*`: the six layer stores, the record / clear / switch logic, and restoring a layer after the processor rebuilds; the Switch Time fade (a gain ramp on
  the output; at 0 the output code path is the one from the first Layers build); the hand-over of layers to and from the UI thread, and the conversion for Load (resample,
  mono/stereo, 16-bit or 8-bit mu-law, the 8-sample interpolation tail). Parameters: `layer`, `switch_time`.
* `LayerAudio.*` (new, JUCE-free): the WAV reader and writer.
* `PluginProcessor.*`, `PluginEditor.*`: the Layer menu, RECORD / CLEAR / SAVE WAV / LOAD WAV buttons, Switch Time slider, status line.
* `tests/layers_test.cpp` and `tests/live_dump.cpp` are new; `tests/bit_exact_dump.cpp` (the Extended comparison) was replaced by `live_dump.cpp`.

## Install
Download `SuperParasites-Layers-macOS` from the latest passing run in the Actions tab and follow `INSTALL.txt`
(universal arm64 + x86_64, macOS 11 or later). The other controls are as in HD.

## Verified and not verified

**Reported by Bryn, listening on the M4 Mac mini in Logic (first Layers build, before Save / Load and Switch Time):** it installs and loads without problem; the layers
sound clear; he recorded four layers and automated the Layer menu from a Logic automation lane and it worked well; the layer switch does not click but changes abruptly (which
is why Switch Time was added). Nothing below is a claim about how any of it sounds beyond that.

**Verified here (Linux, g++, October 2026) — the tests in this repo**
* `tests/layers_test.cpp`: 1109 checks, all pass.
  * *Layers (as before):* six layers record six different tones and each plays its own tone back through 12 switches in a mixed order; stored audio is unchanged by switching;
    trips through the four non-layer modes return the layer; Live leaves the layers alone; CLEAR, re-record, early stop, format changes, all four qualities, nothing overwrites a playing layer
    (Beat repeat output identical with silent and loud live input), six 10 s layers store the calculated number of bytes (7.68 MB at the 32 kHz engine, 11.52 MB at 48 kHz).
  * *Switch Time:* at 0 the quiet gap stays short (under 90 ms); at 800 ms the output fades almost to silence in the middle of the fade (quietest point near the middle), then
    the new layer's tone is playing at full level; no step in the output (largest sample-to-sample change under 0.12) and the level changes gradually; switching again while a fade
    is running ends on the last layer asked for at full level; changing Switch Time to 0 during a fade still lands correctly; going to Live and back with a fade works. At both engine rates.
  * *WAV files:* what the writer writes the reader reads back exactly (16-bit); 8/16/24/32-bit integer, 32/64-bit float, extensible headers, six-channel files (first two kept) and
    cut-off files read correctly; garbage, truncated, compressed and empty files are refused with a message; 300 randomly damaged files never crash the reader.
  * *Save:* exported audio is the recorded tone at the recorded level, at the layer's own rate, right length, mono/stereo as the quality says; an empty layer reports empty; a second request
    while one is pending is refused. *Load:* files at 48, 44.1, 96 and 22.05 kHz, mono and stereo, shorter and longer than the layer, load with the right tone in the right channel, the right
    level and length; a click at 0.1 s in the file lands at 0.1 s in the layer (within 3 samples; the resampler's delay is compensated); the tail the processor expects after the audio is
    right; loading into a playing layer swaps it in and it plays the new audio; loading into another layer leaves the playing one alone; loading is refused during a recording and if the format
    changed while the file was queued; a file louder than full scale is clipped, not wrapped. **Save then Load gives back the identical samples** in all four qualities at both engine rates
    (for 8-bit low-fi: identical decoded samples). A 30 s file into a 10 s layer is cut to 10 s and plays.
  * I broke the code on purpose in six places (live input overwriting a playing layer; not restoring a layer after a mode trip; no Switch Time fade; no resampler delay compensation;
    not swapping in a layer loaded into the playing layer; no interpolation tail on loaded audio) and the tests failed each time.
* **Live equals HD.** `tests/live_dump.cpp` hashes the output for every mode x quality x Buffer Length (1 s, 10 s) x engine rate: 128 combinations, 4 s each. Built from this code and from
  the HD plug-in at commit `aaa28f5` the 128 lines are identical (re-run after Switch Time and Save / Load were added; also run in the workflow).
* The existing suites pass unchanged: `engine_test` and `rate_compare_test`.
* The plug-in and editor sources compile against the JUCE 8.0.6 headers (syntax check, Linux, no warnings; checked that it does report a deliberate error).
* Cost of a layer switch on this Linux machine: slowest 512-sample `process()` call while switching 0.5 ms with 10 s layers and 1.0 ms with 20 s layers (48 kHz stereo).

**Verified on GitHub Actions (macOS runner), run 38492051213, commit `d0bd3b1`: every step passed.** The steps are `engine_test`, `rate_compare_test`, `layers_test`, the Live = HD comparison, the universal
arm64 + x86_64 AU and VST3 build, ad-hoc signing, packaging and the `SuperParasites-Layers-macOS` artifact. (I can see each step's pass / fail result, not its log text.)

**Not verified**
* Loading in Logic on the **Intel Mac** (macOS 12.7).
* **Switch Time by ear**: whether the fade sounds good, what settings are useful, and the Switch Time slider and the new buttons in a host (the editor has been seen in Bryn's screenshots
  of the first build, not this one; the layout of the extra row is new).
* **SAVE WAV / LOAD WAV in Logic on a Mac**: the file dialogs, and the WAV files opening in other programs (only my own reader has read them). Whether Logic keeps processing audio while
  stopped, which Save and Load need (if it does not, the buttons wait until you press Play).
* Project save / reopen (the layers are lost by design; the Switch Time value is saved like any parameter).
* CPU use and layer switch / load copy time on a Mac; memory use inside Logic.
* RECORD cannot be automated or MIDI-triggered (it is a button only); Layer and Switch Time are normal automatable parameters.
* Recording into one layer while another plays is not possible (one layer plays at a time, and it is the one being recorded).
* The FEEDBACK / FREEZE behaviour of a playing layer and Beat repeat's slice source (from reading the code, not listened to).

## Planned
* A true overlap crossfade (two layers sounding at once) and several layers playing together: both need a second engine and roughly double the CPU.
* Keeping layers with the Logic project or a plug-in preset: either embed the audio in the saved state (about 11.5 MB for six 10 s stereo layers at 48 kHz), or remember
  the WAV file locations and reload them (small projects, but a moved file comes back empty).
* RECORD as an automatable parameter.

## Source and licence
Same as HD: `third_party/supercell/` is `supercell/dsp/*` and `supercell/resources.*` from `patrickdowling/superparasites` (commit `2e9ed7c`) with the HD changes
(`docs/RATE_AUDIT.md`) and the layer hooks above, `third_party/stmlib/` is a subset of `mqtthiqs/stmlib` at `8ab2aaee77cbacb47b646d46d22ee5d358effe2d`. MIT per that project's README,
with MIT notices in the source files (Emilie Gillet, Matthias Puech, Julius Kammerl, Patrick Dowling); keep them if you redistribute. The wrapper code in `src/` has no licence
assigned yet. JUCE is used under its own terms (AGPLv3 / commercial).
