#include "CksPage.h"
#include "CksResearchPage.h"

#include "../../Internationalization/LanguageManager.h"
#include "../../UI/PrimaryPageStyle.h"
#include "../../UI/AsyncOperation.h"
#include "../../UI/ParameterWorkspace.h"
#include "../../UI/PageControlStyle.h"
#include "../../UI/ToolbarMetrics.h"
#include "../../UI/FlowLayout.h"
#include "../../UI/CodeEditorWidget.h"
#include "../../UI/ResultTableHost.h"
#include "../../UI/TablePresentation.h"
#include "../../theme.h"

#include <QApplication>
#include <QCryptographicHash>
#include <QDesktopServices>
#include <QDateTime>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QLabel>
#include <QMessageBox>
#include <QMetaObject>
#include <QPointer>
#include <QPushButton>
#include <QShowEvent>
#include <QTabWidget>
#include <QTableWidget>
#include <QUrl>
#include <QVBoxLayout>

#include <utility>

namespace {
const ks::ui::PageNavigationTabDefinition kCksPageNavigation[] = {
    {"official", "", "misc.cks.official.tab", "微软现行流程", true},
    {"ssde", "", "misc.cks.ssde.tab", "SSDE 实验流程", false},
    {"research", "", "misc.cks.research.tab", "免重置研究", false}
};
}

namespace
{
    // T：页面的动态文字统一从双语语言包读取。
    QString T(const QString& key, const QString& chinese)
    {
        return ks::i18n::text(key, chinese);
    }

    // CheckText：明确区分开启、关闭、未知和查询错误。
    QString CheckText(const ks::misc::cks::Check& check)
    {
        if (!check.known)
        {
            return T(QStringLiteral("misc.cks.state.unknown"), QStringLiteral("未知（错误 0x%1）"))
                .arg(check.error, 8, 16, QChar('0'));
        }
        return check.enabled
            ? T(QStringLiteral("misc.cks.state.on"), QStringLiteral("开启"))
            : T(QStringLiteral("misc.cks.state.off"), QStringLiteral("关闭"));
    }

    // Cached file evidence only; language changes never reopen or hash the selected file.
    QString FileLabel(const QString& path, const QByteArray& digest = {}, const bool pending = false)
    {
        if (path.isEmpty())
        {
            return T(QStringLiteral("misc.cks.file.none"), QStringLiteral("尚未选择文件"));
        }
        if (pending)
        {
            return path + QLatin1Char('\n')
                + T(QStringLiteral("misc.cks.file.hashing"), QStringLiteral("正在计算 SHA-256…"));
        }
        if (digest.isEmpty())
        {
            return path + QLatin1Char('\n')
                + T(QStringLiteral("misc.cks.file.unreadable"), QStringLiteral("文件已无法读取"));
        }
        return T(QStringLiteral("misc.cks.file.hash"), QStringLiteral("%1\nSHA-256：%2"))
            .arg(path, QString::fromLatin1(digest.toHex()));
    }

    // DetailText：后台只传原始回执；UI 线程再按当前语言显示。
    QString DetailText(const QString& raw)
    {
        if (raw.startsWith(QStringLiteral("CiTool exit code ")))
        {
            return T(QStringLiteral("misc.cks.detail.exit_code"),
                QStringLiteral("CiTool 退出码 %1"))
                .arg(raw.mid(QStringLiteral("CiTool exit code ").size()));
        }
        if (raw.startsWith(QStringLiteral("CiTool exit ")))
        {
            return T(QStringLiteral("misc.cks.detail.exit"),
                QStringLiteral("CiTool 退出 %1"))
                .arg(raw.mid(QStringLiteral("CiTool exit ").size()));
        }
        if (raw.startsWith(QStringLiteral("CiTool JSON: ")))
        {
            return T(QStringLiteral("misc.cks.detail.json"),
                QStringLiteral("CiTool JSON：%1"))
                .arg(raw.mid(QStringLiteral("CiTool JSON: ").size()));
        }
        return ks::i18n::LanguageManager::instance().packedSourceText(raw);
    }

    // AddBoundLabel：静态说明文字由 LanguageManager 自动响应语言切换。
    QLabel* AddBoundLabel(QWidget* parent, QVBoxLayout* layout,
        const QString& key, const QString& fallback)
    {
        auto* label = new QLabel(parent); // label：本段流程说明。
        label->setWordWrap(true);
        label->setTextInteractionFlags(Qt::TextSelectableByMouse);
        ks::i18n::LanguageManager::instance().bindText(label, key, fallback);
        layout->addWidget(label);
        return label;
    }

    // AddRoutePage：只建普通子页；参数滚动与执行区由共享工作区统一拥有。
    QVBoxLayout* AddRoutePage(QTabWidget* tabs, const ks::ui::PageNavigationTabDefinition& definition,
        ks::ui::PageNavigationTabCatalog& navigationTabs)
    {
        auto* content = new QWidget(tabs); // content：当前路径的参数正文。
        auto* layout = new QVBoxLayout(content); // layout：保持业务说明与文件选择顺序。
        layout->setContentsMargins(0, 0, 0, 0);
        ks::ui::StyleParameterCanvas(content); // 子页沿用参数卡片色，不再铺页根黑色底板。
        layout->setSpacing(12);
        navigationTabs.add(definition, tabs, content);
        return layout;
    }
}

