// vm-sessions: scrollable per-strip app sessions for Voicemeeter Potato, opened from the Potato menu.
// Lists every Windows audio session on the Voicemeeter playback endpoints, grouped by the strip
// each endpoint feeds, with a dB slider, a readable dB value and a mute box per app. Everything is
// reachable by keyboard: Tab/Shift+Tab between controls, arrows (1 dB) and PgUp/PgDn (6 dB) on a
// slider, Space on Mute, Home/End on a slider for 0 dB / off.
//
//   vm-sessions            open the panel (or bring the open one forward)
//   vm-sessions --dump     print the sessions as JSON lines and exit (used by the tests)
//
// It runs as its own process so a fault here never takes the mixer down.
#include <windows.h>
#include <commctrl.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <functiondiscoverykeys_devpkey.h>
#include <shellapi.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <vector>

#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace
{
constexpr wchar_t kMainClass[] = L"VmChromaAppSessions";
constexpr wchar_t kCanvasClass[] = L"VmChromaAppSessionsCanvas";
constexpr UINT WM_APP_REFRESH = WM_APP + 1;
constexpr UINT_PTR kPollTimer = 1;
constexpr int kSliderMax = 120;   // 0 = off, then -59.5 dB .. 0 dB in 0.5 dB steps
constexpr int idSoundSettings = 100, idVolumeMixer = 101;

// Our own event context, so volume changes we make aren't mistaken for outside changes.
const GUID kEventContext = { 0x6b1f7a0e, 0x4c2d, 0x4f7e, { 0x9a, 0x51, 0x3e, 0x2c, 0x77, 0x10, 0x5d, 0x21 } };

template <typename T> void release (T*& p) { if (p) { p->Release(); p = nullptr; } }

// Which strip each Voicemeeter playback endpoint feeds, in mixer order.
struct StripInfo { const wchar_t* prefix; const wchar_t* title; int order; };
const StripInfo kStrips[] = {
    { L"Voicemeeter Input (",       L"Voicemeeter Input  –  strip 6 (VAIO)",  6 },
    { L"Voicemeeter AUX Input",     L"Voicemeeter AUX  –  strip 7",          7 },
    { L"Voicemeeter VAIO3 Input",   L"Voicemeeter VAIO3  –  strip 8",        8 },
    { L"Voicemeeter In 1",          L"Stereo Input 1  –  VAIO extension In 1", 1 },
    { L"Voicemeeter In 2",          L"Stereo Input 2  –  VAIO extension In 2", 2 },
    { L"Voicemeeter In 3",          L"Stereo Input 3  –  VAIO extension In 3", 3 },
    { L"Voicemeeter In 4",          L"Stereo Input 4  –  VAIO extension In 4", 4 },
    { L"Voicemeeter In 5",          L"Stereo Input 5  –  VAIO extension In 5", 5 },
};

const StripInfo* stripFor (const std::wstring& endpointName)
{
    for (const auto& s : kStrips)
        if (endpointName.rfind (s.prefix, 0) == 0) return &s;
    return nullptr;
}

std::wstring appNameForPid (DWORD pid)
{
    std::wstring name = L"PID " + std::to_wstring (pid);
    HANDLE process = OpenProcess (PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (! process) return name;

    wchar_t path[MAX_PATH];
    DWORD size = MAX_PATH;
    if (QueryFullProcessImageNameW (process, 0, path, &size))
    {
        std::wstring file = path;
        name = file.substr (file.find_last_of (L"\\/") + 1);
        if (const auto dot = name.find_last_of (L'.'); dot != std::wstring::npos) name.resize (dot);

        // Same naming as the mixer's own list: the executable's file description when it has one.
        DWORD handle = 0;
        if (const DWORD bytes = GetFileVersionInfoSizeW (path, &handle))
        {
            std::vector<BYTE> info (bytes);
            struct { WORD lang, codepage; }* translation = nullptr;
            UINT len = 0;
            if (GetFileVersionInfoW (path, 0, bytes, info.data())
                && VerQueryValueW (info.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**> (&translation), &len)
                && len >= sizeof (*translation))
            {
                wchar_t key[64];
                swprintf_s (key, L"\\StringFileInfo\\%04x%04x\\FileDescription", translation->lang, translation->codepage);
                wchar_t* text = nullptr;
                if (VerQueryValueW (info.data(), key, reinterpret_cast<void**> (&text), &len) && len > 1 && text[0])
                    name = text;
            }
        }
    }
    CloseHandle (process);
    return name;
}

//==============================================================================
struct Session
{
    std::wstring key, endpointName, name;
    int stripOrder = 0;
    DWORD pid = 0;
    bool active = false;
    ISimpleAudioVolume* volume = nullptr;
};

// Posts a refresh when Windows creates a session, so new apps appear without waiting for the poll.
class SessionWatcher final : public IAudioSessionNotification
{
public:
    explicit SessionWatcher (HWND target) : target (target) {}
    HRESULT STDMETHODCALLTYPE QueryInterface (REFIID iid, void** out) override
    {
        if (iid == __uuidof (IUnknown) || iid == __uuidof (IAudioSessionNotification))
        {
            *out = static_cast<IAudioSessionNotification*> (this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement (&refs); }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG n = InterlockedDecrement (&refs);
        if (n == 0) delete this;
        return n;
    }
    HRESULT STDMETHODCALLTYPE OnSessionCreated (IAudioSessionControl*) override
    {
        PostMessageW (target, WM_APP_REFRESH, 0, 0);
        return S_OK;
    }

private:
    HWND target;
    LONG refs = 1;
};

class SessionSource
{
public:
    ~SessionSource()
    {
        for (auto& [id, m] : managers)
        {
            if (watcher) m->UnregisterSessionNotification (watcher);
            m->Release();
        }
        release (watcher);
        release (enumerator);
    }

    void watch (HWND target) { watcher = new SessionWatcher (target); }

    // Fresh list of non-expired sessions on Voicemeeter endpoints, sorted by strip then app.
    std::vector<Session> list()
    {
        std::vector<Session> out;
        if (! enumerator
            && FAILED (CoCreateInstance (__uuidof (MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS (&enumerator))))
            return out;

        IMMDeviceCollection* devices = nullptr;
        if (FAILED (enumerator->EnumAudioEndpoints (eRender, DEVICE_STATE_ACTIVE, &devices))) return out;
        UINT count = 0;
        devices->GetCount (&count);

        for (UINT i = 0; i < count; ++i)
        {
            IMMDevice* device = nullptr;
            if (FAILED (devices->Item (i, &device))) continue;
            const std::wstring name = endpointName (device);
            const StripInfo* strip = stripFor (name);
            LPWSTR id = nullptr;
            if (strip && SUCCEEDED (device->GetId (&id)))
            {
                if (IAudioSessionManager2* manager = managerFor (device, id))
                    addSessions (manager, *strip, id, out);
                CoTaskMemFree (id);
            }
            device->Release();
        }
        devices->Release();

        std::sort (out.begin(), out.end(), [] (const Session& a, const Session& b)
        {
            if (a.stripOrder != b.stripOrder) return a.stripOrder < b.stripOrder;
            return _wcsicmp (a.name.c_str(), b.name.c_str()) < 0;
        });
        return out;
    }

private:
    static std::wstring endpointName (IMMDevice* device)
    {
        std::wstring result;
        IPropertyStore* props = nullptr;
        if (SUCCEEDED (device->OpenPropertyStore (STGM_READ, &props)))
        {
            PROPVARIANT v;
            PropVariantInit (&v);
            if (SUCCEEDED (props->GetValue (PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR) result = v.pwszVal;
            PropVariantClear (&v);
            props->Release();
        }
        return result;
    }

    IAudioSessionManager2* managerFor (IMMDevice* device, const wchar_t* id)
    {
        if (auto it = managers.find (id); it != managers.end()) return it->second;
        IAudioSessionManager2* manager = nullptr;
        if (FAILED (device->Activate (__uuidof (IAudioSessionManager2), CLSCTX_ALL, nullptr,
                                      reinterpret_cast<void**> (&manager))))
            return nullptr;

        // Windows only sends creation notifications after the first enumeration.
        IAudioSessionEnumerator* first = nullptr;
        if (SUCCEEDED (manager->GetSessionEnumerator (&first))) first->Release();
        if (watcher) manager->RegisterSessionNotification (watcher);
        managers[id] = manager;
        return manager;
    }

    static void addSessions (IAudioSessionManager2* manager, const StripInfo& strip, const wchar_t* endpointId,
                             std::vector<Session>& out)
    {
        IAudioSessionEnumerator* sessions = nullptr;
        if (FAILED (manager->GetSessionEnumerator (&sessions))) return;
        int n = 0;
        sessions->GetCount (&n);
        const size_t firstOfEndpoint = out.size();
        bool onlyIdleSystemSounds = true;

        for (int i = 0; i < n; ++i)
        {
            IAudioSessionControl* control = nullptr;
            IAudioSessionControl2* control2 = nullptr;
            AudioSessionState state = AudioSessionStateExpired;
            if (SUCCEEDED (sessions->GetSession (i, &control))
                && SUCCEEDED (control->QueryInterface (IID_PPV_ARGS (&control2)))
                && SUCCEEDED (control2->GetState (&state)) && state != AudioSessionStateExpired)
            {
                Session s;
                s.endpointName = strip.title;
                s.stripOrder = strip.order;
                s.active = state == AudioSessionStateActive;
                control2->GetProcessId (&s.pid);

                LPWSTR instance = nullptr;
                if (SUCCEEDED (control2->GetSessionInstanceIdentifier (&instance)))
                {
                    s.key = std::wstring (endpointId) + L"|" + instance;
                    CoTaskMemFree (instance);
                }
                const bool systemSounds = control2->IsSystemSoundsSession() == S_OK;
                onlyIdleSystemSounds = onlyIdleSystemSounds && systemSounds && ! s.active;
                if (systemSounds)
                    s.name = L"System sounds";
                else
                {
                    LPWSTR display = nullptr;
                    if (SUCCEEDED (control2->GetDisplayName (&display)) && display && *display && display[0] != L'@')
                        s.name = display;
                    CoTaskMemFree (display);
                    if (s.name.empty()) s.name = appNameForPid (s.pid);
                }
                if (SUCCEEDED (control2->QueryInterface (IID_PPV_ARGS (&s.volume))))
                    out.push_back (std::move (s));
            }
            release (control2);
            release (control);
        }
        sessions->Release();

        // Every endpoint has a System sounds session. On the VAIO extension endpoints (In 1-5),
        // which carry nothing until that licence is active, don't show a group for it alone.
        if (strip.order <= 5 && onlyIdleSystemSounds)
        {
            for (size_t i = firstOfEndpoint; i < out.size(); ++i) release (out[i].volume);
            out.resize (firstOfEndpoint);
        }
    }

    IMMDeviceEnumerator* enumerator = nullptr;
    std::map<std::wstring, IAudioSessionManager2*> managers;
    SessionWatcher* watcher = nullptr;
};

//==============================================================================
int sliderFromVolume (float v)
{
    if (v <= 0.0005f) return 0;
    const double db = 20.0 * std::log10 (v);
    return std::clamp ((int) std::lround (kSliderMax + 2.0 * db), 1, kSliderMax);
}

float volumeFromSlider (int pos)
{
    return pos <= 0 ? 0.0f : (float) std::pow (10.0, (pos - kSliderMax) / 2.0 / 20.0);
}

std::wstring dbText (int pos)
{
    if (pos <= 0) return L"Off";
    wchar_t text[32];
    swprintf_s (text, L"%.1f dB", (pos - kSliderMax) / 2.0);
    return text;
}

struct Row
{
    Session session;
    HWND header = nullptr, name = nullptr, slider = nullptr, value = nullptr, mute = nullptr;
};

struct App
{
    HWND main = nullptr, canvas = nullptr, status = nullptr, title = nullptr, soundButton = nullptr, mixerButton = nullptr;
    HFONT font = nullptr, bold = nullptr;
    UINT dpi = 96;
    int scroll = 0, contentHeight = 0;
    SessionSource source;
    std::vector<Row> rows;
    std::vector<HWND> headers;
    std::wstring shownKeys;

    int px (int v) const { return MulDiv (v, (int) dpi, 96); }
} app;

void makeFonts()
{
    if (app.font) DeleteObject (app.font);
    if (app.bold) DeleteObject (app.bold);
    NONCLIENTMETRICSW m { sizeof (m) };
    SystemParametersInfoForDpi (SPI_GETNONCLIENTMETRICS, sizeof (m), &m, 0, app.dpi);
    app.font = CreateFontIndirectW (&m.lfMessageFont);
    m.lfMessageFont.lfWeight = FW_SEMIBOLD;
    app.bold = CreateFontIndirectW (&m.lfMessageFont);
}

void setFont (HWND w, HFONT f) { SendMessageW (w, WM_SETFONT, reinterpret_cast<WPARAM> (f), TRUE); }

void layoutRows()
{
    RECT rc;
    GetClientRect (app.canvas, &rc);
    const int width = rc.right, rowH = app.px (34), headH = app.px (28), pad = app.px (10);
    const int muteW = app.px (64), valueW = app.px (64), nameW = std::max (app.px (110), (width - 2 * pad) / 3);
    const int sliderW = std::max (app.px (80), width - 2 * pad - nameW - valueW - muteW - app.px (12));

    int y = app.px (4);
    HDWP dwp = BeginDeferWindowPos ((int) (app.rows.size() * 5 + app.headers.size()));
    for (auto& r : app.rows)
    {
        if (r.header)
        {
            dwp = DeferWindowPos (dwp, r.header, nullptr, pad, y - app.scroll, width - 2 * pad, headH, SWP_NOZORDER);
            y += headH;
        }
        int x = pad + app.px (8);
        const int top = y - app.scroll, h = app.px (24), cy = top + (rowH - h) / 2;
        dwp = DeferWindowPos (dwp, r.name, nullptr, x, cy, nameW - app.px (8), h, SWP_NOZORDER);
        x += nameW;
        dwp = DeferWindowPos (dwp, r.slider, nullptr, x, cy, sliderW, h, SWP_NOZORDER);
        x += sliderW + app.px (4);
        dwp = DeferWindowPos (dwp, r.value, nullptr, x, cy, valueW, h, SWP_NOZORDER);
        x += valueW + app.px (4);
        dwp = DeferWindowPos (dwp, r.mute, nullptr, x, cy, muteW, h, SWP_NOZORDER);
        y += rowH;
    }
    EndDeferWindowPos (dwp);
    app.contentHeight = y + app.px (6);

    SCROLLINFO si { sizeof (si), SIF_RANGE | SIF_PAGE | SIF_POS, 0, app.contentHeight - 1, (UINT) rc.bottom, app.scroll };
    SetScrollInfo (app.canvas, SB_VERT, &si, TRUE);
    InvalidateRect (app.canvas, nullptr, TRUE);
}

void scrollTo (int pos)
{
    RECT rc;
    GetClientRect (app.canvas, &rc);
    pos = std::clamp (pos, 0, std::max (0, app.contentHeight - (int) rc.bottom));
    if (pos == app.scroll) return;
    app.scroll = pos;
    layoutRows();
}

// Keeps the keyboard focus visible when Tab moves it to a row below or above the view. Only acts
// when focus moves, so scrolling the list with the wheel isn't pulled back to the focused row.
void revealFocus()
{
    static HWND last = nullptr;
    HWND focus = GetFocus();
    if (focus == last) return;
    last = focus;
    if (! focus || GetParent (focus) != app.canvas) return;
    RECT r, view;
    GetWindowRect (focus, &r);
    MapWindowPoints (nullptr, app.canvas, reinterpret_cast<POINT*> (&r), 2);
    GetClientRect (app.canvas, &view);
    const int margin = app.px (36);
    if (r.top < 0) scrollTo (app.scroll + r.top - margin);
    else if (r.bottom > view.bottom) scrollTo (app.scroll + r.bottom - view.bottom + margin);
}

void showValues (Row& r, bool fromSession)
{
    float v = 0.0f;
    BOOL muted = FALSE;
    r.session.volume->GetMasterVolume (&v);
    r.session.volume->GetMute (&muted);
    if (fromSession && GetCapture() != r.slider)
        SendMessageW (r.slider, TBM_SETPOS, TRUE, sliderFromVolume (v));
    const int pos = (int) SendMessageW (r.slider, TBM_GETPOS, 0, 0);
    SetWindowTextW (r.value, dbText (pos).c_str());
    SendMessageW (r.mute, BM_SETCHECK, muted ? BST_CHECKED : BST_UNCHECKED, 0);
    EnableWindow (r.name, r.session.active);
}

void clearRows()
{
    for (auto& r : app.rows)
    {
        for (HWND w : { r.header, r.name, r.slider, r.value, r.mute })
            if (w) DestroyWindow (w);
        release (r.session.volume);
    }
    app.rows.clear();
}

void refresh()
{
    auto sessions = app.source.list();
    std::wstring keys;
    for (const auto& s : sessions) keys += s.key + L";" + s.name + L"\n";

    if (keys == app.shownKeys)
    {
        // Same set of sessions: keep the controls (and focus), just pick up outside changes.
        for (size_t i = 0; i < app.rows.size(); ++i)
        {
            app.rows[i].session.active = sessions[i].active;
            showValues (app.rows[i], true);
        }
        for (auto& s : sessions) release (s.volume);
    }
    else
    {
        // Remember which control had focus, by session, so a rebuild doesn't lose the user's place.
        std::wstring focusKey;
        int focusPart = 0;
        HWND focus = GetFocus();
        for (const auto& r : app.rows)
            if (focus == r.slider || focus == r.mute)
            {
                focusKey = r.session.key;
                focusPart = focus == r.mute ? 1 : 0;
            }

        clearRows();
        const HINSTANCE inst = GetModuleHandleW (nullptr);
        std::wstring group;
        for (auto& s : sessions)
        {
            Row r;
            if (s.endpointName != group)
            {
                group = s.endpointName;
                r.header = CreateWindowExW (0, L"STATIC", group.c_str(), WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE,
                                            0, 0, 0, 0, app.canvas, nullptr, inst, nullptr);
                setFont (r.header, app.bold);
            }
            r.name = CreateWindowExW (0, L"STATIC", s.name.c_str(), WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE | SS_ENDELLIPSIS | SS_NOTIFY,
                                      0, 0, 0, 0, app.canvas, nullptr, inst, nullptr);
            r.slider = CreateWindowExW (0, TRACKBAR_CLASSW, (s.name + L" volume").c_str(),
                                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_HORZ | TBS_NOTICKS | TBS_BOTH,
                                        0, 0, 0, 0, app.canvas, nullptr, inst, nullptr);
            SendMessageW (r.slider, TBM_SETRANGE, FALSE, MAKELPARAM (0, kSliderMax));
            SendMessageW (r.slider, TBM_SETLINESIZE, 0, 2);
            SendMessageW (r.slider, TBM_SETPAGESIZE, 0, 12);
            r.value = CreateWindowExW (0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_RIGHT | SS_CENTERIMAGE,
                                       0, 0, 0, 0, app.canvas, nullptr, inst, nullptr);
            r.mute = CreateWindowExW (0, L"BUTTON", L"Mute", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                      0, 0, 0, 0, app.canvas, nullptr, inst, nullptr);
            for (HWND w : { r.name, r.slider, r.value, r.mute }) setFont (w, app.font);
            r.session = std::move (s);
            app.rows.push_back (std::move (r));
        }
        for (size_t i = 0; i < app.rows.size(); ++i)
        {
            SetWindowLongPtrW (app.rows[i].slider, GWLP_USERDATA, (LONG_PTR) i + 1);
            SetWindowLongPtrW (app.rows[i].mute, GWLP_USERDATA, (LONG_PTR) i + 1);
            showValues (app.rows[i], true);
            if (! focusKey.empty() && app.rows[i].session.key == focusKey)
                SetFocus (focusPart ? app.rows[i].mute : app.rows[i].slider);
        }
        app.shownKeys = keys;
        layoutRows();
    }

    wchar_t status[128];
    swprintf_s (status, app.rows.empty() ? L"No apps are playing to a Voicemeeter input."
                                         : L"%zu app session%s on Voicemeeter inputs", app.rows.size(), app.rows.size() == 1 ? L"" : L"s");
    SetWindowTextW (app.status, status);
}

Row* rowOf (HWND control)
{
    const auto i = (size_t) GetWindowLongPtrW (control, GWLP_USERDATA);
    return i >= 1 && i <= app.rows.size() ? &app.rows[i - 1] : nullptr;
}

LRESULT CALLBACK canvasProc (HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
        case WM_HSCROLL:
            if (Row* r = rowOf (reinterpret_cast<HWND> (lParam)))
            {
                const int pos = (int) SendMessageW (r->slider, TBM_GETPOS, 0, 0);
                r->session.volume->SetMasterVolume (volumeFromSlider (pos), &kEventContext);
                SetWindowTextW (r->value, dbText (pos).c_str());
            }
            return 0;

        case WM_COMMAND:
            if (HIWORD (wParam) == BN_CLICKED)
                if (Row* r = rowOf (reinterpret_cast<HWND> (lParam)))
                    r->session.volume->SetMute (SendMessageW (r->mute, BM_GETCHECK, 0, 0) == BST_CHECKED, &kEventContext);
            return 0;

        case WM_VSCROLL:
        {
            SCROLLINFO si { sizeof (si), SIF_ALL };
            GetScrollInfo (hwnd, SB_VERT, &si);
            int pos = si.nPos;
            switch (LOWORD (wParam))
            {
                case SB_LINEUP: pos -= app.px (34); break;
                case SB_LINEDOWN: pos += app.px (34); break;
                case SB_PAGEUP: pos -= (int) si.nPage; break;
                case SB_PAGEDOWN: pos += (int) si.nPage; break;
                case SB_THUMBTRACK: case SB_THUMBPOSITION: pos = si.nTrackPos; break;
                case SB_TOP: pos = 0; break;
                case SB_BOTTOM: pos = app.contentHeight; break;
            }
            scrollTo (pos);
            return 0;
        }

        case WM_MOUSEWHEEL:
            scrollTo (app.scroll - MulDiv (GET_WHEEL_DELTA_WPARAM (wParam), app.px (34) * 3, WHEEL_DELTA));
            return 0;

        case WM_SIZE:
            scrollTo (app.scroll);
            layoutRows();
            return 0;

        case WM_CTLCOLORSTATIC:
            SetBkMode (reinterpret_cast<HDC> (wParam), TRANSPARENT);
            return reinterpret_cast<LRESULT> (GetSysColorBrush (COLOR_WINDOW));
    }
    return DefWindowProcW (hwnd, msg, wParam, lParam);
}

void layoutMain()
{
    RECT rc;
    GetClientRect (app.main, &rc);
    const int pad = app.px (10), barH = app.px (28), statusH = app.px (24), buttonW = app.px (118);
    MoveWindow (app.mixerButton, rc.right - pad - buttonW, pad, buttonW, barH, TRUE);
    MoveWindow (app.soundButton, rc.right - 2 * pad - 2 * buttonW + app.px (4), pad, buttonW, barH, TRUE);
    MoveWindow (app.title, pad, pad, std::max (0, (int) rc.right - 2 * buttonW - 4 * pad), barH, TRUE);
    MoveWindow (app.canvas, 0, pad * 2 + barH, rc.right, std::max (0, (int) rc.bottom - pad * 3 - barH - statusH), TRUE);
    MoveWindow (app.status, pad, rc.bottom - statusH - app.px (4), rc.right - 2 * pad, statusH, TRUE);
}

void openShell (const wchar_t* target)
{
    if ((INT_PTR) ShellExecuteW (app.main, L"open", target, nullptr, nullptr, SW_SHOWNORMAL) <= 32)
        MessageBoxW (app.main, L"Windows could not open the selected audio settings.", L"App Sessions", MB_OK | MB_ICONERROR);
}

LRESULT CALLBACK mainProc (HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
        case WM_CREATE:
        {
            app.main = hwnd;
            app.dpi = GetDpiForWindow (hwnd);
            makeFonts();
            const HINSTANCE inst = GetModuleHandleW (nullptr);
            app.title = CreateWindowExW (0, L"STATIC", L"Apps by Voicemeeter strip",
                                         WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE, 0, 0, 0, 0, hwnd, nullptr, inst, nullptr);
            app.soundButton = CreateWindowExW (0, L"BUTTON", L"&Sound settings", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                               0, 0, 0, 0, hwnd, (HMENU) (INT_PTR) idSoundSettings, inst, nullptr);
            app.mixerButton = CreateWindowExW (0, L"BUTTON", L"Volume &mixer", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                               0, 0, 0, 0, hwnd, (HMENU) (INT_PTR) idVolumeMixer, inst, nullptr);
            app.canvas = CreateWindowExW (WS_EX_CONTROLPARENT, kCanvasClass, L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_CLIPCHILDREN,
                                          0, 0, 0, 0, hwnd, nullptr, inst, nullptr);
            app.status = CreateWindowExW (0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE,
                                          0, 0, 0, 0, hwnd, nullptr, inst, nullptr);
            for (HWND w : { app.soundButton, app.mixerButton, app.status }) setFont (w, app.font);
            setFont (app.title, app.bold);
            app.source.watch (hwnd);
            SetTimer (hwnd, kPollTimer, 1000, nullptr);
            layoutMain();
            refresh();
            return 0;
        }

        case WM_SIZE:
            layoutMain();
            return 0;

        case WM_GETMINMAXINFO:
            reinterpret_cast<MINMAXINFO*> (lParam)->ptMinTrackSize = { app.px (380), app.px (220) };
            return 0;

        case WM_DPICHANGED:
        {
            app.dpi = HIWORD (wParam);
            makeFonts();
            for (HWND w : { app.soundButton, app.mixerButton, app.status }) setFont (w, app.font);
            setFont (app.title, app.bold);
            app.shownKeys.clear();   // rebuild rows with the new fonts
            const auto* r = reinterpret_cast<const RECT*> (lParam);
            SetWindowPos (hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            refresh();
            return 0;
        }

        case WM_TIMER:
        case WM_APP_REFRESH:
            refresh();
            return 0;

        case WM_COMMAND:
            if (LOWORD (wParam) == idSoundSettings) openShell (L"ms-settings:sound");
            if (LOWORD (wParam) == idVolumeMixer) openShell (L"ms-settings:apps-volume");
            if (LOWORD (wParam) == IDCANCEL) DestroyWindow (hwnd);   // Esc
            return 0;

        case WM_CTLCOLORSTATIC:
            SetBkMode (reinterpret_cast<HDC> (wParam), TRANSPARENT);
            return reinterpret_cast<LRESULT> (GetSysColorBrush (COLOR_BTNFACE));

        case WM_DESTROY:
            KillTimer (hwnd, kPollTimer);
            clearRows();
            PostQuitMessage (0);
            return 0;
    }
    return DefWindowProcW (hwnd, msg, wParam, lParam);
}

std::wstring jsonString (const std::wstring& s)
{
    std::wstring out = L"\"";
    for (wchar_t c : s)
    {
        if (c == L'"' || c == L'\\') out += L'\\';
        if (c >= 32) out += c;
    }
    return out + L"\"";
}

// One JSON object per line, UTF-8, on the inherited stdout (a pipe or file from a script), or on
// the parent console when run by hand.
int dump()
{
    std::wstring text;
    SessionSource source;
    for (auto& s : source.list())
    {
        float v = 0.0f;
        BOOL muted = FALSE;
        s.volume->GetMasterVolume (&v);
        s.volume->GetMute (&muted);
        wchar_t numbers[160];
        swprintf_s (numbers, L", \"volume\": %.4f, \"slider\": %d, \"mute\": %s, \"active\": %s}\n",
                    v, sliderFromVolume (v), muted ? L"true" : L"false", s.active ? L"true" : L"false");
        text += L"{\"strip\": " + jsonString (s.endpointName) + L", \"pid\": " + std::to_wstring (s.pid)
              + L", \"name\": " + jsonString (s.name) + numbers;
        release (s.volume);
    }

    const int bytes = WideCharToMultiByte (CP_UTF8, 0, text.c_str(), (int) text.size(), nullptr, 0, nullptr, nullptr);
    std::string utf8 ((size_t) bytes, '\0');
    WideCharToMultiByte (CP_UTF8, 0, text.c_str(), (int) text.size(), utf8.data(), bytes, nullptr, nullptr);

    HANDLE out = GetStdHandle (STD_OUTPUT_HANDLE);
    if ((out == nullptr || out == INVALID_HANDLE_VALUE) && AttachConsole (ATTACH_PARENT_PROCESS))
        out = CreateFileW (L"CONOUT$", GENERIC_WRITE, FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    DWORD written = 0;
    return out && out != INVALID_HANDLE_VALUE && WriteFile (out, utf8.data(), (DWORD) utf8.size(), &written, nullptr) ? 0 : 1;
}
} // namespace

int WINAPI wWinMain (HINSTANCE inst, HINSTANCE, LPWSTR cmdLine, int show)
{
    SetProcessDpiAwarenessContext (DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    if (wcsstr (cmdLine, L"--dump"))
    {
        CoInitializeEx (nullptr, COINIT_MULTITHREADED);
        const int rc = dump();
        CoUninitialize();
        return rc;
    }

    if (HWND existing = FindWindowW (kMainClass, nullptr))
    {
        ShowWindow (existing, IsIconic (existing) ? SW_RESTORE : SW_SHOW);
        SetForegroundWindow (existing);
        return 0;
    }

    CoInitializeEx (nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX icc { sizeof (icc), ICC_BAR_CLASSES | ICC_STANDARD_CLASSES };
    InitCommonControlsEx (&icc);

    WNDCLASSW canvas {};
    canvas.lpfnWndProc = canvasProc;
    canvas.hInstance = inst;
    canvas.hCursor = LoadCursorW (nullptr, IDC_ARROW);
    canvas.hbrBackground = GetSysColorBrush (COLOR_WINDOW);
    canvas.lpszClassName = kCanvasClass;
    RegisterClassW (&canvas);

    WNDCLASSW main {};
    main.lpfnWndProc = mainProc;
    main.hInstance = inst;
    main.hCursor = LoadCursorW (nullptr, IDC_ARROW);
    main.hbrBackground = GetSysColorBrush (COLOR_BTNFACE);
    main.hIcon = LoadIconW (nullptr, IDI_APPLICATION);
    main.lpszClassName = kMainClass;
    RegisterClassW (&main);

    const UINT dpi = GetDpiForSystem();
    HWND hwnd = CreateWindowExW (WS_EX_CONTROLPARENT, kMainClass, L"Voicemeeter App Sessions",
                                 WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                 MulDiv (520, (int) dpi, 96), MulDiv (480, (int) dpi, 96), nullptr, nullptr, inst, nullptr);
    ShowWindow (hwnd, show);

    MSG m;
    while (GetMessageW (&m, nullptr, 0, 0) > 0)
    {
        // Esc closes; Home/End on a slider jump to 0 dB / off.
        if (m.message == WM_KEYDOWN && m.wParam == VK_ESCAPE) { DestroyWindow (hwnd); continue; }
        if (m.message == WM_KEYDOWN && (m.wParam == VK_HOME || m.wParam == VK_END))
            if (Row* r = rowOf (m.hwnd); r && m.hwnd == r->slider)
            {
                SendMessageW (r->slider, TBM_SETPOS, TRUE, m.wParam == VK_HOME ? kSliderMax : 0);
                SendMessageW (app.canvas, WM_HSCROLL, SB_THUMBPOSITION, reinterpret_cast<LPARAM> (r->slider));
                continue;
            }
        if (! IsDialogMessageW (hwnd, &m))
        {
            TranslateMessage (&m);
            DispatchMessageW (&m);
        }
        revealFocus();
    }
    CoUninitialize();
    return 0;
}
