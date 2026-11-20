#include "PluginEditor.h"
#include "BinaryData.h"
#include "StreamOutput.h"

#if JucePlugin_Build_Standalone
 #include <juce_audio_devices/juce_audio_devices.h>
 #include <juce_audio_utils/juce_audio_utils.h>
 #include <juce_audio_plugin_client/Standalone/juce_StandaloneFilterWindow.h>

static juce::AudioDeviceManager* getStandaloneDeviceManager()
{
    if (auto* holder = juce::StandalonePluginHolder::getInstance())
        return &holder->deviceManager;
    return nullptr;
}

//==============================================================================
// Preferred audio devices: what the USER last picked, stored separately from JUCE's own
// "audioSetup" so an automatic fallback (device unplugged / busy at launch) never overwrites it.
namespace PreferredAudio
{
    static bool isAvailable (juce::AudioDeviceManager& dm, const juce::String& name, bool isInput)
    {
        if (name.isEmpty()) return false;
        if (auto* type = dm.getCurrentDeviceTypeObject())
            return type->getDeviceNames (isInput).contains (name);
        return false;
    }

    // userChoice = the change came from you (audio settings dialog or the buffer picker): always saved.
    // Otherwise (launch / unplug) a side is only saved if it isn't a fallback away from your device.
    static void record (juce::AudioDeviceManager& dm, juce::PropertySet& s, bool userChoice)
    {
        if (dm.getCurrentAudioDevice() == nullptr) return;
        const auto setup = dm.getAudioDeviceSetup();
        const auto prefIn = s.getValue ("emberPrefInput"), prefOut = s.getValue ("emberPrefOutput");

        const bool inFallback  = ! userChoice && prefIn.isNotEmpty()  && setup.inputDeviceName  != prefIn  && ! isAvailable (dm, prefIn, true);
        const bool outFallback = ! userChoice && prefOut.isNotEmpty() && setup.outputDeviceName != prefOut && ! isAvailable (dm, prefOut, false);

        if (! inFallback)
        {
            s.setValue ("emberPrefInput", setup.inputDeviceName);
            s.setValue ("emberPrefInputChannels", setup.inputChannels.toString (2));
        }
        if (! outFallback)
        {
            s.setValue ("emberPrefOutput", setup.outputDeviceName);
            s.setValue ("emberPrefOutputChannels", setup.outputChannels.toString (2));
        }
        if (! inFallback && ! outFallback)
        {
            s.setValue ("emberPrefRate", setup.sampleRate);
            s.setValue ("emberPrefBuffer", setup.bufferSize);
        }
    }

    static bool restore (juce::AudioDeviceManager& dm, juce::PropertySet& s)
    {
        const auto prefIn = s.getValue ("emberPrefInput"), prefOut = s.getValue ("emberPrefOutput");
        if (prefIn.isEmpty() && prefOut.isEmpty()) return false;

        auto setup = dm.getAudioDeviceSetup();
        bool change = false;

        if (prefIn.isNotEmpty() && setup.inputDeviceName != prefIn && isAvailable (dm, prefIn, true))
        {
            setup.inputDeviceName = prefIn;
            setup.inputChannels.parseString (s.getValue ("emberPrefInputChannels", "1"), 2);
            setup.useDefaultInputChannels = setup.inputChannels.isZero();
            change = true;
        }
        if (prefOut.isNotEmpty() && setup.outputDeviceName != prefOut && isAvailable (dm, prefOut, false))
        {
            setup.outputDeviceName = prefOut;
            setup.outputChannels.parseString (s.getValue ("emberPrefOutputChannels", "11"), 2);
            setup.useDefaultOutputChannels = setup.outputChannels.isZero();
            change = true;
        }
        if (! change) return false;

        if (const double rate = s.getDoubleValue ("emberPrefRate", 0.0); rate > 0.0) setup.sampleRate = rate;
        if (const int buf = s.getIntValue ("emberPrefBuffer", 0); buf > 0)           setup.bufferSize = buf;

        return dm.setAudioDeviceSetup (setup, true).isEmpty();
    }
}

// Drop to the smallest buffer >= minSize the device offers (if that's lower than now).
static void applyLowLatencyBuffer (juce::AudioDeviceManager& dm, int minSize)
{
    auto* dev = dm.getCurrentAudioDevice();
    if (dev == nullptr) return;

    int best = dev->getCurrentBufferSizeSamples();
    for (int s : dev->getAvailableBufferSizes())
        if (s >= minSize && s < best)
            best = s;

    if (best != dev->getCurrentBufferSizeSamples())
    {
        auto setup = dm.getAudioDeviceSetup();
        setup.bufferSize = best;
        dm.setAudioDeviceSetup (setup, true);
    }
}
#endif

//==============================================================================

//==============================================================================
namespace Palette
{
    const juce::Colour bg      { 0xff0b0c0f };
    const juce::Colour bar     { 0xff111216 };
    const juce::Colour panel   { 0xff17181d };
    const juce::Colour border  { 0xff2a2c33 };
    const juce::Colour accent  { 0xffff7a1a };
    const juce::Colour hot     { 0xffffb14a };
    const juce::Colour cream   { 0xffeceae6 };
    const juce::Colour text    { 0xffe8e8ea };
    const juce::Colour muted   { 0xff7d7f88 };
    const juce::Colour good    { 0xff7fe08a };
    const juce::Colour danger  { 0xffd8382c };
}

static juce::Colour accentOf (const juce::Component& c, juce::Colour fallback = Palette::accent)
{
    const auto v = c.getProperties()["accent"];
    return v.isVoid() ? fallback : juce::Colour ((juce::uint32) (juce::int64) v);
}

static void setAccent (juce::Component& c, juce::Colour col)
{
    c.getProperties().set ("accent", (juce::int64) col.getARGB());
}

static juce::Font spaced (float height, bool bold, float kerning)
{
    juce::Font f (juce::FontOptions (height, bold ? juce::Font::bold : juce::Font::plain));
    f.setExtraKerningFactor (kerning);
    return f;
}

juce::Colour EmberAmpEditor::ampAccent (int amp)
{
    static const juce::Colour cols[] = { juce::Colour (0xff4fd1c5),    // Clean: teal
                                         juce::Colour (0xffe6b04a),    // Crunch: amber
                                         juce::Colour (0xffff5a2a),    // Distortion: ember red
                                         juce::Colour (0xffa98bff) };  // Solo: violet
    return cols[juce::jlimit (0, 3, amp)];
}

//==============================================================================
EmberLookAndFeel::EmberLookAndFeel()
{
    using namespace juce;
    setColour (Slider::textBoxTextColourId, Palette::text);
    setColour (Slider::textBoxOutlineColourId, Colours::transparentBlack);
    setColour (Slider::textBoxBackgroundColourId, Colours::transparentBlack);
    setColour (Label::textColourId, Palette::text);
    setColour (Label::outlineColourId, Colours::transparentBlack);
    setColour (Label::outlineWhenEditingColourId, Palette::accent);
    setColour (ComboBox::backgroundColourId, Colour (0xff15161a));
    setColour (ComboBox::outlineColourId, Colours::transparentBlack);
    setColour (ComboBox::textColourId, Palette::text);
    setColour (ComboBox::arrowColourId, Palette::muted);
    setColour (PopupMenu::backgroundColourId, Colour (0xff15161a));
    setColour (PopupMenu::textColourId, Palette::text);
    setColour (PopupMenu::highlightedBackgroundColourId, Colour (0xff2b2d35));
    setColour (PopupMenu::highlightedTextColourId, Colours::white);
    setColour (PopupMenu::headerTextColourId, Palette::muted);
    setColour (AlertWindow::backgroundColourId, Colour (0xff15161a));
    setColour (AlertWindow::textColourId, Palette::text);
    setColour (AlertWindow::outlineColourId, Palette::border);
    setColour (TextEditor::backgroundColourId, Palette::bg);
    setColour (TextEditor::textColourId, Palette::text);
    setColour (TextEditor::outlineColourId, Palette::border);
    setColour (TextEditor::focusedOutlineColourId, Palette::accent);
    setColour (TextEditor::highlightColourId, Palette::accent.withAlpha (0.45f));
    setColour (CaretComponent::caretColourId, Palette::accent);
    setColour (TextButton::textColourOffId, Palette::muted);
    setColour (TextButton::textColourOnId, Colours::white);
    setColour (ToggleButton::textColourId, Palette::text);
}

void EmberLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h, float pos,
                                         float startAngle, float endAngle, juce::Slider& s)
{
    using namespace juce;
    const auto style = s.getProperties()["style"].toString();
    const auto accent = accentOf (s);
    const auto bounds = Rectangle<float> ((float) x, (float) y, (float) w, (float) h).reduced (2.0f);
    const float radius = jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f;
    const auto c = bounds.getCentre();
    const float angle = startAngle + pos * (endAngle - startAngle);

    if (style == "ring")
    {
        // modern minimal knob: thin track, white value arc, dark face with a pointer line
        const float arcR = radius - 3.0f;
        Path track;  track.addCentredArc (c.x, c.y, arcR, arcR, 0.0f, startAngle, endAngle, true);
        g.setColour (Colours::white.withAlpha (0.14f));
        g.strokePath (track, PathStrokeType (2.5f, PathStrokeType::curved, PathStrokeType::rounded));
        Path value;  value.addCentredArc (c.x, c.y, arcR, arcR, 0.0f, startAngle, angle, true);
        g.setColour (Colours::white.withAlpha (0.92f));
        g.strokePath (value, PathStrokeType (2.5f, PathStrokeType::curved, PathStrokeType::rounded));

        const float fr = arcR - 7.0f;
        g.setColour (Colours::black.withAlpha (0.5f));
        g.fillEllipse (c.x - fr, c.y - fr + 2.0f, fr * 2.0f, fr * 2.0f);
        g.setGradientFill (ColourGradient (Colour (0xff23252b), c.x, c.y - fr, Colour (0xff121317), c.x, c.y + fr, false));
        g.fillEllipse (c.x - fr, c.y - fr, fr * 2.0f, fr * 2.0f);
        g.setColour (Colours::white.withAlpha (0.10f));
        g.drawEllipse (c.x - fr, c.y - fr, fr * 2.0f, fr * 2.0f, 1.0f);
        Path ptr; ptr.startNewSubPath (0.0f, -fr + 3.0f); ptr.lineTo (0.0f, -fr * 0.25f);
        ptr.applyTransform (AffineTransform::rotation (angle).translated (c.x, c.y));
        g.setColour (Colours::white);
        g.strokePath (ptr, PathStrokeType (2.2f, PathStrokeType::curved, PathStrokeType::rounded));
        return;
    }

    if (style == "chrome" || style == "black")
    {
        const bool chrome = style == "chrome";
        const bool lightPanel = (bool) s.getProperties()["lightPanel"];
        const float kr = radius * 0.80f;

        // scale ticks printed on the panel
        const auto tickCol = lightPanel ? Colour (0xff4a4640).withAlpha (0.55f) : Colours::white.withAlpha (0.35f);
        g.setColour (tickCol);
        for (int i = 0; i <= 10; ++i)
        {
            const float a = startAngle + (float) i / 10.0f * (endAngle - startAngle);
            const float r1 = radius * 0.90f, r2 = radius * (i % 5 == 0 ? 1.0f : 0.96f);
            g.drawLine (c.x + r1 * std::sin (a), c.y - r1 * std::cos (a), c.x + r2 * std::sin (a), c.y - r2 * std::cos (a), 1.0f);
        }

        // soft shadow, knurled skirt, machined cap
        g.setColour (Colours::black.withAlpha (lightPanel ? 0.30f : 0.55f));
        g.fillEllipse (c.x - kr + 1.5f, c.y - kr + 4.0f, kr * 2.0f, kr * 2.0f);
        g.setGradientFill (ColourGradient (chrome ? Colour (0xffb9bcc2) : Colour (0xff2a2b2f), c.x, c.y - kr,
                                           chrome ? Colour (0xff5b5e64) : Colour (0xff0b0b0d), c.x, c.y + kr, false));
        g.fillEllipse (c.x - kr, c.y - kr, kr * 2.0f, kr * 2.0f);
        g.setColour (Colours::black.withAlpha (chrome ? 0.18f : 0.35f));
        for (int i = 0; i < 48; ++i)
        {
            const float a = (float) i / 48.0f * MathConstants<float>::twoPi;
            g.drawLine (c.x + kr * 0.80f * std::sin (a), c.y - kr * 0.80f * std::cos (a), c.x + kr * std::sin (a), c.y - kr * std::cos (a), 0.8f);
        }
        const float cr = kr * 0.70f;
        g.setGradientFill (ColourGradient (chrome ? Colour (0xfff4f5f7) : Colour (0xff3a3b40), c.x - cr * 0.5f, c.y - cr,
                                           chrome ? Colour (0xff8f9298) : Colour (0xff141518), c.x + cr * 0.4f, c.y + cr, false));
        g.fillEllipse (c.x - cr, c.y - cr, cr * 2.0f, cr * 2.0f);
        g.setColour (Colours::white.withAlpha (chrome ? 0.55f : 0.12f));
        g.drawEllipse (c.x - cr, c.y - cr, cr * 2.0f, cr * 2.0f, 0.8f);
        // pointer notch
        Path ptr; ptr.startNewSubPath (0.0f, -cr + 2.0f); ptr.lineTo (0.0f, -cr * 0.35f);
        ptr.applyTransform (AffineTransform::rotation (angle).translated (c.x, c.y));
        g.setColour (chrome ? Colour (0xff1c1d20) : Colours::white.withAlpha (0.9f));
        g.strokePath (ptr, PathStrokeType (2.0f, PathStrokeType::curved, PathStrokeType::rounded));
        return;
    }

    // "pedal": compact matte knob with a thin coloured arc
    const float arcR = radius - 2.0f;
    Path value; value.addCentredArc (c.x, c.y, arcR, arcR, 0.0f, startAngle, angle, true);
    Path track; track.addCentredArc (c.x, c.y, arcR, arcR, 0.0f, startAngle, endAngle, true);
    g.setColour (Colours::black.withAlpha (0.45f));
    g.strokePath (track, PathStrokeType (2.0f, PathStrokeType::curved, PathStrokeType::rounded));
    g.setColour (accent.withAlpha (0.9f));
    g.strokePath (value, PathStrokeType (2.0f, PathStrokeType::curved, PathStrokeType::rounded));
    const float kr = arcR - 6.0f;
    g.setColour (Colours::black.withAlpha (0.5f));
    g.fillEllipse (c.x - kr + 1.0f, c.y - kr + 3.0f, kr * 2.0f, kr * 2.0f);
    g.setGradientFill (ColourGradient (Colour (0xff2b2c31), c.x, c.y - kr, Colour (0xff0f1013), c.x, c.y + kr, false));
    g.fillEllipse (c.x - kr, c.y - kr, kr * 2.0f, kr * 2.0f);
    Path ptr; ptr.startNewSubPath (0.0f, -kr + 3.0f); ptr.lineTo (0.0f, -kr * 0.3f);
    ptr.applyTransform (AffineTransform::rotation (angle).translated (c.x, c.y));
    g.setColour (Colours::white.withAlpha (0.9f));
    g.strokePath (ptr, PathStrokeType (2.0f, PathStrokeType::curved, PathStrokeType::rounded));
}

void EmberLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour&,
                                             bool highlighted, bool down)
{
    using namespace juce;
    auto r = b.getLocalBounds().toFloat().reduced (1.0f);
    const bool on = b.getToggleState();
    auto& props = b.getProperties();

    if (props["tab"] || props["link"])
        return;   // text-only

    if (props["footswitch"])
    {
        const auto c = r.getCentre();
        const float rad = jmin (r.getWidth(), r.getHeight()) * 0.40f;
        g.setColour (Colours::black.withAlpha (0.55f));
        g.fillEllipse (c.x - rad + 1.5f, c.y - rad + 3.0f, rad * 2.0f, rad * 2.0f);
        g.setGradientFill (ColourGradient (Colour (0xffd4d6da), c.x, c.y - rad, Colour (0xff5d6066), c.x, c.y + rad, false));
        g.fillEllipse (c.x - rad, c.y - rad, rad * 2.0f, rad * 2.0f);
        const float ir = rad * (down ? 0.60f : 0.66f);
        g.setGradientFill (ColourGradient (Colour (0xff8a8d93), c.x, c.y + ir, Colour (0xffeceef1), c.x, c.y - ir, false));
        g.fillEllipse (c.x - ir, c.y - ir, ir * 2.0f, ir * 2.0f);
        g.setColour (Colours::black.withAlpha (highlighted ? 0.25f : 0.4f));
        g.drawEllipse (c.x - rad, c.y - rad, rad * 2.0f, rad * 2.0f, 1.0f);
        return;
    }

    const bool danger = props["danger"];
    if (on && danger)
    {
        g.setColour (Palette::danger);
        g.fillRoundedRectangle (r, 4.0f);
        return;
    }
    g.setColour (Colours::white.withAlpha (down ? 0.14f : highlighted ? 0.09f : 0.05f));
    g.fillRoundedRectangle (r, 4.0f);
    g.setColour (Colours::white.withAlpha (0.10f));
    g.drawRoundedRectangle (r, 4.0f, 1.0f);
}

void EmberLookAndFeel::drawButtonText (juce::Graphics& g, juce::TextButton& b, bool highlighted, bool)
{
    using namespace juce;
    auto& props = b.getProperties();
    if (props["footswitch"]) return;
    auto r = b.getLocalBounds().toFloat();

    if (props["tab"])
    {
        const bool on = b.getToggleState();
        g.setColour (on ? Colours::white : highlighted ? Colours::white.withAlpha (0.75f) : Palette::muted);
        g.setFont (spaced (13.0f, on, 0.18f));
        g.drawText (b.getButtonText(), r.withTrimmedBottom (10.0f), Justification::centred);
        if (on)
        {
            g.setColour (Colours::white);
            g.fillEllipse (Rectangle<float> (5.0f, 5.0f).withCentre ({ r.getCentreX(), r.getBottom() - 6.0f }));
        }
        if (props["live"])   // the amp you hear
        {
            const float tw = (float) GlyphArrangement::getStringWidthInt (spaced (13.0f, on, 0.18f), b.getButtonText());
            g.setColour (accentOf (b));
            g.fillEllipse (r.getCentreX() + tw * 0.5f + 7.0f, r.getCentreY() - 8.0f, 6.0f, 6.0f);
        }
        return;
    }

    const bool on = b.getToggleState();
    g.setColour (props["danger"] && on ? Colours::white
                                       : props["link"] ? (highlighted ? Colours::white : Palette::text.withAlpha (b.isEnabled() ? 0.85f : 0.3f))
                                                       : Palette::text.withAlpha (b.isEnabled() ? 0.9f : 0.3f));
    g.setFont (spaced (12.5f, true, 0.08f));
    g.drawText (b.getButtonText(), r, Justification::centred);
}

void EmberLookAndFeel::drawToggleButton (juce::Graphics& g, juce::ToggleButton& b, bool highlighted, bool)
{
    using namespace juce;
    auto r = b.getLocalBounds().toFloat();
    const bool on = b.getToggleState();
    const auto accent = accentOf (b, Colours::white);
    const auto textCol = b.getProperties()["textColour"].isVoid() ? Palette::text
                                                                 : Colour ((uint32) (int64) b.getProperties()["textColour"]);

    // label, then a pill switch on the right
    auto pill = r.removeFromRight (34.0f).withSizeKeepingCentre (32.0f, 17.0f);
    g.setColour (textCol.withAlpha (on ? 1.0f : 0.75f));
    g.setFont (spaced (13.0f, true, 0.10f));
    g.drawText (b.getButtonText(), r.withTrimmedRight (6.0f), Justification::centredRight);

    g.setColour (on ? accent.withAlpha (0.95f) : Colours::transparentBlack);
    g.fillRoundedRectangle (pill, 8.5f);
    g.setColour (on ? accent : textCol.withAlpha (highlighted ? 0.8f : 0.55f));
    g.drawRoundedRectangle (pill.reduced (0.5f), 8.5f, 1.4f);
    const float d = 11.0f;
    const auto knob = Rectangle<float> (d, d).withCentre ({ on ? pill.getRight() - 8.5f : pill.getX() + 8.5f, pill.getCentreY() });
    g.setColour (on ? Colour (0xff101114) : textCol.withAlpha (0.8f));
    g.fillEllipse (knob);
}

void EmberLookAndFeel::drawComboBox (juce::Graphics& g, int width, int height, bool, int, int, int, int, juce::ComboBox& box)
{
    using namespace juce;
    auto r = Rectangle<float> (0.0f, 0.0f, (float) width, (float) height);
    g.setColour (box.findColour (ComboBox::backgroundColourId));
    g.fillRoundedRectangle (r, 6.0f);
    g.setColour (Colours::white.withAlpha (box.isMouseOver (true) ? 0.14f : 0.06f));
    g.drawRoundedRectangle (r.reduced (0.5f), 6.0f, 1.0f);
    // thin chevron
    const float cx = (float) width - 18.0f, cy = (float) height * 0.5f;
    Path p; p.startNewSubPath (cx - 5.0f, cy - 2.5f); p.lineTo (cx, cy + 2.5f); p.lineTo (cx + 5.0f, cy - 2.5f);
    g.setColour (Palette::muted);
    g.strokePath (p, PathStrokeType (1.5f, PathStrokeType::curved, PathStrokeType::rounded));
}

juce::Font EmberLookAndFeel::getComboBoxFont (juce::ComboBox& box)
{
    return juce::Font (juce::FontOptions (juce::jmin (17.0f, (float) box.getHeight() * 0.45f), juce::Font::bold));
}

void EmberLookAndFeel::positionComboBoxText (juce::ComboBox& box, juce::Label& label)
{
    label.setBounds (14, 1, box.getWidth() - 40, box.getHeight() - 2);
    label.setFont (getComboBoxFont (box));
}

juce::Label* EmberLookAndFeel::createSliderTextBox (juce::Slider& slider)
{
    auto* l = new ValueLabel();
    l->setJustificationType (juce::Justification::centred);
    l->setKeyboardType (juce::TextInputTarget::decimalKeyboard);
    l->setFont (juce::FontOptions (12.5f, juce::Font::bold));
    l->setColour (juce::Label::textColourId, slider.findColour (juce::Slider::textBoxTextColourId));
    l->setColour (juce::Label::backgroundColourId, juce::Colours::transparentBlack);
    l->setColour (juce::Label::outlineColourId, juce::Colours::transparentBlack);
    l->setColour (juce::TextEditor::textColourId, Palette::text);
    l->setColour (juce::TextEditor::backgroundColourId, Palette::bg);
    l->setColour (juce::TextEditor::outlineColourId, Palette::accent);
    l->setColour (juce::TextEditor::highlightColourId, Palette::accent.withAlpha (0.55f));
    return l;
}

void ValueLabel::editorShown (juce::TextEditor* ed)
{
    juce::Label::editorShown (ed);
    ed->selectAll();
}

void ValueLabel::mouseDoubleClick (const juce::MouseEvent& e)
{
    // open the editor only once the double-click is complete, so no click lands inside it
    if (isEnabled() && ! e.mods.isPopupMenu() && ! isBeingEdited())
        showEditor();
}

void ValueLabel::mouseUp (const juce::MouseEvent&)
{
    // single click does nothing (no editing): double-click edits
}

//==============================================================================
Knob::Knob (juce::AudioProcessorValueTreeState& state, const juce::String& paramId, const juce::String& title,
            juce::Colour accent, const juce::String& style, juce::Colour textColour, float titleSize)
{
    slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    slider.setColour (juce::Slider::textBoxTextColourId, textColour);
    slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 80, 18);
    slider.setRotaryParameters (juce::MathConstants<float>::pi * 1.2f, juce::MathConstants<float>::pi * 2.8f, true);
    slider.setDoubleClickReturnValue (true, state.getParameterRange (paramId).convertFrom0to1 (
                                                state.getParameter (paramId)->getDefaultValue()));
    slider.getProperties().set ("style", style);
    slider.getProperties().set ("lightPanel", textColour.getPerceivedBrightness() < 0.5f);
    setAccent (slider, accent);
    addAndMakeVisible (slider);

    label.setText (title.toUpperCase(), juce::dontSendNotification);
    label.setJustificationType (juce::Justification::centred);
    const bool panelStyle = style == "chrome" || style == "black";
    label.setFont (panelStyle ? spaced (titleSize - 1.0f, false, 0.35f) : spaced (titleSize, true, 0.10f));
    label.setColour (juce::Label::textColourId, panelStyle ? textColour.withAlpha (0.85f) : textColour);
    addAndMakeVisible (label);

    attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (state, paramId, slider);
}

