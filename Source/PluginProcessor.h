#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>
#include <array>
#include <atomic>
#include <vector>

//==============================================================================
/** A single biquad whose coefficients can be updated without allocating. */
struct Biquad
{
    Biquad() { filter.coefficients = new juce::dsp::IIR::Coefficients<float> (1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f); }
    void set (const std::array<float, 6>& c) { *filter.coefficients = c; }
    float process (float x) noexcept         { return filter.processSample (x); }
    void reset()                             { filter.reset(); }

    juce::dsp::IIR::Filter<float> filter;
};

//==============================================================================
/** Splice-based pitch shifter (SOLA). One read head plays at the shifted rate almost all the time;
    every ~50 ms it splices to a fresh head over a 12 ms crossfade. The new head's start is chosen
    (within one pitch period) by normalised cross-correlation so the splice lands in phase.
    Mono, ~10-25 ms wet latency. */
class PitchShifter
{
public:
    void prepare (double sampleRate);
    void reset();
    float process (float x, float ratio) noexcept;

private:
    float read (float delay) const noexcept;
    float readInt (int delay) const noexcept   { return buf[(size_t) ((writePos - delay) & mask)]; }
    float findAligned (float nominal, float current) const noexcept;

    std::vector<float> buf;
    int mask = 0, writePos = 0;
    int search = 400, corrLen = 480, xfadeLen = 576, spliceEvery = 2400;
    float active = 1.0f, incoming = 1.0f;
    int fadePos = -1;
};

//==============================================================================
/** Noise gate built for high gain: opens instantly on your pick attack, holds 40 ms, and only
    closes once the signal drops 4 dB below the threshold (so decaying notes don't flutter it),
    then fades fully to silence. The detector ignores mains hum below ~70 Hz. */
class HardGate
{
public:
    void prepare (double sampleRate);
    void reset();
    void process (float* x, int n, float thresholdDb) noexcept;

private:
    float envRel = 0.0f, gAtt = 0.0f, gRel = 0.0f, hpA = 0.0f;
    int holdLen = 0, hold = 0;
    float env = 0.0f, gain = 0.0f, hpY = 0.0f, hpX = 0.0f;
    bool open = false;
};

//==============================================================================
/** Stereo ping-pong delay with a darkened feedback path. */
class PingPongDelay
{
public:
    void prepare (double sampleRate);
    void reset();
    void process (juce::AudioBuffer<float>& buffer, int numSamples, float timeMs, float feedback, float mix);

private:
    double sr = 44100.0;
    std::vector<float> lines[2];
    int size = 1, writePos = 0;
    float lp[2] {};
    juce::SmoothedValue<float> delaySamples;
};

//==============================================================================
/** Stereo chorus: two modulated delay lines (left / right LFOs 90 degrees apart) for a wide shimmer. */
class StereoChorus
{
public:
    void prepare (double sampleRate);
    void reset();
    void process (juce::AudioBuffer<float>& buffer, int numSamples, float rateHz, float depth, float mix);

private:
    double sr = 44100.0;
    std::vector<float> line;
    int size = 1, writePos = 0;
    float phase = 0.0f;
    float read (float delaySamples) const noexcept;
};

