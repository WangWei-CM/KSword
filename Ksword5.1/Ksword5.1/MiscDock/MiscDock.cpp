#include "MiscDock.h"

#include "BootEditor/BootEditorTab.h"

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

    // 主 Tab 目前先放“引导”页，后续可继续扩展更多杂项功能。
    m_mainTabWidget = new QTabWidget(this);
    m_mainTabWidget->setObjectName(QStringLiteral("ksMiscDockMainTab"));
    m_rootLayout->addWidget(m_mainTabWidget, 1);

    m_bootEditorTab = new BootEditorTab(m_mainTabWidget);
    m_mainTabWidget->addTab(m_bootEditorTab, QStringLiteral("引导"));
}

