#include "StreamOutput.h"

StreamOutput::StreamOutput (EmberAmpProcessor& p) : proc (p) {}

StreamOutput::~StreamOutput()
{
    stop();
}

static juce::AudioIODeviceType* mainDeviceType (juce::AudioDeviceManager& dm)
{
    auto& types = dm.getAvailableDeviceTypes();
    for (auto* t : types)   // shared-mode types: they can share the interface with the main output
        if (t->getTypeName() == "CoreAudio" || t->getTypeName() == "Windows Audio")
            return t;
    return types.isEmpty() ? nullptr : types.getFirst();
}

juce::StringArray StreamOutput::listDevices (bool inputs)
{
    juce::AudioDeviceManager tmp;
    if (auto* type = mainDeviceType (tmp))
    {
        type->scanForDevices();
        return type->getDeviceNames (inputs);
    }
    return {};
}

juce::String StreamOutput::findVirtualCable()
{
    for (auto& name : listDevices (false))
        for (auto* key : { "BlackHole", "CABLE Input", "VB-Audio", "Loopback", "Soundflower" })
            if (name.containsIgnoreCase (key))
                return name;
    return {};
}

juce::String StreamOutput::start (const Settings& s)
{
    stop();
    if (s.outputDevice.isEmpty())
        return "Choose where to send the sound.";

    auto* type = mainDeviceType (dm);
    if (type == nullptr) return "No audio system found.";
    dm.setCurrentAudioDeviceType (type->getTypeName(), false);
    dm.closeAudioDevice();   // setCurrentAudioDeviceType may have opened a default device

    juce::AudioDeviceManager::AudioDeviceSetup setup;
    setup.outputDeviceName = s.outputDevice;
    setup.useDefaultOutputChannels = true;
    setup.sampleRate = s.sampleRate;
    setup.bufferSize = 256;
    micOpen = s.micDevice.isNotEmpty() && s.micChannel >= 0;
    if (micOpen)
    {
        setup.inputDeviceName = s.micDevice;
        setup.useDefaultInputChannels = false;
        setup.inputChannels.setBit (s.micChannel);
    }
    else
    {
        setup.inputDeviceName = {};
        setup.useDefaultInputChannels = false;
    }

    auto err = dm.setAudioDeviceSetup (setup, false);
    if (err.isEmpty() && dm.getCurrentAudioDevice() == nullptr)
        err = "Couldn't open " + s.outputDevice + ".";
    if (err.isNotEmpty())
    {
        dm.closeAudioDevice();
        micOpen = false;
        return err;
    }

    auto* dev = dm.getCurrentAudioDevice();
    openedOutput = dev->getName();
    openedRate = dev->getCurrentSampleRate();
    if (std::abs (openedRate - s.sampleRate) > 1.0)
    {
        dm.closeAudioDevice();
        micOpen = false;
        return s.outputDevice + " can't run at " + juce::String (juce::roundToInt (s.sampleRate)) + " Hz like your interface.";
    }

    // fresh, empty hand-over buffer, then start filling it
    proc.streamTapOn = false;
    juce::Thread::sleep (30);            // let an audio block that is mid-copy finish
    proc.streamFifo.reset();
    priming = true;
    dm.addAudioCallback (this);
    proc.streamTapOn = true;
    running = true;
    return {};
}

void StreamOutput::stop()
{
    proc.streamTapOn = false;
    dm.removeAudioCallback (this);
    dm.closeAudioDevice();
    running = false;
    openedOutput.clear();
    micOpen = false;
    micPeak = 0.0f;
    outPeak = 0.0f;
}

