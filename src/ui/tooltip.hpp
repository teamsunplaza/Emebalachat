#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <sapi.h>

namespace emebalachat {

// Returns the Windows LCID for a given language code or name. Returns 0 if unknown.
DWORD GetLcidForLanguage(std::string_view target_lang_name_or_code);

class TooltipWindow {
public:
    using LanguageChangeCallback = std::function<void(std::string_view new_target_lang)>;

    TooltipWindow();
    ~TooltipWindow();

    bool Create(HINSTANCE hInstance);
    void Destroy();

    // Displays the tooltip with translated text at screen coordinates (x, y)
    void ShowTranslation(
        int x, int y,
        std::wstring_view source_text,
        std::string_view source_lang_code,
        std::string_view target_lang,
        std::wstring_view translated_text
    );

    void Dismiss();
    bool IsVisible() const { return visible_; }
    HWND GetHwnd() const { return hwnd_; }

    void SetLanguageChangeCallback(LanguageChangeCallback cb) { lang_change_cb_ = std::move(cb); }

    // Audio & Clipboard actions
    void SpeakCurrentText();
    void StopTTS();
    void CopyToClipboard();

    // Voice selection for multi-language TTS
    bool SelectVoiceForLanguage(std::string_view target_lang_name_or_code);
    const std::string& GetCurrentVoiceName() const { return current_voice_name_; }
    const std::string& GetCurrentVoiceLanguage() const { return current_voice_lang_; }

    // Retrieves current translation content
    const std::wstring& GetSourceText() const { return source_text_; }
    const std::string& GetSourceLangCode() const { return source_lang_code_; }
    const std::string& GetTargetLang() const { return target_lang_; }

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    void Render();
    void UpdateLayered();
    void ReallocateBuffer(int width, int height);
    void LoadLogoBitmap();
    void InitSapi();
    void CleanupSapi();

    HWND hwnd_ = nullptr;
    HINSTANCE hInstance_ = nullptr;
    bool visible_ = false;

    std::wstring source_text_;
    std::string source_lang_code_;
    std::string target_lang_;
    std::wstring translated_text_;

    int current_width_ = 360;
    int current_height_ = 160;

    // GDI Memory DC & DIB Section
    HDC hMemDC_ = nullptr;
    HBITMAP hBitmap_ = nullptr;
    HBITMAP hOldBitmap_ = nullptr;
    void* pBits_ = nullptr;

    // Direct2D & DirectWrite
    ID2D1Factory* d2d_factory_ = nullptr;
    ID2D1DCRenderTarget* dc_render_target_ = nullptr;
    ID2D1Bitmap* logo_bitmap_ = nullptr;
    IDWriteFactory* dwrite_factory_ = nullptr;
    IDWriteTextFormat* header_format_ = nullptr;
    IDWriteTextFormat* body_format_ = nullptr;
    IDWriteTextFormat* button_format_ = nullptr;
    IDWriteTextFormat* small_format_ = nullptr;

    // Interactive Button Rectangles (local window coordinates)
    D2D1_RECT_F copy_btn_rect_ = {};
    D2D1_RECT_F tts_btn_rect_ = {};
    D2D1_RECT_F lang_btn_rect_ = {};
    D2D1_RECT_F close_btn_rect_ = {};

    int hovered_btn_ = 0; // 0=none, 1=copy, 2=tts, 3=lang, 4=close
    bool copied_feedback_ = false;

    // SAPI TTS Voice COM interface
    ISpVoice* voice_ = nullptr;
    ISpObjectToken* default_voice_token_ = nullptr;
    std::string current_voice_name_;
    std::string current_voice_lang_;

    LanguageChangeCallback lang_change_cb_;

    static constexpr UINT_PTR kTimerCopiedFeedback = 4001;
};

} // namespace emebalachat
