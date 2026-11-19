#pragma once

#include "PluginProcessor.h"
#include <juce_audio_devices/juce_audio_devices.h>

//==============================================================================
/** "Stream": sends a second copy of Ember Amp's sound (plus an optional microphone) to another
    output device, e.g. BlackHole (Mac) or VB-Audio Virtual Cable (Windows), which Discord, OBS
    or Zoom can use as their mic. Your own monitoring stays on your interface, untouched.

    The two devices run on separate clocks, so the hand-over buffer keeps itself at a small
    target fill by skipping or repeating a single sample now and then (inaudible on voice chat). */
class StreamOutput : private juce::AudioIODeviceCallback
{
public:
    explicit StreamOutput (EmberAmpProcessor&);
    ~StreamOutput() override;

    struct Settings
    {
        juce::String outputDevice;            // e.g. "BlackHole 2ch"
        juce::String micDevice;               // usually your interface; empty = no mic
        int micChannel = -1;                  // 0-based input channel, -1 = no mic
        double sampleRate = 48000.0;
    };

    /** Opens the stream device. Returns an error message, or empty on success. */
    juce::String start (const Settings&);
    void stop();
    bool isRunning() const                       { return running.load(); }
    juce::String getOutputName() const           { return openedOutput; }
    double getSampleRate() const                 { return openedRate; }
    bool hasMic() const                          { return micOpen; }

    std::atomic<float> guitarGain { 1.0f }, micGain { 1.0f };
    std::atomic<float> micPeak { 0.0f }, outPeak { 0.0f };
    std::atomic<int> underruns { 0 }, adjustments { 0 };   // diagnostics

    /** Output / input device names of the platform's main device type (no device is opened). */
    static juce::StringArray listDevices (bool inputs);
    /** The installed virtual cable, if any: BlackHole / VB-Audio CABLE / Loopback / Soundflower. */
    static juce::String findVirtualCable();

private:
    void audioDeviceIOCallbackWithContext (const float* const* in, int numIn, float* const* out, int numOut,
                                           int numSamples, const juce::AudioIODeviceCallbackContext&) override;
    void audioDeviceAboutToStart (juce::AudioIODevice*) override;
    void audioDeviceStopped() override {}
    void audioDeviceError (const juce::String&) override { running = false; }

    EmberAmpProcessor& proc;
    juce::AudioDeviceManager dm;
    std::atomic<bool> running { false };
    juce::String openedOutput;
    double openedRate = 0.0;
    bool micOpen = false;

    std::vector<float> tmpL, tmpR;
    int target = 1024;
    bool priming = true;
    float micHp = 0.0f, micHpCoef = 0.0f;
};
