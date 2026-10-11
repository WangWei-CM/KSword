#pragma once

#include <memory>
#include <string>

enum class SplashProgressMode
{
    Determinate,
    Indeterminate
};

// The splash owns its Win32 UI thread so animation continues during startup work.
// The API deliberately matches the existing kStartupSplash call sites.
class kStartupSplash final
{
public:
    kStartupSplash();
    ~kStartupSplash();
    kStartupSplash(const kStartupSplash&) = delete;
    kStartupSplash& operator=(const kStartupSplash&) = delete;

    // If unset, loads Style\KswordHome_2.png next to the host executable.
    // Set before show(); fits to 640 logical pixels wide, preserving the aspect
    // ratio. At 200% DPI, renders directly at 1280 physical pixels wide.
    void setBackgroundImagePath(const std::wstring& path);
    void setFooterText(const std::wstring& email, const std::wstring& buildDate,
        const std::wstring& repository);
    void setProgressMode(SplashProgressMode mode);
    // RGB value in 0xRRGGBB form. Defaults to Ksword's #43A0FF.
    void setAccentColorRgb(unsigned int rgb);
    bool show(const std::string& initialStatusText = std::string());
    void hide();
    void progress(const std::string& operationName, int progressPercent);
    bool ready() const;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

extern kStartupSplash kSplash;
