# Unofficial SuperParasites Layers — AU / VST3 (experiment)

A copy of [Unofficial SuperParasites HD](https://github.com/particledots/Unofficial-SuperParasites-HD) (the **Engine Rate** and **Buffer Length** menus and
the eight SuperParasites modes) with one new feature: **six stored layers**. A layer is a separate recording that you can record into, switch between, and
play with the existing modes. Layers can be saved to and loaded from **WAV files**, presets and projects **remember which file each layer came from** and reload it, the layer switch has an optional **Switch Time** fade, and **Record** is an automatable parameter.

It is a separate plug-in (code `SPLy`, "particledots: Unofficial SuperParasites Layers"), so it installs next to HD, Extended and the original.
Not a Mutable Instruments product, and not made or endorsed by any of the module makers.

**I have not listened to it, and nothing here claims it sounds good, or better than anything else.** The tests below measure what is stored, what is
played back and how the memory is used. Whether it is musical is for you to say.

## Limiter switch (version 0.3.0)
A new **LIMITER** button (parameter `limiter`) sits at the end of the button row. **On (the default) is exactly the 0.2.0 sound**: the soft limiter after Output Level keeps loud sounds from going past 0 dBFS.
**Off** removes it: the output is then the engine plus the Output Level gain and nothing else, so loud wet sounds can go past 0 dBFS (by tens of dB at high Output Level settings). Only two safety things
remain: NaN becomes silence and values are held inside +/-16 so a runaway can never reach the host as infinity. Use Output Level, or a limiter after this plug-in, to keep it in range.
Switching glides over about 10 ms, so there is no click. Presets and projects saved before 0.3.0 have no switch and load with the limiter **on**, i.e. they sound as they did in 0.2.0.
Why it exists: the limiter is an instant curve (no attack, no release, no look-ahead), so on very loud sounds it acts like gentle distortion, which can be heard; the switch lets you turn that off and manage the level yourself.

## Output Level (version 0.2.0)
**What was wrong.** Loading any plug-in of this family made the sound quieter than before it was loaded. With Dry/Wet at 0 (nothing but the input) I measured the output at
**-9.04 dB** relative to the input in the six modes that have a dry path (Granular, Pitch shifter / time stretch, Looping delay, Spectral madness, Oliverb, Spectral clouds), at every
host rate (44.1, 48, 96 kHz), both engine rates and all four qualities. The cause is in the firmware code, not the wrapper: its dry/wet crossfade table starts at 0.707 (-3 dB) and its
output conversion halves the signal (x 0.5, -6 dB) before the 16-bit output. The module's analogue output stage presumably makes that up; a plug-in has none.

**The fix.** A new **OUTPUT LEVEL** knob (parameter `output_level`, -24 to +24 dB, default **+9.03 dB** = 20 x log10 (2 x sqrt 2)) sits after the engine, followed by a **soft limiter**.
With the default, a fully dry signal comes out at the level it went in. The limiter passes everything below -0.9 dBFS unchanged and bends louder peaks towards 0 dBFS, which they
never exceed; it is an instant curve (no delay, no look-ahead), a safety net against clipping rather than a mastering limiter. The gain glides (about 10 ms) so moving or automating the knob does not click.
The engine and the firmware code are untouched, so Live still equals HD sample for sample; the stage lives in `src/OutputStage.h` and is called from `processBlock`.

