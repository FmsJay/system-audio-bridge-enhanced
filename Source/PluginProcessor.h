#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "Wasapi.h"

class BridgeProcessor final : public juce::AudioProcessor,
                              private juce::Timer
{
public:
    BridgeProcessor();
    ~BridgeProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    juce::String statusText() const;

    juce::AudioProcessorValueTreeState params;

private:
    void timerCallback() override;   // starts/stops the WASAPI engines off the audio thread

    LoopbackCapture capture;
    DeviceRender render;

    std::atomic<double> currentRate { 0.0 };
    double captureRate = 0.0, renderRate = 0.0;
    int captureSourceInUse = -1;

    std::vector<float> tmpL, tmpR;

    std::atomic<float>* captureOn;
    std::atomic<float>* playbackMode;
    std::atomic<float>* captureSource;
    std::atomic<float>* captureGain;
    std::atomic<float>* passInput;
    std::atomic<float>* sendOn;
    std::atomic<float>* sendGain;
    std::atomic<float>* sendOnlyPlaying;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BridgeProcessor)
};
