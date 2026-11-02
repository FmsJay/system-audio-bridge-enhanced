// The softphone jump-in mic rules, ported from audio-bridge\Laptop-jump.ps1. No Windows or Voicemeeter
// dependency: the mixer and the host are ports, time is passed in, so test_engine.cpp can run every
// rule without a mixer, a call or a network.
//
//   call            stream on, bus still muted; a mic left open by hand closes (every call starts off)
//   person          mic opens                   (unless flipped by hand this call, or automatic is off)
//   machine         mic closes                  (same conditions)
//   idle/off/research   mic closes, stream stops, by-hand ends
//   F9              flips the mic; automatic switching leaves it alone until call/idle/off/research
//   count stops for 5 s   a mic opened automatically closes; one opened by hand stays
//
// Opening tells the host first, then unmutes the bus. Closing mutes the bus first, then tells the host.
// VBAN-Text has no acknowledgement, so host commands go out twice, 100 ms apart. A mixer batch sent while
// the last is applying can be dropped, so every mixer change is read back and retried; a change that can't
// be confirmed within kConfirmMs shows as MIC STATE UNKNOWN.
#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace softphone
{
enum class CallState : uint32_t { Unknown, Idle, Ringing, Answered, Person, Machine, Research, Off };

struct MixerPort
{
    virtual ~MixerPort() = default;
    virtual bool settled() = 0;                                  // false while the last batch is still applying
    virtual bool apply (const std::string& script) = 0;          // false if the mixer refused it
    virtual std::optional<float> read (const std::string& name) = 0;
};

struct HostPort
{
    virtual ~HostPort() = default;
    virtual void send (const std::string& text) = 0;             // one VBAN-Text datagram, fire and forget
};

struct Status
{
    CallState call = CallState::Unknown;
    bool alive = false;        // the monitor's count changed within the last 5 s
    bool micOpen = false;
    bool micUnknown = false;   // a mixer change could not be confirmed
    bool byHand = false;
    bool armed = false;
    bool autoMode = true;
    bool openedAutomatically = false;
    std::string detail;        // latest transcript line, UTF-8
};

class Engine
{
public:
    static constexpr uint64_t kStaleMs = 5000, kRetryMs = 300, kConfirmMs = 2000, kHostRepeatMs = 100;

    Engine (MixerPort& mixerPort, HostPort& hostPort, int sendBus, bool autoMode)
        : mixer (mixerPort), host (hostPort),
          busMute ("Bus[" + std::to_string (sendBus) + "].Mute"), autoOn (autoMode) {}

    // Muted from here on, so the stream can never carry the mic by accident.
    void start (uint64_t now)
    {
        needInstream = true;
        reconcile (now, true);
        hostSend (hostListen, now);
    }

    // Call every ~100 ms. latest is the newest successful read of live-state.txt; a failed read
    // simply leaves it unchanged, so it counts as "no new data".
    void tick (uint64_t now, const std::optional<std::string>& latest)
    {
        flushHost (now);

        if (shuttingDown)
        {
            reconcile (now, false);
            return;
        }

        // Monitor gone (hard stop, crash, lost share) with no hang-up to close the mic.
        if (stale (now) && ! byHand && (talking || armed))
        {
            armed = false;   // before closing, so both go out in one batch, mute first
            setTalk (false, now);
            call = CallState::Unknown;
            detail.clear();
        }

        if (latest)
            follow (*latest, now);

        reconcile (now, false);
    }

    void press (uint64_t now)
    {
        if (shuttingDown) return;
        // Closed by hand with no call up: no hang-up will ever come to stop the stream.
        if (talking && (call == CallState::Unknown || call == CallState::Idle || call == CallState::Off || call == CallState::Research))
        {
            byHand = false;
            armed = false;   // before closing, so both go out in one batch, mute first
            setTalk (false, now);
            return;
        }
        byHand = true;
        setTalk (! talking, now);
    }

    void setAuto (bool on, uint64_t now)
    {
        autoOn = on;
        if (! on && ! byHand)
        {
            armed = false;
            setTalk (false, now);
            reconcile (now, true);
        }
    }

    // Plain listen mode, as setup left it: stream off, bus unmuted, host back to listen.
    void beginShutdown (uint64_t now)
    {
        shuttingDown = true;
        talking = false;
        armed = false;
        reconcile (now, true);
        hostSend (hostListen, now);
    }

    bool shutdownConfirmed() const { return shuttingDown && confirmed; }

    Status status (uint64_t now) const
    {
        Status s;
        s.call = call;
        s.alive = ! stale (now);
        s.micOpen = talking;
        s.micUnknown = unknown;
        s.byHand = byHand;
        s.armed = armed;
        s.autoMode = autoOn;
        s.openedAutomatically = talking && ! byHand;
        s.detail = detail;
        return s;
    }

    static constexpr const char* hostListen = "Strip[2].B3=0; Strip[2].Mute=1;";
    static constexpr const char* hostTalk = "Strip[2].B3=1; Strip[2].Mute=0;";

private:
    bool stale (uint64_t now) const { return ! lastChange || now - *lastChange >= kStaleMs; }

    void follow (const std::string& text, uint64_t now)
    {
        const auto a = text.find ('|');
        const auto b = a == std::string::npos ? a : text.find ('|', a + 1);
        if (b == std::string::npos || a == 0) return;   // not a whole line: no new data

        uint64_t count = 0;
        for (size_t i = 0; i < a; ++i)
        {
            if (text[i] < '0' || text[i] > '9') return;
            count = count * 10 + (uint64_t) (text[i] - '0');
        }

        // The first read proves nothing: a monitor that died hard leaves "person" in the file,
        // and that must never open the mic at startup.
        if (! haveCount)
        {
            haveCount = true;
            lastCount = count;
            return;
        }
        if (count == lastCount) return;
        lastCount = count;
        lastChange = now;
        detail = trimmed (text.substr (b + 1));

        const std::string name = text.substr (a + 1, b - a - 1);
        CallState next;
        if (name == "idle") next = CallState::Idle;
        else if (name == "call") next = CallState::Ringing;
        else if (name == "answered") next = CallState::Answered;
        else if (name == "person") next = CallState::Person;
        else if (name == "machine") next = CallState::Machine;
        else if (name == "research") next = CallState::Research;
        else if (name == "off") next = CallState::Off;
        else return;   // a state this build doesn't know: stay as we are

        if (next == call) return;
        call = next;

        switch (call)
        {
            case CallState::Ringing:
                byHand = false;
                setTalk (false, now);   // every call starts with the mic off
                if (autoOn) armed = true;
                break;
            case CallState::Person:
                if (autoOn && ! byHand) setTalk (true, now);
                break;
            case CallState::Machine:
                if (autoOn && ! byHand) setTalk (false, now);
                break;
            case CallState::Idle: case CallState::Off: case CallState::Research:
                byHand = false;
                armed = false;
                setTalk (false, now);
                break;
            default:
                break;
        }
    }

    static std::string trimmed (std::string s)
    {
        while (! s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ')) s.pop_back();
        return s;
    }

    // Host first on the way in and laptop first on the way out, so nothing half-open reaches the call.
    void setTalk (bool on, uint64_t now)
    {
        if (on == talking) return;
        talking = on;
        openConfirmed = false;
        if (on)
        {
            armed = true;
            hostSend (hostTalk, now);
            reconcile (now, true);
        }
        else
        {
            reconcile (now, true);
            hostSend (hostListen, now);
        }
    }

    void hostSend (const char* text, uint64_t now)
    {
        host.send (text);
        repeatText = text;   // replaces any pending repeat, so a late "talk" can't follow a "listen"
        repeatAt = now + kHostRepeatMs;
    }

    void flushHost (uint64_t now)
    {
        if (repeatText && now >= repeatAt)
        {
            host.send (repeatText);
            repeatText = nullptr;
        }
    }

    void reconcile (uint64_t now, bool force)
    {
        const bool wantMuted = shuttingDown ? false : ! talking;
        const bool wantStream = shuttingDown ? false : armed;
        const auto gotMuted = mixer.read (busMute);
        const auto gotStream = mixer.read ("vban.outstream[0].on");

        // Someone muted the bus by hand in Potato: respect it rather than fight it.
        if (! shuttingDown && talking && openConfirmed && gotMuted && *gotMuted > 0.5f)
        {
            talking = false;
            byHand = true;
            openConfirmed = false;
            hostSend (hostListen, now);
            return;
        }

        const bool busOk = gotMuted && (*gotMuted > 0.5f) == wantMuted;
        const bool streamOk = gotStream && (*gotStream > 0.5f) == wantStream;

        if (busOk && streamOk && ! needInstream)
        {
            mismatchSince.reset();
            unknown = false;
            confirmed = true;
            if (talking) openConfirmed = true;
            return;
        }

        confirmed = false;
        if (! mismatchSince) mismatchSince = now;
        if (! busOk && now - *mismatchSince >= kConfirmMs) unknown = true;

        if (! force && lastApply && now - *lastApply < kRetryMs) return;
        if (! mixer.settled()) return;

        std::string script;
        if (! busOk) script += busMute + (wantMuted ? "=1; " : "=0; ");
        if (! streamOk) script += std::string ("vban.outstream[0].on=") + (wantStream ? "1; " : "0; ");
        if (needInstream) script += "vban.instream[0].on=1; ";
        if (mixer.apply (script)) needInstream = false;
        lastApply = now;
    }

    MixerPort& mixer;
    HostPort& host;
    const std::string busMute;

    bool autoOn;
    bool talking = false, armed = false, byHand = false, unknown = false;
    bool openConfirmed = false, confirmed = false, shuttingDown = false, needInstream = false;
    CallState call = CallState::Unknown;
    std::string detail;

    bool haveCount = false;
    uint64_t lastCount = 0;
    std::optional<uint64_t> lastChange, mismatchSince, lastApply;

    const char* repeatText = nullptr;
    uint64_t repeatAt = 0;
};
} // namespace softphone
