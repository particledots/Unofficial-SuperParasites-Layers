# Unofficial SuperParasites Layers — AU / VST3 (experiment)

A copy of [Unofficial SuperParasites HD](https://github.com/particledots/Unofficial-SuperParasites-HD) (the **Engine Rate** and **Buffer Length** menus and
the eight SuperParasites modes) with one new feature: **six stored layers**. A layer is a separate recording that you can record into, switch between, and
play with the existing modes.

It is a separate plug-in (code `SPLy`, "particledots: Unofficial SuperParasites Layers"), so it installs next to HD, Extended and the original.
Not a Mutable Instruments product, and not made or endorsed by any of the module makers.

**I have not listened to it, and nothing here claims it sounds good, or better than anything else.** The tests below measure what is stored, what is
played back and how the memory is used. Whether it is musical is for you to say.

## How the layers work

There is a new **LAYER** menu with seven choices, plus **RECORD** and **CLEAR** buttons and a status line.

* **Live** — the plug-in as HD: one buffer that is always recording, the newest sound replacing the oldest. With Live selected (and no layer buttons pressed) the
  output is the HD plug-in's, sample for sample (checked, see below).
* **Layer 1 … Layer 6** — six separate recordings. One plays at a time; the one you pick.
  * **RECORD** (with a layer selected) *clears that layer* and records the audio coming in, from the start of the layer, until the layer is full; it then stops by
    itself. Pressing it again (**STOP**) stops early and keeps what was recorded; the rest of the layer is silent. While it records you hear it through the current mode.
  * **CLEAR** empties the selected layer.
  * Picking another layer fades out (about 24 ms), swaps, and fades in. Switching never changes the audio stored in the layers.
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
* **Not saved:** layers live in memory only. Closing the Logic project, or Logic unloading the plug-in, loses them. Saving the audio so it can be reloaded is a possible
  next step (not in this version).
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
* `SuperParasitesEngine.*`: the six layer stores, the record / clear / switch logic, and restoring a layer after the processor rebuilds. One new parameter, `layer`.
* `PluginProcessor.*`, `PluginEditor.*`: the Layer menu, RECORD and CLEAR buttons, status line.
* `tests/layers_test.cpp` and `tests/live_dump.cpp` are new; `tests/bit_exact_dump.cpp` (the Extended comparison) was replaced by `live_dump.cpp`.

## Install
Download `SuperParasites-Layers-macOS` from the latest passing run in the Actions tab and follow `INSTALL.txt`
(universal arm64 + x86_64, macOS 11 or later). The other controls are as in HD.

## Verified and not verified

**Verified here (Linux, g++, October 2026) — the tests in this repo**
* `tests/layers_test.cpp` (550 checks, all pass, run at both engine rates unless noted): recording six layers with six different tones fills each layer by itself in the
  right time and stores that tone at the fed level (16-bit); each layer plays back its own tone (identified by measuring pitch) through Looping delay, in 12 switches in a mixed order,
  at hosts of 44.1, 48 and 96 kHz (44.1 and 96 kHz with the 48 kHz engine only); the stored audio is unchanged after all the switching; trips through Spectral madness, Resonestor, Oliverb and Spectral clouds and back
  return the layer (and Record is ignored in those modes); a layer chosen while in another mode plays on return; Live plays the live input and leaves the layers alone;
  CLEAR empties only the selected layer; recording over a layer replaces it; stopping early keeps what was recorded (about the time recorded); changing layer during a recording
  keeps the recording; changing Quality, Buffer Length or Engine Rate empties all layers, and a re-prepare keeps them; all four qualities record and play; live input at another pitch does not get
  into a playing layer in Granular, Pitch shifter / time stretch or Looping delay, and Beat repeat's output is identical with silent and with loud live input; six 10 s layers store
  the calculated number of bytes; Record does nothing in Live.
  I also broke the code on purpose twice (let live input overwrite a playing layer; skip restoring a layer after a mode trip) and the tests failed each time, so they can
  detect those faults.
* **Live equals HD.** `tests/live_dump.cpp` hashes the output for every mode x quality (4) x Buffer Length (1 s, 10 s) x engine rate (2), 128 combinations, 4 s each, with a fixed
  input and moving knobs. Built from this code and from the HD plug-in at commit `aaa28f5`, the 128 lines are identical.
* The existing suites pass unchanged: `engine_test` and `rate_compare_test`.
* The plug-in and editor sources compile against the JUCE 8.0.6 headers (syntax check, Linux, no warnings).
* Cost of a layer switch on this Linux machine: the slowest 512-sample `process()` call while switching was 0.8 ms with 10 s layers and 1.5 ms with 20 s layers (48 kHz stereo).
* Largest sample-to-sample step in the output while switching between two layers playing tones (Looping delay): about 0.03 (a number, not a claim about clicks you can hear).

**Verified on GitHub Actions (macOS runner), run 38484375319, commit `fcbaa9a`:** every step passed: `engine_test`, `rate_compare_test`, `layers_test` (550 checks), the Live = HD
comparison built with the macOS compiler (128 combinations identical), the universal arm64 + x86_64 AU and VST3 build, ad-hoc signing and packaging; the artifact
`SuperParasites-Layers-macOS` was uploaded. (I can see each step's pass / fail result, not its log text.)

**Not verified**
* Loading in Logic on the M4 Mac mini or the Intel Mac; the layer menu and buttons in a host; what happens in Logic when a project is saved and reopened (the layers are lost by design).
* **How it sounds**: whether switching or recording layers is click-free by ear, whether layers are musically useful in each mode. I have not listened.
* **The editor window has never been looked at** (I cannot run it here); the layout of the new Layers panel may need adjusting.
* CPU use and the copy time of a layer switch on a Mac; memory use inside Logic.
* RECORD cannot be automated or MIDI-triggered in this version (it is a button only); the Layer menu is a normal automatable parameter.
* Recording into one layer while another plays is not possible (one layer plays at a time, and it is the one being recorded).
* Beat repeat with layers beyond the code reading above, and the FEEDBACK / FREEZE behaviour of a playing layer, were not listened to.

## Planned
* Not in this version: saving each layer's audio so it can be reloaded later.
* Possibly later: several layers playing together.

## Source and licence
Same as HD: `third_party/supercell/` is `supercell/dsp/*` and `supercell/resources.*` from `patrickdowling/superparasites` (commit `2e9ed7c`) with the HD changes
(`docs/RATE_AUDIT.md`) and the layer hooks above, `third_party/stmlib/` is a subset of `mqtthiqs/stmlib` at `8ab2aaee77cbacb47b646d46d22ee5d358effe2d`. MIT per that project's README,
with MIT notices in the source files (Emilie Gillet, Matthias Puech, Julius Kammerl, Patrick Dowling); keep them if you redistribute. The wrapper code in `src/` has no licence
assigned yet. JUCE is used under its own terms (AGPLv3 / commercial).
