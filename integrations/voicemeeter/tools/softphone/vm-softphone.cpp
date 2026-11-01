// vm-softphone: runs the softphone jump-in for the Potato mod. The mod DLL starts it when the per-PC
// setting is on and it leaves with the mixer; it has no window, console or startup item of its own.
//
// It's a process rather than code in the DLL so that a stalled share or a fault here can't reach the
// mixer's threads, and so that if the mixer dies it can still tell the host to go back to listen.
// Status goes to shared memory for the banner. The transcript is never written to disk.
//
//   vm-softphone --parent PID --mixer-hwnd HWND --host IP --state PATH
//                [--port 6980] [--stream Command1] [--bus 7] [--mic-strip 0] [--hotkey F9] [--auto 0|1]
//                [--log PATH] [--instance NAME] [--stub-mixer]
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shellapi.h>
#include <shobjidl.h>

#include <atomic>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "softphone_engine.hpp"
#include "softphone_protocol.hpp"

using namespace softphone;

namespace
{
constexpr UINT_PTR kTimer = 1;
constexpr int kHotkeyId = 1;
constexpr uint64_t kRecheckMs = 5000, kShutdownMs = 1200;
const char* const kBuses[] = { "A1", "A2", "A3", "A4", "A5", "B1", "B2", "B3" };

struct Options
{
    DWORD parent = 0;
    HWND mixerHwnd = nullptr;
    std::string host, stream = "Command1";
    int port = 6980, bus = 7, micStrip = 0;
    std::wstring hotkey = L"F9", state, log, instance;
    bool autoMode = true, stub = false;
};

std::string utf8 (const std::wstring& w)
{
    const int n = WideCharToMultiByte (CP_UTF8, 0, w.c_str(), (int) w.size(), nullptr, 0, nullptr, nullptr);
    std::string s ((size_t) n, '\0');
    WideCharToMultiByte (CP_UTF8, 0, w.c_str(), (int) w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring wide (const std::string& s)
{
    const int n = MultiByteToWideChar (CP_UTF8, 0, s.c_str(), (int) s.size(), nullptr, 0);
    std::wstring w ((size_t) n, L'\0');
    MultiByteToWideChar (CP_UTF8, 0, s.c_str(), (int) s.size(), w.data(), n);
    return w;
}

// Mixer and host actions only. Never the transcript.
class Log
{
public:
    void open (const std::wstring& path)
    {
        if (! path.empty()) file = CreateFileW (path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, 0, nullptr);
    }
    void write (const std::string& text)
    {
        if (file == INVALID_HANDLE_VALUE) return;
        SYSTEMTIME t;
        GetLocalTime (&t);
        char stamp[32];
        const int n = sprintf_s (stamp, "%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
        const std::string line = std::string (stamp, (size_t) n) + text + "\r\n";
        DWORD written = 0;
        WriteFile (file, line.data(), (DWORD) line.size(), &written, nullptr);
    }
private:
    HANDLE file = INVALID_HANDLE_VALUE;
} logFile;

class UdpHost final : public HostPort
{
public:
    UdpHost (const std::string& ip, int port, const std::string& stream) : name (stream)
    {
        WSADATA wsa;
        WSAStartup (MAKEWORD (2, 2), &wsa);
        sock = socket (AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        address.sin_family = AF_INET;
        address.sin_port = htons ((u_short) port);
        inet_pton (AF_INET, ip.c_str(), &address.sin_addr);
        name.resize (16, '\0');
    }
    // VBAN-Text, as audio-bridge\vban-text.ps1 sends it.
    void send (const std::string& text) override
    {
        std::vector<char> packet { 'V', 'B', 'A', 'N', 0x52, 0, 0, 0x10 };
        packet.insert (packet.end(), name.begin(), name.end());
        for (int i = 0; i < 4; ++i) packet.push_back ((char) ((frame >> (8 * i)) & 0xFF));
        ++frame;
        packet.insert (packet.end(), text.begin(), text.end());
        sendto (sock, packet.data(), (int) packet.size(), 0, reinterpret_cast<const sockaddr*> (&address), sizeof (address));
        logFile.write ("host " + text);
    }
private:
    SOCKET sock = INVALID_SOCKET;
    sockaddr_in address {};
    std::string name;
    uint32_t frame = GetTickCount();
};

struct BridgeMixer : MixerPort
{
    virtual bool connect() = 0;               // true once the mixer answers
    virtual bool setUp (int bus, int micStrip) = 0;
    virtual void disconnect() {}
};

// The real mixer, through the Remote API, exactly as the audio-bridge scripts use it.
class RemoteMixer final : public BridgeMixer
{
public:
    bool connect() override
    {
        if (! dll)
        {
            wchar_t path[MAX_PATH];
            GetEnvironmentVariableW (L"ProgramFiles(x86)", path, MAX_PATH);
            dll = LoadLibraryW ((std::wstring (path) + L"\\VB\\Voicemeeter\\VoicemeeterRemote64.dll").c_str());
            if (! dll) return false;
            login = bind<long (__stdcall*)()> ("VBVMR_Login");
            logout = bind<long (__stdcall*)()> ("VBVMR_Logout");
            dirty = bind<long (__stdcall*)()> ("VBVMR_IsParametersDirty");
            type = bind<long (__stdcall*)(long*)> ("VBVMR_GetVoicemeeterType");
            getFloat = bind<long (__stdcall*)(const char*, float*)> ("VBVMR_GetParameterFloat");
            getString = bind<long (__stdcall*)(const char*, wchar_t*)> ("VBVMR_GetParameterStringW");
            setParameters = bind<long (__stdcall*)(const char*)> ("VBVMR_SetParameters");
        }
        if (! loggedIn)
        {
            if (! login || login() < 0) return false;
            loggedIn = true;
        }
        long kind = 0;
        if (type (&kind) != 0) return false;
        dirty();
        return true;
    }
    bool settled() override { return dirty() == 0; }
    bool apply (const std::string& script) override
    {
        const bool ok = setParameters (script.c_str()) == 0;
        logFile.write ((ok ? "mixer " : "mixer REFUSED ") + script);
        return ok;
    }
    std::optional<float> read (const std::string& name) override
    {
        // The cache updates on the poll after the one that reports a change.
        for (int i = 0; i < 20 && dirty() > 0; ++i) {}
        float value = 0.0f;
        if (getFloat (name.c_str(), &value) != 0) return std::nullopt;
        return value;
    }
    bool setUp (int bus, int micStrip) override
    {
        wchar_t in[512] = {}, out[512] = {};
        const auto enabled = read ("vban.Enable");
        const auto route = read ("Strip[" + std::to_string (micStrip) + "]." + kBuses[bus]);
        return enabled && *enabled > 0.5f && route && *route > 0.5f
               && getString ("vban.instream[0].name", in) == 0 && std::wstring (in) == L"CallToLaptop"
               && getString ("vban.outstream[0].name", out) == 0 && std::wstring (out) == L"LaptopMic";
    }
    void disconnect() override { if (loggedIn) logout(); loggedIn = false; }

private:
    template <typename F> F bind (const char* name) { return reinterpret_cast<F> (GetProcAddress (dll, name)); }
    HMODULE dll = nullptr;
    bool loggedIn = false;
    long (__stdcall* login)() = nullptr;
    long (__stdcall* logout)() = nullptr;
    long (__stdcall* dirty)() = nullptr;
    long (__stdcall* type)(long*) = nullptr;
    long (__stdcall* getFloat)(const char*, float*) = nullptr;
    long (__stdcall* getString)(const char*, wchar_t*) = nullptr;
    long (__stdcall* setParameters)(const char*) = nullptr;
};

// Test double for --stub-mixer: holds values in memory and never touches Voicemeeter.
class StubMixer final : public BridgeMixer
{
public:
    bool connect() override { return true; }
    bool setUp (int, int) override { return GetEnvironmentVariableW (L"VM_SOFTPHONE_STUB_NOT_SET_UP", nullptr, 0) == 0; }
    bool settled() override { return true; }
    bool apply (const std::string& script) override
    {
        logFile.write ("mixer " + script);
        std::string s = script;
        size_t eq;
        while ((eq = s.find ('=')) != std::string::npos)
        {
            const auto start = s.find_first_not_of (' ');
            const auto end = s.find (';', eq);
            values[s.substr (start, eq - start)] = std::stof (s.substr (eq + 1, end - eq - 1));
            s.erase (0, end + 1);
        }
        return true;
    }
    std::optional<float> read (const std::string& name) override { return values[name]; }
private:
    std::map<std::string, float> values;
};

// live-state.txt lives on a share that can stall for seconds, so only this thread ever touches it.
// A failed read keeps the last good text, which counts as "no new data".
class StateReader
{
public:
    explicit StateReader (std::wstring statePath) : path (std::move (statePath))
    {
        std::thread ([this] { run(); }).detach();
    }
    std::optional<std::string> latest()
    {
        const std::lock_guard<std::mutex> lock (mutex);
        return text;
    }
private:
    void run()
    {
        for (;;)
        {
            HANDLE f = CreateFileW (path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (f != INVALID_HANDLE_VALUE)
            {
                char buffer[4096];
                DWORD got = 0;
                const BOOL ok = ReadFile (f, buffer, sizeof (buffer), &got, nullptr);
                CloseHandle (f);
                if (ok && got > 0)
                {
                    const std::lock_guard<std::mutex> lock (mutex);
                    text = std::string (buffer, got);
                }
            }
            Sleep (120);
        }
    }
    std::wstring path;
    std::mutex mutex;
    std::optional<std::string> text;
};

// Turns the mixer's taskbar button red while the mic may be live, so it shows with Potato minimized or
// covered. On its own thread because a busy Explorer must not delay F9.
class TaskbarAlert
{
public:
    explicit TaskbarAlert (HWND window) : hwnd (window)
    {
        if (hwnd) worker = std::thread ([this] { run(); });
    }
    void set (bool on) { wanted = on; }
    void finish()
    {
        if (! hwnd) return;
        wanted = false;
        stop = true;
        worker.join();
    }
private:
    // A solid red disc with a white ring, drawn here so no icon file has to ship with the helper.
    static HICON makeBadge()
    {
        constexpr int size = 32;
        BITMAPINFO info {};
        info.bmiHeader = { sizeof (BITMAPINFOHEADER), size, -size, 1, 32, BI_RGB };
        void* bits = nullptr;
        HBITMAP color = CreateDIBSection (nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        HBITMAP mask = CreateBitmap (size, size, 1, 1, nullptr);
        auto* pixels = static_cast<uint32_t*> (bits);
        const float centre = (size - 1) / 2.0f;
        for (int y = 0; y < size; ++y)
            for (int x = 0; x < size; ++x)
            {
                const float dx = (float) x - centre, dy = (float) y - centre, d2 = dx * dx + dy * dy;
                uint32_t pixel = 0;
                if (d2 <= 15.5f * 15.5f) pixel = 0xFFFFFFFFu;
                if (d2 <= 12.5f * 12.5f) pixel = 0xFFE01010u;
                pixels[y * size + x] = pixel;
            }
        ICONINFO icon { TRUE, 0, 0, mask, color };
        HICON result = CreateIconIndirect (&icon);
        DeleteObject (color);
        DeleteObject (mask);
        return result;
    }

    void run()
    {
        CoInitializeEx (nullptr, COINIT_APARTMENTTHREADED);
        ITaskbarList3* bar = nullptr;
        if (SUCCEEDED (CoCreateInstance (__uuidof (TaskbarList), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS (&bar))) && bar->HrInit() == S_OK)
        {
            bool shown = false;
            int sinceRefresh = 0;
            HICON badge = makeBadge();
            // A helper that died with the mic open may have left the button red.
            bar->SetProgressState (hwnd, TBPF_NOPROGRESS);
            bar->SetOverlayIcon (hwnd, nullptr, nullptr);
            for (;;)
            {
                const bool on = wanted;
                // Refreshed while on, so it comes back if Explorer restarts.
                if (on != shown || (on && ++sinceRefresh >= 10))
                {
                    if (on)
                    {
                        bar->SetProgressState (hwnd, TBPF_ERROR);
                        bar->SetProgressValue (hwnd, 1, 1);
                        bar->SetOverlayIcon (hwnd, badge, L"Softphone mic open");
                    }
                    else
                    {
                        bar->SetProgressState (hwnd, TBPF_NOPROGRESS);
                        bar->SetOverlayIcon (hwnd, nullptr, nullptr);
                    }
                    shown = on;
                    sinceRefresh = 0;
                }
                if (stop && ! on) break;
                Sleep (200);
            }
            bar->Release();
        }
        CoUninitialize();
    }
    HWND hwnd;
    std::atomic<bool> wanted { false }, stop { false };
    std::thread worker;
};

UINT hotkeyCode (const std::wstring& name)
{
    if (name.size() >= 2 && (name[0] == L'F' || name[0] == L'f'))
    {
        const int n = _wtoi (name.c_str() + 1);
        if (n >= 1 && n <= 24) return VK_F1 + (UINT) n - 1;
    }
    return 0;
}

class Helper
{
public:
    Helper (Options o, BridgeMixer& m, HostPort& h)
        : options (std::move (o)), mixer (m), host (h), engine (m, h, options.bus, options.autoMode),
          reader (options.state), taskbar (options.mixerHwnd), autoMode (options.autoMode)
    {
        const std::wstring name = kStatusName + options.instance;
        mapping = CreateFileMappingW (INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof (SharedStatus), name.c_str());
        shared = static_cast<SharedStatus*> (MapViewOfFile (mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof (SharedStatus)));
        parent = options.parent ? OpenProcess (SYNCHRONIZE, FALSE, options.parent) : nullptr;
    }

    void attach (HWND window)
    {
        hwnd = window;
        bindHotkey();
    }

    void onTimer()
    {
        const uint64_t now = GetTickCount64();

        // The mixer is gone, so there's no bus to restore; the host must still go back to listen.
        if (parent && WaitForSingleObject (parent, 0) == WAIT_OBJECT_0)
        {
            logFile.write ("mixer process gone");
            host.send (Engine::hostListen);
            Sleep ((DWORD) Engine::kHostRepeatMs);
            host.send (Engine::hostListen);
            finish();
            return;
        }

        if (! hotkeyBound && now - lastHotkeyTry >= kRecheckMs) bindHotkey();

        switch (phase)
        {
            case Phase::Connecting:
                if (now - lastCheck >= 1000)
                {
                    lastCheck = now;
                    if (mixer.connect())
                    {
                        phase = Phase::CheckingSetup;
                        lastCheck = 0;
                    }
                }
                break;
            case Phase::CheckingSetup:
                // Nothing on the mixer is touched until it's really set up as the bridge's laptop side.
                if (now - lastCheck >= kRecheckMs)
                {
                    lastCheck = now;
                    setUp = mixer.setUp (options.bus, options.micStrip);
                    if (setUp)
                    {
                        logFile.write ("started");
                        engine.start (now);
                        phase = Phase::Running;
                    }
                }
                break;
            case Phase::Running:
                engine.tick (now, reader.latest());
                if (now - lastCheck >= kRecheckMs)
                {
                    lastCheck = now;
                    setUp = mixer.setUp (options.bus, options.micStrip);
                }
                break;
            case Phase::Stopping:
                engine.tick (now, std::nullopt);
                if (engine.shutdownConfirmed() || now - stopAt >= kShutdownMs)
                {
                    logFile.write (engine.shutdownConfirmed() ? "stopped, confirmed" : "stopped, NOT confirmed");
                    finish();
                    return;
                }
                break;
        }
        publish (now);
    }

    void flip()
    {
        if (phase == Phase::Running && setUp) engine.press (GetTickCount64());
        publish (GetTickCount64());
    }
    void setAuto (bool on)
    {
        autoMode = on;
        if (phase == Phase::Running) engine.setAuto (on, GetTickCount64());
        publish (GetTickCount64());
    }
    void shutdown()
    {
        if (phase == Phase::Running)
        {
            stopAt = GetTickCount64();
            phase = Phase::Stopping;
            engine.beginShutdown (stopAt);
        }
        else
            finish();
    }

private:
    enum class Phase { Connecting, CheckingSetup, Running, Stopping };

    void bindHotkey()
    {
        lastHotkeyTry = GetTickCount64();
        const UINT code = hotkeyCode (options.hotkey);
        hotkeyBound = code && RegisterHotKey (hwnd, kHotkeyId, MOD_NOREPEAT, code);
    }

    void publish (uint64_t now)
    {
        const Status s = engine.status (now);
        uint32_t flags = 0;
        if (phase == Phase::Connecting) flags |= kWaitingForMixer;
        if (phase == Phase::CheckingSetup || (phase == Phase::Running && ! setUp)) flags |= kNotSetUp;
        if (! hotkeyBound) flags |= kHotkeyTaken;
        if (s.micOpen) flags |= kMicOpen;
        if (s.micUnknown) flags |= kMicUnknown;
        if (s.byHand) flags |= kByHand;
        if (s.armed) flags |= kArmed;
        if (autoMode) flags |= kAuto;
        if (s.alive) flags |= kAlive;
        if (s.openedAutomatically) flags |= kOpenedAutomatically;

        taskbar.set (s.micOpen || s.micUnknown);

        InterlockedIncrement (&shared->sequence);
        MemoryBarrier();
        shared->magic = kMagic;
        shared->version = kVersion;
        shared->flags = flags;
        shared->call = (uint32_t) s.call;
        shared->heartbeat = now;
        wcsncpy_s (shared->hotkey, options.hotkey.c_str(), _TRUNCATE);
        wcsncpy_s (shared->detail, wide (s.detail).c_str(), _TRUNCATE);
        MemoryBarrier();
        InterlockedIncrement (&shared->sequence);
    }

    void finish()
    {
        taskbar.finish();
        mixer.disconnect();
        if (hwnd) UnregisterHotKey (hwnd, kHotkeyId);
        // The reader thread may be stuck on a stalled share; ExitProcess doesn't wait for it.
        ExitProcess (0);
    }

    Options options;
    BridgeMixer& mixer;
    HostPort& host;
    Engine engine;
    StateReader reader;
    TaskbarAlert taskbar;
    bool autoMode;
    HWND hwnd = nullptr;
    HANDLE mapping = nullptr, parent = nullptr;
    SharedStatus* shared = nullptr;
    Phase phase = Phase::Connecting;
    bool hotkeyBound = false, setUp = false;
    uint64_t lastCheck = 0, lastHotkeyTry = 0, stopAt = 0;
};

Helper* helper = nullptr;

LRESULT CALLBACK windowProc (HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
        case WM_TIMER: helper->onTimer(); return 0;
        case WM_HOTKEY: helper->flip(); return 0;
        case kMsgFlip: helper->flip(); return 0;
        case kMsgSetAuto: helper->setAuto (wParam != 0); return 0;
        case kMsgShutdown: helper->shutdown(); return 0;
    }
    return DefWindowProcW (hwnd, msg, wParam, lParam);
}
} // namespace

int WINAPI wWinMain (HINSTANCE inst, HINSTANCE, LPWSTR, int)
{
    Options o;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW (GetCommandLineW(), &argc);
    for (int i = 1; i < argc; ++i)
    {
        const std::wstring a = argv[i];
        const auto next = [&]() -> std::wstring { return i + 1 < argc ? argv[++i] : L""; };
        if (a == L"--parent") o.parent = (DWORD) _wtoi (next().c_str());
        else if (a == L"--mixer-hwnd") o.mixerHwnd = reinterpret_cast<HWND> ((INT_PTR) _wtoi64 (next().c_str()));
        else if (a == L"--host") o.host = utf8 (next());
        else if (a == L"--state") o.state = next();
        else if (a == L"--port") o.port = _wtoi (next().c_str());
        else if (a == L"--stream") o.stream = utf8 (next());
        else if (a == L"--bus") o.bus = _wtoi (next().c_str());
        else if (a == L"--mic-strip") o.micStrip = _wtoi (next().c_str());
        else if (a == L"--hotkey") o.hotkey = next();
        else if (a == L"--auto") o.autoMode = next() != L"0";
        else if (a == L"--log") o.log = next();
        else if (a == L"--instance") o.instance = next();
        else if (a == L"--stub-mixer") o.stub = true;
    }
    if (o.host.empty() || o.state.empty() || o.bus < 0 || o.bus > 7) return 2;

    // One per mixer: a restart after a crash finds the old one gone or exits.
    const std::wstring mutexName = kMutexName + o.instance;
    HANDLE single = CreateMutexW (nullptr, TRUE, mutexName.c_str());
    if (! single || GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    logFile.open (o.log);

    UdpHost host (o.host, o.port, o.stream);
    std::unique_ptr<BridgeMixer> mixer;
    if (o.stub) mixer = std::make_unique<StubMixer>();
    else mixer = std::make_unique<RemoteMixer>();

    Helper running (o, *mixer, host);
    helper = &running;

    WNDCLASSW wc {};
    wc.lpfnWndProc = windowProc;
    wc.hInstance = inst;
    const std::wstring className = kWindowClass + o.instance;
    wc.lpszClassName = className.c_str();
    RegisterClassW (&wc);
    HWND hwnd = CreateWindowExW (0, className.c_str(), L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, inst, nullptr);
    running.attach (hwnd);
    SetTimer (hwnd, kTimer, 100, nullptr);

    MSG m;
    while (GetMessageW (&m, nullptr, 0, 0) > 0)
        DispatchMessageW (&m);
    return 0;
}
