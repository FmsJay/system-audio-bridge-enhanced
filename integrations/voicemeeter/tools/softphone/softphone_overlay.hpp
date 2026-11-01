// Draws the softphone status over the Potato window, in the empty header strip between the logo and the
// VBAN button. Coordinates are Potato's default layout; the caller's transform scales them with the window.
// Used by the mod's render path and by overlay-preview.exe, so the look can be checked without a restart.
#pragma once

#include <d2d1.h>
#include <dwrite.h>
#include <string>

#include "softphone_protocol.hpp"

namespace softphone
{
class Overlay
{
public:
    ~Overlay()
    {
        if (ellipsis) ellipsis->Release();
        if (bigText) bigText->Release();
        if (smallText) smallText->Release();
        if (factory) factory->Release();
    }

    void draw (ID2D1RenderTarget* target, float width, float height, const View& view)
    {
        if (! ready() || view.line1.empty()) return;

        const D2D1_RECT_F banner { 345.0f, 4.0f, 1045.0f, 42.0f };
        D2D1_COLOR_F back = D2D1::ColorF (0x2B3A44);
        switch (view.tone)
        {
            case Tone::InCall: back = D2D1::ColorF (0x2F6FA7); break;
            case Tone::MicOpen: back = D2D1::ColorF (0xD0021B); break;
            case Tone::Warning: back = D2D1::ColorF (0x9A6A00); break;
            default: break;
        }

        ID2D1SolidColorBrush* fill = nullptr;
        ID2D1SolidColorBrush* text = nullptr;
        if (SUCCEEDED (target->CreateSolidColorBrush (back, &fill))
            && SUCCEEDED (target->CreateSolidColorBrush (D2D1::ColorF (D2D1::ColorF::White), &text)))
        {
            // Anything that may be a live mic also gets a red border round the whole window.
            if (view.border)
                target->DrawRectangle (D2D1::RectF (4.0f, 4.0f, width - 4.0f, height - 4.0f), fill, 8.0f);

            target->FillRoundedRectangle (D2D1::RoundedRect (banner, 4.0f, 4.0f), fill);

            const bool oneLine = view.line2.empty();
            const D2D1_RECT_F first { banner.left + 10.0f, banner.top + (oneLine ? 8.0f : 1.0f), banner.right - 10.0f, banner.top + (oneLine ? 32.0f : 21.0f) };
            target->DrawText (view.line1.c_str(), (UINT32) view.line1.size(), bigText, first, text, D2D1_DRAW_TEXT_OPTIONS_CLIP);
            if (! oneLine)
            {
                const D2D1_RECT_F second { banner.left + 10.0f, banner.top + 20.0f, banner.right - 10.0f, banner.bottom - 1.0f };
                target->DrawText (view.line2.c_str(), (UINT32) view.line2.size(), smallText, second, text, D2D1_DRAW_TEXT_OPTIONS_CLIP);
            }
        }
        if (fill) fill->Release();
        if (text) text->Release();
    }

private:
    bool ready()
    {
        if (bigText && smallText) return true;
        if (failed) return false;
        failed = true;   // stay quiet rather than retry every frame

        if (FAILED (DWriteCreateFactory (DWRITE_FACTORY_TYPE_SHARED, __uuidof (IDWriteFactory), reinterpret_cast<IUnknown**> (&factory))))
            return false;
        if (FAILED (factory->CreateTextFormat (L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL,
                                               DWRITE_FONT_STRETCH_NORMAL, 15.0f, L"en-us", &bigText))
            || FAILED (factory->CreateTextFormat (L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                                  DWRITE_FONT_STRETCH_NORMAL, 12.0f, L"en-us", &smallText)))
            return false;

        // Long transcript lines end in "..." instead of wrapping out of the strip.
        const DWRITE_TRIMMING trimming { DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
        for (IDWriteTextFormat* format : { bigText, smallText })
        {
            format->SetWordWrapping (DWRITE_WORD_WRAPPING_NO_WRAP);
            format->SetTextAlignment (DWRITE_TEXT_ALIGNMENT_CENTER);
            if (SUCCEEDED (factory->CreateEllipsisTrimmingSign (format, &ellipsis)))
            {
                format->SetTrimming (&trimming, ellipsis);
                ellipsis->Release();
                ellipsis = nullptr;
            }
        }
        failed = false;
        return true;
    }

    IDWriteFactory* factory = nullptr;
    IDWriteTextFormat* bigText = nullptr;
    IDWriteTextFormat* smallText = nullptr;
    IDWriteInlineObject* ellipsis = nullptr;
    bool failed = false;
};
} // namespace softphone
