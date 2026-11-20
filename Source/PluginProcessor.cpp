#include "PluginProcessor.h"
#include "PluginEditor.h"

using Coeffs = juce::dsp::IIR::ArrayCoefficients<float>;

//==============================================================================
namespace
{
    float knobToGain (float knob) { return juce::Decibels::decibelsToGain ((knob - 5.0f) * 2.4f); } // 0..10 -> ±12 dB
    float dB (float d)            { return juce::Decibels::decibelsToGain (d); }

    //--------------------------------------------------------------------------
    // Amp voicings, indexed by amp: Clean, Crunch, Distortion, Solo
    struct Voicing
    {
        float gainMinDb, gainMaxDb;
        float tightHz, humpHz, humpDb;      // before the preamp
        float scoopDb, lowsDb, chunkDb;     // after the preamp
        float depthDb;                      // after the cab
        float makeupDb;
        float powerDrive;                   // 0 = no power-amp saturation stage
        float fizzHz;                       // low-pass after the preamp to tame fizz (0 = off)
        float levelThreshDb, levelRatio;    // built-in leveler so clean amps keep their volume at low pickup levels (ratio 0 = off)
        float levelMakeupDb;
        float splitDriveDb, splitMix;       // bass split: lows skip the distortion and come back clean-ish after it (mix 0 = off)
        float dynK;                         // volume-knob/picking response: output follows input^dynK (0 = off)
    };

    constexpr Voicing voicings[] = {
        //  gain range     tight  hump        scoop lows  chunk depth makeup power
        {  0.0f, 18.0f,   40.0f, 750.0f, 0.0f,  0.0f, 0.0f, 0.0f, 0.0f, 20.0f, 0.0f,    0.0f, -42.0f, 0.0f, 1.8f,   0.0f, 0.0f, 0.00f },   // Clean
        { 14.0f, 40.0f,   90.0f, 750.0f, 3.0f,  0.0f, 1.5f, 1.0f, 2.0f,  6.0f, 0.0f, 9000.0f, -34.0f, 2.0f, 12.0f,   0.0f, 0.0f, 0.50f },   // Crunch
        { 34.0f, 58.0f,  160.0f, 720.0f, 6.0f,  0.0f, 2.5f,-2.5f, 8.0f,  0.0f, 1.7f, 5500.0f,   0.0f, 0.0f,  0.0f,  18.0f, 2.20f, 0.55f },   // Distortion: tight into the gain, bass split for thump, low-mids cleared
        { 34.0f, 60.0f,  150.0f, 900.0f, 7.0f,  0.5f, 1.0f,-3.0f, 2.5f,  0.0f, 1.8f, 8000.0f,   0.0f, 0.0f,  0.0f,  18.0f, 0.50f, 0.50f },   // Solo: tight, mid-forward, saturated
    };

    //--------------------------------------------------------------------------
    // Built-in cab per amp
    struct CabVoicing
    {
        float hpHz, hpQ;
        float resHz, resQ, resDb;
        float scoopHz, scoopQ, scoopDb;
        float combHz, combQ, combDb;
        float biteHz, biteQ, biteDb;
        float lp1Hz, lp1Q, lp2Hz, lp2Q;
    };

    // every cab can go on every amp (EmberAmpProcessor::cabNames() is in this order)
    constexpr CabVoicing cabs[] = {
        // open-back 1x12: small and airy, light lows, bright and quick
        { 90.0f, 0.70f,  135.0f, 0.9f, 1.0f,  450.0f, 0.9f, -1.0f,  1800.0f, 1.2f, -1.0f,  3500.0f, 1.0f, 3.5f,  7000.0f, 0.8f, 9000.0f, 0.7f },
        // open-back 2x12: less thump, wide and sparkly
        { 60.0f, 0.70f,  110.0f, 0.9f, 2.0f,  400.0f, 0.9f, -1.5f,  1600.0f, 1.2f, -1.5f,  3200.0f, 1.0f, 3.0f,  6500.0f, 0.8f, 8500.0f, 0.7f },
        // closed-back 2x12: tighter and punchier than the open 2x12, still compact
        { 70.0f, 0.80f,  115.0f, 1.0f, 3.5f,  450.0f, 0.9f, -2.0f,  1500.0f, 1.3f, -1.5f,  2800.0f, 1.1f, 3.5f,  5800.0f, 0.85f, 7500.0f, 0.7f },
        // closed-back 4x12 with vintage 25 W speakers: thump, slight low-mid dip, forward bark
        { 72.0f, 0.85f,  105.0f, 1.1f, 5.0f,  520.0f, 0.9f, -2.5f,  1300.0f, 1.4f, -2.5f,  2500.0f, 1.3f, 4.5f,  5000.0f, 0.9f, 6800.0f, 0.7f },
        // closed-back 4x12 with modern 60 W speakers: deep, scooped, aggressive upper mids
        { 75.0f, 0.90f,  105.0f, 1.2f, 4.0f,  330.0f, 0.9f, -3.0f,  1100.0f, 1.5f, -1.0f,  2400.0f, 1.2f, 2.5f,  4500.0f, 0.9f, 6000.0f, 0.7f },
        // smooth lead 4x12: vocal mids, soft top end
        { 85.0f, 0.80f,  110.0f, 1.0f, 3.0f,  350.0f, 0.9f, -3.5f,  1500.0f, 1.2f, -1.0f,  2200.0f, 1.0f, 4.5f,  6000.0f, 0.8f, 7500.0f, 0.7f },
    };

    //--------------------------------------------------------------------------
    // Factory presets: 5 per amp
    struct FactoryPreset
    {
        const char* name;
        int amp;
        float gain, bass, mid, treble, presence, volume;
        bool compOn;  float sustain;
        bool odOn;    float boost;
        bool pitchOn; int pitch; float pitchMix;
        bool delayOn; float delayTime, delayFeedback, delayMix;
        bool revOn;   float revSize, revMix;
        float ampSustain, ampDrive;   // the amp's own Sustain knob, and Drive (not on Clean)
        bool chorusOn = false; float chorusRate = 0.8f, chorusDepth = 0.5f, chorusMix = 0.5f;
        bool dsOn = false; float dsDist = 0.6f, dsTone = 0.5f, dsLevel = 0.5f;   // DS-style distortion pedal
    };

    const FactoryPreset factory[] = {
        // ---- Clean
        // a plain, open clean: flat EQ, light sustain, a touch of room, nothing on top
        { "Normal Clean",       0, 0.25f, 5.0f, 5.0f, 6.5f, 6.5f,   0.0f,  false, 0.30f, false, 0.30f, false,  -2, 1.00f, false, 300.0f, 0.20f, 0.10f,  true, 0.35f, 0.12f, 1.00f, 0.00f },
        { "Glass Clean",        0, 0.294f, 5.5f, 4.5f, 6.5f, 6.0f,   1.9f,  false, 0.30f, false, 0.30f, false,  -2, 1.00f,  true, 380.0f, 0.25f, 0.15f,  true, 0.60f, 0.25f, 1.00f, 0.00f },
        { "Ambient Swell",      0, 0.197f, 5.0f, 4.0f, 7.0f, 6.0f,   2.7f,  false, 0.60f, false, 0.30f, false,  -2, 1.00f,  true, 650.0f, 0.60f, 0.35f,  true, 0.95f, 0.42f, 1.00f, 0.00f },
        { "Warm Jazz",          0, 0.426f, 6.5f, 5.0f, 4.5f, 3.5f,   -2.2f, false, 0.30f, false, 0.30f, false,  -2, 1.00f, false, 300.0f, 0.20f, 0.10f,  true, 0.45f, 0.18f, 1.00f, 0.00f },
        { "Chorus Clean",       0, 0.22f, 5.0f, 5.0f, 6.5f, 6.5f,   0.2f, false, 0.30f, false, 0.30f, false,  -2, 1.00f, false, 300.0f, 0.20f, 0.10f,  true, 0.45f, 0.18f, 1.00f, 0.00f,  true, 0.70f, 0.55f, 0.50f },
        // ---- Crunch
        { "Classic Crunch",     1, 0.70f, 6.0f, 6.0f, 6.0f, 6.0f,   3.2f, false, 0.20f, false, 0.30f, false,  -2, 1.00f, false, 300.0f, 0.20f, 0.10f,  true, 0.35f, 0.12f, 1.00f, 0.20f },
        { "Blues Breaker",      1, 0.35f, 5.0f, 6.5f, 5.5f, 5.0f,   7.8f,  false, 0.20f,  true, 0.15f, false,  -2, 1.00f,  true, 120.0f, 0.10f, 0.10f,  true, 0.40f, 0.15f, 1.00f, 0.20f },
        { "British Roar",       1, 0.90f, 5.5f, 7.0f, 6.5f, 6.5f,   -0.4f, false, 0.20f,  true, 0.30f, false,  -2, 1.00f, false, 300.0f, 0.20f, 0.10f,  true, 0.35f, 0.10f, 1.00f, 0.20f },
        { "Edge of Breakup",    1, 0.76f, 5.5f, 5.5f, 6.0f, 5.5f,   5.5f,  false, 0.25f, false, 0.30f, false,  -2, 1.00f, false, 300.0f, 0.20f, 0.10f,  true, 0.40f, 0.18f, 1.00f, 0.20f },
        { "Rock Rhythm",        1, 0.35f, 5.5f, 6.0f, 6.0f, 6.5f,  -0.8f, false, 0.20f, false, 0.30f, false,  -2, 1.00f, false, 300.0f, 0.20f, 0.10f,  true, 0.35f, 0.12f, 1.00f, 0.20f,  false, 0.80f, 0.50f, 0.50f,  true, 0.60f, 0.55f, 0.50f },   // DS-style pedal into a just-breaking-up amp
        // ---- Distortion
        { "Prog Metal Rhythm",  2, 0.72f, 4.5f, 6.0f, 6.0f, 6.5f,  -0.2f, false, 0.20f,  false, 0.55f, false,  -2, 1.00f, false, 300.0f, 0.15f, 0.20f,  true, 0.30f, 0.10f, 1.00f, 0.55f },
        { "Djent Tight",        2, 0.65f, 4.5f, 6.0f, 6.5f, 7.0f,  0.1f, false, 0.20f,  false, 0.80f, false,  -2, 1.00f, false, 300.0f, 0.15f, 0.20f, true, 0.30f, 0.10f, 1.00f, 0.80f },
        { "Drop C Chug",        2, 0.72f, 4.5f, 5.5f, 6.0f, 6.5f,  -1.0f, false, 0.20f,  false, 0.60f,  true,  -2, 1.00f, false, 300.0f, 0.15f, 0.20f,  true, 0.30f, 0.10f, 1.00f, 0.60f },
        { "Octave Doom",        2, 0.72f, 4.0f, 5.5f, 5.5f, 5.5f,   -1.2f, false, 0.20f,  false, 0.55f,  true, -12, 0.40f, false, 300.0f, 0.15f, 0.20f,  true, 0.50f, 0.15f, 1.00f, 0.55f },
        { "Texas Special",      2, 0.80f, 4.0f, 2.5f, 7.0f, 7.0f,   0.8f, false, 0.20f,   true, 0.50f, false,  -2, 1.00f, false, 300.0f, 0.15f, 0.20f,  true, 0.30f, 0.10f, 1.00f, 0.70f },   // Pantera-style: scooped, tight, bright
        // ---- Solo
        { "Singing Lead",       3, 0.72f, 5.0f, 7.0f, 6.0f, 6.0f,  -2.8f,  false, 0.50f,  true, 0.45f, false,  -2, 1.00f,  true, 450.0f, 0.35f, 0.22f,  true, 0.55f, 0.20f, 1.00f, 0.55f },
        { "Shred",              3, 0.90f, 4.5f, 6.5f, 6.5f, 7.0f,  -1.7f,  false, 0.35f,  true, 0.70f, false,  -2, 1.00f,  true, 360.0f, 0.22f, 0.16f,  true, 0.35f, 0.14f, 1.00f, 0.55f },
        { "Liquid Legato",      3, 0.75f, 5.0f, 7.5f, 5.5f, 5.5f,  -3.3f,  false, 0.65f,  true, 0.35f, false,  -2, 1.00f,  true, 520.0f, 0.40f, 0.25f,  true, 0.70f, 0.28f, 1.00f, 0.55f },
        { "Harmony Fifth",      3, 0.75f, 5.0f, 7.0f, 6.0f, 6.0f,  -2.1f,  false, 0.35f,  true, 0.50f,  true,   7, 0.45f,  true, 420.0f, 0.30f, 0.20f,  true, 0.50f, 0.20f, 1.00f, 0.55f },
        { "Texas Scoop Lead",   3, 0.95f, 6.5f, 3.0f, 7.0f, 7.5f,  -3.3f,  false, 0.35f,  true, 0.60f, false,  -2, 1.00f,  true, 380.0f, 0.25f, 0.18f,  true, 0.40f, 0.15f, 1.00f, 0.55f },
    };
    static_assert (std::size (factory) == EmberAmpProcessor::numAmps * EmberAmpProcessor::presetsPerAmp);

