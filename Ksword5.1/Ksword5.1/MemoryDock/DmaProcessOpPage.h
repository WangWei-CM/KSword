#pragma once

// ============================================================
// DmaProcessOpPage.h
// 作用：
// - 通过 DDMA 往目标进程的物理页里写字节：注入载荷，或写 UD2 让目标崩掉。
//
// 与 R-1 那条注入（IOCTL_KSWORD_ARK_HVM_INJECT）的关系：**不是换个后端。**
//
// R-1 注入把载荷放在影子页里，装一个执行视图（执行走影子、读写看真页），并武装
// 一个触发点。真页从头到尾没被改过，任何扫描器读到的都是原样。
//
// DMA 一样都没有。它只有"往物理页写字节"这一件事，于是：
//   * **真页真的被改了**，任何读取路径都看得见，包括本工具自己的 R3/R0/HVM；
//   * **没有触发点**，载荷躺在那里等目标自己执行到，写进一个永远不会被执行的
//     位置等于什么都没做；
//   * **还原是我们的责任**，所以本页强制先备份、并把备份一直留在界面上。
//
// 关于 UD2 那条"结束"：DMA 调不了 PsTerminateProcess，往目标会执行到的代码里写
// UD2 让它因未处理异常退出，是不依赖内核结构偏移的唯一办法。它**不是一个可靠的
// terminate**：目标可能带异常处理器把 #UD 吞掉、生效与否取决于那段代码会不会被
// 执行到、会留下崩溃转储。界面必须把这些说出来，而不是给一个叫"结束进程"的按钮。
//
// 判据与计划全部在 shared/evidence/DmaProcessOpPlan.h，Qt-free，离线套件覆盖。
// 本页只负责采集页内容、发起写入、把读回校验的结果如实展示。
// ============================================================

#include "MemoryAccessBackend.h"

#include "../../../shared/evidence/DmaProcessOpPlan.h"

#include <QString>
#include <QWidget>

#include <cstdint>
#include <vector>
#include <memory>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;

namespace ksword::memory_dock
{
    struct DmaOperationLedger;

    // DmaOpRecord：一次可能已改变物理页的写入，失败/partial 也必须保留还原依据。
    struct DmaOpRecord
    {
        std::uint64_t recordId = 0;
        std::uint32_t processId = 0;
        std::uint64_t creationTime100ns = 0;
        ksword::memory_backend::DdmaSessionSnapshot session;
        // The actual patch VA, including the cave offset, is frozen for restore.
        std::uint64_t virtualAddress = 0;
        std::uint64_t physicalAddress = 0;
        std::size_t offsetInPage = 0;
        std::vector<std::uint8_t> writtenBytes;
        std::vector<std::uint8_t> originalBytes;
        QString description;
        bool verified = false;
    };

    // 不带 Q_OBJECT：与同目录其它页一致，全部用 lambda + connect(this, ...)。
    class DmaProcessOpPage final : public QWidget
    {
    public:
        explicit DmaProcessOpPage(QWidget* parent = nullptr);
        ~DmaProcessOpPage() override;

        void setAttachedProcess(std::uint32_t processId, const QString& processName,
            std::uint64_t expectedCreationTime100ns = 0);
        void refreshChannelAvailability();

    private:
        void buildUi();
        void wireSignals();
        void updateActionState();

        // GUI 只冻结参数；进程级 FIFO 持有完整的备份/写入/最终回读事务。
        // 接受后不因 QWidget 关闭而取消，备份归进程级账本所有。
        void performWrite(bool injectPayload);
        void restoreLastWrite();

        void synchronizeLedger();
        void followPendingOperation();

        std::uint32_t m_attachedPid = 0;
        std::uint64_t m_expectedCreationTime100ns = 0;
        std::uint64_t m_targetGeneration = 0;
        std::uint64_t m_watchedOperation = 0;
        qsizetype m_projectedLogLines = 0;
        QString m_attachedProcessName;
        // Process-owned ledger outlives this QWidget and every queued callback.
        std::shared_ptr<DmaOperationLedger> m_ledger;

        QLabel* m_targetLabel = nullptr;
        QLineEdit* m_addressEdit = nullptr;
        QLineEdit* m_payloadEdit = nullptr;
        QCheckBox* m_forceCheck = nullptr;
        QCheckBox* m_acknowledgeCheck = nullptr;
        // 仅当共享性无法确认时才需要勾；已确认共享的页**没有任何开关能解锁**。
        QCheckBox* m_unknownSharingCheck = nullptr;
        QPushButton* m_injectButton = nullptr;
        QPushButton* m_ud2Button = nullptr;
        QPushButton* m_restoreButton = nullptr;
        QLabel* m_channelHintLabel = nullptr;
        QLabel* m_statusLabel = nullptr;
        QPlainTextEdit* m_logText = nullptr;
    };
}
