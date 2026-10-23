#include "HardwareDock.h"
#include "../../../shared/ui/KsPainterChart.h"
#include "../UI/PerformanceChartTheme.h"
#include "../UI/PerformanceNavCard.h"
#include <QTimer>

void HardwareDock::scheduleUtilizationThemeRefresh()
{
    if (m_utilizationThemeRefreshScheduled)
    {
        return;
    }
    // 同一轮 palette 传播可命中多个控件，只在事件循环返回后刷新一次。
    m_utilizationThemeRefreshScheduled = true;
    QTimer::singleShot(0, this, [this]()
    {
        m_utilizationThemeRefreshScheduled = false;
        refreshUtilizationThemeColors();
    });
}

void HardwareDock::refreshUtilizationThemeColors()
{
    ++m_utilizationMirrorThemeRevision;
    using Role = KswordTheme::PerformanceRole;
    // 所有性能卡按设备身份重算边框与单双曲线角色，不清空采样历史或改变选中状态。
    for (const UtilizationNavEntry& entry : m_utilizationNavEntries)
    {
        if (entry.navCard == nullptr)
        {
            continue;
        }
        Role role = Role::Cpu;
        switch (entry.kind)
        {
        case UtilizationDeviceKind::Cpu: role = Role::Cpu; break;
        case UtilizationDeviceKind::Memory: role = Role::Memory; break;
        case UtilizationDeviceKind::Disk: role = Role::Disk; break;
        case UtilizationDeviceKind::Network:
        case UtilizationDeviceKind::VirtualNetwork: role = Role::Network; break;
        case UtilizationDeviceKind::Gpu: role = Role::Gpu; break;
        }
        entry.navCard->setAccentColor(KswordTheme::PerformanceColor(role));
        if (entry.kind == UtilizationDeviceKind::Memory)
        {
            entry.navCard->setSeriesColors(KswordTheme::PerformanceColor(Role::Memory),
                KswordTheme::PerformanceColor(Role::SharedMemory));
        }
        else if (entry.kind == UtilizationDeviceKind::Disk
            || entry.kind == UtilizationDeviceKind::Network
            || entry.kind == UtilizationDeviceKind::VirtualNetwork)
        {
            entry.navCard->setSeriesColors(KswordTheme::PerformanceColor(Role::Read),
                KswordTheme::PerformanceColor(Role::Write));
        }
    }

    refreshUtilizationChartThemeColors();

    // 保存快照不在 allWidgets 内，主主题切换时同步迁移，返回主界面不会恢复旧主题色。
    const ks::ui::ThemeColorSnapshot currentTheme = ks::ui::CaptureThemeColorSnapshot();
    m_utilizationBorrowedPalette = ks::ui::RemapStaleThemeColorsInPalette(
        m_utilizationBorrowedThemeColors, m_utilizationBorrowedPalette);
    m_utilizationBorrowedThemeColors = currentTheme;
    for (FloatingWidgetStyleState& state : m_utilizationFloatingWidgetStyles)
    {
        state.styleSheet = ks::ui::RemapStaleThemeColorsInText(state.themeColors, state.styleSheet);
        state.palette = ks::ui::RemapStaleThemeColorsInPalette(state.themeColors, state.palette);
        state.themeColors = currentTheme;
    }
    if (m_utilizationFloatingMode != UtilizationFloatingMode::None)
    {
        // 独立 dark/light 继续使用浮窗自己的表面；follow_main 在 Sidebar/Detail 都及时刷新。
        applyUtilizationFloatingTheme();
        applyUtilizationFloatingContentScale(true);
    }
}