//==============================================================================
class EmberAmpProcessor : public juce::AudioProcessor,
                          private juce::AudioProcessorValueTreeState::Listener
{
public:
    // The four amps. Each has its own knobs, its own built-in cab, its own IR slot and 4 presets.
    enum Amp { clean = 0, crunch, distortion, solo, numAmps };
    static constexpr int presetsPerAmp = 5;

    static juce::String ampName (int amp);
    static juce::String ampTagline (int amp);
    static juce::String cabName (int amp);              // the amp's own (default) cab
    static constexpr int numCabs = 6, numMics = 4;
    static juce::StringArray cabNames();                // every cab, selectable on every amp
    static juce::StringArray micNames();
    static int defaultCab (int amp);
    static int speakersInCab (int cab)  { return cab == 0 ? 1 : cab <= 2 ? 2 : 4; }
    static juce::String ampParamID (int amp, const char* knob)  { return "a" + juce::String (amp) + "_" + knob; }

    EmberAmpProcessor();
    ~EmberAmpProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override                   { return true; }

    const juce::String getName() const override       { return JucePlugin_Name; }
    bool acceptsMidi() const override                 { return false; }
    bool producesMidi() const override                { return false; }
    bool isMidiEffect() const override                { return false; }
    double getTailLengthSeconds() const override      { return 4.0; }

    // 20 factory presets: index = amp * 5 + n
    int getNumPrograms() override                     { return numAmps * presetsPerAmp; }
    int getCurrentProgram() override                  { return currentProgram; }
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int, const juce::String&) override {}
    static int programAmp (int index)                 { return index / presetsPerAmp; }

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    juce::AudioProcessorValueTreeState apvts;

    int getSelectedAmp() const                        { return juce::jlimit (0, numAmps - 1, (int) pAmp->load()); }
    void selectAmp (int amp);

    // --- Tuner / metering (read from the GUI thread) ---
    static constexpr int tunerBufferSize = 8192;
    void copyRecentInput (float* dest, int numSamples) const;
    double getSampleRateForTuner() const              { return currentSampleRate.load(); }
    std::atomic<float> inputPeak { 0.0f }, outputPeak { 0.0f };

    // Playing-level tracker (for volume calibration): average of the 100 ms input peaks while you
    // play (ignores silence), measured before the amp's input pad. Read from the GUI thread.
    std::atomic<double> playPeakDbSum { 0.0 };
    std::atomic<int> playPeakCount { 0 };

    // --- Stream tap: a copy of the finished sound for a second output (the app's "Stream" to Discord).
    //     The audio thread writes, the stream device's thread reads. Off = no cost. ---
    static constexpr int streamFifoSize = 1 << 15;
    std::atomic<bool> streamTapOn { false };
    juce::AbstractFifo streamFifo { streamFifoSize };
    std::vector<float> streamBufL = std::vector<float> ((size_t) streamFifoSize), streamBufR = std::vector<float> ((size_t) streamFifoSize);

    // --- User presets (call from the message thread) ---
    static juce::File getUserPresetFolder();
    juce::Array<juce::File> getUserPresets() const;
    bool saveUserPreset (const juce::String& name);          // overwrites a preset with the same name
    bool loadUserPreset (const juce::File& file);
    juce::String getCurrentUserPreset() const               { return currentUserPreset; }
    void clearCurrentUserPreset()                            { currentUserPreset.clear(); }

    // --- Impulse responses, one slot per amp (call from the message thread) ---
    bool loadIR (int amp, const juce::File& file);
    void clearIR (int amp);
    juce::File getIRFile (int amp) const              { return irFile[(size_t) amp]; }
    bool hasIR (int amp) const                        { return irActive[(size_t) amp].load(); }
    static bool isIRFile (const juce::File& f)        { return f.hasFileExtension ("wav;aif;aiff;flac"); }