void Knob::resized()
{
    auto r = getLocalBounds();
    label.setBounds (r.removeFromTop (20));
    slider.setBounds (r);
}

//==============================================================================
void TunerDisplay::setFrequency (float hz)
{
    if (hz > 0.0f)
    {
        const float midi = 69.0f + 12.0f * std::log2 (hz / 440.0f);
        const float cents = (midi - std::round (midi)) * 100.0f;
        if (freq <= 0.0f || std::round (midi) != std::round (69.0f + 12.0f * std::log2 (freq / 440.0f)))
            smoothedCents = cents;
        else
            smoothedCents += 0.35f * (cents - smoothedCents);
        freq = hz;
        holdFrames = 12;
    }
    else if (holdFrames > 0 && --holdFrames == 0)
    {
        freq = -1.0f;
    }
    repaint();
}

void TunerDisplay::paint (juce::Graphics& g)
{
    using namespace juce;
    auto r = getLocalBounds().toFloat();
    const bool active = freq > 0.0f;
    const bool inTune = active && std::abs (smoothedCents) < 3.0f;

    g.setColour (Palette::muted);
    g.setFont (spaced (12.0f, true, 0.15f));
    g.drawText ("TUNER", r.removeFromLeft (58.0f), Justification::centredLeft);

    String note = "--";
    if (active)
        note = MidiMessage::getMidiNoteName (roundToInt (69.0f + 12.0f * std::log2 (freq / 440.0f)), true, true, 4);
    g.setColour (! active ? Palette::muted : inTune ? Palette::good : Palette::text);
    g.setFont (FontOptions (15.0f, Font::bold));
    g.drawText (note, r.removeFromLeft (40.0f), Justification::centred);

    auto scale = r.reduced (6.0f, 0.0f);
    const float midY = scale.getCentreY();
    g.setColour (Colours::white.withAlpha (0.12f));
    g.drawHorizontalLine ((int) midY, scale.getX(), scale.getRight());
    for (int c = -50; c <= 50; c += 25)
    {
        const float px = scale.getX() + scale.getWidth() * (float) (c + 50) / 100.0f;
        g.setColour (c == 0 ? Colours::white.withAlpha (0.7f) : Colours::white.withAlpha (0.2f));
        g.drawVerticalLine ((int) px, midY - (c == 0 ? 6.0f : 3.0f), midY + (c == 0 ? 6.0f : 3.0f));
    }
    if (active)
    {
        const float px = scale.getX() + scale.getWidth() * (jlimit (-50.0f, 50.0f, smoothedCents) + 50.0f) / 100.0f;
        g.setColour (inTune ? Palette::good : Palette::accent);
        g.fillRoundedRectangle (px - 2.0f, midY - 7.0f, 4.0f, 14.0f, 2.0f);
    }
}

//==============================================================================
void LevelMeter::push (float peak)
{
    level = juce::jmax (peak, level * 0.85f);
    repaint();
}

void LevelMeter::paint (juce::Graphics& g)
{
    using namespace juce;
    auto bar = getLocalBounds().toFloat();
    g.setColour (Colours::white.withAlpha (0.08f));
    g.fillRoundedRectangle (bar, 2.0f);
    const float db = Decibels::gainToDecibels (level, -60.0f);
    const float frac = jlimit (0.0f, 1.0f, (db + 60.0f) / 60.0f);
    g.setColour (db > -1.0f ? Palette::danger : db > -9.0f ? Palette::hot : Colours::white.withAlpha (0.85f));
    g.fillRoundedRectangle (bar.withTop (bar.getBottom() - bar.getHeight() * frac), 2.0f);
}

//==============================================================================
AmpPage::Theme AmpPage::themeFor (int amp)
{
    using juce::Colour;
    switch (amp)
    {
        case 0:  return { Colour (0xffe9e5dc), Colour (0xffc9c3b7), Colour (0xffd8d3c8), Colour (0xff9d978b),
                          Colour (0xfff1eee7), Colour (0xffd9d4ca), Colour (0xff3b3833), true };    // Clean: warm ivory
        case 1:  return { Colour (0xff1f1d1b), Colour (0xff0f0e0d), Colour (0xff23201c), Colour (0xff3c362e),
                          Colour (0xffcdb27a), Colour (0xff9c8152), Colour (0xff2a2116), true };    // Crunch: black + gold panel
        case 2:  return { Colour (0xff35383d), Colour (0xff1c1e21), Colour (0xff202226), Colour (0xff3d4046),
                          Colour (0xff2e3135), Colour (0xff1d1f22), Colour (0xffd9dadc), false };   // Distortion: gunmetal
        default: return { Colour (0xff2a2533), Colour (0xff141119), Colour (0xff1c1823), Colour (0xff342d40),
                          Colour (0xff2b2636), Colour (0xff1a1622), Colour (0xffe2dcef), false };   // Solo: dark plum
    }
}

AmpPage::AmpPage (EmberAmpEditor& o, EmberAmpProcessor& p, int a)
    : owner (o), proc (p), amp (a), accent (EmberAmpEditor::ampAccent (a)), theme (themeFor (a))
{
    const juce::String knobStyle = theme.lightPanel ? "chrome" : "black";
    std::vector<std::pair<const char*, const char*>> list;
    if (amp == EmberAmpProcessor::distortion) list.push_back ({ "drive", "Drive" });
    for (auto kv : { std::pair<const char*, const char*> { "gain", "Gain" }, { "bass", "Bass" }, { "mid", "Middle" }, { "treble", "Treble" },
                     { "presence", "Presence" }, { "sustain", "Sustain" }, { "volume", "Volume" } })
        list.push_back (kv);
    for (auto& [id, title] : list)
        addAndMakeVisible (knobs.add (new Knob (p.apvts, EmberAmpProcessor::ampParamID (a, id), title, accent, knobStyle, theme.text, 11.5f)));

    setAccent (cabButton, accent);
    addAndMakeVisible (cabButton);
    cabAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (p.apvts, EmberAmpProcessor::ampParamID (a, "cab"), cabButton);

    for (auto* b : { &loadIRButton, &clearIRButton }) addAndMakeVisible (b);
    loadIRButton.onClick  = [this] { chooseIR(); };
    clearIRButton.onClick = [this] { proc.clearIR (amp); refresh(); };

    irLabel.setFont (spaced (12.5f, false, 0.05f));
    irLabel.setJustificationType (juce::Justification::centredLeft);
    irLabel.setMinimumHorizontalScale (0.7f);
    addAndMakeVisible (irLabel);

    logo = juce::ImageCache::getFromMemory (BinaryData::logo_png, BinaryData::logo_pngSize);
    refresh();
}

void AmpPage::chooseIR()
{
    auto start = proc.getIRFile (amp).existsAsFile() ? proc.getIRFile (amp).getParentDirectory()
                                                     : juce::File::getSpecialLocation (juce::File::userHomeDirectory);
    fileChooser = std::make_unique<juce::FileChooser> ("Load an impulse response for the " + EmberAmpProcessor::ampName (amp) + " amp",
                                                       start, "*.wav;*.aif;*.aiff;*.flac");
    juce::Component::SafePointer<AmpPage> safe (this);
    fileChooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [safe] (const juce::FileChooser& fc)
                              {
                                  if (safe == nullptr) return;
                                  if (fc.getResult().existsAsFile())
                                      safe->proc.loadIR (safe->amp, fc.getResult());
                                  safe->refresh();
                              });
}

void AmpPage::refresh()
{
    const bool ir = proc.hasIR (amp);
    irLabel.setText (ir ? "IR  " + proc.getIRFile (amp).getFileNameWithoutExtension() : EmberAmpProcessor::cabName (amp).toUpperCase(),
                     juce::dontSendNotification);
    irLabel.setColour (juce::Label::textColourId, ir ? accent : Palette::text.withAlpha (0.7f));
    clearIRButton.setEnabled (ir);

    const bool active = proc.getSelectedAmp() == amp;
    const bool cabOn = cabButton.getToggleState();
    if (active != shownActive || cabOn != shownCab) { shownActive = active; shownCab = cabOn; repaint(); }
}

