#pragma once

class QAbstractItemView;

namespace ks::ui
{
    // 为普通表格/树补齐 header 与 viewport 无法被 QSS 裁剪的四个圆角。
    // 只绘制 8px 极角，保留视图对象、鼠标命中、滚动条和模型/委托；false 恢复原外壳。
    void SetRoundedViewSurfaceEnabled(QAbstractItemView* view, bool enabled);
}
