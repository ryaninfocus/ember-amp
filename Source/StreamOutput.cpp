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

static bool isLoopbackName (const juce::String& name)
{
    for (auto* key : { "BlackHole", "CABLE", "VB-Audio", "Loopback", "Soundflower", "Teams Audio", "ZoomAudio" })
        if (name.containsIgnoreCase (key))
            return true;
    return false;
}

std::vector<StreamOutput::Mic> StreamOutput::listMics()
{
    std::vector<Mic> mics;
    juce::AudioDeviceManager tmp;
    if (auto* type = mainDeviceType (tmp))
    {
        type->scanForDevices();
        for (auto& name : type->getDeviceNames (true))
        {
            if (isLoopbackName (name)) continue;
            int n = 1;
            if (std::unique_ptr<juce::AudioIODevice> dev { type->createDevice ({}, name) })
                n = juce::jmax (1, dev->getInputChannelNames().size());
            mics.push_back ({ name, n });
        }
    }
    return mics;
}

juce::String StreamOutput::start (const Settings& s)
{
    stop();
    micProblem.clear();
    if (s.outputDevice.isEmpty())
        return "Choose where to send the sound.";

    auto* type = mainDeviceType (dm);
    if (type == nullptr) return "No audio system found.";
    dm.setCurrentAudioDeviceType (type->getTypeName(), false);
    dm.closeAudioDevice();   // setCurrentAudioDeviceType may have opened a default device

    juce::AudioDeviceManager::AudioDeviceSetup setup;
    setup.outputDeviceName = s.outputDevice;
    setup.useDefaultOutputChannels = true;
    setup.inputDeviceName = {};
    setup.useDefaultInputChannels = false;
    setup.sampleRate = s.sampleRate;
    setup.bufferSize = s.bufferSize;

    auto err = dm.setAudioDeviceSetup (setup, false);
    if (err.isEmpty() && dm.getCurrentAudioDevice() == nullptr)
        err = "Couldn't open " + s.outputDevice + ".";
    if (err.isNotEmpty())
    {
        dm.closeAudioDevice();
        return err;
    }

    // the mic: its own input-only device
    if (s.micDevice.isNotEmpty() && s.micChannel >= 0)
    {
        juce::AudioDeviceManager::AudioDeviceSetup ms;
        ms.inputDeviceName = s.micDevice;
        ms.outputDeviceName = {};
        ms.useDefaultInputChannels = false;
        ms.useDefaultOutputChannels = false;
        ms.inputChannels.setBit (s.micChannel);
        ms.sampleRate = s.sampleRate;
        ms.bufferSize = s.bufferSize;
        micDm.setCurrentAudioDeviceType (type->getTypeName(), false);
        auto merr = micDm.initialise (1, 0, nullptr, false, {}, &ms);   // input only
        auto* md = micDm.getCurrentAudioDevice();
        if (merr.isEmpty() && (md == nullptr || md->getActiveInputChannels().countNumberOfSetBits() == 0))
            merr = "no input";
        if (merr.isEmpty() && std::abs (md->getCurrentSampleRate() - s.sampleRate) > 1.0)
            merr = "wrong rate";
        if (merr.isNotEmpty())
        {
            micDm.closeAudioDevice();
            micProblem = "Couldn't open the mic " + s.micDevice + ".";
        }
        else
        {
            micIn.on = false;
            micIn.fifo.reset();
            micOpen = true;
            micPriming = true;
            micDm.addAudioCallback (&micIn);
            micIn.on = true;
        }
    }

    auto* dev = dm.getCurrentAudioDevice();
    openedOutput = dev->getName();
    openedRate = dev->getCurrentSampleRate();
    if (std::abs (openedRate - s.sampleRate) > 1.0)
    {
        stop();
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
    micIn.on = false;
    micDm.removeAudioCallback (&micIn);
    micDm.closeAudioDevice();
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
    tmpMic.assign ((size_t) bs + 4, 0.0f);
    target = 2 * bs + 512;                     // ~15 ms of cushion at 48 kHz
    micTarget = target + 512;                  // the mic's block size may differ: a bit more cushion
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

    // the mic, same idea: keep its buffer near a small target
    const float* mic = nullptr;
    if (micOpen)
    {
        auto& mf = micIn.fifo;
        int mAvail = mf.getNumReady();
        if (mAvail > micTarget + 2048) { mf.finishedRead (mAvail - micTarget); mAvail = micTarget; }
        int mWant = numSamples;
        if (micPriming)                { if (mAvail >= micTarget) micPriming = false; mWant = 0; }
        else if (mAvail < numSamples)  { micPriming = true; mWant = 0; }
        else if (mAvail > micTarget + 256) mWant = numSamples + 1;
        else if (mAvail < micTarget - 256) mWant = numSamples - 1;
        if (mWant > 0)
        {
            const auto r = mf.read (mWant);
            if (r.blockSize1 > 0) std::copy_n (micIn.buf.data() + r.startIndex1, r.blockSize1, tmpMic.data());
            if (r.blockSize2 > 0) std::copy_n (micIn.buf.data() + r.startIndex2, r.blockSize2, tmpMic.data() + r.blockSize1);
            if (mWant == numSamples - 1) tmpMic[(size_t) numSamples - 1] = tmpMic[(size_t) numSamples - 2];
        }
        else
            std::fill_n (tmpMic.data(), numSamples, 0.0f);
        mic = tmpMic.data();
    }
    juce::ignoreUnused (in, numIn);

    const float gg = guitarGain.load(), mg = micGain.load();
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

void StreamOutput::MicIn::audioDeviceIOCallbackWithContext (const float* const* in, int numIn, float* const* out, int numOut,
                                                           int numSamples, const juce::AudioIODeviceCallbackContext&)
{
    for (int c = 0; c < numOut; ++c)
        if (out[c] != nullptr) juce::FloatVectorOperations::clear (out[c], numSamples);
    if (! on.load() || numIn <= 0 || in[0] == nullptr) return;
    const int n = juce::jmin (numSamples, fifo.getFreeSpace());   // if nobody reads, just drop
    const auto w = fifo.write (n);
    if (w.blockSize1 > 0) std::copy_n (in[0], w.blockSize1, buf.data() + w.startIndex1);
    if (w.blockSize2 > 0) std::copy_n (in[0] + w.blockSize1, w.blockSize2, buf.data() + w.startIndex2);
}