void AmpPage::paint (juce::Graphics& g)
{
    using namespace juce;
    const auto h = head.toFloat();

    // ---- floor shadow + feet
    g.setGradientFill (ColourGradient (Colours::black.withAlpha (0.55f), h.getCentreX(), h.getBottom() + 4.0f,
                                       Colours::transparentBlack, h.getCentreX(), h.getBottom() + 40.0f, false));
    g.fillRect (h.withY (h.getBottom()).withHeight (40.0f).reduced (30.0f, 0.0f));
    for (float fx : { h.getX() + 70.0f, h.getRight() - 150.0f })
    {
        g.setGradientFill (ColourGradient (Colour (0xff2a2b30), fx, h.getBottom(), Colour (0xff0c0c0e), fx, h.getBottom() + 16.0f, false));
        g.fillRoundedRectangle (fx, h.getBottom() - 4.0f, 80.0f, 18.0f, 5.0f);
    }

    // ---- chassis
    g.setGradientFill (ColourGradient (theme.chassisTop, h.getX(), h.getY(), theme.chassisBottom, h.getX(), h.getBottom(), false));
    g.fillRoundedRectangle (h, 26.0f);
    g.setGradientFill (ColourGradient (Colours::white.withAlpha (theme.lightPanel && amp == 0 ? 0.55f : 0.16f), h.getCentreX(), h.getY(),
                                       Colours::transparentBlack, h.getCentreX(), h.getY() + 26.0f, false));
    g.drawRoundedRectangle (h.reduced (1.0f), 25.0f, 2.0f);
    g.setColour (Colours::black.withAlpha (0.35f));
    g.drawRoundedRectangle (h.reduced (0.5f), 26.0f, 1.0f);

    // ---- grille cloth
    const auto gr = grille.toFloat();
    g.saveState();
    {
        Path clip; clip.addRoundedRectangle (gr, 8.0f);
        g.reduceClipRegion (clip);
        g.setGradientFill (ColourGradient (theme.grille.brighter (0.06f), gr.getX(), gr.getY(), theme.grille.darker (0.15f), gr.getX(), gr.getBottom(), false));
        g.fillRect (gr);
        g.setColour (theme.grilleLine.withAlpha (0.35f));
        for (float d = -gr.getHeight(); d < gr.getWidth(); d += 4.0f)
        {
            g.drawLine (gr.getX() + d, gr.getBottom(), gr.getX() + d + gr.getHeight(), gr.getY(), 0.6f);
            g.drawLine (gr.getX() + d, gr.getY(), gr.getX() + d + gr.getHeight(), gr.getBottom(), 0.6f);
        }
        // inset shadow
        g.setGradientFill (ColourGradient (Colours::black.withAlpha (0.28f), gr.getX(), gr.getY(), Colours::transparentBlack, gr.getX(), gr.getY() + 18.0f, false));
        g.fillRect (gr.withHeight (18.0f));
    }
    g.restoreState();
    g.setColour (Colours::black.withAlpha (0.35f));
    g.drawRoundedRectangle (gr, 8.0f, 1.0f);

    // ---- emblem: bezel ring with the Ember mark, lit when this amp is live
    const auto ec = gr.getCentre();
    const float er = jmin (gr.getHeight() * 0.36f, 74.0f);
    if (shownActive)
    {
        g.setGradientFill (ColourGradient (accent.withAlpha (0.45f), ec.x, ec.y, accent.withAlpha (0.0f), ec.x + er * 2.2f, ec.y, true));
        g.fillEllipse (Rectangle<float> (er * 4.4f, er * 4.4f).withCentre (ec));
    }
    g.setColour (Colours::black.withAlpha (0.45f));
    g.fillEllipse (Rectangle<float> (er * 2.0f + 10.0f, er * 2.0f + 10.0f).withCentre (ec.translated (0.0f, 4.0f)));
    g.setGradientFill (ColourGradient (Colour (0xffe6e8ec), ec.x, ec.y - er, Colour (0xff5d6168), ec.x, ec.y + er, false));
    g.fillEllipse (Rectangle<float> (er * 2.0f + 8.0f, er * 2.0f + 8.0f).withCentre (ec));
    g.setGradientFill (ColourGradient (Colour (0xff17181c), ec.x, ec.y - er, Colour (0xff050506), ec.x, ec.y + er, false));
    g.fillEllipse (Rectangle<float> (er * 2.0f, er * 2.0f).withCentre (ec));
    if (logo.isValid())
    {
        g.setOpacity (shownActive ? 1.0f : 0.45f);
        g.drawImage (logo, Rectangle<float> (er * 1.55f, er * 1.55f).withCentre (ec), RectanglePlacement::centred);
        g.setOpacity (1.0f);
    }
    g.setColour (Colours::white.withAlpha (0.18f));
    g.drawEllipse (Rectangle<float> (er * 2.0f, er * 2.0f).withCentre (ec), 1.0f);

    // badge (top-left of the grille)
    auto badge = Rectangle<float> (gr.getX() + 20.0f, gr.getY() + 18.0f, 150.0f, 26.0f);
    g.setGradientFill (ColourGradient (Colour (0xffd9dbdf), badge.getX(), badge.getY(), Colour (0xff8e9197), badge.getX(), badge.getBottom(), false));
    g.fillRoundedRectangle (badge, 4.0f);
    g.setColour (Colour (0xff1d1e21));
    g.setFont (spaced (12.0f, true, 0.30f));
    g.drawText ("EMBER  " + EmberAmpProcessor::ampName (amp).toUpperCase(), badge, Justification::centred);

    // ---- control panel: brushed metal strip
    const auto pn = panel.toFloat();
    g.setGradientFill (ColourGradient (theme.panelTop, pn.getX(), pn.getY(), theme.panelBottom, pn.getX(), pn.getBottom(), false));
    g.fillRoundedRectangle (pn, 6.0f);
    g.saveState();
    g.reduceClipRegion (panel);
    g.setColour ((theme.lightPanel ? Colours::white : Colours::white).withAlpha (theme.lightPanel ? 0.10f : 0.025f));
    for (float yy = pn.getY() + 1.0f; yy < pn.getBottom(); yy += 2.0f)
        g.drawHorizontalLine ((int) yy, pn.getX(), pn.getRight());
    g.restoreState();
    g.setColour (Colours::black.withAlpha (0.35f));
    g.drawRoundedRectangle (pn, 6.0f, 1.0f);
    g.setColour (Colours::white.withAlpha (theme.lightPanel ? 0.5f : 0.08f));
    g.drawHorizontalLine ((int) pn.getY() + 1, pn.getX() + 6.0f, pn.getRight() - 6.0f);

    // screws
    for (auto p : { pn.getTopLeft().translated (12.0f, 12.0f), pn.getTopRight().translated (-12.0f, 12.0f),
                    pn.getBottomLeft().translated (12.0f, -12.0f), pn.getBottomRight().translated (-12.0f, -12.0f) })
    {
        g.setGradientFill (ColourGradient (Colour (0xffd0d2d6), p.x, p.y - 3.0f, Colour (0xff6a6d73), p.x, p.y + 3.0f, false));
        g.fillEllipse (Rectangle<float> (7.0f, 7.0f).withCentre (p));
        g.setColour (Colours::black.withAlpha (0.4f));
        g.drawLine (p.x - 2.5f, p.y, p.x + 2.5f, p.y, 0.8f);
    }

    // input jack (left) and pilot light + name (right)
    const auto jack = Point<float> (pn.getX() + 52.0f, pn.getCentreY());
    g.setGradientFill (ColourGradient (Colour (0xffe1e3e7), jack.x, jack.y - 12.0f, Colour (0xff5f6268), jack.x, jack.y + 12.0f, false));
    g.fillEllipse (Rectangle<float> (24.0f, 24.0f).withCentre (jack));
    g.setColour (Colour (0xff0b0b0c));
    g.fillEllipse (Rectangle<float> (9.0f, 9.0f).withCentre (jack));
    g.setColour (theme.text.withAlpha (0.7f));
    g.setFont (spaced (9.5f, false, 0.3f));
    g.drawText ("INPUT", Rectangle<float> (60.0f, 14.0f).withCentre (jack.translated (0.0f, -24.0f)), Justification::centred);

    const auto lamp = Point<float> (pn.getRight() - 160.0f, pn.getCentreY());
    if (shownActive)
    {
        g.setGradientFill (ColourGradient (accent.withAlpha (0.65f), lamp.x, lamp.y, accent.withAlpha (0.0f), lamp.x + 18.0f, lamp.y, true));
        g.fillEllipse (Rectangle<float> (36.0f, 36.0f).withCentre (lamp));
    }
    g.setGradientFill (ColourGradient ((shownActive ? accent.brighter (0.4f) : Colour (0xff3a3a3d)), lamp.x - 3.0f, lamp.y - 4.0f,
                                       (shownActive ? accent.darker (0.3f) : Colour (0xff151517)), lamp.x + 3.0f, lamp.y + 5.0f, false));
    g.fillEllipse (Rectangle<float> (14.0f, 14.0f).withCentre (lamp));
    g.setColour (Colours::black.withAlpha (0.5f));
    g.drawEllipse (Rectangle<float> (16.0f, 16.0f).withCentre (lamp), 1.5f);

    g.setColour (theme.text.withAlpha (0.85f));
    g.setFont (FontOptions (22.0f, Font::italic | Font::bold));
    g.drawText ("Ember", Rectangle<float> (pn.getRight() - 140.0f, pn.getCentreY() - 22.0f, 110.0f, 26.0f), Justification::centredLeft);
    g.setFont (spaced (9.5f, false, 0.35f));
    g.drawText (EmberAmpProcessor::ampName (amp).toUpperCase(), Rectangle<float> (pn.getRight() - 140.0f, pn.getCentreY() + 4.0f, 110.0f, 16.0f), Justification::centredLeft);

    // ---- cab top below the head
    const auto ct = cabTop.toFloat();
    g.setGradientFill (ColourGradient (Colour (0xff1a1b1f), ct.getX(), ct.getY(), Colour (0xff0b0b0d), ct.getX(), ct.getBottom(), false));
    g.fillRoundedRectangle (ct.withHeight (ct.getHeight() + 30.0f), 22.0f);
    g.setColour (Colours::white.withAlpha (0.10f));
    g.drawHorizontalLine ((int) ct.getY() + 1, ct.getX() + 22.0f, ct.getRight() - 22.0f);
    // grille cloth band with the speaker cones showing through
    auto band = ct.reduced (22.0f, 0.0f).withTrimmedTop (16.0f).withTrimmedBottom (4.0f);
    g.saveState();
    g.reduceClipRegion (band.toNearestInt());
    g.setColour (Colour (0xff121316));
    g.fillRect (band);
    const int cones = amp == EmberAmpProcessor::clean ? 2 : 2;
    for (int i = 0; i < cones; ++i)
    {
        const float cx = band.getX() + band.getWidth() * ((float) i + 0.5f) / (float) cones;
        const float rr = band.getWidth() * 0.18f;
        g.setColour (Colours::white.withAlpha (shownCab ? 0.07f : 0.03f));
        g.drawEllipse (Rectangle<float> (rr * 2.0f, rr * 2.0f).withCentre ({ cx, band.getBottom() + rr * 0.55f }), 2.0f);
        g.drawEllipse (Rectangle<float> (rr * 1.2f, rr * 1.2f).withCentre ({ cx, band.getBottom() + rr * 0.55f }), 1.0f);
    }
    g.setColour (Colours::white.withAlpha (0.025f));
    for (float xx = band.getX(); xx < band.getRight(); xx += 3.0f)
        g.drawVerticalLine ((int) xx, band.getY(), band.getBottom());
    g.restoreState();
}

void AmpPage::resized()
{
    auto r = getLocalBounds();
    cabTop = r.removeFromBottom (110);
    r.removeFromBottom (26);                                   // space for the feet
    head = r.reduced (34, 8);

    auto inner = head.reduced (24, 22);
    panel  = inner.removeFromBottom (juce::roundToInt (inner.getHeight() * 0.40f));
    inner.removeFromBottom (14);
    grille = inner;

    // knobs across the panel, between the input jack and the pilot light
    auto kr = panel.withTrimmedLeft (100).withTrimmedRight (180).reduced (0, 8);
    const int kw = kr.getWidth() / knobs.size();
    for (auto* k : knobs)
        k->setBounds (kr.removeFromLeft (kw).reduced (4, 0));

    // cab controls on the cab top
    auto cab = cabTop.reduced (60, 0).withTrimmedTop (34).withHeight (34);
    cabButton.setBounds (cab.removeFromLeft (84));
    cab.removeFromLeft (14);
    irLabel.setBounds (cab.removeFromLeft (cab.getWidth() - 220));
    clearIRButton.setBounds (cab.removeFromRight (90).reduced (0, 3));
    cab.removeFromRight (10);
    loadIRButton.setBounds (cab.removeFromRight (110).reduced (0, 3));
}

//==============================================================================
PedalUnit::PedalUnit (juce::AudioProcessorValueTreeState& state, const juce::String& n, juce::Colour b,
                      const juce::String& onParam, std::vector<std::pair<juce::String, juce::String>> ks)
    : name (n), body (b)
{
    for (auto& [id, title] : ks)
        addAndMakeVisible (knobs.add (new Knob (state, id, title, body.brighter (0.6f).withSaturation (0.5f), "pedal", Palette::text, 11.0f)));

    footswitch.setClickingTogglesState (true);
    footswitch.getProperties().set ("footswitch", true);
    footswitch.setTooltip ("On / off");
    addAndMakeVisible (footswitch);
    footAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (state, onParam, footswitch);
}

void PedalUnit::refresh()
{
    if (footswitch.getToggleState() != shownOn) { shownOn = footswitch.getToggleState(); repaint(); }
}

void PedalUnit::paint (juce::Graphics& g)
{
    using namespace juce;
    auto r = getLocalBounds().toFloat().reduced (2.0f);
    const bool on = footswitch.getToggleState();
    const auto matte = body.withSaturation (body.getSaturation() * 0.55f);

    g.setColour (Colours::black.withAlpha (0.6f));
    g.fillRoundedRectangle (r.translated (0.0f, 8.0f), 12.0f);
    g.setGradientFill (ColourGradient (matte.withMultipliedBrightness (on ? 0.62f : 0.38f), r.getX(), r.getY(),
                                       matte.withMultipliedBrightness (on ? 0.36f : 0.20f), r.getX(), r.getBottom(), false));
    g.fillRoundedRectangle (r, 12.0f);
    g.setGradientFill (ColourGradient (Colours::white.withAlpha (0.10f), r.getX(), r.getY(), Colours::transparentBlack, r.getX(), r.getY() + 40.0f, false));
    g.fillRoundedRectangle (r, 12.0f);
    g.setColour (Colours::black.withAlpha (0.4f));
    g.drawRoundedRectangle (r.reduced (0.5f), 12.0f, 1.0f);

    g.setColour (Colours::white.withAlpha (on ? 0.95f : 0.55f));
    g.setFont (spaced (13.0f, true, 0.25f));
    g.drawText (name.toUpperCase(), r.withHeight (48.0f).withTrimmedTop (18.0f), Justification::centred);
    const auto led = Rectangle<float> (8.0f, 8.0f).withCentre ({ r.getCentreX(), r.getY() + 58.0f });
    if (on)
    {
        g.setColour (Colour (0xffff3b30).withAlpha (0.35f));
        g.fillEllipse (led.expanded (6.0f));
    }
    g.setColour (on ? Colour (0xffff3b30) : Colour (0xff2a1211));
    g.fillEllipse (led);
}

void PedalUnit::resized()
{
    auto r = getLocalBounds().reduced (10, 0);
    r.removeFromTop (72);
    footswitch.setBounds (r.removeFromBottom (96).withSizeKeepingCentre (80, 80));
    r.removeFromBottom (6);
    const int kh = juce::jmin (124, r.getHeight() / juce::jmax (1, knobs.size()));
    for (auto* k : knobs)
        k->setBounds (r.removeFromTop (kh).reduced (10, 2));
}

