#pragma once

// CKS 管理页：分别呈现微软部署、SSDE 实验与只读免重置研究。
// 系统写入要求选择具体文件并再次确认；研究子页只展示候选与记录模板。

#include "CksBackend.h"

#include <QWidget>

#include <functional>
#include <atomic>
#include <memory>
#include <thread>

class CodeEditorWidget;
class QEvent;
class QLabel;
class QPushButton;
class QShowEvent;
class QTableWidget;

namespace ks::misc
{
    class CksPage final : public QWidget
    {
    public:
        // 构造时只建页面；真正的系统状态在首次显示时后台刷新。
        explicit CksPage(QWidget* parent = nullptr);
        ~CksPage() override;

    protected:
        // showEvent：页签可见时重新读取系统状态；不复用旧开关结论。
        void showEvent(QShowEvent* event) override;
        // changeEvent：切换语言时重绘所有动态状态文字。
        void changeEvent(QEvent* event) override;

    private:
        // initializeUi：建立共享状态区以及微软、SSDE、免重置研究三个子页。
        void initializeUi();
        // refresh：后台查询 Windows 许可、CI、SCM 和 CiTool，结果回到 UI 线程。
        void refresh();
        // showSnapshot：把只读回执显示为独立事实，不拼成“已完全启用”。
        void showSnapshot();
        // updateButtons：根据正在运行的操作与明确的准入条件控制动作。
        void updateButtons();
        // chooseFile：只在用户手势下打开文件选择器并显示 SHA-256。
        void chooseFile(const QString& kind);
        // runOperation：后台执行一次具体操作，然后重新读取系统状态。
        void runOperation(const std::function<bool(QString*)>& operation);
        // startWorker：统一拥有后台线程；页面析构时请求取消并等待线程离开，禁止 detached 回投。
        void startWorker(std::function<void(const std::shared_ptr<std::atomic_bool>&)> task);
        // confirm：不可逆或影响重启的操作需在不透明对话框中确认。
        bool confirm(const QString& text);
        // showResult：显示一次操作结论与系统回读边界。
        void showResult(const QString& text, bool success);

        cks::Snapshot m_snapshot; // m_snapshot：最近一次系统状态回读。
        bool m_hasSnapshot = false; // m_hasSnapshot：状态卡是否有真实数据。
        bool m_busy = false; // m_busy：防止刷新与操作并发重入。
        QString m_policyPath; // m_policyPath：用户选择的已签名 .cip 路径。
        QString m_helperPath; // m_helperPath：用户选择的 SSDE 启用程序路径。
        QString m_driverPath; // m_driverPath：用户选择的已签名 ssde.sys 路径。
        QString m_resultRaw; // m_resultRaw：未翻译的后台回执，语言变化时重绘。
        bool m_resultSuccess = false; // m_resultSuccess：最近一次操作是否成功。
        bool m_hasResult = false; // m_hasResult：是否已有可重绘的操作回执。
        std::shared_ptr<std::atomic_bool> m_cancelRequested =
            std::make_shared<std::atomic_bool>(false); // 每次操作独立门禁，队列中的旧回投也安全失效。
        std::thread m_worker; // m_worker：当前刷新/操作线程，始终可等待回收。

        QLabel* m_status = nullptr; // m_status：许可与安全启动摘要。
        QLabel* m_service = nullptr; // m_service：SSDE 服务与启动类型摘要。
        QLabel* m_policyFile = nullptr; // m_policyFile：策略路径和 SHA-256。
        QLabel* m_helperFile = nullptr; // m_helperFile：辅助程序路径和 SHA-256。
        QLabel* m_driverFile = nullptr; // m_driverFile：驱动路径和 SHA-256。
        QLabel* m_result = nullptr; // m_result：本次操作回执。
        QTableWidget* m_policyList = nullptr; // m_policyList：CiTool 报告的策略列表。
        QPushButton* m_refresh = nullptr; // m_refresh：图标化刷新动作。
        QPushButton* m_deploy = nullptr; // m_deploy：部署或更新签名策略。
        QPushButton* m_launch = nullptr; // m_launch：启动 SSDE 启用程序。
        QPushButton* m_install = nullptr; // m_install：安装按需启动的 ssde 驱动。
        CodeEditorWidget* m_researchPlan = nullptr; // m_researchPlan：只读研究验证模板，语言切换时重绘。
        QPushButton* m_start = nullptr; // m_start：本次启动中加载 ssde 驱动。
        QPushButton* m_enableBoot = nullptr; // m_enableBoot：设置下次 BOOT_START。
        QPushButton* m_disableBoot = nullptr; // m_disableBoot：改回按需启动。
    };
}
