#include "MiscDock.h"

#include "BootEditor/BootEditorTab.h"
#include "DiskEditor/DiskEditorTab.h"

#include <QIcon>
#include <QTabWidget>
#include <QVBoxLayout>

MiscDock::MiscDock(QWidget* parent)
    : QWidget(parent)
{
    initializeUi();

    kLogEvent initEvent;
    info << initEvent << "[MiscDock] 杂项页面初始化完成。" << eol;
}

void MiscDock::initializeUi()
{
    // 根布局负责承载整个“杂项”容器。
    m_rootLayout = new QVBoxLayout(this);
    m_rootLayout->setContentsMargins(0, 0, 0, 0);
    m_rootLayout->setSpacing(0);

    // 主 Tab 承载所有杂项工具，保持 Dock 外层只暴露一个统一入口。
    m_mainTabWidget = new QTabWidget(this);
    m_mainTabWidget->setObjectName(QStringLiteral("ksMiscDockMainTab"));
    m_rootLayout->addWidget(m_mainTabWidget, 1);

    m_bootEditorTab = new BootEditorTab(m_mainTabWidget);
    m_mainTabWidget->addTab(m_bootEditorTab, QStringLiteral("引导"));

    // 磁盘编辑页：
    // - 参考 DiskGenius 类工具布局，提供横向柱形分区图；
    // - 默认只读，用户显式解锁后才允许写回物理磁盘。
    m_diskEditorTab = new ks::misc::DiskEditorTab(m_mainTabWidget);
    m_mainTabWidget->addTab(
        m_diskEditorTab,
        QIcon(QStringLiteral(":/Icon/disk_storage.svg")),
        QStringLiteral("磁盘编辑"));
}