void StreamOutput::audioDeviceAboutToStart (juce::AudioIODevice* dev)
{
    const int bs = juce::jmax (64, dev->getCurrentBufferSizeSamples());
    tmpL.assign ((size_t) bs + 4, 0.0f);
    tmpR.assign ((size_t) bs + 4, 0.0f);
    target = 2 * bs + 512;                     // ~15 ms of cushion at 48 kHz
    priming = true;
    micHp = 0.0f;
    micHpCoef = 1.0f - std::exp (-juce::MathConstants<float>::twoPi * 90.0f / (float) dev->getCurrentSampleRate());
}

void StreamOutput::audioDeviceIOCallbackWithContext (const float* const* in, int numIn, float* const* out, int numOut,
                                                     int numSamples, const juce::AudioIODeviceCallbackContext&)
{
    if (numOut <= 0) return;
    if ((int) tmpL.size() < numSamples + 2)   // device changed its block size: play silence this once
    {
        for (int c = 0; c < numOut; ++c) juce::FloatVectorOperations::clear (out[c], numSamples);
        return;
    }

    auto& fifo = proc.streamFifo;
    int avail = fifo.getNumReady();

    // far too much waiting (e.g. after a hiccup): throw the excess away to keep the delay low
    if (avail > target + 1536)
    {
        fifo.finishedRead (avail - target);
        avail = target;
    }

    // keep the fill near the target: read one sample more or less than needed now and then
    int want = numSamples;
    if (priming)
    {
        if (avail >= target) priming = false;
        want = 0;
    }
    else if (avail < numSamples)
    {
        priming = true;                         // ran dry: wait to refill rather than crackle
        ++underruns;
        want = 0;
    }
    else if (avail > target + 256) { want = numSamples + 1; ++adjustments; }
    else if (avail < target - 256) { want = numSamples - 1; ++adjustments; }

    if (want > 0)
    {
        const auto r = fifo.read (want);
        if (r.blockSize1 > 0) { std::copy_n (proc.streamBufL.data() + r.startIndex1, r.blockSize1, tmpL.data()); std::copy_n (proc.streamBufR.data() + r.startIndex1, r.blockSize1, tmpR.data()); }
        if (r.blockSize2 > 0) { std::copy_n (proc.streamBufL.data() + r.startIndex2, r.blockSize2, tmpL.data() + r.blockSize1);
                                std::copy_n (proc.streamBufR.data() + r.startIndex2, r.blockSize2, tmpR.data() + r.blockSize1); }
        if (want == numSamples - 1) { tmpL[(size_t) numSamples - 1] = tmpL[(size_t) numSamples - 2]; tmpR[(size_t) numSamples - 1] = tmpR[(size_t) numSamples - 2]; }
        // want == numSamples + 1: the extra (last) sample is simply not played
    }
    else
    {
        std::fill_n (tmpL.data(), numSamples, 0.0f);
        std::fill_n (tmpR.data(), numSamples, 0.0f);
    }

    const float gg = guitarGain.load(), mg = micGain.load();
    const float* mic = (micOpen && numIn > 0) ? in[0] : nullptr;
    float mPk = 0.0f, oPk = 0.0f;
    for (int i = 0; i < numSamples; ++i)
    {
        float m = 0.0f;
        if (mic != nullptr)
        {
            micHp += micHpCoef * (mic[i] - micHp);   // cut rumble / handling noise
            m = (mic[i] - micHp) * mg;
            mPk = juce::jmax (mPk, std::abs (m));
        }
        float l = tmpL[(size_t) i] * gg + m, r = tmpR[(size_t) i] * gg + m;
        l = juce::jlimit (-1.0f, 1.0f, l);
        r = juce::jlimit (-1.0f, 1.0f, r);
        oPk = juce::jmax (oPk, std::abs (l), std::abs (r));
        out[0][i] = l;
        if (numOut > 1) out[1][i] = r;
    }
    for (int c = 2; c < numOut; ++c)
        juce::FloatVectorOperations::copy (out[c], out[c % 2], numSamples);

    micPeak = juce::jmax (mPk, micPeak.load());
    outPeak = juce::jmax (oPk, outPeak.load());
}