namespace ks::misc
{
    CksPage::CksPage(QWidget* parent)
        : QWidget(parent)
    {
        for (auto& operation : m_fileHashOperations)
        {
            operation = new ks::ui::AsyncOperation(this);
        }
        m_backendOperation = new ks::ui::AsyncOperation(this);
        initializeUi();
        m_backendOperation->setStateChangedCallback([this]() {
            if (!m_backendOperation->isInFlight() && m_busy)
            {
                m_busy = false;
                showResult(T(QStringLiteral("misc.cks.detail.task_failed"),
                    QStringLiteral("后台任务未返回有效回执，请刷新状态。")), false);
                updateButtons();
            }
        });
        for (int slot = 0; slot < static_cast<int>(m_fileHashOperations.size()); ++slot)
        {
            m_fileHashOperations[slot]->setStateChangedCallback([this, slot]() {
                if (!m_fileHashOperations[slot]->isInFlight() && m_fileHashPending[slot])
                {
                    m_fileHashPending[slot] = false;
                    m_fileDigests[slot].clear();
                    showSelectedFiles();
                }
            });
        }
        updateButtons();
    }

    CksPage::~CksPage()
    {
        for (auto* operation : m_fileHashOperations)
        {
            operation->close();
        }
        // Accepted tool work owns an exit lease; closing the page only revokes GUI delivery.
        // The value worker completes its tool wait, readback and cleanup without borrowing this page.
        m_backendOperation->close();
    }

    void CksPage::showEvent(QShowEvent* event)
    {
        QWidget::showEvent(event);
        refresh();
    }

    void CksPage::changeEvent(QEvent* event)
    {
        QWidget::changeEvent(event);
        if (event != nullptr && event->type() == QEvent::LanguageChange)
        {
            showSnapshot();
            showSelectedFiles();
            if (m_researchPlan != nullptr)
            {
                m_researchPlan->setRawText(CksResearchVerificationTemplate());
            }
            if (m_hasResult) showResult(m_resultRaw, m_resultSuccess);
        }
    }

