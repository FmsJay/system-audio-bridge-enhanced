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
    size = frames + 1;
    L.assign ((size_t) size, 0.0f);
    R.assign ((size_t) size, 0.0f);
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

IAudioClient* activateDefaultRenderDevice()
{
    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* device = nullptr;
    IAudioClient* client = nullptr;

    if (SUCCEEDED (CoCreateInstance (__uuidof (MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS (&enumerator)))
        && SUCCEEDED (enumerator->GetDefaultAudioEndpoint (eRender, eConsole, &device)))
        device->Activate (__uuidof (IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**> (&client));

    safeRelease (device);
    safeRelease (enumerator);
    return client;
}

// Try float first, then 16-bit; AUTOCONVERTPCM lets Windows resample to the host rate.
bool initialise (IAudioClient* client, DWORD extraFlags, int sampleRate, bool& isFloat)
{
    const DWORD flags = extraFlags | AUDCLNT_STREAMFLAGS_EVENTCALLBACK
                        | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    const REFERENCE_TIME bufferDuration = 200000; // 20 ms

    for (bool tryFloat : { true, false })
    {
        auto fmt = makeFormat (sampleRate, tryFloat);
        if (SUCCEEDED (client->Initialize (AUDCLNT_SHAREMODE_SHARED, flags, bufferDuration, 0, &fmt, nullptr)))
        {
            isFloat = tryFloat;
            return true;
        }
    }
    return false;
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
} // namespace

//==============================================================================
void LoopbackCapture::start (int sampleRate, CaptureSource source)
{
    stop();
    ring.setSize (sampleRate); // one second of headroom
    quit = false;
    running = true;
    thread = std::thread ([this, sampleRate, source] { threadMain (sampleRate, source); });
}

void LoopbackCapture::stop()
{
    quit = true;
    if (thread.joinable()) thread.join();
    running = false;
    statusCode = 0;
}

std::string LoopbackCapture::status() const
{
    switch (statusCode.load())
    {
        case 1:  return "Capturing system audio (REAPER excluded)";
        case 2:  return "Capturing whole default device";
        case 3:  return "Process exclusion unavailable - capturing whole device (beware feedback)";
        case -1: return "Capture failed to start";
        default: return "Idle";
    }
}

void LoopbackCapture::threadMain (int sampleRate, CaptureSource source)
{
    ComScope com;
    ProAudioScope mmcss;

    IAudioClient* client = nullptr;
    bool isFloat = true;
    int code = 0;

    if (source == CaptureSource::excludeHostProcess)
    {
        client = activateProcessLoopbackExcluding (GetCurrentProcessId());
        if (client && initialise (client, AUDCLNT_STREAMFLAGS_LOOPBACK, sampleRate, isFloat))
            code = 1;
        else
            safeRelease (client);
    }

    if (code == 0)
    {
        client = activateDefaultRenderDevice();
        if (client && initialise (client, AUDCLNT_STREAMFLAGS_LOOPBACK, sampleRate, isFloat))
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
        statusCode = -1;
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
        while (SUCCEEDED (capture->GetNextPacketSize (&packet)) && packet > 0)
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
    }

    client->Stop();
    safeRelease (capture);
    safeRelease (client);
    CloseHandle (event);
}

//==============================================================================
void DeviceRender::start (int sampleRate)
{
    stop();
    ring.setSize (sampleRate);
    quit = false;
    running = true;
    thread = std::thread ([this, sampleRate] { threadMain (sampleRate); });
}

void DeviceRender::stop()
{
    quit = true;
    if (thread.joinable()) thread.join();
    running = false;
}

void DeviceRender::threadMain (int sampleRate)
{
    ComScope com;
    ProAudioScope mmcss;

    bool isFloat = true;
    IAudioClient* client = activateDefaultRenderDevice();
    IAudioRenderClient* render = nullptr;
    HANDLE event = CreateEventW (nullptr, FALSE, FALSE, nullptr);
    UINT32 bufferFrames = 0;

    if (client == nullptr
        || ! initialise (client, 0, sampleRate, isFloat)
        || FAILED (client->SetEventHandle (event))
        || FAILED (client->GetBufferSize (&bufferFrames))
        || FAILED (client->GetService (IID_PPV_ARGS (&render))))
    {
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
            break;

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
}
