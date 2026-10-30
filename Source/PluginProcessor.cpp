#include "PluginProcessor.h"

namespace
{
juce::AudioProcessorValueTreeState::ParameterLayout makeLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout layout;

    layout.add (std::make_unique<AudioParameterBool> (ParameterID { "capture", 1 }, "Capture system audio", true));
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "playbackMode", 1 }, "During playback",
        StringArray { "Play recorded track audio", "Keep capturing (original behaviour)" }, 0));
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "source", 1 }, "Capture source",
        StringArray { "Everything except REAPER", "Whole default device" }, 0));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "captureGain", 1 }, "Capture gain",
        NormalisableRange<float> (-60.0f, 12.0f, 0.1f), 0.0f, AudioParameterFloatAttributes().withLabel ("dB")));
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { "passInput", 1 }, "Mix track input with capture", false));
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { "send", 1 }, "Send output to Windows device", false));
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { "sendOnlyPlaying", 1 }, "Send only while REAPER is playing", false));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "sendGain", 1 }, "Send gain",
        NormalisableRange<float> (-60.0f, 12.0f, 0.1f), 0.0f, AudioParameterFloatAttributes().withLabel ("dB")));
    return layout;
}

class BridgeEditor final : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit BridgeEditor (BridgeProcessor& p) : AudioProcessorEditor (p), proc (p), generic (p)
    {
        addAndMakeVisible (generic);
        addAndMakeVisible (status);
        status.setJustificationType (juce::Justification::centredLeft);
        status.setMinimumHorizontalScale (0.7f);

        for (auto* l : { &captureLabel, &sendLabel })
        {
            l->setJustificationType (juce::Justification::centredRight);
            addAndMakeVisible (*l);
        }
        for (auto* b : { &captureBox, &sendBox })
            addAndMakeVisible (*b);
        addAndMakeVisible (refresh);

        captureBox.onChange = [this] { choose (captureBox, true); };
        sendBox.onChange    = [this] { choose (sendBox, false); };
        refresh.onClick     = [this] { rescan(); };
        captureBox.setTooltip ("Default follows the Capture source parameter. A chosen device is captured as a "
                               "whole-device loopback, which includes anything REAPER plays to it.");
        sendBox.setTooltip ("Windows device the send plays on. A chosen device that disappears is reported, "
                            "never replaced by another one.");

        setSize (480, generic.getHeight() + 3 * rowHeight + 48);
        rescan();
        startTimerHz (4);
        timerCallback();
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (getLookAndFeel().findColour (juce::ResizableWindow::backgroundColourId));
    }

    void resized() override
    {
        auto r = getLocalBounds();
        status.setBounds (r.removeFromBottom (48).reduced (8, 4));

        auto devices = r.removeFromBottom (3 * rowHeight).reduced (8, 2);
        auto row = [&devices] { return devices.removeFromTop (rowHeight).reduced (0, 2); };

        auto c = row();
        captureLabel.setBounds (c.removeFromLeft (labelWidth));
        captureBox.setBounds (c.withTrimmedLeft (6));
        auto s = row();
        sendLabel.setBounds (s.removeFromLeft (labelWidth));
        sendBox.setBounds (s.withTrimmedLeft (6));
        refresh.setBounds (row().removeFromRight (140));

        generic.setBounds (r);
    }

