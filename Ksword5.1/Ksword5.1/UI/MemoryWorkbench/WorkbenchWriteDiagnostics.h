#pragma once

#include "../../Internationalization/LanguageManager.h"

#include <QString>
#include <string>
#include <vector>

// GUI presentation only. Keep canonical worker reports/audit unchanged.
// Known envelopes are translated recursively; opaque backend/exception
// detail is preserved. Iterative peeling avoids a recursive call stack.
namespace ks::ui::workbench_write_diagnostics
{
    namespace detail
    {
        inline QString FormatLeaf(const QString& raw)
        {
            if (raw == QLatin1StringView("current target state is unavailable"))
                return ks::i18n::sourceText(QStringLiteral("当前目标状态不可用"));
            if (raw == QLatin1StringView("write operation stopped at a safe boundary"))
                return ks::i18n::sourceText(QStringLiteral("写入操作已在安全边界停止"));
            if (raw == QLatin1StringView("write operation owner was lost at a safe boundary"))
                return ks::i18n::sourceText(QStringLiteral("写入操作的所属窗口已关闭，操作已在安全边界停止"));
            if (raw == QLatin1StringView("target session changed while waiting for confirmation"))
                return ks::i18n::sourceText(QStringLiteral("等待确认期间目标会话已变化"));
            if (raw == QLatin1StringView("source revision changed while waiting for confirmation"))
                return ks::i18n::sourceText(QStringLiteral("等待确认期间来源代次已变化"));
            if (raw == QLatin1StringView("content revision changed while waiting for confirmation"))
                return ks::i18n::sourceText(QStringLiteral("等待确认期间编辑内容已变化"));
            if (raw == QLatin1StringView("pre-write re-read was partial"))
                return ks::i18n::sourceText(QStringLiteral("写入前复核仅读取了部分字节"));
            if (raw == QLatin1StringView("target bytes differ from the staged original bytes"))
                return ks::i18n::sourceText(QStringLiteral("目标字节与暂存时的原始字节不同"));
            if (raw == QLatin1StringView("backend requested approval again after it was granted"))
                return ks::i18n::sourceText(QStringLiteral("后端在获得批准后再次要求批准"));
            if (raw == QLatin1StringView("backend requested approval after persisting part of the block"))
                return ks::i18n::sourceText(QStringLiteral("后端已写入部分字节后才要求批准"));
            if (raw == QLatin1StringView("user declined the write confirmation"))
                return ks::i18n::sourceText(QStringLiteral("用户取消了写入确认"));
            if (raw == QLatin1StringView("user denied the explicit approval requested by the backend"))
                return ks::i18n::sourceText(QStringLiteral("用户拒绝了后端要求的显式批准"));
            if (raw == QLatin1StringView("target session is not self-consistent"))
                return ks::i18n::sourceText(QStringLiteral("目标会话信息不一致"));
            if (raw == QLatin1StringView("frozen write blocks are invalid or overlap"))
                return ks::i18n::sourceText(QStringLiteral("捕获的写入块无效或相互重叠"));
            if (raw == QLatin1StringView("overlay refused to accept the verified write"))
                return ks::i18n::sourceText(QStringLiteral("叠加层拒绝接收已核验的写入结果"));
            if (raw == QLatin1StringView("another commit is already in progress"))
                return ks::i18n::sourceText(QStringLiteral("另一项写入提交正在进行"));
            if (raw == QLatin1StringView("unknown exception"))
                return ks::i18n::sourceText(QStringLiteral("未知异常"));
            if (raw == QLatin1StringView("write validation rejected the target"))
                return ks::i18n::sourceText(QStringLiteral("写入校验拒绝了当前目标"));
            if (raw == QLatin1StringView("write validation failed with an unknown exception"))
                return ks::i18n::sourceText(QStringLiteral("写入校验发生未知异常"));
            if (raw == QLatin1StringView("kernel virtual address write requires a kernel mutation port, but none is configured"))
                return ks::i18n::sourceText(QStringLiteral("缺少内核写入事务后端，无法写入内核虚拟地址"));
            if (raw == QLatin1StringView("kernel mutation write could not read a before-snapshot for the target range"))
                return ks::i18n::sourceText(QStringLiteral("内核写入无法读取目标范围的写前快照"));
            if (raw == QLatin1StringView("verified write receipt was rejected"))
                return ks::i18n::sourceText(QStringLiteral("已核验写入的回执被拒绝"));
            if (raw == QLatin1StringView("verified write journal failed"))
                return ks::i18n::sourceText(QStringLiteral("已核验写入的历史记录保存失败"));
            if (raw == QLatin1StringView("write receipt retention failed"))
                return ks::i18n::sourceText(QStringLiteral("写入回执保存失败"));
            if (raw == QLatin1StringView("verified history replay cursor was not advanced"))
                return ks::i18n::sourceText(QStringLiteral("已核验历史回放未能推进记录位置"));
            if (raw == QLatin1StringView("application shutdown is waiting for accepted cleanup"))
                return ks::i18n::sourceText(QStringLiteral("应用程序正在等待已接受操作完成清理"));
            if (raw == QLatin1StringView("frozen write runner capture is incomplete or changed"))
                return ks::i18n::sourceText(QStringLiteral("写入任务的捕获上下文不完整或已变化"));
            if (raw == QLatin1StringView("frozen-write-process-identity-unavailable"))
                return ks::i18n::sourceText(QStringLiteral("无法确认目标进程身份"));
            if (raw == QLatin1StringView("frozen-write-process-identity-changed"))
                return ks::i18n::sourceText(QStringLiteral("目标进程身份已变化"));
            if (raw == QLatin1StringView("frozen-write-ddma-scratch-dirty"))
                return ks::i18n::sourceText(QStringLiteral("DDMA 暂存区尚未恢复，已停止后续写入"));
            if (raw == QLatin1StringView("frozen-write-unknown-kernel-transaction"))
                return ks::i18n::sourceText(QStringLiteral("内核写入事务不存在或已失效"));
            if (raw == QLatin1StringView("frozen-write-factory-failed"))
                return ks::i18n::sourceText(QStringLiteral("创建写入后端失败"));
            if (raw == QLatin1StringView("frozen-write-invalid-capture"))
                return ks::i18n::sourceText(QStringLiteral("捕获的写入上下文无效"));
            if (raw == QLatin1StringView("frozen-write-scope-policy"))
                return ks::i18n::sourceText(QStringLiteral("当前策略不允许写入此地址空间"));
            if (raw == QLatin1StringView("frozen-write-duplicate-definition"))
                return ks::i18n::sourceText(QStringLiteral("捕获的指针链定义存在重复标识"));
            if (raw == QLatin1StringView("frozen-write-identity-factory-required"))
                return ks::i18n::sourceText(QStringLiteral("缺少目标进程身份校验后端"));
            if (raw == QLatin1StringView("frozen-write-io-factory-empty"))
                return ks::i18n::sourceText(QStringLiteral("写入后端创建结果为空"));
            if (raw == QLatin1StringView("frozen-write-wrong-thread"))
                return ks::i18n::sourceText(QStringLiteral("写入阶段在错误线程执行"));
            if (raw == QLatin1StringView("frozen-write-authority-changed"))
                return ks::i18n::sourceText(QStringLiteral("写入授权已失效"));
            if (raw == QLatin1StringView("frozen-write-ddma-generation-changed"))
                return ks::i18n::sourceText(QStringLiteral("DDMA 会话已变化"));
            if (raw == QLatin1StringView("frozen-write-address-policy"))
                return ks::i18n::sourceText(QStringLiteral("写入范围不符合当前地址空间策略"));
            const QLatin1StringView witness("frozen-write-pointer-witness-changed/");
            if (raw.startsWith(witness))
            {
                const QString code = raw.mid(witness.size());
                if (code == QLatin1StringView("1"))
                    return ks::i18n::sourceText(QStringLiteral("指针链写入凭据无效"));
                if (code == QLatin1StringView("2"))
                    return ks::i18n::sourceText(QStringLiteral("指针链定义已变化"));
                if (code == QLatin1StringView("3"))
                    return ks::i18n::sourceText(QStringLiteral("指针链目标会话已变化"));
                if (code == QLatin1StringView("4"))
                    return ks::i18n::sourceText(QStringLiteral("指针链解析路径已变化"));
                if (code == QLatin1StringView("5"))
                    return ks::i18n::sourceText(QStringLiteral("指针链写入范围记录已达上限"));
                return ks::i18n::sourceText(QStringLiteral("指针链写入凭据已失效（原因代码：%1）")).arg(code);
            }
            return raw;
        }
    }