    // Every preset has its own cab and mic setup (cab model, mic type, position 0 = cap .. 1 = edge, distance 0..1 = 0..30 cm)
    struct CabSetup { int cab, mic; float pos, dist; };
    constexpr CabSetup presetCab[] = {
        { 1, 0, 0.35f, 0.15f },   // Normal Clean: 2x12 Open, Dynamic, 35% out, 4 cm
        { 0, 2, 0.20f, 0.30f },   // Glass Clean: 1x12 Open, Condenser, 20% out, 9 cm
        { 1, 2, 0.55f, 0.70f },   // Ambient Swell: 2x12 Open, Condenser, 55% out, 21 cm
        { 2, 1, 0.60f, 0.25f },   // Warm Jazz: 2x12 Closed, Ribbon, 60% out, 8 cm
        { 0, 3, 0.40f, 0.20f },   // Chorus Clean: 1x12 Open, Large Dyn, 40% out, 6 cm
        { 3, 0, 0.35f, 0.15f },   // Classic Crunch: 4x12 V25, Dynamic, 35% out, 4 cm
        { 2, 1, 0.45f, 0.35f },   // Blues Breaker: 2x12 Closed, Ribbon, 45% out, 10 cm
        { 3, 3, 0.25f, 0.10f },   // British Roar: 4x12 V25, Large Dyn, 25% out, 3 cm
        { 1, 0, 0.55f, 0.30f },   // Edge of Breakup: 2x12 Open, Dynamic, 55% out, 9 cm
        { 3, 0, 0.20f, 0.05f },   // Rock Rhythm: 4x12 V25, Dynamic, 20% out, 2 cm
        { 4, 0, 0.35f, 0.15f },   // Prog Metal Rhythm: 4x12 M60, Dynamic, 35% out, 4 cm
        { 4, 2, 0.15f, 0.05f },   // Djent Tight: 4x12 M60, Condenser, 15% out, 2 cm
        { 4, 3, 0.30f, 0.10f },   // Drop C Chug: 4x12 M60, Large Dyn, 30% out, 3 cm
        { 3, 1, 0.50f, 0.20f },   // Octave Doom: 4x12 V25, Ribbon, 50% out, 6 cm
        { 4, 0, 0.10f, 0.00f },   // Texas Special: 4x12 M60, Dynamic, 10% out, 0 cm
        { 5, 0, 0.35f, 0.15f },   // Singing Lead: 4x12 Smooth, Dynamic, 35% out, 4 cm
        { 5, 2, 0.25f, 0.20f },   // Shred: 4x12 Smooth, Condenser, 25% out, 6 cm
        { 5, 1, 0.45f, 0.25f },   // Liquid Legato: 4x12 Smooth, Ribbon, 45% out, 8 cm
        { 2, 3, 0.35f, 0.30f },   // Harmony Fifth: 2x12 Closed, Large Dyn, 35% out, 9 cm
        { 4, 3, 0.20f, 0.15f },   // Texas Scoop Lead: 4x12 M60, Large Dyn, 20% out, 4 cm
    };
    static_assert (std::size (presetCab) == std::size (factory));

    // Standard gate threshold per amp: more gain = more noise = harder gate.
    constexpr float standardGateDb = -81.0f;
    constexpr float ampGateDb[] = { -81.0f, -81.0f, -81.0f, -81.0f };
}

juce::String EmberAmpProcessor::ampName (int amp)
{
    static const char* names[] = { "Clean", "Crunch", "Distortion", "Solo" };
    return names[juce::jlimit (0, numAmps - 1, amp)];
}

juce::String EmberAmpProcessor::ampTagline (int amp)
{
    static const char* t[] = { "Chime, sparkle and headroom", "British-style crunch and bite",
                               "Modern high gain for heavy rhythm", "Singing, sustaining lead tone" };
    return t[juce::jlimit (0, numAmps - 1, amp)];
}

juce::StringArray EmberAmpProcessor::cabNames()
{
    return { "1x12 Open-Back", "2x12 Open-Back", "2x12 Closed-Back", "4x12 Vintage 25W", "4x12 Modern 60W", "4x12 Smooth Lead" };
}

juce::StringArray EmberAmpProcessor::micNames()
{
    return { "Dynamic", "Ribbon", "Condenser", "Large Dynamic" };
}

int EmberAmpProcessor::defaultCab (int amp)
{
    static const int c[] = { 1, 3, 4, 5 };   // each amp's original cab
    return c[juce::jlimit (0, numAmps - 1, amp)];
}

juce::String EmberAmpProcessor::cabName (int amp)
{
    return cabNames()[defaultCab (amp)];
}

//==============================================================================
void PitchShifter::prepare (double sampleRate)
{
    search      = juce::roundToInt (0.0085 * sampleRate);  // +-8.5 ms: one period of a low B string
    corrLen     = juce::roundToInt (0.0100 * sampleRate);  // longer match window = steadier on real playing
    xfadeLen    = juce::roundToInt (0.0120 * sampleRate);  // smooth splice
    spliceEvery = juce::roundToInt (0.0500 * sampleRate);  // fewer splices
    int size = 1;
    while (size < 2 * (spliceEvery + xfadeLen + 3 * search + corrLen) + 16) size <<= 1;
    buf.assign ((size_t) size, 0.0f);
    mask = size - 1;
    reset();
}

void PitchShifter::reset()
{
    std::fill (buf.begin(), buf.end(), 0.0f);
    writePos = 0;
    active = incoming = (float) (1 + search);
    fadePos = -1;
}

float PitchShifter::read (float delay) const noexcept
{
    float rp = (float) writePos - delay;
    const float size = (float) (mask + 1);
    while (rp < 0.0f) rp += size;
    const int i0 = (int) rp;
    const float frac = rp - (float) i0;
    const float a = buf[(size_t) (i0 & mask)], b = buf[(size_t) ((i0 + 1) & mask)];
    return a + frac * (b - a);
}

float PitchShifter::findAligned (float nominal, float current) const noexcept
{
    // Normalised cross-correlation of the waveform under each candidate start against the one
    // under the playing head, with a pull toward the nominal start so it doesn't hop between periods.
    const int cd = juce::roundToInt (current);
    const float frac = current - (float) cd;   // keep the same sub-sample grid as the playing head
    auto ncc = [&] (int d)
    {
        float dot = 0.0f, e = 1.0e-9f;
        for (int m = 0; m < corrLen; m += 3)
        {
            const float c = readInt (d + m);
            dot += c * readInt (cd + m);
            e   += c * c;
        }
        return dot / std::sqrt (e);
    };
    auto weight = [&] (int offset) { return 1.0f - 0.15f * std::abs ((float) offset) / (float) search; };

    const int n0 = juce::roundToInt (nominal);
    int best = n0; float bestScore = -1.0e30f;
    for (int o = -search; o <= search; o += 2)
    {
        const int d = n0 + o;
        if (d < 1) continue;
        const float score = ncc (d) * weight (o);
        if (score > bestScore) { bestScore = score; best = d; }
    }
    for (int d = best - 1; d <= best + 1; d += 2)          // fine step
        if (d >= 1)
            if (const float sc = ncc (d) * weight (d - n0); sc > bestScore)
                { bestScore = sc; best = d; }

    return juce::jmax (1.0f, (float) best + frac);
}