//==============================================================================
PedalsPage::PedalsPage (EmberAmpProcessor& p)
    : proc (p),
      gatePedal   (p.apvts, "Gate",       juce::Colour (0xff7a8794), "gateOn",   { { "gate", "Threshold" } }),
      compPedal   (p.apvts, "Compressor", juce::Colour (0xff3d8f74), "compOn",   { { "sustain", "Sustain" } }),
      odPedal     (p.apvts, "Overdrive",  juce::Colour (0xff5d9b3a), "odOn",     { { "boost", "Drive" } }),
      dsPedal     (p.apvts, "Distortion", juce::Colour (0xffd9822b), "dsOn",     { { "dsDist", "Dist" }, { "dsTone", "Tone" }, { "dsLevel", "Level" } }),
      pitchPedal  (p.apvts, "Pitch",      juce::Colour (0xff7d5fc4), "pitchOn",  { { "pitch", "Shift" }, { "pitchMix", "Mix" } }),
      chorusPedal (p.apvts, "Chorus",     juce::Colour (0xff3f9fb0), "chorusOn", { { "chorusRate", "Rate" }, { "chorusDepth", "Depth" }, { "chorusMix", "Mix" } }),
      delayPedal  (p.apvts, "Delay",      juce::Colour (0xff3a74b4), "delayOn",  { { "delayTime", "Time" }, { "delayFeedback", "Repeats" }, { "delayMix", "Mix" } }),
      reverbPedal (p.apvts, "Reverb",     juce::Colour (0xff8a7c6c), "reverbOn", { { "reverbSize", "Size" }, { "reverbMix", "Mix" } })
{
    for (auto* u : { &gatePedal, &compPedal, &odPedal, &dsPedal, &pitchPedal, &chorusPedal, &delayPedal, &reverbPedal })
        addAndMakeVisible (u);
}

void PedalsPage::refresh()
{
    for (auto* u : { &gatePedal, &compPedal, &odPedal, &dsPedal, &pitchPedal, &chorusPedal, &delayPedal, &reverbPedal })
        u->refresh();
    if (proc.getSelectedAmp() != shownAmp) { shownAmp = proc.getSelectedAmp(); repaint(); }
}

void PedalsPage::paint (juce::Graphics& g)
{
    using namespace juce;
    auto r = getLocalBounds().toFloat();

    // pedalboard: dark anodised rails
    auto board = r.withTrimmedTop (30.0f).reduced (10.0f, 0.0f).withTrimmedBottom (8.0f);
    g.setColour (Colours::black.withAlpha (0.5f));
    g.fillRoundedRectangle (board.translated (0.0f, 8.0f), 14.0f);
    g.setGradientFill (ColourGradient (Colour (0xff1c1d22), board.getX(), board.getY(), Colour (0xff0d0e11), board.getX(), board.getBottom(), false));
    g.fillRoundedRectangle (board, 14.0f);
    g.setColour (Colours::white.withAlpha (0.025f));
    for (float yy = board.getY() + 20.0f; yy < board.getBottom() - 10.0f; yy += 26.0f)
        g.fillRect (board.getX() + 14.0f, yy, board.getWidth() - 28.0f, 10.0f);

    g.setColour (Palette::muted);
    g.setFont (spaced (11.0f, true, 0.3f));
    g.drawText ("BEFORE THE AMP", Rectangle<float> (r.getX() + 14.0f, r.getY(), 400.0f, 22.0f), Justification::centredLeft);
    g.drawText ("AFTER THE AMP", Rectangle<float> (r.getRight() - 414.0f, r.getY(), 400.0f, 22.0f), Justification::centredRight);

    const auto a = ampSlot.toFloat();
    const auto accent = EmberAmpEditor::ampAccent (proc.getSelectedAmp());
    g.setColour (Colours::white.withAlpha (0.04f));
    g.fillRoundedRectangle (a, 10.0f);
    g.setColour (accent.withAlpha (0.8f));
    g.drawRoundedRectangle (a.reduced (0.5f), 10.0f, 1.2f);
    g.setColour (Palette::muted);
    g.setFont (spaced (10.0f, true, 0.3f));
    g.drawText ("AMP", a.withHeight (a.getHeight() * 0.5f - 6.0f), Justification::centredBottom);
    g.setColour (Colours::white);
    g.setFont (spaced (13.0f, true, 0.15f));
    g.drawFittedText (EmberAmpProcessor::ampName (proc.getSelectedAmp()).toUpperCase(), a.withTrimmedTop (a.getHeight() * 0.5f).toNearestInt().reduced (6, 0),
                      Justification::centredTop, 1);

    g.setColour (Colour (0xff050506));
    g.fillRect (board.getX() + 20.0f, board.getCentreY() - 1.5f, board.getWidth() - 40.0f, 3.0f);
}

void PedalsPage::resized()
{
    auto r = getLocalBounds().withTrimmedTop (30).reduced (28, 22);
    const int gap = 12, slotW = 90;
    const int pedalW = (r.getWidth() - slotW - 8 * gap) / 8;
    for (auto* u : { &gatePedal, &compPedal, &odPedal, &dsPedal, &pitchPedal })
    {
        u->setBounds (r.removeFromLeft (pedalW));
        r.removeFromLeft (gap);
    }
    ampSlot = r.removeFromLeft (slotW).withSizeKeepingCentre (slotW, 110);
    r.removeFromLeft (gap);
    for (auto* u : { &chorusPedal, &delayPedal, &reverbPedal })
    {
        u->setBounds (r.removeFromLeft (pedalW));
        r.removeFromLeft (gap);
    }
}

//==============================================================================
EmberAmpEditor::EmberAmpEditor (EmberAmpProcessor& p)
    : AudioProcessorEditor (p), proc (p),
      inputKnob     (p.apvts, "input",  "Input",     juce::Colours::white, "ring", Palette::text, 13.0f),
      gateKnob      (p.apvts, "gate",   "",          juce::Colours::white, "ring", Palette::text, 13.0f),
      transposeKnob (p.apvts, "pitch",  "",          juce::Colours::white, "ring", Palette::text, 13.0f),
      outputKnob    (p.apvts, "master", "Output",    juce::Colours::white, "ring", Palette::text, 13.0f),
      pedalsPage (p)
{
    setLookAndFeel (&lnf);

    // ---- navigation
    const char* tabNames[] = { "PEDALS", "CLEAN", "CRUNCH", "DISTORTION", "SOLO" };
    for (int i = 0; i < 1 + EmberAmpProcessor::numAmps; ++i)
    {
        auto& t = tabs[i];
        t.setButtonText (tabNames[i]);
        t.getProperties().set ("tab", true);
        setAccent (t, i == 0 ? Palette::accent : ampAccent (i - 1));
        t.onClick = [this, i] { showPage (i); };
        addAndMakeVisible (t);
    }

    // ---- pages
    addChildComponent (pedalsPage);
    for (int a = 0; a < EmberAmpProcessor::numAmps; ++a)
    {
        ampPages[a] = std::make_unique<AmpPage> (*this, p, a);
        addChildComponent (*ampPages[a]);
    }

    // ---- top control strip
    for (auto* c : std::initializer_list<juce::Component*> { &inMeter, &outMeter, &inputKnob, &gateKnob, &transposeKnob, &outputKnob,
                                                             &gateSwitch, &transposeSwitch, &dynamicsSwitch })
        addAndMakeVisible (c);
    gateSwitchAtt      = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (p.apvts, "gateOn", gateSwitch);
    transposeSwitchAtt = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (p.apvts, "pitchOn", transposeSwitch);
    dynamicsAtt        = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (p.apvts, "dynamics", dynamicsSwitch);
    dynamicsSwitch.setTooltip ("On: the amp follows how hard you pick and your guitar's volume knob");
    transposeSwitch.setTooltip ("Pitch shifter (same as the Pitch pedal)");
    gateSwitch.setTooltip ("Noise gate (same as the Gate pedal)");

    prevPreset.setButtonText (juce::String::fromUTF8 ("\xe2\x86\x90"));
    nextPreset.setButtonText (juce::String::fromUTF8 ("\xe2\x86\x92"));
    for (auto* b : { &prevPreset, &nextPreset, &savePresetButton })
    {
        b->getProperties().set ("link", true);
        addAndMakeVisible (b);
    }
    prevPreset.onClick = [this] { stepPreset (-1); };
    nextPreset.onClick = [this] { stepPreset (+1); };
    savePresetButton.onClick = [this] { askToSavePreset(); };
    presetBox.onChange = [this] { presetChosen(); };
    addAndMakeVisible (presetBox);
    rebuildPresetMenu();

    // ---- status bar
    addAndMakeVisible (tuner);
    muteButton.setClickingTogglesState (true);
    muteButton.getProperties().set ("danger", true);
    muteAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (p.apvts, "inputMute", muteButton);
    addAndMakeVisible (muteButton);

   #if JucePlugin_Build_Standalone
    // In the standalone app, take over JUCE's own "input muted to avoid feedback" switch so
    // the mute button is the one place to mute/unmute. Its first-launch safety mute carries over.
    if (p.wrapperType == juce::AudioProcessor::wrapperType_Standalone)
        if (auto* holder = juce::StandalonePluginHolder::getInstance())
        {
            auto& wrapperMute = holder->getMuteInputValue();
            if ((bool) wrapperMute.getValue())
                if (auto* param = p.apvts.getParameter ("inputMute"))
                    param->setValueNotifyingHost (1.0f);
            wrapperMute.setValue (false);

            isStandalone = true;
            holder->deviceManager.addChangeListener (this);

            if (auto* settings = holder->settings.get())
            {
                if (! settings->getBoolValue ("emberLowLatencyApplied", false))
                {
                    applyLowLatencyBuffer (holder->deviceManager, 64);
                    settings->setValue ("emberLowLatencyApplied", true);
                }
                if (! PreferredAudio::restore (holder->deviceManager, *settings))
                    PreferredAudio::record (holder->deviceManager, *settings, false);
            }

            bufferBox.setTooltip ("Audio buffer size - lower = less delay, higher = fewer dropouts");
            bufferBox.onChange = [this]
            {
                if (auto* dm = getStandaloneDeviceManager())
                {
                    auto setup = dm->getAudioDeviceSetup();
                    setup.bufferSize = bufferBox.getSelectedId();
                    userChangedAudio = true;
                    dm->setAudioDeviceSetup (setup, true);
                    lastDeviceKey.clear();
                    refreshLatencyUI();
                }
            };
            addAndMakeVisible (bufferBox);

            // which input on your interface the guitar is plugged into (1-6)
            inputChannelBox.setTooltip ("Which input on your audio interface your guitar is plugged into");
            inputChannelBox.onChange = [this]
            {
                const int ch = inputChannelBox.getSelectedId() - 1;
                if (auto* dm = getStandaloneDeviceManager(); dm != nullptr && ch >= 0)
                {
                    auto setup = dm->getAudioDeviceSetup();
                    setup.inputChannels.clear();
                    setup.inputChannels.setBit (ch);            // only this input feeds the amp
                    setup.useDefaultInputChannels = false;
                    userChangedAudio = true;                    // saved as your choice
                    dm->setAudioDeviceSetup (setup, true);
                    lastDeviceKey.clear();
                    refreshLatencyUI();
                }
            };
            addAndMakeVisible (inputChannelBox);

            latencyLabel.setFont (spaced (12.0f, true, 0.1f));
            latencyLabel.setMinimumHorizontalScale (0.75f);
            addAndMakeVisible (latencyLabel);
            refreshLatencyUI();

            // Stream: a second copy of the sound (plus an optional mic) for Discord & co.
            stream = std::make_unique<StreamOutput> (p);
            setAccent (streamSwitch, juce::Colour (0xff7f8cff));
            streamSwitch.setTooltip ("Send Ember Amp (and your mic) to Discord, OBS or Zoom through a virtual cable");
            streamSwitch.onClick = [this]
            {
                streamWanted = streamSwitch.getToggleState();
                if (streamWanted) startStream (true);
                else              stopStream();
                saveStreamSettings();
            };
            addAndMakeVisible (streamSwitch);
            streamMoreButton.setButtonText ("...");
            streamMoreButton.setTooltip ("Stream settings");
            streamMoreButton.onClick = [this] { showStreamPanel(); };
            addAndMakeVisible (streamMoreButton);
            if (auto* settings = holder->settings.get())
            {
                stream->guitarGain = juce::Decibels::decibelsToGain ((float) settings->getDoubleValue ("emberStreamGuitarDb", 0.0));
                stream->micGain    = juce::Decibels::decibelsToGain ((float) settings->getDoubleValue ("emberStreamMicDb", 0.0));
                streamWanted = settings->getBoolValue ("emberStreamOn", false);
            }
            if (streamWanted) startStream (false);
            streamSwitch.setToggleState (streamWanted, juce::dontSendNotification);
        }
   #endif

    updateMuteButton();
    setWantsKeyboardFocus (true);

    pitchBuffer.resize (4096);
    logo = juce::ImageCache::getFromMemory (BinaryData::logo_png, BinaryData::logo_pngSize);

    setSize (1200, 800);
    showPage (1 + proc.getSelectedAmp());
    startTimerHz (30);
}

