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
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { "send", 1 }, "Send output to default device", false));
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
        setSize (480, generic.getHeight() + 32);
        startTimerHz (4);
        timerCallback();
    }

    void resized() override
    {
        auto r = getLocalBounds();
        status.setBounds (r.removeFromBottom (32).reduced (8, 4));
        generic.setBounds (r);
    }

private:
    void timerCallback() override { status.setText (proc.statusText(), juce::dontSendNotification); }

    BridgeProcessor& proc;
    juce::GenericAudioProcessorEditor generic;
    juce::Label status;
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

void BridgeProcessor::timerCallback()
{
    const double rate = currentRate.load();
    const int source = (int) captureSource->load();

    const bool wantCapture = rate > 0.0 && captureOn->load() > 0.5f;
    if (! wantCapture && capture.isRunning())
        capture.stop();
    else if (wantCapture && (! capture.isRunning() || rate != captureRate || source != captureSourceInUse))
    {
        capture.start ((int) rate, source == 0 ? CaptureSource::excludeHostProcess : CaptureSource::wholeDefaultDevice);
        captureRate = rate;
        captureSourceInUse = source;
    }

    const bool wantSend = rate > 0.0 && sendOn->load() > 0.5f;
    if (! wantSend && render.isRunning())
        render.stop();
    else if (wantSend && (! render.isRunning() || rate != renderRate))
    {
        render.start ((int) rate);
        renderRate = rate;
    }
}

juce::String BridgeProcessor::statusText() const
{
    juce::String s = capture.isRunning() || captureOn->load() > 0.5f ? juce::String (capture.status()) : "Capture off";
    s << "  |  Send: " << (render.isRunning() ? "on" : "off");
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

void BridgeProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    if (auto xml = params.copyState().createXml())
        copyXmlToBinary (*xml, dest);
}

void BridgeProcessor::setStateInformation (const void* data, int size)
{
    if (auto xml = getXmlFromBinary (data, size))
        if (xml->hasTagName (params.state.getType()))
            params.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new BridgeProcessor();
}
