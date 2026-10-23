#include "WindowPickerButton.h"

#include "../theme.h"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPen>
#include <QWidget>

#include <windows.h>
#include <psapi.h>

// ============================================================
// WindowPickerButton.cpp
// 作用：
// - 实现十字准星拾取：按下抓鼠标，移动时解析光标下窗口所属进程并高亮，
//   松手提交，Esc 取消。
// ============================================================

namespace ks::ui
{
    // WindowPickerHighlight：
    // - 一个无边框置顶小部件，只画一圈边框，用来标出当前会拾到哪个窗口；
    // - 位置用 Win32 的物理像素直接下发。这样做是为了绕开逻辑像素与物理像素的
    //   换算：GetWindowRect 给的是物理坐标，而 Qt 的 setGeometry 吃逻辑坐标，
    //   多屏不同缩放时两者的原点和比例都不一样，自己换算很容易在副屏上错位。
    //   交给 SetWindowPos 之后 Qt 会跟着 WM_SIZE 调整自身几何，绘制照常。
    class WindowPickerHighlight : public QWidget
    {
    public:
        WindowPickerHighlight()
            : QWidget(nullptr,
                Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint
                | Qt::WindowTransparentForInput | Qt::NoDropShadowWindowHint)
        {
            setAttribute(Qt::WA_TranslucentBackground, true);
            setAttribute(Qt::WA_ShowWithoutActivating, true);
            setAttribute(Qt::WA_TransparentForMouseEvents, true);
            // 先实例化原生窗口，后面才能拿到 HWND 直接摆位置。
            (void)winId();
        }

        // nativeHandle：拾取时要把高亮框自己从命中测试结果里排除掉。
        HWND nativeHandle() const
        {
            return reinterpret_cast<HWND>(winId());
        }

        void showOverPhysicalRect(HWND target, const RECT& physicalRect)
        {
            const int width = physicalRect.right - physicalRect.left;
            const int height = physicalRect.bottom - physicalRect.top;
            if (width <= 0 || height <= 0)
            {
                hide();
                return;
            }
            const HWND overlay = nativeHandle();
            if (m_target == target && ::EqualRect(&m_physicalRect, &physicalRect)
                && ::IsWindowVisible(overlay))
            {
                return; // 同一物理几何只维持命中查询，不重发位置和整框更新。
            }
            if (::SetWindowPos(
                overlay,
                HWND_TOPMOST,
                physicalRect.left,
                physicalRect.top,
                width,
                height,
                SWP_NOACTIVATE | SWP_SHOWWINDOW) != FALSE)
            {
                m_target = target;
                m_physicalRect = physicalRect; // 失败不提交缓存，下次移动仍允许重试。
                update();
            }
        }

    protected:
        void paintEvent(QPaintEvent*) override
        {
            QPainter painter(this);
            painter.setRenderHint(QPainter::Antialiasing, false);
            // 三像素实线边框：既能在浅色和深色窗口上都看得见，又不遮住内容。
            QPen pen(KswordTheme::PrimaryAccentColor());
            pen.setWidth(3);
            painter.setPen(pen);
            painter.setBrush(Qt::NoBrush);
            painter.drawRect(rect().adjusted(1, 1, -2, -2));
        }
    private:
        HWND m_target = nullptr;
        RECT m_physicalRect{};
    };

    namespace
    {
        // processNameOf：取进程映像文件名。拿不到就返回空串，由调用方决定怎么说，
        // 而不是在这里编一个占位名字。
        QString processNameOf(const DWORD processId)
        {
            if (processId == 0)
            {
                return QString();
            }
            const HANDLE processHandle = ::OpenProcess(
                PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
            if (processHandle == nullptr)
            {
                return QString();
            }
            wchar_t imagePath[MAX_PATH] = {};
            DWORD pathLength = MAX_PATH;
            QString name;
            if (::QueryFullProcessImageNameW(processHandle, 0, imagePath, &pathLength) != FALSE)
            {
                const QString fullPath = QString::fromWCharArray(imagePath, static_cast<int>(pathLength));
                const int separatorIndex = fullPath.lastIndexOf(QLatin1Char('\\'));
                name = (separatorIndex >= 0) ? fullPath.mid(separatorIndex + 1) : fullPath;
            }
            ::CloseHandle(processHandle);
            return name;
        }
    }

    WindowPickerButton::WindowPickerButton(QWidget* const parent)
        : QToolButton(parent)
    {
        setCheckable(false);
        setCursor(Qt::CrossCursor);
        setFocusPolicy(Qt::StrongFocus);
    }

    WindowPickerButton::~WindowPickerButton()
    {
        delete m_highlight;
        m_highlight = nullptr;
    }