EmberAmpEditor::~EmberAmpEditor()
{
    stopTimer();
    stream.reset();
   #if JucePlugin_Build_Standalone
    if (isStandalone)
        if (auto* dm = getStandaloneDeviceManager())
            dm->removeChangeListener (this);
   #endif
    setLookAndFeel (nullptr);
}

//==============================================================================
void EmberAmpEditor::showPage (int page)
{
    currentPage = juce::jlimit (0, (int) EmberAmpProcessor::numAmps, page);
    pedalsPage.setVisible (currentPage == 0);
    for (int a = 0; a < EmberAmpProcessor::numAmps; ++a)
        ampPages[a]->setVisible (currentPage == a + 1);

    // opening another amp's page makes it the amp you hear, starting on its preset 1
    if (currentPage > 0 && proc.getSelectedAmp() != currentPage - 1)
    {
        proc.setCurrentProgram ((currentPage - 1) * EmberAmpProcessor::presetsPerAmp);
        presetsChanged();
    }

    for (int i = 0; i < 1 + EmberAmpProcessor::numAmps; ++i)
        tabs[i].setToggleState (i == currentPage, juce::dontSendNotification);
    repaint();
}

void EmberAmpEditor::presetsChanged()
{
    rebuildPresetMenu();
    for (auto& page : ampPages) page->refresh();
    pedalsPage.refresh();
    if (currentPage > 0 && proc.getSelectedAmp() != currentPage - 1)
        showPage (1 + proc.getSelectedAmp());
}

//==============================================================================
void EmberAmpEditor::changeListenerCallback (juce::ChangeBroadcaster*)
{
   #if JucePlugin_Build_Standalone
    // The audio settings window is modal, so a change made while any modal window is open is yours.
    const bool userChoice = userChangedAudio || juce::ModalComponentManager::getInstance()->getNumModalComponents() > 0;
    userChangedAudio = false;
    if (auto* holder = juce::StandalonePluginHolder::getInstance())
        if (auto* settings = holder->settings.get())
            PreferredAudio::record (holder->deviceManager, *settings, userChoice);
   #endif
    saveStandaloneState (true);
    lastDeviceKey.clear();
    refreshLatencyUI();

    // your interface changed (rate, input...): reopen the stream to match
    if (streamWanted)
        startStream (false);
}

void EmberAmpEditor::saveStandaloneState (bool includeDevice)
{
   #if JucePlugin_Build_Standalone
    if (auto* holder = juce::StandalonePluginHolder::getInstance())
    {
        if (includeDevice)
            holder->saveAudioDeviceState();
        holder->savePluginState();
        if (auto* file = dynamic_cast<juce::PropertiesFile*> (holder->settings.get()))
            file->saveIfNeeded();
    }
   #else
    juce::ignoreUnused (includeDevice);
   #endif
}

//==============================================================================
void EmberAmpEditor::paint (juce::Graphics& g)
{
    using namespace juce;
    const auto b = getLocalBounds().toFloat();

    // stage: near-black with a soft spotlight behind the amp
    g.setGradientFill (ColourGradient (Colour (0xff14161b), b.getCentreX(), 0.0f, Colour (0xff060708), b.getCentreX(), b.getBottom(), false));
    g.fillAll();
    const auto st = stageArea.toFloat();
    g.setGradientFill (ColourGradient (Colours::white.withAlpha (0.07f), st.getCentreX(), st.getY() + st.getHeight() * 0.35f,
                                       Colours::transparentBlack, st.getCentreX() + st.getWidth() * 0.55f, st.getY() + st.getHeight() * 0.35f, true));
    g.fillRect (st.expanded (0.0f, 60.0f));

    // logo (top-left)
    if (logo.isValid())
        g.drawImage (logo, Rectangle<float> (18.0f, 14.0f, 32.0f, 32.0f), RectanglePlacement::centred);
    g.setColour (Palette::text);
    g.setFont (spaced (15.0f, true, 0.30f));
    g.drawText ("EMBER", 56, 14, 120, 32, Justification::centredLeft);

    // dividers in the control strip
    g.setColour (Colours::white.withAlpha (0.10f));
    for (auto& d : dividers)
        g.fillRect (d);

    // status bar
    const auto sb = statusArea.toFloat();
    g.setColour (Colour (0xff050506));
    g.fillRect (sb);
    g.setColour (Colours::white.withAlpha (0.07f));
    g.drawHorizontalLine ((int) sb.getY(), sb.getX(), sb.getRight());
    g.setColour (Palette::muted);
    g.setFont (spaced (11.0f, false, 0.25f));
    g.drawText ("EMBER AUDIO", sb.withTrimmedRight (20.0f), Justification::centredRight);
}

void EmberAmpEditor::paintOverChildren (juce::Graphics& g)
{
    if (! dragHover) return;
    const int amp = currentPage > 0 ? currentPage - 1 : proc.getSelectedAmp();
    auto r = getLocalBounds().toFloat().reduced (6.0f);
    g.setColour (Palette::bg.withAlpha (0.82f));
    g.fillRoundedRectangle (r, 12.0f);
    g.setColour (ampAccent (amp));
    g.drawRoundedRectangle (r, 12.0f, 3.0f);
    g.setFont (juce::FontOptions (24.0f, juce::Font::bold));
    g.drawText ("Drop to load IR into the " + EmberAmpProcessor::ampName (amp) + " amp's cab", r, juce::Justification::centred);
}

void EmberAmpEditor::resized()
{
    auto area = getLocalBounds();

    // ---- navigation
    auto nav = area.removeFromTop (58);
    const int tabW = 120;
    auto tabRow = nav.withSizeKeepingCentre (tabW * 5, 44).translated (0, 4);
    for (auto& t : tabs)
        t.setBounds (tabRow.removeFromLeft (tabW));

    // ---- control strip
    auto strip = area.removeFromTop (124).reduced (24, 6);
    dividers.clear();
    inMeter.setBounds (strip.removeFromLeft (6).reduced (0, 8));
    strip.removeFromLeft (14);
    inputKnob.setBounds (strip.removeFromLeft (96));
    strip.removeFromLeft (14);
    dividers.push_back (strip.removeFromLeft (1).reduced (0, 14));
    strip.removeFromLeft (14);
    {
        auto col = strip.removeFromLeft (110);
        gateSwitch.setBounds (col.removeFromTop (22).withSizeKeepingCentre (100, 22));
        gateKnob.setBounds (col.translated (0, -18).withTrimmedBottom (-18));
    }
    strip.removeFromLeft (12);
    {
        auto col = strip.removeFromLeft (140);
        transposeSwitch.setBounds (col.removeFromTop (22).withSizeKeepingCentre (138, 22));
        transposeKnob.setBounds (col.translated (0, -18).withTrimmedBottom (-18).withSizeKeepingCentre (110, col.getHeight() + 18));
    }

    auto right = strip.removeFromRight (6);
    outMeter.setBounds (right.reduced (0, 8));
    strip.removeFromRight (14);
    outputKnob.setBounds (strip.removeFromRight (96));
    strip.removeFromRight (14);
    dividers.push_back (strip.removeFromRight (1).reduced (0, 14));
    strip.removeFromRight (14);
    dynamicsSwitch.setBounds (strip.removeFromRight (130).withSizeKeepingCentre (130, 26).translated (0, -24));
    strip.removeFromRight (20);

    // centre: preset arrows + save, then the preset menu
    {
        auto centre = strip.reduced (10, 0);
        auto top = centre.removeFromTop (30);
        prevPreset.setBounds (top.removeFromLeft (36));
        nextPreset.setBounds (top.removeFromLeft (36));
        savePresetButton.setBounds (top.removeFromRight (96));
        centre.removeFromTop (8);
        presetBox.setBounds (centre.removeFromTop (48));
    }

    // ---- status bar
    statusArea = area.removeFromBottom (40);
    {
        auto sb = statusArea.reduced (18, 6);
        tuner.setBounds (sb.removeFromLeft (290));
        sb.removeFromLeft (16);
        muteButton.setBounds (sb.removeFromLeft (110));
        sb.removeFromLeft (16);
        if (isStandalone)
        {
            bufferBox.setBounds (sb.removeFromLeft (104));
            sb.removeFromLeft (8);
            latencyLabel.setBounds (sb.removeFromLeft (70));
            sb.removeFromLeft (8);
            inputChannelBox.setBounds (sb.removeFromLeft (118));
            sb.removeFromLeft (20);
            streamSwitch.setBounds (sb.removeFromLeft (104));
            sb.removeFromLeft (4);
            streamMoreButton.setBounds (sb.removeFromLeft (34));
        }
    }

    // ---- stage
    stageArea = area.reduced (0, 4);
    pedalsPage.setBounds (stageArea.reduced (20, 10));
    for (auto& page : ampPages)
        page->setBounds (stageArea);
}

//==============================================================================
bool EmberAmpEditor::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (auto& f : files)
        if (EmberAmpProcessor::isIRFile (juce::File (f)))
            return true;
    return false;
}

void EmberAmpEditor::filesDropped (const juce::StringArray& files, int, int)
{
    dragHover = false;
    const int amp = currentPage > 0 ? currentPage - 1 : proc.getSelectedAmp();
    for (auto& f : files)
        if (proc.loadIR (amp, juce::File (f)))
            break;
    ampPages[amp]->refresh();
    repaint();
}

//==============================================================================
float EmberAmpEditor::detectPitch()
{
    // YIN pitch detection on the most recent input samples
    const double sr = proc.getSampleRateForTuner();
    const int total = (int) pitchBuffer.size();
    proc.copyRecentInput (pitchBuffer.data(), total);

    const int maxTau = juce::jmin ((int) (sr / 60.0), total / 2);
    const int minTau = juce::jmax (2, (int) (sr / 1200.0));
    const int window = total - maxTau;
    const float* x = pitchBuffer.data();

    double energy = 0.0;
    for (int j = 0; j < window; ++j) energy += (double) x[j] * x[j];
    if (std::sqrt (energy / window) < 0.002) return -1.0f;

    yin.assign ((size_t) maxTau + 2, 1.0f);
    double running = 0.0;
    for (int tau = 1; tau <= maxTau; ++tau)
    {
        double sum = 0.0;
        for (int j = 0; j < window; ++j)
        {
            const double d = x[j] - x[j + tau];
            sum += d * d;
        }
        running += sum;
        yin[(size_t) tau] = running > 0.0 ? (float) (sum * tau / running) : 1.0f;
    }

    int tau = -1;
    for (int t = minTau; t < maxTau; ++t)
        if (yin[(size_t) t] < 0.12f)
        {
            while (t + 1 < maxTau && yin[(size_t) t + 1] < yin[(size_t) t]) ++t;
            tau = t;
            break;
        }
    if (tau < 1) return -1.0f;

    const float s0 = yin[(size_t) tau - 1], s1 = yin[(size_t) tau], s2 = yin[(size_t) tau + 1];
    const float denom = s0 - 2.0f * s1 + s2;
    const float refined = std::abs (denom) > 1e-9f ? (float) tau + 0.5f * (s0 - s2) / denom : (float) tau;
    return (float) (sr / refined);
}

void EmberAmpEditor::updateMuteButton()
{
    const bool muted = muteButton.getToggleState();
    if (muted == lastMuteShown && muteButton.getButtonText().isNotEmpty())
        return;
    lastMuteShown = muted;
    muteButton.setButtonText (muted ? "INPUT MUTED" : "INPUT ON");
}

bool EmberAmpEditor::keyPressed (const juce::KeyPress& key)
{
    const auto c = key.getTextCharacter();
    if (c == 'm' || c == 'M') { muteButton.triggerClick(); return true; }
    if (c >= '1' && c <= '5')  { showPage (c - '1'); return true; }   // 1 = pedals, 2..5 = amps
    return false;
}

//==============================================================================
// Preset menu: factory presets grouped by amp (ids 1..16), your presets (ids 1000..), delete (id 9000)
static constexpr int userPresetIdBase = 1000, deletePresetId = 9000;