    void CksPage::initializeUi()
    {
        auto& language = ks::i18n::LanguageManager::instance();
        auto* root = new QVBoxLayout(this); // root：页面固定状态与流程页签。
        ks::ui::StyleWorkspacePage(this, 0);
        root->setSpacing(10);

        // 与系统变速、虚拟定位共用两栏：左侧参数滚动，右侧证据与结果常驻。
        auto* introduction = new QWidget(this); // introduction：固定的操作边界提示。
        auto* introductionLayout = new QVBoxLayout(introduction);
        introductionLayout->setContentsMargins(16, 16, 16, 0);
        root->addWidget(introduction);
        auto* workspace = new ks::ui::ParameterWorkspace(this, 500);
        workspace->useWideParameterLayout(55);
        root->addWidget(workspace, 1);

        AddBoundLabel(introduction, introductionLayout, QStringLiteral("misc.cks.warning"),
            QStringLiteral("CKS 改变内核驱动信任边界。微软路径要求 PK/KEK 所有权、已签名的专用策略及首次启用后的系统重置；SSDE 是旧版实验流程，可能造成无法启动。策略文件存在、许可值为 1 或 CiTool 部署成功，都不等于目标驱动已获准加载。"));

        auto* statusGroup = new QGroupBox(this); // statusGroup：只读状态证据。
        ks::ui::StylePrimaryGroup(statusGroup);
        language.bindText(statusGroup, QStringLiteral("misc.cks.status.title"),
            QStringLiteral("当前系统证据"));
        auto* statusLayout = new QVBoxLayout(statusGroup); // statusLayout：状态与刷新按钮。
        auto* refreshRow = new QHBoxLayout(); // refreshRow：状态标题右侧操作。
        refreshRow->addStretch();
        m_refresh = new QPushButton(QIcon(QStringLiteral(":/Icon/process_refresh.svg")), QString(), statusGroup);
        m_refresh->setObjectName(QStringLiteral("ksCksRefresh"));
        KswordTheme::ApplyCompactIconButtonMetrics(m_refresh);
        language.bindToolTip(m_refresh, QStringLiteral("misc.cks.refresh.tip"),
            QStringLiteral("重新读取许可、策略与服务状态"));
        refreshRow->addWidget(m_refresh);
        statusLayout->addLayout(refreshRow);
        m_status = new QLabel(statusGroup); // m_status：系统与许可状态。
        m_status->setWordWrap(true);
        m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
        statusLayout->addWidget(m_status);
        m_service = new QLabel(statusGroup); // m_service：SSDE 服务状态。
        m_service->setWordWrap(true);
        m_service->setTextInteractionFlags(Qt::TextSelectableByMouse);
        statusLayout->addWidget(m_service);
        ks::ui::StyleParameterSection(statusGroup);
        ks::ui::NormalizeToolbarRow(refreshRow);
        workspace->results()->addWidget(statusGroup);

        auto* routes = new QTabWidget(this); // routes：两种操作流程与免重置研究分别呈现。
        routes->setObjectName(QStringLiteral("ksCksRoutes"));
        ks::ui::StylePageTabsContentFrame(routes);
        workspace->parameters()->addWidget(routes);
        auto* official = AddRoutePage(routes, kCksPageNavigation[0], m_navigationTabs);
        auto* experimental = AddRoutePage(routes, kCksPageNavigation[1], m_navigationTabs);
        auto* research = AddRoutePage(routes, kCksPageNavigation[2], m_navigationTabs);
        m_researchPlan = PopulateCksResearchPage(routes->widget(2), research, workspace);
        workspace->results()->addWidget(m_researchPlan, 1);
        m_researchPlan->hide();

        AddBoundLabel(routes, official, QStringLiteral("misc.cks.official.intro"),
            QStringLiteral("先在受支持的 Windows 11 上准备并审核 App Control 策略：包含 Windows 及现有驱动信任规则、策略更新签名者和自有内核签名者；由 PK/KEK 链中的密钥签名。此页只部署已完成的 .cip，不创建密钥、不扫描磁盘，也不把策略自动改为强制模式。首次启用须按微软文档重置 Windows。"));
        m_policyFile = new QLabel(routes); // 动态路径不绑定固定文本，避免切换语言时覆盖文件名。
        m_policyFile->setWordWrap(true);
        m_policyFile->setTextInteractionFlags(Qt::TextSelectableByMouse);
        m_policyFile->setText(FileLabel(m_policyPath));
        official->addWidget(m_policyFile);
        auto* policyRow = new QHBoxLayout(); // policyRow：选择文件、部署和原文入口。
        auto* choosePolicy = new QPushButton(routes); // choosePolicy：选择既有签名文件。
        language.bindText(choosePolicy, QStringLiteral("misc.cks.official.choose"),
            QStringLiteral("选择已签名 .cip"));
        language.bindToolTip(choosePolicy, QStringLiteral("misc.cks.official.choose.tip"),
            QStringLiteral("选择由 PK/KEK 链中的密钥签名的策略；不会读取私钥"));
        policyRow->addWidget(choosePolicy);
        m_deploy = new QPushButton(routes);
        language.bindText(m_deploy, QStringLiteral("misc.cks.official.deploy"),
            QStringLiteral("部署或更新策略"));
        language.bindToolTip(m_deploy, QStringLiteral("misc.cks.official.deploy.tip"),
            QStringLiteral("通过系统 CiTool 部署；首次启用还需要系统重置"));
        policyRow->addWidget(m_deploy);
        auto* officialDocs = new QPushButton(routes); // officialDocs：微软原文入口。
        language.bindText(officialDocs, QStringLiteral("misc.cks.official.docs"),
            QStringLiteral("微软步骤"));
        language.bindToolTip(officialDocs, QStringLiteral("misc.cks.official.docs.tip"),
            QStringLiteral("打开微软 Custom Kernel Signers 部署文档"));
        policyRow->addWidget(officialDocs);
        policyRow->addStretch();
        official->addLayout(policyRow);
        AddBoundLabel(routes, official, QStringLiteral("misc.cks.official.recovery"),
            QStringLiteral("首次部署后请先验证审核模式、驱动加载和 Code Integrity 事件，再考虑强制模式。已签名策略的删除或错误更新可能使 Windows 无法启动；更新必须由原策略认可的签名者重新签名。此页不提供直接删除 EFI 策略。"));
        m_policyList = new ks::ui::VisibleTableWidget(routes); // 共享宿主维护紧凑复制/导出工具条。
        m_policyList->setColumnCount(4);
        m_policyList->setEditTriggers(QAbstractItemView::NoEditTriggers);
        m_policyList->setSelectionBehavior(QAbstractItemView::SelectRows);
        m_policyList->setSelectionMode(QAbstractItemView::ExtendedSelection);
        ks::ui::ApplyTablePresentation(m_policyList);
        ks::ui::ResultTableCapabilities policyCapabilities; // 只读清单采用紧凑复制/导出操作条。
        policyCapabilities.actionBar = ks::ui::TableActionBarMode::Compact;
        ks::ui::ResultTableHost::ensure(m_policyList)->bindModel(m_policyList->model(),
            {{0, QStringLiteral("cks.name")}, {1, QStringLiteral("cks.id")},
             {2, QStringLiteral("cks.signed")}, {3, QStringLiteral("cks.active")}},
            policyCapabilities);
        m_policyList->setAlternatingRowColors(true);
        m_policyList->setMinimumHeight(150);
        m_policyList->horizontalHeader()->setStretchLastSection(false);
        m_policyList->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
        m_policyList->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
        m_policyList->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
        m_policyList->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
        workspace->results()->addWidget(m_policyList, 1);
        official->addStretch();

        AddBoundLabel(routes, experimental, QStringLiteral("misc.cks.ssde.intro"),
            QStringLiteral("SSDE 使用 Windows Setup 模式临时写入 CKS 产品策略，并可能重启两次。请只使用从上游源码自行构建或已核对哈希的工具。启动前确认已有 PK/KEK、签名策略、系统恢复介质；本页不会关闭 Secure Boot、HVCI 或测试签名。"));
        m_helperFile = new QLabel(routes); // 动态工具路径不绑定固定翻译。
        m_helperFile->setWordWrap(true);
        m_helperFile->setTextInteractionFlags(Qt::TextSelectableByMouse);
        m_helperFile->setText(FileLabel(m_helperPath));
        experimental->addWidget(m_helperFile);
        auto* helperRow = new QHBoxLayout(); // helperRow：旧版启用程序选择与启动。
        auto* chooseHelper = new QPushButton(routes); // chooseHelper：选择用户自行取得的工具。
        language.bindText(chooseHelper, QStringLiteral("misc.cks.ssde.choose_helper"),
            QStringLiteral("选择 ssde_enable.exe"));
        language.bindToolTip(chooseHelper, QStringLiteral("misc.cks.ssde.choose_helper.tip"),
            QStringLiteral("仅选择 SSDE 上游启用程序；页面会显示其 SHA-256"));
        helperRow->addWidget(chooseHelper);
        m_launch = new QPushButton(routes);
        language.bindText(m_launch, QStringLiteral("misc.cks.ssde.launch"),
            QStringLiteral("启动实验性启用流程"));
        language.bindToolTip(m_launch, QStringLiteral("misc.cks.ssde.launch.tip"),
            QStringLiteral("独立控制台启动 SSDE；工具会自行询问是否重启"));
        helperRow->addWidget(m_launch);
        helperRow->addStretch();
        experimental->addLayout(helperRow);

        AddBoundLabel(routes, experimental, QStringLiteral("misc.cks.ssde.persistence"),
            QStringLiteral("重启返回后先刷新状态。只有产品策略、安全启动和 Licensed 都明确开启，才允许安装已由当前系统信任的 ssde.sys。先以按需启动方式安装并验证 RUNNING，再单独设置开机启动；错误的 BOOT_START 驱动可能导致启动循环。"));
        m_driverFile = new QLabel(routes); // 动态驱动路径只由用户选择与语言事件刷新。
        m_driverFile->setWordWrap(true);
        m_driverFile->setTextInteractionFlags(Qt::TextSelectableByMouse);
        m_driverFile->setText(FileLabel(m_driverPath));
        experimental->addWidget(m_driverFile);
        auto* driverRow = new QHBoxLayout(); // driverRow：安装与本次加载动作。
        auto* chooseDriver = new QPushButton(routes); // chooseDriver：选择签名后的 ssde.sys。
        language.bindText(chooseDriver, QStringLiteral("misc.cks.ssde.choose_driver"),
            QStringLiteral("选择已签名 ssde.sys"));
        language.bindToolTip(chooseDriver, QStringLiteral("misc.cks.ssde.choose_driver.tip"),
            QStringLiteral("驱动必须由当前 Windows 信任的证书签名"));
        driverRow->addWidget(chooseDriver);
        m_install = new QPushButton(routes);
        language.bindText(m_install, QStringLiteral("misc.cks.ssde.install"),
            QStringLiteral("安装为按需启动"));
        language.bindToolTip(m_install, QStringLiteral("misc.cks.ssde.install.tip"),
            QStringLiteral("复制到系统驱动目录并创建 ssde 服务；不会覆盖已有文件"));
        driverRow->addWidget(m_install);
        m_start = new QPushButton(routes);
        language.bindText(m_start, QStringLiteral("misc.cks.ssde.start"),
            QStringLiteral("启动并验证驱动"));
        language.bindToolTip(m_start, QStringLiteral("misc.cks.ssde.start.tip"),
            QStringLiteral("通过 SCM 启动 ssde 并等待 RUNNING 回执"));
        driverRow->addWidget(m_start);
        driverRow->addStretch();
        experimental->addLayout(driverRow);
        auto* bootRow = new QHBoxLayout(); // bootRow：持久化开关独立于安装与启动。
        m_enableBoot = new QPushButton(routes);
        language.bindText(m_enableBoot, QStringLiteral("misc.cks.ssde.boot_on"),
            QStringLiteral("开启开机持久化"));
        language.bindToolTip(m_enableBoot, QStringLiteral("misc.cks.ssde.boot_on.tip"),
            QStringLiteral("仅在本次已验证 RUNNING 后设置 BOOT_START；下次启动仍需实机验证"));
        bootRow->addWidget(m_enableBoot);
        m_disableBoot = new QPushButton(routes);
        language.bindText(m_disableBoot, QStringLiteral("misc.cks.ssde.boot_off"),
            QStringLiteral("停用开机持久化"));
        language.bindToolTip(m_disableBoot, QStringLiteral("misc.cks.ssde.boot_off.tip"),
            QStringLiteral("只改为按需启动，不删除驱动、服务或签名策略"));
        bootRow->addWidget(m_disableBoot);
        bootRow->addStretch();
        // bootRow 的动作在构造末尾移入共享固定执行区。
        AddBoundLabel(routes, experimental, QStringLiteral("misc.cks.ssde.alternative"),
            QStringLiteral("issue #10 的人工替代步骤：准备自有 PK 与签名策略后，在 UEFI 暂时关闭 Secure Boot；安装 SSDE 驱动；开启测试签名并重启两次；确认 Licensed=1 且服务 RUNNING；再回 UEFI 开启 Secure Boot。此方法同样需要逐次回读，并保留关闭 Secure Boot 的恢复通道。"));
        auto* ssdeDocs = new QPushButton(routes); // ssdeDocs：上游问题说明入口。
        language.bindText(ssdeDocs, QStringLiteral("misc.cks.ssde.docs"),
            QStringLiteral("SSDE 与 issue #10"));
        language.bindToolTip(ssdeDocs, QStringLiteral("misc.cks.ssde.docs.tip"),
            QStringLiteral("打开 SSDE 上游 issue #10"));
        experimental->addWidget(ssdeDocs, 0, Qt::AlignLeft);
        experimental->addStretch();

        m_result = new QLabel(this); // m_result：最近一次操作的明确回执。
        m_result->setWordWrap(true);
        m_result->setTextInteractionFlags(Qt::TextSelectableByMouse);
        workspace->results()->addWidget(m_result);

        // 主操作移到共享固定执行区，切换路径只切换对应动作组。
        auto* officialActions = new QWidget(workspace);
        auto* officialActionRow = new QHBoxLayout(officialActions);
        officialActionRow->setContentsMargins(0, 0, 0, 0);
        officialActionRow->addWidget(m_deploy);
        officialActionRow->addStretch();
        auto* ssdeActions = new QWidget(workspace);
        auto* ssdeFlow = new ks::ui::FlowLayout(ssdeActions, 0, 8, 8); // 动作按实际宽度换行。
        bootRow->removeWidget(m_enableBoot);
        bootRow->removeWidget(m_disableBoot);
        for (QPushButton* button : {m_launch, m_install, m_start, m_enableBoot, m_disableBoot})
        {
            ks::ui::NormalizeToolbarControl(button);
            ssdeFlow->addWidget(button);
        }
        delete bootRow;
        workspace->actions()->addWidget(officialActions);
        workspace->actions()->addWidget(ssdeActions);
        ssdeActions->hide();
        connect(routes, &QTabWidget::currentChanged, this,
            [this, officialActions, ssdeActions](const int index)
            {
                officialActions->setVisible(index == 0);
                ssdeActions->setVisible(index == 1);
                // 研究资料显示记录模板；原策略清单与操作回执保留对象及数据。
                m_researchPlan->setVisible(index == 2);
                m_policyList->setVisible(index != 2);
                m_result->setVisible(index != 2);
            });

        // 工具行统一行高、间距与按钮主题；参数和结果保留共享画布所有权。
        ks::ui::NormalizeToolbarRow(policyRow);
        ks::ui::NormalizeToolbarRow(helperRow);
        ks::ui::NormalizeToolbarRow(driverRow);
        ks::ui::NormalizeToolbarRow(officialActionRow);
        ks::ui::NormalizeToolbarControl(ssdeDocs);
        workspace->finish();
        connect(m_refresh, &QPushButton::clicked, this, [this]() { refresh(); });
        connect(choosePolicy, &QPushButton::clicked, this, [this]() { chooseFile(QStringLiteral("policy")); });
        connect(chooseHelper, &QPushButton::clicked, this, [this]() { chooseFile(QStringLiteral("helper")); });
        connect(chooseDriver, &QPushButton::clicked, this, [this]() { chooseFile(QStringLiteral("driver")); });
        connect(officialDocs, &QPushButton::clicked, this, []() {
            QDesktopServices::openUrl(QUrl(QStringLiteral("https://learn.microsoft.com/windows/security/application-security/application-control/app-control-for-business/design/custom-kernel-signers")));
        });
        connect(ssdeDocs, &QPushButton::clicked, this, []() {
            QDesktopServices::openUrl(QUrl(QStringLiteral("https://github.com/valinet/ssde/issues/10")));
        });
        connect(m_deploy, &QPushButton::clicked, this, [this]() {
            if (!confirm(T(QStringLiteral("misc.cks.confirm.deploy"),
                QStringLiteral("即将通过 CiTool 部署或更新所选签名策略。错误策略可能阻止系统启动，首次启用还要求系统重置。请确认已验证签名、保存恢复密钥并能进入 UEFI 恢复。继续吗？")))) return;
            const QString path = m_policyPath;
            runOperation([path](QString* detail) { return cks::DeploySignedPolicy(path, detail); });
        });
        connect(m_launch, &QPushButton::clicked, this, [this]() {
            if (!confirm(T(QStringLiteral("misc.cks.confirm.launch"),
                QStringLiteral("SSDE 将接管下一次 Setup 启动并可能重启两次。请确认工具来源与 SHA-256、已备份恢复密钥及 UEFI 恢复方式。继续启动吗？")))) return;
            const QString path = m_helperPath;
            runOperation([path](QString* detail) { return cks::LaunchSsdeEnable(path, detail); });
        });
        connect(m_install, &QPushButton::clicked, this, [this]() {
            if (!confirm(T(QStringLiteral("misc.cks.confirm.install"),
                QStringLiteral("将复制所选 ssde.sys 到系统驱动目录并创建按需启动服务。请确认签名、来源和恢复方式。继续吗？")))) return;
            const QString path = m_driverPath;
            runOperation([path](QString* detail) { return cks::InstallSsdeDriver(path, detail); });
        });
        connect(m_start, &QPushButton::clicked, this, [this]() {
            runOperation([](QString* detail) { return cks::StartSsdeDriver(detail); });
        });
        connect(m_enableBoot, &QPushButton::clicked, this, [this]() {
            if (!confirm(T(QStringLiteral("misc.cks.confirm.boot_on"),
                QStringLiteral("将 ssde 设为 BOOT_START。若下次启动许可或策略失效，系统可能进入启动循环。请确认本次服务已运行且可从 UEFI/恢复环境撤销。继续吗？")))) return;
            runOperation([](QString* detail) { return cks::SetSsdeBootStart(true, detail); });
        });
        connect(m_disableBoot, &QPushButton::clicked, this, [this]() {
            if (!confirm(T(QStringLiteral("misc.cks.confirm.boot_off"),
                QStringLiteral("将 ssde 改为按需启动。下次启动后旧版 CKS 持久化可能失效，依赖自签驱动的服务也可能无法加载。继续吗？")))) return;
            runOperation([](QString* detail) { return cks::SetSsdeBootStart(false, detail); });
        });
        showSnapshot();
    }

