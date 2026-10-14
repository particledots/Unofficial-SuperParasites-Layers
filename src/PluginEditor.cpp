#include "PluginEditor.h"
#include "LayerAudio.h"

namespace
{
    const juce::Colour kBg      (0xff1a1c20);
    const juce::Colour kPanel   (0xff23262c);
    const juce::Colour kTrack   (0xff3a3e47);
    const juce::Colour kAccent  (0xff5bb5a6);
    const juce::Colour kText    (0xffe8e6e1);
    const juce::Colour kDim     (0xff8d909a);
}

SuperParasitesLookAndFeel::SuperParasitesLookAndFeel()
{
    setColour (juce::Slider::textBoxTextColourId, kText);
    setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    setColour (juce::Label::textColourId, kDim);
    setColour (juce::ComboBox::backgroundColourId, kPanel);
    setColour (juce::ComboBox::textColourId, kText);
    setColour (juce::ComboBox::outlineColourId, kTrack);
    setColour (juce::ComboBox::arrowColourId, kAccent);
    setColour (juce::PopupMenu::backgroundColourId, kPanel);
    setColour (juce::PopupMenu::textColourId, kText);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, kAccent);
    setColour (juce::PopupMenu::highlightedTextColourId, juce::Colours::black);
}

void SuperParasitesLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h, float pos,
                                         float a0, float a1, juce::Slider& s)
{
    auto r = juce::Rectangle<float> ((float) x, (float) y, (float) w, (float) h).reduced (6.0f);
    const float d = juce::jmin (r.getWidth(), r.getHeight());
    r = juce::Rectangle<float> (d, d).withCentre (r.getCentre());
    const float thick = juce::jmax (3.0f, d * 0.09f);
    const auto arcR = r.reduced (thick * 0.5f);

    juce::Path track;
    track.addCentredArc (arcR.getCentreX(), arcR.getCentreY(), arcR.getWidth() * 0.5f, arcR.getHeight() * 0.5f, 0, a0, a1, true);
    g.setColour (kTrack);
    g.strokePath (track, juce::PathStrokeType (thick, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    const float angle = a0 + pos * (a1 - a0);
    // bipolar controls fill from the centre, others from the start
    const bool bipolar = (bool) s.getProperties().getWithDefault ("bipolar", false);
    const float from = bipolar ? (a0 + a1) * 0.5f : a0;
    juce::Path val;
    val.addCentredArc (arcR.getCentreX(), arcR.getCentreY(), arcR.getWidth() * 0.5f, arcR.getHeight() * 0.5f, 0,
                       juce::jmin (from, angle), juce::jmax (from, angle), true);
    g.setColour (s.isEnabled() ? kAccent : kDim);
    g.strokePath (val, juce::PathStrokeType (thick, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    // pointer
    const auto c = r.getCentre();
    const float len = d * 0.5f - thick * 2.2f;
    juce::Line<float> line (c.getPointOnCircumference (len * 0.35f, angle),
                            c.getPointOnCircumference (len, angle));
    g.setColour (kText);
    g.drawLine (line, juce::jmax (2.0f, d * 0.045f));
}

void SuperParasitesLookAndFeel::drawToggleButton (juce::Graphics& g, juce::ToggleButton& b, bool, bool)
{
    auto r = b.getLocalBounds().toFloat().reduced (2.0f);
    const bool on = b.getToggleState();
    g.setColour (on ? kAccent : kPanel);
    g.fillRoundedRectangle (r, 6.0f);
    g.setColour (on ? kAccent : kTrack);
    g.drawRoundedRectangle (r, 6.0f, 1.2f);
    g.setColour (on ? juce::Colours::black : kText);
    g.setFont (juce::Font (juce::FontOptions (13.0f, juce::Font::bold)));
    g.drawText (b.getButtonText(), b.getLocalBounds(), juce::Justification::centred);
}

SuperParasitesEditor::SuperParasitesEditor (SuperParasitesAudioProcessor& p) : AudioProcessorEditor (&p), proc (p)
{
    setLookAndFeel (&laf);

    addKnob (position, "position", "POSITION");
    addKnob (size,     "size",     "SIZE");
    addKnob (pitch,    "pitch",    "PITCH");
    addKnob (density,  "density",  "DENSITY");
    addKnob (texture,  "texture",  "TEXTURE");
    addKnob (dryWet,   "dry_wet",  "DRY/WET");
    addKnob (spread,   "spread",   "STEREO SPREAD");
    addKnob (feedback, "feedback", "FEEDBACK");
    addKnob (reverb,   "reverb",   "REVERB");

    pitch.slider.getProperties().set ("bipolar", true);
    addMenu (mode,    "mode",    "MODE",    { "Granular", "Pitch shifter / time stretch", "Looping delay", "Spectral madness",
                                              "Oliverb", "Resonestor", "Beat repeat", "Spectral clouds" });
    addMenu (bufferLength, "buffer_length", "BUFFER LENGTH", { "1 s (as the module)", "4 s", "10 s", "20 s" });
    addMenu (engineRate, "engine_rate", "ENGINE RATE", { "32 kHz (original)", "48 kHz (HD)" });
    addMenu (quality, "quality", "QUALITY", { "Stereo, 16-bit", "Mono, 16-bit", "Stereo, 8-bit (low-fi)", "Mono, 8-bit (low-fi)" });

    addAndMakeVisible (freeze);
    addAndMakeVisible (reverse);
    addAndMakeVisible (gate);
    addAndMakeVisible (pitchMidi);
    freezeAtt    = std::make_unique<ButtonAtt> (proc.apvts, "freeze", freeze);
    reverseAtt   = std::make_unique<ButtonAtt> (proc.apvts, "reverse", reverse);
    gateAtt      = std::make_unique<ButtonAtt> (proc.apvts, "gate", gate);
    pitchMidiAtt = std::make_unique<ButtonAtt> (proc.apvts, "pitch_midi", pitchMidi);

    capture.onClick = [this] { proc.requestCapture(); };
    capture.setColour (juce::TextButton::buttonColourId, kPanel);
    capture.setColour (juce::TextButton::buttonOnColourId, kAccent);
    capture.setColour (juce::TextButton::textColourOffId, kText);
    addAndMakeVisible (capture);

    addMenu (layerBMenu, "layer_b", "ALSO PLAY", { "Off", "Layer 1", "Layer 2", "Layer 3", "Layer 4", "Layer 5", "Layer 6" });
    layerBMenu.label.setVisible (false);
    layerBMenu.box.setTooltip ("Plays a second layer together with the selected one (on the second engine, so the CPU use goes up while it is on). Ignored if it is the same layer, empty, or the mode doesn't play layers. A Crossfade becomes a Dip while this is on.");
    addKnob (layerBLevel, "layer_b_level", "ALSO PLAY");
    layerBLevel.slider.setSliderStyle (juce::Slider::LinearHorizontal);
    layerBLevel.slider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 60, 20);
    layerBLevel.slider.setColour (juce::Slider::trackColourId, kAccent);
    layerBLevel.slider.setColour (juce::Slider::thumbColourId, kText);
    layerBLevel.slider.setColour (juce::Slider::backgroundColourId, kTrack);
    layerBLevel.label.setJustificationType (juce::Justification::centredLeft);
    layerBLevel.slider.setTooltip ("Level of the second layer.");
    addMenu (styleMenu, "switch_style", "SWITCH STYLE", { "Dip", "Crossfade" });
    styleMenu.label.setVisible (false);
    styleMenu.box.setTooltip ("Dip: the sound fades out, the layer is swapped, then it fades in. Crossfade: the new layer is started a moment earlier on a second engine and the two are faded over each other (more CPU while it lasts; the switch happens about 0.2 s after you change layer). Only used when Switch Time is above 0.");
    addMenu (layerMenu, "layer", "LAYER", { "Live", "Layer 1", "Layer 2", "Layer 3", "Layer 4", "Layer 5", "Layer 6" });
    recordLayer.onClick = [this]
    {   // RECORD is the automatable "Record" parameter, so the button and a Logic automation lane do the same thing
        if (auto* rec = proc.apvts.getParameter ("record"))
        {
            rec->beginChangeGesture(); rec->setValueNotifyingHost (rec->getValue() > 0.5f ? 0.0f : 1.0f); rec->endChangeGesture();
        }
    };
    clearLayer.onClick  = [this] { proc.requestClearLayer(); };
    saveLayer.onClick   = [this] { startSave(); };
    loadLayer.onClick   = [this] { startLoad(); };
    homeButton.onClick  = [this] { chooseHome(); };
    for (auto* b : { &recordLayer, &clearLayer, &saveLayer, &loadLayer, &homeButton })
    {
        b->setColour (juce::TextButton::buttonColourId, kPanel);
        b->setColour (juce::TextButton::buttonOnColourId, kAccent);
        b->setColour (juce::TextButton::textColourOffId, kText);
        addAndMakeVisible (*b);
    }
    layerStatusLabel.setJustificationType (juce::Justification::centredLeft);
    layerStatusLabel.setFont (juce::Font (juce::FontOptions (12.0f, juce::Font::bold)));
    layerStatusLabel.setColour (juce::Label::textColourId, kText);
    addAndMakeVisible (layerStatusLabel);
    for (auto& l : layerLines)
    {
        l.setJustificationType (juce::Justification::centredLeft);
        l.setFont (juce::Font (juce::FontOptions (12.0f)));
        l.setMinimumHorizontalScale (0.8f);
        addAndMakeVisible (l);
    }
    homeLabel.setJustificationType (juce::Justification::centredLeft);
    homeLabel.setFont (juce::Font (juce::FontOptions (11.0f)));
    homeLabel.setColour (juce::Label::textColourId, kDim);
    addAndMakeVisible (homeLabel);
    layerNote.setJustificationType (juce::Justification::topLeft);
    layerNote.setFont (juce::Font (juce::FontOptions (11.0f)));
    layerNote.setColour (juce::Label::textColourId, kDim);
    layerNote.setText ("Pick a layer, press RECORD (it clears the layer and records until it is full; press again to stop early; RECORD can be automated). "
                       "SAVE WAV writes the layer to a WAV file and links the layer to it; LOAD WAV puts a WAV file into the layer (any sample rate, mono or stereo; longer files are cut). "
                       "Presets and projects remember the links, not the audio: the files are found again by name or, if they were moved, by looking through the FOLDER... home folder. "
                       "A layer marked 'unsaved' has no file, so it will not come back with a preset. Changing Quality, Buffer Length or Engine Rate reloads linked layers from their files and empties the others. "
                       "SWITCH TIME: Instant = a very short swap; higher = fade out, swap, fade in. Layers play in Granular, Pitch shifter / time stretch, Looping delay and Beat repeat, held like FREEZE.", juce::dontSendNotification);
    addAndMakeVisible (layerNote);

    addKnob (switchTime, "switch_time", "SWITCH TIME");
    switchTime.slider.setSliderStyle (juce::Slider::LinearHorizontal);
    switchTime.slider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 80, 20);
    switchTime.slider.setColour (juce::Slider::trackColourId, kAccent);
    switchTime.slider.setColour (juce::Slider::thumbColourId, kText);
    switchTime.slider.setColour (juce::Slider::backgroundColourId, kTrack);
    switchTime.label.setJustificationType (juce::Justification::centredLeft);
    switchTime.slider.setTooltip ("0 = instant (a very short swap). Above 0 the sound fades out, the layer is swapped, and it fades back in over this total time.");
    ioLabel.setJustificationType (juce::Justification::centredLeft);
    ioLabel.setFont (juce::Font (juce::FontOptions (12.0f, juce::Font::bold)));
    ioLabel.setColour (juce::Label::textColourId, kAccent);
    addAndMakeVisible (ioLabel);
    startTimerHz (10);

    hint.setJustificationType (juce::Justification::topLeft);
    hint.setFont (juce::Font (juce::FontOptions (11.0f)));
    hint.setColour (juce::Label::textColourId, kDim);
    addAndMakeVisible (hint);
    updateHint ((int) *proc.apvts.getRawParameterValue ("mode"));
    modeWatch = std::make_unique<juce::ParameterAttachment> (
        *proc.apvts.getParameter ("mode"), [this] (float v) { updateHint ((int) v); });
    modeWatch->sendInitialUpdate();

    setSize (660, 860);
}

SuperParasitesEditor::~SuperParasitesEditor() { stopTimer(); setLookAndFeel (nullptr); }

void SuperParasitesEditor::timerCallback()
{
    const int layer = (int) *proc.apvts.getRawParameterValue ("layer");
    const int mode  = (int) *proc.apvts.getRawParameterValue ("mode");
    const bool recordingNow = proc.anyLayerRecording();
    auto& lib = proc.layerLibrary();

    juce::String head;
    if (layer == 0)
        head = "Live: the always-recording buffer (no layer is playing).";
    else if (! SuperParasitesEngine::modeUsesLayers (mode))
        head = "Layer " + juce::String (layer) + " is kept safe; this mode works on the live input. Layers play in Granular, Pitch shifter / time stretch, Looping delay and Beat repeat.";
    else if (proc.loadedLayer() != layer)
        head = "Layer " + juce::String (layer) + ": switching...";
    else
    {
        const auto s = proc.layerStatus (layer);
        head = s.state == SuperParasitesEngine::LayerRecording ? "Layer " + juce::String (layer) + ": recording..."
             : s.state == SuperParasitesEngine::LayerFilled    ? "Layer " + juce::String (layer) + ": playing"
                                                               : "Layer " + juce::String (layer) + ": empty - press RECORD, or LOAD WAV";
    }
    layerStatusLabel.setText (head, juce::dontSendNotification);

    // one line per layer: length, and the file it is linked to (or "unsaved")
    for (int i = 1; i <= SuperParasitesEngine::kNumLayers; ++i)
    {
        const auto s = proc.layerStatus (i);
        const auto li = lib.info (i);
        juce::String t = juce::String (i) + "   ";
        auto colour = kText;
        if (s.state == SuperParasitesEngine::LayerRecording) { t << "recording..."; colour = juce::Colour (0xffd9776f); }
        else if (s.state == SuperParasitesEngine::LayerFilled) t << juce::String (s.seconds, 1) << " s";
        else { t << "empty"; colour = kDim; }
        if (li.status == LayerLibrary::Missing || li.status == LayerLibrary::Failed) { t << "   " << juce::String::fromUTF8 (li.message.c_str()); colour = juce::Colour (0xffd9776f); }
        else if (li.status == LayerLibrary::Waiting || li.status == LayerLibrary::Working) { t << "   " << juce::String::fromUTF8 (li.message.c_str()); colour = kAccent; }
        else if (li.bound) t << "   " << juce::String::fromUTF8 (li.fileName.c_str()) << (li.status == LayerLibrary::Changed ? "  (file changed)" : "");
        else if (li.unsaved) { t << "   unsaved"; colour = juce::Colour (0xffe0b050); }
        if (i == layer) t = juce::String (juce::CharPointer_UTF8 ("\xe2\x96\xb6 ")) + t;
        layerLines[i - 1].setText (t, juce::dontSendNotification);
        layerLines[i - 1].setColour (juce::Label::textColourId, colour);
    }
    const auto home = proc.getHomeFolder();
    auto shown = home.getFullPathName();
    const auto userHome = juce::File::getSpecialLocation (juce::File::userHomeDirectory).getFullPathName();
    if (shown.startsWith (userHome)) shown = "~" + shown.substring (userHome.length());
    homeLabel.setText ("Home folder: " + shown, juce::dontSendNotification);

    if (ioMessageTicks > 0 && --ioMessageTicks == 0) ioLabel.setText ({}, juce::dontSendNotification);
    const bool recParamOn = proc.apvts.getRawParameterValue ("record")->load() > 0.5f;
    saveLayer.setEnabled (layer > 0 && proc.layerStatus (juce::jmax (1, layer)).state == SuperParasitesEngine::LayerFilled);
    loadLayer.setEnabled (layer > 0 && ! recordingNow && proc.layerFormat().valid);
    recordLayer.setButtonText (recordingNow || recParamOn ? "STOP" : "RECORD");
    recordLayer.setColour (juce::TextButton::buttonColourId, recordingNow || recParamOn ? juce::Colour (0xffb5413a) : kPanel);
    recordLayer.setEnabled (layer > 0 && SuperParasitesEngine::modeUsesLayers (mode));
    clearLayer.setEnabled (layer > 0);
}

void SuperParasitesEditor::showMessage (const juce::String& text)
{
    ioLabel.setText (text, juce::dontSendNotification);
    ioMessageTicks = 90;     // about 9 seconds
}

void SuperParasitesEditor::startSave()
{
    const int layer = (int) *proc.apvts.getRawParameterValue ("layer");
    if (layer < 1) return;
    const auto home = proc.getHomeFolder();
    home.createDirectory();
    chooser = std::make_unique<juce::FileChooser> ("Save layer " + juce::String (layer) + " as a WAV file (the layer is then linked to it)",
                                                   home.getChildFile (juce::String::fromUTF8 (proc.layerLibrary().suggestedFileName (layer).c_str())), "*.wav");
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::warnAboutOverwriting,
                          [this, layer] (const juce::FileChooser& fc)
    {
        auto f = fc.getResult();
        if (f == juce::File()) return;                         // cancelled
        if (! f.hasFileExtension ("wav")) f = f.withFileExtension ("wav");
        std::string why;
        if (! proc.layerLibrary().saveLayer (layer, f.getFullPathName().toStdString(), why)) showMessage (juce::String::fromUTF8 (why.c_str()));
        else showMessage ("Saving " + f.getFileName() + "...");
    });
}

void SuperParasitesEditor::startLoad()
{
    const int layer = (int) *proc.apvts.getRawParameterValue ("layer");
    if (layer < 1) return;
    chooser = std::make_unique<juce::FileChooser> ("Load a WAV file into layer " + juce::String (layer) + " (the layer is then linked to it)", proc.getHomeFolder(), "*.wav");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [this, layer] (const juce::FileChooser& fc)
    {
        const auto f = fc.getResult();
        if (f == juce::File()) return;                         // cancelled
        std::string why;
        if (! proc.layerLibrary().loadLayer (layer, f.getFullPathName().toStdString(), why)) showMessage (juce::String::fromUTF8 (why.c_str()));
        else showMessage ("Loading " + f.getFileName() + "...");
    });
}

