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
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { "send", 1 }, "Send REAPER audio", false));
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { "sendOnlyPlaying", 1 }, "Send only while REAPER is playing", false));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "sendGain", 1 }, "Send gain",
        NormalisableRange<float> (-60.0f, 12.0f, 0.1f), 0.0f, AudioParameterFloatAttributes().withLabel ("dB")));
    return layout;
}

// One mode at a time: capture system audio into the track, or send the track's audio to a Windows
// device. The mode is the "capture" and "send" parameters set exclusively, so 1.0.x projects and
// automation keep working. The Capture source parameter is folded into the Capture from menu.
class BridgeEditor final : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit BridgeEditor (BridgeProcessor& p) : AudioProcessorEditor (p), proc (p)
    {
        captureMode.setComponentID ("captureMode");
        sendMode.setComponentID ("sendMode");
        captureBox.setComponentID ("captureBox");
        sendBox.setComponentID ("sendBox");

        captureMode.setConnectedEdges (juce::Button::ConnectedOnRight);
        sendMode.setConnectedEdges (juce::Button::ConnectedOnLeft);
        for (auto* b : { &captureMode, &sendMode })
        {
            b->setColour (juce::TextButton::buttonOnColourId, getLookAndFeel().findColour (juce::Slider::thumbColourId));
            b->setColour (juce::TextButton::textColourOnId, juce::Colours::white);
        }
        captureMode.onClick = [this] { setMode (false); };
        sendMode.onClick    = [this] { setMode (true); };
        captureBox.onChange = [this] { chooseCapture(); };
        sendBox.onChange    = [this] { chooseSend(); };
        refresh.onClick     = [this] { rescan(); };

        const auto& choices = dynamic_cast<juce::AudioParameterChoice&> (*p.params.getParameter ("playbackMode")).choices;
        playbackBox.addItemList (choices, 1);

        for (auto* s : { &captureGain, &sendGain })
        {
            s->setSliderStyle (juce::Slider::LinearHorizontal);
            s->setTextBoxStyle (juce::Slider::TextBoxRight, false, 72, 22);
            s->setTextValueSuffix (" dB");
        }

        tip ({ &captureMode }, "Record system audio (YouTube, Spotify, a call...) into this track.");
        tip ({ &sendMode }, "Play this track's audio on a Windows output device, e.g. to hear the REAPER mix "
                            "on your normal speakers while REAPER runs on ASIO. Put it on the master track.");
        tip ({ &captureFromLabel, &captureBox },
             "Everything except REAPER: whatever Windows plays, minus REAPER, so there is no feedback.\n"
             "Whole default device: everything on the default output, REAPER included.\n"
             "A named device: everything playing on that device, REAPER included. If it is unplugged "
             "the plugin goes silent and waits for it; it never switches device.");
        tip ({ &sendToLabel, &sendBox }, "Windows output device to play on. Default device follows Windows' "
                                         "default when the send starts. A named device that disappears is "
                                         "reported and waited for, never replaced.");
        tip ({ &refresh }, "Re-read the Windows device list, e.g. after plugging something in.");
        tip ({ &playbackLabel, &playbackBox }, "While REAPER plays back (not recording): play the track's recorded "
                                               "takes, or keep monitoring live system audio like the original plugin.");
        tip ({ &captureGainLabel, &captureGain }, "Level of the captured system audio.");
        tip ({ &passInput }, "Off: system audio replaces the track input. On: both are mixed.");
        tip ({ &sendGainLabel, &sendGain }, "Level sent to the Windows device. The track output is unchanged.");
        tip ({ &onlyPlaying }, "Silence the send while the transport is stopped. It still sends while recording.");
        tip ({ &status }, "What the plugin is doing right now.");

        status.setJustificationType (juce::Justification::centredLeft);
        status.setMinimumHorizontalScale (0.8f);
        status.setFont (juce::FontOptions (13.0f));
        for (auto* l : { &captureFromLabel, &sendToLabel, &playbackLabel, &captureGainLabel, &sendGainLabel })
            l->setJustificationType (juce::Justification::centredLeft);

        for (auto* c : std::initializer_list<juce::Component*> { &captureMode, &sendMode, &refresh, &status })
            addAndMakeVisible (c);
        for (auto* c : captureSection) addChildComponent (c);
        for (auto* c : sendSection) addChildComponent (c);

        playbackAttachment = std::make_unique<ComboAttachment> (p.params, "playbackMode", playbackBox);
        captureGainAttachment = std::make_unique<SliderAttachment> (p.params, "captureGain", captureGain);
        sendGainAttachment = std::make_unique<SliderAttachment> (p.params, "sendGain", sendGain);
        passInputAttachment = std::make_unique<ButtonAttachment> (p.params, "passInput", passInput);
        onlyPlayingAttachment = std::make_unique<ButtonAttachment> (p.params, "sendOnlyPlaying", onlyPlaying);

        setSize (460, 262);
        rescan();
        startTimerHz (4);
        timerCallback();
    }

    void paint (juce::Graphics& g) override
    {
        const auto bg = getLookAndFeel().findColour (juce::ResizableWindow::backgroundColourId);
        g.fillAll (bg);
        g.setColour (bg.brighter (0.12f));
        g.fillRoundedRectangle (panel.toFloat(), 6.0f);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (12);
        auto modes = r.removeFromTop (32);
        captureMode.setBounds (modes.removeFromLeft (modes.getWidth() / 2));
        sendMode.setBounds (modes);

        r.removeFromTop (10);
        status.setBounds (r.removeFromBottom (40));
        r.removeFromBottom (8);
        panel = r;

        // Both sections share the panel; only the current mode's is visible. The device row is
        // first in both, so the one Refresh button sits beside whichever menu is shown.
        auto layout = [this] (std::initializer_list<std::pair<juce::Component*, juce::Component*>> rows)
        {
            auto area = panel.reduced (10, 6);
            for (auto [label, control] : rows)
            {
                auto row = area.removeFromTop (rowHeight).reduced (0, 3);
                auto left = row.removeFromLeft (labelWidth);
                if (label != nullptr) label->setBounds (left);
                if (control == &captureBox || control == &sendBox)
                {
                    refresh.setBounds (row.removeFromRight (64));
                    row.removeFromRight (6);
                }
                control->setBounds (row);
            }
        };
        layout ({ { &captureFromLabel, &captureBox }, { &playbackLabel, &playbackBox },
                  { &captureGainLabel, &captureGain }, { nullptr, &passInput } });
        layout ({ { &sendToLabel, &sendBox }, { &sendGainLabel, &sendGain }, { nullptr, &onlyPlaying } });
    }