**What it does not do.**
* **Beat repeat and Resonestor have no dry path in the firmware**, so "fully dry equals the input" does not apply to them. Beat repeat forces its blend to wet and the input always goes through
  the slice player (the firmware applies a 1.2 x, +1.6 dB, gain to it that the other modes' dry signal does not get); Resonestor's Dry/Wet knob is a distortion amount. Measured at the default Output Level, input -18 dBFS:
  Beat repeat **+1.57 dB** (16-bit), Resonestor **-1.61 dB** (16-bit); in the low-fi qualities Beat repeat is -0.2 dB (stereo 8-bit) and Resonestor -2.6 dB. I left those as they are: whether they should be
  trimmed to match is your call.
* **Beat repeat in Mono 8-bit (low-fi) gives no output at all** in this test (-330 dB, at every Dry/Wet setting). It is in the firmware code path and was there before this change; I have not looked into it.
* Loud dry signals are bent slightly by the firmware's own soft limit, before this stage: Looping delay, dry, input -6 dBFS: -0.06 dB; -1.9 dBFS: -0.16 dB; -0.4 dBFS: -0.22 dB.
* The firmware's output is 16-bit and is 9 dB lower than the input before this gain is applied, so the gain also raises the firmware's own 16-bit rounding noise by 9 dB (it does not add any). I have not measured or listened for it in quiet passages.
* **Presets and projects saved before 0.2.0 have no Output Level**, so they load at the +9.03 dB default and come out about 9 dB louder than they did. That is the fix, but if you
  had turned the channel up to compensate, turn it back down.

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
    the new one starts. If you ask for another layer while a fade is running it carries on from where the level is.
  * **SWITCH STYLE** (automatable; only used when Switch Time is above Instant): *Dip* is the fade described above. *Crossfade* starts the incoming layer on a **second engine** about 0.2 s
    before the fade (so its sound is already running, not starting from nothing), then fades the old layer out and the new one in over the Switch Time, at equal power, then the second engine
    becomes the main one. Consequences: the switch happens about 0.2 s after you change layer; CPU is roughly double while a Crossfade lasts (0.2 s plus the Switch Time) and normal otherwise;
    RECORD is refused while a Crossfade is running; clearing or loading into the layer being faded to cancels the Crossfade (the old layer carries on); going to Live, or to an empty layer,
    or in a mode that doesn't play layers, uses the Dip instead. Memory use goes up by the size of one more engine (a few MB).
  * **ALSO PLAY** (a menu: Off / Layer 1..6, plus a level slider; both automatable): plays a **second layer together with the selected one**, on the second engine, added to the main sound at
    the slider's level (a 20 ms ramp when it comes or goes). It is all-wet (it adds only the layer's own sound, not another copy of the live input). Ignored if it is the same layer as the
    selected one, if it is empty, or if the mode doesn't play layers. If you switch the main layer onto the one that is playing as the second, the second one stops (it can't play twice).
    CPU is roughly double while it is on. While it is on, the Crossfade style becomes a Dip (they use the same second engine). RECORD still records the selected (main) layer.
  * **PAN** (next to the ALSO PLAY level, automatable): a balance for the second layer, from hard left to hard right. The centre leaves it exactly as it was (no change in level), and it is
    smoothed so automation doesn't zipper. It only changes the gains of the second layer's output, so it costs no extra CPU.
  * **RECORD** is also a parameter, "Record" (on = recording, off = stopped), so a Logic automation lane or the button do the same thing; it switches itself off when the layer is full.
  * **SAVE WAV** writes the selected layer to a 16-bit WAV file (mono or stereo as the layer is, at the layer's own sample rate: the engine rate, or 16 kHz in the low-fi
    qualities; low-fi layers are decoded to 16-bit first) and **links the layer to that file** (see "Layer files" below). **LOAD WAV** puts a WAV file into the selected layer and links
    it: any sample rate (resampled to the layer's rate), mono or stereo (a mono file goes to both sides of a stereo layer; a stereo file is mixed down for a mono layer),
    8/16/24/32-bit integer or 32/64-bit float. A file longer than the layer is cut to the layer length; a shorter file gives a shorter layer (the rest is silent), just like stopping a
    recording early. Louder than full scale is clipped. A loaded layer behaves exactly like a recorded one, including being replaced by RECORD. Loading into the layer that is playing
    swaps the new audio in with the usual fade. Save and Load hand the audio over through the audio thread, so they complete while the audio is running (Logic keeps running it when the
    transport is stopped); if it is not running the status line says it is waiting. LOAD is refused while a recording is running.

### Layer files (links) and presets
A preset (or a Logic project) saves the settings and, for each linked layer, **a reference to its file, not the audio**: the layer number, the path, the file size and a fingerprint
(a hash of the file's bytes). When the preset is loaded each linked layer is loaded from its file again.
* **Home folder** (the **FOLDER...** button; default `~/Music/Unofficial Layers`): where the Save dialog starts, and where missing files are searched for. It is remembered per computer
  (in a small settings file in the user's Application Support folder), not in presets, so set it once on each Mac. Paths inside the home folder are stored relative to it, so the same
  preset works on another Mac if the home folder contains the same files (for example a Dropbox folder). A file outside the home folder is stored with its full path.
* **Finding files that moved:** if the file isn't at the stored path, the home folder and all its sub-folders are searched for a `.wav` with the same size and the same fingerprint, whatever
  it is now called. A file with the same *name* but different audio is **not** used. If the stored path still has a file but its audio has changed, that file is loaded and the layer is
  marked "(file changed)". If nothing is found the layer stays empty and the status line says "missing: name.wav"; the reference is kept, in case the file comes back.
* **When a link is dropped:** recording over a linked layer or clearing it removes the link (the layer then shows **unsaved** if it holds audio). A layer marked *unsaved* has no file, so it
  will **not** come back with a preset.
* **Format changes:** changing Quality, Buffer Length or Engine Rate still empties the layers, but linked layers are loaded again from their files in the new format; unsaved ones are lost.
* A preset that lists no layers leaves the current layer audio alone but drops the links. Loading a preset does not start a recording (the Record parameter is never saved as on).
* Not done: embedding the audio itself in the preset / project.

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
* **Not saved with the project:** the audio is in memory only; closing the Logic project, or Logic unloading the plug-in, loses it. What a preset or project keeps is the *links to files*
  (see "Layer files"), so a layer must be saved to a file (SAVE WAV) to come back.
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
* `LayerLibrary.*` (new, JUCE-free, std::filesystem): the file links: save / load jobs, the home folder search by size and fingerprint, what a preset stores and restores,
  dropping a link when the user edits a layer, reloading linked layers after a format change. The engine got per-layer revision and edit counters for this.
* `PluginProcessor.*`, `PluginEditor.*`: the Layer menu, RECORD / CLEAR / SAVE WAV / LOAD WAV / FOLDER... buttons, Switch Time slider, a status line per layer, the preset
  state (a `LAYERFILES` element), the home-folder setting, the `record` parameter, a 10 Hz message-thread timer that finishes saves and loads even when the editor is closed.
* **Version 0.2.0:** `OutputStage.h` (new, JUCE-free: Output Level gain with smoothing, soft limiter), the `output_level` parameter, the stage called at the end of `processBlock`, an OUTPUT LEVEL knob in the second knob row
  (five knobs now), `tests/level_test.cpp`, `tests/proc_level_test.cpp`, `tests/mutation_check.sh`, `tests/wiring_mutation_check.sh`, four new workflow steps. Plug-in code `SPLy` unchanged; CMake version 0.1.0 to 0.2.0.
* `tests/layers_test.cpp` and `tests/live_dump.cpp` are new; `tests/bit_exact_dump.cpp` (the Extended comparison) was replaced by `live_dump.cpp`.

## Install
Download `SuperParasites-Layers-macOS` from the latest passing run in the Actions tab and follow `INSTALL.txt`
(universal arm64 + x86_64, macOS 11 or later). The other controls are as in HD.

## Verified and not verified

**Limiter switch (version 0.3.0): built on GitHub Actions (macOS runner), every step passed, run 39075605891, commit `8b5476c`** (all earlier tests, the extended level tests and both mutation checks, the universal arm64 + x86_64 AU and VST3 build, signing, packaging). I can see each step's pass / fail, not its log text. Not yet loaded in Logic and not listened to.

**Limiter switch (version 0.3.0) - measured and tested here (Linux, g++, October 2026), not listened to**
* `tests/level_test.cpp`: 10581 checks, 0 failed (the 0.2.0 checks plus the switch): with the limiter on, a loud signal never passes 1.0; off, the same signal comes out at exactly the Output Level gain (within 0.0001) and
  its peak is past 1.5; a quiet signal is identical on and off; NaN and infinity never get through when off; switching glides (no step above 2 % of the whole move) and settles exactly, and switching back on gives exactly the 0.2.0 value.
* `tests/proc_level_test.cpp`: 31 checks, 0 failed, on the real processor class: the parameter exists and is on by default; a preset saved without it loads with the limiter on; with Output Level at +24 dB and a loud wet input
  (four modes) the peak is 1.0000 with the limiter on and well past 1.0 with it off, so the switch really reaches the sound.
* Mutation checks: `tests/mutation_check.sh` now breaks the stage in 7 ways (the earlier 4 plus switch ignored one way and the other, and the off-guard removed) and `tests/wiring_mutation_check.sh` in 6 ways (the earlier 3 plus
  switch not read one way and the other, and default wrong); every broken copy was caught.
* Not tested: how the switch sounds or looks in Logic, whether the button fits the panel as intended (the row is 606 of 612 pixels wide), Logic's handling of the new parameter in an existing project (the plug-in code is unchanged).

**Output Level (version 0.2.0): every step passed on the macOS runner, run 39047118805, commit `943333d`** (all earlier tests, Live = HD, the four new level steps, the universal arm64 + x86_64 AU and VST3 build, signing, packaging; an earlier run on `50253ab` never started because a workflow step name was not valid YAML, fixed in `943333d`). I can see each step's pass / fail, not its log text. Artifact `SuperParasites-Layers-macOS`.

**Output Level (version 0.2.0) - measured and tested here (Linux, g++, October 2026), not listened to**
* `tests/level_test.cpp`: 10,573 checks, 0 failed. Fully dry (Dry/Wet = 0) at the default Output Level, engine + stage output equals the input level within 0.1 dB in each of the six dry-path modes, at 44.1 / 48 / 96 kHz
  host, both engine rates, all four qualities, input -18 dBFS (and -6 dBFS at 48 kHz): 192 runs, worst deviation 0.064 dB. The limiter: identity up to its threshold, odd-symmetric, monotonic, slope 1 at the threshold,
  never above 1.0 for any input including infinity, NaN becomes 0. The gain is exact to 0.001 dB at -24, -6, 0, +9.03 and +12 dB; a step from +9.03 to -24 dB glides with no jump. No clipping: with a 0.95-amplitude input, all modes
  and qualities, Dry/Wet 0 / 0.5 / 1, Output Level at the default and at +24 dB (192 runs) no sample exceeds 1.0; without the limiter the engine's wet peaks at the default gain would reach about 2.3 x full scale.
* `tests/proc_level_test.cpp`: 24 checks, 0 failed, on the real plug-in processor class (the same `PluginProcessor.cpp` as the AU / VST3, compiled against JUCE 8.0.6, run headless): the parameter exists and its default shows as "+9.03 dB";
  the six dry-path modes come out within 0.013 dB of the input at the defaults; setting Output Level to -24 / -9.03 / 0 / +6 dB moves the output by exactly that much (within 0.02 dB); loud input at +24 dB in every mode stays at or below 1.0;
  a preset saved without an Output Level loads at +9.03 dB (and keeps its other settings), also into an instance whose knob had been turned to 0 dB.
* **Mutation checks (the tests fail when the level is broken).** `tests/mutation_check.sh` breaks `OutputStage.h` in four ways (gain removed, default 8.0 dB instead of 9.03, wrong dB-to-gain factor, limiter removed) and the level test failed for every one
  (17, 13, 19 and 4,035 failed checks), while passing on the unbroken code. `tests/wiring_mutation_check.sh` breaks `processBlock` in three ways (stage not called, level parameter not read, parameter default 0 dB) and the whole-plug-in test failed each
  time (9, 9 and 8 failed checks); the source is restored afterwards. Both are workflow steps.
* The existing suites still pass with the new CMake targets (`engine_test`, `rate_compare_test`; the engine and firmware code are not touched, so the Live = HD check below is unaffected).
* The editor source (the fifth knob) compiles against JUCE 8.0.6 with the rest of the plug-in. **How it looks is not checked: I have not seen the editor.**
* **Not verified:** how it sounds; that the level you read on Logic's meters / faders now matches; the 16-bit noise floor at +9 dB in quiet passages; the knob row in Logic; behaviour when automated; any
  match with the real module's output level; Beat repeat and Resonestor levels against a real module.

**Reported by Bryn, listening in Logic (first Layers build):** it installs and loads without problem on the M4 Mac mini and on the Intel Mac (macOS 12.7); the layers sound clear;
he recorded four layers and automated the Layer menu from a Logic automation lane and it worked well; the layer switch does not click but changes abruptly (which is why Switch Time was
added). **Second build (Switch Time, Save / Load WAV):** the file dialogs and buttons work; a layer keeps droning when Logic's transport is stopped (so Logic keeps running the
audio, which Save and Load need); a preset saved with four layers loaded fine but its layers came back empty and had to be loaded by hand (the second build had no file links; the
links are the third build). On Switch Time he heard a quick fade-out and a slower fade-in, and would prefer the two layers to morph over each other. Nothing here is a claim about
how any of it sounds beyond that.

**Verified here (Linux, g++, October 2026) — the tests in this repo**
* `tests/library_test.cpp` (third build): 73 checks, all pass, with real files in a temporary folder: saving writes the file and links the layer (relative path, size, fingerprint),
  a new instance restoring the saved links loads identical audio; a renamed file moved into a sub-folder is found by fingerprint (with same-named decoys with different audio ignored);
  a missing file is reported and nothing is loaded; a changed file is loaded and flagged; garbage in place of the file fails cleanly; absolute paths work without a home folder;
  recording over or clearing a linked layer drops the link; loading a file links the layer; a format change (stereo to mono, then low-fi with a longer buffer) reloads linked layers
  and loses unsaved ones; a file that vanished before a format change ends as "missing" without retrying forever; restore waits for the engine to be ready; a save waits while no audio
  runs; four saves queued together all complete with different content. I broke the library on purpose in six places (no search by fingerprint; ignoring user edits; no reload after a
  format change; accepting a same-named file; not linking after a save; storing absolute paths only) and the tests failed each time.
* `tests/layers_test.cpp`: 1311 checks, all pass (the 1227 of the fourth build plus two layers together and the pan: centre is in both channels, hard left only the left, hard right only the right, back to centre is as before).
  * *Two layers together (measured, not heard):* in Looping delay and Pitch shifter at both engine rates, with layer 1 selected and ALSO PLAY on layer 2 both tones are heard, level 0.3 gives
    a quieter second tone and level 0 silences it, changing the second layer replaces the old one, Off removes it and layer 1 carries on, the second layer is not doubled when it becomes the
    selected one, a Crossfade request while it is on becomes a Dip and still lands, an emptied second layer goes quiet, stored layers are unchanged.
  * *Crossfade (measured, not heard):* switching layer 1 to 2 with Switch Time 800 ms and Switch Style Crossfade: in Looping delay and Pitch shifter at both engine rates the level never falls
    below 45 % of the steady level (the Dip falls below 10 %), for a while both layers' tones are present at the same time, afterwards only the new one; no step in the output; the first
    ~100 ms still play the old layer alone (the pre-roll); rapid back-and-forth ends on the last layer; RECORD is refused during it (including on the layer still playing); clearing the target
    cancels it; Dip is unchanged; Live and empty targets fall back to the Dip. Granular and Beat repeat: only the mechanics (starts, ends on the new layer, stays finite, layers untouched).
    I broke the Crossfade on purpose in seven places (the second engine not given the layer; the old layer not faded out; no pre-roll; style ignored; no hand-over at the end; recording allowed
    during it; clearing the target not cancelling) and the tests failed each time. Cost on this Linux machine: slowest 512-sample call around Crossfades 1.4 ms (10 s layers) and 3.0 ms (20 s layers),
    against 0.2 ms normally (a call of that size is 10.7 ms of audio). **Mac CPU is not measured.**
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
  the HD plug-in at commit `aaa28f5` the 128 lines are identical (re-run after Switch Time, Save / Load and the Crossfade were added; also run in the workflow).
* The existing suites pass unchanged: `engine_test` and `rate_compare_test`.
* The plug-in and editor sources compile against the JUCE 8.0.6 headers (syntax check, Linux, no warnings; checked that it does report a deliberate error).
* Cost of a layer switch on this Linux machine: slowest 512-sample `process()` call while switching 0.5 ms with 10 s layers and 1.0 ms with 20 s layers (48 kHz stereo).

**Sixth build (Also Play Pan): every step passed on the macOS runner, run 38570854827, commit `be580ee`.**

**Fifth build (Also Play, two layers together): every step passed on the macOS runner, run 38565811679, commit `efd2eab`.** Four deliberate breaks (second layer not mixed in; level ignored; second layer not loaded; same layer allowed twice) were each caught by the tests.

**Fourth build (Switch Style / Crossfade): every step passed on the macOS runner, run 38519039695, commit `f5b9929`** (all tests, Live = HD, arm64 + x86_64 AU and VST3, packaging).

**Verified on GitHub Actions (macOS runner): every step passed for the second build (run 38492051213, commit `d0bd3b1`) and for the third build with file links and the Record parameter (run 38502391893, commit `e2dab61`), including `library_test` with the macOS compiler.** The steps are `engine_test`, `rate_compare_test`, `layers_test`, the Live = HD comparison, the universal
arm64 + x86_64 AU and VST3 build, ad-hoc signing, packaging and the `SuperParasites-Layers-macOS` artifact. (I can see each step's pass / fail result, not its log text.)

**Not verified**
* **ALSO PLAY PAN (sixth build) in Logic and by ear** (Also Play itself you tested: works, balance good). Pan: how it sounds, Mac CPU with it on, and with its menu / level automated.
* **The Crossfade by ear and in Logic** (fourth build): how it sounds, whether the 0.2 s delay and the extra CPU matter on your Macs, and how it behaves with a Layer automation lane.
* **Switch Time measured, not heard.** In the test (four modes, Switch Time 800 ms, including your Granular settings) the fade-out and the fade-in are symmetric: the level is below
  half after about 130-200 ms, quietest at about 400 ms, back above half at about 600 ms and above 90 % by 670-720 ms (Pitch shifter, Looping delay, Beat repeat; Granular is too irregular
  to put numbers on, but its envelope follows the same shape). So the "quick out, slow in" you heard is not a difference in the two fades in the engine; the fade is a dip, not a morph.
* **Switch Time by ear**: whether the fade sounds good, what settings are useful, and the Switch Time slider and the new buttons in a host (the editor has been seen in Bryn's screenshots
  of the first build, not this one; the layout of the extra row is new).
* **File links in Logic** (third build): the FOLDER... button, a preset or project bringing its layers back, files found after being moved, "unsaved" / "missing" in the status line, linked
  layers reloading after a Quality / Buffer Length / Engine Rate change, in a real host. What the host does with a project's plug-in state (when and how often it calls the plug-in's
  save / load of state, and from which thread) has not been observed; the code takes a lock and tolerates repeated calls.
* **The Record parameter in an automation lane**: the button and the parameter share one code path (tested in the layer tests for the engine, not in a host). If an automation lane that stays
  "on" keeps re-asserting the value after the plug-in switches it off at the end of a recording, it could start another recording; not observed.
* The WAV files opening in other programs (only my own reader has read them).
* Large home folders: the search for a moved file reads every `.wav` of the same size under the home folder (up to 50,000 entries) on the plug-in's message thread; with thousands of files it
  could pause the interface for a moment.
* Project save / reopen (the layers are lost by design; the Switch Time value is saved like any parameter).
* CPU use and layer switch / load copy time on a Mac; memory use inside Logic.
* RECORD cannot be automated or MIDI-triggered (it is a button only); Layer and Switch Time are normal automatable parameters.
* Recording into one layer while another plays is not possible (one layer plays at a time, and it is the one being recorded).
* The FEEDBACK / FREEZE behaviour of a playing layer and Beat repeat's slice source (from reading the code, not listened to).

## Planned
* More than two layers together (each extra one would need another engine and its CPU); maybe a pan or a separate level per layer.
* Optionally embedding the audio in the preset / project (about 11.5 MB for six 10 s stereo layers at 48 kHz) as an alternative to file links.

## Source and licence
Same as HD: `third_party/supercell/` is `supercell/dsp/*` and `supercell/resources.*` from `patrickdowling/superparasites` (commit `2e9ed7c`) with the HD changes
(`docs/RATE_AUDIT.md`) and the layer hooks above, `third_party/stmlib/` is a subset of `mqtthiqs/stmlib` at `8ab2aaee77cbacb47b646d46d22ee5d358effe2d`. MIT per that project's README,
with MIT notices in the source files (Emilie Gillet, Matthias Puech, Julius Kammerl, Patrick Dowling); keep them if you redistribute. The wrapper code in `src/` has no licence
assigned yet. JUCE is used under its own terms (AGPLv3 / commercial).
