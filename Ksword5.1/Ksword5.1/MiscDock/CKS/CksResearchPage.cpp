#include "../../Framework.h"
#include "CksResearchPage.h"

#include "../../Internationalization/LanguageManager.h"
#include "../../UI/CodeEditorWidget.h"
#include "../../UI/FlowLayout.h"
#include "../../UI/ParameterWorkspace.h"
#include "../../UI/ToolbarMetrics.h"

#include <QDesktopServices>
#include <QGroupBox>
#include <QLabel>
#include <QPushButton>
#include <QSizePolicy>
#include <QUrl>
#include <QVBoxLayout>

namespace
{
    // AddResearchText：添加自然高度说明，长标识不参与横向最小宽度计算。
    void AddResearchText(QWidget* parent, QVBoxLayout* layout,
        const QString& key, const QString& fallback)
    {
        auto* label = new QLabel(parent); // label：可选择、可换行的研究正文。
        label->setWordWrap(true);
        label->setTextInteractionFlags(Qt::TextSelectableByMouse);
        label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        ks::i18n::LanguageManager::instance().bindText(label, key, fallback);
        layout->addWidget(label);
    }

    // ResearchSection：分区通过共享参数组件取得标题、留白、字号与主题。
    QVBoxLayout* ResearchSection(QGroupBox* group,
        const QString& key, const QString& fallback)
    {
        ks::i18n::LanguageManager::instance().bindText(group, key, fallback);
        auto* layout = new QVBoxLayout(group); // layout：区内说明按自然高度排列。
        layout->setSpacing(8);
        ks::ui::StyleParameterSection(group);
        return layout;
    }
}

namespace ks::misc
{
    QString CksResearchVerificationTemplate()
    {
        // 整份模板只维护这一处；共享编辑器的搜索、复制与导出使用同一文档。
        return ks::i18n::text(QStringLiteral("misc.cks.research.record.template"),
            QStringLiteral("CKS 免重置验证记录（模板）\n"
                "状态：尚未测试\n"
                "候选：启动前重新建立登记 / 保留应用的修复安装\n\n"
                "实验日期与环境：\n"
                "Windows 版本、build、SKU：\n"
                "ci.dll / winload.efi / skci.dll 版本、SHA-256、RSDS：\n"
                "恢复方案及恢复验证结果：\n\n"
                "实验前后证据\n"
                "产品许可 API 结果：\n"
                "Licensed 与 CustomKernelSignerLockedEnabled：\n"
                "Secure Boot、HVCI 实际运行状态：\n"
                "PK/KEK 信任关系与签名者身份：\n"
                "基础策略 ID、签名、版本、当前生效状态：\n"
                "实际 CKS 信任诊断字段：\n"
                "目标驱动 SHA-256、签名及加载结果：\n"
                "Code Integrity 事件、错误码、对应策略 ID：\n\n"
                "结果与后续\n"
                "首次启用是否成功：\n"
                "重启后登记、策略及目标驱动加载：\n"
                "累积更新后登记、策略及目标驱动加载：\n"
                "修复安装是否保留应用、迁移或重建登记：\n"
                "回滚结果、局限及附件位置：\n\n"
                "许可值、登记值或策略部署成功，均不单独证明 CKS 可用。\n"));
    }

