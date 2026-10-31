// Checks the processor's endpoint state, saved-project compatibility and engine control on any
// platform, with WasapiStub.cpp standing in for the Windows engines. Build instructions are in
// tests/CMakeLists.txt.
#include "PluginProcessor.h"
#include <iostream>
#include <set>

struct BridgeProcessorTest
{
    static void stopTimer (BridgeProcessor& p)             { p.stopTimer(); }   // the test ticks by hand
    static void tick (BridgeProcessor& p)                  { p.timerCallback(); }
    static bool renderRunning (const BridgeProcessor& p)   { return p.render.isRunning(); }
    static void expireRenderRetry (BridgeProcessor& p)     { p.renderRetryAt = 0; }
    static float sourceIndex (const BridgeProcessor& p)    { return p.captureSource->load(); }
};
using T = BridgeProcessorTest;
extern std::vector<std::string> engineLog;
extern std::set<std::string> presentIds;
static int failures = 0;
#define CHECK(c) do { if (! (c)) { std::cout << "FAIL line " << __LINE__ << ": " #c "\n"; ++failures; } else std::cout << "ok: " #c "\n"; } while (0)

static juce::String xmlOf (BridgeProcessor& p)
{
    juce::MemoryBlock mb; p.getStateInformation (mb);
    return juce::AudioProcessor::getXmlFromBinary (mb.getData(), (int) mb.getSize())->toString();
}
static juce::MemoryBlock stateOf (BridgeProcessor& p) { juce::MemoryBlock mb; p.getStateInformation (mb); return mb; }
static bool logHas (const std::string& s) { return std::find (engineLog.begin(), engineLog.end(), s) != engineLog.end(); }

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI gui;

    // A project saved before endpoint selection: plain APVTS state, built by hand without the new properties.
    juce::MemoryBlock legacy;
    {
        BridgeProcessor p;
        T::stopTimer (p);
        CHECK (! xmlOf (p).contains ("DeviceId"));        // default-device state is saved unchanged
        p.params.getParameter ("source")->setValueNotifyingHost (1.0f);
        if (auto x = p.params.copyState().createXml()) juce::AudioProcessor::copyXmlToBinary (*x, legacy);
    }
    {
        BridgeProcessor p; T::stopTimer (p);
        p.setStateInformation (legacy.getData(), (int) legacy.getSize());
        CHECK (p.getCaptureEndpoint().id.isEmpty());
        CHECK (p.getSendEndpoint().id.isEmpty());
        CHECK ((int) T::sourceIndex (p) == 1);       // parameters still load
    }

    // Round trip with selections, then back to default.
    juce::MemoryBlock saved;
    {
        BridgeProcessor p; T::stopTimer (p);
        const int rev = p.getEndpointRevision();
        p.setCaptureEndpoint ({ "{dev-B}", "Headphones B" });
        p.setSendEndpoint ({ "{dev-A}", "Speakers A" });
        CHECK (p.getEndpointRevision() == rev + 2);
        saved = stateOf (p);
        CHECK (xmlOf (p).contains ("captureDeviceName=\"Headphones B\""));
    }
    {
        BridgeProcessor p; T::stopTimer (p);
        const int rev = p.getEndpointRevision();
        p.setStateInformation (saved.getData(), (int) saved.getSize());
        CHECK (p.getEndpointRevision() != rev);
        CHECK (p.getCaptureEndpoint().id == "{dev-B}" && p.getCaptureEndpoint().name == "Headphones B");
        CHECK (p.getSendEndpoint().id == "{dev-A}" && p.getSendEndpoint().name == "Speakers A");
        p.setCaptureEndpoint ({}); p.setSendEndpoint ({});
        CHECK (! xmlOf (p).contains ("DeviceId"));
        CHECK (! xmlOf (p).contains ("DeviceName"));
        // Loading the legacy state over a selection resets it to default.
        p.setSendEndpoint ({ "{dev-A}", "Speakers A" });
        p.setStateInformation (legacy.getData(), (int) legacy.getSize());
        CHECK (p.getSendEndpoint().id.isEmpty());
    }

    // Engine control: default modes, explicit endpoints, missing endpoints never rerouted.
    {
        BridgeProcessor p; T::stopTimer (p);
        p.prepareToPlay (48000, 512);
        engineLog.clear();
        T::tick (p);
        CHECK (logHas ("capture 48000 mode=0 id="));      // default: everything except REAPER
        T::tick (p);
        CHECK (engineLog.size() == 1);                     // no restart while running

        p.params.getParameter ("send")->setValueNotifyingHost (1.0f);
        T::tick (p);
        CHECK (logHas ("render 48000 id="));               // default device send, as before

        p.setCaptureEndpoint ({ "{dev-B}", "Headphones B" });
        p.setSendEndpoint ({ "{dev-B}", "Headphones B" });
        engineLog.clear();
        T::tick (p);
        CHECK (logHas ("capture 48000 mode=2 id={dev-B}"));
        CHECK (logHas ("render 48000 id={dev-B}"));
        CHECK (p.statusText().contains ("feed back"));

        presentIds.erase ("{dev-B}");
        p.setSendEndpoint ({ "{dev-gone}", "Old Interface" });
        engineLog.clear();
        T::tick (p);
        CHECK (logHas ("render 48000 id={dev-gone}"));
        CHECK (! T::renderRunning (p));
        CHECK (p.statusText().contains ("Old Interface"));
        engineLog.clear();
        T::tick (p);
        CHECK (engineLog.empty());                         // backoff: no immediate retry
        T::expireRenderRetry (p);
        T::tick (p);
        CHECK (engineLog.size() == 1 && engineLog[0] == "render 48000 id={dev-gone}");   // retried same device only

        engineLog.clear();
        p.params.getParameter ("send")->setValueNotifyingHost (0.0f);
        p.params.getParameter ("capture")->setValueNotifyingHost (0.0f);
        T::tick (p);
        CHECK (logHas ("render stop") && logHas ("capture stop"));
        CHECK (p.statusText().startsWith ("Off"));
        p.releaseResources();
    }

    // The editor: exclusive mode buttons, one section at a time, the source folded into the
    // capture menu, a missing selection shown by name, and a tooltip on every control.
    {
        BridgeProcessor p; T::stopTimer (p);
        p.setSendEndpoint ({ "{dev-gone}", "Old Interface" });
        std::unique_ptr<juce::AudioProcessorEditor> ed (p.createEditor());
        auto find = [&] (const juce::String& id) { return ed->findChildWithID (id); };
        auto* captureMode = dynamic_cast<juce::Button*> (find ("captureMode"));
        auto* sendMode = dynamic_cast<juce::Button*> (find ("sendMode"));
        auto* captureBox = dynamic_cast<juce::ComboBox*> (find ("captureBox"));
        auto* sendBox = dynamic_cast<juce::ComboBox*> (find ("sendBox"));
        CHECK (captureMode && sendMode && captureBox && sendBox);
        auto on = [&] (const char* id) { return p.params.getRawParameterValue (id)->load() > 0.5f; };

        CHECK (captureMode->getToggleState() && ! sendMode->getToggleState());   // default: capture
        CHECK (captureBox->isVisible() && ! sendBox->isVisible());
        CHECK (captureBox->getText() == "Everything except REAPER");

        sendMode->onClick();
        CHECK (on ("send") && ! on ("capture"));
        CHECK (sendBox->isVisible() && ! captureBox->isVisible());
        CHECK (sendBox->getText() == "Missing: Old Interface");

        captureMode->onClick();
        CHECK (on ("capture") && ! on ("send"));
        CHECK (captureBox->isVisible() && ! sendBox->isVisible());

        captureBox->setSelectedId (2, juce::sendNotificationSync);               // Whole default device
        CHECK ((int) T::sourceIndex (p) == 1 && p.getCaptureEndpoint().id.isEmpty());
        captureBox->setSelectedId (10, juce::sendNotificationSync);              // first listed device
        CHECK (p.getCaptureEndpoint().id == "{dev-A}" && (int) T::sourceIndex (p) == 1);
        captureBox->setSelectedId (1, juce::sendNotificationSync);               // back to Everything except REAPER
        CHECK ((int) T::sourceIndex (p) == 0 && p.getCaptureEndpoint().id.isEmpty());

        bool allTipped = true;
        for (auto* c : ed->getChildren())
            if (auto* t = dynamic_cast<juce::SettableTooltipClient*> (c))
                if (t->getTooltip().isEmpty() && ! c->getComponentID().startsWith ("juce"))
                {
                    std::cout << "  no tooltip: " << c->getName() << " " << c->getComponentID() << "\n";
                    allTipped = false;
                }
        CHECK (allTipped);
    }

    if (argc > 1) // renders both modes, with a chosen capture device and a missing send device
    {
        BridgeProcessor p; T::stopTimer (p);
        p.setSendEndpoint ({ "{dev-gone}", "Old Interface" });
        p.prepareToPlay (48000, 512);
        p.setCaptureEndpoint ({ "{dev-A}", "Speakers A" });
        T::tick (p);
        std::unique_ptr<juce::AudioProcessorEditor> ed (p.createEditor());
        juce::File out (juce::File::getCurrentWorkingDirectory().getChildFile (argv[1]));

        auto snap = [&] (const juce::File& f)
        {
            auto img = ed->createComponentSnapshot (ed->getLocalBounds(), true, 1.5f);
            f.deleteFile();
            juce::FileOutputStream os (f);
            juce::PNGImageFormat().writeImageToStream (img, os);
        };
        snap (out);
        dynamic_cast<juce::Button*> (ed->findChildWithID ("sendMode"))->onClick();
        T::tick (p);
        ed.reset (p.createEditor());   // fresh status line; the editor's own timer doesn't run here
        snap (out.getSiblingFile (out.getFileNameWithoutExtension() + "-send.png"));
        p.releaseResources();
    }

    std::cout << (failures ? "FAILURES: " : "ALL PASSED ") << failures << "\n";
    return failures ? 1 : 0;
}
