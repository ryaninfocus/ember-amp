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


// Fine surface textures, made once: tolex grain and woven grille cloth
static const juce::Image& tolexTexture()
{
    static juce::Image img = []
    {
        juce::Image im (juce::Image::ARGB, 192, 192, true);
        juce::Random rng (7);
        for (int y = 0; y < im.getHeight(); ++y)
            for (int x = 0; x < im.getWidth(); ++x)
            {
                const float n = rng.nextFloat();
                im.setPixelAt (x, y, n > 0.5f ? juce::Colours::white.withAlpha ((n - 0.5f) * 0.07f)
                                              : juce::Colours::black.withAlpha ((0.5f - n) * 0.30f));
            }
        return im;
    }();
    return img;
}

static const juce::Image& clothTexture()
{
    static juce::Image img = []
    {
        juce::Image im (juce::Image::ARGB, 96, 96, true);
        juce::Random rng (11);
        for (int y = 0; y < im.getHeight(); ++y)
            for (int x = 0; x < im.getWidth(); ++x)
            {
                // over-under weave: alternating 2x2 cells, lit threads and dark gaps
                const bool warp = ((x / 2) + (y / 2)) % 2 == 0;
                const bool gap = (x % 2 == 1 && y % 2 == 1);
                float a = gap ? 0.0f : (warp ? 0.10f : 0.05f);
                a += (rng.nextFloat() - 0.5f) * 0.03f;
                im.setPixelAt (x, y, gap ? juce::Colours::black.withAlpha (0.45f) : juce::Colours::white.withAlpha (juce::jmax (0.0f, a)));
            }
        return im;
    }();
    return img;
}

static void fillTextured (juce::Graphics& g, const juce::Path& area, const juce::Image& tex, float opacity = 1.0f)
{
    g.saveState();
    g.reduceClipRegion (area);
    g.setOpacity (opacity);
    g.setTiledImageFill (tex, 0, 0, opacity);
    g.fillPath (area);
    g.restoreState();
}

// brushed metal: a base gradient plus fine horizontal streaks
static void fillBrushed (juce::Graphics& g, juce::Rectangle<float> r, juce::Colour top, juce::Colour bottom, float streak)
{
    using namespace juce;
    g.setGradientFill (ColourGradient (top, r.getX(), r.getY(), bottom, r.getX(), r.getBottom(), false));
    g.fillRect (r);
    Random rng (3);
    g.saveState();
    g.reduceClipRegion (r.toNearestInt());
    for (float yy = r.getY(); yy < r.getBottom(); yy += 1.0f)
    {
        const float a = rng.nextFloat() * streak;
        g.setColour ((rng.nextBool() ? Colours::white : Colours::black).withAlpha (a));
        g.drawHorizontalLine ((int) yy, r.getX(), r.getRight());
    }
    // soft horizontal sheen
    ColourGradient sheen (Colours::white.withAlpha (0.0f), r.getX(), r.getY(), Colours::white.withAlpha (0.0f), r.getRight(), r.getY(), false);
    sheen.addColour (0.35, Colours::white.withAlpha (streak * 1.6f));
    sheen.addColour (0.55, Colours::white.withAlpha (0.0f));
    g.setGradientFill (sheen);
    g.fillRect (r);
    g.restoreState();
}

