// ProcessDetailWindow.HvmWatch：对 EPROCESS 与令牌的单个字段建立 R-1 首次访问监视。
//
// 这一份回答的问题与「Process Detail Evidence」页其余部分不同：那些说的是
// 「现在长什么样」，这里说的是「下一次是谁动的」。快照能告诉用户令牌被换过、
// 进程从链表里消失了，但答不出是谁、从哪条指令、什么时候——因为那一刻已经
// 过去了。首次访问监视把问题换个方向问：盯住那几个字节，等下一次访问。
//
// 贯穿这一份的三条规矩：
//
// - **偏移只能来自驱动。** 字段地址 = EPROCESS 基址 + 字段偏移，而偏移是随
//   Windows 版本变化的内核结构布局。在用户态写死一个常数，算错时不会报错，
//   只会把监视装到一页毫不相干的内存上，然后那条监视永远不响——用户读到的
//   是「没人动过」。所以偏移一律取 R0 随响应带回的那一份，问不出来就禁用
//   按钮并说明原因。
//
// - **每次都重新问一次 R0。** 不复用上一次刷新的结果：进程可能已经退出，
//   EPROCESS 可能已经被回收，用旧地址装监视会盯到一页已经属于别人的内存。
//
// - **EPROCESS 跨页，所以不存在「监视整个 EPROCESS」。** 它是池分配的大结构，
//   一条监视只覆盖一个 4 KiB 页。每个字段都按它自己的地址单独翻译、单独装。

#include "ProcessDetailWindow.h"

#include "../ArkDriverClient/ArkDriverClient.h"
#include "../UI/HvmWatchDialog.h"
#include "../Internationalization/LanguageManager.h"

#include <QLabel>
#include <QMetaObject>
#include <QPointer>
#include <QPushButton>
#include <QString>

#include <algorithm>
#include <memory>

namespace
{
    // hex64：与本窗口其余地址显示一致的十六进制格式。
    QString watchHex64(const unsigned long long value)
    {
        return QStringLiteral("0x%1")
            .arg(value, 16, 16, QLatin1Char('0')).toUpper();
    }

    /*
     * offsetUsable：判断一个协议偏移能不能拿去做加法。
     *
     * 唯一的失败编码是 KSWORD_ARK_PROCESS_OFFSET_UNAVAILABLE（0xFFFFFFFF）。
     * 它是 32 位的，零扩展进 64 位相加会得到「基址 + 4 GiB - 1」——那个地址
     * 多半在翻译时响亮地失败，但没有任何理由把判断交给运气。
     *
     * 不要顺手把 0 也当成不可用：实测全部 profile 里没有一个 Ep* 偏移是 0，
     * 而把合法的 0 判成不可用会让某个未来版本上的字段无声地失去入口。
     */
    bool offsetUsable(const unsigned long value)
    {
        return value != KSWORD_ARK_PROCESS_OFFSET_UNAVAILABLE;
    }
}

/*
 * startKernelFieldWatch：为指定字段建立一条首次访问监视。
 *
 * 入参 target：要盯哪个字段，见 ProcessDetailWindow.h 的 KernelFieldWatchTarget。
 * 返回：无。查询走后台线程（阻塞 IOCTL），结果回到 UI 线程后弹安装对话框。
 *
 * 失败一律走状态行说明原因，不静默返回——一个点了没反应的按钮，与一个装上了
 * 但盯错地方的监视，是这个功能仅有的两种失败方式，两种都要能看见。
 */
