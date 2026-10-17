#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "OutputStage.h"

using APF = juce::AudioParameterFloat;
using APB = juce::AudioParameterBool;
using APC = juce::AudioParameterChoice;

juce::AudioProcessorValueTreeState::ParameterLayout SuperParasitesAudioProcessor::createLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> p;
    auto unit = [] (const char* id, const char* name, float def)
    {
        return std::make_unique<APF> (juce::ParameterID { id, 1 }, name, juce::NormalisableRange<float> (0.f, 1.f, 0.0001f), def,
                                      juce::AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return juce::String (v, 2); }));
    };

    p.push_back (unit ("position", "Position", 0.5f));
    p.push_back (unit ("size",     "Size",     0.5f));
    // The PITCH knob is the module's own detented knob (about -24..+24 semitones); the text shows the semitones it gives.
    p.push_back (std::make_unique<APF> (juce::ParameterID { "pitch", 1 }, "Pitch",
                 juce::NormalisableRange<float> (0.f, 1.f, 0.0001f), 0.5f,
                 juce::AudioParameterFloatAttributes().withLabel ("st")
                     .withStringFromValueFunction ([] (float v, int) { return juce::String (SuperParasitesEngine::pitchPotToSemitones (v), 1); })));
    p.push_back (unit ("density",  "Density",  0.7f));
    p.push_back (unit ("texture",  "Texture",  0.5f));
    p.push_back (unit ("dry_wet",  "Dry/Wet",  0.5f));
    p.push_back (unit ("spread",   "Stereo Spread", 0.5f));
    p.push_back (unit ("feedback", "Feedback", 0.0f));
    p.push_back (unit ("reverb",   "Reverb",   0.0f));
    // Output Level: makes up the firmware's output attenuation (-9 dB with Dry/Wet at 0) so that a fully dry signal comes out at the input level.
    // A soft limiter after it keeps loud wet sounds from clipping.
    p.push_back (std::make_unique<APF> (juce::ParameterID { "output_level", 1 }, "Output Level",
                 juce::NormalisableRange<float> (OutputStage::kMinLevelDb, OutputStage::kMaxLevelDb, 0.01f), OutputStage::kDefaultLevelDb,
                 juce::AudioParameterFloatAttributes().withLabel ("dB")
                     .withStringFromValueFunction ([] (float v, int) { return (v >= 0.0f ? "+" : "") + juce::String (v, 2) + " dB"; })
                     .withValueFromStringFunction ([] (const juce::String& s) { return s.retainCharacters ("-+.0123456789").getFloatValue(); })));
    p.push_back (std::make_unique<APB> (juce::ParameterID { "freeze", 1 }, "Freeze", false));
    p.push_back (std::make_unique<APB> (juce::ParameterID { "reverse", 1 }, "Reverse (granular)", false));
    p.push_back (std::make_unique<APB> (juce::ParameterID { "gate", 1 }, "Gate", false));
    p.push_back (std::make_unique<APB> (juce::ParameterID { "pitch_midi", 1 }, "Pitch Follows MIDI", false));
    p.push_back (std::make_unique<APC> (juce::ParameterID { "mode", 1 }, "Mode",
                 juce::StringArray { "Granular", "Pitch shifter / time stretch", "Looping delay", "Spectral madness",
                                     "Oliverb", "Resonestor", "Beat repeat", "Spectral clouds" }, 0));
    p.push_back (std::make_unique<APC> (juce::ParameterID { "quality", 1 }, "Quality",
                 juce::StringArray { "Stereo, 16-bit", "Mono, 16-bit", "Stereo, 8-bit (low-fi)", "Mono, 8-bit (low-fi)" }, 0));
    p.push_back (std::make_unique<APC> (juce::ParameterID { "buffer_length", 1 }, "Buffer Length",
                 juce::StringArray { "1 s (as the module)", "4 s", "10 s", "20 s" }, 2));
    p.push_back (std::make_unique<APC> (juce::ParameterID { "engine_rate", 1 }, "Engine Rate",
                 juce::StringArray { "32 kHz (original)", "48 kHz (HD)" }, 0));
    p.push_back (std::make_unique<APC> (juce::ParameterID { "layer", 1 }, "Layer",
                 juce::StringArray { "Live", "Layer 1", "Layer 2", "Layer 3", "Layer 4", "Layer 5", "Layer 6" }, 0));
    // Record: on = record into the selected layer (same as the RECORD button); it switches itself off when the layer is full. Automatable.
    p.push_back (std::make_unique<APB> (juce::ParameterID { "record", 1 }, "Record", false));
    // How long a layer switch takes. 0 = the short instant swap; above 0 the sound fades out, the layer is swapped, and it fades back in.
    p.push_back (std::make_unique<APF> (juce::ParameterID { "switch_time", 1 }, "Switch Time",
                 juce::NormalisableRange<float> (0.f, SuperParasitesEngine::kMaxSwitchMs, 1.f, 0.4f), 0.f,
                 juce::AudioParameterFloatAttributes()
                     .withStringFromValueFunction ([] (float v, int)
                     {
                         if (v < 0.5f) return juce::String ("Instant");
                         return v < 1000.f ? juce::String ((int) std::lround (v)) + " ms" : juce::String (v / 1000.f, 2) + " s";
                     })
                     .withValueFromStringFunction ([] (const juce::String& s)
                     {
                         const auto t = s.trim().toLowerCase();
                         if (t.startsWith ("inst") || t.isEmpty()) return 0.f;
                         const float x = t.getFloatValue();
                         return t.endsWith ("ms") ? x : (t.endsWith ("s") ? x * 1000.f : x);
                     })));
    // How a switch with Switch Time above 0 sounds: Dip = fade out, swap, fade in; Crossfade = the new layer is started on a second engine
    // a moment earlier and the two are cross-faded (about double the CPU while it lasts).
    p.push_back (std::make_unique<APC> (juce::ParameterID { "switch_style", 1 }, "Switch Style",
                 juce::StringArray { "Dip", "Crossfade" }, 0));
    // A second layer played together with the first (on the second engine), and its level.
    p.push_back (std::make_unique<APC> (juce::ParameterID { "layer_b", 1 }, "Also Play Layer",
                 juce::StringArray { "Off", "Layer 1", "Layer 2", "Layer 3", "Layer 4", "Layer 5", "Layer 6" }, 0));
    p.push_back (std::make_unique<APF> (juce::ParameterID { "layer_b_level", 1 }, "Also Play Level",
                 juce::NormalisableRange<float> (0.f, 1.f, 0.001f), 0.8f));
    p.push_back (std::make_unique<APF> (juce::ParameterID { "layer_b_pan", 1 }, "Also Play Pan",
                 juce::NormalisableRange<float> (-1.f, 1.f, 0.001f), 0.f,
                 juce::AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int)
                 {
                     if (std::abs (v) < 0.005f) return juce::String ("Centre");
                     return juce::String ((int) std::lround (std::abs (v) * 100.f)) + (v < 0.f ? "% L" : "% R");
                 })));
    return { p.begin(), p.end() };
}