// Line icons for the tabs and preset buttons
static void drawIcon (juce::Graphics& g, const juce::String& name, juce::Rectangle<float> r, juce::Colour c)
{
    using namespace juce;
    const float s = jmin (r.getWidth(), r.getHeight());
    auto b = Rectangle<float> (s, s).withCentre (r.getCentre());
    const float w = jmax (1.4f, s * 0.075f);
    g.setColour (c);
    auto stroke = [&] (const Path& p) { g.strokePath (p, PathStrokeType (w, PathStrokeType::curved, PathStrokeType::rounded)); };
    if (name == "pedal")
    {
        auto box = b.reduced (s * 0.22f, s * 0.06f);
        g.drawRoundedRectangle (box, s * 0.08f, w);
        g.drawEllipse (Rectangle<float> (s * 0.16f, s * 0.16f).withCentre ({ box.getX() + box.getWidth() * 0.3f, box.getY() + s * 0.2f }), w * 0.8f);
        g.drawEllipse (Rectangle<float> (s * 0.16f, s * 0.16f).withCentre ({ box.getRight() - box.getWidth() * 0.3f, box.getY() + s * 0.2f }), w * 0.8f);
        g.drawEllipse (Rectangle<float> (s * 0.26f, s * 0.26f).withCentre ({ box.getCentreX(), box.getBottom() - s * 0.2f }), w);
    }
    else if (name == "amp")
    {
        auto box = b.reduced (s * 0.04f, s * 0.24f);
        g.drawRoundedRectangle (box, s * 0.06f, w);
        g.drawLine (box.getX(), box.getY() + box.getHeight() * 0.62f, box.getRight(), box.getY() + box.getHeight() * 0.62f, w * 0.8f);
        for (int i = 0; i < 4; ++i)
            g.fillEllipse (Rectangle<float> (w * 1.6f, w * 1.6f).withCentre ({ box.getX() + box.getWidth() * (0.25f + 0.17f * (float) i), box.getY() + box.getHeight() * 0.81f }));
    }
    else if (name == "cab")
    {
        auto box = b.reduced (s * 0.1f);
        g.drawRoundedRectangle (box, s * 0.06f, w);
        g.drawEllipse (box.reduced (s * 0.14f), w);
        g.fillEllipse (Rectangle<float> (s * 0.16f, s * 0.16f).withCentre (box.getCentre()));
    }
    else if (name == "eq")
    {
        for (int i = 0; i < 3; ++i)
        {
            const float x = b.getX() + s * (0.25f + 0.25f * (float) i);
            g.drawLine (x, b.getY() + s * 0.1f, x, b.getBottom() - s * 0.1f, w);
            const float y = b.getY() + s * (i == 1 ? 0.62f : i == 0 ? 0.35f : 0.45f);
            g.drawEllipse (Rectangle<float> (s * 0.17f, s * 0.17f).withCentre ({ x, y }), w);
        }
    }
    else if (name == "save")
    {
        auto box = b.reduced (s * 0.14f);
        Path p; p.startNewSubPath (box.getX(), box.getY()); p.lineTo (box.getRight() - s * 0.16f, box.getY()); p.lineTo (box.getRight(), box.getY() + s * 0.16f);
        p.lineTo (box.getRight(), box.getBottom()); p.lineTo (box.getX(), box.getBottom()); p.closeSubPath();
        stroke (p);
        g.drawRect (Rectangle<float> (box.getWidth() * 0.5f, box.getHeight() * 0.28f).withPosition (box.getX() + box.getWidth() * 0.2f, box.getY()), w * 0.8f);
        g.drawRect (Rectangle<float> (box.getWidth() * 0.6f, box.getHeight() * 0.32f).withPosition (box.getX() + box.getWidth() * 0.2f, box.getBottom() - box.getHeight() * 0.36f), w * 0.8f);
    }
    else if (name == "trash")
    {
        auto box = b.reduced (s * 0.2f, s * 0.12f);
        g.drawLine (box.getX() - s * 0.05f, box.getY() + s * 0.1f, box.getRight() + s * 0.05f, box.getY() + s * 0.1f, w);
        g.drawLine (box.getCentreX() - s * 0.08f, box.getY(), box.getCentreX() + s * 0.08f, box.getY(), w);
        Path p; p.startNewSubPath (box.getX() + s * 0.04f, box.getY() + s * 0.18f); p.lineTo (box.getX() + s * 0.08f, box.getBottom());
        p.lineTo (box.getRight() - s * 0.08f, box.getBottom()); p.lineTo (box.getRight() - s * 0.04f, box.getY() + s * 0.18f);
        stroke (p);
    }
    else if (name == "import")
    {
        Path p; p.startNewSubPath (b.getCentreX() + s * 0.05f, b.getY() + s * 0.15f); p.lineTo (b.getRight() - s * 0.15f, b.getY() + s * 0.15f);
        p.lineTo (b.getRight() - s * 0.15f, b.getBottom() - s * 0.15f); p.lineTo (b.getCentreX() + s * 0.05f, b.getBottom() - s * 0.15f);
        stroke (p);
        Path a; a.startNewSubPath (b.getX() + s * 0.12f, b.getCentreY()); a.lineTo (b.getCentreX() + s * 0.15f, b.getCentreY());
        a.startNewSubPath (b.getCentreX(), b.getCentreY() - s * 0.14f); a.lineTo (b.getCentreX() + s * 0.15f, b.getCentreY()); a.lineTo (b.getCentreX(), b.getCentreY() + s * 0.14f);
        stroke (a);
    }
    else if (name == "search")
    {
        const auto cc = b.getCentre().translated (-s * 0.07f, -s * 0.07f);
        g.drawEllipse (Rectangle<float> (s * 0.48f, s * 0.48f).withCentre (cc), w);
        g.drawLine (cc.x + s * 0.17f, cc.y + s * 0.17f, b.getRight() - s * 0.14f, b.getBottom() - s * 0.14f, w * 1.2f);
    }
    else if (name == "left" || name == "right")
    {
        const float dir = name == "left" ? -1.0f : 1.0f;
        Path p; p.startNewSubPath (b.getCentreX() - dir * s * 0.12f, b.getCentreY() - s * 0.25f);
        p.lineTo (b.getCentreX() + dir * s * 0.12f, b.getCentreY());
        p.lineTo (b.getCentreX() - dir * s * 0.12f, b.getCentreY() + s * 0.25f);
        stroke (p);
    }
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

    if (style == "clay")
    {
        // sculpted knob: a soft matte cylinder, a small pointer, a fine dotted scale
        const bool lightPanel = (bool) s.getProperties()["lightPanel"];
        const bool lightKnob = s.getProperties()["knobLight"].isVoid() ? lightPanel : (bool) s.getProperties()["knobLight"];
        const float kr = radius * 0.62f;
        const auto ink = lightPanel ? Colour (0xff3a3d43) : Colour (0xffd9dbdf);

        g.setColour (ink.withAlpha (0.55f));
        for (int i = 0; i <= 10; ++i)
        {
            const float a = startAngle + (float) i / 10.0f * (endAngle - startAngle);
            const float rr = radius * 0.90f;
            const float d = i % 5 == 0 ? 2.2f : 1.4f;
            g.fillEllipse (Rectangle<float> (d, d).withCentre ({ c.x + rr * std::sin (a), c.y - rr * std::cos (a) }));
        }

        // optional value arc (EQ page): shows the setting at a glance even over a wide range
        if ((bool) s.getProperties()["arc"])
        {
            const float ar = radius * 0.97f;
            Path track; track.addCentredArc (c.x, c.y, ar, ar, 0.0f, startAngle, endAngle, true);
            g.setColour (ink.withAlpha (0.15f));
            g.strokePath (track, PathStrokeType (2.2f, PathStrokeType::curved, PathStrokeType::rounded));
            const bool bipolar = (bool) s.getProperties()["bipolar"];
            const float from = bipolar ? (startAngle + endAngle) * 0.5f : (bool) s.getProperties()["fromEnd"] ? endAngle : startAngle;
            Path val; val.addCentredArc (c.x, c.y, ar, ar, 0.0f, jmin (from, angle), jmax (from, angle), true);
            g.setColour (accent);
            g.strokePath (val, PathStrokeType (2.6f, PathStrokeType::curved, PathStrokeType::rounded));
        }

        // contact shadow, then the body (lit from above)
        g.setGradientFill (ColourGradient (Colours::black.withAlpha (lightPanel ? 0.35f : 0.6f), c.x, c.y + kr * 0.5f,
                                           Colours::transparentBlack, c.x, c.y + kr * 1.6f, true));
        g.fillEllipse (Rectangle<float> (kr * 2.6f, kr * 2.4f).withCentre (c.translated (0.0f, kr * 0.35f)));
        const auto hi = lightKnob ? Colour (0xfffdfdfe) : Colour (0xff55585e);
        const auto lo = lightKnob ? Colour (0xffbfc2c7) : Colour (0xff17181b);
        g.setGradientFill (ColourGradient (lo, c.x, c.y + kr, hi, c.x, c.y - kr, false));
        g.fillEllipse (Rectangle<float> (kr * 2.0f, kr * 2.0f).withCentre (c.translated (0.0f, 2.0f)));   // side wall
        g.setGradientFill (ColourGradient (hi, c.x - kr * 0.4f, c.y - kr, lo.interpolatedWith (hi, 0.45f), c.x + kr * 0.3f, c.y + kr, false));
        g.fillEllipse (Rectangle<float> (kr * 1.86f, kr * 1.86f).withCentre (c));                            // top face
        g.setColour (Colours::white.withAlpha (lightKnob ? 0.9f : 0.18f));
        g.drawEllipse (Rectangle<float> (kr * 1.86f, kr * 1.86f).withCentre (c).reduced (0.6f), 0.8f);
        g.setColour (Colours::black.withAlpha (lightKnob ? 0.18f : 0.6f));
        g.drawEllipse (Rectangle<float> (kr * 2.0f, kr * 2.0f).withCentre (c.translated (0.0f, 2.0f)), 0.8f);

        // pointer: a small dark wedge near the rim
        Path ptr;
        ptr.addTriangle (0.0f, -kr * 0.82f, -kr * 0.13f, -kr * 0.56f, kr * 0.13f, -kr * 0.56f);
        ptr.applyTransform (AffineTransform::rotation (angle).translated (c.x, c.y));
        g.setColour (lightKnob ? Colour (0xff1b1c20) : Colours::white.withAlpha (0.95f));
        g.fillPath (ptr);
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

    if (props["tab"] || props["link"] || props["iconOnly"].toString().isNotEmpty())
        return;   // text / icon only

    if (props["footswitch"])
    {
        const auto c = r.getCentre();
        const float rad = jmin (r.getWidth(), r.getHeight()) * 0.40f;
        // silver footswitch: knurled ring, domed cap with a small emblem (pressed = slightly lower)
        g.setGradientFill (ColourGradient (Colours::black.withAlpha (0.55f), c.x, c.y + rad * 0.4f, Colours::transparentBlack, c.x, c.y + rad * 1.5f, true));
        g.fillEllipse (Rectangle<float> (rad * 2.6f, rad * 2.4f).withCentre (c.translated (0.0f, rad * 0.3f)));
        g.setGradientFill (ColourGradient (Colour (0xffe9ebee), c.x - rad * 0.6f, c.y - rad, Colour (0xff6c6f75), c.x + rad * 0.6f, c.y + rad, false));
        g.fillEllipse (c.x - rad, c.y - rad, rad * 2.0f, rad * 2.0f);
        g.setColour (Colours::black.withAlpha (0.25f));
        for (int i = 0; i < 36; ++i)   // knurling
        {
            const float a = (float) i / 36.0f * MathConstants<float>::twoPi;
            g.drawLine (c.x + rad * 0.80f * std::sin (a), c.y - rad * 0.80f * std::cos (a), c.x + rad * 0.98f * std::sin (a), c.y - rad * 0.98f * std::cos (a), 0.8f);
        }
        const float ir = rad * (down ? 0.66f : 0.72f);
        const auto cc = c.translated (0.0f, down ? 1.0f : 0.0f);
        g.setGradientFill (ColourGradient (Colour (0xfffafbfc), cc.x - ir * 0.5f, cc.y - ir, Colour (highlighted ? 0xffb7bac0 : 0xffa4a7ad), cc.x + ir * 0.4f, cc.y + ir, false));
        g.fillEllipse (cc.x - ir, cc.y - ir, ir * 2.0f, ir * 2.0f);
        g.setColour (Colours::black.withAlpha (0.3f));
        g.drawEllipse (cc.x - ir, cc.y - ir, ir * 2.0f, ir * 2.0f, 0.8f);
        g.setColour (Colour (0xff3a3c42).withAlpha (0.8f));   // emblem
        g.drawEllipse (Rectangle<float> (ir * 0.7f, ir * 0.7f).withCentre (cc), 1.2f);
        g.fillEllipse (Rectangle<float> (ir * 0.22f, ir * 0.22f).withCentre (cc));
        return;
    }

    if (props["choice"])   // one of a set of options (cab / mic lists): the chosen one lights up
    {
        const auto acc = accentOf (b);
        g.setColour (on ? acc.withAlpha (0.22f) : Colours::white.withAlpha (down ? 0.10f : highlighted ? 0.07f : 0.035f));
        g.fillRoundedRectangle (r, 5.0f);
        g.setColour (on ? acc.withAlpha (0.9f) : Colours::white.withAlpha (0.08f));
        g.drawRoundedRectangle (r.reduced (0.5f), 5.0f, on ? 1.4f : 1.0f);
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
        const auto col = on ? Colours::white : highlighted ? Colours::white.withAlpha (0.8f) : Colours::white.withAlpha (0.5f);
        const auto icon = props["icon"].toString();
        if (icon.isNotEmpty())
        {
            drawIcon (g, icon, r.withHeight (32.0f).withY (r.getY() + 2.0f), col);
            g.setColour (col.withMultipliedAlpha (0.85f));
            g.setFont (spaced (8.5f, true, 0.25f));
            g.drawText (b.getButtonText(), r.withY (r.getY() + 35.0f).withHeight (12.0f), Justification::centred);
        }
        else
        {
            g.setColour (col);
            g.setFont (spaced (13.0f, on, 0.18f));
            g.drawText (b.getButtonText(), r.withTrimmedBottom (10.0f), Justification::centred);
        }
        if (on)
        {
            g.setColour (Colours::white);
            g.fillEllipse (Rectangle<float> (4.0f, 4.0f).withCentre ({ r.getCentreX(), r.getBottom() - 3.0f }));
        }
        if (props["live"])   // the amp you hear
        {
            g.setColour (accentOf (b));
            g.fillEllipse (r.getCentreX() + 18.0f, r.getY() + 4.0f, 6.0f, 6.0f);
        }
        return;
    }
    if (props["iconOnly"].toString().isNotEmpty())
    {
        drawIcon (g, props["iconOnly"].toString(), r.reduced (3.0f),
                  Colours::white.withAlpha (! b.isEnabled() ? 0.25f : highlighted ? 1.0f : 0.75f));
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
    if (b.getProperties()["vswitch"])
    {
        // a vertical flip switch with a label beside each end
        const bool on = b.getToggleState();
        const bool knobTop = (bool) b.getProperties()["onIsTop"] ? on : ! on;
        auto slot = Rectangle<float> (20.0f, jmin (44.0f, r.getHeight() - 4.0f)).withPosition (r.getX() + 2.0f, r.getCentreY() - jmin (22.0f, r.getHeight() * 0.5f - 2.0f));
        const auto ink = (bool) b.getProperties()["darkText"] ? Colour (0xff24262b) : Colours::white;
        g.setColour (Colours::black.withAlpha (0.45f));
        g.fillRoundedRectangle (slot, 10.0f);
        g.setColour (ink.withAlpha (highlighted ? 0.6f : 0.4f));
        g.drawRoundedRectangle (slot.reduced (0.5f), 10.0f, 1.3f);
        const auto k = Rectangle<float> (14.0f, 14.0f).withCentre ({ slot.getCentreX(), knobTop ? slot.getY() + 10.0f : slot.getBottom() - 10.0f });
        g.setColour (Colours::white.withAlpha (0.9f));
        g.fillEllipse (k);
        g.setFont (spaced (10.5f, true, 0.12f));
        auto labels = r.withTrimmedLeft (30.0f);
        g.setColour (ink.withAlpha (knobTop ? 0.95f : 0.4f));
        g.drawText (b.getProperties()["topText"].toString(), labels.withHeight (r.getHeight() * 0.5f), Justification::centredLeft);
        g.setColour (ink.withAlpha (knobTop ? 0.4f : 0.95f));
        g.drawText (b.getProperties()["bottomText"].toString(), labels.withTrimmedTop (r.getHeight() * 0.5f), Justification::centredLeft);
        return;
    }
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
    const bool panelStyle = style == "chrome" || style == "black" || style == "clay";
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
    // chassisTop/Bottom = the sculpted frame, grille = the face, grilleLine = engraved traces,
    // panelTop/Bottom = the control strip, text = printed labels
    using juce::Colour;
    switch (amp)
    {
        case 0:  return { Colour (0xfff4f5f7), Colour (0xffc9ccd1), Colour (0xffe3e5e8), Colour (0xff4d5158),
                          Colour (0xffe8eaed), Colour (0xffd3d6da), Colour (0xff35383e), true };    // Clean: matte white
        case 1:  return { Colour (0xfff1ebe0), Colour (0xffcbc1b0), Colour (0xffe2dacd), Colour (0xff5a4c38),
                          Colour (0xffe8e1d5), Colour (0xffd2c8b8), Colour (0xff3f3528), true };    // Crunch: warm ivory
        case 2:  return { Colour (0xff45484e), Colour (0xff1c1d20), Colour (0xff2a2c31), Colour (0xffa9aeb6),
                          Colour (0xff303237), Colour (0xff222327), Colour (0xffdcdee2), false };   // Distortion: graphite
        default: return { Colour (0xff423d4d), Colour (0xff1b1922), Colour (0xff292532), Colour (0xffb1a6c8),
                          Colour (0xff2f2b38), Colour (0xff221f29), Colour (0xffe1dbec), false };   // Solo: slate violet
    }
}

AmpPage::AmpPage (EmberAmpEditor& o, EmberAmpProcessor& p, int a)
    : owner (o), proc (p), amp (a), accent (EmberAmpEditor::ampAccent (a)), theme (themeFor (a))
{
    const juce::String knobStyle = "clay";
    std::vector<std::pair<const char*, const char*>> list;
    if (amp != EmberAmpProcessor::clean) list.push_back ({ "drive", "Drive" });
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

    cabSummary.setFont (spaced (11.5f, true, 0.18f));
    cabSummary.setColour (juce::Label::textColourId, Palette::text.withAlpha (0.85f));
    cabSummary.setMinimumHorizontalScale (0.7f);
    addAndMakeVisible (cabSummary);
    editCabButton.setTooltip ("Choose the cabinet and microphone, and place the mic");
    editCabButton.onClick = [this] { owner.showPage (EmberAmpEditor::cabPageIndex); };
    addAndMakeVisible (editCabButton);
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
    irLabel.setText (ir ? "IR  " + proc.getIRFile (amp).getFileNameWithoutExtension() : juce::String ("NO IR LOADED"),
                     juce::dontSendNotification);
    {
        auto raw = [this] (const char* id) { return proc.apvts.getRawParameterValue (EmberAmpProcessor::ampParamID (amp, id))->load(); };
        const auto text = ir ? juce::String ("CUSTOM IR")
                             : EmberAmpProcessor::cabNames()[juce::roundToInt (raw ("cabModel"))].toUpperCase() + "   /   "
                               + EmberAmpProcessor::micNames()[juce::roundToInt (raw ("mic"))].toUpperCase() + "   /   "
                               + proc.apvts.getParameter (EmberAmpProcessor::ampParamID (amp, "micPos"))->getCurrentValueAsText().toUpperCase() + "   /   "
                               + proc.apvts.getParameter (EmberAmpProcessor::ampParamID (amp, "micDist"))->getCurrentValueAsText().toUpperCase();
        if (cabSummary.getText() != text)
            cabSummary.setText (text, juce::dontSendNotification);
    }
    irLabel.setColour (juce::Label::textColourId, ir ? accent : Palette::muted.withAlpha (0.7f));
    clearIRButton.setEnabled (ir);

    const bool active = proc.getSelectedAmp() == amp;
    const bool cabOn = cabButton.getToggleState();
    if (active != shownActive || cabOn != shownCab) { shownActive = active; shownCab = cabOn; repaint(); }
}

void AmpPage::paint (juce::Graphics& g)
{
    using namespace juce;
    const auto h = head.toFloat();
    const bool light = theme.lightPanel;
    const float frameR = 30.0f;

    // ---- rubber feet + shadow on the cab
    g.setGradientFill (ColourGradient (Colours::black.withAlpha (0.55f), h.getCentreX(), h.getBottom(),
                                       Colours::transparentBlack, h.getCentreX(), h.getBottom() + 26.0f, false));
    g.fillRect (h.withY (h.getBottom() - 6.0f).withHeight (32.0f).reduced (40.0f, 0.0f));
    for (float fx : { h.getX() + 110.0f, h.getRight() - 190.0f })
    {
        auto foot = Rectangle<float> (fx, h.getBottom() - 8.0f, 80.0f, 30.0f);
        g.setGradientFill (ColourGradient (Colour (0xff3b3d48), foot.getX(), foot.getY(), Colour (0xff15161b), foot.getX(), foot.getBottom(), false));
        g.fillRoundedRectangle (foot, 6.0f);
        g.setColour (Colours::white.withAlpha (0.12f));
        g.drawHorizontalLine ((int) foot.getBottom() - 10, foot.getX() + 6.0f, foot.getRight() - 6.0f);
    }

    // ---- sculpted frame: soft top light, darker underside, a crisp rim
    Path frame; frame.addRoundedRectangle (h, frameR);
    g.setColour (Colours::black.withAlpha (0.45f));
    g.fillRoundedRectangle (h.translated (0.0f, 6.0f), frameR);
    g.setGradientFill (ColourGradient (theme.chassisTop, h.getX(), h.getY(), theme.chassisBottom, h.getX(), h.getBottom(), false));
    g.fillPath (frame);
    g.setGradientFill (ColourGradient (Colours::white.withAlpha (light ? 0.7f : 0.16f), h.getCentreX(), h.getY(),
                                       Colours::transparentBlack, h.getCentreX(), h.getY() + 30.0f, false));
    g.strokePath (frame, PathStrokeType (2.0f));

    // ---- recessed face (the area inside the frame)
    const auto face = grille.getUnion (panel).toFloat();
    g.setColour (Colours::black.withAlpha (light ? 0.22f : 0.6f));
    g.fillRoundedRectangle (face.expanded (2.0f), 6.0f);
    g.setColour (theme.grille);
    g.fillRect (face);

    const auto gr = grille.toFloat();
    g.saveState();
    g.reduceClipRegion (grille);
    {
        // engraved circuit traces, mirrored left and right
        const auto ec = gr.getCentre();
        const float er = jmin (gr.getHeight() * 0.95f, gr.getWidth() * 0.22f);
        const auto dark = theme.grilleLine.withAlpha (light ? 0.55f : 0.45f);
        const auto lit  = light ? Colours::white.withAlpha (0.8f) : Colours::black.withAlpha (0.5f);
        Random rng (17 + amp);
        for (int side = 0; side < 2; ++side)
        {
            const float dir = side == 0 ? -1.0f : 1.0f;
            const float startX = ec.x + dir * er * 0.80f;
            const float edgeX  = side == 0 ? gr.getX() + 30.0f : gr.getRight() - 30.0f;
            Random r2 (rng.nextInt());
            for (int t = 0; t < 11; ++t)
            {
                const float y0 = gr.getY() + gr.getHeight() * (0.10f + 0.80f * (float) t / 10.0f) + (r2.nextFloat() - 0.5f) * 8.0f;
                Path tr; tr.startNewSubPath (startX, y0);
                float x = startX, y = y0;
                const float run1 = (20.0f + r2.nextFloat() * 60.0f);
                x += dir * run1; tr.lineTo (x, y);
                const float jog = (r2.nextFloat() - 0.5f) * 70.0f;
                x += dir * std::abs (jog); y += jog; y = jlimit (gr.getY() + 10.0f, gr.getBottom() - 10.0f, y);
                tr.lineTo (x, y);
                const float run2 = jmax (10.0f, std::abs (edgeX - x) * (0.3f + 0.7f * r2.nextFloat()));
                x += dir * run2; tr.lineTo (x, y);
                g.setColour (lit);
                g.strokePath (tr, PathStrokeType (1.2f), AffineTransform::translation (0.0f, 1.0f));
                g.setColour (dark);
                g.strokePath (tr, PathStrokeType (1.2f));
                g.setColour (dark.withMultipliedAlpha (1.4f));
                g.fillEllipse (Rectangle<float> (5.5f, 5.5f).withCentre ({ x, y }));
                if (r2.nextFloat() < 0.5f)
                    g.fillEllipse (Rectangle<float> (3.5f, 3.5f).withCentre ({ x + dir * (14.0f + r2.nextFloat() * 30.0f), y + (r2.nextFloat() - 0.5f) * 30.0f }));
            }
        }

        // ---- the emblem: stepped rings around a glowing core
        auto shade = [&] (float rad, float brightTop, float darkBottom)
        {
            const auto top = light ? Colours::white.withAlpha (brightTop) : Colours::white.withAlpha (brightTop * 0.25f);
            g.setGradientFill (ColourGradient (theme.grille.interpolatedWith (Colours::white, light ? brightTop * 0.5f : brightTop * 0.12f), ec.x, ec.y - rad,
                                               theme.grille.darker (darkBottom), ec.x, ec.y + rad, false));
            g.fillEllipse (Rectangle<float> (rad * 2.0f, rad * 2.0f).withCentre (ec));
            g.setColour (top);
            g.drawEllipse (Rectangle<float> (rad * 2.0f, rad * 2.0f).withCentre (ec).reduced (0.7f), 1.0f);
            g.setColour (Colours::black.withAlpha (light ? 0.12f : 0.5f));
            g.drawEllipse (Rectangle<float> (rad * 2.0f, rad * 2.0f).withCentre (ec.translated (0.0f, 1.5f)), 1.0f);
        };
        // soft shadow pool
        g.setGradientFill (ColourGradient (Colours::black.withAlpha (light ? 0.18f : 0.45f), ec.x, ec.y + er * 0.1f, Colours::transparentBlack, ec.x + er * 1.15f, ec.y, true));
        g.fillEllipse (Rectangle<float> (er * 2.3f, er * 2.3f).withCentre (ec.translated (0.0f, er * 0.08f)));
        shade (er,         0.75f, 0.10f);
        // segmented outer band
        g.setColour ((light ? Colours::black : Colours::white).withAlpha (light ? 0.10f : 0.08f));
        for (int i = 0; i < 6; ++i)
        {
            Path seg; const float a0 = (float) i / 6.0f * MathConstants<float>::twoPi + 0.2f;
            seg.addCentredArc (ec.x, ec.y, er * 0.90f, er * 0.90f, 0.0f, a0, a0 + 0.55f, true);
            g.strokePath (seg, PathStrokeType (er * 0.07f, PathStrokeType::curved, PathStrokeType::butt));
        }
        shade (er * 0.76f, 0.95f, 0.16f);
        // dotted tick ring
        g.setColour ((light ? Colours::black : Colours::white).withAlpha (light ? 0.35f : 0.3f));
        for (int i = 0; i < 72; ++i)
        {
            const float a = (float) i / 72.0f * MathConstants<float>::twoPi;
            if (std::fmod ((float) i, 18.0f) < 4.0f) continue;   // gaps
            const float rr = er * 0.68f;
            g.fillRect (Rectangle<float> (1.6f, 3.2f).withCentre ({ ec.x + rr * std::sin (a), ec.y - rr * std::cos (a) }));
        }
        shade (er * 0.58f, 1.0f, 0.20f);
        // fine radial grooves around the core
        g.setColour ((light ? Colours::black : Colours::white).withAlpha (0.10f));
        for (int i = 0; i < 120; ++i)
        {
            const float a = (float) i / 120.0f * MathConstants<float>::twoPi;
            g.drawLine (ec.x + er * 0.44f * std::sin (a), ec.y - er * 0.44f * std::cos (a), ec.x + er * 0.53f * std::sin (a), ec.y - er * 0.53f * std::cos (a), 0.6f);
        }
        shade (er * 0.42f, 0.9f, 0.25f);

        // the core: a glass orb lit from inside in the amp's colour
        const float cr = er * 0.33f;
        const float glow = shownActive ? 1.0f : 0.45f;
        g.setGradientFill (ColourGradient (accent.withAlpha (0.55f * glow), ec.x, ec.y, accent.withAlpha (0.0f), ec.x + cr * 1.9f, ec.y, true));
        g.fillEllipse (Rectangle<float> (cr * 3.8f, cr * 3.8f).withCentre (ec));
        ColourGradient orb (accent.brighter (0.9f).withMultipliedBrightness (0.6f + 0.4f * glow), ec.x - cr * 0.15f, ec.y - cr * 0.2f,
                            accent.darker (1.6f), ec.x + cr, ec.y + cr, true);
        orb.addColour (0.55, accent.withMultipliedBrightness (0.55f + 0.45f * glow));
        g.setGradientFill (orb);
        g.fillEllipse (Rectangle<float> (cr * 2.0f, cr * 2.0f).withCentre (ec));
        // filaments
        g.setColour (Colours::white.withAlpha (0.55f * glow));
        for (float a : { -0.55f, 0.6f, 2.9f })
            g.drawLine (ec.x, ec.y, ec.x + cr * 0.8f * std::sin (a), ec.y - cr * 0.8f * std::cos (a), 1.1f);
        g.fillEllipse (Rectangle<float> (4.0f, 4.0f).withCentre (ec));
        // glass reflections
        g.setGradientFill (ColourGradient (Colours::white.withAlpha (0.55f), ec.x - cr * 0.3f, ec.y - cr * 0.85f,
                                           Colours::white.withAlpha (0.0f), ec.x - cr * 0.3f, ec.y - cr * 0.1f, false));
        g.fillEllipse (Rectangle<float> (cr * 1.2f, cr * 0.75f).withCentre (ec.translated (-cr * 0.18f, -cr * 0.52f)));
        g.setColour (Colours::black.withAlpha (0.45f));
        g.drawEllipse (Rectangle<float> (cr * 2.0f, cr * 2.0f).withCentre (ec), 1.2f);
    }
    // top inner shadow of the recess
    g.setGradientFill (ColourGradient (Colours::black.withAlpha (light ? 0.16f : 0.4f), gr.getX(), gr.getY(), Colours::transparentBlack, gr.getX(), gr.getY() + 14.0f, false));
    g.fillRect (gr.withHeight (14.0f));
    g.restoreState();

    // ---- control strip: a separate plate below the face
    const auto pn = panel.toFloat();
    g.setGradientFill (ColourGradient (theme.panelTop, pn.getX(), pn.getY(), theme.panelBottom, pn.getX(), pn.getBottom(), false));
    g.fillRect (pn);
    g.setColour (Colours::black.withAlpha (light ? 0.25f : 0.6f));
    g.drawHorizontalLine ((int) pn.getY(), pn.getX(), pn.getRight());
    g.setColour (Colours::white.withAlpha (light ? 0.9f : 0.08f));
    g.drawHorizontalLine ((int) pn.getY() + 1, pn.getX(), pn.getRight());
    // three-dot marks in the corners
    g.setColour (theme.text.withAlpha (0.5f));
    for (auto p : { pn.getTopRight().translated (-22.0f, 16.0f), pn.getBottomLeft().translated (16.0f, -16.0f) })
        for (auto o : { Point<float> (0.0f, 0.0f), Point<float> (6.0f, 3.0f), Point<float> (2.0f, 7.0f) })
            g.fillEllipse (Rectangle<float> (3.0f, 3.0f).withCentre (p + o));

    const auto ink = theme.text;

    // input jack with a cable running down off the amp
    const auto jack = Point<float> (pn.getX() + 58.0f, pn.getCentreY() + 4.0f);
    const auto cableCol = light ? Colour (0xffe9eaec) : Colour (0xff3a3c42);
    Path cable;
    cable.startNewSubPath (jack.translated (0.0f, 14.0f));
    cable.cubicTo (jack.translated (0.0f, 60.0f), jack.translated (-6.0f, (float) cabTop.getY() - jack.y + 40.0f),
                   jack.translated (-60.0f, (float) cabTop.getY() - jack.y + 70.0f));
    g.setColour (Colours::black.withAlpha (0.35f));
    g.strokePath (cable, PathStrokeType (11.0f, PathStrokeType::curved, PathStrokeType::rounded), AffineTransform::translation (3.0f, 4.0f));
    g.setColour (cableCol.darker (0.25f));
    g.strokePath (cable, PathStrokeType (10.0f, PathStrokeType::curved, PathStrokeType::rounded));
    g.setColour (cableCol);
    g.strokePath (cable, PathStrokeType (6.0f, PathStrokeType::curved, PathStrokeType::rounded), AffineTransform::translation (-1.2f, 0.0f));
    // plug barrel
    auto plug = Rectangle<float> (16.0f, 26.0f).withCentre (jack.translated (0.0f, 4.0f));
    g.setGradientFill (ColourGradient (cableCol.brighter (0.2f), plug.getX(), plug.getY(), cableCol.darker (0.35f), plug.getRight(), plug.getY(), false));
    g.fillRoundedRectangle (plug, 5.0f);
    g.setColour (Colours::black.withAlpha (0.3f));
    g.drawRoundedRectangle (plug, 5.0f, 0.8f);
    g.setGradientFill (ColourGradient (Colour (0xffd5d7db), jack.x - 9.0f, jack.y, Colour (0xff7d8086), jack.x + 9.0f, jack.y, false));
    g.fillEllipse (Rectangle<float> (18.0f, 8.0f).withCentre (jack.translated (0.0f, -9.0f)));
    g.setColour (ink.withAlpha (0.7f));
    g.setFont (spaced (8.5f, false, 0.5f));
    g.drawText ("INPUT", Rectangle<float> (60.0f, 12.0f).withCentre (jack.translated (0.0f, -26.0f)), Justification::centred);

    // power toggle with the pilot light, then the wordmark
    const auto sw = Point<float> (pn.getRight() - 170.0f, pn.getCentreY() + 4.0f);
    g.setColour (Colours::black.withAlpha (0.45f));
    g.fillEllipse (Rectangle<float> (14.0f, 14.0f).withCentre (sw.translated (0.0f, 8.0f)));
    g.setGradientFill (ColourGradient (Colour (0xffe8e9eb), sw.x - 4.0f, sw.y, Colour (0xff6d7076), sw.x + 4.0f, sw.y, false));
    g.fillRoundedRectangle (Rectangle<float> (6.0f, 22.0f).withCentre (sw.translated (0.0f, -4.0f)), 3.0f);
    g.setColour (Colour (0xff1b1c1f));
    g.fillEllipse (Rectangle<float> (12.0f, 12.0f).withCentre (sw.translated (0.0f, 8.0f)));
    for (int i = 0; i < 3; ++i)
    {
        const auto led = Point<float> (sw.x + 18.0f + (float) i * 9.0f, sw.y + 2.0f);
        if (shownActive)
        {
            g.setColour (accent.withAlpha (0.35f));
            g.fillEllipse (Rectangle<float> (9.0f, 9.0f).withCentre (led));
        }
        g.setColour (shownActive ? accent.brighter (0.5f) : ink.withAlpha (0.25f));
        g.fillEllipse (Rectangle<float> (4.0f, 4.0f).withCentre (led));
    }
    g.setColour (ink.withAlpha (0.85f));
    g.setFont (spaced (13.0f, true, 0.5f));
    g.drawText ("EMBER", Rectangle<float> (pn.getRight() - 120.0f, pn.getCentreY() - 14.0f, 100.0f, 18.0f), Justification::centredLeft);
    g.setColour (ink.withAlpha (0.55f));
    g.setFont (spaced (8.5f, false, 0.5f));
    g.drawText (EmberAmpProcessor::ampName (amp).toUpperCase(), Rectangle<float> (pn.getRight() - 120.0f, pn.getCentreY() + 6.0f, 100.0f, 14.0f), Justification::centredLeft);

    // ---- cabinet top below the head
    const auto ct = cabTop.toFloat();
    g.setGradientFill (ColourGradient (Colour (0xffcfd1d5), ct.getX(), ct.getY(), Colour (0xff9fa2a8), ct.getX(), ct.getY() + 18.0f, false));
    g.fillRect (ct.withHeight (18.0f));
    g.setColour (Colours::white.withAlpha (0.6f));
    g.drawHorizontalLine ((int) ct.getY(), ct.getX(), ct.getRight());
    auto body = ct.withTrimmedTop (18.0f);
    g.setGradientFill (ColourGradient (Colour (0xff4a4c51), body.getX(), body.getY(), Colour (0xff2a2b2f), body.getX(), body.getBottom(), false));
    g.fillRect (body);
    g.setGradientFill (ColourGradient (Colours::black.withAlpha (0.45f), body.getX(), body.getY(), Colours::transparentBlack, body.getX(), body.getY() + 10.0f, false));
    g.fillRect (body.withHeight (10.0f));
    g.saveState();
    g.reduceClipRegion (body.toNearestInt());
    for (int i = 0; i < 2; ++i)   // speakers peeking up from below
    {
        const float cx = body.getX() + body.getWidth() * (0.25f + 0.5f * (float) i), rr = body.getWidth() * 0.2f;
        const auto cc = Point<float> (cx, body.getBottom() + rr * 0.78f);
        g.setGradientFill (ColourGradient (Colour (0xff1a1b1e), cc.x, cc.y - rr, Colour (0xff2c2d31), cc.x, cc.y, false));
        g.fillEllipse (Rectangle<float> (rr * 2.0f, rr * 2.0f).withCentre (cc));
        g.setColour (Colours::white.withAlpha (shownCab ? 0.10f : 0.04f));
        g.drawEllipse (Rectangle<float> (rr * 2.0f, rr * 2.0f).withCentre (cc), 1.5f);
    }
    g.restoreState();

}

void AmpPage::resized()
{
    auto r = getLocalBounds();
    cabTop = r.removeFromBottom (110);
    r.removeFromBottom (24);                                   // space for the feet
    head = r.reduced (30, 6);

    auto inner = head.reduced (34, 32);
    panel  = inner.removeFromBottom (juce::jmax (120, juce::roundToInt ((float) inner.getHeight() * 0.36f)));
    grille = inner;

    // knobs across the panel, between the input jack and the pilot light
    auto kr = panel.withTrimmedLeft (100).withTrimmedRight (180).reduced (0, 8);
    const int kw = kr.getWidth() / knobs.size();
    for (auto* k : knobs)
        k->setBounds (kr.removeFromLeft (kw).reduced (4, 0));

    // cab strip: [CAB] cab · mic · position · distance [EDIT CAB] | IR name [LOAD IR] [CLEAR]
    auto strip = cabTop.withTrimmedTop (22).withTrimmedBottom (4).reduced (60, 0).withTrimmedLeft (60);   // clear of the cable
    auto centreRow = [] (juce::Rectangle<int> col, int h) { return col.withSizeKeepingCentre (col.getWidth(), h); };
    cabButton.setBounds (centreRow (strip.removeFromLeft (84), 34));
    strip.removeFromLeft (16);
    clearIRButton.setBounds (centreRow (strip.removeFromRight (80), 28));
    strip.removeFromRight (8);
    loadIRButton.setBounds (centreRow (strip.removeFromRight (96), 28));
    strip.removeFromRight (12);
    irLabel.setBounds (centreRow (strip.removeFromRight (180), 28));
    strip.removeFromRight (12);
    editCabButton.setBounds (centreRow (strip.removeFromRight (100), 28));
    strip.removeFromRight (12);
    cabSummary.setBounds (centreRow (strip, 28));
}

//==============================================================================
PedalUnit::PedalUnit (juce::AudioProcessorValueTreeState& state, const juce::String& n, juce::Colour b,
                      const juce::String& onParam, std::vector<std::pair<juce::String, juce::String>> ks)
    : name (n), body (b)
{
    for (auto& [id, title] : ks)
    {
        auto* k = knobs.add (new Knob (state, id, title, body.brighter (0.6f).withSaturation (0.5f), "clay", Palette::text, 10.0f));
        k->getSlider().getProperties().set ("knobLight", true);
        addAndMakeVisible (k);
    }

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
    auto r = getLocalBounds().toFloat().reduced (2.0f, 1.0f);
    const bool on = footswitch.getToggleState();
    // metallic, softly tinted enclosure (graphite, taupe, slate, warm grey...)
    const auto base = body.withSaturation (jmin (body.getSaturation(), 0.12f)).withBrightness (on ? 0.55f : 0.42f);
    const float corner = r.getWidth() * 0.17f;

    // shadow on the board
    g.setGradientFill (ColourGradient (Colours::black.withAlpha (0.65f), r.getCentreX(), r.getBottom() - 6.0f, Colours::transparentBlack, r.getCentreX(), r.getBottom() + 14.0f, false));
    g.fillRoundedRectangle (r.withY (r.getBottom() - 16.0f).withHeight (30.0f).reduced (4.0f, 0.0f), corner);

    // bevelled rim, then the face
    Path shell; shell.addRoundedRectangle (r, corner);
    g.setGradientFill (ColourGradient (base.brighter (0.55f), r.getX(), r.getY(), base.darker (0.55f), r.getRight(), r.getBottom(), false));
    g.fillPath (shell);
    const auto f = r.reduced (3.5f);
    Path face; face.addRoundedRectangle (f, corner - 3.0f);
    ColourGradient metal (base.brighter (0.12f), f.getX(), f.getY(), base.darker (0.18f), f.getX(), f.getBottom(), false);
    metal.addColour (0.35, base.brighter (0.22f));   // a soft band of sheen
    metal.addColour (0.55, base);
    g.setGradientFill (metal);
    g.fillPath (face);
    fillTextured (g, face, tolexTexture(), 0.35f);       // fine powder-coat grain
    g.setColour (Colours::black.withAlpha (0.55f));
    g.strokePath (shell, PathStrokeType (1.0f));

    // LED, top centre
    const auto led = Rectangle<float> (6.0f, 6.0f).withCentre ({ r.getCentreX(), r.getY() + 13.0f });
    if (on)
    {
        g.setGradientFill (ColourGradient (Colour (0xffff3b30).withAlpha (0.6f), led.getCentreX(), led.getCentreY(), Colour (0x00ff3b30), led.getCentreX() + 9.0f, led.getCentreY(), true));
        g.fillEllipse (led.expanded (7.0f));
    }
    g.setColour (Colours::black.withAlpha (0.6f));
    g.fillEllipse (led.expanded (1.5f));
    g.setColour (on ? Colour (0xffff4a3a) : Colour (0xff3a1715));
    g.fillEllipse (led);

    // name: big, spaced, lightly debossed
    g.setFont (spaced (jmin (15.0f, r.getWidth() * 0.11f), true, 0.18f));
    g.setColour (Colours::black.withAlpha (0.45f));
    g.drawFittedText (name.toUpperCase(), nameArea.translated (0.0f, 1.0f).toNearestInt(), Justification::centred, 1, 0.5f);
    g.setColour (Colours::white.withAlpha (on ? 0.92f : 0.62f));
    g.drawFittedText (name.toUpperCase(), nameArea.toNearestInt(), Justification::centred, 1, 0.5f);
}

void PedalUnit::resized()
{
    auto r = getLocalBounds().reduced (6, 0);
    r.removeFromTop (22);                                    // LED
    const int fs = juce::jmin (54, r.getWidth() / 2);
    footswitch.setBounds (r.removeFromBottom (fs + 14).withSizeKeepingCentre (fs, fs));
    nameArea = r.removeFromBottom (24).toFloat();
    r.removeFromBottom (2);
    // knobs: one centred; two side by side; three = two on top and one below
    const int n = knobs.size();
    const int rowH = n == 3 ? r.getHeight() / 2 : juce::jmin (r.getHeight(), 92);
    if (n == 1)
        knobs[0]->setBounds (r.removeFromTop (rowH).withSizeKeepingCentre (juce::jmin (r.getWidth(), 86), rowH));
    else if (n >= 2)
    {
        auto top = r.removeFromTop (rowH);
        knobs[0]->setBounds (top.removeFromLeft (top.getWidth() / 2));
        knobs[1]->setBounds (top);
        if (n >= 3)
            knobs[2]->setBounds (r.removeFromTop (rowH).withSizeKeepingCentre (r.getWidth() / 2 + 6, rowH));
    }
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
    // one pedalboard per row, with a cable line showing the signal path
    for (auto [row, label] : { std::pair<Rectangle<int>, const char*> { row1, "BEFORE THE AMP" }, { row2, "AFTER THE AMP" } })
    {
        const auto rr = row.toFloat();
        g.setColour (Palette::muted);
        g.setFont (spaced (10.5f, true, 0.3f));
        g.drawText (label, Rectangle<float> (rr.getX() + 6.0f, rr.getY() - 20.0f, 300.0f, 18.0f), Justification::centredLeft);
        auto board = rr.withTrimmedTop (rr.getHeight() * 0.62f).expanded (0.0f, 6.0f);
        Path b; b.addRoundedRectangle (board, 8.0f);
        g.setGradientFill (ColourGradient (Colour (0xff1b1c20), board.getX(), board.getY(), Colour (0xff0b0b0d), board.getX(), board.getBottom(), false));
        g.fillPath (b);
        fillTextured (g, b, tolexTexture(), 0.7f);
        g.setColour (Colours::white.withAlpha (0.06f));
        g.drawHorizontalLine ((int) board.getY() + 1, board.getX() + 8.0f, board.getRight() - 8.0f);
        g.setColour (Colours::black);
        g.strokePath (b, PathStrokeType (1.0f));
        // patch cable through the row
        g.setColour (Colour (0xff050506));
        g.fillRoundedRectangle (rr.getX() + 10.0f, rr.getCentreY() - 2.0f, rr.getWidth() - 20.0f, 4.0f, 2.0f);
    }

    const auto a = ampSlot.toFloat();
    const auto accent = EmberAmpEditor::ampAccent (proc.getSelectedAmp());
    g.setColour (Colour (0xff0d0e10));
    g.fillRoundedRectangle (a, 10.0f);
    g.setColour (accent.withAlpha (0.7f));
    g.drawRoundedRectangle (a.reduced (0.5f), 10.0f, 1.2f);
    g.setColour (Palette::muted);
    g.setFont (spaced (10.0f, true, 0.3f));
    g.drawText ("AMP", a.withHeight (a.getHeight() * 0.5f - 6.0f), Justification::centredBottom);
    g.setColour (Colours::white);
    g.setFont (spaced (13.0f, true, 0.15f));
    g.drawFittedText (EmberAmpProcessor::ampName (proc.getSelectedAmp()).toUpperCase(), a.withTrimmedTop (a.getHeight() * 0.5f).toNearestInt().reduced (6, 0),
                      Justification::centredTop, 1);
}

void PedalsPage::resized()
{
    // two rows: 5 pedals before the amp, then the amp and 3 pedals after it.
    // Pedals keep a compact stompbox shape (about 1.6 times as tall as wide).
    auto r = getLocalBounds().reduced (24, 4);
    const int rowGap = 26;
    const int rowH = (r.getHeight() - rowGap - 20) / 2;
    r.removeFromTop (20);
    row1 = r.removeFromTop (rowH);
    r.removeFromTop (rowGap);
    row2 = r.removeFromTop (rowH);

    const int pedalH = rowH - 6;
    const int pedalW = juce::jmin (juce::roundToInt ((float) pedalH / 1.6f), (row1.getWidth() - 4 * 26) / 5);
    const int gap = 26;
    auto place = [&] (juce::Rectangle<int> row, std::initializer_list<juce::Component*> items, int extraLeft)
    {
        const int n = (int) items.size();
        const int total = n * pedalW + (n - 1) * gap + extraLeft;
        auto x = row.getCentreX() - total / 2 + extraLeft;
        for (auto* c : items)
        {
            c->setBounds (x, row.getY(), pedalW, pedalH);
            x += pedalW + gap;
        }
        return row.getCentreX() - total / 2;
    };
    place (row1, { &pitchPedal, &gatePedal, &compPedal, &odPedal, &dsPedal }, 0);   // signal order
    const int slotW = 120;
    const int left = place (row2, { &chorusPedal, &delayPedal, &reverbPedal }, slotW + gap);
    ampSlot = juce::Rectangle<int> (left, row2.getY(), slotW, pedalH).withSizeKeepingCentre (slotW, 120);
}


//==============================================================================
CabPage::CabPage (EmberAmpEditor& e, EmberAmpProcessor& p) : ed (e), proc (p)
{
    title.setFont (spaced (13.0f, true, 0.3f));
    addAndMakeVisible (title);
    for (auto* l : { &cabTitle, &micTitle, &irTitle })
    {
        l->setFont (spaced (10.5f, true, 0.3f));
        l->setColour (juce::Label::textColourId, Palette::muted);
        addAndMakeVisible (l);
    }
    cabTitle.setText ("CABINET", juce::dontSendNotification);
    micTitle.setText ("MICROPHONE", juce::dontSendNotification);
    irTitle.setText ("IMPULSE RESPONSE", juce::dontSendNotification);
    hint.setFont (juce::FontOptions (12.0f));
    hint.setColour (juce::Label::textColourId, Palette::muted);
    hint.setJustificationType (juce::Justification::centred);
    hint.setText ("Click a speaker to mic it  -  drag or scroll sideways to move the mic across the cone  -  scroll up / down for closer or further", juce::dontSendNotification);
    hint.setInterceptsMouseClicks (false, false);
    addAndMakeVisible (hint);

    const auto cabs = EmberAmpProcessor::cabNames();
    for (int i = 0; i < cabs.size(); ++i)
    {
        auto* b = cabButtons.add (new juce::TextButton (cabs[i]));
        b->getProperties().set ("choice", true);
        b->onClick = [this, i] { setParam ("cabModel", (float) i); };
        addAndMakeVisible (b);
    }
    const auto mics = EmberAmpProcessor::micNames();
    for (int i = 0; i < mics.size(); ++i)
    {
        auto* b = micButtons.add (new juce::TextButton (mics[i]));
        b->getProperties().set ("choice", true);
        b->onClick = [this, i] { setParam ("mic", (float) i); };
        addAndMakeVisible (b);
    }

    irLabel.setFont (spaced (12.0f, false, 0.05f));
    irLabel.setMinimumHorizontalScale (0.7f);
    addAndMakeVisible (irLabel);
    addAndMakeVisible (cabOn);
    for (auto* b : { &loadIRButton, &clearIRButton }) addAndMakeVisible (b);
    loadIRButton.onClick  = [this] { if (amp >= 0) ed.ampPages[amp]->chooseIR(); };
    clearIRButton.onClick = [this] { if (amp >= 0) { proc.clearIR (amp); ed.ampPages[amp]->refresh(); refresh(); } };

    bindToAmp (proc.getSelectedAmp());
    startTimerHz (60);
}

void CabPage::timerCallback()
{
    if (! isShowing() || amp < 0) return;
    // follow the parameters every frame (not just on the slower page refresh), then ease toward them
    refresh();
    const float tPos = juce::jlimit (0.0f, 1.0f, value ("micPos")), tDist = juce::jlimit (0.0f, 1.0f, value ("micDist"));
    const auto tCentre = speakerCentre();
    if (animPos < 0.0f || dragging) { animPos = tPos; }
    if (animDist < 0.0f) { animDist = tDist; animCentre = tCentre; }
    const float k = 0.28f;   // ease: a fraction of the way there each frame (about 90 ms to settle)
    const float dp = tPos - animPos, dd = tDist - animDist;
    const auto dc = tCentre - animCentre;
    if (std::abs (dp) < 0.0005f && std::abs (dd) < 0.0005f && dc.getDistanceFromOrigin() < 0.2f)
    {
        if (animPos != tPos || animDist != tDist || animCentre != tCentre)
        {
            animPos = tPos; animDist = tDist; animCentre = tCentre;
            repaint (cabView.expanded (30));
        }
        return;
    }
    animPos += dp * k; animDist += dd * k; animCentre += dc * k;
    repaint (cabView.expanded (30));
}

juce::RangedAudioParameter* CabPage::param (const char* id) const
{
    return proc.apvts.getParameter (EmberAmpProcessor::ampParamID (amp, id));
}

float CabPage::value (const char* id) const
{
    return proc.apvts.getRawParameterValue (EmberAmpProcessor::ampParamID (amp, id))->load();
}

void CabPage::setParam (const char* id, float v)
{
    if (auto* prm = param (id))
    {
        prm->beginChangeGesture();
        prm->setValueNotifyingHost (prm->convertTo0to1 (v));
        prm->endChangeGesture();
    }
    refresh();
}

// the page always edits the amp you hear
void CabPage::bindToAmp (int a)
{
    amp = juce::jlimit (0, (int) EmberAmpProcessor::numAmps - 1, a);
    const auto accent = EmberAmpEditor::ampAccent (amp);
    posKnob  = std::make_unique<Knob> (proc.apvts, EmberAmpProcessor::ampParamID (amp, "micPos"),  "Position", accent, "ring", Palette::text, 12.0f);
    distKnob = std::make_unique<Knob> (proc.apvts, EmberAmpProcessor::ampParamID (amp, "micDist"), "Distance", accent, "ring", Palette::text, 12.0f);
    addAndMakeVisible (*posKnob);
    addAndMakeVisible (*distKnob);
    cabOnAtt.reset();
    cabOnAtt = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (proc.apvts, EmberAmpProcessor::ampParamID (amp, "cab"), cabOn);
    setAccent (cabOn, accent);
    for (auto* b : cabButtons) setAccent (*b, accent);
    for (auto* b : micButtons) setAccent (*b, accent);
    title.setText ("CAB  -  " + EmberAmpProcessor::ampName (amp).toUpperCase() + " AMP", juce::dontSendNotification);
    shownCab = shownMic = shownPos = shownDist = shownSpk = -1.0f;
    animPos = animDist = -1.0f;
    resized();
    refresh();
}

void CabPage::refresh()
{
    if (proc.getSelectedAmp() != amp)
        bindToAmp (proc.getSelectedAmp());

    const bool ir = proc.hasIR (amp);
    irLabel.setText (ir ? proc.getIRFile (amp).getFileNameWithoutExtension() : juce::String ("None - using the cabinet and mic below"),
                     juce::dontSendNotification);
    irLabel.setColour (juce::Label::textColourId, ir ? EmberAmpEditor::ampAccent (amp) : Palette::muted);
    clearIRButton.setEnabled (ir);
    for (auto* b : cabButtons) b->setEnabled (! ir);
    for (auto* b : micButtons) b->setEnabled (! ir);
    posKnob->setEnabled (! ir);
    distKnob->setEnabled (! ir);

    const float cab = value ("cabModel"), mic = value ("mic"), pos = value ("micPos"), dist = value ("micDist"), spk = value ("micSpeaker");
    const bool on = cabOn.getToggleState();
    for (int i = 0; i < cabButtons.size(); ++i) cabButtons[i]->setToggleState (i == juce::roundToInt (cab), juce::dontSendNotification);
    for (int i = 0; i < micButtons.size(); ++i) micButtons[i]->setToggleState (i == juce::roundToInt (mic), juce::dontSendNotification);
    if (cab != shownCab || mic != shownMic || pos != shownPos || dist != shownDist || ir != shownIR || on != shownOn || spk != shownSpk)
    {
        shownCab = cab; shownMic = mic; shownPos = pos; shownDist = dist; shownIR = ir; shownOn = on; shownSpk = spk;
        repaint (cabView.expanded (30));
    }
}

int CabPage::speakerCount() const
{
    return EmberAmpProcessor::speakersInCab (juce::roundToInt (shownCab));
}

int CabPage::mikedSpeaker() const
{
    return juce::jlimit (0, speakerCount() - 1, juce::roundToInt (shownSpk) - 1);
}

// speakers: 1x12 = one in the middle; 2x12 = left, right; 4x12 = top-left, top-right, bottom-left, bottom-right
juce::Point<float> CabPage::speakerCentreAt (int i) const
{
    const auto v = cabView.toFloat().reduced (40.0f, 36.0f);
    const int n = speakerCount();
    if (n == 1) return v.getCentre();
    if (n == 2) return { v.getX() + v.getWidth() * (i == 0 ? 0.27f : 0.73f), v.getCentreY() };
    return { v.getX() + v.getWidth() * (i % 2 == 0 ? 0.27f : 0.73f), v.getY() + v.getHeight() * (i < 2 ? 0.27f : 0.73f) };
}

juce::Point<float> CabPage::speakerCentre() const
{
    return speakerCentreAt (mikedSpeaker());
}

float CabPage::speakerRadius() const
{
    const auto v = cabView.toFloat().reduced (40.0f, 36.0f);
    const int n = speakerCount();
    if (n == 1) return juce::jmin (v.getWidth(), v.getHeight()) * 0.40f;
    if (n == 2) return juce::jmin (v.getWidth() * 0.22f, v.getHeight() * 0.40f);
    return juce::jmin (v.getWidth() * 0.21f, v.getHeight() * 0.22f);
}

void CabPage::paint (juce::Graphics& g)
{
    using namespace juce;
    const auto theme = AmpPage::themeFor (amp);
    const auto accent = EmberAmpEditor::ampAccent (amp);
    const bool light = theme.lightPanel;
    const float alpha = shownIR || ! shownOn ? 0.45f : 1.0f;

    // ---- side panel
    {
        const auto sp = side.toFloat();
        g.setColour (Colour (0xff0d0e12).withAlpha (0.85f));
        g.fillRoundedRectangle (sp, 10.0f);
        g.setColour (Colours::white.withAlpha (0.07f));
        g.drawRoundedRectangle (sp.reduced (0.5f), 10.0f, 1.0f);
    }

    // ---- the cabinet, front on: sculpted frame like the amp, speakers behind open baffle
    const auto v = cabView.toFloat();
    g.setColour (Colours::black.withAlpha (0.5f));
    g.fillRoundedRectangle (v.translated (0.0f, 8.0f), 28.0f);
    Path frame; frame.addRoundedRectangle (v, 28.0f);
    g.setGradientFill (ColourGradient (theme.chassisTop, v.getX(), v.getY(), theme.chassisBottom, v.getX(), v.getBottom(), false));
    g.fillPath (frame);
    g.setColour (Colours::white.withAlpha (light ? 0.6f : 0.14f));
    g.drawRoundedRectangle (v.reduced (1.0f), 27.0f, 1.5f);

    auto baffle = v.reduced (30.0f, 28.0f);
    g.setColour (Colours::black.withAlpha (light ? 0.25f : 0.6f));
    g.fillRoundedRectangle (baffle.expanded (2.0f), 8.0f);
    g.setGradientFill (ColourGradient (Colour (0xff1e1f23), baffle.getX(), baffle.getY(), Colour (0xff111215), baffle.getX(), baffle.getBottom(), false));
    g.fillRoundedRectangle (baffle, 8.0f);
    {
        Path b; b.addRoundedRectangle (baffle, 8.0f);
        fillTextured (g, b, tolexTexture(), 0.8f);
    }

    const int n = speakerCount();
    const float r = speakerRadius();
    const auto v2 = cabView.toFloat().reduced (40.0f, 36.0f);
    juce::ignoreUnused (v2);
    Array<Point<float>> centres;
    for (int i = 0; i < n; ++i) centres.add (speakerCentreAt (i));

    for (int i = 0; i < centres.size(); ++i)
    {
        const auto c = centres[i];
        const bool miked = i == mikedSpeaker();
        // basket rim, cone, surround, dust cap
        g.setColour (Colours::black.withAlpha (0.6f));
        g.fillEllipse (Rectangle<float> (r * 2.12f, r * 2.12f).withCentre (c.translated (0.0f, 3.0f)));
        g.setGradientFill (ColourGradient (Colour (0xff5b5e65).withMultipliedAlpha (alpha), c.x, c.y - r, Colour (0xff1c1d21).withMultipliedAlpha (alpha), c.x, c.y + r, false));
        g.fillEllipse (Rectangle<float> (r * 2.08f, r * 2.08f).withCentre (c));
        g.setGradientFill (ColourGradient (Colour (0xff2c2d32), c.x - r * 0.3f, c.y - r, Colour (0xff0b0b0d), c.x + r * 0.3f, c.y + r, false));
        g.fillEllipse (Rectangle<float> (r * 1.92f, r * 1.92f).withCentre (c));
        for (int k = 0; k < 9; ++k)   // concentric ridges in the cone
        {
            const float f = 0.40f + 0.055f * (float) k;
            g.setColour (Colours::white.withAlpha (0.045f));
            g.drawEllipse (Rectangle<float> (r * 2.0f * f, r * 2.0f * f).withCentre (c.translated (0.0f, -0.8f)), 1.0f);
            g.setColour (Colours::black.withAlpha (0.35f));
            g.drawEllipse (Rectangle<float> (r * 2.0f * f, r * 2.0f * f).withCentre (c.translated (0.0f, 0.8f)), 1.0f);
        }
        for (int k = 0; k < 4; ++k)   // rim screws
        {
            const float a = MathConstants<float>::halfPi * (float) k;
            const auto sp = c + Point<float> (std::sin (a), -std::cos (a)) * (r * 0.985f);
            g.setColour (Colour (0xff8d9097).withMultipliedAlpha (alpha));
            g.fillEllipse (Rectangle<float> (4.0f, 4.0f).withCentre (sp));
        }
        g.setColour (Colours::white.withAlpha (0.10f));
        g.drawEllipse (Rectangle<float> (r * 1.80f, r * 1.80f).withCentre (c), 2.0f);
        g.setGradientFill (ColourGradient (Colour (0xff55585f), c.x - r * 0.1f, c.y - r * 0.32f, Colour (0xff141518), c.x + r * 0.1f, c.y + r * 0.32f, false));
        g.fillEllipse (Rectangle<float> (r * 0.64f, r * 0.64f).withCentre (c));
        g.setColour (Colours::white.withAlpha (0.18f));
        g.drawEllipse (Rectangle<float> (r * 0.64f, r * 0.64f).withCentre (c), 1.0f);
        if (miked)
        {
            g.setColour (accent.withAlpha (0.55f * alpha));
            g.drawEllipse (Rectangle<float> (r * 2.2f, r * 2.2f).withCentre (c), 1.5f);
        }
        // speaker number (click a speaker to mic it)
        g.setColour (Colours::white.withAlpha (miked ? 0.8f : 0.35f));
        g.setFont (spaced (10.0f, true, 0.2f));
        g.drawText (String (i + 1), Rectangle<float> (24.0f, 14.0f).withCentre (c.translated (-r * 0.78f, -r * 0.82f)), Justification::centred);
    }

    // ---- the mic, seen from the front: further from the centre = darker tone; nearer = bigger
    const auto c = animDist < 0.0f ? speakerCentre() : animCentre;
    const float pos = jlimit (0.0f, 1.0f, animPos < 0.0f ? shownPos : animPos), dist = jlimit (0.0f, 1.0f, animDist < 0.0f ? shownDist : animDist);
    const auto mp = c + Point<float> (std::cos (micAngle), std::sin (micAngle)) * (pos * r * 0.95f);
    const int micType = roundToInt (shownMic);

    // ---- the microphone, seen from the front as it points into the speaker (like looking past it at the cone)
    {
        const float u = r * (0.085f - 0.025f * dist);   // closer = bigger
        // soft shadow on the cone: further back = more offset, softer
        const float off = 3.0f + 10.0f * dist;
        g.setGradientFill (ColourGradient (Colours::black.withAlpha ((0.6f - 0.2f * dist) * alpha), mp.x + off, mp.y + off,
                                           Colours::transparentBlack, mp.x + off + u * 2.2f, mp.y + off, true));
        g.fillEllipse (Rectangle<float> (u * 4.4f, u * 4.4f).withCentre (mp.translated (off, off)));

        auto mesh = [&] (const Path& area, float step, Colour c)
        {
            g.saveState();
            g.reduceClipRegion (area);
            g.setColour (c);
            const auto b = area.getBounds();
            for (float d = -b.getHeight(); d < b.getWidth() + b.getHeight(); d += step)
            {
                g.drawLine (b.getX() + d, b.getY(), b.getX() + d - b.getHeight(), b.getBottom(), 0.6f);
                g.drawLine (b.getX() + d - b.getHeight(), b.getY(), b.getX() + d, b.getBottom(), 0.6f);
            }
            g.restoreState();
        };

        Rectangle<float> outline;
        switch (micType)
        {
            case 1:   // ribbon: a slim upright bar, dark, with a red stripe
            {
                auto body = Rectangle<float> (u * 1.25f, u * 3.6f).withCentre (mp);
                outline = body;
                g.setGradientFill (ColourGradient (Colour (0xff4a4c52).withMultipliedAlpha (alpha), body.getX(), body.getY(), Colour (0xff0d0e10).withMultipliedAlpha (alpha), body.getRight(), body.getY(), false));
                g.fillRoundedRectangle (body, u * 0.35f);
                Path grille; grille.addRoundedRectangle (body.reduced (u * 0.22f, u * 0.3f).withTrimmedBottom (u * 0.9f), u * 0.2f);
                g.setColour (Colour (0xff1a1b1e).withMultipliedAlpha (alpha));
                g.fillPath (grille);
                mesh (grille, jmax (2.0f, u * 0.22f), Colours::white.withAlpha (0.22f * alpha));
                g.setColour (Colour (0xffd8322a).withMultipliedAlpha (alpha));
                g.fillRect (Rectangle<float> (u * 0.16f, u * 0.75f).withCentre ({ body.getCentreX(), body.getBottom() - u * 0.55f }));
                g.setColour (Colours::white.withAlpha (0.35f * alpha));
                g.drawRoundedRectangle (body, u * 0.35f, 0.8f);
                break;
            }
            case 2:   // condenser: a big silver disc with a fine mesh
            {
                const float rr = u * 1.55f;
                outline = Rectangle<float> (rr * 2.0f, rr * 2.0f).withCentre (mp);
                g.setGradientFill (ColourGradient (Colour (0xfff2f3f5).withMultipliedAlpha (alpha), mp.x - rr * 0.6f, mp.y - rr, Colour (0xff7b7e85).withMultipliedAlpha (alpha), mp.x + rr * 0.6f, mp.y + rr, false));
                g.fillEllipse (outline);
                Path grille; grille.addEllipse (outline.reduced (rr * 0.18f));
                g.setGradientFill (ColourGradient (Colour (0xffcdd0d5).withMultipliedAlpha (alpha), mp.x, mp.y - rr, Colour (0xff5d6067).withMultipliedAlpha (alpha), mp.x, mp.y + rr, false));
                g.fillPath (grille);
                mesh (grille, jmax (2.0f, u * 0.2f), Colours::black.withAlpha (0.28f * alpha));
                g.setColour (Colours::black.withAlpha (0.4f * alpha));
                g.drawEllipse (outline, 1.0f);
                break;
            }
            case 3:   // large dynamic: black, with long slots across the front
            {
                const float rr = u * 1.3f;
                outline = Rectangle<float> (rr * 2.0f, rr * 2.0f).withCentre (mp);
                g.setGradientFill (ColourGradient (Colour (0xff3c3e44).withMultipliedAlpha (alpha), mp.x - rr * 0.5f, mp.y - rr, Colour (0xff08090a).withMultipliedAlpha (alpha), mp.x + rr * 0.5f, mp.y + rr, false));
                g.fillEllipse (outline);
                g.setColour (Colours::black.withAlpha (0.8f * alpha));
                for (int k = -2; k <= 2; ++k)
                    g.fillRoundedRectangle (Rectangle<float> (rr * 1.2f, rr * 0.12f).withCentre (mp.translated (0.0f, (float) k * rr * 0.28f)), rr * 0.06f);
                g.setColour (Colours::white.withAlpha (0.25f * alpha));
                g.drawEllipse (outline.reduced (0.5f), 1.0f);
                break;
            }
            default:  // dynamic: small round head, silver ring around a dark mesh
            {
                const float rr = u * 1.0f;
                outline = Rectangle<float> (rr * 2.0f, rr * 2.0f).withCentre (mp);
                g.setGradientFill (ColourGradient (Colour (0xffeef0f2).withMultipliedAlpha (alpha), mp.x - rr * 0.7f, mp.y - rr, Colour (0xff6d7077).withMultipliedAlpha (alpha), mp.x + rr * 0.7f, mp.y + rr, false));
                g.fillEllipse (outline);
                Path grille; grille.addEllipse (outline.reduced (rr * 0.28f));
                g.setColour (Colour (0xff16171a).withMultipliedAlpha (alpha));
                g.fillPath (grille);
                mesh (grille, jmax (1.8f, u * 0.2f), Colours::white.withAlpha (0.25f * alpha));
                g.setColour (Colours::white.withAlpha (0.6f * alpha));
                g.fillEllipse (Rectangle<float> (rr * 0.35f, rr * 0.22f).withCentre (mp.translated (-rr * 0.45f, -rr * 0.6f)));
                break;
            }
        }
        if (dragging)
        {
            g.setColour (accent.withAlpha (0.9f * alpha));
            g.drawEllipse (Rectangle<float> (jmax (outline.getWidth(), outline.getHeight()) + 10.0f, jmax (outline.getWidth(), outline.getHeight()) + 10.0f).withCentre (mp), 1.5f);
        }
    }

    // readout under the cab
    g.setColour (Palette::text.withAlpha (0.85f));
    g.setFont (spaced (12.0f, true, 0.25f));
    const auto pText = proc.apvts.getParameter (EmberAmpProcessor::ampParamID (amp, "micPos"))->getCurrentValueAsText();
    const auto dText = proc.apvts.getParameter (EmberAmpProcessor::ampParamID (amp, "micDist"))->getCurrentValueAsText();
    g.drawText ((EmberAmpProcessor::cabNames()[roundToInt (shownCab)] + "   /   " + EmberAmpProcessor::micNames()[micType]
                 + "   /   SPEAKER " + String (mikedSpeaker() + 1) + "   /   " + pText + " out   /   " + dText).toUpperCase(),
                Rectangle<float> (v.getX(), v.getBottom() + 16.0f, v.getWidth(), 20.0f), Justification::centred);
    if (shownIR || ! shownOn)
    {
        g.setColour (Colours::black.withAlpha (0.55f));
        g.fillRoundedRectangle (Rectangle<float> (360.0f, 40.0f).withCentre (v.getCentre()), 8.0f);
        g.setColour (Colours::white);
        g.setFont (spaced (13.0f, true, 0.2f));
        g.drawText (! shownOn ? "CAB OFF" : "USING YOUR IR INSTEAD", Rectangle<float> (360.0f, 40.0f).withCentre (v.getCentre()), Justification::centred);
    }
}

void CabPage::resized()
{
    auto r = getLocalBounds().reduced (24, 12);
    side = r.removeFromRight (340);
    r.removeFromRight (24);
    title.setBounds (r.removeFromTop (24));
    hint.setBounds (r.removeFromBottom (22));
    r.removeFromBottom (36);
    cabView = r.reduced (10, 10);

    auto s = side.reduced (20, 16);
    cabOn.setBounds (s.removeFromTop (26).removeFromLeft (90));
    s.removeFromTop (6);
    cabTitle.setBounds (s.removeFromTop (18));
    for (auto* b : cabButtons) { b->setBounds (s.removeFromTop (26)); s.removeFromTop (3); }
    s.removeFromTop (8);
    micTitle.setBounds (s.removeFromTop (18));
    {
        auto row1 = s.removeFromTop (26); s.removeFromTop (3);
        auto row2 = s.removeFromTop (26);
        for (int i = 0; i < micButtons.size(); ++i)
        {
            auto& row = i < 2 ? row1 : row2;
            micButtons[i]->setBounds (i % 2 == 0 ? row.removeFromLeft (row.getWidth() / 2 - 2) : row.withTrimmedLeft (2));
        }
    }
    s.removeFromTop (8);
    auto knobs = s.removeFromTop (104);
    if (posKnob != nullptr)  posKnob->setBounds (knobs.removeFromLeft (knobs.getWidth() / 2).reduced (10, 0));
    if (distKnob != nullptr) distKnob->setBounds (knobs.reduced (10, 0));
    s.removeFromTop (6);
    irTitle.setBounds (s.removeFromTop (18));
    irLabel.setBounds (s.removeFromTop (24));
    s.removeFromTop (6);
    auto irRow = s.removeFromTop (30);
    loadIRButton.setBounds (irRow.removeFromLeft (irRow.getWidth() / 2 - 4));
    irRow.removeFromLeft (8);
    clearIRButton.setBounds (irRow);
}

// put the mic at a point: on the miked speaker (stopping at its edge), or - only when you click
// on another speaker - on that one
void CabPage::placeMicAt (juce::Point<float> p, bool mayChangeSpeaker)
{
    int best = mikedSpeaker();
    // switch only for a click clearly inside another speaker's cone (not where two rims meet)
    if (mayChangeSpeaker && p.getDistanceFrom (speakerCentreAt (best)) > speakerRadius())
        for (int i = 0; i < speakerCount(); ++i)
            if (i != best && p.getDistanceFrom (speakerCentreAt (i)) < speakerRadius() * 0.9f)
                best = i;
    const auto c = speakerCentreAt (best);
    const auto d = p - c;
    if (d.getDistanceFromOrigin() > 2.0f) micAngle = std::atan2 (d.y, d.x);
    if (best != mikedSpeaker())
        if (auto* prm = param ("micSpeaker"))
            prm->setValueNotifyingHost (prm->convertTo0to1 ((float) (best + 1)));
    if (auto* prm = param ("micPos"))
        prm->setValueNotifyingHost (prm->convertTo0to1 (juce::jlimit (0.0f, 1.0f, d.getDistanceFromOrigin() / (speakerRadius() * 0.95f))));
    refresh();
    repaint (cabView.expanded (30));   // at the edge only the angle changes (not a setting), so redraw here
}

juce::Point<float> CabPage::micPoint() const
{
    return speakerCentre() + juce::Point<float> (std::cos (micAngle), std::sin (micAngle)) * (juce::jlimit (0.0f, 1.0f, value ("micPos")) * speakerRadius() * 0.95f);
}

void CabPage::mouseDown (const juce::MouseEvent& e)
{
    if (shownIR || ! shownOn) return;
    bool onASpeaker = false;
    for (int i = 0; i < speakerCount(); ++i)
        onASpeaker = onASpeaker || e.position.getDistanceFrom (speakerCentreAt (i)) < speakerRadius() * 1.1f;
    if (! onASpeaker) return;
    dragging = true;
    for (auto* id : { "micPos", "micSpeaker" })
        if (auto* prm = param (id)) prm->beginChangeGesture();
    // grabbing the mic itself only ever moves the mic; elsewhere, a click can pick another speaker
    const bool onMic = e.position.getDistanceFrom (micPoint()) < speakerRadius() * 0.28f;
    placeMicAt (e.position, ! onMic);
}

void CabPage::mouseDrag (const juce::MouseEvent& e)
{
    if (! dragging) return;
    placeMicAt (e.position, false);   // dragging stays on the miked speaker
}

void CabPage::mouseUp (const juce::MouseEvent&)
{
    if (! dragging) return;
    dragging = false;
    for (auto* id : { "micPos", "micSpeaker" })
        if (auto* prm = param (id)) prm->endChangeGesture();
    repaint (cabView.expanded (30));
}

void CabPage::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    if (shownIR || ! cabView.contains (e.getPosition())) return;
    const float speed = w.isSmooth ? 0.9f : 0.45f;   // trackpads: many tiny steps; wheels: a few big ones
    // sideways (trackpad swipe, or Shift + wheel): slide the mic left / right across the whole cab
    const float sideways = e.mods.isShiftDown() ? w.deltaY : w.deltaX;
    if (std::abs (sideways) > std::abs (e.mods.isShiftDown() ? 0.0f : w.deltaY))
    {
        // slide along the line through the centre of the cone the mic is on: in toward the centre,
        // through it and out the other side (so it can never get stuck against the rim)
        juce::Point<float> dir (std::cos (micAngle), std::sin (micAngle));
        float t = juce::jlimit (0.0f, 1.0f, value ("micPos"));
        if (dir.x < -0.001f || (std::abs (dir.x) <= 0.001f && dir.y < 0.0f)) { dir = -dir; t = -t; }   // dir points right (or down)
        t = juce::jlimit (-1.0f, 1.0f, t - sideways * speed * 1.6f);
        micAngle = t >= 0.0f ? std::atan2 (dir.y, dir.x) : std::atan2 (-dir.y, -dir.x);
        if (auto* prm = param ("micPos"))
        {
            prm->beginChangeGesture();
            prm->setValueNotifyingHost (prm->convertTo0to1 (std::abs (t)));
            prm->endChangeGesture();
        }
        refresh();
        repaint (cabView.expanded (30));
        return;
    }
    // up / down: closer or further (the drawn mic glides there, see timerCallback)
    const float step = speed * w.deltaY * (w.isReversed ? -1.0f : 1.0f);
    if (auto* prm = param ("micDist"))
    {
        prm->beginChangeGesture();
        prm->setValueNotifyingHost (juce::jlimit (0.0f, 1.0f, value ("micDist") - step));
        prm->endChangeGesture();
    }
}

//==============================================================================
EqPage::EqPage (EmberAmpProcessor& p) : proc (p)
{
    const char* topIds[]   = { "eqLowCut", "eqFreq1", "eqFreq2", "eqFreq3", "eqFreq4", "eqHighCut" };
    const char* topNames[] = { "Low Cut", "Low", "Low Mid", "High Mid", "High", "High Cut" };
    for (int i = 0; i < 6; ++i)
        addAndMakeVisible (topKnobs.add (new Knob (p.apvts, topIds[i], topNames[i], Palette::accent, "clay", Palette::text, 9.5f)));
    for (int b = 0; b < 4; ++b)
        addAndMakeVisible (gainKnobs.add (new Knob (p.apvts, "eqGain" + juce::String (b + 1), "Gain", Palette::accent, "clay", Palette::text, 9.5f)));
    power.getProperties().set ("vswitch", true);
    power.getProperties().set ("topText", "ON");
    power.getProperties().set ("bottomText", "OFF");
    power.getProperties().set ("onIsTop", true);
    power.setTooltip ("Whole EQ on / off: the 4 bands and the low / high cut (after the amp and cab)");
    addAndMakeVisible (power);
    powerAtt = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (p.apvts, "eqOn", power);
    refresh();
}

void EqPage::refresh()
{
    // the unit takes the colour of the amp you hear: white, ivory, black or blue
    const int a = proc.getSelectedAmp();
    if (a != amp)
    {
        amp = a;
        const bool light = a <= EmberAmpProcessor::crunch;
        const auto ink = light ? juce::Colour (0xff2b2d33) : juce::Colour (0xffe3e5ea);
        for (auto* ks : { &topKnobs, &gainKnobs })
            for (auto* k : *ks)
            {
                k->getSlider().getProperties().set ("arc", true);
                k->getSlider().setMouseDragSensitivity (400);   // a long throw: fine, fluid control over a wide range
                k->getSlider().getProperties().set ("bipolar", ks == &gainKnobs);
                k->getSlider().getProperties().set ("fromEnd", k == topKnobs.getLast());   // high cut: fills in from the top end
                setAccent (k->getSlider(), EmberAmpEditor::ampAccent (a));
                k->getSlider().getProperties().set ("lightPanel", light);
                k->getSlider().getProperties().set ("knobLight", ! light ? juce::var (false) : juce::var (true));
                k->getSlider().setColour (juce::Slider::textBoxTextColourId, ink);
                k->getLabel().setColour (juce::Label::textColourId, ink.withAlpha (0.85f));
                k->getSlider().repaint();
            }
        power.getProperties().set ("darkText", light);
        power.repaint();
        repaint();
    }
    // redraw the curve when any EQ setting moves
    float sig = proc.apvts.getRawParameterValue ("eqOn")->load() * 7.0f;
    for (auto* id : { "eqLowCut", "eqHighCut", "eqFreq1", "eqFreq2", "eqFreq3", "eqFreq4", "eqGain1", "eqGain2", "eqGain3", "eqGain4" })
        sig = sig * 1.37f + proc.apvts.getRawParameterValue (id)->load();
    if (sig != lastSig) { lastSig = sig; repaint (curveArea.expanded (4)); }
}

void EqPage::paint (juce::Graphics& g)
{
    using namespace juce;
    const bool on = power.getToggleState();
    const auto accent = EmberAmpEditor::ampAccent (amp);

    // ---- response curve (skipped when only a knob below needs redrawing)
    const auto ca = curveArea.toFloat();
    if (g.clipRegionIntersects (curveArea.expanded (4)))
    {
    g.setColour (Colour (0xff0b0c10).withAlpha (0.8f));
    g.fillRoundedRectangle (ca, 10.0f);
    g.setColour (Colours::white.withAlpha (0.08f));
    g.drawRoundedRectangle (ca.reduced (0.5f), 10.0f, 1.0f);
    auto xFor = [this] (double hz) { return xForHz (hz); };
    auto yFor = [this] (float db) { return yForDb (db); };
    const double maxHz = topHz();
    g.setFont (spaced (9.0f, false, 0.1f));
    for (double hz : { 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0 })
    {
        if (hz > maxHz * 0.95) continue;
        g.setColour (Colours::white.withAlpha (0.06f));
        g.drawVerticalLine ((int) xFor (hz), plot.getY(), plot.getBottom());
        g.setColour (Palette::muted);
        g.drawText (hz >= 1000.0 ? String ((int) (hz / 1000.0)) + "k" : String ((int) hz), Rectangle<float> (40.0f, 12.0f).withCentre ({ xFor (hz), plot.getBottom() + 8.0f }), Justification::centred);
    }
    for (float db : { -18.0f, -12.0f, -6.0f, 0.0f, 6.0f, 12.0f, 18.0f })
    {
        g.setColour (Colours::white.withAlpha (db == 0.0f ? 0.16f : 0.06f));
        g.drawHorizontalLine ((int) yFor (db), plot.getX(), plot.getRight());
        g.setColour (Palette::muted);
        g.drawText ((db > 0 ? "+" : "") + String ((int) db), Rectangle<float> (30.0f, 12.0f).withPosition (ca.getX() + 6.0f, yFor (db) - 6.0f), Justification::centredLeft);
    }
    Path curve, fill;
    double hzs[241]; float dbs[241];
    for (int i = 0; i <= 240; ++i) hzs[i] = 20.0 * std::pow (maxHz / 20.0, (double) i / 240.0);
    proc.getEqResponseCurve (hzs, dbs, 241);
    for (int i = 0; i <= 240; ++i)
    {
        const double hz = hzs[i];
        const float x = xFor (hz), y = yFor (dbs[i]);
        if (i == 0) { curve.startNewSubPath (x, y); fill.startNewSubPath (x, yFor (0.0f)); fill.lineTo (x, y); }
        else        { curve.lineTo (x, y); fill.lineTo (x, y); }
    }
    fill.lineTo (xFor (maxHz), yFor (0.0f));
    fill.closeSubPath();
    const auto curveCol = on ? accent : Palette::muted;
    g.setColour (curveCol.withAlpha (0.15f));
    g.fillPath (fill);
    g.setColour (curveCol.withAlpha (on ? 1.0f : 0.6f));
    g.strokePath (curve, PathStrokeType (2.0f, PathStrokeType::curved, PathStrokeType::rounded));
    // band handles
    for (int b = 0; b < 4; ++b)
    {
        const auto pt = handleFor (b);
        g.setColour (Colours::black.withAlpha (0.6f));
        g.fillEllipse (Rectangle<float> (18.0f, 18.0f).withCentre (pt));
        g.setColour (b == dragBand ? Colours::white : curveCol);
        if (b == dragBand) g.fillEllipse (Rectangle<float> (16.0f, 16.0f).withCentre (pt).reduced (5.0f));
        g.drawEllipse (Rectangle<float> (16.0f, 16.0f).withCentre (pt), b == dragBand ? 2.0f : 1.5f);
        g.setFont (spaced (9.0f, true, 0.0f));
        g.drawText (String (b + 1), Rectangle<float> (14.0f, 14.0f).withCentre (pt), Justification::centred);
    }
    // cut handles: faded while parked at the ends (off), solid once dragged in
    for (int b = 4; b < 6; ++b)
    {
        const bool engaged = b == 4 ? proc.lowCutEngaged() : proc.highCutEngaged();
        const auto pt = handleFor (b);
        const auto box = Rectangle<float> (26.0f, 18.0f).withCentre (pt);
        g.setColour (Colours::black.withAlpha (0.7f));
        g.fillRoundedRectangle (box, 5.0f);
        g.setColour ((b == dragBand ? Colours::white : curveCol).withAlpha (engaged ? 1.0f : 0.45f));
        g.drawRoundedRectangle (box, 5.0f, b == dragBand ? 2.0f : 1.3f);
        g.setFont (spaced (8.5f, true, 0.05f));
        g.drawText (b == 4 ? "LC" : "HC", box, Justification::centred);
    }
    if (! on)
    {
        g.setColour (Palette::muted);
        g.setFont (spaced (11.0f, true, 0.25f));
        g.drawText ("EQ OFF", ca.reduced (14.0f, 8.0f), Justification::topRight);
    }

    }

    // ---- the rack unit: sculpted like the amps, colour follows the live amp
    const auto u = unit.toFloat();
    const Colour cols[][3] = { { Colour (0xfff3f4f6), Colour (0xffc9ccd1), Colour (0xffe4e6e9) },    // white
                               { Colour (0xfff0e9dd), Colour (0xffc9bfad), Colour (0xffe2dacb) },    // ivory
                               { Colour (0xff3a3c42), Colour (0xff141517), Colour (0xff24262a) },    // black
                               { Colour (0xff36407a), Colour (0xff141a3a), Colour (0xff232b57) } };  // blue
    const auto& c = cols[jlimit (0, 3, amp)];
    const bool light = amp <= EmberAmpProcessor::crunch;
    // feet + shadow
    for (float fx : { u.getX() + 60.0f, u.getRight() - 120.0f })
    {
        g.setGradientFill (ColourGradient (Colour (0xff3a3d46), fx, u.getBottom(), Colour (0xff111216), fx, u.getBottom() + 16.0f, false));
        g.fillRoundedRectangle (fx, u.getBottom() - 6.0f, 60.0f, 20.0f, 5.0f);
    }
    g.setColour (Colours::black.withAlpha (0.5f));
    g.fillRoundedRectangle (u.translated (0.0f, 6.0f), 16.0f);
    g.setGradientFill (ColourGradient (c[0], u.getX(), u.getY(), c[1], u.getX(), u.getBottom(), false));
    g.fillRoundedRectangle (u, 16.0f);
    g.setColour (Colours::white.withAlpha (light ? 0.7f : 0.15f));
    g.drawRoundedRectangle (u.reduced (1.0f), 15.0f, 1.5f);
    // recessed face
    const auto f = face.toFloat();
    g.setColour (Colours::black.withAlpha (light ? 0.2f : 0.55f));
    g.fillRoundedRectangle (f.expanded (1.5f), 6.0f);
    g.setGradientFill (ColourGradient (c[2], f.getX(), f.getY(), c[2].darker (0.12f), f.getX(), f.getBottom(), false));
    g.fillRoundedRectangle (f, 6.0f);
    // a groove between the two knob rows
    g.setColour (Colours::black.withAlpha (light ? 0.12f : 0.4f));
    g.drawHorizontalLine ((int) f.getCentreY() - 6, f.getX() + 110.0f, f.getRight() - 130.0f);
    g.setColour (Colours::white.withAlpha (light ? 0.7f : 0.06f));
    g.drawHorizontalLine ((int) f.getCentreY() - 5, f.getX() + 110.0f, f.getRight() - 130.0f);
    // name + LEDs on the right
    const auto ink = light ? Colour (0xff2b2d33) : Colour (0xffe3e5ea);
    g.setColour (ink.withAlpha (0.85f));
    g.setFont (spaced (14.0f, true, 0.5f));
    g.drawText ("EMBER", Rectangle<float> (f.getRight() - 120.0f, f.getCentreY() - 22.0f, 100.0f, 20.0f), Justification::centredLeft);
    g.setColour (ink.withAlpha (0.55f));
    g.setFont (spaced (9.0f, false, 0.5f));
    g.drawText ("4-BAND EQ", Rectangle<float> (f.getRight() - 120.0f, f.getCentreY(), 100.0f, 14.0f), Justification::centredLeft);
    for (int i = 0; i < 3; ++i)
    {
        const auto led = Point<float> (f.getRight() - 112.0f + (float) i * 10.0f, f.getCentreY() + 26.0f);
        if (on) { g.setColour (accent.withAlpha (0.35f)); g.fillEllipse (Rectangle<float> (10.0f, 10.0f).withCentre (led)); }
        g.setColour (on ? accent.brighter (0.4f) : ink.withAlpha (0.25f));
        g.fillEllipse (Rectangle<float> (4.5f, 4.5f).withCentre (led));
    }
}

// the graph runs from 20 Hz to the top of what your audio device carries (20 kHz at 44.1 / 48 kHz)
double EqPage::topHz() const             { return juce::jmin (20000.0, 0.49 * (proc.getSampleRate() > 0 ? proc.getSampleRate() : 48000.0)); }
float EqPage::xForHz (double hz) const   { return plot.getX() + plot.getWidth() * (float) (std::log (juce::jmax (20.0, hz) / 20.0) / std::log (topHz() / 20.0)); }
double EqPage::hzForX (float x) const    { return 20.0 * std::pow (topHz() / 20.0, (double) juce::jlimit (0.0f, 1.0f, (x - plot.getX()) / plot.getWidth())); }
float EqPage::yForDb (float db) const    { return juce::jmap (juce::jlimit (-21.0f, 21.0f, db), 21.0f, -21.0f, plot.getY(), plot.getBottom()); }
float EqPage::dbForY (float y) const     { return juce::jmap (y, plot.getY(), plot.getBottom(), 21.0f, -21.0f); }

// each band's handle sits on its own gain at its frequency; the cut handles ride along the bottom
juce::Point<float> EqPage::handleFor (int b) const
{
    if (b >= 4)
    {
        // on the curve, at the cut's corner (-3 dB); parked at the end of the line while off
        const double hz = juce::jlimit (20.0, topHz(), (double) proc.apvts.getRawParameterValue (b == 4 ? "eqLowCut" : "eqHighCut")->load());
        return { xForHz (hz), yForDb (proc.getEqResponseDb (hz)) };
    }
    const auto n = juce::String (b + 1);
    return { xForHz (proc.apvts.getRawParameterValue ("eqFreq" + n)->load()), yForDb (proc.apvts.getRawParameterValue ("eqGain" + n)->load()) };
}

void EqPage::mouseDown (const juce::MouseEvent& e)
{
    if (! curveArea.contains (e.getPosition())) return;
    // grab the nearest handle (or, if none is close, the band whose range the click is in)
    float best = 1.0e9f;
    dragBand = -1;
    for (int b = 0; b < 6; ++b)
    {
        const float d = e.position.getDistanceFrom (handleFor (b)) * (b >= 4 ? 0.8f : 1.0f);   // cuts are easy to grab
        if (d < best) { best = d; dragBand = b; }
    }
    // (no handle near the click: the nearest one comes to you)
    if (dragBand < 0) return;
    if (dragBand >= 4) { if (auto* prm = proc.apvts.getParameter (dragBand == 4 ? "eqLowCut" : "eqHighCut")) prm->beginChangeGesture(); }
    else
        for (auto* id : { "eqFreq", "eqGain" })
            if (auto* prm = proc.apvts.getParameter (id + juce::String (dragBand + 1))) prm->beginChangeGesture();
    if (auto* on = proc.apvts.getParameter ("eqOn"); on != nullptr && on->getValue() < 0.5f)
        on->setValueNotifyingHost (1.0f);   // touching the graph switches the EQ on
    mouseDrag (e);
}

void EqPage::mouseDrag (const juce::MouseEvent& e)
{
    if (dragBand < 0) return;
    if (dragBand >= 4)
    {
        // drag a cut in from its end to switch it on; back to the end = off
        if (auto* f = proc.apvts.getParameter (dragBand == 4 ? "eqLowCut" : "eqHighCut"))
        {
            double hz = juce::jlimit (20.0, topHz(), hzForX (e.position.x));
            if (dragBand == 5 && hz >= 0.98 * topHz()) hz = 20000.0;   // parked at the top = off
            f->setValueNotifyingHost (f->convertTo0to1 ((float) hz));
        }
        refresh();
        return;
    }
    const auto n = juce::String (dragBand + 1);
    if (auto* f = proc.apvts.getParameter ("eqFreq" + n))
        f->setValueNotifyingHost (f->convertTo0to1 ((float) juce::jlimit (20.0, juce::jmin (20000.0, topHz()), hzForX (e.position.x))));
    if (auto* gn = proc.apvts.getParameter ("eqGain" + n))
        gn->setValueNotifyingHost (gn->convertTo0to1 (juce::jlimit (-EmberAmpProcessor::eqMaxGainDb, EmberAmpProcessor::eqMaxGainDb, dbForY (e.position.y))));
    refresh();
}

void EqPage::mouseUp (const juce::MouseEvent&)
{
    if (dragBand < 0) return;
    if (dragBand >= 4) { if (auto* prm = proc.apvts.getParameter (dragBand == 4 ? "eqLowCut" : "eqHighCut")) prm->endChangeGesture(); }
    else
        for (auto* id : { "eqFreq", "eqGain" })
            if (auto* prm = proc.apvts.getParameter (id + juce::String (dragBand + 1))) prm->endChangeGesture();
    dragBand = -1;
    repaint (curveArea.expanded (4));
}

void EqPage::mouseDoubleClick (const juce::MouseEvent& e)
{
    for (int b = 4; b < 6; ++b)   // double-click a cut: off
        if (e.position.getDistanceFrom (handleFor (b)) < 16.0f)
            if (auto* f = proc.apvts.getParameter (b == 4 ? "eqLowCut" : "eqHighCut"))
                f->setValueNotifyingHost (f->convertTo0to1 (b == 4 ? 20.0f : 20000.0f));
    for (int b = 0; b < 4; ++b)
        if (e.position.getDistanceFrom (handleFor (b)) < 16.0f)
            if (auto* gn = proc.apvts.getParameter ("eqGain" + juce::String (b + 1)))
                gn->setValueNotifyingHost (gn->convertTo0to1 (0.0f));   // double-click a handle: back to 0 dB
    refresh();
}

void EqPage::resized()
{
    auto r = getLocalBounds().reduced (40, 10);
    curveArea = r.removeFromTop (juce::jmin (200, r.getHeight() / 3));
    plot = curveArea.toFloat().reduced (40.0f, 16.0f);
    r.removeFromTop (30);
    unit = r.withSizeKeepingCentre (juce::jmin (r.getWidth(), 1040), juce::jmin (r.getHeight() - 24, 300));
    face = unit.reduced (22, 20);

    auto inner = face.reduced (10, 10);
    power.setBounds (inner.removeFromLeft (96).withSizeKeepingCentre (84, 56));
    inner.removeFromRight (130);
    const int colW = inner.getWidth() / 6;
    auto topRow = inner.removeFromTop (inner.getHeight() / 2);
    for (int i = 0; i < 6; ++i)
        topKnobs[i]->setBounds (topRow.withX (inner.getX() + i * colW).withWidth (colW).reduced (6, 2));
    // big gain knobs, each under its band's frequency knob; the cut switches under the cut knobs
    for (int b = 0; b < 4; ++b)
        gainKnobs[b]->setBounds (inner.withX (inner.getX() + (b + 1) * colW).withWidth (colW).reduced (2, 0));

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
    const char* tabNames[] = { "PEDALS", "CLEAN", "CRUNCH", "DISTORTION", "SOLO", "CAB", "EQ" };
    for (int i = 0; i < 3 + EmberAmpProcessor::numAmps; ++i)
    {
        auto& t = tabs[i];
        t.setButtonText (tabNames[i]);
        t.getProperties().set ("tab", true);
        t.getProperties().set ("icon", i == 0 ? "pedal" : i == cabPageIndex ? "cab" : i == eqPageIndex ? "eq" : "amp");
        setAccent (t, i == 0 || i >= cabPageIndex ? Palette::accent : ampAccent (i - 1));
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
    cabPage = std::make_unique<CabPage> (*this, p);
    addChildComponent (*cabPage);
    eqPage = std::make_unique<EqPage> (p);
    addChildComponent (*eqPage);

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

    // stereo / mono and the doubler
    monoSwitch.getProperties().set ("vswitch", true);
    monoSwitch.getProperties().set ("topText", "STEREO");
    monoSwitch.getProperties().set ("bottomText", "MONO");
    monoSwitch.getProperties().set ("onIsTop", false);
    monoSwitch.setTooltip ("Stereo: the doubler, chorus, delay and reverb spread wide.  Mono: everything in the centre");
    doublerSwitch.getProperties().set ("vswitch", true);
    doublerSwitch.getProperties().set ("topText", "ON");
    doublerSwitch.getProperties().set ("bottomText", "OFF");
    doublerSwitch.getProperties().set ("onIsTop", true);
    doublerSwitch.setTooltip ("Doubler: a second take on the right side, for a wide double-tracked sound");
    dynamicsSwitch.getProperties().set ("vswitch", true);
    dynamicsSwitch.getProperties().set ("topText", "ON");
    dynamicsSwitch.getProperties().set ("bottomText", "OFF");
    dynamicsSwitch.getProperties().set ("onIsTop", true);
    for (auto* b : { &monoSwitch, &doublerSwitch }) addAndMakeVisible (b);
    monoAtt    = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (p.apvts, "monoOut", monoSwitch);
    doublerAtt = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (p.apvts, "doublerOn", doublerSwitch);
    spreadKnob = std::make_unique<Knob> (p.apvts, "doublerSpread", "Spread", juce::Colours::white, "ring", Palette::text, 9.5f);
    addAndMakeVisible (*spreadKnob);

    // presets: icons, arrows
    prevPreset.getProperties().set ("iconOnly", "left");
    nextPreset.getProperties().set ("iconOnly", "right");
    savePresetButton.getProperties().set ("iconOnly", "save");
    presetDeleteIcon.getProperties().set ("iconOnly", "trash");
    presetImportIcon.getProperties().set ("iconOnly", "import");
    presetSearchIcon.getProperties().set ("iconOnly", "search");
    savePresetButton.setTooltip ("Save as a new preset");
    presetDeleteIcon.setTooltip ("Delete this preset (your own presets only)");
    presetImportIcon.setTooltip ("Import a preset file");
    presetSearchIcon.setTooltip ("Browse all presets");
    prevPreset.setTooltip ("Previous preset");
    nextPreset.setTooltip ("Next preset");
    presetDeleteIcon.onClick = [this] { deleteCurrentPreset(); };
    presetImportIcon.onClick = [this] { importPreset(); };
    presetSearchIcon.onClick = [this] { presetBox.showPopup(); };
    for (auto* b : { &prevPreset, &nextPreset, &savePresetButton, &presetDeleteIcon, &presetImportIcon, &presetSearchIcon })
    {
        b->setButtonText ({});
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
            // the app always opens with the input muted: unmute when you're ready to play
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

            settingsButton.setTooltip ("Audio settings: your interface, inputs and outputs");
            settingsButton.onClick = [this]
            {
                juce::PopupMenu m;
                m.addItem ("Audio Settings...", [] { if (auto* h = juce::StandalonePluginHolder::getInstance()) h->showAudioSettingsDialog(); });
                m.addSeparator();
                m.addItem ("Save Current State...", [] { if (auto* h = juce::StandalonePluginHolder::getInstance()) h->askUserToSaveState(); });
                m.addItem ("Load a Saved State...", [] { if (auto* h = juce::StandalonePluginHolder::getInstance()) h->askUserToLoadState(); });
                m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (settingsButton));
            };
            addAndMakeVisible (settingsButton);
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
    currentPage = juce::jlimit (0, (int) eqPageIndex, page);
    pedalsPage.setVisible (currentPage == 0);
    for (int a = 0; a < EmberAmpProcessor::numAmps; ++a)
        ampPages[a]->setVisible (currentPage == a + 1);
    cabPage->setVisible (currentPage == cabPageIndex);
    eqPage->setVisible (currentPage == eqPageIndex);
    if (currentPage == eqPageIndex) eqPage->refresh();
    if (currentPage == cabPageIndex) cabPage->refresh();

    // opening another amp's page makes it the amp you hear, starting on its preset 1
    if (isAmpPage() && proc.getSelectedAmp() != currentPage - 1)
    {
        proc.setCurrentProgram ((currentPage - 1) * EmberAmpProcessor::presetsPerAmp);
        presetsChanged();
    }

    for (int i = 0; i < 3 + EmberAmpProcessor::numAmps; ++i)
        tabs[i].setToggleState (i == currentPage, juce::dontSendNotification);
    repaint();
}

void EmberAmpEditor::presetsChanged()
{
    rebuildPresetMenu();
    for (auto& page : ampPages) page->refresh();
    pedalsPage.refresh();
    cabPage->refresh();
    if (isAmpPage() && proc.getSelectedAmp() != currentPage - 1)
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
        // the sound itself is NOT kept between launches: only saved presets are
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
    // night sky: deep indigo with a scatter of stars (rendered once per window size)
    if (starfield.getWidth() != getWidth() || starfield.getHeight() != getHeight())
    {
        starfield = Image (Image::RGB, jmax (1, getWidth()), jmax (1, getHeight()), false);
        Graphics sg (starfield);
        const auto sb = starfield.getBounds().toFloat();
        ColourGradient sky (Colour (0xff141634), sb.getCentreX(), sb.getHeight() * 0.35f, Colour (0xff05060f), sb.getRight(), sb.getBottom(), true);
        sky.addColour (0.5, Colour (0xff0c0d22));
        sg.setGradientFill (sky);
        sg.fillAll();
        Random rng (42);
        const int count = (int) (sb.getWidth() * sb.getHeight() / 900.0f);
        for (int i = 0; i < count; ++i)
        {
            const float x = rng.nextFloat() * sb.getWidth(), y = rng.nextFloat() * sb.getHeight();
            const float m = rng.nextFloat();
            const float size = m > 0.985f ? 2.4f : m > 0.9f ? 1.5f : 1.0f;
            const float alpha = 0.15f + 0.6f * rng.nextFloat() * (size > 1.4f ? 1.0f : 0.6f);
            if (size > 2.0f)
            {
                sg.setGradientFill (ColourGradient (Colours::white.withAlpha (0.35f), x, y, Colours::white.withAlpha (0.0f), x + 6.0f, y, true));
                sg.fillEllipse (x - 6.0f, y - 6.0f, 12.0f, 12.0f);
            }
            sg.setColour (Colour (0xffdfe4ff).withAlpha (alpha));
            sg.fillEllipse (x - size * 0.5f, y - size * 0.5f, size, size);
        }
    }
    g.drawImageAt (starfield, 0, 0);

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

    // headings: MODE, DOUBLER (with rules either side, spanning the switch and Spread), PRESETS, DYNAMICS
    g.setFont (spaced (12.0f, true, 0.12f));
    g.setColour (Palette::text);
    for (auto [area, text] : { std::pair<juce::Rectangle<int>, const char*> { modeHead, "MODE" }, { presetsHead, "PRESETS" }, { dynamicsHead, "DYNAMICS" } })
        g.drawText (text, area, Justification::centred);
    {
        const auto hd = doublerHead.toFloat();
        const float tw = (float) GlyphArrangement::getStringWidthInt (spaced (12.0f, true, 0.12f), "DOUBLER") + 16.0f;
        g.drawText ("DOUBLER", hd, Justification::centred);
        g.setColour (Colours::white.withAlpha (0.4f));
        g.drawHorizontalLine ((int) hd.getCentreY(), hd.getX() + 4.0f, hd.getCentreX() - tw * 0.5f);
        g.drawHorizontalLine ((int) hd.getCentreY(), hd.getCentreX() + tw * 0.5f, hd.getRight() - 4.0f);
    }

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
    const int amp = isAmpPage() ? currentPage - 1 : proc.getSelectedAmp();
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
    auto nav = area.removeFromTop (62);
    const int tabW = 84;
    auto tabRow = nav.withSizeKeepingCentre (tabW * (int) std::size (tabs), 54).translated (0, 4);
    for (auto& t : tabs)
        t.setBounds (tabRow.removeFromLeft (tabW));

    // ---- control strip:  INPUT | GATE  TRANSPOSE | MODE | DOUBLER | PRESETS | DYNAMICS | OUTPUT
    auto strip = area.removeFromTop (124).reduced (20, 6);
    dividers.clear();
    inMeter.setBounds (strip.removeFromLeft (6).reduced (0, 8));
    strip.removeFromLeft (8);
    inputKnob.setBounds (strip.removeFromLeft (100));
    strip.removeFromLeft (8);
    dividers.push_back (strip.removeFromLeft (1).reduced (0, 14));
    strip.removeFromLeft (8);
    {
        auto col = strip.removeFromLeft (92);
        gateSwitch.setBounds (col.removeFromTop (22).withSizeKeepingCentre (92, 22));
        gateKnob.setBounds (col.translated (0, -18).withTrimmedBottom (-18));
    }
    strip.removeFromLeft (2);
    {
        auto col = strip.removeFromLeft (136);
        transposeSwitch.setBounds (col.removeFromTop (22).withSizeKeepingCentre (136, 22));
        transposeKnob.setBounds (col.translated (0, -18).withTrimmedBottom (-18).withSizeKeepingCentre (100, col.getHeight() + 18));
    }
    strip.removeFromLeft (8);
    {
        auto col = strip.removeFromLeft (84);
        modeHead = col.removeFromTop (22);
        monoSwitch.setBounds (col.withSizeKeepingCentre (84, 56).translated (0, -4));
    }
    strip.removeFromLeft (8);
    {
        auto col = strip.removeFromLeft (150);
        doublerHead = col.removeFromTop (22);
        doublerSwitch.setBounds (col.removeFromLeft (64).withSizeKeepingCentre (64, 56).translated (0, -4));
        spreadKnob->setBounds (col.withTrimmedBottom (-8).reduced (4, 0));
    }

    auto right = strip.removeFromRight (6);
    outMeter.setBounds (right.reduced (0, 8));
    strip.removeFromRight (8);
    outputKnob.setBounds (strip.removeFromRight (100));
    strip.removeFromRight (8);
    dividers.push_back (strip.removeFromRight (1).reduced (0, 14));
    strip.removeFromRight (8);
    {
        auto col = strip.removeFromRight (82);
        dynamicsHead = col.removeFromTop (22);
        dynamicsSwitch.setBounds (col.withSizeKeepingCentre (82, 56).translated (0, -4));
    }
    strip.removeFromRight (8);

    // presets: heading, icon row, then  <  menu  >
    {
        auto centre = strip;
        presetsHead = centre.removeFromTop (22);
        auto icons = centre.removeFromTop (30).withSizeKeepingCentre (4 * 34 + 3 * 14, 30);
        for (auto* b : { &savePresetButton, &presetDeleteIcon, &presetImportIcon, &presetSearchIcon })
        {
            b->setBounds (icons.removeFromLeft (34));
            icons.removeFromLeft (14);
        }
        centre.removeFromTop (4);
        auto row = centre.removeFromTop (40);
        prevPreset.setBounds (row.removeFromLeft (30));
        nextPreset.setBounds (row.removeFromRight (30));
        presetBox.setBounds (row.reduced (4, 2));
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
            sb.removeFromLeft (16);
            settingsButton.setBounds (sb.removeFromLeft (96));
        }
    }

    // ---- stage
    stageArea = area.reduced (0, 4);
    pedalsPage.setBounds (stageArea.reduced (20, 10));
    for (auto& page : ampPages)
        page->setBounds (stageArea);
    cabPage->setBounds (stageArea);
    eqPage->setBounds (stageArea);
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
    const int amp = isAmpPage() ? currentPage - 1 : proc.getSelectedAmp();
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
    if (c >= '1' && c <= '7')  { showPage (c - '1'); return true; }   // 1 = pedals, 2..5 = amps, 6 = cab, 7 = eq
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

void EmberAmpEditor::deleteCurrentPreset()
{
    if (proc.getCurrentUserPreset().isEmpty()) return;
    presetBox.setSelectedId (deletePresetId, juce::sendNotificationSync);   // same confirm-and-delete path as the menu
}

void EmberAmpEditor::importPreset()
{
    importChooser = std::make_unique<juce::FileChooser> ("Import an Ember Amp preset", juce::File::getSpecialLocation (juce::File::userHomeDirectory), "*.emberpreset");
    juce::Component::SafePointer<EmberAmpEditor> safe (this);
    importChooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                                [safe] (const juce::FileChooser& fc)
                                {
                                    if (safe == nullptr || ! fc.getResult().existsAsFile()) return;
                                    auto dest = EmberAmpProcessor::getUserPresetFolder().getChildFile (fc.getResult().getFileName()).getNonexistentSibling (false);
                                    EmberAmpProcessor::getUserPresetFolder().createDirectory();
                                    if (fc.getResult().copyFileTo (dest) && safe->proc.loadUserPreset (dest))
                                        safe->presetsChanged();
                                });
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
        if (isAmpPage() && currentPage - 1 != sel)
            showPage (1 + sel);
        rebuildPresetMenu();
    }

    if (proc.getCurrentProgram() != lastProgramShown || proc.getCurrentUserPreset() != lastUserPresetShown)
        presetsChanged();
    presetDeleteIcon.setEnabled (proc.getCurrentUserPreset().isNotEmpty());

    if (frameCounter % 6 == 0)
    {
        for (auto& page : ampPages) page->refresh();
        pedalsPage.refresh();
        if (currentPage == cabPageIndex) cabPage->refresh();
        if (currentPage == eqPageIndex) eqPage->refresh();
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
