#pragma once

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>

namespace emebalachat {

// REF-3.7 (session 260910_0006 T6, verification report §10a/§10c): shared
// header-only helpers for the layered Direct2D windows. Extracted verbatim
// from the two byte-identical file-local copies in src/ui/tooltip.cpp and
// src/ui/about_window.cpp (diffed line-by-line before deletion - identical).
// The former "deliberate local replication" constraint (B-5 file-disjoint
// wave discipline, session 260907_0002 design §3) is void: both files are
// edited together in this single task. Lives directly in `emebalachat`
// (same precedent as layered_renderer.hpp in src/ui/) so every existing
// unqualified call site keeps compiling unchanged.

// ---- ui geometry (§10c) ----

// Point-in-rect hit test in DIP space for D2D1_RECT_F button/thumb rects.
inline bool IsPointInRect(const D2D1_RECT_F& r, float x, float y) {
    return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom;
}

// ---- DWrite (§10a) ----

// P4 Batch B-2 (session 260907_0002, design §2-Q5 verdict A / §3 B-2; B-5
// design §2-Q5 verdict A): DWrite localeName is CREATION-ONLY -
// IDWriteTextFormat exposes no SetLocaleName at any interface version
// (SDK 10.0.26100 header audit + B-2 headless probe). A locale change is
// therefore a CLONE-SWAP: the live format is cloned under a new BCP-47 tag
// (family/weight/style/stretch/size and all paragraph settings carried over).
// Returns the fresh format, or nullptr - a failed swap must never lose the
// working format.
inline IDWriteTextFormat* CloneFormatWithLocale(IDWriteFactory* factory, IDWriteTextFormat* src,
                                                const wchar_t* locale_name) {
    if (!factory || !src || !locale_name) return nullptr;
    const UINT32 fam_len = src->GetFontFamilyNameLength();
    if (fam_len == 0 || fam_len > 255) return nullptr;
    wchar_t family[256] = {};
    if (FAILED(src->GetFontFamilyName(family, fam_len + 1))) return nullptr;
    IDWriteTextFormat* dst = nullptr;
    if (FAILED(factory->CreateTextFormat(
            family, nullptr, src->GetFontWeight(), src->GetFontStyle(),
            src->GetFontStretch(), src->GetFontSize(), locale_name, &dst)) || !dst) {
        return nullptr;
    }
    dst->SetWordWrapping(src->GetWordWrapping());
    dst->SetTextAlignment(src->GetTextAlignment());
    dst->SetParagraphAlignment(src->GetParagraphAlignment());
    dst->SetReadingDirection(src->GetReadingDirection());
    return dst;
}

} // namespace emebalachat