private:
    static constexpr int rowHeight = 30, labelWidth = 110, defaultItem = 1, missingItem = 10000;

    void timerCallback() override
    {
        status.setText (proc.statusText(), juce::dontSendNotification);
        if (proc.getEndpointRevision() != shownRevision)
            populate();
    }

    void rescan()
    {
        endpoints = listRenderEndpoints();
        populate();
    }

    void populate()
    {
        shownRevision = proc.getEndpointRevision();
        fill (captureBox, proc.getCaptureEndpoint(), true);
        fill (sendBox, proc.getSendEndpoint(), false);
    }

    // Item ids: defaultItem, then 2 + index into endpoints, then missingItem for a saved
    // endpoint that is not currently present.
    void fill (juce::ComboBox& box, const BridgeProcessor::Endpoint& chosen, bool forCapture)
    {
        box.clear (juce::dontSendNotification);
        box.addItem (forCapture ? "Default device (see Capture source)" : "Default device", defaultItem);

        int selected = chosen.id.isEmpty() ? defaultItem : missingItem;
        for (size_t i = 0; i < endpoints.size(); ++i)
        {
            const auto& e = endpoints[i];
            auto text = juce::String::fromUTF8 (e.name.c_str());
            if (forCapture) text = "Whole-device loopback: " + text;
            if (e.isDefault) text << "  (current default)";
            box.addItem (text, (int) i + 2);

            if (chosen.id.isNotEmpty() && chosen.id == juce::String::fromUTF8 (e.id.c_str()))
                selected = (int) i + 2;
        }

        if (selected == missingItem)
            box.addItem ("Missing: " + (chosen.name.isNotEmpty() ? chosen.name : chosen.id), missingItem);

        box.setSelectedId (selected, juce::dontSendNotification);
    }

    void choose (juce::ComboBox& box, bool forCapture)
    {
        const int id = box.getSelectedId();
        if (id == missingItem)
            return;

        BridgeProcessor::Endpoint e;
        if (id >= 2 && id - 2 < (int) endpoints.size())
        {
            const auto& found = endpoints[(size_t) (id - 2)];
            e = { juce::String::fromUTF8 (found.id.c_str()), juce::String::fromUTF8 (found.name.c_str()) };
        }

        if (forCapture)
            proc.setCaptureEndpoint (e);
        else
            proc.setSendEndpoint (e);

        populate(); // drops a "Missing" entry once something else is chosen
    }

    BridgeProcessor& proc;
    juce::GenericAudioProcessorEditor generic;
    juce::Label status;
    juce::Label captureLabel { {}, "Capture from" }, sendLabel { {}, "Send to" };
    juce::ComboBox captureBox, sendBox;
    juce::TextButton refresh { "Refresh devices" };
    juce::TooltipWindow tooltips { this, 500 };
    std::vector<AudioEndpoint> endpoints;
    int shownRevision = -1;
};
} // namespace

//==============================================================================
BridgeProcessor::BridgeProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      params (*this, nullptr, "params", makeLayout())
{
    captureOn     = params.getRawParameterValue ("capture");
    playbackMode  = params.getRawParameterValue ("playbackMode");
    captureSource = params.getRawParameterValue ("source");
    captureGain   = params.getRawParameterValue ("captureGain");
    passInput     = params.getRawParameterValue ("passInput");
    sendOn        = params.getRawParameterValue ("send");
    sendGain      = params.getRawParameterValue ("sendGain");
    sendOnlyPlaying = params.getRawParameterValue ("sendOnlyPlaying");

    startTimer (100);
}

BridgeProcessor::~BridgeProcessor()
{
    stopTimer();
    capture.stop();
    render.stop();
}

bool BridgeProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    return (out == juce::AudioChannelSet::stereo() || out == juce::AudioChannelSet::mono())
           && layouts.getMainInputChannelSet() == out;
}

void BridgeProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    tmpL.assign ((size_t) juce::jmax (samplesPerBlock, 4096), 0.0f);
    tmpR.assign (tmpL.size(), 0.0f);
    currentRate = sampleRate;
}

void BridgeProcessor::releaseResources()
{
    currentRate = 0.0;
}

BridgeProcessor::Endpoint BridgeProcessor::getCaptureEndpoint() const
{
    const juce::ScopedLock sl (endpointLock);
    return captureEndpoint;
}

BridgeProcessor::Endpoint BridgeProcessor::getSendEndpoint() const
{
    const juce::ScopedLock sl (endpointLock);
    return sendEndpoint;
}

void BridgeProcessor::setCaptureEndpoint (const Endpoint& e)
{
    {
        const juce::ScopedLock sl (endpointLock);
        captureEndpoint = e;
    }
    ++endpointRevision;
    updateHostDisplay (ChangeDetails().withNonParameterStateChanged (true));   // marks the project dirty
}

