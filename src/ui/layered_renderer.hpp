#pragma once

#include <windows.h>
#include <d2d1.h>

namespace emebalachat {

// REF-3.6: shared RAII owner of the GDI DIB + bound ID2D1DCRenderTarget +
// UpdateLayeredWindow present path that the four layered Direct2D windows
// (FloatingBadge, DragIconWindow, TooltipWindow, AboutWindow) each duplicated
// (~260 lines across three phases per site: allocate / present / teardown,
// plus the device-lost recreate preamble). Verified per-site-identical in
// session 260910_0006 §6 before extraction.
//
// Owns:
//   - mem_dc_   (CreateCompatibleDC on the screen DC)
//   - bitmap_ / old_bitmap_ (32bpp top-down DIB via CreateDIBSection)
//   - bits_     (DIB pixel pointer - write-only bookkeeping, never read)
//   - target_   (ID2D1DCRenderTarget, created through the CALLER's factory)
//
// Does NOT own (stays at each call site, per the §6 behavioral-difference
// matrix): the ID2D1Factory, DWrite factory/text formats (1..9 per window),
// WS_EX flag sets, window styles, alpha VALUES (passed to Present), logo
// bitmaps, message loops, timers, threads. Device-lost handlers keep their
// own extra steps (ReallocateBuffer + LoadLogoBitmap / RebindRenderTarget)
// at the site; the class only exposes ReleaseTarget()/CreateTarget() so the
// identical recreate preamble is not duplicated a fifth time.
//
// Each site keeps a NON-OWNING `dc_render_target_` alias so its ~120 render
// references stay untouched; the alias is passed to CreateTarget/ReleaseTarget
// at the three points where the target changes (Create, RecreateAfterDeviceLost,
// Destroy) and is always kept in sync with target().
class LayeredD2DRenderer {
public:
    LayeredD2DRenderer() = default;
    ~LayeredD2DRenderer();

    // Single-owner semantics: the GDI DC/bitmap pair and the COM target are
    // raw handles; copying would double-DeleteObject / double-Release.
    LayeredD2DRenderer(const LayeredD2DRenderer&) = delete;
    LayeredD2DRenderer& operator=(const LayeredD2DRenderer&) = delete;

    // Create a fresh DC render target on `factory` (B8G8R8A8 premultiplied,
    // DEFAULT type - byte-identical props at all four original sites),
    // releasing any currently owned target first. On success, mirrors the new
    // target into the optional site alias `out_target`; on failure (null
    // factory or CreateDCRenderTarget failure) leaves target() null and
    // nulls `out_target` if provided. Returns false on failure.
    bool CreateTarget(ID2D1Factory* factory,
                      ID2D1DCRenderTarget** out_target = nullptr);

    // Release the owned target (no-op when null) and null the optional site
    // alias. Used by device-lost handlers (before the site's factory-null
    // early-return, preserving each site's original release-then-bail order)
    // and by the teardown path.
    void ReleaseTarget(ID2D1DCRenderTarget** out_target = nullptr);

    // (Re)allocate the 32bpp top-down DIB at width x height PHYSICAL pixels.
    // Frees the previous GDI triple first (select-old -> delete-bitmap ->
    // delete-dc, the order all four sites used). When a target is attached
    // and `bind` is true, refreshes it in one step: SetDpi(dpi) +
    // BindDC({0,0,width,height}) (REQ-R15: the DPI transform tracks the
    // monitor on every reallocation; badge/tooltip/about bound inline here,
    // drag_icon passes bind=false and rebinds through Rebind() itself).
    // The skip-when-no-target guard matched all four originals.
    void ReallocateBuffer(int width, int height, UINT dpi, bool bind = true);

    // Free the GDI triple (idempotent). The bits pointer is nulled here;
    // only about_window.cpp explicitly nulled pBits_ on teardown - an
    // unobservable difference since no site ever reads the bits back.
    void FreeBuffer();

    // Re-bind the attached target over the current buffer (per-render repaint).
    // No-op without target or buffer, matching every site's original
    // `if (!dc_render_target_ || !hMemDC_) return;` guard. `set_dpi`
    // additionally refreshes the DPI transform: only drag_icon's
    // RebindRenderTarget did that (after BindDC); badge/tooltip/about re-bind
    // size only. BindDC and SetDpi are pure state setters on the DC target -
    // running SetDpi first (this impl's order) is not observable.
    void Rebind(int width, int height, UINT dpi, bool set_dpi);

    // Commit the buffer to the layered window. No-op when hwnd or the buffer
    // is missing (matches each site's `if (!hwnd_ || !hMemDC_) return;`).
    // ptDst comes from GetWindowRect(hwnd) in screen coords (all four sites;
    // audit §2.2), the ULW destination DC is GetDC(nullptr) released right
    // after, and the blend is AC_SRC_OVER + AC_SRC_ALPHA with the per-window
    // constant `alpha` (badge 217/51 member, drag_icon 230 member,
    // tooltip/about fixed 245).
    void Present(HWND hwnd, int width, int height, BYTE alpha);

    ID2D1DCRenderTarget* target() const { return target_; }
    bool has_bitmap() const { return bitmap_ != nullptr; }

private:
    HDC mem_dc_ = nullptr;
    HBITMAP bitmap_ = nullptr;
    HBITMAP old_bitmap_ = nullptr;
    void* bits_ = nullptr;
    ID2D1DCRenderTarget* target_ = nullptr;
};

} // namespace emebalachat