    inline QString Format(const std::string& raw)
    {
        QString remaining = QString::fromUtf8(raw.data(), static_cast<qsizetype>(raw.size()));
        std::vector<QString> envelopes;
        for (;;)
        {
            if (remaining.startsWith(QLatin1StringView("current target state failed: ")))
            {
                remaining.remove(0, 29);
                envelopes.push_back(ks::i18n::sourceText(QStringLiteral("获取当前目标状态失败：%1")));
            }
            else if (remaining.startsWith(QLatin1StringView("pre-write re-read failed: ")))
            {
                remaining.remove(0, 26);
                envelopes.push_back(ks::i18n::sourceText(QStringLiteral("写入前复核失败：%1")));
            }
            else if (remaining.startsWith(QLatin1StringView("write failed: ")))
            {
                remaining.remove(0, 14);
                envelopes.push_back(ks::i18n::sourceText(QStringLiteral("写入失败：%1")));
            }
            else if (remaining.startsWith(QLatin1StringView("read-back verification failed: ")))
            {
                remaining.remove(0, 31);
                envelopes.push_back(ks::i18n::sourceText(QStringLiteral("回读校验失败：%1")));
            }
            else if (remaining.startsWith(QLatin1StringView("read-back failed: ")))
            {
                remaining.remove(0, 18);
                envelopes.push_back(ks::i18n::sourceText(QStringLiteral("回读失败：%1")));
            }
            else if (remaining.startsWith(QLatin1StringView("final read-back failed: ")))
            {
                remaining.remove(0, 24);
                envelopes.push_back(ks::i18n::sourceText(QStringLiteral("最终回读失败：%1")));
            }
            else if (remaining.startsWith(QLatin1StringView("write validation failed: ")))
            {
                remaining.remove(0, 25);
                envelopes.push_back(ks::i18n::sourceText(QStringLiteral("写入校验失败：%1")));
            }
            else if (remaining.startsWith(QLatin1StringView("kernel mutation write before-snapshot failed: ")))
            {
                remaining.remove(0, 46);
                envelopes.push_back(ks::i18n::sourceText(QStringLiteral("内核写入的写前快照读取失败：%1")));
            }
            else if (remaining.startsWith(QLatin1StringView("write audit archive failed: ")))
            {
                remaining.remove(0, 28);
                envelopes.push_back(ks::i18n::sourceText(QStringLiteral("写入审计归档失败：%1")));
            }
            else if (remaining.startsWith(QLatin1StringView("verified write receipt failed: ")))
            {
                remaining.remove(0, 31);
                envelopes.push_back(ks::i18n::sourceText(QStringLiteral("已核验写入回执保存失败：%1")));
            }
            else if (remaining.startsWith(QLatin1StringView("write phase failed: ")))
            {
                remaining.remove(0, 20);
                envelopes.push_back(ks::i18n::sourceText(QStringLiteral("写入阶段失败：%1")));
            }
            else if (remaining.startsWith(QLatin1StringView("history replay receipt failed: ")))
            {
                remaining.remove(0, 31);
                envelopes.push_back(ks::i18n::sourceText(QStringLiteral("历史回放回执保存失败：%1")));
            }
            else break;
        }
        QString text = detail::FormatLeaf(remaining);
        for (auto it = envelopes.crbegin(); it != envelopes.crend(); ++it) text = it->arg(text);
        return text;
    }
}