void BridgeProcessor::setSendEndpoint (const Endpoint& e)
{
    {
        const juce::ScopedLock sl (endpointLock);
        sendEndpoint = e;
    }
    ++endpointRevision;
    updateHostDisplay (ChangeDetails().withNonParameterStateChanged (true));
}

void BridgeProcessor::timerCallback()
{
    // A stream that failed or lost its device is retried at this interval rather than every tick.
    // Retrying a selected endpoint only ever reopens that same endpoint.
    constexpr juce::uint32 retryMs = 2000;
    const auto now = juce::Time::getMillisecondCounter();
    const double rate = currentRate.load();
    const int source = (int) captureSource->load();
    const auto captureTarget = getCaptureEndpoint();
    const auto sendTarget = getSendEndpoint();

    const bool wantCapture = rate > 0.0 && captureOn->load() > 0.5f;
    if (! wantCapture && captureStarted)
    {
        capture.stop();
        captureStarted = false;
    }
    else if (wantCapture)
    {
        const bool changed = ! captureStarted || rate != captureRate || source != captureSourceInUse
                             || captureTarget != captureEndpointInUse;
        if (changed || (! capture.isRunning() && now >= captureRetryAt))
        {
            const auto mode = captureTarget.id.isNotEmpty() ? CaptureSource::selectedEndpoint
                              : source == 0                 ? CaptureSource::excludeHostProcess
                                                            : CaptureSource::wholeDefaultDevice;
            capture.start ((int) rate, mode, captureTarget.id.toStdString(), captureTarget.name.toStdString());
            captureStarted = true;
            captureRate = rate;
            captureSourceInUse = source;
            captureEndpointInUse = captureTarget;
            captureRetryAt = now + retryMs;
        }
    }

    const bool wantSend = rate > 0.0 && sendOn->load() > 0.5f;
    if (! wantSend && renderStarted)
    {
        render.stop();
        renderStarted = false;
    }
    else if (wantSend)
    {
        const bool changed = ! renderStarted || rate != renderRate || sendTarget != sendEndpointInUse;
        if (changed || (! render.isRunning() && now >= renderRetryAt))
        {
            render.start ((int) rate, sendTarget.id.toStdString(), sendTarget.name.toStdString());
            renderStarted = true;
            renderRate = rate;
            sendEndpointInUse = sendTarget;
            renderRetryAt = now + retryMs;
        }
    }
}

juce::String BridgeProcessor::statusText() const
{
    const bool captureWanted = captureOn->load() > 0.5f;
    const bool sendWanted = sendOn->load() > 0.5f;

    juce::String s = capture.isRunning() || captureWanted ? juce::String (capture.status()) : "Capture off";
    s << "\n" << (render.isRunning() || sendWanted ? juce::String (render.status()) : "Send off");

    const auto c = getCaptureEndpoint(), d = getSendEndpoint();
    if (captureWanted && sendWanted && c.id.isNotEmpty() && c == d)
        s << "  -  WARNING: capturing the device you send to will feed back";

    return s;
}

void BridgeProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int numSamples = buffer.getNumSamples();
    const int numChannels = buffer.getNumChannels();
    const double rate = getSampleRate();

    bool playing = false, recording = false;
    if (auto* ph = getPlayHead())
        if (auto pos = ph->getPosition())
        {
            playing = pos->getIsPlaying();
            recording = pos->getIsRecording();
        }

    // The fix: during plain playback the track's recorded items pass through untouched.
    const bool passThroughPlayback = playbackMode->load() < 0.5f && playing && ! recording;

    if (capture.isRunning() && captureOn->load() > 0.5f)
    {
        // Keep the capture ring near one block deep so monitoring latency cannot creep.
        const int maxDepth = numSamples + (int) (rate * 0.05);
        const int avail = capture.ring.available();
        if (avail > maxDepth)
            capture.ring.read (nullptr, nullptr, avail - (numSamples + (int) (rate * 0.01)));

        if (passThroughPlayback)
        {
            capture.ring.read (nullptr, nullptr, numSamples); // drain, don't hear
        }
        else
        {
            if (passInput->load() < 0.5f)
                buffer.clear();

            const float gain = juce::Decibels::decibelsToGain (captureGain->load(), -60.0f);
            const int chunkMax = (int) tmpL.size();

            for (int start = 0; start < numSamples; start += chunkMax)
            {
                const int n = juce::jmin (chunkMax, numSamples - start);
                std::fill (tmpL.begin(), tmpL.begin() + n, 0.0f);
                std::fill (tmpR.begin(), tmpR.begin() + n, 0.0f);
                capture.ring.read (tmpL.data(), tmpR.data(), n);

                if (numChannels >= 2)
                {
                    buffer.addFrom (0, start, tmpL.data(), n, gain);
                    buffer.addFrom (1, start, tmpR.data(), n, gain);
                }
                else if (numChannels == 1)
                {
                    buffer.addFrom (0, start, tmpL.data(), n, gain * 0.5f);
                    buffer.addFrom (0, start, tmpR.data(), n, gain * 0.5f);
                }
            }
        }
    }

    if (render.isRunning() && sendOn->load() > 0.5f && numChannels > 0
        && (playing || sendOnlyPlaying->load() < 0.5f))
    {
        const float gain = juce::Decibels::decibelsToGain (sendGain->load(), -60.0f);
        const int chunkMax = (int) tmpL.size();

        for (int start = 0; start < numSamples; start += chunkMax)
        {
            const int n = juce::jmin (chunkMax, numSamples - start);
            const float* l = buffer.getReadPointer (0, start);
            const float* r = buffer.getReadPointer (numChannels > 1 ? 1 : 0, start);
            for (int i = 0; i < n; ++i) { tmpL[(size_t) i] = l[i] * gain; tmpR[(size_t) i] = r[i] * gain; }
            render.ring.write (tmpL.data(), tmpR.data(), n);
        }
    }
}

juce::AudioProcessorEditor* BridgeProcessor::createEditor()
{
    return new BridgeEditor (*this);
}

namespace
{
// Endpoint selections are host state rather than automatable parameters. They are omitted while
// on the default device, so default-device projects save exactly as they did before.
const juce::Identifier captureIdKey { "captureDeviceId" }, captureNameKey { "captureDeviceName" },
                       sendIdKey { "sendDeviceId" }, sendNameKey { "sendDeviceName" };

void storeEndpoint (juce::ValueTree& state, const juce::Identifier& idKey, const juce::Identifier& nameKey,
                    const BridgeProcessor::Endpoint& e)
{
    state.removeProperty (idKey, nullptr);
    state.removeProperty (nameKey, nullptr);
    if (e.id.isNotEmpty())
    {
        state.setProperty (idKey, e.id, nullptr);
        state.setProperty (nameKey, e.name, nullptr);
    }
}

BridgeProcessor::Endpoint loadEndpoint (const juce::ValueTree& state, const juce::Identifier& idKey,
                                        const juce::Identifier& nameKey)
{
    return { state.getProperty (idKey).toString(), state.getProperty (nameKey).toString() };
}
} // namespace

void BridgeProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    auto state = params.copyState();
    storeEndpoint (state, captureIdKey, captureNameKey, getCaptureEndpoint());
    storeEndpoint (state, sendIdKey, sendNameKey, getSendEndpoint());

    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, dest);
}

void BridgeProcessor::setStateInformation (const void* data, int size)
{
    if (auto xml = getXmlFromBinary (data, size))
        if (xml->hasTagName (params.state.getType()))
        {
            const auto state = juce::ValueTree::fromXml (*xml);
            {
                const juce::ScopedLock sl (endpointLock);
                captureEndpoint = loadEndpoint (state, captureIdKey, captureNameKey);
                sendEndpoint = loadEndpoint (state, sendIdKey, sendNameKey);
            }
            ++endpointRevision;
            params.replaceState (state);
        }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new BridgeProcessor();
}
