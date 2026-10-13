#include "PluginEditor.h"

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

    addMenu (layerMenu, "layer", "LAYER", { "Live", "Layer 1", "Layer 2", "Layer 3", "Layer 4", "Layer 5", "Layer 6" });
    recordLayer.onClick = [this] { proc.requestRecordLayer(); };
    clearLayer.onClick  = [this] { proc.requestClearLayer(); };
    for (auto* b : { &recordLayer, &clearLayer })
    {
        b->setColour (juce::TextButton::buttonColourId, kPanel);
        b->setColour (juce::TextButton::buttonOnColourId, kAccent);
        b->setColour (juce::TextButton::textColourOffId, kText);
        addAndMakeVisible (*b);
    }
    layerStatusLabel.setJustificationType (juce::Justification::topLeft);
    layerStatusLabel.setFont (juce::Font (juce::FontOptions (12.0f)));
    layerStatusLabel.setColour (juce::Label::textColourId, kText);
    addAndMakeVisible (layerStatusLabel);
    layerNote.setJustificationType (juce::Justification::topLeft);
    layerNote.setFont (juce::Font (juce::FontOptions (11.0f)));
    layerNote.setColour (juce::Label::textColourId, kDim);
    layerNote.setText ("Pick a layer, press RECORD (it clears the layer and records from its start until it is full; press again to stop early). "
                       "Layers play in Granular, Pitch shifter / time stretch, Looping delay and Beat repeat, held like FREEZE. "
                       "LIVE is the always-recording buffer. Changing Quality, Buffer Length or Engine Rate empties the layers. "
                       "Layers are not saved with the project.", juce::dontSendNotification);
    addAndMakeVisible (layerNote);
    startTimerHz (10);

    hint.setJustificationType (juce::Justification::topLeft);
    hint.setFont (juce::Font (juce::FontOptions (11.0f)));
    hint.setColour (juce::Label::textColourId, kDim);
    addAndMakeVisible (hint);
    updateHint ((int) *proc.apvts.getRawParameterValue ("mode"));
    modeWatch = std::make_unique<juce::ParameterAttachment> (
        *proc.apvts.getParameter ("mode"), [this] (float v) { updateHint ((int) v); });
    modeWatch->sendInitialUpdate();

    setSize (660, 680);
}

SuperParasitesEditor::~SuperParasitesEditor() { stopTimer(); setLookAndFeel (nullptr); }

void SuperParasitesEditor::timerCallback()
{
    const int layer = (int) *proc.apvts.getRawParameterValue ("layer");
    const int mode  = (int) *proc.apvts.getRawParameterValue ("mode");
    bool recordingNow = false;
    juce::String all;
    for (int i = 1; i <= SuperParasitesEngine::kNumLayers; ++i)
    {
        const auto s = proc.layerStatus (i);
        if (s.state == SuperParasitesEngine::LayerRecording) recordingNow = true;
        all << i << ": " << (s.state == SuperParasitesEngine::LayerRecording ? juce::String ("recording")
                             : s.state == SuperParasitesEngine::LayerFilled ? juce::String (s.seconds, 1) + " s" : juce::String ("empty"))
            << (i < SuperParasitesEngine::kNumLayers ? "    " : "");
    }
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
             : s.state == SuperParasitesEngine::LayerFilled    ? "Layer " + juce::String (layer) + ": playing (" + juce::String (s.seconds, 1) + " s recorded)"
                                                               : "Layer " + juce::String (layer) + ": empty - press RECORD to fill it";
    }
    const juce::String text = head + "\n" + all;
    if (text != shownStatus) { shownStatus = text; layerStatusLabel.setText (text, juce::dontSendNotification); }
    recordLayer.setButtonText (recordingNow ? "STOP" : "RECORD");
    recordLayer.setColour (juce::TextButton::buttonColourId, recordingNow ? juce::Colour (0xffb5413a) : kPanel);
    recordLayer.setEnabled (layer > 0 && SuperParasitesEngine::modeUsesLayers (mode));
    clearLayer.setEnabled (layer > 0);
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
    g.fillRoundedRectangle (12.0f, 558.0f, (float) getWidth() - 24.0f, 112.0f, 10.0f);   // layers
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
    recordLayer.setBounds (186, 582, 90, 26);
    clearLayer.setBounds (282, 582, 70, 26);
    layerStatusLabel.setBounds (366, 564, W - 366 - 24, 44);
    layerNote.setBounds (24, 614, W - 48, 52);
}