    void CksPage::refresh()
    {
        if (m_busy) return;
        m_busy = true;
        updateButtons();
        // QuerySnapshot owns a CiTool process. Use a transaction lease so application exit
        // waits asynchronously for the bounded tool cleanup even after this page closes.
        const auto accepted = m_backendOperation->submitTransaction<int, cks::Snapshot>(0,
            [](const int&) { return cks::QuerySnapshot(); },
            [this](const cks::Snapshot& snapshot, std::uint64_t) {
                m_snapshot = snapshot;
                m_hasSnapshot = true;
                m_busy = false;
                showSnapshot();
                updateButtons();
                return true;
            },
            [this](std::exception_ptr) {
                m_busy = false;
                showResult(T(QStringLiteral("misc.cks.detail.task_failed"),
                    QStringLiteral("后台任务未返回有效回执，请刷新状态。")), false);
                updateButtons();
            });
        if (accepted == 0)
        {
            m_busy = false;
            showResult(T(QStringLiteral("misc.cks.detail.task_failed"),
                QStringLiteral("后台任务未返回有效回执，请刷新状态。")), false);
            updateButtons();
        }
    }

    void CksPage::showSnapshot()
    {
        if (!m_status || !m_service || !m_policyList)
        {
            return;
        }
        m_policyList->setHorizontalHeaderLabels({
            T(QStringLiteral("misc.cks.policy.name"), QStringLiteral("策略名称")),
            T(QStringLiteral("misc.cks.policy.id"), QStringLiteral("策略 ID")),
            T(QStringLiteral("misc.cks.policy.signed"), QStringLiteral("签名")),
            T(QStringLiteral("misc.cks.policy.enforced"), QStringLiteral("当前生效"))});
        m_policyList->clearContents();
        m_policyList->setRowCount(0);
        if (!m_hasSnapshot)
        {
            m_status->setText(T(QStringLiteral("misc.cks.status.loading"),
                QStringLiteral("尚未读取系统状态")));
            m_service->clear();
            return;
        }

        // 五个值各自标记来源，避免将“许可=1”推断为实际驱动可用。
        m_status->setText(T(QStringLiteral("misc.cks.status.format"),
            QStringLiteral("Windows build：%1   已提升：%2\n安全启动：%3   HVCI 配置：%4\nCKS 产品策略：%5   CI Licensed：%6"))
            .arg(m_snapshot.build)
            .arg(m_snapshot.elevated
                ? T(QStringLiteral("misc.cks.state.yes"), QStringLiteral("是"))
                : T(QStringLiteral("misc.cks.state.no"), QStringLiteral("否")))
            .arg(CheckText(m_snapshot.secureBoot))
            .arg(CheckText(m_snapshot.hvci))
            .arg(CheckText(m_snapshot.productPolicy))
            .arg(CheckText(m_snapshot.licensed)));
        if (m_snapshot.ssde.queryFailed)
        {
            m_service->setText(T(QStringLiteral("misc.cks.service.error"),
                QStringLiteral("SSDE 服务查询失败：%1"))
                .arg(m_snapshot.ssde.errorText));
        }
        else if (!m_snapshot.ssde.installed)
        {
            m_service->setText(T(QStringLiteral("misc.cks.service.missing"),
                QStringLiteral("SSDE 服务：未安装")));
        }
        else
        {
            m_service->setText(T(QStringLiteral("misc.cks.service.format"),
                QStringLiteral("SSDE 服务：%1   开机启动：%2\n镜像：%3"))
                .arg(m_snapshot.ssde.running
                    ? T(QStringLiteral("misc.cks.service.running"), QStringLiteral("RUNNING"))
                    : T(QStringLiteral("misc.cks.service.stopped"), QStringLiteral("未运行")))
                .arg(m_snapshot.ssde.bootStart
                    ? T(QStringLiteral("misc.cks.state.yes"), QStringLiteral("是"))
                    : T(QStringLiteral("misc.cks.state.no"), QStringLiteral("否")))
                .arg(m_snapshot.ssde.imagePath));
        }
        for (const cks::Policy& policy : m_snapshot.policies)
        {
            const int row = m_policyList->rowCount(); // row：本条只读回执的行号。
            m_policyList->insertRow(row);
            auto setCell = [this, row](int column, const QString& text)
            {
                m_policyList->setItem(row, column, new QTableWidgetItem(text));
            };
            setCell(0, policy.name);
            setCell(1, policy.id);
            setCell(2, !policy.signedKnown
                ? T(QStringLiteral("misc.cks.state.not_reported"), QStringLiteral("未报告"))
                : policy.signedPolicy
                    ? T(QStringLiteral("misc.cks.state.yes"), QStringLiteral("是"))
                    : T(QStringLiteral("misc.cks.state.no"), QStringLiteral("否")));
            setCell(3, !policy.enforcedKnown
                ? T(QStringLiteral("misc.cks.state.not_reported"), QStringLiteral("未报告"))
                : policy.enforced
                    ? T(QStringLiteral("misc.cks.state.yes"), QStringLiteral("是"))
                    : T(QStringLiteral("misc.cks.state.no"), QStringLiteral("否")));
        }
        if (m_snapshot.policies.isEmpty() && m_snapshot.policyError.isEmpty())
        {
            m_policyList->insertRow(0);
            auto setCell = [this](int column, const QString& text)
            {
                m_policyList->setItem(0, column, new QTableWidgetItem(text));
            };
            setCell(0, T(QStringLiteral("misc.cks.policy.empty"),
                QStringLiteral("CiTool 未报告策略")));
        }
        if (!m_snapshot.policyError.isEmpty())
        {
            m_policyList->insertRow(0);
            auto setCell = [this](int column, const QString& text)
            {
                m_policyList->setItem(0, column, new QTableWidgetItem(text));
            };
            setCell(0, T(QStringLiteral("misc.cks.policy.error"),
                QStringLiteral("CiTool 策略查询失败")));
            setCell(1, DetailText(m_snapshot.policyError));
        }
    }