float PitchShifter::process (float x, float ratio) noexcept
{
    buf[(size_t) writePos] = x;
    const float rate = 1.0f - ratio;                      // delay change per sample (+ = pitch down)

    if (fadePos < 0)
    {
        if (rate > 0.0f)
        {
            // pitch down: the delay grows; once it has travelled a splice's worth, jump back near "now"
            const float start = 1.0f + (float) search;
            if (active >= start + (float) search + rate * (float) spliceEvery)
                { incoming = findAligned (start, active); fadePos = 0; }
        }
        else if (rate < 0.0f)
        {
            // pitch up: the delay shrinks; before it reaches "now" (allowing for the fade), jump back
            const float floorD = 2.0f - rate * (float) xfadeLen;
            if (active <= floorD)
                { incoming = findAligned (floorD + (float) search - rate * (float) spliceEvery, active); fadePos = 0; }
        }
    }

    float out;
    if (fadePos >= 0)
    {
        const float t = ((float) fadePos + 0.5f) / (float) xfadeLen;
        const float g = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::pi * t);
        out = (1.0f - g) * read (active) + g * read (incoming);
        incoming = juce::jmax (1.0f, incoming + rate);
        active   = juce::jmax (1.0f, active + rate);
        if (++fadePos >= xfadeLen) { active = incoming; fadePos = -1; }
    }
    else
    {
        out = read (active);
        active = juce::jmax (1.0f, active + rate);
    }

    writePos = (writePos + 1) & mask;
    return out;
}

//==============================================================================
void HardGate::prepare (double sr)
{
    envRel  = std::exp (-1.0f / (0.012f * (float) sr));        // detector release 12 ms
    gAtt    = 1.0f - std::exp (-1.0f / (0.0004f * (float) sr)); // opens in ~0.4 ms
    gRel    = 1.0f - std::exp (-1.0f / (0.040f * (float) sr));  // closes over ~40 ms
    holdLen = (int) (0.040 * sr);
    hpA     = 1.0f / (1.0f + juce::MathConstants<float>::twoPi * 70.0f / (float) sr);
    reset();
}

void HardGate::reset()
{
    env = gain = hpY = hpX = 0.0f;
    hold = 0;
    open = false;
}

void HardGate::process (float* x, int n, float thresholdDb) noexcept
{
    const float openT  = juce::Decibels::decibelsToGain (thresholdDb);
    const float closeT = juce::Decibels::decibelsToGain (thresholdDb - 4.0f);
    for (int i = 0; i < n; ++i)
    {
        // detector: high-passed (ignores hum), peak envelope
        hpY = hpA * (hpY + x[i] - hpX);
        hpX = x[i];
        env = juce::jmax (std::abs (hpY), env * envRel);

        if (env > openT)        { open = true; hold = holdLen; }
        else if (env < closeT)  { if (hold > 0) --hold; else open = false; }

        const float target = open ? 1.0f : 0.0f;
        gain += (target > gain ? gAtt : gRel) * (target - gain);
        x[i] *= gain;
    }
}

//==============================================================================
void StereoChorus::prepare (double sampleRate)
{
    sr = sampleRate;
    size = 1;
    while (size < (int) (sr * 0.05) + 8) size <<= 1;
    line.assign ((size_t) size, 0.0f);
    reset();
}

void StereoChorus::reset()
{
    std::fill (line.begin(), line.end(), 0.0f);
    writePos = 0;
    phase = 0.0f;
}

float StereoChorus::read (float d) const noexcept
{
    const int mask = size - 1;
    const float rp = (float) writePos - d;
    const int i0 = (int) std::floor (rp);
    const float fr = rp - (float) i0;
    // cubic (Hermite) interpolation: smooth, no zipper noise
    const float xm1 = line[(size_t) ((i0 - 1) & mask)], x0 = line[(size_t) (i0 & mask)];
    const float x1 = line[(size_t) ((i0 + 1) & mask)], x2 = line[(size_t) ((i0 + 2) & mask)];
    const float c1 = 0.5f * (x1 - xm1), c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2, c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
    return ((c3 * fr + c2) * fr + c1) * fr + x0;
}

void StereoChorus::process (juce::AudioBuffer<float>& buffer, int numSamples, float rateHz, float depth, float mix)
{
    const bool stereo = buffer.getNumChannels() > 1;
    auto* L = buffer.getWritePointer (0);
    auto* R = stereo ? buffer.getWritePointer (1) : nullptr;
    const float base = 0.012f * (float) sr;                       // 12 ms centre delay
    const float swing = (0.0005f + 0.0045f * depth) * (float) sr; // up to +-5 ms
    const float inc = juce::MathConstants<float>::twoPi * rateHz / (float) sr;
    const int mask = size - 1;

    for (int i = 0; i < numSamples; ++i)
    {
        const float dry = stereo ? 0.5f * (L[i] + R[i]) : L[i];
        line[(size_t) writePos] = dry;
        const float wl = read (base + swing * std::sin (phase));
        const float wr = read (base + swing * std::sin (phase + juce::MathConstants<float>::halfPi));
        phase += inc;
        if (phase > juce::MathConstants<float>::twoPi) phase -= juce::MathConstants<float>::twoPi;
        writePos = (writePos + 1) & mask;

        // dry stays at full level; the moving copies are added on top, panned left / right
        L[i] += mix * 0.7f * wl;
        if (R != nullptr) R[i] += mix * 0.7f * wr;
    }
}

//==============================================================================
void PingPongDelay::prepare (double sampleRate)
{
    sr = sampleRate;
    size = (int) (sr * 2.0) + 4;
    for (auto& l : lines) l.assign ((size_t) size, 0.0f);
    delaySamples.reset (sr, 0.12);
    delaySamples.setCurrentAndTargetValue ((float) (0.3 * sr));
    reset();
}

void PingPongDelay::reset()
{
    for (auto& l : lines) std::fill (l.begin(), l.end(), 0.0f);
    lp[0] = lp[1] = 0.0f;
    writePos = 0;
}

void PingPongDelay::process (juce::AudioBuffer<float>& buffer, int numSamples, float timeMs, float feedback, float mix)
{
    delaySamples.setTargetValue (juce::jlimit (1.0f, (float) size - 3.0f, timeMs * 0.001f * (float) sr));

    const bool stereo = buffer.getNumChannels() > 1;
    auto* L = buffer.getWritePointer (0);
    auto* R = stereo ? buffer.getWritePointer (1) : nullptr;
    const float a = 1.0f - std::exp (-juce::MathConstants<float>::twoPi * 3800.0f / (float) sr); // feedback LPF

    for (int i = 0; i < numSamples; ++i)
    {
        float readPos = (float) writePos - delaySamples.getNextValue();
        if (readPos < 0.0f) readPos += (float) size;
        int i0 = (int) readPos;
        const float frac = juce::jlimit (0.0f, 1.0f, readPos - (float) i0);
        // a tiny negative position can round up to exactly `size` once wrapped: keep the index inside the line
        if (i0 >= size) i0 -= size;
        if (i0 < 0)     i0 += size;
        const int i1 = (i0 + 1) % size;

        const float dl = lines[0][(size_t) i0] + frac * (lines[0][(size_t) i1] - lines[0][(size_t) i0]);
        const float dr = lines[1][(size_t) i0] + frac * (lines[1][(size_t) i1] - lines[1][(size_t) i0]);

        lp[0] += a * (dl - lp[0]);
        lp[1] += a * (dr - lp[1]);

        const float inL = L[i];
        const float inR = stereo ? R[i] : inL;

        // input -> left line, left echo -> right line, right echo -> left line
        lines[0][(size_t) writePos] = 0.5f * (inL + inR) + feedback * lp[1];
        lines[1][(size_t) writePos] = feedback * lp[0];

        if (stereo)
        {
            L[i] = inL + mix * dl;
            R[i] = inR + mix * dr;
        }
        else
        {
            L[i] = inL + mix * 0.5f * (dl + dr);
        }

        if (++writePos >= size) writePos = 0;
    }
}

