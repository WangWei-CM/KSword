#include "UI_All.h"
#include <QWidget>
#include <QLabel>
#include <QVBoxLayout>
QWidget* createBasicPlaceholder(const QString& tipText/* = "占位区域"*/) {
    // 占位符主容器
    QWidget* placeholder = new QWidget();
    // 设置大小策略为可扩展
    placeholder->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    // 设置最小尺寸确保可见
    //placeholder->setMinimumSize(100, 100);
    placeholder->setStyleSheet(
        "border: 2px solid #0066CC; " // 蓝色线框
        "background-color: transparent; " // 透明背景
        "border-radius: 0px;" // 强制方角
    );

    // 蓝色字体提示
    QLabel* tipLabel = new QLabel(tipText, placeholder);
    tipLabel->setStyleSheet("color: #0066CC; font-size: 14px;"); // 蓝色字体
    tipLabel->setAlignment(Qt::AlignCenter); // 文字居中

    // 布局（让文字在占位符正中间）
    QVBoxLayout* layout = new QVBoxLayout(placeholder);
    layout->addWidget(tipLabel);
    layout->setContentsMargins(0, 0, 0, 0); // 去掉布局边距

    return placeholder;
}
