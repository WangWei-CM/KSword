#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <roapi.h>
#include <dwmapi.h>
#include <DispatcherQueue.h>
// 在本单元实例化 D2D 效果 GUID，效果执行仍由 Windows 合成器负责。
#include <initguid.h>
#include <d2d1effects.h>
#include <windows.graphics.effects.interop.h>
#include <windows.ui.composition.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Graphics.Effects.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/Windows.UI.Composition.Desktop.h>
#include "PageOverviewBackdrop.h"
#include <QWidget>
#include <algorithm>

#pragma comment(lib, "windowsapp.lib")
#pragma comment(lib, "dwmapi.lib")

namespace
{
    namespace composition = winrt::Windows::UI::Composition;
    namespace effects = winrt::Windows::Graphics::Effects;
    namespace effectsAbi = ABI::Windows::Graphics::Effects;
    using winrt::Windows::Foundation::PropertyValue;

    // 直接声明 D2D 高斯效果的属性图，避免为三个参数引入额外 Win2D DLL。
    struct GaussianEffect : winrt::implements<GaussianEffect,
        effects::IGraphicsEffect, effects::IGraphicsEffectSource, effectsAbi::IGraphicsEffectD2D1Interop>
    {
        winrt::hstring effectName{L"Blur"}; // 可调属性的稳定路径为 Blur.Amount。
        effects::IGraphicsEffectSource source{nullptr}; // 系统提供的窗口后方画面。
        winrt::hstring Name() const
        {
            return effectName;
        }
        void Name(winrt::hstring const& value)
        {
            effectName = value;
        }

        HRESULT __stdcall GetEffectId(GUID* result) noexcept override
        {
            if (!result) return E_POINTER;
            *result = CLSID_D2D1GaussianBlur;
            return S_OK;
        }
        // 合成器将 UI 滑条的标量写入高斯标准差，而不是写入背景透明度。
        HRESULT __stdcall GetNamedPropertyMapping(LPCWSTR name, UINT* index,
            effectsAbi::GRAPHICS_EFFECT_PROPERTY_MAPPING* mapping) noexcept override
        {
            if (!name || !index || !mapping) return E_POINTER;
            if (wcscmp(name, L"Amount") != 0) return E_INVALIDARG;
            *index = D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION;
            *mapping = effectsAbi::GRAPHICS_EFFECT_PROPERTY_MAPPING_DIRECT;
            return S_OK;
        }
        HRESULT __stdcall GetPropertyCount(UINT* count) noexcept override
        {
            if (!count) return E_POINTER;
            *count = 3;
            return S_OK;
        }
        // 固定质量与边缘采样方式，避免模糊后矩形边缘出现透明黑边。
        HRESULT __stdcall GetProperty(UINT index,
            ABI::Windows::Foundation::IPropertyValue** value) noexcept override
        {
            if (!value) return E_POINTER;
            *value = nullptr;
            try
            {
                winrt::Windows::Foundation::IInspectable property{nullptr};
                if (index == D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION)
                    property = PropertyValue::CreateSingle(20.0f);
                else if (index == D2D1_GAUSSIANBLUR_PROP_OPTIMIZATION)
                    property = PropertyValue::CreateUInt32(D2D1_GAUSSIANBLUR_OPTIMIZATION_BALANCED);
                else if (index == D2D1_GAUSSIANBLUR_PROP_BORDER_MODE)
                    property = PropertyValue::CreateUInt32(D2D1_BORDER_MODE_HARD);
                else
                    return E_INVALIDARG;
                winrt::copy_to_abi(property.as<winrt::Windows::Foundation::IPropertyValue>(),
                    *reinterpret_cast<void**>(value));
                return S_OK;
            }
            catch (...) { return winrt::to_hresult(); }
        }
        HRESULT __stdcall GetSource(UINT index,
            ABI::Windows::Graphics::Effects::IGraphicsEffectSource** value) noexcept override
        {
            if (!value) return E_POINTER;
            *value = nullptr;
            if (index != 0) return E_INVALIDARG;
            winrt::copy_to_abi(source, *reinterpret_cast<void**>(value));
            return S_OK;
        }
        HRESULT __stdcall GetSourceCount(UINT* count) noexcept override
        {
            if (!count) return E_POINTER;
            *count = 1;
            return S_OK;
        }
    };

    // 背景原生窗不接收焦点或点击；Qt 顶层保留全部交互与透明像素语义。
    LRESULT CALLBACK backdropProcedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
    {
        if (message == WM_NCHITTEST) return HTTRANSPARENT;
        if (message == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
        if (message == WM_ERASEBKGND) return TRUE;
        return ::DefWindowProcW(window, message, wparam, lparam);
    }

    // Win11 的公开开关允许 HostBackdropBrush 采样桌面；失败时保留普通透明 UI。
    bool enableHostBackdrop(HWND window)
    {
        const BOOL enabled = TRUE;
        return SUCCEEDED(::DwmSetWindowAttribute(window, DWMWA_USE_HOSTBACKDROPBRUSH,
            &enabled, sizeof(enabled)));
    }
}

struct PageOverviewBackdrop::State
{
    QWidget* foreground; // 与此对象同寿命的 Qt 前景窗口，不保存跨线程 QWidget。
    HWND background = nullptr; // 始终紧贴在前景 HWND 下方的无激活原生窗口。
    HMODULE coreMessaging = nullptr; // 动态加载，旧系统缺少入口不会阻止 Taskbar 启动。
    bool initialized = false; // 只进行一次能力探测；失败不会每次拖动滑条重试。
    bool ownsApartment = false; // 只平衡本对象成功取得的 RoInitialize 引用。
    int amount = 20; // 0 完全隐藏背景；正数直接写入高斯效果参数。
    winrt::Windows::System::DispatcherQueueController queue{nullptr};
    composition::Compositor compositor{nullptr};
    composition::Desktop::DesktopWindowTarget target{nullptr};
    composition::CompositionEffectBrush blur{nullptr};