void SuperParasitesEditor::chooseHome()
{
    chooser = std::make_unique<juce::FileChooser> ("Choose the home folder for layer files (saved layers go here by default; "
                                                   "moved files are searched for here)", proc.getHomeFolder(), "");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                          [this] (const juce::FileChooser& fc)
    {
        const auto d = fc.getResult();
        if (d == juce::File() || ! d.isDirectory()) return;
        proc.setHomeFolder (d);
        showMessage ("Home folder set.");
    });
}

void SuperParasitesEditor::updateHint (int mode)
{
    // What the knobs do in each mode, read from the firmware source (not checked against a real module).
    static const char* text[] = {
        "Granular: DENSITY sets grain overlap and whether grains are random or repeatable; TEXTURE sets the grain window and then the diffusion.",
        "Pitch shifter / time stretch: TEXTURE is a low-pass / high-pass filter (centre = off), DENSITY adds diffusion. CAPTURE taps a tempo.",
        "Looping delay: TEXTURE is a low-pass / high-pass filter (centre = off), DENSITY adds diffusion, PITCH shifts the loop. CAPTURE taps a tempo.",
        "Spectral madness: SIZE warps the spectrum, DENSITY sets the refresh rate and phase randomisation, TEXTURE quantises it. GATE glitches it.",
        "Oliverb (Parasites reverb): POSITION is pre-delay, SIZE room size, DENSITY decay, TEXTURE a filter, PITCH shifts the reverb. STEREO SPREAD = diffusion, FEEDBACK = modulation rate, REVERB = modulation depth. FREEZE holds the tail.",
        "Resonestor (resonator bank): CAPTURE strikes it. PITCH tunes it, SIZE picks the chord, DENSITY is the feedback/decay, TEXTURE the damping or narrowness, POSITION shapes the burst. DRY/WET = distortion, FEEDBACK = harmonicity, REVERB = spread, STEREO SPREAD = stereo/separation.",
        "Beat repeat (Kammerl): CAPTURE starts a slice. DRY/WET = chance a slice fires, STEREO SPREAD = clock divider, FEEDBACK = playback pattern, POSITION/SIZE set the loop, DENSITY scales it, TEXTURE moves the slice, REVERB = slice feedback.",
        "Spectral clouds: POSITION is how many bands survive (all muted at 0), SIZE the band spacing, DENSITY how smoothly bands change, TEXTURE randomises phase, STEREO SPREAD = how often bands re-roll. CAPTURE re-rolls them now."
    };
    hint.setText (text[juce::jlimit (0, 7, mode)], juce::dontSendNotification);
}