    void CksPage::updateButtons()
    {
        if (!m_refresh || !m_deploy || !m_launch || !m_install || !m_start
            || !m_enableBoot || !m_disableBoot)
        {
            return;
        }
        const bool ready = m_hasSnapshot && !m_busy && m_snapshot.elevated;
        const bool licensed = ready
            && m_snapshot.secureBoot.known && m_snapshot.secureBoot.enabled
            && m_snapshot.productPolicy.known && m_snapshot.productPolicy.enabled
            && m_snapshot.licensed.known && m_snapshot.licensed.enabled;
        m_refresh->setEnabled(!m_busy);
        m_deploy->setEnabled(ready && m_snapshot.build >= 26100U
            && m_snapshot.secureBoot.known && m_snapshot.secureBoot.enabled
            && !m_policyPath.isEmpty());
        m_launch->setEnabled(ready && !m_helperPath.isEmpty());
        m_install->setEnabled(licensed && !m_snapshot.ssde.installed
            && !m_snapshot.ssde.queryFailed && !m_driverPath.isEmpty());
        m_start->setEnabled(licensed && m_snapshot.ssde.installed
            && m_snapshot.ssde.expectedImage && !m_snapshot.ssde.running);
        m_enableBoot->setEnabled(licensed && m_snapshot.ssde.installed
            && m_snapshot.ssde.expectedImage && m_snapshot.ssde.running
            && !m_snapshot.ssde.bootStart);
        m_disableBoot->setEnabled(ready && m_snapshot.ssde.installed
            && m_snapshot.ssde.expectedImage && m_snapshot.ssde.bootStart);
    }

