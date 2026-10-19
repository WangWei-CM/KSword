#include "./UiAnimationGate.h"

#include <QApplication>
#include <QCoreApplication>
#include <QVariant>

namespace
{
    // 属性名集中在此定义，避免调用方各自硬编码字符串。
    const char* const kUiAnimationsEnabledProperty = "ksword_ui_animations_enabled";
}

void ks::ui::SetGlobalUiAnimationsEnabled(const bool enabled)
{
    if (QApplication* appInstance = qobject_cast<QApplication*>(QCoreApplication::instance()))
    {
        appInstance->setProperty(kUiAnimationsEnabledProperty, enabled);
    }
}

bool ks::ui::AreUiAnimationsEnabled()
{
    QApplication* appInstance = qobject_cast<QApplication*>(QCoreApplication::instance());
    if (appInstance == nullptr)
    {
        return false; // 没有图形应用时不存在界面动画。
    }
    const QVariant value = appInstance->property(kUiAnimationsEnabledProperty);
    // 设置尚未下发前默认保留动画，避免启动阶段被误判为关闭。
    return value.isValid() ? value.toBool() : true;
}
