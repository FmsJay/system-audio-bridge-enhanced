// Renders the softphone banner over a saved picture of the mixer, for every state, so the look and the
// wording can be checked without restarting Voicemeeter.
//   overlay-preview <mixer picture.png> <output folder>
#include <windows.h>
#include <wincodec.h>
#include <cwchar>
#include <string>

#include "softphone_overlay.hpp"

using namespace softphone;

namespace
{
template <typename T> void release (T*& p) { if (p) { p->Release(); p = nullptr; } }

SharedStatus status (uint32_t flags, uint32_t call, const wchar_t* detail = L"", const wchar_t* key = L"F9")
{
    SharedStatus s {};
    s.flags = flags;
    s.call = call;
    wcsncpy_s (s.hotkey, key, _TRUNCATE);
    wcsncpy_s (s.detail, detail, _TRUNCATE);
    return s;
}
} // namespace

int wmain (int argc, wchar_t** argv)
{
    if (argc < 3) return 2;
    CoInitializeEx (nullptr, COINIT_MULTITHREADED);

    constexpr UINT width = 1645, height = 770;
    IWICImagingFactory* wic = nullptr;
    ID2D1Factory* d2d = nullptr;
    CoCreateInstance (CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS (&wic));
    D2D1CreateFactory (D2D1_FACTORY_TYPE_SINGLE_THREADED, &d2d);

    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* converted = nullptr;
    if (FAILED (wic->CreateDecoderFromFilename (argv[1], nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder)))
        return 3;
    decoder->GetFrame (0, &frame);
    wic->CreateFormatConverter (&converted);
    converted->Initialize (frame, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);

    const auto w = [] (const wchar_t* s) { return std::wstring (s); };
    constexpr uint32_t Auto = kAuto, Alive = kAlive;
    const struct { const wchar_t* name; bool alive; bool lastOpen; SharedStatus s; } cases[] = {
        { L"no-call", true, false, status (Auto | Alive, 1) },
        { L"ringing", true, false, status (Auto | Alive | kArmed, 2, L"dialing 555 0100") },
        { L"person-mic-open", true, false, status (Auto | Alive | kArmed | kMicOpen | kOpenedAutomatically, 4, L"Hi, this is Sam, who's calling please?") },
        { L"machine", true, false, status (Auto | Alive | kArmed, 5, L"please leave a message after the tone") },
        { L"mic-open-by-hand", true, false, status (Auto | Alive | kArmed | kMicOpen | kByHand, 3, L"hello? hello?") },
        { L"mic-unknown", true, false, status (Auto | Alive | kMicUnknown, 5) },
        { L"alert-not-running", true, false, status (Auto, 0) },
        { L"long-transcript", true, false, status (Auto | Alive | kArmed, 3, L"this is a very long transcript line that keeps going and going to check that it ends in dots rather than wrapping out of the header strip or over the other controls") },
        { L"f9-taken", true, false, status (Auto | Alive | kHotkeyTaken, 1) },
        { L"automatic-off", true, false, status (Alive, 1) },
        { L"not-set-up", true, false, status (kNotSetUp | Auto, 0) },
        { L"waiting-for-mixer", true, false, status (kWaitingForMixer | Auto, 0) },
        { L"helper-stopped", false, false, status (0, 0) },
        { L"helper-stopped-mic-was-open", false, true, status (0, 0) },
    };

    Overlay overlay;
    int written = 0;
    for (const auto& c : cases)
    {
        IWICBitmap* bitmap = nullptr;
        ID2D1RenderTarget* target = nullptr;
        wic->CreateBitmap (width, height, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnDemand, &bitmap);
        d2d->CreateWicBitmapRenderTarget (bitmap, D2D1::RenderTargetProperties (D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat (DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f), &target);

        ID2D1Bitmap* back = nullptr;
        target->CreateBitmapFromWicBitmap (converted, &back);
        target->BeginDraw();
        target->DrawBitmap (back, D2D1::RectF (0, 0, (float) width, (float) height));
        overlay.draw (target, (float) width, (float) height, describe (c.alive, c.lastOpen, c.s));
        const HRESULT drawn = target->EndDraw();

        IWICStream* stream = nullptr;
        IWICBitmapEncoder* encoder = nullptr;
        IWICBitmapFrameEncode* out = nullptr;
        wic->CreateStream (&stream);
        const std::wstring path = w (argv[2]) + L"\\" + c.name + L".png";
        stream->InitializeFromFilename (path.c_str(), GENERIC_WRITE);
        wic->CreateEncoder (GUID_ContainerFormatPng, nullptr, &encoder);
        encoder->Initialize (stream, WICBitmapEncoderNoCache);
        encoder->CreateNewFrame (&out, nullptr);
        out->Initialize (nullptr);
        out->WriteSource (bitmap, nullptr);
        out->Commit();
        encoder->Commit();
        if (SUCCEEDED (drawn)) ++written;

        release (out); release (encoder); release (stream); release (back); release (target); release (bitmap);
    }
    release (converted); release (frame); release (decoder); release (d2d); release (wic);
    CoUninitialize();
    return written == (int) (sizeof (cases) / sizeof (cases[0])) ? 0 : 1;
}