void EmberAmpEditor::rebuildPresetMenu()
{
    presetBox.clear (juce::dontSendNotification);
    for (int a = 0; a < EmberAmpProcessor::numAmps; ++a)
    {
        presetBox.addSectionHeading (EmberAmpProcessor::ampName (a) + " amp");
        for (int i = 0; i < EmberAmpProcessor::presetsPerAmp; ++i)
        {
            const int index = a * EmberAmpProcessor::presetsPerAmp + i;
            presetBox.addItem (proc.getProgramName (index), index + 1);
        }
    }

    userPresetFiles = proc.getUserPresets();
    presetBox.addSectionHeading ("My presets");
    if (userPresetFiles.isEmpty())
    {
        presetBox.addItem ("(click SAVE AS... to add one)", deletePresetId - 1);
        presetBox.setItemEnabled (deletePresetId - 1, false);
    }
    for (int k = 0; k < userPresetFiles.size(); ++k)
        presetBox.addItem (userPresetFiles[k].getFileNameWithoutExtension(), userPresetIdBase + k);

    if (proc.getCurrentUserPreset().isNotEmpty())
    {
        presetBox.addSeparator();
        presetBox.addItem ("Delete \"" + proc.getCurrentUserPreset() + "\"...", deletePresetId);
    }
    if (const int id = currentPresetMenuId(); id != 0)
        presetBox.setSelectedId (id, juce::dontSendNotification);
    else
        presetBox.setText (EmberAmpProcessor::ampName (proc.getSelectedAmp()) + " amp - pick a preset", juce::dontSendNotification);
    lastProgramShown = proc.getCurrentProgram();
    lastUserPresetShown = proc.getCurrentUserPreset();
}

int EmberAmpEditor::currentPresetMenuId() const
{
    const auto user = proc.getCurrentUserPreset();
    if (user.isNotEmpty())
    {
        for (int k = 0; k < userPresetFiles.size(); ++k)
            if (userPresetFiles[k].getFileNameWithoutExtension() == user)
                return userPresetIdBase + k;
        return 0;
    }
    if (EmberAmpProcessor::programAmp (proc.getCurrentProgram()) != proc.getSelectedAmp())
        return 0;   // you switched to another amp since loading this preset
    return proc.getCurrentProgram() + 1;
}

void EmberAmpEditor::stepPreset (int delta)
{
    // the ← → arrows walk through every preset in menu order (factory, then yours)
    std::vector<int> ids;
    for (int i = 0; i < proc.getNumPrograms(); ++i) ids.push_back (i + 1);
    for (int k = 0; k < userPresetFiles.size(); ++k) ids.push_back (userPresetIdBase + k);

    const int current = currentPresetMenuId();
    int pos = -1;
    for (size_t i = 0; i < ids.size(); ++i) if (ids[i] == current) pos = (int) i;
    if (pos < 0) pos = delta > 0 ? -1 : (int) ids.size();
    pos = (pos + delta + (int) ids.size()) % (int) ids.size();
    presetBox.setSelectedId (ids[(size_t) pos], juce::sendNotificationSync);
}

void EmberAmpEditor::presetChosen()
{
    const int id = presetBox.getSelectedId();
    if (id == deletePresetId)
    {
        presetBox.setSelectedId (currentPresetMenuId(), juce::dontSendNotification);
        const auto name = proc.getCurrentUserPreset();
        juce::Component::SafePointer<EmberAmpEditor> safe (this);
        juce::AlertWindow::showAsync (juce::MessageBoxOptions()
                                        .withIconType (juce::MessageBoxIconType::WarningIcon)
                                        .withTitle ("Delete preset")
                                        .withMessage ("Delete \"" + name + "\"? This can't be undone.")
                                        .withButton ("Delete").withButton ("Cancel")
                                        .withAssociatedComponent (this),
                                      [safe, name] (int result)
                                      {
                                          if (safe == nullptr || result != 1) return;
                                          EmberAmpProcessor::getUserPresetFolder().getChildFile (name + ".emberpreset").deleteFile();
                                          safe->proc.clearCurrentUserPreset();
                                          safe->presetsChanged();
                                      });
        return;
    }

    if (id >= userPresetIdBase && id - userPresetIdBase < userPresetFiles.size())
        proc.loadUserPreset (userPresetFiles[id - userPresetIdBase]);
    else if (id >= 1 && id <= proc.getNumPrograms())
        proc.setCurrentProgram (id - 1);

    presetsChanged();
}

void EmberAmpEditor::askToSavePreset()
{
    auto* w = new juce::AlertWindow ("Save preset", "Name your preset:", juce::MessageBoxIconType::NoIcon, this);
    const auto suggestion = proc.getCurrentUserPreset().isNotEmpty() ? proc.getCurrentUserPreset()
                                                                      : "My " + proc.getProgramName (proc.getCurrentProgram());
    w->addTextEditor ("name", suggestion);
    w->addButton ("Save", 1, juce::KeyPress (juce::KeyPress::returnKey));
    w->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

    juce::Component::SafePointer<EmberAmpEditor> safe (this);
    w->enterModalState (true, juce::ModalCallbackFunction::create ([safe, w] (int result)
    {
        if (safe == nullptr || result != 1) return;
        const auto name = juce::File::createLegalFileName (w->getTextEditorContents ("name").trim());
        if (name.isEmpty()) return;

        auto doSave = [safe, name]
        {
            if (safe == nullptr) return;
            if (! safe->proc.saveUserPreset (name))
                juce::AlertWindow::showAsync (juce::MessageBoxOptions().withIconType (juce::MessageBoxIconType::WarningIcon)
                                                .withTitle ("Couldn't save preset").withMessage ("Couldn't write the preset file.")
                                                .withButton ("OK"), nullptr);
            safe->presetsChanged();
        };

        if (EmberAmpProcessor::getUserPresetFolder().getChildFile (name + ".emberpreset").existsAsFile())
            juce::AlertWindow::showAsync (juce::MessageBoxOptions().withIconType (juce::MessageBoxIconType::QuestionIcon)
                                            .withTitle ("Replace preset?")
                                            .withMessage ("A preset named \"" + name + "\" already exists. Replace it?")
                                            .withButton ("Replace").withButton ("Cancel"),
                                          [doSave] (int r) { if (r == 1) doSave(); });
        else
            doSave();
    }), true);
}

//==============================================================================
void EmberAmpEditor::refreshLatencyUI()
{
   #if JucePlugin_Build_Standalone
    if (! isStandalone) return;

    auto* dm = getStandaloneDeviceManager();
    auto* dev = dm != nullptr ? dm->getCurrentAudioDevice() : nullptr;
    if (dev == nullptr)
    {
        latencyLabel.setText ("no device", juce::dontSendNotification);
        latencyLabel.setColour (juce::Label::textColourId, Palette::muted);
        return;
    }

    const int bs = dev->getCurrentBufferSizeSamples();
    const double sr = dev->getCurrentSampleRate();

    const auto active = dev->getActiveInputChannels();
    const auto inNames = dev->getInputChannelNames();
    const auto key = dev->getName() + "|" + juce::String (sr) + "|" + juce::String (bs) + "|" + active.toString (2) + "|" + juce::String (inNames.size());
    if (key != lastDeviceKey)
    {
        lastDeviceKey = key;

        inputChannelBox.clear (juce::dontSendNotification);
        for (int c = 0; c < 6; ++c)
        {
            const bool exists = c < inNames.size();
            inputChannelBox.addItem ("Input " + juce::String (c + 1) + (exists ? juce::String() : juce::String (" (n/a)")), c + 1);
            inputChannelBox.setItemEnabled (c + 1, exists);
        }
        const int first = active.findNextSetBit (0);
        if (first >= 0 && first < 6) inputChannelBox.setSelectedId (first + 1, juce::dontSendNotification);
        else                          inputChannelBox.setText ("Input -", juce::dontSendNotification);

        bufferBox.clear (juce::dontSendNotification);
        for (int s : dev->getAvailableBufferSizes())
            if (s >= 16 && s <= 2048)
                bufferBox.addItem (juce::String (s) + " smp", s);
        bufferBox.setSelectedId (bs, juce::dontSendNotification);
    }

    const int total = dev->getInputLatencyInSamples() + dev->getOutputLatencyInSamples() + 2 * bs + proc.getLatencySamples();
    const double ms = sr > 0 ? 1000.0 * total / sr : 0.0;
    latencyLabel.setText (juce::String (ms, 1) + " ms", juce::dontSendNotification);
    latencyLabel.setColour (juce::Label::textColourId, ms <= 10.0 ? Palette::good : ms <= 20.0 ? Palette::hot : juce::Colour (0xffff6a5c));
   #endif
}

void EmberAmpEditor::timerCallback()
{
    updateMuteButton();
    if (frameCounter % 15 == 0)
        refreshLatencyUI();
    if (isStandalone && frameCounter % 300 == 150)   // every ~10 s
    {
       #if JucePlugin_Build_Standalone
        // store your average playing level so the amps can be volume-matched at it
        if (auto* holder = juce::StandalonePluginHolder::getInstance())
            if (auto* settings = holder->settings.get())
                if (const int cnt = proc.playPeakCount.load(); cnt > 0)
                {
                    settings->setValue ("emberPlayPeakDb", proc.playPeakDbSum.load() / cnt);
                    settings->setValue ("emberPlaySeconds", cnt * 0.1);
                }
       #endif
        saveStandaloneState (false);
    }

   #if JucePlugin_Build_Standalone
    if (isStandalone && frameCounter % 60 == 30)     // every ~2 s: switch back to your interface if it returned
        if (auto* holder = juce::StandalonePluginHolder::getInstance())
            if (auto* settings = holder->settings.get())
                PreferredAudio::restore (holder->deviceManager, *settings);
   #endif

    inMeter.push (proc.inputPeak.exchange (0.0f));
    outMeter.push (proc.outputPeak.exchange (0.0f));

    // which amp is live (dot on its tab); follow it if a preset/host switched amps
    const int sel = proc.getSelectedAmp();
    if (sel != lastSelectedAmp)
    {
        lastSelectedAmp = sel;
        for (int a = 0; a < EmberAmpProcessor::numAmps; ++a)
        {
            tabs[a + 1].getProperties().set ("live", a == sel);
            tabs[a + 1].repaint();
        }
        if (currentPage > 0 && currentPage - 1 != sel)
            showPage (1 + sel);
        rebuildPresetMenu();
    }

    if (proc.getCurrentProgram() != lastProgramShown || proc.getCurrentUserPreset() != lastUserPresetShown)
        presetsChanged();

    if (frameCounter % 6 == 0)
    {
        for (auto& page : ampPages) page->refresh();
        pedalsPage.refresh();
    }

   #if JucePlugin_Build_Standalone
    // Stream: keep it matched to your interface's sample rate, and retry if its device went away
    if (stream != nullptr && streamWanted && frameCounter % 90 == 45)
        if (auto* dm = getStandaloneDeviceManager())
            if (auto* dev = dm->getCurrentAudioDevice())
                if (! stream->isRunning() || std::abs (stream->getSampleRate() - dev->getCurrentSampleRate()) > 1.0)
                    startStream (false);
   #endif

    if (++frameCounter % 2 == 0)
        tuner.setFrequency (detectPitch());
}

