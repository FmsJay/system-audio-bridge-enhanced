// Every softphone mic rule, run against a simulated mixer and host: no call, mixer or network.
#include "softphone_engine.hpp"

#include <cstdio>
#include <deque>
#include <map>
#include <string>
#include <vector>

using namespace softphone;

namespace
{
int failures = 0;
#define CHECK(c) do { if (! (c)) { std::printf ("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

// What the mixer does: batches apply a little later, and can be dropped on demand.
struct StubMixer final : MixerPort
{
    std::map<std::string, float> values { { "Bus[7].Mute", 0.0f }, { "vban.outstream[0].on", 1.0f }, { "vban.instream[0].on", 0.0f } };
    std::vector<std::string>* journal = nullptr;
    int drop = 0, busyTicks = 0;
    std::deque<std::string> pending;
    bool frozen = false;   // reads fail, as if the mixer stopped answering

    bool settled() override { return pending.empty() && busyTicks == 0; }
    bool apply (const std::string& script) override
    {
        if (journal) journal->push_back ("mixer " + script);
        if (drop > 0) { --drop; return true; }   // accepted, then lost: what the README saw
        pending.push_back (script);
        return true;
    }
    std::optional<float> read (const std::string& n) override
    {
        if (frozen) return std::nullopt;
        return values[n];
    }
    void advance()
    {
        if (busyTicks > 0) --busyTicks;
        while (! pending.empty())
        {
            std::string s = pending.front();
            pending.pop_front();
            size_t p = 0;
            while ((p = s.find ('=')) != std::string::npos)
            {
                const auto start = s.find_first_not_of (' ');
                const auto end = s.find (';', p);
                values[s.substr (start, p - start)] = std::stof (s.substr (p + 1, end - p - 1));
                s.erase (0, end + 1);
            }
        }
    }
};

struct StubHost final : HostPort
{
    std::vector<std::string>* journal = nullptr;
    std::vector<std::string> sent;
    void send (const std::string& t) override { sent.push_back (t); if (journal) journal->push_back ("host " + t); }
};

struct Rig
{
    std::vector<std::string> journal;
    StubMixer mixer;
    StubHost host;
    Engine engine;
    uint64_t now = 1000;
    int count = 100;
    std::optional<std::string> file;