//==============================================================================
EmberAmpProcessor::EmberAmpProcessor()
    : AudioProcessor (BusesProperties()
                        .withInput  ("Input",  juce::AudioChannelSet::mono(),   true)
                        .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PARAMS", createLayout())
{
    auto raw = [this] (const juce::String& id) { return apvts.getRawParameterValue (id); };

    pInput = raw ("input");   pMaster = raw ("master");   pMute = raw ("inputMute");   pAmp = raw ("amp");
    pDynamics = raw ("dynamics");
    pGateOn = raw ("gateOn");   pGate = raw ("gate");
    pCompOn = raw ("compOn");   pSustain = raw ("sustain");
    pOdOn = raw ("odOn");       pBoost = raw ("boost");
    pDsOn = raw ("dsOn"); pDsDist = raw ("dsDist"); pDsTone = raw ("dsTone"); pDsLevel = raw ("dsLevel");
    pPitchOn = raw ("pitchOn"); pPitch = raw ("pitch");  pPitchMix = raw ("pitchMix");
    pDelayOn = raw ("delayOn"); pDTime = raw ("delayTime"); pDFb = raw ("delayFeedback"); pDMix = raw ("delayMix");
    pChorusOn = raw ("chorusOn"); pCRate = raw ("chorusRate"); pCDepth = raw ("chorusDepth"); pCMix = raw ("chorusMix");
    pRevOn = raw ("reverbOn");  pRSize = raw ("reverbSize"); pRMix = raw ("reverbMix");

    for (int a = 0; a < numAmps; ++a)
        knobs[a] = { raw (ampParamID (a, "gain")), raw (ampParamID (a, "bass")), raw (ampParamID (a, "mid")),
                     raw (ampParamID (a, "treble")), raw (ampParamID (a, "presence")), raw (ampParamID (a, "volume")),
                     raw (ampParamID (a, "cab")), raw (ampParamID (a, "sustain")),
                     a != clean ? raw (ampParamID (a, "drive")) : nullptr,
                     raw (ampParamID (a, "cabModel")), raw (ampParamID (a, "mic")), raw (ampParamID (a, "micPos")), raw (ampParamID (a, "micDist")) };

    for (auto& f : irActive) f = false;

    // Give every amp a sensible starting sound (its first factory preset), then start on Clean.
    for (int a = numAmps - 1; a >= 0; --a)
        setCurrentProgram (a * presetsPerAmp);

    for (auto* id : { "pitchOn", "pitch", "pitchMix" })
        apvts.addParameterListener (id, this);
}

EmberAmpProcessor::~EmberAmpProcessor()
{
    for (auto* id : { "pitchOn", "pitch", "pitchMix" })
        apvts.removeParameterListener (id, this);
}

void EmberAmpProcessor::parameterChanged (const juce::String& id, float value)
{
    // remember what YOU set; changes made by loading a preset, or while on a pitch preset, don't count
    if (applyingPreset.load() || onPitchPreset.load()) return;
    if (id == "pitchOn")       userPitchOn = value > 0.5f;
    else if (id == "pitch")    userPitch = juce::roundToInt (value);
    else if (id == "pitchMix") userPitchMix = value;
}

juce::AudioProcessorValueTreeState::ParameterLayout EmberAmpProcessor::createLayout()
{
    using namespace juce;
    std::vector<std::unique_ptr<RangedAudioParameter>> p;

    auto dBA   = AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return String (v, 1) + " dB"; });
    auto knob  = AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return String (v, 1); });
    auto pct   = AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return String (roundToInt (v * 100.0f)) + "%"; })
                                                .withValueFromStringFunction ([] (const String& t) { return t.retainCharacters ("0123456789.-").getFloatValue() / 100.0f; });
    auto ms    = AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return String (roundToInt (v)) + " ms"; });
    auto gateA = AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return String (roundToInt (v)) + " dB"; });

    // global
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "input", 1 }, "Input", NormalisableRange<float> (-24.0f, 24.0f, 0.1f), 0.0f, dBA));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "master", 1 }, "Output", NormalisableRange<float> (-36.0f, 12.0f, 0.1f), 0.0f, dBA));
    p.push_back (std::make_unique<AudioParameterBool>  (ParameterID { "inputMute", 1 }, "Input Mute", false));
    p.push_back (std::make_unique<AudioParameterBool>  (ParameterID { "dynamics", 1 }, "Dynamics", false));   // amp follows picking / volume knob
    p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { "amp", 1 }, "Amp", StringArray { "Clean", "Crunch", "Distortion", "Solo" }, 0));

    // each amp: its own knobs and cab switch
    for (int a = 0; a < numAmps; ++a)
    {
        const auto n = ampName (a) + " ";
        p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { ampParamID (a, "gain"), 1 }, n + "Gain", NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.5f, pct));
        p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { ampParamID (a, "bass"), 1 }, n + "Bass", NormalisableRange<float> (0.0f, 10.0f, 0.01f), 5.0f, knob));
        p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { ampParamID (a, "mid"), 1 }, n + "Mid", NormalisableRange<float> (0.0f, 10.0f, 0.01f), 5.0f, knob));
        p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { ampParamID (a, "treble"), 1 }, n + "Treble", NormalisableRange<float> (0.0f, 10.0f, 0.01f), 5.0f, knob));
        p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { ampParamID (a, "presence"), 1 }, n + "Presence", NormalisableRange<float> (0.0f, 10.0f, 0.01f), 5.0f, knob));
        p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { ampParamID (a, "volume"), 1 }, n + "Volume", NormalisableRange<float> (-24.0f, 12.0f, 0.1f), 0.0f, dBA));
        p.push_back (std::make_unique<AudioParameterBool>  (ParameterID { ampParamID (a, "cab"), 1 }, n + "Cab", true));
        p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { ampParamID (a, "sustain"), 1 }, n + "Sustain", NormalisableRange<float> (0.0f, 1.0f, 0.001f), 1.0f, pct));
        // cab and mic: any cab on any amp; the defaults reproduce each amp's original cab exactly
        p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { ampParamID (a, "cabModel"), 1 }, n + "Cab Model", cabNames(), defaultCab (a)));
        p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { ampParamID (a, "mic"), 1 }, n + "Mic", micNames(), 0));
        p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { ampParamID (a, "micPos"), 1 }, n + "Mic Position", NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.35f,
                         AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int)
                         { return v < 0.12f ? String ("Cap") : v > 0.88f ? String ("Edge") : String (roundToInt (v * 100.0f)) + "%"; })));
        p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { ampParamID (a, "micDist"), 1 }, n + "Mic Distance", NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.15f,
                         AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return String (roundToInt (v * 30.0f)) + " cm"; })
                                                        .withValueFromStringFunction ([] (const String& t) { return t.retainCharacters ("0123456789.").getFloatValue() / 30.0f; })));
        if (a != clean)   // Distortion has always had Drive; Crunch and Solo got it in 1.2 (starts at 0 = as before)
            p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { ampParamID (a, "drive"), 1 }, n + "Drive", NormalisableRange<float> (0.0f, 1.0f, 0.001f),
                                                                a == distortion ? 0.5f : 0.0f, pct));
    }

    // pedals
    p.push_back (std::make_unique<AudioParameterBool>  (ParameterID { "gateOn", 1 }, "Gate On", true));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "gate", 1 }, "Gate Threshold", NormalisableRange<float> (-96.0f, -20.0f, 0.5f), standardGateDb, gateA));
    p.push_back (std::make_unique<AudioParameterBool>  (ParameterID { "compOn", 1 }, "Compressor On", false));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "sustain", 1 }, "Sustain", NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.4f, pct));
    p.push_back (std::make_unique<AudioParameterBool>  (ParameterID { "odOn", 1 }, "Overdrive On", false));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "boost", 1 }, "Overdrive Drive", NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.5f, pct));
    p.push_back (std::make_unique<AudioParameterBool>  (ParameterID { "dsOn", 1 }, "Distortion On", false));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "dsDist", 1 }, "Distortion Dist", NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.6f, pct));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "dsTone", 1 }, "Distortion Tone", NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.5f, pct));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "dsLevel", 1 }, "Distortion Level", NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.5f, pct));
    p.push_back (std::make_unique<AudioParameterBool>  (ParameterID { "pitchOn", 1 }, "Pitch On", false));
    p.push_back (std::make_unique<AudioParameterInt>   (ParameterID { "pitch", 1 }, "Pitch", -12, 12, -1,
                     AudioParameterIntAttributes().withStringFromValueFunction ([] (int v, int)
                     {
                         if (v == 0)   return String ("0 st");
                         if (v == -12) return String ("-1 oct");
                         if (v == 12)  return String ("+1 oct");
                         return (v > 0 ? "+" : "") + String (v) + " st";
                     })
                     .withValueFromStringFunction ([] (const String& t)
                     {
                         const int n = t.retainCharacters ("0123456789-").getIntValue();
                         return t.containsIgnoreCase ("oct") ? juce::jlimit (-12, 12, n * 12) : juce::jlimit (-12, 12, n);
                     })));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "pitchMix", 1 }, "Pitch Mix", NormalisableRange<float> (0.0f, 1.0f, 0.001f), 1.0f, pct));
    p.push_back (std::make_unique<AudioParameterBool>  (ParameterID { "chorusOn", 1 }, "Chorus On", false));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "chorusRate", 1 }, "Chorus Rate", NormalisableRange<float> (0.1f, 5.0f, 0.01f, 0.5f), 0.8f,
                     AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return String (v, 2) + " Hz"; })));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "chorusDepth", 1 }, "Chorus Depth", NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.5f, pct));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "chorusMix", 1 }, "Chorus Mix", NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.5f, pct));
    p.push_back (std::make_unique<AudioParameterBool>  (ParameterID { "delayOn", 1 }, "Delay On", false));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "delayTime", 1 }, "Delay Time", NormalisableRange<float> (40.0f, 1500.0f, 1.0f, 0.5f), 380.0f, ms));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "delayFeedback", 1 }, "Delay Feedback", NormalisableRange<float> (0.0f, 0.9f, 0.001f), 0.3f, pct));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "delayMix", 1 }, "Delay Mix", NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.25f, pct));
    p.push_back (std::make_unique<AudioParameterBool>  (ParameterID { "reverbOn", 1 }, "Reverb On", false));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "reverbSize", 1 }, "Reverb Size", NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.5f, pct));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "reverbMix", 1 }, "Reverb Mix", NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.2f, pct));

    return { p.begin(), p.end() };
}

bool EmberAmpProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto in  = layouts.getMainInputChannelSet();
    const auto out = layouts.getMainOutputChannelSet();
    const bool inOk  = in == juce::AudioChannelSet::mono() || in == juce::AudioChannelSet::stereo();
    const bool outOk = out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo();
    return inOk && outOk;
}

void EmberAmpProcessor::selectAmp (int amp)
{
    amp = juce::jlimit (0, numAmps - 1, amp);
    if (auto* param = apvts.getParameter ("amp"))
        param->setValueNotifyingHost (param->convertTo0to1 ((float) amp));

    // Switching amps: delay goes off and a little reverb stays on; on Solo, delay + reverb are on.
    auto set = [this] (const char* id, float value)
    {
        if (auto* param = apvts.getParameter (id))
            param->setValueNotifyingHost (param->convertTo0to1 (value));
    };
    set ("delayOn", amp == solo ? 1.0f : 0.0f);
    set ("chorusOn", 0.0f);
    set ("reverbOn", 1.0f);
    if (amp != solo)
    {
        set ("reverbMix", 0.12f);
        set ("reverbSize", 0.35f);
    }

    // ...and the gate goes to this amp's standard level (more gain = harder gate)
    if (auto* param = apvts.getParameter ("gateOn")) param->setValueNotifyingHost (1.0f);
    if (auto* param = apvts.getParameter ("gate"))   param->setValueNotifyingHost (param->convertTo0to1 (ampGateDb[amp]));
}

