// What vm-softphone.exe and the mod DLL share: a status block in named shared memory, the messages the
// DLL posts to the helper, and the wording of the status banner.
#pragma once

#include <windows.h>
#include <cstdint>
#include <string>

namespace softphone
{
inline constexpr wchar_t kStatusName[] = L"Local\\VmChromaSoftphoneStatus";
inline constexpr wchar_t kMutexName[] = L"Local\\VmChromaSoftphoneHelper";
inline constexpr wchar_t kWindowClass[] = L"VmChromaSoftphoneHelper";

inline constexpr UINT kMsgSetAuto = WM_APP + 10;    // wParam: 0 or 1
inline constexpr UINT kMsgFlip = WM_APP + 11;       // same as pressing the hotkey
inline constexpr UINT kMsgShutdown = WM_APP + 12;   // restore listen mode and exit

inline constexpr uint32_t kMagic = 0x53504831, kVersion = 1;

enum Flags : uint32_t
{
    kMicOpen = 1u << 0,
    kMicUnknown = 1u << 1,
    kByHand = 1u << 2,
    kArmed = 1u << 3,
    kAuto = 1u << 4,
    kAlive = 1u << 5,
    kOpenedAutomatically = 1u << 6,
    kHotkeyTaken = 1u << 7,
    kNotSetUp = 1u << 8,
    kWaitingForMixer = 1u << 9,
};

// Written only by the helper, under a sequence lock, so the render path reads it with no lock or I/O.
struct SharedStatus
{
    uint32_t magic;
    uint32_t version;
    volatile LONG sequence;       // odd while being written
    uint32_t flags;
    uint32_t call;                // softphone::CallState
    uint64_t heartbeat;           // GetTickCount64() at the last publish
    wchar_t hotkey[8];
    wchar_t detail[200];
};

inline bool readStatus (const SharedStatus* shared, SharedStatus& out)
{
    for (int attempt = 0; attempt < 4; ++attempt)
    {
        const LONG before = shared->sequence;
        if (before & 1) continue;
        MemoryBarrier();
        out = *shared;
        MemoryBarrier();
        if (shared->sequence == before)
            return out.magic == kMagic && out.version == kVersion;
    }
    return false;
}

// What the banner says, and how loud it is.
enum class Tone { Idle, InCall, MicOpen, Warning };

struct View
{
    Tone tone = Tone::Idle;
    bool border = false;       // the red window border: anything that may be a live mic
    std::wstring line1, line2;
};

// helperAlive false means the status is old or absent. lastKnownOpen is what it last said.
inline View describe (bool helperAlive, bool lastKnownOpen, const SharedStatus& s)
{
    View v;
    const std::wstring key = s.hotkey[0] ? s.hotkey : L"F9";

    if (! helperAlive)
    {
        if (lastKnownOpen)
        {
            v.tone = Tone::MicOpen;
            v.border = true;
            v.line1 = L"MIC STATE UNKNOWN";
            v.line2 = L"Softphone helper stopped with the mic open; restarting it closes the mic";
        }
        else
        {
            v.tone = Tone::Warning;
            v.line1 = L"Softphone helper not running";
            v.line2 = key + L" and automatic mic are not working; restarting it";
        }
        return v;
    }
    if (s.flags & kWaitingForMixer)
    {
        v.line1 = L"Softphone: waiting for the mixer";
        return v;
    }
    if (s.flags & kNotSetUp)
    {
        v.tone = Tone::Warning;
        v.line1 = L"Softphone bridge not set up";
        v.line2 = L"Run Laptop-setup in the audio-bridge folder (a crash can lose setup)";
        return v;
    }

    const wchar_t* what = L"Alert not running";
    if (s.flags & kAlive)
    {
        switch (s.call)
        {
            case 1: what = L"No call"; break;
            case 2: what = L"Ringing"; break;
            case 3: what = L"Answered"; break;
            case 4: what = L"Person"; break;
            case 5: what = L"Machine"; break;
            case 6: what = L"Research call"; break;
            default: break;
        }
    }

    const std::wstring hand = (s.flags & kByHand) ? L" (by hand)" : L"";
    const std::wstring autoNote = (s.flags & kAuto) ? L"" : L"  |  Automatic off";
    const std::wstring hint = (s.flags & kHotkeyTaken) ? key + L" IS TAKEN, use the Menu"
                            : key + ((s.flags & kMicOpen) ? L" to mute" : L" to talk");

    if (s.flags & kMicUnknown)
    {
        v.tone = Tone::MicOpen;
        v.border = true;
        v.line1 = L"MIC STATE UNKNOWN  |  " + std::wstring (what) + L"  |  " + hint;
    }
    else if (s.flags & kMicOpen)
    {
        v.tone = Tone::MicOpen;
        v.border = true;
        v.line1 = L"MIC OPEN" + hand + L"  |  " + what + L"  |  " + hint + autoNote;
    }
    else
    {
        const bool inCall = (s.flags & kAlive) && (s.call == 2 || s.call == 3 || s.call == 4 || s.call == 5);
        v.tone = (s.flags & kHotkeyTaken) ? Tone::Warning : inCall ? Tone::InCall : Tone::Idle;
        v.line1 = L"Mic off" + hand + L"  |  " + what + L"  |  " + hint + autoNote;
    }
    v.line2 = s.detail;
    return v;
}
} // namespace softphone