    void WindowPickerButton::mousePressEvent(QMouseEvent* const event)
    {
        if (event->button() != Qt::LeftButton)
        {
            QToolButton::mousePressEvent(event);
            return;
        }
        beginPicking();
        event->accept();
    }

    void WindowPickerButton::mouseMoveEvent(QMouseEvent* const event)
    {
        if (!m_picking)
        {
            QToolButton::mouseMoveEvent(event);
            return;
        }
        refreshTargetUnderCursor();
        event->accept();
    }

    void WindowPickerButton::mouseReleaseEvent(QMouseEvent* const event)
    {
        if (!m_picking)
        {
            QToolButton::mouseReleaseEvent(event);
            return;
        }
        refreshTargetUnderCursor();
        finishPicking(true);
        event->accept();
    }

    void WindowPickerButton::keyPressEvent(QKeyEvent* const event)
    {
        if (m_picking && event->key() == Qt::Key_Escape)
        {
            finishPicking(false);
            event->accept();
            return;
        }
        QToolButton::keyPressEvent(event);
    }

    void WindowPickerButton::beginPicking()
    {
        if (m_picking)
        {
            return;
        }
        m_picking = true;
        m_hoverProcessId = 0;
        m_hoverProcessName.clear();

        if (m_highlight == nullptr)
        {
            m_highlight = new WindowPickerHighlight();
        }
        // 键盘也要抓：Esc 取消依赖本控件拿到按键事件。
        grabMouse(QCursor(Qt::CrossCursor));
        grabKeyboard();
        emit pickingChanged(true);
        refreshTargetUnderCursor();
    }

    void WindowPickerButton::finishPicking(const bool commit)
    {
        if (!m_picking)
        {
            return;
        }
        m_picking = false;
        releaseMouse();
        releaseKeyboard();
        if (m_highlight != nullptr)
        {
            m_highlight->hide();
        }
        emit pickingChanged(false);

        if (commit)
        {
            emit processPicked(m_hoverProcessId, m_hoverProcessName);
        }
        m_hoverProcessId = 0;
        m_hoverProcessName.clear();
    }

    void WindowPickerButton::refreshTargetUnderCursor()
    {
        // 用 Win32 取光标位置而不是 QCursor::pos()：WindowFromPoint 吃的是物理
        // 像素，而 Qt 给的是逻辑像素，多屏不同缩放时两者不等，混用会在副屏上
        // 指到另一个窗口。两边都用 Win32 就不存在换算。
        POINT cursorPoint{};
        if (::GetCursorPos(&cursorPoint) == FALSE)
        {
            return;
        }

        HWND targetWindow = ::WindowFromPoint(cursorPoint);
        // 高亮框虽然带 WS_EX_TRANSPARENT，这里仍显式跳一次：这条判据一旦失效
        // 的表现是"拖到哪儿都拾到同一个进程"，而那时已经很难看出是高亮框在挡路。
        if (m_highlight != nullptr && targetWindow == m_highlight->nativeHandle())
        {
            targetWindow = nullptr;
        }
        if (targetWindow != nullptr)
        {
            // 命中的往往是子控件，要的是它所属的顶层窗口。
            HWND rootWindow = ::GetAncestor(targetWindow, GA_ROOT);
            if (rootWindow != nullptr)
            {
                targetWindow = rootWindow;
            }
        }

        DWORD targetProcessId = 0;
        if (targetWindow != nullptr)
        {
            ::GetWindowThreadProcessId(targetWindow, &targetProcessId);
        }
        // 跳过自己：鼠标经过主界面时不该把本程序报成目标。
        if (targetProcessId == ::GetCurrentProcessId())
        {
            targetProcessId = 0;
            targetWindow = nullptr;
        }

        const quint32 newProcessId = static_cast<quint32>(targetProcessId);
        if (targetWindow == nullptr || newProcessId == 0)
        {
            if (m_highlight != nullptr)
            {
                m_highlight->hide();
            }
            if (m_hoverProcessId != 0)
            {
                m_hoverProcessId = 0;
                m_hoverProcessName.clear();
                emit hoverPreview(0, QString());
            }
            return;
        }

        RECT windowRect{};
        if (::GetWindowRect(targetWindow, &windowRect) != FALSE && m_highlight != nullptr)
        {
            m_highlight->showOverPhysicalRect(targetWindow, windowRect);
        }

        if (newProcessId != m_hoverProcessId)
        {
            m_hoverProcessId = newProcessId;
            m_hoverProcessName = processNameOf(targetProcessId);
            emit hoverPreview(m_hoverProcessId, m_hoverProcessName);
        }
    }
}
