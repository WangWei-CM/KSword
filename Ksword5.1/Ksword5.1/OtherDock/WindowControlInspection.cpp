#include "WindowControlInspection.h"
#include <objbase.h>
#include <oleauto.h>
#include <UIAutomation.h>
#include <wrl/client.h>
#include <QSet>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <algorithm>
#include <cmath>
#include <limits>
#include <tuple>

#pragma comment(lib, "UIAutomationCore.lib")
#pragma comment(lib, "OleAut32.lib")

namespace ks::control_inspection
{
    using Microsoft::WRL::ComPtr;
    namespace
    {
        struct DpiScope
        {
            DPI_AWARENESS_CONTEXT previous = ::SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
            ~DpiScope() { if (previous) ::SetThreadDpiAwarenessContext(previous); }
        };
        QString hex(quintptr value) { return QStringLiteral("0x%1").arg(value, 0, 16).toUpper(); }
        QString valueText(const VARIANT& value)
        {
            switch (value.vt)
            {
            case VT_BSTR: return QString::fromWCharArray(value.bstrVal, static_cast<int>(::SysStringLen(value.bstrVal)));
            case VT_BOOL: return value.boolVal ? QStringLiteral("true") : QStringLiteral("false");
            case VT_I4: return QString::number(value.lVal);
            case VT_UI4: return QString::number(value.ulVal);
            case VT_R8: return QString::number(value.dblVal);
            default: return {};
            }
        }
        struct Property { PROPERTYID id; const char* name; };
        const Property properties[] = {
            {UIA_NamePropertyId, "Name"}, {UIA_ControlTypePropertyId, "ControlType"},
            {UIA_LocalizedControlTypePropertyId, "LocalizedControlType"},
            {UIA_AutomationIdPropertyId, "AutomationId"}, {UIA_FrameworkIdPropertyId, "FrameworkId"},
            {UIA_ClassNamePropertyId, "ClassName"}, {UIA_ProcessIdPropertyId, "ProcessId"},
            {UIA_NativeWindowHandlePropertyId, "NativeWindowHandle"},
            {UIA_IsOffscreenPropertyId, "IsOffscreen"}, {UIA_IsEnabledPropertyId, "IsEnabled"},
            {UIA_HasKeyboardFocusPropertyId, "HasKeyboardFocus"},
            {UIA_IsKeyboardFocusablePropertyId, "IsKeyboardFocusable"}, {UIA_IsPasswordPropertyId, "IsPassword"},
            {UIA_AccessKeyPropertyId, "AccessKey"}, {UIA_AcceleratorKeyPropertyId, "AcceleratorKey"},
            {UIA_HelpTextPropertyId, "HelpText"}, {UIA_ProviderDescriptionPropertyId, "ProviderDescription"},
            {UIA_IsInvokePatternAvailablePropertyId, "Invoke"}, {UIA_IsValuePatternAvailablePropertyId, "Value"},
            {UIA_IsRangeValuePatternAvailablePropertyId, "RangeValue"},
            {UIA_IsSelectionPatternAvailablePropertyId, "Selection"},
            {UIA_IsSelectionItemPatternAvailablePropertyId, "SelectionItem"},
            {UIA_IsTogglePatternAvailablePropertyId, "Toggle"},
            {UIA_IsExpandCollapsePatternAvailablePropertyId, "ExpandCollapse"},
            {UIA_IsScrollPatternAvailablePropertyId, "Scroll"}, {UIA_IsTextPatternAvailablePropertyId, "Text"},
            {UIA_IsGridPatternAvailablePropertyId, "Grid"}, {UIA_IsGridItemPatternAvailablePropertyId, "GridItem"},
            {UIA_IsWindowPatternAvailablePropertyId, "Window"}, {UIA_IsTransformPatternAvailablePropertyId, "Transform"},
            {UIA_IsTablePatternAvailablePropertyId, "Table"}, {UIA_IsTableItemPatternAvailablePropertyId, "TableItem"},
            {UIA_IsScrollItemPatternAvailablePropertyId, "ScrollItem"},
            {UIA_IsMultipleViewPatternAvailablePropertyId, "MultipleView"},
            {UIA_IsDockPatternAvailablePropertyId, "Dock"}, {UIA_IsItemContainerPatternAvailablePropertyId, "ItemContainer"},
            {UIA_IsVirtualizedItemPatternAvailablePropertyId, "VirtualizedItem"}
        };
        QString runtimeKey(IUIAutomationElement* element)
        {
            SAFEARRAY* ids = nullptr;
            QStringList parts;
            if (SUCCEEDED(element->GetRuntimeId(&ids)) && ids)
            {
                LONG first = 0, last = -1;
                ::SafeArrayGetLBound(ids, 1, &first);
                ::SafeArrayGetUBound(ids, 1, &last);
                for (LONG index = first; index <= last; ++index)
                {
                    int value = 0;
                    if (SUCCEEDED(::SafeArrayGetElement(ids, &index, &value))) parts << QString::number(value);
                }
                ::SafeArrayDestroy(ids);
            }
            return parts.join(QLatin1Char('/'));
        }
        Node readElement(IUIAutomationElement* element, HWND host, const QString& parent)
        {
            Node node;
            node.id = runtimeKey(element);
            node.parent = parent;
            node.host = host;
            node.hostBounds = physicalBounds(host);
            node.properties.insert(QStringLiteral("RuntimeId"), node.id);
            for (const auto& property : properties)
            {
                VARIANT value; ::VariantInit(&value);
                if (SUCCEEDED(element->GetCachedPropertyValue(property.id, &value)))
                {
                    const QString text = valueText(value);
                    if (!text.isEmpty()) node.properties.insert(QString::fromLatin1(property.name), text);
                }
                ::VariantClear(&value);
            }
            node.name = node.properties.value(QStringLiteral("Name"));
            node.type = node.properties.value(QStringLiteral("LocalizedControlType"));
            node.enabled = node.properties.value(QStringLiteral("IsEnabled")) != QStringLiteral("false");
            node.offscreen = node.properties.value(QStringLiteral("IsOffscreen")) == QStringLiteral("true");
            UIA_HWND native = nullptr;
            if (SUCCEEDED(element->get_CachedNativeWindowHandle(&native))) node.window = static_cast<HWND>(native);
            node.properties.insert(QStringLiteral("NativeWindowHandle"), hex(reinterpret_cast<quintptr>(native)));
            RECT bounds{};
            if (SUCCEEDED(element->get_CachedBoundingRectangle(&bounds)))
                node.bounds = QRect(bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top);
            return node;
        }
        void readPatternValues(IUIAutomationElement* element, Node& node)
        {
            const Property values[] = {
                {UIA_ValueValuePropertyId, "Value.Value"}, {UIA_ValueIsReadOnlyPropertyId, "Value.IsReadOnly"},
                {UIA_RangeValueValuePropertyId, "RangeValue.Value"}, {UIA_RangeValueMinimumPropertyId, "RangeValue.Minimum"},
                {UIA_RangeValueMaximumPropertyId, "RangeValue.Maximum"},
                {UIA_ToggleToggleStatePropertyId, "Toggle.State"},
                {UIA_ExpandCollapseExpandCollapseStatePropertyId, "ExpandCollapse.State"},
                {UIA_SelectionItemIsSelectedPropertyId, "SelectionItem.IsSelected"},
                {UIA_ScrollHorizontalScrollPercentPropertyId, "Scroll.HorizontalPercent"},
                {UIA_ScrollVerticalScrollPercentPropertyId, "Scroll.VerticalPercent"},
                {UIA_GridRowCountPropertyId, "Grid.RowCount"}, {UIA_GridColumnCountPropertyId, "Grid.ColumnCount"}
            };
            for (const auto& property : values)
            {
                const QString label = QString::fromLatin1(property.name);
                const QString pattern = label.section(QLatin1Char('.'), 0, 0);
                if (node.properties.value(pattern) != QStringLiteral("true")) continue;
                if (property.id == UIA_ValueValuePropertyId
                    && node.properties.value(QStringLiteral("IsPassword")) == QStringLiteral("true")) continue;
                VARIANT value; ::VariantInit(&value);
                if (SUCCEEDED(element->GetCurrentPropertyValue(property.id, &value)))
                    node.properties.insert(label, valueText(value));
                ::VariantClear(&value);
            }
        }
        bool parseBoolean(QString value, bool& result)
        {
            value = value.trimmed();
            if (!value.compare(QStringLiteral("true"), Qt::CaseInsensitive)
                || !value.compare(QStringLiteral("on"), Qt::CaseInsensitive)
                || value == QStringLiteral("1"))
            { result = true; return true; }
            if (!value.compare(QStringLiteral("false"), Qt::CaseInsensitive)
                || !value.compare(QStringLiteral("off"), Qt::CaseInsensitive)
                || value == QStringLiteral("0"))
            { result = false; return true; }
            return false;
        }
        HRESULT mutateNative(HWND window, const QString& property, const QString& value);
        HRESULT mutateElement(IUIAutomationElement* element, const QString& property,
            const QString& value, HWND nativeWindow)
        {
            if (!element) return UIA_E_ELEMENTNOTAVAILABLE;
            if (property == QStringLiteral("Value.Value"))
            {
                ComPtr<IUnknown> object;
                HRESULT hr = element->GetCurrentPattern(UIA_ValuePatternId, object.GetAddressOf());
                if (FAILED(hr) || !object)
                    return nativeWindow ? mutateNative(nativeWindow, QStringLiteral("Name"), value)
                        : (FAILED(hr) ? hr : UIA_E_NOTSUPPORTED);
                ComPtr<IUIAutomationValuePattern> pattern;
                if (FAILED(hr = object.As(&pattern)) || !pattern)
                    return nativeWindow ? mutateNative(nativeWindow, QStringLiteral("Name"), value)
                        : (FAILED(hr) ? hr : UIA_E_NOTSUPPORTED);
                BOOL readOnly = FALSE;
                if (FAILED(hr = pattern->get_CurrentIsReadOnly(&readOnly)) || readOnly) return readOnly ? UIA_E_INVALIDOPERATION : hr;
                const std::wstring text = value.toStdWString();
                BSTR bstr = ::SysAllocStringLen(text.data(), static_cast<UINT>(text.size()));
                if (!bstr) return E_OUTOFMEMORY;
                hr = pattern->SetValue(bstr); ::SysFreeString(bstr); return hr;
            }
            if (property == QStringLiteral("RangeValue.Value"))
            {
                bool ok = false; const double number = value.trimmed().toDouble(&ok);
                if (!ok || !std::isfinite(number)) return E_INVALIDARG;
                ComPtr<IUnknown> object;
                HRESULT hr = element->GetCurrentPattern(UIA_RangeValuePatternId, object.GetAddressOf());
                if (FAILED(hr) || !object) return FAILED(hr) ? hr : UIA_E_NOTSUPPORTED;
                ComPtr<IUIAutomationRangeValuePattern> pattern;
                if (FAILED(hr = object.As(&pattern)) || !pattern) return FAILED(hr) ? hr : UIA_E_NOTSUPPORTED;
                BOOL readOnly = FALSE;
                if (FAILED(hr = pattern->get_CurrentIsReadOnly(&readOnly)) || readOnly) return readOnly ? UIA_E_INVALIDOPERATION : hr;
                double minimum = 0.0, maximum = 0.0;
                if (SUCCEEDED(pattern->get_CurrentMinimum(&minimum)) && SUCCEEDED(pattern->get_CurrentMaximum(&maximum))
                    && (number < minimum || number > maximum)) return E_INVALIDARG;
                return pattern->SetValue(number);
            }
            if (property == QStringLiteral("Toggle.State"))
            {
                bool ok = false; const int desired = value.trimmed().toInt(&ok);
                if (!ok || desired < ToggleState_Off || desired > ToggleState_Indeterminate) return E_INVALIDARG;
                ComPtr<IUnknown> object;
                HRESULT hr = element->GetCurrentPattern(UIA_TogglePatternId, object.GetAddressOf());
                if (FAILED(hr) || !object) return FAILED(hr) ? hr : UIA_E_NOTSUPPORTED;
                ComPtr<IUIAutomationTogglePattern> pattern;
                if (FAILED(hr = object.As(&pattern)) || !pattern) return FAILED(hr) ? hr : UIA_E_NOTSUPPORTED;
                for (int attempt = 0; attempt < 3; ++attempt)
                {
                    ToggleState state = ToggleState_Off;
                    if (FAILED(hr = pattern->get_CurrentToggleState(&state))) return hr;
                    if (state == desired) return S_OK;
                    if (FAILED(hr = pattern->Toggle())) return hr;
                }
                return UIA_E_INVALIDOPERATION;
            }
            if (property == QStringLiteral("ExpandCollapse.State"))
            {
                bool ok = false; const int desired = value.trimmed().toInt(&ok);
                if (!ok || (desired != ExpandCollapseState_Collapsed && desired != ExpandCollapseState_Expanded)) return E_INVALIDARG;
                ComPtr<IUnknown> object;
                HRESULT hr = element->GetCurrentPattern(UIA_ExpandCollapsePatternId, object.GetAddressOf());
                if (FAILED(hr) || !object) return FAILED(hr) ? hr : UIA_E_NOTSUPPORTED;
                ComPtr<IUIAutomationExpandCollapsePattern> pattern;
                if (FAILED(hr = object.As(&pattern)) || !pattern) return FAILED(hr) ? hr : UIA_E_NOTSUPPORTED;
                ExpandCollapseState state = ExpandCollapseState_LeafNode;
                if (FAILED(hr = pattern->get_CurrentExpandCollapseState(&state))) return hr;
                if (state == desired) return S_OK;
                return desired == ExpandCollapseState_Collapsed ? pattern->Collapse() : pattern->Expand();
            }
            if (property == QStringLiteral("SelectionItem.IsSelected"))
            {
                bool selected = false;
                if (!parseBoolean(value, selected)) return E_INVALIDARG;
                ComPtr<IUnknown> object;
                HRESULT hr = element->GetCurrentPattern(UIA_SelectionItemPatternId, object.GetAddressOf());
                if (FAILED(hr) || !object) return FAILED(hr) ? hr : UIA_E_NOTSUPPORTED;
                ComPtr<IUIAutomationSelectionItemPattern> pattern;
                if (FAILED(hr = object.As(&pattern)) || !pattern) return FAILED(hr) ? hr : UIA_E_NOTSUPPORTED;
                if (selected) return pattern->Select();
                return pattern->RemoveFromSelection();
            }
            if (property == QStringLiteral("Scroll.HorizontalPercent")
                || property == QStringLiteral("Scroll.VerticalPercent"))
            {
                bool ok = false; const double number = value.trimmed().toDouble(&ok);
                if (!ok || !std::isfinite(number) || number < 0.0 || number > 100.0) return E_INVALIDARG;
                ComPtr<IUnknown> object;
                HRESULT hr = element->GetCurrentPattern(UIA_ScrollPatternId, object.GetAddressOf());
                if (FAILED(hr) || !object) return FAILED(hr) ? hr : UIA_E_NOTSUPPORTED;
                ComPtr<IUIAutomationScrollPattern> pattern;
                if (FAILED(hr = object.As(&pattern)) || !pattern) return FAILED(hr) ? hr : UIA_E_NOTSUPPORTED;
                double horizontal = UIA_ScrollPatternNoScroll, vertical = UIA_ScrollPatternNoScroll;
                pattern->get_CurrentHorizontalScrollPercent(&horizontal);
                pattern->get_CurrentVerticalScrollPercent(&vertical);
                if (property == QStringLiteral("Scroll.HorizontalPercent")) horizontal = number;
                else vertical = number;
                return pattern->SetScrollPercent(horizontal, vertical);
            }
            return UIA_E_NOTSUPPORTED;
        }
        Node readNative(HWND window, HWND host, const QString& parent)
        {
            const DpiScope dpi;
            Node node;
            node.id = nativeKey(window); node.parent = parent; node.window = window; node.host = host;
            node.hostBounds = physicalBounds(host);
            wchar_t text[1024]{};
            ::GetWindowTextW(window, text, 1024);
            node.name = QString::fromWCharArray(text);
            ::GetClassNameW(window, text, 1024);
            node.type = QString::fromWCharArray(text);
            node.bounds = physicalBounds(window);
            node.offscreen = !::IsWindowVisible(window) || ::IsIconic(host);
            node.enabled = ::IsWindowEnabled(window) != FALSE;
            DWORD pid = 0;
            const DWORD tid = ::GetWindowThreadProcessId(window, &pid);
            node.properties = {{QStringLiteral("Name"), node.name}, {QStringLiteral("ClassName"), node.type},
                {QStringLiteral("NativeWindowHandle"), hex(reinterpret_cast<quintptr>(window))},
                {QStringLiteral("ProcessId"), QString::number(pid)}, {QStringLiteral("ThreadId"), QString::number(tid)},
                {QStringLiteral("ControlId"), QString::number(::GetDlgCtrlID(window))},
                {QStringLiteral("Style"), hex(static_cast<quintptr>(::GetWindowLongPtrW(window, GWL_STYLE)))},
                {QStringLiteral("ExStyle"), hex(static_cast<quintptr>(::GetWindowLongPtrW(window, GWL_EXSTYLE)))},
                {QStringLiteral("IsEnabled"), node.enabled ? QStringLiteral("true") : QStringLiteral("false")},
                {QStringLiteral("IsOffscreen"), node.offscreen ? QStringLiteral("true") : QStringLiteral("false")}};
            return node;
        }
        HRESULT mutateNative(HWND window, const QString& property, const QString& value)
        {
            if (!window || !::IsWindow(window)) return UIA_E_ELEMENTNOTAVAILABLE;
            if (property == QStringLiteral("Name"))
            {
                const std::wstring text = value.toStdWString();
                return ::SetWindowTextW(window, text.c_str()) ? S_OK : HRESULT_FROM_WIN32(::GetLastError());
            }
            if (property == QStringLiteral("IsEnabled"))
            {
                bool enabled = false;
                if (!parseBoolean(value, enabled)) return E_INVALIDARG;
                ::EnableWindow(window, enabled ? TRUE : FALSE);
                return ::IsWindow(window) ? S_OK : UIA_E_ELEMENTNOTAVAILABLE;
            }
            return UIA_E_NOTSUPPORTED;
        }
        // COM events only mark the mailbox dirty; no Qt widgets or translations.
        class Events final : public IUIAutomationStructureChangedEventHandler,
            public IUIAutomationPropertyChangedEventHandler
        {
        public:
            explicit Events(std::shared_ptr<std::atomic_bool> dirty) : m_dirty(std::move(dirty)) {}
            HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** result) override
            {
                if (!result) return E_POINTER;
                *result = nullptr;
                if (id == __uuidof(IUnknown) || id == __uuidof(IUIAutomationStructureChangedEventHandler))
                    *result = static_cast<IUIAutomationStructureChangedEventHandler*>(this);
                else if (id == __uuidof(IUIAutomationPropertyChangedEventHandler))
                    *result = static_cast<IUIAutomationPropertyChangedEventHandler*>(this);
                else return E_NOINTERFACE;
                AddRef(); return S_OK;
            }
            ULONG STDMETHODCALLTYPE AddRef() override { return ++m_refs; }
            ULONG STDMETHODCALLTYPE Release() override { const ULONG refs = --m_refs; if (!refs) delete this; return refs; }
            HRESULT STDMETHODCALLTYPE HandleStructureChangedEvent(IUIAutomationElement*, StructureChangeType, SAFEARRAY*) override
            { m_dirty->store(true); return S_OK; }
            HRESULT STDMETHODCALLTYPE HandlePropertyChangedEvent(IUIAutomationElement*, PROPERTYID, VARIANT) override
            { m_dirty->store(true); return S_OK; }
        private:
            std::atomic<ULONG> m_refs{1};
            std::shared_ptr<std::atomic_bool> m_dirty;
        };
    }

    QRect physicalBounds(HWND window)
    {
        const DpiScope dpi;
        RECT bounds{};
        if (!::GetWindowRect(window, &bounds)) return {};
        return QRect(bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top);
    }
    QString nativeKey(HWND window)
    {
        DWORD pid = 0;
        const DWORD tid = ::GetWindowThreadProcessId(window, &pid);
        return QStringLiteral("hwnd/%1/%2/%3").arg(reinterpret_cast<quintptr>(window)).arg(pid).arg(tid);
    }
    bool belongs(HWND window, HWND root, HWND excluded)
    {
        if (!window || !root || window == excluded || ::IsChild(excluded, window)) return false;
        if (window == root || ::IsChild(root, window)) return true;
        HWND ancestor = ::GetAncestor(window, GA_ROOT);
        for (HWND owner = ancestor; owner; owner = ::GetWindow(owner, GW_OWNER))
            if (owner == root || ::IsChild(root, owner)) return true;
        // Menus/combobox popups may have no owner. Confirm their GUI-thread
        // active/focus relationship, rather than admitting every same-PID window.
        DWORD pid = 0, rootPid = 0;
        const DWORD tid = ::GetWindowThreadProcessId(ancestor, &pid);
        ::GetWindowThreadProcessId(root, &rootPid);
        if (pid != rootPid) return false;
        wchar_t name[128]{}; ::GetClassNameW(ancestor, name, 128);
        if (wcscmp(name, L"#32768") && wcscmp(name, L"ComboLBox") && wcscmp(name, L"tooltips_class32")) return false;
        GUITHREADINFO info{}; info.cbSize = sizeof(info);
        if (!::GetGUIThreadInfo(tid, &info)) return false;
        return info.hwndActive == root || ::IsChild(root, info.hwndActive)
            || info.hwndFocus == root || ::IsChild(root, info.hwndFocus);
    }
    QVector<HWND> relatedRoots(HWND root, HWND excluded)
    {
        QVector<HWND> roots{root};
        struct Context { HWND root, excluded; QVector<HWND>* roots; } context{root, excluded, &roots};
        ::EnumWindows([](HWND window, LPARAM argument) -> BOOL {
            auto& context = *reinterpret_cast<Context*>(argument);
            if (window != context.root && ::IsWindowVisible(window)
                && belongs(window, context.root, context.excluded)) context.roots->push_back(window);
            return TRUE;
        }, reinterpret_cast<LPARAM>(&context));
        return roots;
    }
    bool PickGate::mouse(UINT message, QPoint point, bool inScope, bool& completed)
    {
        completed = false;
        if (message == WM_LBUTTONDOWN && armed && !consuming && inScope)
        { consuming = true; pressed = point; return true; }
        if (message == WM_LBUTTONUP && consuming)
        { consuming = false; completed = armed; armed = false; return true; }
        return false;
    }

    struct Collector::State
    {
        std::mutex mutex;
        std::condition_variable wake;
        Request request;
        quint64 epoch = 0, serial = 0;
        QPoint point;
        QString hitId;
        QString mutationId, mutationProperty, mutationValue;
        bool stopped = false, scan = false, hit = false, mutate = false, picked = false;
        ReplyMailbox replies;
        std::shared_ptr<std::atomic_bool> dirty = std::make_shared<std::atomic_bool>(false);
        HDESK desktop = nullptr;
        ~State() { if (desktop) ::CloseDesktop(desktop); }
    };
    Collector::Collector() : m_state(std::make_shared<State>())
    {
        const auto state = m_state;
        HANDLE desktop = nullptr;
        if (::DuplicateHandle(::GetCurrentProcess(), ::GetThreadDesktop(::GetCurrentThreadId()),
            ::GetCurrentProcess(), &desktop, 0, FALSE, DUPLICATE_SAME_ACCESS)) state->desktop = static_cast<HDESK>(desktop);
        // Detached lifetime is backed by shared state, never by this/dialog.
        // A slow provider cannot keep a closing dialog blocked on join().
        std::thread([state] {
            const HDESK previousDesktop = ::GetThreadDesktop(::GetCurrentThreadId());
            const bool bound = state->desktop && ::SetThreadDesktop(state->desktop);
            const HRESULT initialized = bound ? ::CoInitializeEx(nullptr, COINIT_MULTITHREADED) : E_ACCESSDENIED;
            {
                ComPtr<IUIAutomation> automation;
                HRESULT setup = initialized;
                if (SUCCEEDED(initialized)) setup = ::CoCreateInstance(CLSID_CUIAutomation8, nullptr,
                    CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&automation));
                if (FAILED(setup) && SUCCEEDED(initialized)) setup = ::CoCreateInstance(CLSID_CUIAutomation, nullptr,
                    CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&automation));
                ComPtr<IUIAutomation2> modern;
                if (automation && SUCCEEDED(automation.As(&modern)))
                { modern->put_ConnectionTimeout(1000); modern->put_TransactionTimeout(1000); modern->put_AutoSetFocus(FALSE); }
                ComPtr<IUIAutomationCacheRequest> cache;
                if (automation)
                {
                    automation->CreateCacheRequest(&cache);
                    if (cache)
                    {
                        cache->put_TreeScope(TreeScope_Element);
                        cache->AddProperty(UIA_BoundingRectanglePropertyId);
                        for (const auto& property : properties) cache->AddProperty(property.id);
                    }
                }
                ComPtr<Events> events;
                events.Attach(new Events(state->dirty));
                struct Pending { ComPtr<IUIAutomationElement> element; QString parent; HWND host; bool root = false; };
                std::deque<Pending> pending;
                std::deque<std::tuple<HWND, HWND, QString>> nativePending;
                QSet<QString> visited;
                QMap<QString, Pending> elements;
                QMap<QString, Node> records;
                ComPtr<IUIAutomationTreeWalker> walker;
                Request current;
                bool scanning = false;
                const auto publish = [&](Reply reply) {
                    std::unique_lock<std::mutex> lock(state->mutex);
                    if (!reply.hover && !reply.mutation)
                        state->wake.wait(lock, [&] { return state->stopped || state->epoch != reply.generation
                            || !state->replies.treeFull(); });
                    if (!state->stopped && state->epoch == reply.generation) state->replies.push(std::move(reply));
                };
                for (;;)
                {
                    Request request;
                    QPoint point;
                    quint64 serial = 0;
                    QString hitId;
                    QString mutationId, mutationProperty, mutationValue;
                    bool scan = false, hit = false, mutate = false, picked = false, scanBudget = false;
                    {
                        std::unique_lock<std::mutex> lock(state->mutex);
                        if (!scanning || state->replies.treeFull())
                            state->wake.wait(lock, [&] { return state->stopped || state->scan || state->hit || state->mutate
                                || (scanning && !state->replies.treeFull()); });
                        scanBudget = !state->replies.treeFull();
                        if (state->stopped) break;
                        request = state->request;
                        if (scanning && current.generation != state->epoch)
                        { pending.clear(); nativePending.clear(); scanning = false; }
                        scan = state->scan; state->scan = false;
                        hit = state->hit; state->hit = false;
                        mutate = state->mutate; state->mutate = false;
                        point = state->point; serial = state->serial; picked = state->picked;
                        hitId = state->hitId;
                        mutationId = state->mutationId; mutationProperty = state->mutationProperty;
                        mutationValue = state->mutationValue;
                    }
                    if (scan)
                    {
                        if (automation) automation->RemoveAllEventHandlers();
                        pending.clear(); nativePending.clear(); visited.clear(); elements.clear(); records.clear(); current = request;
                        scanning = true;
                        const auto roots = relatedRoots(current.root, current.excluded);
                        if (current.view == View::Native)
                        {
                            for (HWND root : roots) nativePending.emplace_back(root, root,
                                root == current.root ? QString() : nativeKey(current.root));
                        }
                        else if (automation && cache)
                        {
                            walker.Reset();
                            if (current.view == View::Controls) automation->get_ControlViewWalker(&walker);
                            else automation->get_RawViewWalker(&walker);
                            PROPERTYID changed[] = {UIA_BoundingRectanglePropertyId, UIA_IsOffscreenPropertyId,
                                UIA_IsEnabledPropertyId, UIA_NamePropertyId};
                            QString rootId;
                            for (HWND root : roots)
                            {
                                ComPtr<IUIAutomationElement> element;
                                const HRESULT result = automation->ElementFromHandleBuildCache(root, cache.Get(), &element);
                                if (SUCCEEDED(result) && element)
                                {
                                    if (root == current.root) rootId = runtimeKey(element.Get());
                                    pending.push_back({element, root == current.root ? QString() : rootId, root, true});
                                    automation->AddStructureChangedEventHandler(element.Get(), TreeScope_Subtree, nullptr, events.Get());
                                    automation->AddPropertyChangedEventHandlerNativeArray(element.Get(), TreeScope_Subtree,
                                        nullptr, events.Get(), changed, 4);
                                }
                                else publish({current.generation, 0, {}, {}, result, false, false, false});
                            }
                        }
                        else publish({current.generation, 0, {}, {}, FAILED(setup) ? setup : E_FAIL, false, false, false});
                    }
                    if (mutate)
                    {
                        Reply reply;
                        reply.generation = request.generation;
                        reply.serial = serial;
                        reply.mutation = true;
                        reply.mutationId = mutationId;
                        reply.mutationProperty = mutationProperty;
                        if (request.view == View::Native)
                        {
                            const auto found = records.constFind(mutationId);
                            if (found == records.cend()) reply.error = UIA_E_ELEMENTNOTAVAILABLE;
                            else
                            {
                                reply.error = mutateNative(found->window, mutationProperty, mutationValue);
                                if (SUCCEEDED(reply.error)) reply.hit = readNative(found->window, found->host, found->parent);
                            }
                        }
                        else
                        {
                            const auto found = elements.constFind(mutationId);
                            if (found == elements.cend() || !found->element) reply.error = UIA_E_ELEMENTNOTAVAILABLE;
                            else
                            {
                                const HWND fallbackWindow = records.value(mutationId).window;
                                ComPtr<IUIAutomationElement> cached;
                                reply.error = found->element->BuildUpdatedCache(cache.Get(), &cached);
                                if (SUCCEEDED(reply.error) && cached)
                                {
                                    reply.error = mutateElement(cached.Get(), mutationProperty, mutationValue,
                                        fallbackWindow);
                                    if (SUCCEEDED(reply.error))
                                    {
                                        ComPtr<IUIAutomationElement> updated;
                                        reply.error = cached->BuildUpdatedCache(cache.Get(), &updated);
                                        if (SUCCEEDED(reply.error) && updated)
                                        {
                                            reply.hit = readElement(updated.Get(), found->host, found->parent);
                                            readPatternValues(updated.Get(), reply.hit);
                                            if (mutationProperty == QStringLiteral("Value.Value") && fallbackWindow)
                                            {
                                                wchar_t text[4096]{};
                                                ::GetWindowTextW(fallbackWindow, text, 4096);
                                                reply.hit.name = QString::fromWCharArray(text);
                                                reply.hit.properties.insert(QStringLiteral("Value.Value"), reply.hit.name);
                                            }
                                            elements[mutationId].element = updated;
                                            records[mutationId] = reply.hit;
                                        }
                                    }
                                }
                            }
                        }
                        publish(std::move(reply));
                    }
                    if (hit)
                    {
                        Reply reply; reply.generation = request.generation; reply.serial = serial;
                        reply.hover = true; reply.picked = picked;
                        reply.fromTree = !hitId.isEmpty();
                        const DpiScope dpi;
                        HWND window = ::WindowFromPhysicalPoint(POINT{point.x(), point.y()});
                        if (reply.fromTree)
                        {
                            const auto found = records.constFind(hitId);
                            if (found == records.cend()) reply.error = UIA_E_ELEMENTNOTAVAILABLE;
                            else if (request.view == View::Native)
                            {
                                if (nativeKey(found->window) == hitId) reply.hit = readNative(found->window, found->host, found->parent);
                                else reply.error = UIA_E_ELEMENTNOTAVAILABLE;
                            }
                            else
                            {
                                const auto entry = elements.value(hitId);
                                ComPtr<IUIAutomationElement> cached;
                                reply.error = entry.element ? entry.element->BuildUpdatedCache(cache.Get(), &cached) : UIA_E_ELEMENTNOTAVAILABLE;
                                if (SUCCEEDED(reply.error) && cached)
                                { reply.hit = readElement(cached.Get(), entry.host, entry.parent); readPatternValues(cached.Get(), reply.hit); }
                            }
                        }
                        else if (belongs(window, request.root, request.excluded))
                        {
                            const HWND host = ::IsChild(request.root, window) || window == request.root
                                ? request.root : ::GetAncestor(window, GA_ROOT);
                            if (request.view == View::Native) reply.hit = readNative(window, host, {});
                            else if (automation && cache)
                            {
                                ComPtr<IUIAutomationElement> element, normalized, cached;
                                reply.error = automation->ElementFromPoint(POINT{point.x(), point.y()}, &element);
                                if (SUCCEEDED(reply.error) && element)
                                {
                                    ComPtr<IUIAutomationTreeWalker> hitWalker;
                                    if (request.view == View::Controls) automation->get_ControlViewWalker(&hitWalker);
                                    else automation->get_RawViewWalker(&hitWalker);
                                    if (hitWalker) hitWalker->NormalizeElement(element.Get(), &normalized);
                                    if (!normalized) normalized = element;
                                    reply.error = normalized->BuildUpdatedCache(cache.Get(), &cached);
                                    if (SUCCEEDED(reply.error) && cached)
                                    {
                                        reply.hit = readElement(cached.Get(), host, {});
                                        if (reply.hit.window && !belongs(reply.hit.window, request.root, request.excluded)) reply.hit = {};
                                        else readPatternValues(cached.Get(), reply.hit);
                                    }
                                }
                            }
                        }
                        publish(std::move(reply));
                    }
                    if (scanning && scanBudget)
                    {
                        Reply batch; batch.generation = current.generation;
                        for (int count = 0; count < 64; ++count)
                        {
                            { std::lock_guard<std::mutex> lock(state->mutex);
                                if (state->stopped || state->epoch != current.generation || state->hit) break; }
                            if (current.view == View::Native)
                            {
                                if (nativePending.empty()) break;
                                const auto entry = nativePending.front(); nativePending.pop_front();
                                const HWND window = std::get<0>(entry), host = std::get<1>(entry);
                                Node node = readNative(window, host, std::get<2>(entry));
                                if (!::IsWindow(window) || visited.contains(node.id)) continue;
                                visited.insert(node.id);
                                records.insert(node.id, node);
                                for (HWND child = ::GetWindow(window, GW_CHILD); child; child = ::GetWindow(child, GW_HWNDNEXT))
                                    nativePending.emplace_back(child, host, node.id);
                                batch.nodes.push_back(std::move(node));
                            }
                            else
                            {
                                if (pending.empty() || !walker) break;
                                Pending entry = pending.front(); pending.pop_front();
                                Node node = readElement(entry.element.Get(), entry.host, entry.parent);
                                if (node.id.isEmpty() || visited.contains(node.id)) continue;
                                visited.insert(node.id);
                                records.insert(node.id, node); elements.insert(node.id, entry);
                                ComPtr<IUIAutomationElement> child;
                                HRESULT result = walker->GetFirstChildElement(entry.element.Get(), &child);
                                if (child)
                                {
                                    ComPtr<IUIAutomationElement> cached;
                                    result = child->BuildUpdatedCache(cache.Get(), &cached);
                                    if (SUCCEEDED(result)) child = cached;
                                    else child.Reset();
                                }
                                if (FAILED(result)) batch.error = result;
                                // Children are queued one at a time, so hovering/cancellation
                                // can preempt traversal of a huge sibling list.
                                if (child) pending.push_back({child, node.id, entry.host});
                                if (!entry.root)
                                {
                                    ComPtr<IUIAutomationElement> sibling;
                                    result = walker->GetNextSiblingElement(entry.element.Get(), &sibling);
                                    if (sibling)
                                    {
                                        ComPtr<IUIAutomationElement> cached;
                                        result = sibling->BuildUpdatedCache(cache.Get(), &cached);
                                        if (SUCCEEDED(result)) sibling = cached;
                                        else sibling.Reset();
                                    }
                                    if (FAILED(result)) batch.error = result;
                                    if (sibling) pending.push_back({sibling, entry.parent, entry.host});
                                }
                                batch.nodes.push_back(std::move(node));
                            }
                        }
                        batch.done = pending.empty() && nativePending.empty();
                        scanning = !batch.done;
                        if (!batch.nodes.isEmpty() || batch.done || FAILED(batch.error)) publish(std::move(batch));
                    }
                }
                if (automation) automation->RemoveAllEventHandlers();
            }
            if (SUCCEEDED(initialized)) ::CoUninitialize();
            if (bound) ::SetThreadDesktop(previousDesktop);
        }).detach();
    }
    Collector::~Collector()
    { std::lock_guard<std::mutex> lock(m_state->mutex); m_state->stopped = true; m_state->wake.notify_one(); }
    void Collector::scan(const Request& request)
    {
        std::lock_guard<std::mutex> lock(m_state->mutex);
        m_state->request = request; m_state->epoch = request.generation;
        m_state->scan = true; m_state->hit = false; m_state->mutate = false; m_state->replies.clear(); m_state->wake.notify_one();
    }
    void Collector::hit(const Request& request, QPoint physical, quint64 serial, bool picked)
    {
        std::lock_guard<std::mutex> lock(m_state->mutex);
        // Do not overwrite a pending one-shot pick with an ordinary hover.
        if (m_state->hit && m_state->picked && !picked) return;
        m_state->request = request; m_state->point = physical; m_state->serial = serial;
        m_state->hitId.clear();
        m_state->picked = picked; m_state->hit = true; m_state->wake.notify_one();
    }
    void Collector::details(const Request& request, const QString& id, quint64 serial)
    {
        std::lock_guard<std::mutex> lock(m_state->mutex);
        if (m_state->hit && m_state->picked) return;
        m_state->request = request; m_state->serial = serial; m_state->hitId = id;
        m_state->picked = false; m_state->hit = true; m_state->wake.notify_one();
    }
    void Collector::mutate(const Request& request, const QString& id, const QString& property,
        const QString& value, quint64 serial)
    {
        std::lock_guard<std::mutex> lock(m_state->mutex);
        m_state->request = request; m_state->serial = serial;
        m_state->mutationId = id; m_state->mutationProperty = property; m_state->mutationValue = value;
        m_state->mutate = true; m_state->wake.notify_one();
    }
    void Collector::cancel()
    {
        std::lock_guard<std::mutex> lock(m_state->mutex);
        ++m_state->epoch; m_state->scan = false; m_state->hit = false; m_state->mutate = false; m_state->replies.clear(); m_state->wake.notify_one();
    }
    QVector<Reply> Collector::take(const qsizetype maximum)
    {
        std::lock_guard<std::mutex> lock(m_state->mutex);
        auto replies = m_state->replies.take(maximum);
        m_state->wake.notify_one();
        return replies;
    }
    QVector<Reply> Collector::takeControl(const qsizetype maximum)
    {
        std::lock_guard<std::mutex> lock(m_state->mutex);
        return m_state->replies.take(maximum, true);
    }
    bool Collector::hasReplies()
    { std::lock_guard<std::mutex> lock(m_state->mutex); return !m_state->replies.empty(); }
    bool Collector::takeDirty() { return m_state->dirty->exchange(false); }
}