//==============================================================================
void EmberAmpProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    currentSampleRate = sampleRate;
    mono.setSize (1, samplesPerBlock);
    splitBuf.assign ((size_t) samplesPerBlock, 0.0f);
    dynGain.assign ((size_t) samplesPerBlock, 1.0f);
    dynEnv = 0.0f;
    // volume-pedal feel: follows your overall playing level (guitar volume knob, playing softer
    // or harder for a passage) and glides between levels, instead of jumping on every pick
    dynAtt = 1.0f - std::exp (-1.0f / (0.060f * (float) sampleRate));     // loudness follower rise
    dynRel = 1.0f - std::exp (-1.0f / (0.350f * (float) sampleRate));     // and fall
    dynGainUp   = 1.0f - std::exp (-1.0f / (0.150f * (float) sampleRate));
    dynGainDown = 1.0f - std::exp (-1.0f / (0.300f * (float) sampleRate));
    dynGainDb = 0.0f;

    juce::dsp::ProcessSpec monoSpec { sampleRate, (juce::uint32) samplesPerBlock, 1 };
    gate.prepare (sampleRate);

    ampComp.prepare (monoSpec);
    ampComp.setAttack (10.0f);      // keeps the pick attack
    ampComp.setRelease (220.0f);
    ampCompMakeup.reset (sampleRate, 0.05);
    ampCompMakeup.setCurrentAndTargetValue (1.0f);
    ampDrive.reset (sampleRate * 4.0, 0.05);
    ampDrive.setCurrentAndTargetValue (0.0f);
    ampDriveLp = 0.0f;

    leveler.prepare (monoSpec);
    leveler.setAttack (8.0f);
    leveler.setRelease (180.0f);

    sustainComp.prepare (monoSpec);
    sustainComp.setAttack (12.0f);    // let the pick attack through
    sustainComp.setRelease (250.0f);
    sustainMakeup.reset (sampleRate, 0.05);
    sustainMakeup.setCurrentAndTargetValue (1.0f);

    oversampler.reset();
    oversampler.initProcessing ((size_t) samplesPerBlock);
    setLatencySamples ((int) std::round (oversampler.getLatencyInSamples()));

    const double osRate = sampleRate * (double) oversampler.getOversamplingFactor();
    inGain.reset (sampleRate, 0.05);
    muteGain.reset (sampleRate, 0.02);
    muteGain.setCurrentAndTargetValue (pMute->load() > 0.5f ? 0.0f : 1.0f);
    outGain.reset (sampleRate, 0.05);
    ampVolume.reset (sampleRate, 0.05);
    switchGain.reset (sampleRate, 0.008);
    switchGain.setCurrentAndTargetValue (1.0f);
    driveGain.reset (osRate, 0.05);
    boostAmount.reset (osRate, 0.05);
    boostAmount.setCurrentAndTargetValue (pOdOn->load() > 0.5f ? pBoost->load() : 0.0f);
    boostLp = 0.0f;
    {
        const auto osr = (float) osRate;
        auto onePole = [osr] (float hz) { return 1.0f - std::exp (-juce::MathConstants<float>::twoPi * hz / osr); };
        dsInCoef = onePole (120.0f);     // input booster trims the lows (keeps it from farting out)
        dsLpCoef = onePole (500.0f);     // tone: dark side
        dsHpCoef = onePole (1100.0f);    // tone: bright side (the blend scoops the mids, DS style)
        dsSmoothCoef = onePole (7500.0f);
        dsInLp = dsLp = dsHpLp = dsSmooth = 0.0f;
        for (auto* sv : { &dsWet, &dsDrive, &dsTone, &dsLevel }) sv->reset (osRate, 0.05);
        dsWet.setCurrentAndTargetValue (pDsOn->load() > 0.5f ? 1.0f : 0.0f);
        dsDrive.setCurrentAndTargetValue (pDsDist->load());
        dsTone.setCurrentAndTargetValue (pDsTone->load());
        dsLevel.setCurrentAndTargetValue (pDsLevel->load());
    }
    boostCoef = 1.0f - std::exp (-juce::MathConstants<float>::twoPi * 320.0f / (float) osRate); // pedal low-cut
    inGain.setCurrentAndTargetValue (dB (pInput->load()));
    outGain.setCurrentAndTargetValue (dB (pMaster->load()));

    for (auto* b : { &tight, &preEmphasis, &postVoicing, &postLows, &chunk, &fizz, &splitLp1, &splitLp2, &splitPost, &dynSmooth, &bass, &mid, &treble, &presence,
                     &cabHp, &cabLowRes, &cabScoop, &cabComb, &cabBite, &cabLp1, &cabLp2, &depth,
                     &micA, &micB, &micPosShelf, &micDistLow, &micDistHigh })
        b->reset();
    std::fill (reflBuf.begin(), reflBuf.end(), 0.0f);
    reflPos = 0;

    activeAmp = getSelectedAmp();
    setAmpVoicing (activeAmp);
    const auto& v = voicings[activeAmp];
    driveGain.setCurrentAndTargetValue (dB (juce::jmap (knobs[activeAmp].gain->load(), v.gainMinDb, v.gainMaxDb)));
    ampVolume.setCurrentAndTargetValue (dB (knobs[activeAmp].volume->load()));

    for (auto& c : irConvolution) { c.prepare (monoSpec); c.reset(); }

    pitchShifter.prepare (sampleRate);
    pitchWet.reset (sampleRate, 0.03);
    pitchRatio.reset (sampleRate, 0.03);
    {
        const int semis = juce::roundToInt (pPitch->load());
        const bool on = pPitchOn->load() > 0.5f && semis != 0;
        pitchWet.setCurrentAndTargetValue (on ? pPitchMix->load() : 0.0f);
        pitchRatio.setCurrentAndTargetValue (std::pow (2.0f, (float) semis / 12.0f));
    }

    chorus.prepare (sampleRate);
    chorusWet.reset (sampleRate, 0.05);
    chorusWet.setCurrentAndTargetValue (pChorusOn->load() > 0.5f ? pCMix->load() : 0.0f);
    delay.prepare (sampleRate);
    delayWet.reset (sampleRate, 0.05);
    delayWet.setCurrentAndTargetValue (pDelayOn->load() > 0.5f ? pDMix->load() : 0.0f);

    juce::dsp::ProcessSpec stereoSpec { sampleRate, (juce::uint32) samplesPerBlock, 2 };
    reverb.prepare (stereoSpec);
    reverb.reset();
    reverbWet.reset (sampleRate, 0.05);
    reverbWet.setCurrentAndTargetValue (pRevOn->load() > 0.5f ? pRMix->load() : 0.0f);
}

void EmberAmpProcessor::setAmpVoicing (int amp)
{
    const auto sr = getSampleRate() > 0 ? (float) getSampleRate() : 44100.0f;
    const auto& v = voicings[amp];
    tight.set       (Coeffs::makeHighPass   (sr, v.tightHz, 0.707f));
    preEmphasis.set (Coeffs::makePeakFilter (sr, v.humpHz, 0.8f, dB (v.humpDb)));
    postVoicing.set (Coeffs::makePeakFilter (sr, 480.0f, 0.75f, dB (v.scoopDb)));
    postLows.set    (Coeffs::makeLowShelf   (sr, 120.0f, 0.7f, dB (v.lowsDb)));
    chunk.set       (Coeffs::makePeakFilter (sr, 270.0f, 0.8f, dB (v.chunkDb)));
    splitLp1.set    (Coeffs::makeLowPass    (sr, 150.0f, 0.707f));
    splitLp2.set    (Coeffs::makeLowPass    (sr, 150.0f, 0.707f));
    splitPost.set   (Coeffs::makeLowPass    (sr, 170.0f, 0.707f));
    dynSmooth.set   (Coeffs::makeLowPass    (sr, 40.0f, 0.707f));    // smooth the response gain
    fizz.set        (Coeffs::makeLowPass    (sr, v.fizzHz > 0.0f ? juce::jmin (v.fizzHz, 0.45f * sr) : 0.45f * sr, 0.707f));
    depth.set       (Coeffs::makePeakFilter (sr, 90.0f, 1.4f, dB (v.depthDb)));

    updateCabMic (amp, true);

    lastBass = lastMid = lastTreble = lastPresence = -1.0f;   // force the tone stack to refresh
    updateFilters (amp);
    updateCabMic (amp, false);
}

// The cab model, the mic type, where the mic points (centre of the cone = bright, edge = dark)
// and how far back it is (close = proximity bass, far = less bass, softer top, a little room)
void EmberAmpProcessor::updateCabMic (int amp, bool force)
{
    const auto sr = getSampleRate() > 0 ? (float) getSampleRate() : 44100.0f;
    const auto& k = knobs[amp];
    const float cabV = k.cabModel->load(), micV = k.mic->load(), posV = k.micPos->load(), distV = k.micDist->load();

    if (force || cabV != lastCabModel)
    {
        lastCabModel = cabV;
        const auto& c = cabs[juce::jlimit (0, numCabs - 1, juce::roundToInt (cabV))];
        cabHp.set     (Coeffs::makeHighPass   (sr, c.hpHz, c.hpQ));
        cabLowRes.set (Coeffs::makePeakFilter (sr, c.resHz, c.resQ, dB (c.resDb)));
        cabScoop.set  (Coeffs::makePeakFilter (sr, c.scoopHz, c.scoopQ, dB (c.scoopDb)));
        cabComb.set   (Coeffs::makePeakFilter (sr, c.combHz, c.combQ, dB (c.combDb)));
        cabBite.set   (Coeffs::makePeakFilter (sr, c.biteHz, c.biteQ, dB (c.biteDb)));
        cabLp1.set    (Coeffs::makeLowPass    (sr, juce::jmin (c.lp1Hz, 0.45f * sr), c.lp1Q));
        cabLp2.set    (Coeffs::makeLowPass    (sr, juce::jmin (c.lp2Hz, 0.45f * sr), c.lp2Q));
    }
    if (force || micV != lastMic)
    {
        lastMic = micV;
        switch (juce::roundToInt (micV))
        {
            case 1:  // ribbon: warm and smooth, rolled-off top
                micA.set (Coeffs::makeLowShelf  (sr, 150.0f, 0.7f, dB (2.0f)));
                micB.set (Coeffs::makeHighShelf (sr, 3500.0f, 0.6f, dB (-5.0f)));
                break;
            case 2:  // condenser: open, detailed top, a touch less low-mid
                micA.set (Coeffs::makeHighShelf  (sr, 6000.0f, 0.7f, dB (3.0f)));
                micB.set (Coeffs::makePeakFilter (sr, 220.0f, 0.8f, dB (-1.5f)));
                break;
            case 3:  // large dynamic: thicker low-mids, slightly softer fizz
                micA.set (Coeffs::makePeakFilter (sr, 250.0f, 0.9f, dB (2.0f)));
                micB.set (Coeffs::makeHighShelf  (sr, 7000.0f, 0.7f, dB (-2.0f)));
                break;
            default: // dynamic: the reference the built-in cabs were voiced with
                micA.set (Coeffs::makePeakFilter (sr, 1000.0f, 0.7f, 1.0f));
                micB.set (Coeffs::makePeakFilter (sr, 1000.0f, 0.7f, 1.0f));
                break;
        }
    }
    if (force || posV != lastMicPos)
    {
        lastMicPos = posV;
        micPosShelf.set (Coeffs::makeHighShelf (sr, 3200.0f, 0.6f, dB ((0.35f - posV) * 9.0f)));   // neutral at 35%
    }
    if (force || distV != lastMicDist)
    {
        lastMicDist = distV;
        const float far = juce::jmax (0.0f, distV - 0.15f) / 0.85f;                                  // neutral at 15% (4.5 cm)
        micDistLow.set  (Coeffs::makeLowShelf  (sr, 140.0f, 0.7f, dB ((0.15f - distV) * 7.0f)));
        micDistHigh.set (Coeffs::makeHighShelf (sr, 7000.0f, 0.7f, dB (-5.0f * far)));
        reflGain  = 0.35f * far;
        reflDelay = juce::jmin ((float) reflBuf.size() - 2.0f, (0.0006f + 0.0024f * distV) * sr);
    }
}