void SuperParasitesEditor::addKnob (Knob& k, const juce::String& id, const juce::String& name)
{
    k.slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    k.slider.setRotaryParameters (juce::degreesToRadians (225.0f), juce::degreesToRadians (495.0f), true);
    k.slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 70, 16);
    k.slider.setDoubleClickReturnValue (true, proc.apvts.getParameter (id)->convertFrom0to1 (proc.apvts.getParameter (id)->getDefaultValue()));
    k.slider.setPopupDisplayEnabled (false, false, nullptr);
    k.slider.setNumDecimalPlacesToDisplay (2);
    k.slider.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    k.slider.setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
    k.slider.setColour (juce::Slider::textBoxTextColourId, kText);
    k.label.setText (name, juce::dontSendNotification);
    k.label.setJustificationType (juce::Justification::centred);
    k.label.setFont (juce::Font (juce::FontOptions (12.0f, juce::Font::bold)));
    addAndMakeVisible (k.slider);
    addAndMakeVisible (k.label);
    k.att = std::make_unique<SliderAtt> (proc.apvts, id, k.slider);
}

void SuperParasitesEditor::addMenu (Menu& m, const juce::String& id, const juce::String& name, const juce::StringArray& items)
{
    m.box.addItemList (items, 1);
    m.label.setText (name, juce::dontSendNotification);
    m.label.setJustificationType (juce::Justification::centredLeft);
    m.label.setFont (juce::Font (juce::FontOptions (11.0f, juce::Font::bold)));
    addAndMakeVisible (m.box);
    addAndMakeVisible (m.label);
    m.att = std::make_unique<ComboAtt> (proc.apvts, id, m.box);
}