    // 初始化只在首次显示后执行，所有失败均交给 UI 显示不可用而不影响导航。
    bool initialize()
    {
        if (initialized) return bool(blur);
        initialized = true;
        try
        {
            const HRESULT apartment = ::RoInitialize(RO_INIT_SINGLETHREADED);
            ownsApartment = SUCCEEDED(apartment);
            if (FAILED(apartment) && apartment != RPC_E_CHANGED_MODE) return false;
            if (!winrt::Windows::System::DispatcherQueue::GetForCurrentThread())
            {
                coreMessaging = ::LoadLibraryExW(L"CoreMessaging.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
                if (!coreMessaging) return false;
                using CreateQueue = HRESULT(WINAPI*)(DispatcherQueueOptions,
                    ABI::Windows::System::IDispatcherQueueController**);
                const auto createQueue = reinterpret_cast<CreateQueue>(
                    ::GetProcAddress(coreMessaging, "CreateDispatcherQueueController"));
                if (!createQueue) return false;
                DispatcherQueueOptions options{sizeof(options), DQTYPE_THREAD_CURRENT, DQTAT_COM_NONE};
                winrt::check_hresult(createQueue(options,
                    reinterpret_cast<ABI::Windows::System::IDispatcherQueueController**>(winrt::put_abi(queue))));
            }
            // 不给 Qt 的 layered HWND 挂合成树，防止它的 backing store 与 DWM 互相覆盖。
            WNDCLASSW windowClass{};
            windowClass.lpfnWndProc = backdropProcedure;
            windowClass.hInstance = ::GetModuleHandleW(nullptr);
            windowClass.lpszClassName = L"KSword.Taskbar.OverviewBackdrop";
            if (!::RegisterClassW(&windowClass) && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
                return false;
            background = ::CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOOLWINDOW
                | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT, windowClass.lpszClassName, L"",
                WS_POPUP | WS_DISABLED, 0, 0, 1, 1, nullptr, nullptr, windowClass.hInstance, nullptr);
            if (!background || !enableHostBackdrop(background)) return false;

            compositor = composition::Compositor();
            const auto interop = compositor.as<ABI::Windows::UI::Composition::Desktop::ICompositorDesktopInterop>();
            winrt::check_hresult(interop->CreateDesktopWindowTarget(background, false,
                reinterpret_cast<ABI::Windows::UI::Composition::Desktop::IDesktopWindowTarget**>(winrt::put_abi(target))));
            auto effect = winrt::make_self<GaussianEffect>();
            effect->source = composition::CompositionEffectSourceParameter(L"Desktop");
            const auto factory = compositor.CreateEffectFactory(effect.as<effects::IGraphicsEffect>(), {L"Blur.Amount"});
            blur = factory.CreateBrush();
            // HostBackdrop 自带系统底层柔化；滑条在其上追加真正的可调高斯模糊。
            blur.SetSourceParameter(L"Desktop", compositor.CreateHostBackdropBrush());
            const auto visual = compositor.CreateSpriteVisual();
            visual.RelativeSizeAdjustment({1.0f, 1.0f});
            visual.Brush(blur);
            target.Root(visual);
            return true;
        }
        catch (...)
        {
            blur = nullptr;
            return false;
        }
    }

    // 隐藏和销毁都先脱开可见合成目标，再释放 COM 对象与原生窗口。
    ~State()
    {
        if (background) ::ShowWindow(background, SW_HIDE);
        try
        {
            if (target) target.Close();
            target = nullptr;
            blur = nullptr;
            if (compositor) compositor.Close();
            compositor = nullptr;
            if (queue) queue.ShutdownQueueAsync();
        }
        catch (...) { /* 退出期间不让 DWM 设备丢失异常越过析构边界。 */ }
        queue = nullptr;
        if (background) ::DestroyWindow(background);
        if (ownsApartment) ::RoUninitialize();
        // DispatcherQueue 的异步退出仍可能经过 CoreMessaging，模块保留到进程退出。
    }
};

PageOverviewBackdrop::PageOverviewBackdrop(QWidget* foreground) : m_state(std::make_unique<State>())
{
    m_state->foreground = foreground;
}

PageOverviewBackdrop::~PageOverviewBackdrop() = default;

bool PageOverviewBackdrop::setAmount(int amount)
{
    m_state->amount = std::clamp(amount, 0, 40);
    if (m_state->amount == 0)
    {
        hide();
        return true;
    }
    if (!m_state->initialize()) return false;
    try
    {
        m_state->blur.Properties().InsertScalar(L"Blur.Amount", float(m_state->amount));
        synchronize();
        return true;
    }
    catch (...)
    {
        hide();
        return false;
    }
}

void PageOverviewBackdrop::synchronize()
{
    if (!m_state->background || !m_state->blur) return;
    const HWND front = reinterpret_cast<HWND>(m_state->foreground->winId());
    if (!m_state->amount || !m_state->foreground->isVisible() || !::IsWindowVisible(front) || ::IsIconic(front))
    {
        hide();
        return;
    }
    RECT bounds{}; // 原生物理坐标避免跨屏不同 DPI 时重复缩放 Qt 逻辑像素。
    if (::GetWindowRect(front, &bounds))
    {
        ::SetWindowPos(m_state->background, front, bounds.left, bounds.top,
            bounds.right - bounds.left, bounds.bottom - bounds.top,
            SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
    }
}

void PageOverviewBackdrop::hide()
{
    if (m_state->background) ::ShowWindow(m_state->background, SW_HIDE);
}