namespace
{
    std::unique_ptr<juce::PropertiesFile> openSettings()
    {
        juce::PropertiesFile::Options o;
        o.applicationName = "Unofficial SuperParasites Layers";
        o.folderName = "particledots";
        o.filenameSuffix = ".settings";
        o.osxLibrarySubFolder = "Application Support";
        o.storageFormat = juce::PropertiesFile::storeAsXML;
        return std::make_unique<juce::PropertiesFile> (o);
    }
}

namespace
{
    juce::File loadHomeFolder()
    {
        if (auto s = openSettings())
        {
            const auto v = s->getValue ("homeFolder");
            if (v.isNotEmpty()) return juce::File (v);
        }
        return juce::File::getSpecialLocation (juce::File::userMusicDirectory).getChildFile ("Unofficial Layers");
    }
}

SuperParasitesAudioProcessor::SuperParasitesAudioProcessor()
    : AudioProcessor (BusesProperties().withInput ("Input", juce::AudioChannelSet::stereo(), true)
                                       .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "STATE", createLayout())
{
    homeFolder = loadHomeFolder();
    library.setHomeFolder (homeFolder.getFullPathName().toStdString());
    startTimerHz (10);
}

SuperParasitesAudioProcessor::~SuperParasitesAudioProcessor() { stopTimer(); }

