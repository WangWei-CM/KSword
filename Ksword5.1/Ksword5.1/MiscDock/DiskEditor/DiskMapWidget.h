#pragma once

// ============================================================
// DiskMapWidget.h
// 作用：
// 1) 提供类似 DiskGenius 的横向柱形磁盘视图；
// 2) 按真实偏移与容量比例绘制分区、未分配区域和刻度；
// 3) 通过信号把用户点击的分区回传给上层磁盘编辑页。
// ============================================================

#include "DiskEditorModels.h"

#include <QPoint>
#include <QRect>
#include <QWidget>

#include <cstdint>
#include <vector>

class QEvent;
class QMouseEvent;
class QPaintEvent;

namespace ks::misc
{
    // DiskMapWidget 说明：
    // - 输入：DiskDeviceInfo 快照；
    // - 处理逻辑：按容量比例构建横向分区矩形并绘制标签；
    // - 返回行为：无直接返回值，点击分区时发出 partitionActivated。
    class DiskMapWidget final : public QWidget
    {
        Q_OBJECT

    public:
        // 构造函数：
        // - parent 为 Qt 父控件；
        // - 初始化鼠标跟踪和最小高度。
        explicit DiskMapWidget(QWidget* parent = nullptr);

        // setDisk：
        // - 设置当前展示的磁盘快照；
        // - diskInfo 为磁盘与分区信息；
        // - 无返回值，内部触发重绘。
        void setDisk(const DiskDeviceInfo& diskInfo);

        // clearDisk：
        // - 清空当前展示；
        // - 无输入参数，无返回值。
        void clearDisk();

        // setSelectedPartitionIndex：
        // - 外部同步表格当前分区；
        // - partitionIndex 为 DiskPartitionInfo::tableIndex；
        // - 无返回值，内部触发重绘。
        void setSelectedPartitionIndex(int partitionIndex);

    signals:
        // partitionActivated：
        // - 当用户点击柱形图上的某个分区或未分配块时触发；
        // - partitionIndex 对应 DiskPartitionInfo::tableIndex；
        // - 未命中有效区域时不会发出。
        void partitionActivated(int partitionIndex);

    protected:
        // paintEvent：
        // - Qt 绘制入口；
        // - event 为绘制事件对象；
        // - 无返回值。
        void paintEvent(QPaintEvent* event) override;

        // mousePressEvent：
        // - 处理左键点击命中测试；
        // - event 为鼠标事件对象；
        // - 无返回值。
        void mousePressEvent(QMouseEvent* event) override;

        // mouseMoveEvent：
        // - 更新悬停分区索引和 tooltip；
        // - event 为鼠标事件对象；
        // - 无返回值。
        void mouseMoveEvent(QMouseEvent* event) override;

        // leaveEvent：
        // - 鼠标离开时清空悬停状态；
        // - event 为离开事件对象；
        // - 无返回值。
        void leaveEvent(QEvent* event) override;

    private:
        // PaintSegment 作用：
        // - 保存一次布局得到的可点击绘制块；
        // - rect 使用 widget 坐标系。
        struct PaintSegment
        {
            QRect rect;                  // rect：绘制和命中测试矩形。
            DiskPartitionInfo partition; // partition：该绘制块对应的分区信息。
        };

    private:
        // rebuildPaintSegments：
        // - 根据当前 widget 尺寸和磁盘信息重建矩形布局；
        // - 无输入参数；
        // - 返回绘制块数组。
        std::vector<PaintSegment> rebuildPaintSegments() const;

        // hitTest：
        // - 根据鼠标位置查找分区；
        // - point 为 widget 坐标；
        // - 返回 tableIndex，未命中返回 -1。
        int hitTest(const QPoint& point) const;

        // segmentTooltipText：
        // - 构造某个分区块 tooltip；
        // - segment 为绘制块；
        // - 返回可展示文本。
        static QString segmentTooltipText(const PaintSegment& segment);

    private:
        DiskDeviceInfo m_diskInfo;       // m_diskInfo：当前磁盘快照。
        bool m_hasDisk = false;          // m_hasDisk：是否已经绑定有效磁盘。
        int m_selectedPartitionIndex = -1; // m_selectedPartitionIndex：当前选中分区。
        int m_hoverPartitionIndex = -1;    // m_hoverPartitionIndex：当前悬停分区。
    };
}