void ProcessDetailWindow::startKernelFieldWatch(const KernelFieldWatchTarget target)
{
    const unsigned long processId =
        static_cast<unsigned long>(m_baseRecord.pid);
    if (processId == 0UL)
    {
        if (m_watchStatusLabel != nullptr)
        {
            m_watchStatusLabel->setText(
                QStringLiteral("当前窗口没有绑定有效的进程。"));
        }
        return;
    }
    if (m_watchStatusLabel != nullptr)
    {
        m_watchStatusLabel->setText(
            QStringLiteral("正在向 R0 重新查询这个进程的内核对象地址与字段偏移..."));
    }
    if (!m_kernelFieldWatchOperation)
        m_kernelFieldWatchOperation = std::make_unique<ks::ui::AsyncOperation>(this);
    struct WatchPreparationRequest
    {
        std::uint32_t processId;
        std::uint64_t creationTime;
        KernelFieldWatchTarget target;
    };
    struct WatchPreparation
    {
        ksword::ark::ProcessRuntimeDetailResult detail;
        ksword::ark::VirtualAddressTranslateResult pte;
        bool hasPte = false;
    };
    const WatchPreparationRequest request{processId, m_baseRecord.creationTime100ns, target};
    m_kernelFieldWatchOperation->submit<WatchPreparationRequest, WatchPreparation>(request,
        [](const WatchPreparationRequest& value, const ks::ui::AsyncOperationToken& token)
        {
            WatchPreparation prepared;
            const ksword::ark::DriverClient client;
            if (token.isCanceled()) return prepared;
            prepared.detail = client.queryProcessRuntimeDetail(value.processId);
            if (!token.isCanceled() && value.target == KernelFieldWatchTarget::ProcessObjectPte
                && prepared.detail.io.ok && prepared.detail.response.processObjectAddress != 0ULL
                && (prepared.detail.response.fieldFlags & KSWORD_ARK_PROCESS_DETAIL_FIELD_OBJECT_ADDRESS) != 0UL)
            {
                prepared.pte = client.translateVirtualAddress(0U, prepared.detail.response.processObjectAddress);
                prepared.hasPte = true;
            }
            return prepared;
        },
        [this, request](const WatchPreparation& prepared, std::uint64_t)
        {
            if (request.processId != m_baseRecord.pid || request.creationTime != m_baseRecord.creationTime100ns) return true;
            applyKernelFieldWatch(prepared.detail, request.target, prepared.hasPte ? &prepared.pte : nullptr);
            return true;
        },
        [this](std::exception_ptr)
        {
            if (m_watchStatusLabel) m_watchStatusLabel->setText(ks::i18n::sourceText(QStringLiteral("后台任务发生异常。")));
        });
}

/*
 * applyKernelFieldWatch：拿到 R0 详情之后，把它换算成一条监视请求。
 *
 * 入参 detail：queryProcessRuntimeDetail 的结果（按值传进 UI 线程）。
 * 入参 target：要盯哪个字段。
 * 返回：无。可用时弹出安装对话框，不可用时把原因写进状态行。
 */