juce::File SuperParasitesAudioProcessor::getHomeFolder() const { return homeFolder; }

void SuperParasitesAudioProcessor::setHomeFolder (const juce::File& f)
{
    homeFolder = f;
    if (auto s = openSettings()) { s->setValue ("homeFolder", f.getFullPathName()); s->saveIfNeeded(); }
    library.setHomeFolder (f.getFullPathName().toStdString());
}

bool SuperParasitesAudioProcessor::anyLayerRecording() const
{
    for (int i = 1; i <= SuperParasitesEngine::kNumLayers; ++i)
        if (engine.layerStatus (i).state == SuperParasitesEngine::LayerRecording) return true;
    return false;
}

// Runs on the message thread about 10 times a second, whether or not the editor is open: finishes saves and loads, reloads layers after
// a format change, and switches the Record parameter off once a recording has finished by itself.
void SuperParasitesAudioProcessor::timerCallback()
{
    library.tick();
    auto* rec = apvts.getParameter ("record");
    if (rec != nullptr && rec->getValue() > 0.5f)
    {
        if (anyLayerRecording()) { recordSeen = true; recordWaitTicks = 0; }
        else if (recordSeen || ++recordWaitTicks > 12)       // finished by itself, or it never started (Live selected, a mode without layers)
        {
            rec->beginChangeGesture(); rec->setValueNotifyingHost (0.0f); rec->endChangeGesture();
            recordSeen = false; recordWaitTicks = 0;
        }
    }
    else { recordSeen = false; recordWaitTicks = 0; }
}

bool SuperParasitesAudioProcessor::isBusesLayoutSupported (const BusesLayout& l) const
{
    const auto out = l.getMainOutputChannelSet();
    return (out == juce::AudioChannelSet::stereo() || out == juce::AudioChannelSet::mono())
           && l.getMainInputChannelSet() == out;
}

void SuperParasitesAudioProcessor::prepareToPlay (double sr, int maxBlock)
{
    engine.prepare (sr);
    outputStage.prepare (sr);
    scratchL.assign ((size_t) maxBlock, 0.f);
    scratchR.assign ((size_t) maxBlock, 0.f);
    inL.assign ((size_t) maxBlock, 0.f);
    inR.assign ((size_t) maxBlock, 0.f);
    captureRequested.store (false);
    reportedLatency = engine.latencyInHostSamples();
    setLatencySamples (reportedLatency);
}

void SuperParasitesAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    const int n = buffer.getNumSamples();
    const int chans = buffer.getNumChannels();

    if ((size_t) n > scratchL.size())
    {   // host exceeded its promised max block size
        scratchL.resize ((size_t) n); scratchR.resize ((size_t) n);
        inL.resize ((size_t) n);      inR.resize ((size_t) n);
    }

    auto v = [this] (const char* id) { return apvts.getRawParameterValue (id)->load(); };

    // MIDI note-on = the module's CAPTURE input; a held note = its GATE input. The last note can also transpose the pitch (note 60 = no change).
    bool cap = captureRequested.exchange (false);
    for (const auto meta : midi)
    {
        const auto msg = meta.getMessage();
        if (msg.isNoteOn())       { cap = true; lastMidiNote = msg.getNoteNumber(); ++heldNotes; }
        else if (msg.isNoteOff()) { heldNotes = juce::jmax (0, heldNotes - 1); }
        else if (msg.isAllNotesOff() || msg.isAllSoundOff()) { heldNotes = 0; }
    }
    midi.clear();
    if (cap) engine.capture();

    SuperParasitesEngine::Params prm;
    prm.position = v ("position"); prm.size = v ("size"); prm.density = v ("density"); prm.texture = v ("texture");
    prm.pitchPot = v ("pitch");
    prm.pitchOffsetSemitones = v ("pitch_midi") > 0.5f ? (float) (lastMidiNote - 60) : 0.0f;
    prm.dryWet = v ("dry_wet"); prm.stereoSpread = v ("spread"); prm.feedback = v ("feedback"); prm.reverb = v ("reverb");
    prm.freeze = v ("freeze") > 0.5f;
    prm.reverse = v ("reverse") > 0.5f;
    prm.gate = v ("gate") > 0.5f || heldNotes > 0;
    prm.mode = (int) v ("mode");
    prm.quality = (int) v ("quality");
    prm.bufferLength = (int) v ("buffer_length");
    prm.engineRate = (int) v ("engine_rate");
    prm.layer = (int) v ("layer");
    prm.switchTimeMs = v ("switch_time");
    prm.switchStyle = (int) v ("switch_style");
    prm.layerB = (int) v ("layer_b");
    prm.layerBLevel = v ("layer_b_level");
    prm.layerBPan = v ("layer_b_pan");

    // Record parameter: a rising edge starts recording into the selected layer, a falling edge stops it (the engine's call toggles).
    const bool recNow = v ("record") > 0.5f;
    if (recNow != lastRecordParam)
    {
        const bool recording = anyLayerRecording();
        if (recNow != recording) engine.recordLayer();
        lastRecordParam = recNow;
    }

    const float* l = buffer.getReadPointer (0);
    const float* r = buffer.getReadPointer (chans > 1 ? 1 : 0);
    std::copy (l, l + n, inL.begin());
    std::copy (r, r + n, inR.begin());

    engine.process (inL.data(), inR.data(), scratchL.data(), scratchR.data(), n, prm);
    if (engine.latencyInHostSamples() != reportedLatency)   // the engine rate changes the latency
    {
        reportedLatency = engine.latencyInHostSamples();
        setLatencySamples (reportedLatency);
    }
    buffer.copyFrom (0, 0, scratchL.data(), n);
    if (chans > 1) buffer.copyFrom (1, 0, scratchR.data(), n);

    // Output Level and the soft limiter come last, after the engine, so the engine's output is untouched (Live still equals HD sample for sample).
    outputStage.setLevelDb (v ("output_level"));
    outputStage.process (buffer.getArrayOfWritePointers(), juce::jmin (chans, 2), n);
}