private:
    struct AmpKnobs { std::atomic<float>* gain; std::atomic<float>* bass; std::atomic<float>* mid;
                      std::atomic<float>* treble; std::atomic<float>* presence; std::atomic<float>* volume;
                      std::atomic<float>* cab; std::atomic<float>* sustain;
                      std::atomic<float>* drive;     // drive: Crunch, Distortion, Solo (nullptr on Clean)
                      std::atomic<float>* cabModel; std::atomic<float>* mic; std::atomic<float>* micPos; std::atomic<float>* micDist;
                      std::atomic<float>* micSpeaker; };

    void updateFilters (int amp);
    void setAmpVoicing (int amp);
    float shape (float x, int amp) const noexcept;
    void restoreIRsFrom (const juce::ValueTree& state);

    // Transpose follows YOU across presets: your last on/off, shift and mix are re-applied by every
    // factory preset, except the ones built on the pitch shifter (Drop C Chug, Octave Doom, Harmony Fifth).
    void parameterChanged (const juce::String& id, float value) override;
    std::atomic<bool> userPitchOn { false }, applyingPreset { false }, onPitchPreset { false };
    std::atomic<int> userPitch { -1 };
    std::atomic<float> userPitchMix { 1.0f };

    std::atomic<float>* pInput;  std::atomic<float>* pMaster; std::atomic<float>* pMute; std::atomic<float>* pAmp;
    std::atomic<float>* pDynamics;
    std::atomic<float>* pGateOn; std::atomic<float>* pGate;
    std::atomic<float>* pCompOn; std::atomic<float>* pSustain;
    std::atomic<float>* pOdOn;   std::atomic<float>* pBoost;
    std::atomic<float>* pDsOn;   std::atomic<float>* pDsDist; std::atomic<float>* pDsTone; std::atomic<float>* pDsLevel;
    std::atomic<float>* pPitchOn; std::atomic<float>* pPitch; std::atomic<float>* pPitchMix;
    std::atomic<float>* pDelayOn; std::atomic<float>* pDTime; std::atomic<float>* pDFb; std::atomic<float>* pDMix;
    std::atomic<float>* pEqOn; std::atomic<float>* pEqLowCut; std::atomic<float>* pEqHighCut;
    std::atomic<float>* pDoublerOn; std::atomic<float>* pDoublerSpread; std::atomic<float>* pMonoOut;
    std::atomic<float>* pEqFreq[4]; std::atomic<float>* pEqGain[4];
    std::atomic<float>* pChorusOn; std::atomic<float>* pCRate; std::atomic<float>* pCDepth; std::atomic<float>* pCMix;
    std::atomic<float>* pRevOn;  std::atomic<float>* pRSize; std::atomic<float>* pRMix;
    AmpKnobs knobs[numAmps];

    std::atomic<double> currentSampleRate { 44100.0 };
    int currentProgram = 0;
    juce::String currentUserPreset;   // name of the loaded user preset, empty = factory preset

    juce::AudioBuffer<float> mono;
    HardGate gate;
    juce::dsp::Compressor<float> sustainComp, leveler, ampComp;
    juce::dsp::Oversampling<float> oversampler { 1, 2, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true };
    juce::SmoothedValue<float> inGain, outGain, ampVolume, driveGain, muteGain, boostAmount, sustainMakeup, switchGain;
    float boostLp = 0.0f, boostCoef = 0.0f, ampDriveLp = 0.0f;
    // DS-style distortion pedal (runs oversampled, in front of the preamp)
    juce::SmoothedValue<float> dsWet, dsDrive, dsTone, dsLevel;
    float dsInLp = 0.0f, dsLp = 0.0f, dsHpLp = 0.0f, dsSmooth = 0.0f;
    float dsInCoef = 0.0f, dsLpCoef = 0.0f, dsHpCoef = 0.0f, dsSmoothCoef = 0.0f;
    juce::SmoothedValue<float> ampCompMakeup, ampDrive;

    // amp currently heard (switching fades out, swaps, fades back in)
    int activeAmp = -1;

    Biquad tight, preEmphasis;                    // pre-distortion shaping
    Biquad postVoicing, postLows, chunk, fizz;    // post-preamp voicing
    Biquad splitLp1, splitLp2, splitPost;         // bass split (lows that skip the distortion)
    std::vector<float> splitBuf, dynGain;
    float dynEnv = 0.0f, dynAtt = 0.0f, dynRel = 0.0f, dynGainDb = 0.0f, dynGainUp = 0.0f, dynGainDown = 0.0f;
    Biquad dynSmooth;
    Biquad bass, mid, treble, presence;           // tone stack
    Biquad cabHp, cabLowRes, cabScoop, cabComb, cabBite, cabLp1, cabLp2; // per-amp built-in cab
    Biquad depth;                                 // post-cab low resonance
    Biquad micA, micB, micPosShelf, micDistLow, micDistHigh;   // the mic: its type, where it points, how far back
    Biquad micSpkLow, micSpkPeak;                              // which speaker in the cab is miked
    float lastMicSpeaker = -1.0f;
    std::vector<float> reflBuf = std::vector<float> (2048, 0.0f);   // far mic: a short reflection off the floor/room
    int reflPos = 0;
    float reflGain = 0.0f, reflDelay = 0.0f;
    float lastCabModel = -1.0f, lastMic = -1.0f, lastMicPos = -1.0f, lastMicDist = -1.0f;
    void updateCabMic (int amp, bool force);

    // post EQ (after the amp and cab): low cut, 4 semi-parametric bands, high cut
    Biquad eqLowCut, eqHighCut, eqBand[4];
    float eqLast[10] = { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 };
    juce::SmoothedValue<float> eqWet;

    // doubler: a delayed, slowly detuned copy of the amp on the right side
    std::vector<float> dblBuf;
    int dblMask = 0, dblPos = 0;
    float dblPhase = 0.0f;
    juce::SmoothedValue<float> dblWet, dblDelay, monoAmount;
    void updateEq (bool force);
public:
    /** Response of the post EQ at a frequency, in dB (for the EQ page's curve). Message thread. */
    float getEqResponseDb (double hz) const;
    static constexpr float eqBandMinHz[4] = { 40.0f, 150.0f, 600.0f, 2000.0f };
    static constexpr float eqBandMaxHz[4] = { 500.0f, 2000.0f, 6000.0f, 12000.0f };
    static constexpr float eqBandDefHz[4] = { 100.0f, 500.0f, 2000.0f, 6000.0f };
private:

    std::array<juce::dsp::Convolution, numAmps> irConvolution;
    std::array<juce::File, numAmps> irFile;
    std::array<std::atomic<bool>, numAmps> irActive {};

    PitchShifter pitchShifter;
    juce::SmoothedValue<float> pitchWet, pitchRatio;

    StereoChorus chorus;
    juce::SmoothedValue<float> chorusWet;
    PingPongDelay delay;
    juce::SmoothedValue<float> delayWet;
    juce::dsp::Reverb reverb;
    juce::SmoothedValue<float> reverbWet;

    // last values used to compute tone-stack coefficients
    float lastBass = -1, lastMid = -1, lastTreble = -1, lastPresence = -1;

    float trackPeak = 0.0f; int trackSamples = 0;
    std::array<float, tunerBufferSize> tunerBuffer {};
    std::atomic<int> tunerWritePos { 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EmberAmpProcessor)
};