    void CksPage::chooseFile(const QString& kind)
    {
        if (m_busy)
        {
            return;
        }
        const bool policy = kind == QStringLiteral("policy");
        const bool helper = kind == QStringLiteral("helper");
        const QString filter = policy ? QStringLiteral("CI policy (*.cip)")
            : helper ? QStringLiteral("SSDE helper (*.exe)")
            : QStringLiteral("SSDE driver (*.sys)");
        const QString path = QFileDialog::getOpenFileName(this,
            T(QStringLiteral("misc.cks.file.choose"), QStringLiteral("选择文件")),
            QString(), filter);
        if (path.isEmpty())
        {
            return;
        }
        const QFileInfo file(path); // file：拒绝已丢失或目录路径。
        if (!file.isFile() || file.isSymLink())
        {
            showResult(T(QStringLiteral("misc.cks.file.invalid"),
                QStringLiteral("所选文件无效或是符号链接。")), false);
            return;
        }
        if (policy)
        {
            m_policyPath = file.absoluteFilePath();
            queueFileHash(0, m_policyPath);
        }
        else if (helper)
        {
            m_helperPath = file.absoluteFilePath();
            queueFileHash(1, m_helperPath);
        }
        else
        {
            m_driverPath = file.absoluteFilePath();
            queueFileHash(2, m_driverPath);
        }
        updateButtons();
    }