void ProcessDetailWindow::applyKernelFieldWatch(
    const ksword::ark::ProcessRuntimeDetailResult& detail,
    const KernelFieldWatchTarget target,
    const ksword::ark::VirtualAddressTranslateResult* const preparedPte)
{
    const auto report = [this](const QString& text) {
        if (m_watchStatusLabel != nullptr)
        {
            m_watchStatusLabel->setText(text);
        }
    };
    if (!detail.io.ok)
    {
        report(QStringLiteral("R0 查询失败，拿不到这个进程的内核对象地址：%1")
            .arg(QString::fromStdString(detail.io.message)));
        return;
    }
    const KSWORD_ARK_PROCESS_DETAIL_RESPONSE& response = detail.response;
    const unsigned long long processObject = response.processObjectAddress;
    // EPROCESS 基址是所有字段地址的基准，它缺了下面哪一条都算不出来。
    if ((response.fieldFlags &
            KSWORD_ARK_PROCESS_DETAIL_FIELD_OBJECT_ADDRESS) == 0UL ||
        processObject == 0ULL)
    {
        report(QStringLiteral("R0 没有回报这个进程的 EPROCESS 地址，所有字段监视都无从下手。"));
        return;
    }
    /*
     * 偏移一组是否可用，要先看 OFFSET_SOURCES 这一位。
     *
     * 它不置位时整个 offsets 结构是零值，而 0 在这里是一个**合法**的偏移值，
     * 于是缺失会伪装成「这个字段在 EPROCESS 的第 0 个字节」。承重的判断仍然
     * 是每个偏移自己的哨兵，这一位只是把「整组都没填」这种情况提前挡掉。
     */
    const bool offsetsPresent = (response.fieldFlags &
        KSWORD_ARK_PROCESS_DETAIL_FIELD_OFFSET_SOURCES) != 0UL;

    unsigned long long address = 0ULL;
    unsigned long long length = 0ULL;
    QString label;
    bool byVirtualAddress = true;
    // 页表项那一条走另一条路：它要的是 PTE 的物理地址，由统一入口自己翻译。
    bool viaPte = false;

    switch (target)
    {
    case KernelFieldWatchTarget::ActiveProcessLinks:
    {
        if (!offsetsPresent || !offsetUsable(response.offsets.epActiveProcessLinks))
        {
            report(QStringLiteral("本机的 DynData 没有解析出 EPROCESS.ActiveProcessLinks 的偏移，算不出这个字段的地址。这是一条能力限制，不是这个进程的问题。"));
            return;
        }
        address = processObject + response.offsets.epActiveProcessLinks;
        // LIST_ENTRY 恒为两个指针；这个宽度由指针宽度决定，与 Windows 版本无关。
        length = 2ULL * sizeof(void*);
        label = QStringLiteral("PID %1 %2 的 EPROCESS.ActiveProcessLinks")
            .arg(m_baseRecord.pid)
            .arg(QString::fromStdString(m_baseRecord.processName));
        break;
    }
    case KernelFieldWatchTarget::TokenSlot:
    {
        if (!offsetsPresent || !offsetUsable(response.offsets.epToken))
        {
            report(QStringLiteral("本机的 DynData 没有解析出 EPROCESS.Token 的偏移，算不出这个槽位的地址。"));
            return;
        }
        address = processObject + response.offsets.epToken;
        // EX_FAST_REF 就是一个指针宽：低位是引用计数，高位是对象地址。
        length = sizeof(void*);
        label = QStringLiteral("PID %1 %2 的 EPROCESS.Token 槽位")
            .arg(m_baseRecord.pid)
            .arg(QString::fromStdString(m_baseRecord.processName));
        break;
    }
    case KernelFieldWatchTarget::TokenObject:
    {
        /*
         * 这一条盯的是令牌**对象**，与上一条不是同一个目标。
         *
         * tokenFastRef 的低 4 位是 EX_FAST_REF 的引用计数，绝不能直接当地址；
         * 驱动已经抹掉那几位并单独回报了 tokenObjectAddress，用它。
         */
        if ((response.fieldFlags &
                KSWORD_ARK_PROCESS_DETAIL_FIELD_TOKEN_FASTREF) == 0UL ||
            response.tokenObjectAddress == 0ULL)
        {
            report(QStringLiteral("R0 没有回报令牌对象地址（可能本次查询没有包含令牌字段组）。"));
            return;
        }
        address = response.tokenObjectAddress;
        // _TOKEN 是变长结构，大小是 Windows 结构布局，不在用户态写死：给整页。
        length = 0ULL;
        label = QStringLiteral("PID %1 %2 引用的令牌对象 %3 基址所在页")
            .arg(m_baseRecord.pid)
            .arg(QString::fromStdString(m_baseRecord.processName))
            .arg(watchHex64(response.tokenObjectAddress));
        break;
    }
    case KernelFieldWatchTarget::Protection:
    {
        /*
         * 三个字节的起点取三个偏移里**最小**的那个，长度取跨度。
         *
         * 不从 Protection 起头：实测全部已知 profile 上的顺序是
         * SignatureLevel、SectionSignatureLevel、Protection（Protection 在最后），
         * 从它起头往后读会正好漏掉真正要看的另外两个。也不写死 3：顺序与相邻性
         * 是观测到的规律，不是架构保证，所以按运行期拿到的三个数算。
         */
        unsigned long long lowest = 0ULL;
        unsigned long long highest = 0ULL;
        bool any = false;
        const unsigned long candidates[3] = {
            response.offsets.epProtection,
            response.offsets.epSignatureLevel,
            response.offsets.epSectionSignatureLevel,
        };
        for (const unsigned long candidate : candidates)
        {
            if (!offsetsPresent || !offsetUsable(candidate))
            {
                continue;
            }
            const unsigned long long value = candidate;
            lowest = any ? (std::min)(lowest, value) : value;
            highest = any ? (std::max)(highest, value) : value;
            any = true;
        }
        if (!any)
        {
            report(QStringLiteral("本机的 DynData 没有解析出 Protection / SignatureLevel / SectionSignatureLevel 任何一个的偏移。"));
            return;
        }
        address = processObject + lowest;
        length = highest - lowest + 1ULL;
        label = QStringLiteral("PID %1 %2 的 EPROCESS 保护与签名级别")
            .arg(m_baseRecord.pid)
            .arg(QString::fromStdString(m_baseRecord.processName));
        break;
    }
    case KernelFieldWatchTarget::ImageFileName:
    {
        if (!offsetsPresent || !offsetUsable(response.offsets.epImageFileName))
        {
            report(QStringLiteral("本机的 DynData 没有解析出 EPROCESS.ImageFileName 的偏移。"));
            return;
        }
        address = processObject + response.offsets.epImageFileName;
        // 驱动就是按这个长度读回 imageName 的，长度与协议缓冲一致。
        length = KSWORD_ARK_RUNTIME_IMAGE_NAME_CHARS - 1;
        label = QStringLiteral("PID %1 %2 的 EPROCESS.ImageFileName")
            .arg(m_baseRecord.pid)
            .arg(QString::fromStdString(m_baseRecord.processName));
        break;
    }
    case KernelFieldWatchTarget::ProcessObjectPte:
    {
        viaPte = true;
        address = processObject;
        label = QStringLiteral("PID %1 %2 的 EPROCESS 基址 %3")
            .arg(m_baseRecord.pid)
            .arg(QString::fromStdString(m_baseRecord.processName))
            .arg(watchHex64(processObject));
        break;
    }
    default:
        return;
    }

    if (viaPte)
    {
        report(QStringLiteral("正在翻译 EPROCESS 基址所在页的页表项..."));
        if (preparedPte != nullptr) ks::ui::openHvmWatchOnPtePrepared(this, address, label, *preparedPte);
        return;
    }
    /*
     * 字段跨页时把长度收到本页剩余部分。
     *
     * 硬件监视范围不因此改变（EPT 恒为整页）；收窄的是「命中算不算落在你的
     * 目标上」这个判据。不收窄的话，一次落在后半段的访问会被判成不在范围内，
     * 而那句话是假的——真正的事实是后半段根本不在被监视的页上。
     */
    const unsigned long long remaining = 0x1000ULL - (address & 0xFFFULL);
    QString truncationNote;
    if (length > remaining)
    {
        truncationNote = QStringLiteral("；注意这个字段横跨页边界，硬件只覆盖它在前一页的 %1 字节")
            .arg(remaining);
        length = remaining;
    }
    report(QStringLiteral("目标地址 %1（EPROCESS %2），%3 字节%4。")
        .arg(watchHex64(address))
        .arg(watchHex64(processObject))
        .arg(length)
        .arg(truncationNote));

    ks::ui::HvmWatchRequest request;
    request.virtualAddress = byVirtualAddress;
    request.address = address;
    request.length = length;
    // 一律只给写：这几个字段关心的都是「谁改了它」，而勾读会被架构归一化
    // 连带放大成读写，等于悄悄换了监视目标。
    request.access = KSWORD_ARK_HVM_EPT_ACCESS_WRITE;
    request.label = label + truncationNote;
    ks::ui::openHvmWatch(this, request);
}