void SuperParasitesEditor::paint (juce::Graphics& g)
{
    g.fillAll (kBg);
    g.setColour (kText);
    g.setFont (juce::Font (juce::FontOptions (20.0f, juce::Font::bold)));
    g.drawText ("UNOFFICIAL SUPERPARASITES LAYERS", 20, 10, 400, 26, juce::Justification::centredLeft);
    g.setColour (kDim);
    g.setFont (juce::Font (juce::FontOptions (11.0f)));
    g.drawText ("Clouds + Parasites + Beat Repeat (as in Typhoon / Supercell) - community port, not a Mutable Instruments product", 20, 34, getWidth() - 40, 16, juce::Justification::centredLeft);

    g.setColour (kPanel);
    g.fillRoundedRectangle (12.0f, 58.0f, (float) getWidth() - 24.0f, 150.0f, 10.0f);    // main knobs
    g.fillRoundedRectangle (12.0f, 216.0f, (float) getWidth() - 24.0f, 150.0f, 10.0f);   // blend knobs
    g.fillRoundedRectangle (12.0f, 374.0f, (float) getWidth() - 24.0f, 176.0f, 10.0f);   // switches, menus, mode hint
    g.fillRoundedRectangle (12.0f, 558.0f, (float) getWidth() - 24.0f, 260.0f, 10.0f);   // layers
    g.setColour (kDim);
    g.setFont (juce::Font (juce::FontOptions (10.0f)));
    g.drawText ("the module's BLEND knob, one control per role", 24, 218, 320, 14, juce::Justification::centredLeft);
}

