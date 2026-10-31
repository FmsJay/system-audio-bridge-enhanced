#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <avrt.h>

#include <algorithm>
#include <cstring>

#include "Wasapi.h"

//==============================================================================
void StereoRing::setSize (int frames)
{
    // Reallocating races the audio thread, so restarts at the same rate (device retries) reuse the buffer.
    if (size != frames + 1)
    {
        size = frames + 1;
        L.assign ((size_t) size, 0.0f);
        R.assign ((size_t) size, 0.0f);
    }
    clear();
}

void StereoRing::clear()
{
    readPos.store (0);
    writePos.store (0);
}

int StereoRing::available() const
{
    if (size == 0) return 0;
    const int w = writePos.load (std::memory_order_acquire);
    const int r = readPos.load (std::memory_order_acquire);
    return (w - r + size) % size;
}

int StereoRing::freeSpace() const
{
    return size == 0 ? 0 : size - 1 - available();
}

int StereoRing::write (const float* l, const float* r, int frames)
{
    frames = std::min (frames, freeSpace());
    int w = writePos.load (std::memory_order_relaxed);
    for (int i = 0; i < frames; ++i)
    {
        L[(size_t) w] = l[i];
        R[(size_t) w] = r[i];
        if (++w == size) w = 0;
    }
    writePos.store (w, std::memory_order_release);
    return frames;
}

int StereoRing::read (float* l, float* r, int frames)
{
    frames = std::min (frames, available());
    int p = readPos.load (std::memory_order_relaxed);
    for (int i = 0; i < frames; ++i)
    {
        if (l) l[i] = L[(size_t) p];
        if (r) r[i] = R[(size_t) p];
        if (++p == size) p = 0;
    }
    readPos.store (p, std::memory_order_release);
    return frames;
}

//==============================================================================
namespace
{
template <typename T>
void safeRelease (T*& p)
{
    if (p) { p->Release(); p = nullptr; }
}

WAVEFORMATEX makeFormat (int sampleRate, bool asFloat)
{
    WAVEFORMATEX f {};
    f.wFormatTag = asFloat ? WAVE_FORMAT_IEEE_FLOAT : WAVE_FORMAT_PCM;
    f.nChannels = 2;
    f.nSamplesPerSec = (DWORD) sampleRate;
    f.wBitsPerSample = asFloat ? 32 : 16;
    f.nBlockAlign = (WORD) (f.nChannels * f.wBitsPerSample / 8);
    f.nAvgBytesPerSec = f.nSamplesPerSec * f.nBlockAlign;
    return f;
}

// ActivateAudioInterfaceAsync needs an agile completion handler.
struct ActivationHandler final : IActivateAudioInterfaceCompletionHandler, IAgileObject
{
    ActivationHandler() { done = CreateEventW (nullptr, TRUE, FALSE, nullptr); }
    ~ActivationHandler() { CloseHandle (done); }

    HRESULT STDMETHODCALLTYPE QueryInterface (REFIID riid, void** ppv) override
    {
        if (riid == __uuidof (IUnknown) || riid == __uuidof (IActivateAudioInterfaceCompletionHandler))
            *ppv = static_cast<IActivateAudioInterfaceCompletionHandler*> (this);
        else if (riid == __uuidof (IAgileObject))
            *ppv = static_cast<IAgileObject*> (this);
        else
        {
            *ppv = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }

    ULONG STDMETHODCALLTYPE AddRef() override  { return (ULONG) InterlockedIncrement (&refs); }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const auto n = InterlockedDecrement (&refs);
        if (n == 0) delete this;
        return (ULONG) n;
    }

    HRESULT STDMETHODCALLTYPE ActivateCompleted (IActivateAudioInterfaceAsyncOperation* op) override
    {
        HRESULT activateResult = E_FAIL;
        IUnknown* unk = nullptr;
        result = op->GetActivateResult (&activateResult, &unk);
        if (SUCCEEDED (result)) result = activateResult;
        if (SUCCEEDED (result) && unk) result = unk->QueryInterface (IID_PPV_ARGS (&client));
        safeRelease (unk);
        SetEvent (done);
        return S_OK;
    }