void EmberAmpProcessor::updateFilters (int amp)
{
    const auto sr = getSampleRate() > 0 ? (float) getSampleRate() : 44100.0f;
    const auto& k = knobs[amp];
    auto refresh = [] (float value, float& last) { if (value == last) return false; last = value; return true; };

    if (refresh (k.bass->load(), lastBass))         bass.set     (Coeffs::makeLowShelf   (sr, 110.0f,  0.7f, knobToGain (lastBass)));
    if (refresh (k.mid->load(), lastMid))           mid.set      (Coeffs::makePeakFilter (sr, 700.0f,  0.7f, knobToGain (lastMid)));
    if (refresh (k.treble->load(), lastTreble))     treble.set   (Coeffs::makeHighShelf  (sr, 2800.0f, 0.7f, knobToGain (lastTreble)));
    if (refresh (k.presence->load(), lastPresence)) presence.set (Coeffs::makePeakFilter (sr, 4500.0f, 0.9f, knobToGain (lastPresence)));
}

float EmberAmpProcessor::shape (float x, int amp) const noexcept
{
    switch (amp)
    {
        case clean:       // barely-there tube warmth
            return std::tanh (x + 0.05f) - std::tanh (0.05f);

        case crunch:      // single asymmetric stage
            return 0.6f * (std::tanh (x + 0.2f) - std::tanh (0.2f));

        case distortion:  // three cascaded stages, alternating bias: a dense, compressed wall
        {
            const float s1 = std::tanh (x + 0.3f) - std::tanh (0.3f);
            const float s2 = std::tanh (3.0f * s1 - 0.15f) + std::tanh (0.15f);
            const float s3 = std::tanh (2.0f * s2 + 0.1f) - std::tanh (0.1f);
            return 0.6f * s3;
        }

        default:          // solo: three smooth stages - singing, compressed, endless sustain
        {
            const float s1 = std::tanh (x + 0.25f) - std::tanh (0.25f);
            const float s2 = std::tanh (2.5f * s1 - 0.1f) + std::tanh (0.1f);
            return 0.6f * (std::tanh (1.6f * s2 + 0.05f) - std::tanh (0.05f));
        }
    }
}