    explicit Rig (bool autoMode = true) : engine (mixer, host, 7, autoMode)
    {
        mixer.journal = &journal;
        host.journal = &journal;
        engine.start (now);
        run (500);
    }
    void run (uint64_t ms)
    {
        for (uint64_t t = 0; t < ms; t += 100)
        {
            now += 100;
            mixer.advance();
            engine.tick (now, file);
        }
    }
    void state (const char* name, const char* text = "")
    {
        file = std::to_string (++count) + "|" + name + "|" + text;
        run (300);
    }
    // A live monitor: the count keeps rising with the state unchanged.
    void hold (uint64_t ms)
    {
        for (uint64_t t = 0; t < ms; t += 500)
        {
            const auto b = file->find ('|');
            file = std::to_string (++count) + file->substr (b);
            run (500);
        }
    }
    void press() { engine.press (now); run (300); }
    float bus() { return mixer.values["Bus[7].Mute"]; }
    float stream() { return mixer.values["vban.outstream[0].on"]; }
    Status st() { return engine.status (now); }
    bool micReallyClosed() { return bus() > 0.5f; }
    int firstIndex (const std::string& prefix)
    {
        for (size_t i = 0; i < journal.size(); ++i)
            if (journal[i].find (prefix) != std::string::npos) return (int) i;
        return -1;
    }
    void clearJournal() { journal.clear(); host.sent.clear(); }
    // A monitor that has been heard from once, so the next line counts.
    void alive (const char* name = "idle") { file = std::to_string (++count) + "|" + name + "|"; run (200); file = std::to_string (++count) + "|" + name + "|"; run (200); }
};

void startup()
{
    Rig r;
    CHECK (r.micReallyClosed());
    CHECK (r.stream() < 0.5f);
    CHECK (r.mixer.values["vban.instream[0].on"] > 0.5f);
    CHECK (r.host.sent.size() == 2 && r.host.sent[0] == Engine::hostListen && r.host.sent[1] == Engine::hostListen);
    CHECK (! r.st().micOpen && ! r.st().micUnknown);
}

void leftoverPersonNeverOpens()
{
    Rig r;
    r.file = "500|person|leftover from a dead monitor";
    r.run (2000);
    CHECK (! r.st().micOpen);
    CHECK (r.micReallyClosed());
    CHECK (! r.st().alive);
    CHECK (r.st().detail.empty());

    // The same line again is still not a change.
    r.file = "500|person|leftover from a dead monitor";
    r.run (500);
    CHECK (! r.st().micOpen);

    // Only a changed count counts, and then the state is real.
    r.file = "501|person|hello";
    r.run (300);
    CHECK (r.st().micOpen);
    CHECK (! r.micReallyClosed());
}

void automaticFlowAndOrder()
{
    Rig r;
    r.alive();
    r.clearJournal();

    r.state ("call");
    CHECK (r.stream() > 0.5f);
    CHECK (r.micReallyClosed());
    CHECK (r.st().call == CallState::Ringing);
    CHECK (r.firstIndex (Engine::hostTalk) < 0);

    r.state ("answered");
    CHECK (r.micReallyClosed());

    r.clearJournal();
    r.state ("person", "hi there");
    CHECK (r.st().micOpen && ! r.micReallyClosed());
    CHECK (r.st().openedAutomatically);
    CHECK (r.st().detail == "hi there");
    const int hostFirst = r.firstIndex (Engine::hostTalk), unmute = r.firstIndex ("Bus[7].Mute=0");
    CHECK (hostFirst >= 0 && unmute > hostFirst);   // host first on the way in

    r.clearJournal();
    r.state ("machine");
    CHECK (! r.st().micOpen && r.micReallyClosed());
    CHECK (r.stream() > 0.5f);                      // the stream stays up for the rest of the call
    const int mute = r.firstIndex ("Bus[7].Mute=1"), hostAfter = r.firstIndex (Engine::hostListen);
    CHECK (mute >= 0 && hostAfter > mute);          // laptop first on the way out

    r.state ("person");
    CHECK (r.st().micOpen);
    r.state ("idle");
    CHECK (! r.st().micOpen && r.micReallyClosed());
    CHECK (r.stream() < 0.5f);
}

void offAndResearchClose()
{
    for (const char* closing : { "off", "research", "idle" })
    {
        Rig r;
        r.alive();
        r.state ("call");
        r.state ("person");
        CHECK (r.st().micOpen);
        r.state (closing);
        CHECK (! r.st().micOpen && r.micReallyClosed() && r.stream() < 0.5f);
    }

    // Research calls never open the mic, whatever follows in the same state.
    Rig r;
    r.alive();
    r.state ("research", "operator call");
    r.hold (1000);
    CHECK (! r.st().micOpen && r.micReallyClosed());
}

void byHandRules()
{
    Rig r;
    r.alive();
    r.state ("call");
    r.state ("person");
    CHECK (r.st().micOpen);

    r.press();                                      // jumps out by hand
    CHECK (! r.st().micOpen && r.st().byHand && r.micReallyClosed());
    r.state ("machine");
    r.state ("person");
    CHECK (! r.st().micOpen);                       // automatic leaves this call alone

    r.press();                                      // jumps in by hand
    CHECK (r.st().micOpen && r.st().byHand && ! r.micReallyClosed());
    r.state ("machine");
    CHECK (r.st().micOpen);                         // still hands off

    r.state ("idle");                               // hang-up ends it
    CHECK (! r.st().micOpen && ! r.st().byHand && r.micReallyClosed());

    // A mic opened by hand with no call closes when a call starts: every call starts off.
    r.press();
    CHECK (r.st().micOpen && r.stream() > 0.5f);    // F9 arms the stream itself
    r.state ("call");
    CHECK (! r.st().micOpen && ! r.st().byHand && r.micReallyClosed());
    r.state ("person");
    CHECK (r.st().micOpen);                         // and automatic is back for the new call
}

void staleMonitor()
{
    // Automatic mic: closes after 5 s, stream stops.
    {
        Rig r;
        r.alive();
        r.state ("call");
        r.state ("person");
        r.run (4000);
        CHECK (r.st().micOpen);
        r.run (1200);
        CHECK (! r.st().micOpen && r.micReallyClosed() && r.stream() < 0.5f);
        CHECK (! r.st().alive);
        CHECK (r.st().call == CallState::Unknown);

        // A monitor that comes back with "person" opens it again.
        r.state ("person");
        CHECK (r.st().micOpen);
    }
    // Hand mic: stays open with the monitor gone.
    {
        Rig r;
        r.alive();
        r.state ("call");
        r.press();
        CHECK (r.st().micOpen);
        r.run (8000);
        CHECK (r.st().micOpen && ! r.micReallyClosed());
        CHECK (! r.st().alive);
    }
    // F9 works with no monitor at all, and nothing opens by itself.
    {
        Rig r;
        r.run (6000);
        CHECK (! r.st().micOpen);
        r.press();
        CHECK (r.st().micOpen);
        r.press();
        CHECK (! r.st().micOpen && r.micReallyClosed());
    }
    // A rising count keeps it alive through a long conversation.
    {
        Rig r;
        r.alive();
        r.state ("call");
        r.state ("person");
        r.hold (20000);
        CHECK (r.st().micOpen && r.st().alive);
    }
}

void failedAndBadReads()
{
    // A failed read leaves the latest text unchanged, which is "no new data".
    Rig r;
    r.alive();
    r.state ("call");
    r.run (1000);
    CHECK (r.st().call == CallState::Ringing && r.stream() > 0.5f);

    for (const char* bad : { "", "garbage", "12|", "|person|x", "abc|person|x", "9999999" })
    {
        r.file = bad;
        r.run (300);
        CHECK (r.st().call == CallState::Ringing);
        CHECK (! r.st().micOpen);
    }

    // A state this build doesn't know changes nothing, but it does prove the monitor is alive.
    r.state ("call");
    r.state ("newstate", "x");
    CHECK (r.st().call == CallState::Ringing && r.st().alive && ! r.st().micOpen);

    // A transcript may contain the separator.
    r.state ("person", "a|b|c");
    CHECK (r.st().detail == "a|b|c");
}

void transcriptFollowsEveryLine()
{
    Rig r;
    r.alive();
    r.state ("call", "first");
    CHECK (r.st().detail == "first");
    r.state ("call", "second, same state");
    CHECK (r.st().detail == "second, same state");
}

void droppedBatches()
{
    // A dropped close is retried until it's real, with the screen not claiming success meanwhile.
    Rig r;
    r.alive();
    r.state ("call");
    r.state ("person");
    CHECK (! r.micReallyClosed());
    r.mixer.drop = 2;
    r.state ("machine");
    r.run (1500);
    CHECK (r.micReallyClosed());
    CHECK (! r.st().micUnknown);

    // A mixer that never takes the close shows MIC STATE UNKNOWN, and clears once it does.
    r.state ("person");
    CHECK (! r.micReallyClosed());
    r.mixer.drop = 1000;
    r.file = std::to_string (++r.count) + "|machine|";
    r.run (2500);
    CHECK (r.st().micUnknown);
    CHECK (! r.micReallyClosed());
    r.mixer.drop = 0;
    r.run (800);
    CHECK (r.micReallyClosed() && ! r.st().micUnknown);

    // Reads failing outright is unknown too, not "closed".
    r.mixer.frozen = true;
    r.run (2500);
    CHECK (r.st().micUnknown);
    r.mixer.frozen = false;
    r.run (500);
    CHECK (! r.st().micUnknown);

    // A busy mixer gets no second batch until it has settled.
    Rig s;
    s.alive();
    s.clearJournal();
    s.mixer.busyTicks = 5;
    s.press();
    CHECK (s.firstIndex ("Bus[7].Mute=0") < 0);
    s.run (600);
    CHECK (s.firstIndex ("Bus[7].Mute=0") >= 0 && ! s.micReallyClosed());
}

void hostRepeats()
{
    Rig r;
    r.alive();
    r.clearJournal();
    r.engine.press (r.now);
    CHECK (r.host.sent.size() == 1 && r.host.sent[0] == Engine::hostTalk);
    r.run (100);
    CHECK (r.host.sent.size() == 2 && r.host.sent[1] == Engine::hostTalk);
    r.run (500);
    CHECK (r.host.sent.size() == 2);

    // Closing right after opening: the pending "talk" repeat must never follow the "listen".
    r.clearJournal();
    r.engine.press (r.now);    // closes (it was open)
    r.engine.press (r.now);    // opens
    r.engine.press (r.now);    // closes, within the same 100 ms
    r.run (500);
    CHECK (r.host.sent.back() == Engine::hostListen);
    CHECK (r.host.sent[r.host.sent.size() - 2] == Engine::hostListen);
}

void automaticOff()
{
    // Off from the start: nothing arms or opens on its own; F9 still does both.
    {
        Rig r (false);
        r.alive();
        r.state ("call");
        CHECK (r.stream() < 0.5f);
        r.state ("person");
        CHECK (! r.st().micOpen && r.micReallyClosed());
        r.press();
        CHECK (r.st().micOpen && r.stream() > 0.5f && ! r.micReallyClosed());
        r.state ("machine");
        CHECK (r.st().micOpen);
        r.state ("idle");                           // a hang-up still closes
        CHECK (! r.st().micOpen && r.micReallyClosed() && r.stream() < 0.5f);
    }
    // Turning it off closes a mic it opened, and stops the stream.
    {
        Rig r;
        r.alive();
        r.state ("call");
        r.state ("person");
        CHECK (r.st().micOpen);
        r.engine.setAuto (false, r.now);
        r.run (300);
        CHECK (! r.st().micOpen && r.micReallyClosed() && r.stream() < 0.5f);
    }
    // Turning it off leaves a mic opened by hand alone.
    {
        Rig r;
        r.alive();
        r.press();
        r.engine.setAuto (false, r.now);
        r.run (300);
        CHECK (r.st().micOpen && ! r.micReallyClosed());
    }
    // Back on mid-call: it picks up at the next change of state.
    {
        Rig r (false);
        r.alive();
        r.state ("call");
        r.state ("answered");
        r.engine.setAuto (true, r.now);
        r.run (300);
        CHECK (! r.st().micOpen);
        r.state ("person");
        CHECK (r.st().micOpen);
    }
}

void externalChanges()
{
    // James mutes the bus by hand in Potato while talking: believe him, and tell the host.
    Rig r;
    r.alive();
    r.state ("call");
    r.state ("person");
    CHECK (r.st().micOpen);
    r.clearJournal();
    r.mixer.values["Bus[7].Mute"] = 1.0f;
    r.run (300);
    CHECK (! r.st().micOpen && r.st().byHand);
    CHECK (r.firstIndex (Engine::hostListen) >= 0);
    CHECK (r.micReallyClosed());

    // Something unmutes the bus while we say closed: that's a leak, so it gets muted again.
    Rig s;
    s.alive();
    s.mixer.values["Bus[7].Mute"] = 0.0f;
    s.run (600);
    CHECK (s.micReallyClosed());
}

void shutdownRestores()
{
    // Mic open at exit: back to plain listen mode, laptop before host.
    Rig r;
    r.alive();
    r.state ("call");
    r.state ("person");
    r.clearJournal();
    r.engine.beginShutdown (r.now);
    CHECK (! r.engine.shutdownConfirmed());
    r.run (300);
    CHECK (r.engine.shutdownConfirmed());
    CHECK (r.bus() < 0.5f && r.stream() < 0.5f);    // bus unmuted, stream off, as setup left them
    const int mixerFirst = r.firstIndex ("vban.outstream[0].on=0"), hostAfter = r.firstIndex (Engine::hostListen);
    CHECK (mixerFirst >= 0 && hostAfter > mixerFirst);
    CHECK (r.host.sent.size() == 2);

    // After that, nothing reopens anything.
    r.file = std::to_string (++r.count) + "|person|";
    r.run (500);
    r.engine.press (r.now);
    r.run (300);
    CHECK (r.stream() < 0.5f && ! r.st().micOpen);
    CHECK (r.host.sent.back() == Engine::hostListen);

    // A dropped restore is retried too.
    Rig s;
    s.alive();
    s.mixer.drop = 1;
    s.engine.beginShutdown (s.now);
    s.run (800);
    CHECK (s.engine.shutdownConfirmed() && s.bus() < 0.5f);
}
} // namespace

int main()
{
    struct { const char* name; void (*fn)(); } tests[] = {
        { "startup state", startup },
        { "leftover person never opens", leftoverPersonNeverOpens },
        { "automatic flow and order", automaticFlowAndOrder },
        { "off/research/idle close", offAndResearchClose },
        { "by-hand rules", byHandRules },
        { "stale monitor", staleMonitor },
        { "failed and bad reads", failedAndBadReads },
        { "transcript follows every line", transcriptFollowsEveryLine },
        { "dropped batches", droppedBatches },
        { "host repeats", hostRepeats },
        { "automatic off", automaticOff },
        { "external changes", externalChanges },
        { "shutdown restores", shutdownRestores },
    };
    for (const auto& t : tests)
    {
        const int before = failures;
        t.fn();
        std::printf ("%s %s\n", failures == before ? "ok  " : "FAIL", t.name);
    }
    std::printf (failures ? "%d CHECKS FAILED\n" : "ALL PASSED\n", failures);
    return failures ? 1 : 0;
}
