// vmtone: play a sine tone on one Windows render endpoint, or meter a recording endpoint.
//   vmtone "<render endpoint substring>" [dBFS=-50] [seconds=1] [Hz=1000]
//   vmtone --meter "<recording endpoint substring>" [seconds=1] [Hz...]
//       prints peak dBFS and packet count, plus the level of each listed frequency (Goertzel)
//   vmtone --list
// Exit code 0 on success, 2 when no endpoint matched, 1 on any other failure.
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <string>
#include <vector>

// Records from a capture endpoint and reports what arrived, to prove a bus actually carries audio.
static int meterEndpoint (IMMDevice* target, const std::wstring& targetName, double seconds,
                          const std::vector<double>& freqs)
{
    std::vector<float> mono;
    IAudioClient* client = nullptr;
    IAudioCaptureClient* capture = nullptr;
    WAVEFORMATEX fmt { WAVE_FORMAT_IEEE_FLOAT, 2, 48000, 48000 * 8, 8, 32, 0 };
    if (FAILED (target->Activate (__uuidof (IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**> (&client)))
        || FAILED (client->Initialize (AUDCLNT_SHAREMODE_SHARED,
                                       AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                       1000000, 0, &fmt, nullptr))
        || FAILED (client->GetService (IID_PPV_ARGS (&capture))))
    {
        std::fwprintf (stderr, L"could not open \"%ls\"\n", targetName.c_str());
        return 1;
    }

    float peak = 0.0f;
    UINT64 frames = 0, silentFrames = 0;
    const DWORD end = GetTickCount() + (DWORD) (seconds * 1000);
    client->Start();
    while (GetTickCount() < end)
    {
        Sleep (10);
        UINT32 packet = 0;
        while (SUCCEEDED (capture->GetNextPacketSize (&packet)) && packet > 0)
        {
            BYTE* data = nullptr;
            UINT32 n = 0;
            DWORD flags = 0;
            if (FAILED (capture->GetBuffer (&data, &n, &flags, nullptr, nullptr))) break;
            frames += n;
            const auto* f = reinterpret_cast<const float*> (data);
            const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
            if (silent) silentFrames += n;
            for (UINT32 i = 0; i < n; ++i)
            {
                const float l = silent ? 0.0f : f[2 * i], r = silent ? 0.0f : f[2 * i + 1];
                peak = std::max ({ peak, std::abs (l), std::abs (r) });
                mono.push_back (0.5f * (l + r));
            }
            capture->ReleaseBuffer (n);
        }
    }
    client->Stop();
    const double peakDb = peak > 0.0f ? 20.0 * std::log10 (peak) : -140.0;
    std::wprintf (L"metered %ls: %llu frames (%llu flagged silent), peak %.1f dBFS\n",
                  targetName.c_str(), frames, silentFrames, peakDb);

    // Goertzel amplitude of each requested sine, so two simultaneous test tones can be told apart.
    for (const double hz : freqs)
    {
        const double w = 6.283185307179586 * hz / fmt.nSamplesPerSec, coeff = 2.0 * std::cos (w);
        double s1 = 0.0, s2 = 0.0;
        for (const float x : mono)
        {
            const double s0 = x + coeff * s1 - s2;
            s2 = s1;
            s1 = s0;
        }
        const double power = s1 * s1 + s2 * s2 - coeff * s1 * s2;
        const double amplitude = mono.empty() ? 0.0 : 2.0 * std::sqrt (std::max (power, 0.0)) / (double) mono.size();
        std::wprintf (L"hz %.0f: %.1f dBFS\n", hz, amplitude > 0.0 ? 20.0 * std::log10 (amplitude) : -140.0);
    }
    capture->Release();
    client->Release();
    return 0;
}

int wmain (int argc, wchar_t** argv)
{
    if (argc < 2)
    {
        std::fwprintf (stderr, L"usage: vmtone \"<endpoint substring>\" [dBFS] [seconds] [Hz] | --list\n");
        return 1;
    }

    const bool list = std::wcscmp (argv[1], L"--list") == 0;
    const bool meter = std::wcscmp (argv[1], L"--meter") == 0;
    if (meter)
    {
        if (argc < 3) return 1;
        ++argv;
        --argc;
    }

    CoInitializeEx (nullptr, COINIT_MULTITHREADED);
    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDeviceCollection* devices = nullptr;
    if (FAILED (CoCreateInstance (__uuidof (MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS (&enumerator)))
        || FAILED (enumerator->EnumAudioEndpoints (meter ? eCapture : eRender, DEVICE_STATE_ACTIVE, &devices)))
        return 1;

    IMMDevice* target = nullptr;
    std::wstring targetName;
    UINT count = 0;
    devices->GetCount (&count);

    for (UINT i = 0; i < count; ++i)
    {
        IMMDevice* device = nullptr;
        IPropertyStore* props = nullptr;
        PROPVARIANT name;
        PropVariantInit (&name);
        devices->Item (i, &device);
        device->OpenPropertyStore (STGM_READ, &props);
        props->GetValue (PKEY_Device_FriendlyName, &name);
        const std::wstring friendly = name.vt == VT_LPWSTR ? name.pwszVal : L"";
        PropVariantClear (&name);
        props->Release();

        if (list)
            std::wprintf (L"%ls\n", friendly.c_str());
        else if (target == nullptr && friendly.find (argv[1]) != std::wstring::npos)
        {
            target = device;
            targetName = friendly;
            continue;
        }
        device->Release();
    }
    if (list) return 0;
    if (target == nullptr)
    {
        std::fwprintf (stderr, L"no active render endpoint contains \"%ls\"\n", argv[1]);
        return 2;
    }

    if (meter)
    {
        std::vector<double> freqs;
        for (int i = 3; i < argc; ++i) freqs.push_back (_wtof (argv[i]));
        return meterEndpoint (target, targetName, argc > 2 ? _wtof (argv[2]) : 1.0, freqs);
    }

    const double dbfs = argc > 2 ? _wtof (argv[2]) : -50.0;
    const double seconds = argc > 3 ? _wtof (argv[3]) : 1.0;
    const double hz = argc > 4 ? _wtof (argv[4]) : 1000.0;
    const double amplitude = std::pow (10.0, dbfs / 20.0);

    IAudioClient* client = nullptr;
    IAudioRenderClient* render = nullptr;
    WAVEFORMATEX fmt { WAVE_FORMAT_IEEE_FLOAT, 2, 48000, 48000 * 8, 8, 32, 0 };
    UINT32 bufferFrames = 0;
    if (FAILED (target->Activate (__uuidof (IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**> (&client)))
        || FAILED (client->Initialize (AUDCLNT_SHAREMODE_SHARED,
                                       AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                       1000000, 0, &fmt, nullptr))
        || FAILED (client->GetBufferSize (&bufferFrames))
        || FAILED (client->GetService (IID_PPV_ARGS (&render))))
    {
        std::fwprintf (stderr, L"could not open \"%ls\"\n", targetName.c_str());
        return 1;
    }

    // Short fades so the tone starts and stops without clicks.
    const UINT64 total = (UINT64) (seconds * fmt.nSamplesPerSec), fade = fmt.nSamplesPerSec / 100;
    UINT64 written = 0;
    client->Start();
    while (written < total)
    {
        UINT32 padding = 0;
        client->GetCurrentPadding (&padding);
        const UINT32 frames = (UINT32) std::min<UINT64> (bufferFrames - padding, total - written);
        BYTE* data = nullptr;
        if (frames > 0 && SUCCEEDED (render->GetBuffer (frames, &data)))
        {
            auto* out = reinterpret_cast<float*> (data);
            for (UINT32 i = 0; i < frames; ++i, ++written)
            {
                const UINT64 edge = std::min (written, total - 1 - written);
                const double env = edge < fade ? (double) edge / fade : 1.0;
                const float s = (float) (amplitude * env * std::sin (6.283185307179586 * hz * written / fmt.nSamplesPerSec));
                out[2 * i] = out[2 * i + 1] = s;
            }
            render->ReleaseBuffer (frames, 0);
        }
        Sleep (5);
    }
    Sleep ((DWORD) (1000.0 * bufferFrames / fmt.nSamplesPerSec)); // let the buffer drain
    client->Stop();
    std::wprintf (L"played %.1f dBFS %.0f Hz for %.2f s on %ls\n", dbfs, hz, seconds, targetName.c_str());
    render->Release();
    client->Release();
    target->Release();
    devices->Release();
    enumerator->Release();
    return 0;
}