    LONG refs = 1;
    HANDLE done = nullptr;
    HRESULT result = E_FAIL;
    IAudioClient* client = nullptr;
};

// An IAudioClient that loops back everything except the given process tree.
IAudioClient* activateProcessLoopbackExcluding (DWORD pid)
{
    AUDIOCLIENT_ACTIVATION_PARAMS params {};
    params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    params.ProcessLoopbackParams.TargetProcessId = pid;
    params.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE;

    PROPVARIANT pv {};
    pv.vt = VT_BLOB;
    pv.blob.cbSize = sizeof (params);
    pv.blob.pBlobData = reinterpret_cast<BYTE*> (&params);

    auto* handler = new ActivationHandler();
    IActivateAudioInterfaceAsyncOperation* op = nullptr;
    IAudioClient* client = nullptr;

    if (SUCCEEDED (ActivateAudioInterfaceAsync (VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof (IAudioClient),
                                                &pv, handler, &op))
        && WaitForSingleObject (handler->done, 5000) == WAIT_OBJECT_0
        && SUCCEEDED (handler->result))
    {
        client = handler->client;
        handler->client = nullptr;
    }

    safeRelease (handler->client);
    safeRelease (op);
    handler->Release();
    return client;
}

std::string toUtf8 (const wchar_t* text)
{
    if (text == nullptr || *text == 0) return {};
    const int n = WideCharToMultiByte (CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    std::string out ((size_t) std::max (n - 1, 0), '\0');
    if (n > 1) WideCharToMultiByte (CP_UTF8, 0, text, -1, out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring toWide (const std::string& text)
{
    if (text.empty()) return {};
    const int n = MultiByteToWideChar (CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    std::wstring out ((size_t) std::max (n - 1, 0), L'\0');
    if (n > 1) MultiByteToWideChar (CP_UTF8, 0, text.c_str(), -1, out.data(), n);
    return out;
}

// PKEY_Device_FriendlyName, spelled out to avoid needing INITGUID for the SDK's definition.
const PROPERTYKEY friendlyNameKey { { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 14 };

std::string friendlyName (IMMDevice* device)
{
    IPropertyStore* props = nullptr;
    std::string name;
    if (SUCCEEDED (device->OpenPropertyStore (STGM_READ, &props)))
    {
        PROPVARIANT pv;
        PropVariantInit (&pv);
        if (SUCCEEDED (props->GetValue (friendlyNameKey, &pv)) && pv.vt == VT_LPWSTR)
            name = toUtf8 (pv.pwszVal);
        PropVariantClear (&pv);
    }
    safeRelease (props);
    return name;
}

enum class Activation { ok, failed, missing };

// The default render device when endpointId is empty, otherwise exactly that endpoint.
// A selected endpoint that is unplugged, disabled or not a render device is "missing".
IAudioClient* activateRenderDevice (const std::wstring& endpointId, Activation& result)
{
    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* device = nullptr;
    IMMEndpoint* endpoint = nullptr;
    IAudioClient* client = nullptr;
    result = Activation::failed;

    if (SUCCEEDED (CoCreateInstance (__uuidof (MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS (&enumerator))))
    {
        if (endpointId.empty())
        {
            if (SUCCEEDED (enumerator->GetDefaultAudioEndpoint (eRender, eConsole, &device)))
                result = Activation::ok;
        }
        else
        {
            DWORD state = 0;
            EDataFlow flow = eCapture;
            const bool usable = SUCCEEDED (enumerator->GetDevice (endpointId.c_str(), &device))
                                && SUCCEEDED (device->GetState (&state)) && state == DEVICE_STATE_ACTIVE
                                && SUCCEEDED (device->QueryInterface (IID_PPV_ARGS (&endpoint)))
                                && SUCCEEDED (endpoint->GetDataFlow (&flow)) && flow == eRender;
            result = usable ? Activation::ok : Activation::missing;
        }

        if (result == Activation::ok
            && FAILED (device->Activate (__uuidof (IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**> (&client))))
            result = Activation::failed;
    }

    safeRelease (endpoint);
    safeRelease (device);
    safeRelease (enumerator);
    return client;
}

// Try float first, then 16-bit; AUTOCONVERTPCM lets Windows resample to the host rate.
// Returns the last Initialize result, so callers can tell a device held in exclusive mode apart.
HRESULT initialise (IAudioClient* client, DWORD extraFlags, int sampleRate, bool& isFloat)
{
    const DWORD flags = extraFlags | AUDCLNT_STREAMFLAGS_EVENTCALLBACK
                        | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    const REFERENCE_TIME bufferDuration = 200000; // 20 ms
    HRESULT hr = E_FAIL;

    for (bool tryFloat : { true, false })
    {
        auto fmt = makeFormat (sampleRate, tryFloat);
        hr = client->Initialize (AUDCLNT_SHAREMODE_SHARED, flags, bufferDuration, 0, &fmt, nullptr);
        if (SUCCEEDED (hr))
        {
            isFloat = tryFloat;
            break;
        }
    }
    return hr;
}

// Status for a stream that didn't start: missing endpoint, exclusive-mode owner, or anything else.
int failureCode (Activation activation, HRESULT initResult)
{
    if (activation == Activation::missing) return -2;
    return initResult == AUDCLNT_E_DEVICE_IN_USE ? -4 : -1;
}

struct ComScope
{
    ComScope()  { CoInitializeEx (nullptr, COINIT_MULTITHREADED); }
    ~ComScope() { CoUninitialize(); }
};

struct ProAudioScope
{
    ProAudioScope()  { DWORD idx = 0; h = AvSetMmThreadCharacteristicsW (L"Pro Audio", &idx); }
    ~ProAudioScope() { if (h) AvRevertMmThreadCharacteristics (h); }
    HANDLE h = nullptr;
};

std::string named (const char* prefix, const std::string& name)
{
    return name.empty() ? std::string (prefix) : std::string (prefix) + ": " + name;
}
} // namespace

//==============================================================================
std::vector<AudioEndpoint> listRenderEndpoints()
{
    std::vector<AudioEndpoint> result;

    // A private MTA thread keeps us out of whatever apartment the caller's thread is in.
    std::thread ([&result]
    {
        ComScope com;
        IMMDeviceEnumerator* enumerator = nullptr;
        IMMDeviceCollection* devices = nullptr;
        IMMDevice* defaultDevice = nullptr;
        std::string defaultId;

        if (FAILED (CoCreateInstance (__uuidof (MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS (&enumerator))))
            return;

        LPWSTR id = nullptr;
        if (SUCCEEDED (enumerator->GetDefaultAudioEndpoint (eRender, eConsole, &defaultDevice))
            && SUCCEEDED (defaultDevice->GetId (&id)))
        {
            defaultId = toUtf8 (id);
            CoTaskMemFree (id);
        }

        UINT count = 0;
        if (SUCCEEDED (enumerator->EnumAudioEndpoints (eRender, DEVICE_STATE_ACTIVE, &devices))
            && SUCCEEDED (devices->GetCount (&count)))
        {
            for (UINT i = 0; i < count; ++i)
            {
                IMMDevice* device = nullptr;
                id = nullptr;
                if (SUCCEEDED (devices->Item (i, &device)) && SUCCEEDED (device->GetId (&id)))
                {
                    AudioEndpoint e;
                    e.id = toUtf8 (id);
                    e.name = friendlyName (device);
                    if (e.name.empty()) e.name = e.id;
                    e.isDefault = e.id == defaultId;
                    result.push_back (std::move (e));
                    CoTaskMemFree (id);
                }
                safeRelease (device);
            }
        }

        safeRelease (defaultDevice);
        safeRelease (devices);
        safeRelease (enumerator);
    }).join();

    std::sort (result.begin(), result.end(), [] (const AudioEndpoint& a, const AudioEndpoint& b) { return a.name < b.name; });
    return result;
}

//==============================================================================
void LoopbackCapture::start (int sampleRate, CaptureSource source, const std::string& endpointId,
                             const std::string& endpointName)
{
    join(); // keeps the last status visible until the new thread reports, so retries don't flicker
    name = source == CaptureSource::selectedEndpoint ? endpointName : std::string();
    ring.setSize (sampleRate); // one second of headroom
    quit = false;
    running = true;
    thread = std::thread ([this, sampleRate, source, endpointId] { threadMain (sampleRate, source, endpointId); });
}

void LoopbackCapture::join()
{
    quit = true;
    if (thread.joinable()) thread.join();
    running = false;
}

void LoopbackCapture::stop()
{
    join();
    statusCode = 0;
}

std::string LoopbackCapture::status() const
{
    switch (statusCode.load())
    {
        case 1:  return "Capturing system audio (REAPER excluded)";
        case 2:  return "Capturing whole default device";
        case 3:  return "Process exclusion unavailable - capturing whole device (beware feedback)";
        case 4:  return named ("Capturing whole-device loopback", name);
        case -1: return "Capture failed to start";
        case -2: return named ("Capture device missing", name) + " - reconnect it or choose another";
        case -3: return named ("Capture device lost", name) + " - retrying";
        case -4: return named ("Capture device in use", name) + " - another app holds it in exclusive mode";
        default: return "Idle";
    }
}

void LoopbackCapture::threadMain (int sampleRate, CaptureSource source, std::string endpointId)
{
    ComScope com;
    ProAudioScope mmcss;

    IAudioClient* client = nullptr;
    bool isFloat = true;
    int code = 0;
    auto activation = Activation::failed;
    HRESULT initResult = E_FAIL;

    if (source == CaptureSource::excludeHostProcess)
    {
        client = activateProcessLoopbackExcluding (GetCurrentProcessId());
        if (client && SUCCEEDED (initResult = initialise (client, AUDCLNT_STREAMFLAGS_LOOPBACK, sampleRate, isFloat)))
            code = 1;
        else
            safeRelease (client);
    }

    if (source == CaptureSource::selectedEndpoint)
    {
        client = activateRenderDevice (toWide (endpointId), activation);
        if (client && SUCCEEDED (initResult = initialise (client, AUDCLNT_STREAMFLAGS_LOOPBACK, sampleRate, isFloat)))
            code = 4;
        else
            safeRelease (client);
    }
    else if (code == 0)
    {
        client = activateRenderDevice ({}, activation);
        if (client && SUCCEEDED (initResult = initialise (client, AUDCLNT_STREAMFLAGS_LOOPBACK, sampleRate, isFloat)))
            code = source == CaptureSource::excludeHostProcess ? 3 : 2;
        else
            safeRelease (client);
    }

    HANDLE event = CreateEventW (nullptr, FALSE, FALSE, nullptr);
    IAudioCaptureClient* capture = nullptr;

    if (code == 0
        || FAILED (client->SetEventHandle (event))
        || FAILED (client->GetService (IID_PPV_ARGS (&capture)))
        || FAILED (client->Start()))
    {
        statusCode = failureCode (activation, initResult);
        safeRelease (capture);
        safeRelease (client);
        CloseHandle (event);
        running = false;
        return;
    }

    statusCode = code;
    std::vector<float> l, r;

    while (! quit.load())
    {
        WaitForSingleObject (event, 50);

        UINT32 packet = 0;
        HRESULT hr = S_OK;
        while (SUCCEEDED (hr = capture->GetNextPacketSize (&packet)) && packet > 0)
        {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            if (FAILED (capture->GetBuffer (&data, &frames, &flags, nullptr, nullptr)))
                break;

            l.resize (frames);
            r.resize (frames);

            if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || data == nullptr)
            {
                std::fill (l.begin(), l.end(), 0.0f);
                std::fill (r.begin(), r.end(), 0.0f);
            }
            else if (isFloat)
            {
                auto* f = reinterpret_cast<const float*> (data);
                for (UINT32 i = 0; i < frames; ++i) { l[i] = f[2 * i]; r[i] = f[2 * i + 1]; }
            }
            else
            {
                auto* s = reinterpret_cast<const int16_t*> (data);
                for (UINT32 i = 0; i < frames; ++i) { l[i] = s[2 * i] / 32768.0f; r[i] = s[2 * i + 1] / 32768.0f; }
            }

            capture->ReleaseBuffer (frames);
            ring.write (l.data(), r.data(), (int) frames); // drops on overflow; reader trims latency
        }

        if (FAILED (hr)) // usually AUDCLNT_E_DEVICE_INVALIDATED: unplugged, disabled or format changed
        {
            statusCode = -3;
            break;
        }
    }

    client->Stop();
    safeRelease (capture);
    safeRelease (client);
    CloseHandle (event);
    running = false; // lets the owner restart us after a lost device
}

//==============================================================================
void DeviceRender::start (int sampleRate, const std::string& endpointId, const std::string& endpointName)
{
    join();
    name = endpointId.empty() ? std::string() : endpointName;
    ring.setSize (sampleRate);
    quit = false;
    running = true;
    thread = std::thread ([this, sampleRate, endpointId] { threadMain (sampleRate, endpointId); });
}

void DeviceRender::join()
{
    quit = true;
    if (thread.joinable()) thread.join();
    running = false;
}

void DeviceRender::stop()
{
    join();
    statusCode = 0;
}

std::string DeviceRender::status() const
{
    switch (statusCode.load())
    {
        case 1:  return "Sending to default device";
        case 2:  return named ("Sending to", name);
        case -1: return "Send failed to start";
        case -2: return named ("Send device missing", name) + " - reconnect it or choose another";
        case -3: return named ("Send device lost", name) + " - retrying";
        case -4: return named ("Send device in use", name) + " - another app holds it in exclusive mode";
        default: return "Send idle";
    }
}

void DeviceRender::threadMain (int sampleRate, std::string endpointId)
{
    ComScope com;
    ProAudioScope mmcss;

    bool isFloat = true;
    auto activation = Activation::failed;
    IAudioClient* client = activateRenderDevice (toWide (endpointId), activation);
    IAudioRenderClient* render = nullptr;
    HANDLE event = CreateEventW (nullptr, FALSE, FALSE, nullptr);
    UINT32 bufferFrames = 0;
    HRESULT initResult = E_FAIL;

    if (client == nullptr
        || FAILED (initResult = initialise (client, 0, sampleRate, isFloat))
        || FAILED (client->SetEventHandle (event))
        || FAILED (client->GetBufferSize (&bufferFrames))
        || FAILED (client->GetService (IID_PPV_ARGS (&render))))
    {
        statusCode = failureCode (activation, initResult);
        safeRelease (render);
        safeRelease (client);
        CloseHandle (event);
        running = false;
        return;
    }

    BYTE* data = nullptr;
    if (SUCCEEDED (render->GetBuffer (bufferFrames, &data)))
        render->ReleaseBuffer (bufferFrames, AUDCLNT_BUFFERFLAGS_SILENT);

    client->Start();
    statusCode = endpointId.empty() ? 1 : 2;

    const int maxLatency = sampleRate / 10;    // 100 ms: beyond this, the clocks have drifted
    const int targetLatency = sampleRate / 50; // 20 ms
    std::vector<float> l, r;

    while (! quit.load())
    {
        WaitForSingleObject (event, 50);

        if (ring.available() > maxLatency)
            ring.read (nullptr, nullptr, ring.available() - targetLatency);

        UINT32 padding = 0;
        if (FAILED (client->GetCurrentPadding (&padding)))
        {
            statusCode = -3;
            break;
        }

        const UINT32 frames = bufferFrames - padding;
        if (frames == 0 || FAILED (render->GetBuffer (frames, &data)))
            continue;

        l.assign (frames, 0.0f);
        r.assign (frames, 0.0f);
        ring.read (l.data(), r.data(), (int) frames); // short reads leave silence

        if (isFloat)
        {
            auto* f = reinterpret_cast<float*> (data);
            for (UINT32 i = 0; i < frames; ++i) { f[2 * i] = l[i]; f[2 * i + 1] = r[i]; }
        }
        else
        {
            auto* s = reinterpret_cast<int16_t*> (data);
            for (UINT32 i = 0; i < frames; ++i)
            {
                s[2 * i]     = (int16_t) (std::clamp (l[i], -1.0f, 1.0f) * 32767.0f);
                s[2 * i + 1] = (int16_t) (std::clamp (r[i], -1.0f, 1.0f) * 32767.0f);
            }
        }

        render->ReleaseBuffer (frames, 0);
    }

    client->Stop();
    safeRelease (render);
    safeRelease (client);
    CloseHandle (event);
    running = false; // previously stayed true after a device error, so the send never restarted
}