//==============================================================================
void EmberAmpProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    midi.clear();

    const int n = buffer.getNumSamples();
    if (n == 0) return;

    const int numIn  = getTotalNumInputChannels();
    const int numOut = getTotalNumOutputChannels();

    mono.setSize (1, n, false, false, true);
    if (numIn > 0) mono.copyFrom (0, 0, buffer, 0, 0, n);
    else           mono.clear();

    auto* x = mono.getWritePointer (0);
    juce::dsp::AudioBlock<float> monoBlock (mono);

    // --- Amp switching: fade out, swap voicing/cab, fade back in (no clicks) ---
    {
        const int want = getSelectedAmp();
        if (activeAmp < 0) { activeAmp = want; setAmpVoicing (want); }
        if (want != activeAmp && switchGain.getCurrentValue() < 1.0e-3f)
        {
            activeAmp = want;
            for (auto* b : { &tight, &preEmphasis, &postVoicing, &postLows, &chunk, &fizz, &splitLp1, &splitLp2, &splitPost, &dynSmooth, &bass, &mid, &treble, &presence,
                             &cabHp, &cabLowRes, &cabScoop, &cabComb, &cabBite, &cabLp1, &cabLp2, &depth,
                             &micA, &micB, &micPosShelf, &micDistLow, &micDistHigh })
                b->reset();
            std::fill (reflBuf.begin(), reflBuf.end(), 0.0f);
            setAmpVoicing (activeAmp);
            const auto& v = voicings[activeAmp];
            driveGain.setCurrentAndTargetValue (dB (juce::jmap (knobs[activeAmp].gain->load(), v.gainMinDb, v.gainMaxDb)));
            ampVolume.setCurrentAndTargetValue (dB (knobs[activeAmp].volume->load()));
            irConvolution[(size_t) activeAmp].reset();
        }
        switchGain.setTargetValue (want == activeAmp ? 1.0f : 0.0f);
    }
    const int amp = activeAmp;
    const auto& v = voicings[amp];
    const auto& k = knobs[amp];

    // --- Input gain, metering, tuner feed, then input mute ---
    // The meter and tuner see the input even while muted, so you can tune silently.
    inGain.setTargetValue (dB (pInput->load()));
    muteGain.setTargetValue (pMute->load() > 0.5f ? 0.0f : 1.0f);
    float inPk = 0.0f;
    int wp = tunerWritePos.load (std::memory_order_relaxed);
    static const float inputPad = dB (-8.0f);   // tame hot interfaces/pickups
    for (int i = 0; i < n; ++i)
    {
        // playing-level tracker: peak of each 100 ms window of your input (after the Input knob)
        const float raw = std::abs (x[i] * inGain.getCurrentValue());
        trackPeak = juce::jmax (trackPeak, raw);
        if (++trackSamples >= (int) (0.1 * getSampleRate()))
        {
            const float pkDb = juce::Decibels::gainToDecibels (trackPeak, -120.0f);
            if (pkDb > -50.0f && pkDb < 0.0f)   // only while you're actually playing
            {
                playPeakDbSum.store (playPeakDbSum.load() + pkDb);
                playPeakCount.store (playPeakCount.load() + 1);
            }
            trackPeak = 0.0f; trackSamples = 0;
        }

        x[i] *= inGain.getNextValue() * inputPad;
        inPk = juce::jmax (inPk, std::abs (x[i]));
        tunerBuffer[(size_t) wp] = x[i];
        if (++wp >= tunerBufferSize) wp = 0;
        x[i] *= muteGain.getNextValue();
    }
    tunerWritePos.store (wp, std::memory_order_release);

    // Dynamics: the amp's output follows your playing level (picking strength / guitar volume knob).
    // Tracks average loudness (not waveform peaks), bends softly instead of hitting hard limits,
    // is smoothed in dB, and stops reacting below a floor so note tails fade naturally.
    if ((int) dynGain.size() < n) dynGain.resize ((size_t) n);
    {
        const float k = pDynamics->load() > 0.5f ? voicings[amp].dynK : 0.0f;   // Dynamics switch (off = normal)
        constexpr float refDb = -35.0f, floorDb = -55.0f, rangeDb = 12.0f;   // centred on your measured playing level
        for (int i = 0; i < n; ++i)
        {
            const float sq = x[i] * x[i];
            dynEnv += (sq > dynEnv ? dynAtt : dynRel) * (sq - dynEnv);             // mean-square follower
            const float envDb = juce::jmax (floorDb, 10.0f * std::log10 (dynEnv + 1.0e-12f));
            const float target = k > 0.0f ? rangeDb * std::tanh (k * (envDb - refDb) / rangeDb) : 0.0f;
            dynGainDb += (target > dynGainDb ? dynGainUp : dynGainDown) * (target - dynGainDb);
            dynGain[(size_t) i] = dB (dynGainDb);
        }
    }
    inputPeak = juce::jmax (inPk, inputPeak.load());

    // ===== Pedals before the amp: Gate -> Compressor -> Pitch -> (Overdrive, inside the preamp) =====

    // --- Noise gate ---
    if (pGateOn->load() > 0.5f)
        gate.process (x, n, pGate->load());

    // --- Compressor: squeezes the decaying part of each note back up so it rings longer ---
    const float sus = pCompOn->load() > 0.5f ? pSustain->load() : 0.0f;
    if (sus > 0.001f)
    {
        const float threshold = -12.0f - 30.0f * sus;      // -12 .. -42 dB
        const float ratio     = 2.0f + 8.0f * sus;          // 2:1 .. 10:1
        sustainComp.setThreshold (threshold);
        sustainComp.setRatio (ratio);
        sustainComp.process (juce::dsp::ProcessContextReplacing<float> (monoBlock));
        sustainMakeup.setTargetValue (dB (juce::jmax (0.0f, (-14.0f - threshold) * (1.0f - 1.0f / ratio) * 0.8f)));
    }
    else
    {
        sustainMakeup.setTargetValue (1.0f);
    }
    if (sus > 0.001f || sustainMakeup.isSmoothing())
        for (int i = 0; i < n; ++i)
            x[i] *= sustainMakeup.getNextValue();

    // --- Pitch shifter (before the amp, so drop-tuned riffs distort like a real low string) ---
    {
        const int semis = juce::roundToInt (pPitch->load());
        const bool on = pPitchOn->load() > 0.5f && semis != 0;
        pitchWet.setTargetValue (on ? pPitchMix->load() : 0.0f);
        pitchRatio.setTargetValue (std::pow (2.0f, (float) semis / 12.0f));

        if (pitchWet.isSmoothing() || pitchWet.getTargetValue() > 0.0f)
        {
            for (int i = 0; i < n; ++i)
            {
                const float wet = pitchWet.getNextValue();
                const float shifted = pitchShifter.process (x[i], pitchRatio.getNextValue());
                x[i] += wet * (shifted - x[i]);
            }
        }
        else
        {
            pitchRatio.skip (n);
        }
    }

    // ===== Amp =====
    // the amp's own Sustain knob: compression in front of its preamp so notes ring longer
    {
        const float as = k.sustain->load();
        if (as > 0.001f)
        {
            const float threshold = -12.0f - 16.0f * as;   // -12 .. -28 dB
            const float ratio     = 1.2f + 1.8f * as;      // 1.2:1 .. 3:1
            ampComp.setThreshold (threshold);
            ampComp.setRatio (ratio);
            ampComp.process (juce::dsp::ProcessContextReplacing<float> (monoBlock));
            ampCompMakeup.setTargetValue (dB (juce::jmax (0.0f, (-16.0f - threshold) * (1.0f - 1.0f / ratio) * 0.8f)));
        }
        else
        {
            ampCompMakeup.setTargetValue (1.0f);
        }
        if (as > 0.001f || ampCompMakeup.isSmoothing())
            for (int i = 0; i < n; ++i)
                x[i] *= ampCompMakeup.getNextValue();
    }

    updateFilters (amp);
    updateCabMic (amp, false);
    if (v.splitMix > 0.0f)
    {
        if ((int) splitBuf.size() < n) splitBuf.resize ((size_t) n);
        for (int i = 0; i < n; ++i)
            splitBuf[(size_t) i] = splitLp2.process (splitLp1.process (x[i]));
    }
    for (int i = 0; i < n; ++i)
        x[i] = preEmphasis.process (tight.process (x[i]));

    driveGain.setTargetValue (dB (juce::jmap (k.gain->load(), v.gainMinDb, v.gainMaxDb)));

    auto up = oversampler.processSamplesUp (monoBlock);
    auto* u = up.getChannelPointer (0);
    boostAmount.setTargetValue (pOdOn->load() > 0.5f ? pBoost->load() : 0.0f);
    ampDrive.setTargetValue (k.drive != nullptr ? k.drive->load() : 0.0f);
    const bool boostActive = boostAmount.isSmoothing() || boostAmount.getTargetValue() > 0.0005f;
    dsWet.setTargetValue (pDsOn->load() > 0.5f ? 1.0f : 0.0f);
    dsDrive.setTargetValue (pDsDist->load()); dsTone.setTargetValue (pDsTone->load()); dsLevel.setTargetValue (pDsLevel->load());
    const bool dsActive = dsWet.isSmoothing() || dsWet.getTargetValue() > 0.0f;
    for (size_t i = 0; i < up.getNumSamples(); ++i)
    {
        float s = u[i];

        // DS-style distortion pedal: lows trimmed, lots of gain into hard (diode) clipping,
        // then the classic tone control that blends a dark and a bright filter (scooped mids).
        if (dsActive)
        {
            const float w = dsWet.getNextValue(), dd = dsDrive.getNextValue(), tn = dsTone.getNextValue(), lv = dsLevel.getNextValue();
            dsInLp += dsInCoef * (s - dsInLp);
            const float pre = (s - 0.85f * dsInLp) * dB (18.0f + 30.0f * dd);   // 18..48 dB
            const float a = pre + 0.06f;                                         // slight asymmetry: a bit of even harmonics
            const float a2 = a * a;
            const float clipped = a / std::sqrt (std::sqrt (1.0f + a2 * a2)) - 0.06f;   // hard knee, like a diode pair
            dsSmooth += dsSmoothCoef * (clipped - dsSmooth);
            dsLp += dsLpCoef * (dsSmooth - dsLp);
            dsHpLp += dsHpCoef * (dsSmooth - dsHpLp);
            const float hi = dsSmooth - dsHpLp;
            const float toned = (1.0f - tn) * dsLp * 1.4f + tn * hi * 1.8f;
            const float out = toned * (0.05f + 0.55f * lv);
            s += w * (out - s);
        }

        // Overdrive pedal: cuts lows (tight), adds its own soft clipping and slams the preamp harder.
        if (boostActive)
        {
            const float d = boostAmount.getNextValue();
            boostLp += boostCoef * (s - boostLp);
            const float tightIn = s - 0.8f * d * boostLp;
            const float pg = dB (6.0f + 24.0f * d);           // 6..30 dB of pedal gain
            s += d * (0.35f * std::tanh (pg * tightIn) - s);
        }

        // the amp's own Drive knob (Crunch, Distortion, Solo): a built-in tight boost in front of its preamp
        if (k.drive != nullptr || ampDrive.isSmoothing())
        {
            const float d = ampDrive.getNextValue();
            if (d > 0.0005f)
            {
                ampDriveLp += boostCoef * (s - ampDriveLp);
                const float tightIn = s - 0.8f * d * ampDriveLp;
                const float pg = dB (6.0f + 24.0f * d);
                s += d * (0.35f * std::tanh (pg * tightIn) - s);
                if (amp != distortion)
                    s *= dB (12.0f * d);   // Crunch / Solo: Drive also pushes the preamp harder (up to +12 dB)
            }
        }

        u[i] = shape (s * driveGain.getNextValue(), amp);
    }
    oversampler.processSamplesDown (monoBlock);

    const float makeup = dB (v.makeupDb);
    for (int i = 0; i < n; ++i)
        x[i] = chunk.process (postLows.process (postVoicing.process (x[i]))) * makeup;

    // bass split: add the lows back (lightly saturated) so the chug has weight without mud
    if (v.splitMix > 0.0f)
    {
        const float g = dB (v.splitDriveDb);
        for (int i = 0; i < n; ++i)   // gentle saturation, then filtered again so no new mid harmonics get in
            x[i] += splitPost.process (v.splitMix * std::tanh (g * splitBuf[(size_t) i]));
    }

    // tone stack
    for (int i = 0; i < n; ++i)
        x[i] = presence.process (treble.process (mid.process (bass.process (x[i]))));

    // built-in leveler (Clean, Crunch): keeps them as loud as the gain amps even at low pickup levels
    if (v.levelRatio > 0.0f)
    {
        leveler.setThreshold (v.levelThreshDb);
        leveler.setRatio (v.levelRatio);
        leveler.process (juce::dsp::ProcessContextReplacing<float> (monoBlock));
    }
    if (v.levelMakeupDb != 0.0f)
    {
        const float lm = dB (v.levelMakeupDb);
        for (int i = 0; i < n; ++i) x[i] *= lm;
    }

    // power-amp saturation (Distortion, Solo)
    if (v.powerDrive > 0.0f)
        for (int i = 0; i < n; ++i)
            x[i] = 0.75f * std::tanh (v.powerDrive * x[i]);

    // de-fizz after all the saturation, just before the cab
    if (v.fizzHz > 0.0f)
        for (int i = 0; i < n; ++i)
            x[i] = fizz.process (x[i]);

    // ===== This amp's cab: its IR if one is loaded, otherwise its built-in cab =====
    if (k.cab->load() > 0.5f)
    {
        if (irActive[(size_t) amp].load())
            irConvolution[(size_t) amp].process (juce::dsp::ProcessContextReplacing<float> (monoBlock));
        else
        {
            const int rbMask = (int) reflBuf.size() - 1;
            const int rd = juce::jlimit (1, rbMask - 1, juce::roundToInt (reflDelay));
            for (int i = 0; i < n; ++i)
            {
                float y = cabLp2.process (cabLp1.process (cabBite.process (cabComb.process (cabScoop.process (cabLowRes.process (cabHp.process (x[i])))))));
                y = micDistHigh.process (micDistLow.process (micPosShelf.process (micB.process (micA.process (y)))));
                reflBuf[(size_t) reflPos] = y;
                if (reflGain > 0.0f)
                    y += reflGain * reflBuf[(size_t) ((reflPos - rd) & rbMask)];
                reflPos = (reflPos + 1) & rbMask;
                x[i] = y;
            }
        }
    }

    // low-end resonance, amp volume, and the switching fade
    ampVolume.setTargetValue (dB (k.volume->load()));
    for (int i = 0; i < n; ++i)
        x[i] = depth.process (x[i]) * ampVolume.getNextValue() * switchGain.getNextValue() * dynGain[(size_t) i];

    // --- Mono -> output channels, with fixed headroom so the amp sits well below 0 dBFS ---
    static const float headroom = dB (-14.0f);
    for (int c = 0; c < numOut; ++c)
    {
        buffer.copyFrom (c, 0, mono, 0, 0, n);
        buffer.applyGain (c, 0, n, headroom);
    }

    // ===== Pedals after the amp: Chorus -> Delay -> Reverb =====
    if (numOut > 0)
    {
        chorusWet.setTargetValue (pChorusOn->load() > 0.5f ? pCMix->load() : 0.0f);
        const float cm = chorusWet.getNextValue();
        chorusWet.skip (n - 1);
        if (cm > 0.0001f || chorusWet.isSmoothing())
            chorus.process (buffer, n, pCRate->load(), pCDepth->load(), cm);

        delayWet.setTargetValue (pDelayOn->load() > 0.5f ? pDMix->load() : 0.0f);
        const float dm = delayWet.getNextValue();
        delayWet.skip (n - 1);
        if (dm > 0.0001f || delayWet.isSmoothing())
            delay.process (buffer, n, pDTime->load(), pDFb->load(), dm);

        reverbWet.setTargetValue (pRevOn->load() > 0.5f ? pRMix->load() : 0.0f);
        const float rm = reverbWet.getNextValue();
        reverbWet.skip (n - 1);
        juce::Reverb::Parameters rp;
        rp.roomSize = 0.3f + 0.68f * pRSize->load();
        rp.damping  = 0.45f;
        rp.width    = 1.0f;
        rp.wetLevel = rm * 0.3f;   // juce::Reverb scales wet by 3x -> up to ~0.9 at 100%
        rp.dryLevel = 0.5f;        // juce::Reverb scales dry by 2x -> dry stays exactly at unity
        reverb.setParameters (rp);
        auto outBlock = juce::dsp::AudioBlock<float> (buffer).getSubsetChannelBlock (0, (size_t) juce::jmin (numOut, 2));
        reverb.process (juce::dsp::ProcessContextReplacing<float> (outBlock));
    }

    // --- Output + safety soft limiter (transparent below -1 dBFS, can never exceed 0 dBFS) ---
    outGain.setTargetValue (dB (pMaster->load()));
    static constexpr float knee = 0.891f; // -1 dBFS
    auto safety = [] (float s) noexcept
    {
        const float a = std::abs (s);
        if (a <= knee) return s;
        return std::copysign (knee + (1.0f - knee) * std::tanh ((a - knee) / (1.0f - knee)), s);
    };

    float outPk = 0.0f;
    for (int i = 0; i < n; ++i)
    {
        const float g = outGain.getNextValue();
        for (int c = 0; c < numOut; ++c)
        {
            auto* o = buffer.getWritePointer (c);
            o[i] = safety (o[i] * g);
            outPk = juce::jmax (outPk, std::abs (o[i]));
        }
    }
    outputPeak = juce::jmax (outPk, outputPeak.load());

    // copy for the Stream output (dropped if the stream side has fallen behind)
    if (streamTapOn.load (std::memory_order_relaxed) && numOut > 0)
    {
        const auto* L = buffer.getReadPointer (0);
        const auto* R = buffer.getReadPointer (numOut > 1 ? 1 : 0);
        const auto w = streamFifo.write (juce::jmin (n, streamFifo.getFreeSpace()));
        if (w.blockSize1 > 0) { std::copy (L, L + w.blockSize1, streamBufL.data() + w.startIndex1); std::copy (R, R + w.blockSize1, streamBufR.data() + w.startIndex1); }
        if (w.blockSize2 > 0) { std::copy (L + w.blockSize1, L + w.blockSize1 + w.blockSize2, streamBufL.data() + w.startIndex2);
                                std::copy (R + w.blockSize1, R + w.blockSize1 + w.blockSize2, streamBufR.data() + w.startIndex2); }
    }
}