private:
    using ComboAttachment  = juce::AudioProcessorValueTreeState::ComboBoxAttachment;
    using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ButtonAttachment = juce::AudioProcessorValueTreeState::ButtonAttachment;

    static constexpr int rowHeight = 34, labelWidth = 104;
    // Capture menu ids: the two default-device sources, then deviceBase + index into endpoints,
    // then missingItem for a saved endpoint that is not currently present. The send menu uses
    // sendDefaultItem in place of the two sources.
    static constexpr int everythingItem = 1, wholeDefaultItem = 2, sendDefaultItem = 1,
                         deviceBase = 10, missingItem = 10000;

    static void tip (std::initializer_list<juce::SettableTooltipClient*> targets, const juce::String& text)
    {
        for (auto* t : targets) t->setTooltip (text);
    }

    bool isOn (const char* id) const { return proc.params.getRawParameterValue (id)->load() > 0.5f; }
    int source() const { return (int) proc.params.getRawParameterValue ("source")->load(); }

    void setParam (const char* id, float value)
    {
        auto* param = proc.params.getParameter (id);
        if (param->convertFrom0to1 (param->getValue()) == value) return;
        param->beginChangeGesture();
        param->setValueNotifyingHost (param->convertTo0to1 (value));
        param->endChangeGesture();
    }

    void setMode (bool send)
    {
        setParam ("capture", send ? 0.0f : 1.0f);
        setParam ("send", send ? 1.0f : 0.0f);
        syncMode();
    }

    // Lit buttons mirror the two parameters. A 1.0.x project with both on lights both and shows
    // the capture section until a mode is clicked.
    void syncMode()
    {
        const bool capture = isOn ("capture"), send = isOn ("send");
        captureMode.setToggleState (capture, juce::dontSendNotification);
        sendMode.setToggleState (send, juce::dontSendNotification);

        const bool showSend = send && ! capture;
        for (auto* c : captureSection) c->setVisible (! showSend);
        for (auto* c : sendSection) c->setVisible (showSend);
    }

    void timerCallback() override
    {
        status.setText (proc.statusText(), juce::dontSendNotification);
        syncMode();
        if (proc.getEndpointRevision() != shownRevision || source() != shownSource)
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
        shownSource = source();

        const auto c = proc.getCaptureEndpoint();
        captureBox.clear (juce::dontSendNotification);
        captureBox.addItem ("Everything except REAPER", everythingItem);
        captureBox.addItem ("Whole default device", wholeDefaultItem);
        captureBox.addSectionHeading ("Whole device (includes REAPER)");
        addDevices (captureBox, c, c.id.isEmpty() ? (shownSource == 0 ? everythingItem : wholeDefaultItem) : missingItem);

        const auto s = proc.getSendEndpoint();
        sendBox.clear (juce::dontSendNotification);
        sendBox.addItem ("Default device", sendDefaultItem);
        sendBox.addSeparator();
        addDevices (sendBox, s, s.id.isEmpty() ? sendDefaultItem : missingItem);
    }

    void addDevices (juce::ComboBox& box, const BridgeProcessor::Endpoint& chosen, int selected)
    {
        for (size_t i = 0; i < endpoints.size(); ++i)
        {
            const auto& e = endpoints[i];
            auto text = juce::String::fromUTF8 (e.name.c_str());
            if (e.isDefault) text << "  (default)";
            box.addItem (text, deviceBase + (int) i);

            if (chosen.id.isNotEmpty() && chosen.id == juce::String::fromUTF8 (e.id.c_str()))
                selected = deviceBase + (int) i;
        }

        if (selected == missingItem)
            box.addItem ("Missing: " + (chosen.name.isNotEmpty() ? chosen.name : chosen.id), missingItem);

        box.setSelectedId (selected, juce::dontSendNotification);
    }

    BridgeProcessor::Endpoint endpointFor (int id) const
    {
        if (id < deviceBase || id - deviceBase >= (int) endpoints.size()) return {};
        const auto& found = endpoints[(size_t) (id - deviceBase)];
        return { juce::String::fromUTF8 (found.id.c_str()), juce::String::fromUTF8 (found.name.c_str()) };
    }

    void chooseCapture()
    {
        const int id = captureBox.getSelectedId();
        if (id == missingItem) return;

        if (id == everythingItem || id == wholeDefaultItem)
            setParam ("source", id == everythingItem ? 0.0f : 1.0f);

        const auto e = endpointFor (id);
        if (e != proc.getCaptureEndpoint()) proc.setCaptureEndpoint (e);
        populate(); // drops a "Missing" entry once something else is chosen
    }

    void chooseSend()
    {
        const int id = sendBox.getSelectedId();
        if (id == missingItem) return;

        const auto e = endpointFor (id);
        if (e != proc.getSendEndpoint()) proc.setSendEndpoint (e);
        populate();
    }

    BridgeProcessor& proc;
    juce::TextButton captureMode { "Capture system audio" }, sendMode { "Send REAPER audio" };
    juce::Label captureFromLabel { {}, "Capture from" }, sendToLabel { {}, "Send to" },
                playbackLabel { {}, "On playback" }, captureGainLabel { {}, "Gain" }, sendGainLabel { {}, "Gain" };
    juce::ComboBox captureBox, sendBox, playbackBox;
    juce::Slider captureGain, sendGain;
    juce::ToggleButton passInput { "Mix track input with capture" }, onlyPlaying { "Only while REAPER is playing" };
    juce::TextButton refresh { "Refresh" };
    juce::Label status;
    juce::TooltipWindow tooltips { this, 500 };

    const std::array<juce::Component*, 7> captureSection { &captureFromLabel, &captureBox, &playbackLabel, &playbackBox,
                                                           &captureGainLabel, &captureGain, &passInput };
    const std::array<juce::Component*, 5> sendSection { &sendToLabel, &sendBox, &sendGainLabel, &sendGain, &onlyPlaying };

    std::unique_ptr<ComboAttachment> playbackAttachment;
    std::unique_ptr<SliderAttachment> captureGainAttachment, sendGainAttachment;
    std::unique_ptr<ButtonAttachment> passInputAttachment, onlyPlayingAttachment;

    juce::Rectangle<int> panel;
    std::vector<AudioEndpoint> endpoints;
    int shownRevision = -1, shownSource = -1;
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

    // Only the active stream(s) are listed; both appear only for a 1.0.x project with both on.
    juce::StringArray lines;
    if (capture.isRunning() || captureWanted) lines.add (capture.status());
    if (render.isRunning() || sendWanted) lines.add (render.status());
    if (lines.isEmpty()) lines.add ("Off - choose Capture system audio or Send REAPER audio");

    const auto c = getCaptureEndpoint(), d = getSendEndpoint();
    if (captureWanted && sendWanted && c.id.isNotEmpty() && c == d)
        lines.add ("WARNING: capturing the device you send to will feed back");

    return lines.joinIntoString ("\n");
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
