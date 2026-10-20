#pragma once

// HvmWatchDialog：内存监视的安装对话框，以及给其它页面用的统一入口。
//
// 存在的理由是 issue #195 第十四节那句话：功能不能永远只停留在 HVM 实验页。
// SSDT 页、DriverObject 页、内存页、反汇编页的作者不应该、也没理由去理解
// GPA、EPT 叶、ruleId 这些东西——他们手里有的是"一个地址和一句人话描述"，
// 而那正好就是 openHvmWatch 要的全部输入。
//
// 这一层替调用方承担三件他们不该关心的事：
// - 地址翻译与页对齐（EPT 是页粒度，用户选的 8 字节要与整页并排显示）；
// - 前置条件（写权限门、常驻必须停着、目标页不能已被别的 EPT 机制占着）；
// - 结果解释（冲突时指名占用者，而不是丢一个协议状态码）。

#include <QDialog>
#include <QString>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;

namespace ksword::ark { struct VirtualAddressTranslateResult; }

namespace ksword::hvm
{
    struct HvmWatchTarget;
}

namespace ks::ui
{
    // HvmWatchRequest：一次来自其它页面的监视请求。
    struct HvmWatchRequest
    {
        // 地址是内核虚拟地址（真）还是物理地址（假）。
        bool virtualAddress = true;
        quint64 address = 0;
        // 调用方真正关心的字节数。0 表示整页。
        //
        // 它不改变硬件监视的范围，只决定命中后能不能判断这次访问落在调用方
        // 关心的那几个字节上。SSDT 一项是 4 或 8 字节，MajorFunction 一项是
        // 8 字节——填准了，"同一页的其它偏移"与"你的目标"才分得开。
        quint64 length = 0;
        // KSWORD_ARK_HVM_EPT_ACCESS_* 的组合，作为对话框里的初始勾选。
        unsigned long access = 0;
        // 目标的人话描述，直接显示给用户，例如
        // "\Driver\Foo MajorFunction[IRP_MJ_DEVICE_CONTROL]"。
        QString label;
    };

    // openHvmWatchOnPte：监视某个内核地址的**页表项**，而不是它指向的内容。
    //
    // 回答的是另一个问题：openHvmWatch 问"谁动了这段内存"，这个问"谁改了这段
    // 内存的映射"。后者是 remap / PTE 改写这类手法唯一能被抓住的地方——目标页
    // 一个字节没动，改的是指向它的那一项。
    //
    // 这一层替调用方承担四件事，都是各自去做就会做错的：
    //
    // - **翻译**：把内核 VA 送进驱动走一遍页表，取回 PTE 自己的**物理**地址。
    //   调用方拿不到也算不出这个地址。
    // - **门禁**：判据是 fieldFlags 的 PTE_PRESENT 位，**不是** resolved。
    //   一个 P=0 的 PTE 会让 resolved 为假，而它的地址早算出来了——"盯着一个
    //   当前不存在的 PTE 等它被填上"恰恰是最该做的那条监视。
    // - **大页**：落在 2 MiB 或 1 GiB 映射上时根本没有 PTE 这一级，直接说清楚
    //   而不是退回去监视 PDE（那是另一个目标，粒度差 512 倍）。
    // - **措辞**：这条监视按物理地址建立，于是详情里"落在请求范围内"与"映射
    //   核对"两行都会退化成无法判断（前者拿线性地址比物理地址，后者只对虚拟
    //   监视跑）。label 里因此带上原始 VA 与页内偏移，让人还能自己对上。
    //
    // 参数：
    //   parent        —— 弹窗归属窗口。
    //   kernelAddress —— 关心的内核虚拟地址；取它所在页的 PTE。
    //   label         —— 目标的人话描述，会被加上「的页表项」后缀。
    // 返回：无。失败（翻译不出、大页、这一级没走到）时弹一次说明，不装监视。
    void openHvmWatchOnPte(
        QWidget* parent,
        quint64 kernelAddress,
        const QString& label);

    // Present a worker-prepared page walk. This function performs no driver query.
    void openHvmWatchOnPtePrepared(QWidget* parent, quint64 kernelAddress,
        const QString& label, const ksword::ark::VirtualAddressTranslateResult& walk);

    // openHvmWatch：打开安装对话框并预填目标；用户确认后安装。
    //
    // 非阻塞语义上与其它 HVM 面板一致：对话框是模态的（用户正在对一个具体
    // 目标下手，此刻不该同时去点别处），安装本身放到后台线程，因为它是阻塞
    // IOCTL。安装结果以一次消息框回报。
    void openHvmWatch(QWidget* parent, const HvmWatchRequest& request);
}

// HvmWatchAddDialog：安装表单。
//
// 内存监视页与上面那个统一入口共用同一份表单，而不是各写一个：两份表单会在
// "页粒度"和"请求访问与实际访问"这两段说明上慢慢漂开，而那两段恰恰是这个
// 功能最容易被误解的地方。
class HvmWatchAddDialog final : public QDialog
{
    Q_OBJECT

public:
    explicit HvmWatchAddDialog(QWidget* parent = nullptr);
    void accept() override;

    // prefill：由统一入口预填目标。label 非空时在顶部显示"正在监视：<label>"，
    // 并把地址栏设为只读——调用方已经算好了地址，让用户在这里改它只会让
    // 屏幕上的描述与实际监视的东西对不上。
    void prefill(const ks::ui::HvmWatchRequest& request);

    ksword::hvm::HvmWatchTarget target() const;
    bool addressValid() const;

private:
    void updateGranularity();
    bool parseLength(unsigned long long* valueOut) const;

    QLabel* m_targetLabel = nullptr;
    QComboBox* m_addressKind = nullptr;
    QLineEdit* m_address = nullptr;
    QLineEdit* m_length = nullptr;
    QCheckBox* m_read = nullptr;
    QCheckBox* m_write = nullptr;
    QCheckBox* m_execute = nullptr;
    QLabel* m_granularity = nullptr;
};