void EmberAmpProcessor::copyRecentInput (float* dest, int numSamples) const
{
    numSamples = juce::jmin (numSamples, tunerBufferSize);
    int start = tunerWritePos.load (std::memory_order_acquire) - numSamples;
    if (start < 0) start += tunerBufferSize;
    for (int i = 0; i < numSamples; ++i)
        dest[i] = tunerBuffer[(size_t) ((start + i) % tunerBufferSize)];
}

//==============================================================================
const juce::String EmberAmpProcessor::getProgramName (int index)
{
    return juce::isPositiveAndBelow (index, getNumPrograms()) ? juce::String (factory[index].name) : juce::String();
}

void EmberAmpProcessor::setCurrentProgram (int index)
{
    if (! juce::isPositiveAndBelow (index, getNumPrograms())) return;
    currentProgram = index;
    currentUserPreset.clear();

    const auto& f = factory[index];
    auto set = [this] (const juce::String& id, float value)
    {
        if (auto* param = apvts.getParameter (id))
            param->setValueNotifyingHost (param->convertTo0to1 (value));
    };

    set ("amp", (float) f.amp);
    set (ampParamID (f.amp, "gain"), f.gain);         set (ampParamID (f.amp, "bass"), f.bass);
    set (ampParamID (f.amp, "mid"), f.mid);           set (ampParamID (f.amp, "treble"), f.treble);
    set (ampParamID (f.amp, "presence"), f.presence); set (ampParamID (f.amp, "volume"), f.volume);
    set (ampParamID (f.amp, "cab"), 1.0f);
    {
        const auto& cs = presetCab[index];
        set (ampParamID (f.amp, "cabModel"), (float) cs.cab);
        set (ampParamID (f.amp, "mic"), (float) cs.mic);
        set (ampParamID (f.amp, "micPos"), cs.pos);
        set (ampParamID (f.amp, "micDist"), cs.dist);
    }
    set (ampParamID (f.amp, "sustain"), f.ampSustain);
    if (f.amp != clean) set (ampParamID (f.amp, "drive"), f.ampDrive);

    set ("gateOn", 1.0f);                 set ("gate", ampGateDb[f.amp]);
    set ("compOn", f.compOn ? 1.0f : 0.0f);   set ("sustain", f.sustain);
    set ("odOn", f.odOn ? 1.0f : 0.0f);       set ("boost", f.boost);
    applyingPreset = true;
    if (f.pitchOn)   // a preset built on the pitch shifter brings its own setting
    {
        set ("pitchOn", 1.0f); set ("pitch", (float) f.pitch); set ("pitchMix", f.pitchMix);
    }
    else             // every other preset keeps Transpose the way you left it
    {
        set ("pitchOn", userPitchOn.load() ? 1.0f : 0.0f); set ("pitch", (float) userPitch.load()); set ("pitchMix", userPitchMix.load());
    }
    onPitchPreset = f.pitchOn;
    applyingPreset = false;
    set ("delayOn", f.delayOn ? 1.0f : 0.0f); set ("delayTime", f.delayTime);   set ("delayFeedback", f.delayFeedback); set ("delayMix", f.delayMix);
    set ("reverbOn", f.revOn ? 1.0f : 0.0f);  set ("reverbSize", f.revSize);    set ("reverbMix", f.revMix);
    set ("chorusOn", f.chorusOn ? 1.0f : 0.0f);
    set ("dsOn", f.dsOn ? 1.0f : 0.0f);
    if (f.dsOn) { set ("dsDist", f.dsDist); set ("dsTone", f.dsTone); set ("dsLevel", f.dsLevel); }
    if (f.chorusOn) { set ("chorusRate", f.chorusRate); set ("chorusDepth", f.chorusDepth); set ("chorusMix", f.chorusMix); }
}

//==============================================================================
juce::File EmberAmpProcessor::getUserPresetFolder()
{
   #if JUCE_MAC
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
             .getChildFile ("Application Support/Ember Amp/Presets");
   #else
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)   // Windows: %APPDATA%
             .getChildFile ("Ember Amp/Presets");
   #endif
}

juce::Array<juce::File> EmberAmpProcessor::getUserPresets() const
{
    auto files = getUserPresetFolder().findChildFiles (juce::File::findFiles, false, "*.emberpreset");
    std::sort (files.begin(), files.end(), [] (const juce::File& a, const juce::File& b)
               { return a.getFileNameWithoutExtension().compareNatural (b.getFileNameWithoutExtension()) < 0; });
    return files;
}

bool EmberAmpProcessor::saveUserPreset (const juce::String& rawName)
{
    const auto name = juce::File::createLegalFileName (rawName.trim());
    if (name.isEmpty()) return false;

    auto folder = getUserPresetFolder();
    if (! folder.createDirectory()) return false;

    auto state = apvts.copyState();
    for (int a = 0; a < numAmps; ++a)
        state.setProperty ("ir" + juce::String (a), irActive[(size_t) a].load() ? irFile[(size_t) a].getFullPathName() : juce::String(), nullptr);
    state.setProperty ("presetName", name, nullptr);

    auto xml = state.createXml();
    if (xml == nullptr || ! xml->writeTo (folder.getChildFile (name + ".emberpreset")))
        return false;

    currentUserPreset = name;
    return true;
}

bool EmberAmpProcessor::loadUserPreset (const juce::File& file)
{
    auto xml = juce::XmlDocument::parse (file);
    if (xml == nullptr || ! xml->hasTagName (apvts.state.getType()))
        return false;

    const auto state = juce::ValueTree::fromXml (*xml);
    onPitchPreset = false;
    for (auto* p : getParameters())
        if (auto* param = dynamic_cast<juce::RangedAudioParameter*> (p))
        {
            if (param->getParameterID() == "inputMute") continue;   // presets never mute/unmute you
            const auto child = state.getChildWithProperty ("id", param->getParameterID());
            if (child.isValid())
                param->setValueNotifyingHost (param->convertTo0to1 ((float) child.getProperty ("value")));
        }

    restoreIRsFrom (state);
    currentUserPreset = file.getFileNameWithoutExtension();
    return true;
}

void EmberAmpProcessor::restoreIRsFrom (const juce::ValueTree& state)
{
    for (int a = 0; a < numAmps; ++a)
    {
        const juce::String path = state.getProperty ("ir" + juce::String (a), {}).toString();
        if (path.isNotEmpty() && juce::File::isAbsolutePath (path) && juce::File (path).existsAsFile())
        {
            const bool cabWasOn = knobs[a].cab->load() > 0.5f;
            loadIR (a, juce::File (path));
            if (! cabWasOn)
                if (auto* cab = apvts.getParameter (ampParamID (a, "cab")))
                    cab->setValueNotifyingHost (0.0f);
        }
        else
        {
            clearIR (a);
        }
    }
}

bool EmberAmpProcessor::loadIR (int amp, const juce::File& file)
{
    if (! juce::isPositiveAndBelow (amp, (int) numAmps) || ! file.existsAsFile() || ! isIRFile (file))
        return false;

    // Loads on a background thread and swaps in glitch-free.
    irConvolution[(size_t) amp].loadImpulseResponse (file, juce::dsp::Convolution::Stereo::no,
                                                     juce::dsp::Convolution::Trim::yes, 0,
                                                     juce::dsp::Convolution::Normalise::yes);
    irFile[(size_t) amp] = file;
    irActive[(size_t) amp] = true;

    if (auto* cab = apvts.getParameter (ampParamID (amp, "cab")))
        cab->setValueNotifyingHost (1.0f);
    return true;
}

void EmberAmpProcessor::clearIR (int amp)
{
    if (! juce::isPositiveAndBelow (amp, (int) numAmps)) return;
    irActive[(size_t) amp] = false;
    irFile[(size_t) amp] = juce::File();
}

void EmberAmpProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    state.setProperty ("program", currentProgram, nullptr);
    state.setProperty ("presetsPerAmp", presetsPerAmp, nullptr);
    state.setProperty ("userPitchOn", userPitchOn.load(), nullptr);
    state.setProperty ("userPitch", userPitch.load(), nullptr);
    state.setProperty ("userPitchMix", userPitchMix.load(), nullptr);
    state.setProperty ("userPreset", currentUserPreset, nullptr);
    for (int a = 0; a < numAmps; ++a)
        state.setProperty ("ir" + juce::String (a), irActive[(size_t) a].load() ? irFile[(size_t) a].getFullPathName() : juce::String(), nullptr);
    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void EmberAmpProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
        {
            auto state = juce::ValueTree::fromXml (*xml);

            // State saved by the old single-amp version has none of the per-amp knobs. Loading it
            // would reset every amp to blank defaults, so keep the factory sounds instead.
            if (! state.getChildWithProperty ("id", ampParamID (0, "gain")).isValid())
                return;

            int prog = (int) state.getProperty ("program", 0);
            const int savedPer = (int) state.getProperty ("presetsPerAmp", 4);   // older versions had 4 presets per amp
            if (savedPer != presetsPerAmp && savedPer > 0)
                prog = (prog / savedPer) * presetsPerAmp + prog % savedPer;
            currentProgram = juce::jlimit (0, getNumPrograms() - 1, prog);
            currentUserPreset = state.getProperty ("userPreset", {}).toString();
            applyingPreset = true;
            apvts.replaceState (state);
            applyingPreset = false;
            userPitchOn  = (bool) state.getProperty ("userPitchOn", false);   // older saves: Transpose starts off
            userPitch    = (int) state.getProperty ("userPitch", -1);
            userPitchMix = (float) state.getProperty ("userPitchMix", 1.0f);
            onPitchPreset = currentUserPreset.isEmpty() && factory[currentProgram].pitchOn;
            restoreIRsFrom (state);
        }
}

juce::AudioProcessorEditor* EmberAmpProcessor::createEditor() { return new EmberAmpEditor (*this); }

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new EmberAmpProcessor(); }