    void CksPage::showSelectedFiles()
    {
        m_policyFile->setText(FileLabel(m_policyPath, m_fileDigests[0], m_fileHashPending[0]));
        m_helperFile->setText(FileLabel(m_helperPath, m_fileDigests[1], m_fileHashPending[1]));
        m_driverFile->setText(FileLabel(m_driverPath, m_fileDigests[2], m_fileHashPending[2]));
    }

    void CksPage::queueFileHash(const int slot, const QString& path)
    {
        m_fileDigests[slot].clear();
        m_fileHashPending[slot] = true;
        showSelectedFiles();
        m_fileHashOperations[slot]->submit<QString, QByteArray>(path,
            [](const QString& selectedPath, const ks::ui::AsyncOperationToken& token) {
                if (token.isCanceled()) return QByteArray();
                QFile file(selectedPath);
                if (!file.open(QIODevice::ReadOnly)) return QByteArray();
                const QFileInfo before(file);
                const qint64 expectedSize = before.size();
                const QDateTime expectedModified = before.lastModified();
                QCryptographicHash hash(QCryptographicHash::Sha256);
                qint64 bytesRead = 0;
                while (!file.atEnd())
                {
                    if (token.isCanceled()) return QByteArray();
                    const QByteArray chunk = file.read(1024 * 1024);
                    if (chunk.isEmpty()) return QByteArray();
                    bytesRead += chunk.size();
                    hash.addData(chunk);
                }
                const QFileInfo after(file);
                if (file.error() != QFileDevice::NoError || bytesRead != expectedSize
                    || after.size() != expectedSize || after.lastModified() != expectedModified)
                {
                    return QByteArray();
                }
                return hash.result();
            },
            [this, slot](const QByteArray& digest, std::uint64_t) {
                m_fileDigests[slot] = digest;
                m_fileHashPending[slot] = false;
                showSelectedFiles();
                return true;
            },
            [this, slot](std::exception_ptr) {
                m_fileDigests[slot].clear();
                m_fileHashPending[slot] = false;
                showSelectedFiles();
            });
    }