//==============================================================================
// Stream to Discord
#if JucePlugin_Build_Standalone
class StreamPanel : public juce::Component, private juce::Timer
{
public:
    explicit StreamPanel (EmberAmpEditor& e) : ed (e)
    {
        auto* settings = juce::StandalonePluginHolder::getInstance()->settings.get();

        title.setText ("STREAM TO DISCORD", juce::dontSendNotification);
        title.setFont (spaced (14.0f, true, 0.2f));
        addAndMakeVisible (title);
        info.setText ("Sends Ember Amp (and your mic) to a virtual cable. In Discord, pick that cable as your "
                      "microphone and turn off Noise Suppression, Echo Cancellation and Automatic Gain Control.",
                      juce::dontSendNotification);
        info.setFont (juce::FontOptions (13.0f));
        info.setColour (juce::Label::textColourId, Palette::text.withAlpha (0.75f));
        info.setJustificationType (juce::Justification::topLeft);
        addAndMakeVisible (info);

        for (auto* l : { &sendLabel, &micLabel, &guitarLabel, &micLevelLabel })
        {
            l->setFont (spaced (11.5f, true, 0.15f));
            l->setColour (juce::Label::textColourId, Palette::muted);
            addAndMakeVisible (l);
        }
        sendLabel.setText ("SEND TO", juce::dontSendNotification);
        micLabel.setText ("MIC", juce::dontSendNotification);
        guitarLabel.setText ("AMP LEVEL", juce::dontSendNotification);
        micLevelLabel.setText ("MIC LEVEL", juce::dontSendNotification);

        // outputs: the virtual cable first
        const auto cable = StreamOutput::findVirtualCable();
        const auto saved = settings != nullptr ? settings->getValue ("emberStreamDevice") : juce::String();
        outputs = StreamOutput::listDevices (false);
        for (int i = 0; i < outputs.size(); ++i)
            sendBox.addItem (outputs[i] + (outputs[i] == cable ? "  (virtual cable)" : ""), i + 1);
        const int pick = outputs.indexOf (saved.isNotEmpty() ? saved : cable);
        if (pick >= 0) sendBox.setSelectedId (pick + 1, juce::dontSendNotification);
        else           sendBox.setText ("Install a virtual cable first", juce::dontSendNotification);
        sendBox.onChange = [this] { apply(); };
        addAndMakeVisible (sendBox);

        // mic: any microphone on the computer (built-in, headset, USB) or another input on your interface
        micBox.addItem ("Off - guitar only", 1);
        auto& mainDm = juce::StandalonePluginHolder::getInstance()->deviceManager;
        const auto mainIn = mainDm.getAudioDeviceSetup().inputDeviceName;
        int mainIns = 0;
        if (auto* dev = mainDm.getCurrentAudioDevice())
            mainIns = dev->getInputChannelNames().size();
        for (auto& m : StreamOutput::listMics())
        {
            const bool isMain = m.device == mainIn;
            const int n = isMain ? mainIns : m.numChannels;
            for (int ch = 0; ch < juce::jmin (n, 8); ++ch)
            {
                juce::String label = m.device;
                if (n > 1) label << " - input " << (ch + 1);
                if (isMain) label << "  (your interface)";
                mics.push_back ({ m.device, ch });
                micBox.addItem (label, (int) mics.size() + 1);
            }
        }
        const int micCh = settings != nullptr ? settings->getIntValue ("emberStreamMicCh", -1) : -1;
        auto micDev = settings != nullptr ? settings->getValue ("emberStreamMicDevice") : juce::String();
        if (micDev.isEmpty()) micDev = mainIn;   // older setting: an input on the interface
        int sel = 1;
        for (size_t i = 0; i < mics.size(); ++i)
            if (micCh >= 0 && mics[i].device == micDev && mics[i].channel == micCh)
                sel = (int) i + 2;
        micBox.setSelectedId (sel, juce::dontSendNotification);
        micBox.onChange = [this] { apply(); };
        addAndMakeVisible (micBox);

        for (auto* s : { &guitarLevel, &micLevel })
        {
            s->setSliderStyle (juce::Slider::LinearHorizontal);
            s->setTextBoxStyle (juce::Slider::TextBoxRight, false, 64, 20);
            s->setColour (juce::Slider::trackColourId, juce::Colour (0xff7f8cff));
            s->setColour (juce::Slider::backgroundColourId, juce::Colours::white.withAlpha (0.1f));
            s->setColour (juce::Slider::thumbColourId, juce::Colours::white);
            s->setTextValueSuffix (" dB");
            s->setNumDecimalPlacesToDisplay (1);
            addAndMakeVisible (s);
        }
        guitarLevel.setRange (-24.0, 12.0, 0.5);
        micLevel.setRange (-24.0, 24.0, 0.5);
        guitarLevel.setValue (juce::Decibels::gainToDecibels (ed.stream->guitarGain.load()), juce::dontSendNotification);
        micLevel.setValue (juce::Decibels::gainToDecibels (ed.stream->micGain.load()), juce::dontSendNotification);
        guitarLevel.setDoubleClickReturnValue (true, 0.0);
        micLevel.setDoubleClickReturnValue (true, 0.0);
        guitarLevel.onValueChange = [this] { ed.stream->guitarGain = juce::Decibels::decibelsToGain ((float) guitarLevel.getValue()); ed.saveStreamSettings(); };
        micLevel.onValueChange    = [this] { ed.stream->micGain    = juce::Decibels::decibelsToGain ((float) micLevel.getValue());    ed.saveStreamSettings(); };

        status.setFont (juce::FontOptions (13.0f, juce::Font::bold));
        status.setJustificationType (juce::Justification::topLeft);
        addAndMakeVisible (status);
        getCable.setButtonText ("Get the free virtual cable");
       #if JUCE_MAC
        getCable.setURL (juce::URL ("https://existential.audio/blackhole/"));
       #else
        getCable.setURL (juce::URL ("https://vb-audio.com/Cable/"));
       #endif
        getCable.setFont (juce::FontOptions (13.0f), false, juce::Justification::centredLeft);
        getCable.setColour (juce::HyperlinkButton::textColourId, juce::Colour (0xff9aa4ff));
        addAndMakeVisible (getCable);

        setSize (420, 330);
        timerCallback();
        startTimerHz (10);
    }

    void paint (juce::Graphics& g) override
    {
        // little level meters next to the status
        auto m = meterArea.toFloat();
        for (int k = 0; k < 2; ++k)
        {
            auto bar = m.removeFromTop (5.0f);
            m.removeFromTop (4.0f);
            g.setColour (juce::Colours::white.withAlpha (0.08f));
            g.fillRoundedRectangle (bar, 2.0f);
            const float db = juce::Decibels::gainToDecibels (k == 0 ? outLevel : micLevelShown, -60.0f);
            g.setColour (k == 0 ? juce::Colour (0xff7f8cff) : Palette::good);
            g.fillRoundedRectangle (bar.withWidth (bar.getWidth() * juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 60.0f)), 2.0f);
        }
        g.setColour (Palette::muted);
        g.setFont (spaced (10.0f, true, 0.15f));
        g.drawText ("TO DISCORD", meterArea.translated (0, -14).withHeight (12), juce::Justification::left);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (18, 16);
        title.setBounds (r.removeFromTop (22));
        r.removeFromTop (4);
        info.setBounds (r.removeFromTop (52));
        r.removeFromTop (8);
        auto row = [&r] (juce::Label& l, juce::Component& c)
        {
            auto line = r.removeFromTop (30);
            l.setBounds (line.removeFromLeft (90));
            c.setBounds (line);
            r.removeFromTop (8);
        };
        row (sendLabel, sendBox);
        row (micLabel, micBox);
        row (guitarLabel, guitarLevel);
        row (micLevelLabel, micLevel);
        r.removeFromTop (4);
        auto bottom = r.removeFromTop (40);
        meterArea = bottom.removeFromRight (120).withTrimmedTop (16).withHeight (14);
        status.setBounds (bottom.withTrimmedRight (10));
        getCable.setBounds (r.removeFromTop (22));
    }

private:
    void apply()
    {
        if (auto* settings = juce::StandalonePluginHolder::getInstance()->settings.get())
        {
            const int idx = sendBox.getSelectedId() - 1;
            if (juce::isPositiveAndBelow (idx, outputs.size()))
                settings->setValue ("emberStreamDevice", outputs[idx]);
            const int m = micBox.getSelectedId() - 2;
            const bool on = juce::isPositiveAndBelow (m, (int) mics.size());
            settings->setValue ("emberStreamMicDevice", on ? mics[(size_t) m].device : juce::String());
            settings->setValue ("emberStreamMicCh", on ? mics[(size_t) m].channel : -1);
        }
        if (ed.streamWanted)
            ed.startStream (false);
    }

    void timerCallback() override
    {
        const auto text = ed.streamStatusText();
        status.setText (text, juce::dontSendNotification);
        status.setColour (juce::Label::textColourId, ed.stream->isRunning() ? Palette::good
                                                                              : ed.streamError.isNotEmpty() ? juce::Colour (0xffff6a5c) : Palette::muted);
        if (++ticks % 20 == 1)   // look for a newly installed cable every 2 s
            getCable.setVisible (StreamOutput::findVirtualCable().isEmpty());
        outLevel = juce::jmax (ed.stream->outPeak.exchange (0.0f), outLevel * 0.8f);
        micLevelShown = juce::jmax (ed.stream->micPeak.exchange (0.0f), micLevelShown * 0.8f);
        repaint (meterArea.expanded (2, 16));
    }

    EmberAmpEditor& ed;
    juce::Label title, info, sendLabel, micLabel, guitarLabel, micLevelLabel, status;
    juce::ComboBox sendBox, micBox;
    juce::Slider guitarLevel, micLevel;
    juce::HyperlinkButton getCable;
    juce::StringArray outputs;
    struct MicChoice { juce::String device; int channel; };
    std::vector<MicChoice> mics;
    juce::Rectangle<int> meterArea;
    float outLevel = 0.0f, micLevelShown = 0.0f;
    int ticks = 0;
};
#endif

juce::String EmberAmpEditor::streamStatusText() const
{
   #if JucePlugin_Build_Standalone
    if (stream == nullptr) return {};
    if (stream->isRunning())
    {
        if (stream->getMicProblem().isNotEmpty())
            return "Live: amp only. " + stream->getMicProblem();
        return "Live: sending to " + stream->getOutputName() + (stream->hasMic() ? " (amp + mic)" : " (amp)");
    }
    if (streamError.isNotEmpty()) return streamError;
    if (StreamOutput::findVirtualCable().isEmpty())
       #if JUCE_MAC
        return "No virtual cable yet. Install BlackHole 2ch (free), then reopen this panel.";
       #else
        return "No virtual cable yet. Install VB-Audio Virtual Cable (free), then reopen this panel.";
       #endif
    return "Off. Turn on STREAM to start.";
   #else
    return {};
   #endif
}

void EmberAmpEditor::startStream (bool showProblems)
{
   #if JucePlugin_Build_Standalone
    auto* holder = juce::StandalonePluginHolder::getInstance();
    if (stream == nullptr || holder == nullptr) return;
    auto* settings = holder->settings.get();

    StreamOutput::Settings s;
    s.outputDevice = settings != nullptr ? settings->getValue ("emberStreamDevice") : juce::String();
    const auto outputs = StreamOutput::listDevices (false);
    if (s.outputDevice.isEmpty() || ! outputs.contains (s.outputDevice))
        s.outputDevice = StreamOutput::findVirtualCable();

    const auto mainSetup = holder->deviceManager.getAudioDeviceSetup();
    if (auto* dev = holder->deviceManager.getCurrentAudioDevice())
        s.sampleRate = dev->getCurrentSampleRate();
    s.micChannel = settings != nullptr ? settings->getIntValue ("emberStreamMicCh", -1) : -1;
    s.micDevice = settings != nullptr ? settings->getValue ("emberStreamMicDevice") : juce::String();
    if (s.micDevice.isEmpty()) s.micDevice = mainSetup.inputDeviceName;   // older setting: an input on the interface
    if (s.micChannel < 0) s.micDevice.clear();

    if (s.outputDevice.isEmpty())
    {
        stream->stop();
        streamError = StreamOutput::listDevices (false).isEmpty() ? "No audio outputs found." : juce::String();
    }
    else if (s.outputDevice == mainSetup.outputDeviceName)
    {
        stream->stop();   // never send a second copy into your own speakers / headphones
        streamError = "Pick a virtual cable, not the device you listen on.";
    }
    else
        streamError = stream->start (s);

    if (! stream->isRunning() && showProblems)
        showStreamPanel();
   #else
    juce::ignoreUnused (showProblems);
   #endif
}

void EmberAmpEditor::stopStream()
{
    if (stream != nullptr) stream->stop();
    streamError.clear();
}

void EmberAmpEditor::saveStreamSettings()
{
   #if JucePlugin_Build_Standalone
    if (auto* holder = juce::StandalonePluginHolder::getInstance())
        if (auto* settings = holder->settings.get(); settings != nullptr && stream != nullptr)
        {
            settings->setValue ("emberStreamOn", streamWanted);
            settings->setValue ("emberStreamGuitarDb", juce::Decibels::gainToDecibels (stream->guitarGain.load()));
            settings->setValue ("emberStreamMicDb", juce::Decibels::gainToDecibels (stream->micGain.load()));
        }
   #endif
}

void EmberAmpEditor::showStreamPanel()
{
   #if JucePlugin_Build_Standalone
    if (stream == nullptr) return;
    juce::CallOutBox::launchAsynchronously (std::make_unique<StreamPanel> (*this), streamMoreButton.getBounds(), this);
   #endif
}
