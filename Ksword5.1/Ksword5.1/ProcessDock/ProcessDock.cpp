#include "ProcessDock.h"
#include <QVBoxLayout>
ProcessDock::ProcessDock(QWidget* parent) : QWidget(parent) {
	QVBoxLayout* mainLayout = new QVBoxLayout(this);
	mainLayout->setSpacing(0);
	mainLayout->addWidget(createBasicPlaceholder("功能预留区"));
	mainLayout->addStretch(1); // 确保占位符随 ProcessDock 高度拉伸

	// 2. 给 ProcessDock 设置最小高度（避免 Dock 高度被压缩）
	this->setMinimumHeight(200);
	mainLayout->setContentsMargins(0, 0, 0, 0);






}