    void CksPage::runOperation(const std::function<bool(QString*)>& operation)
    {
        if (m_busy) return;
        m_busy = true;
        updateButtons();
        using Operation = std::function<bool(QString*)>;
        using Receipt = std::pair<std::pair<bool, QString>, cks::Snapshot>;
        // Existing callers capture only their frozen path or no state. Once accepted,
        // the operation and its final system readback complete regardless of page lifetime.
        const auto accepted = m_backendOperation->submitTransaction<Operation, Receipt>(operation,
            [](const Operation& execute) {
                QString detail;
                const bool success = execute(&detail);
                return Receipt{{success, std::move(detail)}, cks::QuerySnapshot()};
            },
            [this](const Receipt& receipt, std::uint64_t) {
                m_snapshot = receipt.second;
                m_hasSnapshot = true;
                m_busy = false;
                showResult(receipt.first.second, receipt.first.first);
                showSnapshot();
                updateButtons();
                return true;
            },
            [this](std::exception_ptr) {
                m_busy = false;
                showResult(T(QStringLiteral("misc.cks.detail.task_failed"),
                    QStringLiteral("后台任务未返回有效回执，请刷新状态。")), false);
                updateButtons();
            });
        if (accepted == 0)
        {
            m_busy = false;
            showResult(T(QStringLiteral("misc.cks.detail.task_failed"),
                QStringLiteral("后台任务未返回有效回执，请刷新状态。")), false);
            updateButtons();
        }
    }

    bool CksPage::confirm(const QString& text)
    {
        QMessageBox dialog(this); // dialog：有不透明背景且默认取消的风险确认。
        dialog.setObjectName(QStringLiteral("ksCksConfirm"));
        // 普通确认框由主程序已安装的 ThemedMessageBox 统一维护主题和按钮。
        dialog.setIcon(QMessageBox::Warning);
        dialog.setWindowTitle(T(QStringLiteral("misc.cks.confirm.title"),
            QStringLiteral("确认 CKS 操作")));
        dialog.setText(text);
        dialog.setStandardButtons(QMessageBox::Yes | QMessageBox::Cancel);
        dialog.setDefaultButton(QMessageBox::Cancel);
        return dialog.exec() == QMessageBox::Yes;
    }

    void CksPage::showResult(const QString& text, const bool success)
    {
        m_resultRaw = text;
        m_resultSuccess = success;
        m_hasResult = true;
        m_result->setText((success
            ? T(QStringLiteral("misc.cks.result.success"), QStringLiteral("操作已提交：%1"))
            : T(QStringLiteral("misc.cks.result.failed"), QStringLiteral("操作未完成：%1")))
            .arg(DetailText(text)));
    }
}

ks::ui::PageNavigationCatalog ks::misc::CksPage::navigationCatalog(const CksPage* existing)
{
    ks::ui::PageNavigationCatalog schema;
    for (const auto& definition : kCksPageNavigation)
        schema.append(ks::ui::PageNavigationTabDescriptor(definition));
    return existing ? existing->m_navigationTabs.describe(std::move(schema)) : schema;
}
QWidget* ks::misc::CksPage::activateNavigationDescriptor(const QString& id)
{
    return m_navigationTabs.activate(id, navigationCatalog(this));
}
namespace { const bool kCksPageNavigationRegistered =
    ks::ui::RegisterTypedPageNavigationCatalog<ks::misc::CksPage>(QStringLiteral("misc.cks")); }
