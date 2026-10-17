#pragma once

#include "KernelServiceAuditEvidence.h"
#include <QString>
#include <QStringList>
#include <vector>

class QLabel;
class QHBoxLayout;
class QTableWidget;

namespace ks::kernel::audit
{
    // QueryIssue：保留失败来源、原始错误及阶段原因；不在 worker 调用翻译器。
    struct QueryIssue
    {
        QString stage; // 稳定来源名称，例如 SSDT 或 ObjectType。
        QString reason; // 中文规范源文本，显示时才按当前语言翻译。
        long nativeStatus = 0; // 失败/部分结果的原始 NTSTATUS。
        unsigned long win32 = 0; // 传输层 Win32 错误，成功传输时为零。
    };
    // QueryModules：复用项目公共模块查询，不增加第二套原生 API/协议。
    ModuleSnapshot QueryModules();
    // CategoryText：仅在 UI 线程把稳定业务类别投影为当前语言文本。
    QString CategoryText(Category category);
    // IssueText：把来源及错误细节完整放入状态释义，保留未知/失败语义。
    QString IssueText(const std::vector<QueryIssue>& issues);
    // CheckQuery：成功传输不替代完整性检查；失败原因集中保存供 UI 诊断。
    bool CheckQuery(std::vector<QueryIssue>& issues, const QString& stage, bool transportOk,
        bool complete, long nativeStatus, unsigned long win32);
    // BindAuditLabel：登记动态主题拥有者，深浅换肤只改外观，不重新枚举。
    void BindAuditLabel(QLabel* label, bool secondary);
    // InstallColumnPresets：两页复用共享分段控件、表头逐列选择及自定义空选态。
    void InstallColumnPresets(QTableWidget* table, QHBoxLayout* toolbar,
        const std::vector<int>& overview, const std::vector<int>& evidence);
}