    CodeEditorWidget* PopulateCksResearchPage(
        QWidget* page, QVBoxLayout* parameters, QWidget* resultParent)
    {
        auto& language = ks::i18n::LanguageManager::instance(); // language：绑定可热切换的资料文字。
        AddResearchText(page, parameters, QStringLiteral("misc.cks.research.intro"),
            QStringLiteral("两条路径都是尚未运行验证的研究候选。先在可恢复的独立环境记录证据，再判断是否适用于目标系统；此页提供机制、待证条件和验证记录模板。"));

        // 最强候选保持展开，按机制、待证条件与验收判据组织，不添加启用动作。
        auto* enrollment = new QGroupBox(page); // enrollment：启动登记候选。
        enrollment->setObjectName(QStringLiteral("ksCksResearchEnrollment"));
        auto* enrollmentBody = ResearchSection(enrollment,
            QStringLiteral("misc.cks.research.enrollment.title"),
            QStringLiteral("启动前重新建立登记 · 未验证"));
        AddResearchText(enrollment, enrollmentBody,
            QStringLiteral("misc.cks.research.enrollment.mechanism"),
            QStringLiteral("机制证据：本机 winload 与 CI 区分“尚未登记”和“已有关闭登记”。已有关闭值优先；首次初始化分支消费启动候选，所查函数本身不调用整机重置。"));
        AddResearchText(enrollment, enrollmentBody,
            QStringLiteral("misc.cks.research.enrollment.conditions"),
            QStringLiteral("待证条件：启动许可、PK/KEK 信任的基础策略、引导器与 CI/SKCI 的一致登记结果，以及目标驱动的实际签名兼容性。登记字段变为 1 仍不能作为成功结论。"));
        AddResearchText(enrollment, enrollmentBody,
            QStringLiteral("misc.cks.research.enrollment.acceptance"),
            QStringLiteral("后续判据：比较启动前后登记与实际 CKS 诊断；验证目标驱动加载、Code Integrity 事件、重启保持和累积更新后的行为。"));
        parameters->addWidget(enrollment);

        // 安装候选单独保留未知迁移职责，避免把清单局部无条目当作成功证据。
        auto* repair = new QGroupBox(page); // repair：保留应用安装候选。
        repair->setObjectName(QStringLiteral("ksCksResearchRepair"));
        auto* repairBody = ResearchSection(repair,
            QStringLiteral("misc.cks.research.repair.title"),
            QStringLiteral("保留应用的修复安装 · 未验证"));
        AddResearchText(repair, repairBody, QStringLiteral("misc.cks.research.repair.mechanism"),
            QStringLiteral("新线索：本机 CI 迁移清单明确保留策略与配置，但没有把 CI\\Protected 列入迁移 include 或 exclude；CKS 登记没有显式条目。"));
        AddResearchText(repair, repairBody, QStringLiteral("misc.cks.research.repair.conditions"),
            QStringLiteral("待证条件：Setup 核心是否整体迁移 SYSTEM hive、保留关闭登记或重新建立登记。普通升级、修复重装与 PBR 不能混为同一路径；PBR 仍属于系统重置。"));
        AddResearchText(repair, repairBody, QStringLiteral("misc.cks.research.repair.acceptance"),
            QStringLiteral("后续判据：在匹配版本的独立环境比较安装前后登记、策略及应用保留情况。若仍继承关闭登记，所查启动链不会因此授予 CKS。"));
        parameters->addWidget(repair);

        // 资料入口只打开原文，不调用安装、注册表、固件、策略部署或驱动加载接口。
        auto* links = new QWidget(page); // links：可换行的资料工具行。
        auto* linkFlow = new ks::ui::FlowLayout(links, 0, 8, 8);
        auto* repairDocs = new QPushButton(links); // repairDocs：修复重装的第一方说明。
        language.bindText(repairDocs, QStringLiteral("misc.cks.research.repair.docs"),
            QStringLiteral("修复安装文档"));
        language.bindToolTip(repairDocs, QStringLiteral("misc.cks.research.repair.docs.tip"),
            QStringLiteral("查看保留应用、文件和设置的微软修复重装说明；不启动安装"));
        ks::ui::NormalizeToolbarControl(repairDocs);
        linkFlow->addWidget(repairDocs);
        QObject::connect(repairDocs, &QPushButton::clicked, links, []()
        {
            QDesktopServices::openUrl(QUrl(QStringLiteral("https://support.microsoft.com/en-us/windows/deployment/install-upgrade/fix-issues-by-reinstalling-the-current-version-of-windows")));
        });
        parameters->addWidget(links);

        // 精确样本和排除项属于低频资料，使用共享折叠分区且不参与实时系统查询。
        auto* sample = new QGroupBox(page); // sample：2026-10-12 的固定研究样本。
        sample->setObjectName(QStringLiteral("ksCksResearchSample"));
        auto* sampleBody = ResearchSection(sample,
            QStringLiteral("misc.cks.research.sample.title"),
            QStringLiteral("研究样本与限制"));
        AddResearchText(sample, sampleBody, QStringLiteral("misc.cks.research.sample.identity"),
            QStringLiteral("静态样本日期：2026-10-12，非当前系统查询。Windows 26300.9022；ci.dll 26100.8941；winload.efi、skci.dll 与 vbssysprep.dll 26100.9022。该版本组合不是兼容性门槛。"));
        AddResearchText(sample, sampleBody, QStringLiteral("misc.cks.research.sample.state"),
            QStringLiteral("当次只读证据：产品许可 API 与 Licensed 均为 1，CustomKernelSignerLockedEnabled 为 0；Secure Boot 与 HVCI 严格态实际运行。未取得有效策略清单、PK/KEK 所有权或实际 CKS 信任诊断。"));
        AddResearchText(sample, sampleBody, QStringLiteral("misc.cks.research.sample.limits"),
            QStringLiteral("已查普通策略刷新和 SKCI 更新均不建立首次登记；单独维持 SSDE 许可也不能跨越登记门槛。未穷尽全部安装与恢复组件，也没有首次启用、驱动加载或跨重启成功证据；不能推导 HVCI 一律必须关闭。"));
        parameters->addWidget(ks::ui::FoldParameterSection(sample));
        parameters->addStretch();

        // 记录模板独占研究页结果空间，复用编辑器的搜索、复制与导出，不叠加外层滚动。
        auto* record = new CodeEditorWidget(resultParent); // record：尚未测试的完整验收模板。
        record->setObjectName(QStringLiteral("ksCksResearchRecord"));
        record->setReadOnly(true);
        record->setReadOnlyExportEnabled(true);
        record->setWordWrapEnabled(true);
        record->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
        record->setMinimumHeight(150);
        record->setRawText(CksResearchVerificationTemplate());
        return record;
    }
}