juce::AudioProcessorEditor* SuperParasitesAudioProcessor::createEditor()
{
    return new SuperParasitesEditor (*this);
}

void SuperParasitesAudioProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    auto state = apvts.copyState();
    for (auto child : state)                         // never store "recording" as on: loading a preset must not start a recording
        if (child.hasType ("PARAM") && child.getProperty ("id").toString() == "record") child.setProperty ("value", 0.0f, nullptr);
    if (auto xml = state.createXml())
    {
        // The layers' files (not their audio): which layer, where the file is (relative to the home folder when it is inside it), its size
        // and its fingerprint, so the file can be found again if it was moved.
        const auto refs = library.bindings();
        if (! refs.empty())
        {
            auto* node = xml->createNewChildElement ("LAYERFILES");
            for (const auto& [layer, b] : refs)
            {
                auto* e = node->createNewChildElement ("LAYER");
                e->setAttribute ("index", layer);
                e->setAttribute ("path", juce::String::fromUTF8 (b.path.c_str()));
                e->setAttribute ("size", juce::String ((juce::int64) b.size));
                e->setAttribute ("hash", juce::String::toHexString ((juce::int64) b.hash));
            }
        }
        copyXmlToBinary (*xml, dest);
    }
}

void SuperParasitesAudioProcessor::setStateInformation (const void* data, int size)
{
    if (auto xml = getXmlFromBinary (data, size))
        if (xml->hasTagName (apvts.state.getType()))
        {
            std::vector<std::pair<int, LayerLibrary::Binding>> refs;
            if (auto* node = xml->getChildByName ("LAYERFILES"))
            {
                for (auto* e : node->getChildIterator())
                {
                    LayerLibrary::Binding b;
                    b.path = e->getStringAttribute ("path").toStdString();
                    b.size = (uint64_t) e->getStringAttribute ("size").getLargeIntValue();
                    b.hash = (uint64_t) e->getStringAttribute ("hash").getHexValue64();
                    const int layer = e->getIntAttribute ("index");
                    if (layer >= 1 && layer <= SuperParasitesEngine::kNumLayers && ! b.path.empty()) refs.push_back ({ layer, b });
                }
                xml->removeChildElement (node, true);
            }
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
            if (auto* rec = apvts.getParameter ("record")) rec->setValueNotifyingHost (0.0f);
            library.restore (refs);
        }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new SuperParasitesAudioProcessor(); }