void HardwareDock::refreshUtilizationChartThemeColors(const QPalette* const floatingPalette)
{
    using Role = KswordTheme::PerformanceRole;
    QWidget* const floatingPage = m_utilizationFloatingPage.data();
    const auto refreshChart = [floatingPalette, floatingPage](QChartView* const view,
        const Role primaryRole, const Role secondaryRole) {
        if (view == nullptr)
        {
            return;
        }
        // 指定浮窗颜色时只改被借走的页；主页面的图表仍使用全局颜色。
        if (floatingPalette != nullptr
            && (floatingPage == nullptr || (view != floatingPage && !floatingPage->isAncestorOf(view))))
        {
            return;
        }
        ks::ui::RefreshPerformanceChartTheme(view->chart(), primaryRole, secondaryRole, floatingPalette);
    };
    // engineRole 只读既有稳定引擎 key，避免多种角色碰色时靠旧 RGB 猜测。
    const auto engineRole = [](const QString& key) {
        if (key == QStringLiteral("copy")) return Role::Copy;
        if (key == QStringLiteral("video_encode")) return Role::VideoEncode;
        if (key == QStringLiteral("video_decode")) return Role::VideoDecode;
        return Role::Gpu;
    };

    // 固定页与后发现的设备页都复用同一图表刷新，保留坐标范围、透明度和现有点位。
    for (const CoreChartEntry& entry : m_coreChartEntries)
    {
        refreshChart(entry.chartView, Role::Cpu, Role::Cpu);
    }
    refreshChart(m_diskUtilChartView, Role::Read, Role::Write);
    refreshChart(m_networkUtilChartView, Role::Read, Role::Write);
    for (const int deviceIndex : m_presentedDiskDevices)
    {
        const auto& device = m_diskUtilDevices[static_cast<std::size_t>(deviceIndex)];
        refreshChart(device.chartView, Role::Read, Role::Write);
    }
    for (const int deviceIndex : m_presentedNetworkDevices)
    {
        const auto& device = m_networkUtilDevices[static_cast<std::size_t>(deviceIndex)];
        refreshChart(device.chartView, Role::Read, Role::Write);
    }
    for (const GpuEngineChartEntry& entry : m_gpuEngineCharts)
    {
        const Role role = engineRole(entry.engineKeyText);
        refreshChart(entry.chartView, role, role);
    }
    refreshChart(m_gpuDedicatedMemoryChartView, Role::DedicatedMemory, Role::DedicatedMemory);
    refreshChart(m_gpuSharedMemoryChartView, Role::SharedMemory, Role::SharedMemory);
    for (const int deviceIndex : m_presentedGpuDevices)
    {
        const auto& device = m_gpuUtilDevices[static_cast<std::size_t>(deviceIndex)];
        for (const GpuEngineChartEntry& entry : device.engineCharts)
        {
            const Role role = engineRole(entry.engineKeyText);
            refreshChart(entry.chartView, role, role);
        }
        refreshChart(device.dedicatedMemoryChartView, Role::DedicatedMemory, Role::DedicatedMemory);
        refreshChart(device.sharedMemoryChartView, Role::SharedMemory, Role::SharedMemory);
    }

}

// 新控件只准备自身；数据采样tick不再刷新全域图表主题。
void HardwareDock::prepareNewUtilizationChartTheme(QChartView* const view,
    const KswordTheme::PerformanceRole primaryRole, const KswordTheme::PerformanceRole secondaryRole)
{
    if (view != nullptr) ++m_utilizationMirrorStructureRevision;
    QWidget* const page = m_utilizationFloatingPage.data();
    QWidget* const window = m_utilizationFloatingWindow.data();
    if (view == nullptr || page == nullptr || window == nullptr
        || (view != page && !page->isAncestorOf(view)))
    {
        return;
    }
    const QPalette palette = window->palette();
    const QPalette* const localPalette = m_utilizationFloatingThemeMode == QStringLiteral("follow_main")
        ? nullptr : &palette;
    ks::ui::RefreshPerformanceChartTheme(view->chart(), primaryRole, secondaryRole, localPalette);
}

void HardwareDock::prepareNewUtilizationEngineChartTheme(QChartView* const view, const QString& engineKey)
{
    using Role = KswordTheme::PerformanceRole;
    const auto role = engineKey == QStringLiteral("copy") ? Role::Copy
        : engineKey == QStringLiteral("video_encode") ? Role::VideoEncode
        : engineKey == QStringLiteral("video_decode") ? Role::VideoDecode : Role::Gpu;
    prepareNewUtilizationChartTheme(view, role, role);
}