void SuperParasitesEditor::resized()
{
    const int W = getWidth();

    auto place = [&] (Knob& k, juce::Rectangle<int> r)
    {
        k.label.setBounds (r.removeFromTop (16));
        k.slider.setBounds (r);
    };

    // Row 1: five main knobs
    const int col5 = (W - 24) / 5;
    Knob* row1[5] = { &position, &size, &pitch, &density, &texture };
    for (int i = 0; i < 5; ++i)
        place (*row1[i], juce::Rectangle<int> (12 + i * col5, 62, col5, 142).reduced (10, 0));

    // Row 2: the four blend roles
    const int col4 = (W - 24) / 4;
    Knob* row2[4] = { &dryWet, &spread, &feedback, &reverb };
    for (int i = 0; i < 4; ++i)
        place (*row2[i], juce::Rectangle<int> (12 + i * col4, 234, col4, 130).reduced (14, 0));

    // Row 3: buttons and menus
    auto row = juce::Rectangle<int> (24, 384, W - 48, 36);
    freeze.setBounds (row.removeFromLeft (84)); row.removeFromLeft (6);
    capture.setBounds (row.removeFromLeft (90).reduced (2)); row.removeFromLeft (6);
    reverse.setBounds (row.removeFromLeft (84)); row.removeFromLeft (6);
    gate.setBounds (row.removeFromLeft (64)); row.removeFromLeft (6);
    pitchMidi.setBounds (row.removeFromLeft (170));

    const int thirdW = (W - 48 - 3 * 12) / 4;   // four menus in a row
    auto placeMenu = [&] (Menu& m, int x, int y)
    {
        m.label.setBounds (x, y, thirdW, 16);
        m.box.setBounds (x, y + 18, thirdW, 26);
    };
    placeMenu (mode,         24, 430);
    placeMenu (quality,      24 + thirdW + 12, 430);
    placeMenu (bufferLength, 24 + 2 * (thirdW + 12), 430);
    placeMenu (engineRate,   24 + 3 * (thirdW + 12), 430);
    hint.setBounds (24, 482, W - 48, 62);

    // Layers panel
    layerMenu.label.setBounds (24, 564, 150, 16);
    layerMenu.box.setBounds (24, 582, 150, 26);
    recordLayer.setBounds (186, 582, 84, 26);
    clearLayer.setBounds (276, 582, 64, 26);
    saveLayer.setBounds (352, 582, 84, 26);
    loadLayer.setBounds (442, 582, 84, 26);
    homeButton.setBounds (532, 582, W - 532 - 24, 26);
    switchTime.label.setBounds (24, 616, 96, 22);
    switchTime.slider.setBounds (124, 616, W - 124 - 24 - 140, 22);
    styleMenu.box.setBounds (W - 24 - 132, 614, 132, 26);
    layerBMenu.box.setBounds (124, 646, 150, 26);
    layerBLevel.label.setBounds (24, 648, 96, 22);
    layerBLevel.slider.setBounds (286, 648, W - 286 - 24, 22);
    layerStatusLabel.setBounds (24, 676, W - 48, 18);
    const int colW = (W - 48) / 2;
    for (int i = 0; i < SuperParasitesEngine::kNumLayers; ++i)
        layerLines[i].setBounds (24 + (i / 3) * colW, 696 + (i % 3) * 17, colW - 6, 17);
    homeLabel.setBounds (24, 750, W - 48, 16);
    ioLabel.setBounds (24, 768, W - 48, 18);
    layerNote.setBounds (24, 788, W - 48, 58);
}
