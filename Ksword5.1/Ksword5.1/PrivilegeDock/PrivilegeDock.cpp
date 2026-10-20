#include "PrivilegeDock.h"
namespace {
const ks::ui::PageNavigationTabDefinition kPrivilegeNavigation[] = {
    {"privilege.tab.accounts", "", "privilege.tab.accounts", "账号", true},
    {"privilege.workbench.tab.account_manager", "privilege.tab.accounts", "privilege.workbench.tab.account_manager", "账号管理", true},
    {"privilege.workbench.tab.account_tools", "privilege.tab.accounts", "privilege.workbench.tab.account_tools", "创建与重置", false},
    {"privilege.tab.permissions", "", "privilege.tab.permissions", "权限", false},
    {"privilege.workbench.tab.groups", "", "privilege.workbench.tab.groups", "用户组", false},
    {"privilege.workbench.tab.rights", "", "privilege.workbench.tab.rights", "权限分配", false},
    {"privilege.workbench.tab.tokens", "", "privilege.workbench.tab.tokens", "令牌对比", false},
    {"privilege.workbench.tab.access", "", "privilege.workbench.tab.access", "访问诊断", false},
    {"privilege.workbench.tab.sessions", "", "privilege.workbench.tab.sessions", "登录会话", false},
    {"privilege.workbench.tab.launch", "", "privilege.workbench.tab.launch", "指定身份启动", false},
    {"privilege.workbench.tab.snapshots", "", "privilege.workbench.tab.snapshots", "权限快照", false}
};
}

#include "../UI/TableContextMenu.h"
#include "../UI/FlatTableModel.h"
#include "../UI/AsyncOperation.h"
#include "../UI/TablePresentation.h"
#include <QSortFilterProxyModel>
#include <QAbstractProxyModel>
#include <QItemSelectionModel>
#include <QShowEvent>
#include "../UI/PageControlStyle.h"
#include "../UI/ToolbarMetrics.h"
#include "../UI/ParameterWorkspace.h"
#include "PrivilegeAccountPages.h"
#include "PrivilegeTokenPages.h"
#include "PrivilegeAccessPage.h"
#include "PrivilegeSnapshotPage.h"
#include "../UI/VisibleTableWidget.h"
#include "../UI/TableInteractionSupport.h"
#include "../Internationalization/LanguageManager.h"
#include "../Framework/PrivilegeElevationPrompt.h"

// ============================================================
// PrivilegeDock.cpp
// 作用：
// 1) 实现本地账号管理（创建用户、重置密码）；
// 2) 枚举用户/组/当前进程权限，用于权限审计；
// 3) 所有敏感操作均要求二次确认并写入日志。
// ============================================================

#include "../theme.h"

#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QEvent>
#include <QFormLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QModelIndex>
#include <QPushButton>
#include <QPointer>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <string>
#include <vector>
#include <functional>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <Lm.h>

#pragma comment(lib, "Netapi32.lib")

namespace
{
    QString privilegeText(const char* const key, const QString& sourceText)
    {
        return ks::i18n::contextText(QString::fromLatin1(key), sourceText);
    }

    std::string privilegeLogText(const char* const key, const QString& sourceText)
    {
        return privilegeText(key, sourceText).toStdString();
    }

    // blueButtonStyle 作用：
    // - 统一按钮风格，保证权限页视觉与其他 Dock 一致。
    QString blueButtonStyle()
    {
        return KswordTheme::ThemedButtonStyle();
    }

    // blueInputStyle 作用：
    // - 账号表单使用可辨认的中性实底，边界只在焦点时强调。
    QString blueInputStyle()
    {
        return QStringLiteral(
            "QLineEdit{border:1px solid %2;border-radius:5px;background:%3;color:%4;padding:2px 8px;}"
            "QLineEdit:focus{border:1px solid %1;}")
            .arg(KswordTheme::PrimaryBlueHex)
            .arg(KswordTheme::BorderHex())
            .arg(KswordTheme::SurfaceAltHex())
            .arg(KswordTheme::TextPrimaryHex());
    }

    // boolText 作用：
    // - 把布尔状态统一转换成“是/否”中文文本。
    QString boolText(const bool value)
    {
        return value
            ? privilegeText("privilege.bool.yes", QStringLiteral("是"))
            : privilegeText("privilege.bool.no", QStringLiteral("否"));
    }

    QString privilegeTableMenuStyle()
    {
        // 输入：无。
        // 处理：生成不透明菜单样式，保持账号/权限表右键菜单在深浅色主题下可读。
        // 返回：QMenu 样式表文本。
        return QStringLiteral(
            "QMenu{background:%1;color:%2;border:1px solid %3;}"
            "QMenu::item{padding:5px 24px 5px 24px;background:transparent;}"
            "QMenu::item:selected{background:%4;color:%6;}"
            "QMenu::item:disabled{color:%5;}")
            .arg(KswordTheme::SurfaceHex())
            .arg(KswordTheme::TextPrimaryHex())
            .arg(KswordTheme::BorderHex())
            .arg(KswordTheme::AccentHex(KswordTheme::AccentRole::Blue))
            .arg(KswordTheme::TextSecondaryHex())
            .arg(KswordTheme::OnAccentDynamicHex());
    }

    void copyPrivilegeTableCurrentRow(QTableView* table)
    {
        // 输入：账号表或权限表。
        // 处理：读取当前显示行的所有列，按 TSV 复制到剪贴板。
        // 返回：无返回值；表格/剪贴板不可用或无当前行时不做动作。
        if (table == nullptr || QApplication::clipboard() == nullptr)
        {
            return;
        }

        const int rowIndex = table->currentIndex().row();
        if (rowIndex < 0 || rowIndex >= table->model()->rowCount())
        {
            return;
        }

        QStringList fields;
        fields.reserve(table->model()->columnCount());
        for (int columnIndex = 0; columnIndex < table->model()->columnCount(); ++columnIndex)
        {
            fields.push_back(table->model()->index(rowIndex, columnIndex).data().toString());
        }
        QApplication::clipboard()->setText(fields.join(QChar('\t')));
    }

    void installPrivilegeTableCopyMenu(QTableView* table)
    {
        // 输入：需要复制当前行能力的账号/权限表。
        // 处理：安装只读右键菜单；右键某行时先选中对应单元格。
        // 返回：无返回值；不会触发账号创建、密码重置或权限调整。
        if (table == nullptr)
        {
            return;
        }

        table->setContextMenuPolicy(Qt::CustomContextMenu);
        QObject::connect(table, &QTableView::customContextMenuRequested, table, [table](const QPoint& localPosition) {
            const QModelIndex clickedIndex = table->indexAt(localPosition);
            if (clickedIndex.isValid())
            {
                table->setCurrentIndex(clickedIndex);
            }

            QMenu menu(table);
            menu.setStyleSheet(privilegeTableMenuStyle());
            QAction* copyRowAction = menu.addAction(privilegeText(
                "privilege.menu.copy_row", QStringLiteral("复制当前行")));
            // 声明业务菜单已提供此能力，避免共享表格菜单追加重复项。
            ks::ui::SetTableContextActionRole(copyRowAction, ks::ui::TableContextActionRole::CopyRows);
            copyRowAction->setEnabled(table->currentIndex().row() >= 0);
            if (menu.exec(table->viewport()->mapToGlobal(localPosition)) == copyRowAction)
            {
                copyPrivilegeTableCurrentRow(table);
            }
        });
    }

    class LegacyHeaderProxy final : public QSortFilterProxyModel
    {
    public:
        LegacyHeaderProxy(std::function<QString(int)> header, QObject* parent)
            : QSortFilterProxyModel(parent), m_header(std::move(header)) {}
        QVariant data(const QModelIndex& index, int role) const override
        {
            // Business strings and explicitly localized cells must bypass source-text translation.
            return QSortFilterProxyModel::data(index,
                role == Qt::DisplayRole || role == Qt::ToolTipRole ? Qt::UserRole + 1 : role);
        }
        QVariant headerData(int section, Qt::Orientation orientation, int role) const override
        {
            if (orientation == Qt::Horizontal && role == Qt::DisplayRole) return m_header(section);
            return QSortFilterProxyModel::headerData(section, orientation, role);
        }
        void retranslate() { emit headerDataChanged(Qt::Horizontal, 0, 3); }
    private:
        std::function<QString(int)> m_header;
    };
    // A factory runs only when its page is first selected. The placeholder owns its content.
    QWidget* addWorkbenchPage(QTabWidget* tabs, const ks::ui::PageNavigationTabDefinition& definition,
        ks::ui::PageNavigationTabCatalog& navigationTabs, std::function<QWidget*(QWidget*)> factory)
    {
        auto* holder = new QWidget(tabs);
        // 惰性壳不再叠加真实页边距，由外层导航框架管理同一份等边留白。
        holder->setProperty("ksword_page_frame_host", true);
        holder->setObjectName(QString::fromLatin1(definition.id));
        auto* layout = new QVBoxLayout(holder);
        layout->setContentsMargins(0, 0, 0, 0);
        holder->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
        navigationTabs.add(definition, tabs, holder);
        QObject::connect(tabs, &QTabWidget::currentChanged, holder, [tabs, holder, layout, factory](int index) {
            if (tabs->widget(index) != holder || holder->property("ks_privilege_loaded").toBool()) return;
            holder->setProperty("ks_privilege_loaded", true);
            QWidget* content = factory(holder);
            holder->setProperty("ks_privilege_content", QVariant::fromValue(static_cast<QObject*>(content)));
            layout->addWidget(content);
        });
        return holder;
    }
}

PrivilegeDock::PrivilegeDock(QWidget* parent)
    : QWidget(parent)
{
    kLogEvent event;
    info << event
        << privilegeLogText("privilege.log.construct.start", QStringLiteral("[PrivilegeDock] 构造开始。"))
        << eol;

    initializeUi();
    initializeConnections();

    info << event
        << privilegeLogText("privilege.log.construct.completed", QStringLiteral("[PrivilegeDock] 构造完成。"))
        << eol;
}

PrivilegeDock::~PrivilegeDock()
{
    if (m_readOperation) m_readOperation->close();
    if (m_prepareOperation) m_prepareOperation->close();
    if (m_mutationOperation) m_mutationOperation->close();
}

void PrivilegeDock::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    QTimer::singleShot(0, this, [this]() { ensureVisibleLegacyData(); });
}

void PrivilegeDock::changeEvent(QEvent* event)
{
    QWidget::changeEvent(event);
    if (event == nullptr || event->type() != QEvent::LanguageChange) return;
    applyTranslatedHeaders();
    // Models contain raw values. Translation invalidates presentation only, never queries NetAPI/token.
    refreshLocalUserTable();
    refreshPermissionTable();
    updateStatusLabels();
}

void PrivilegeDock::initializeUi()
{
    m_rootLayout = new QVBoxLayout(this);
    m_rootLayout->setContentsMargins(0, 0, 0, 0); // 分类导航与内容框架分别负责自身几何。
    m_rootLayout->setSpacing(6);

    m_tabWidget = new QTabWidget(this);
    m_tabWidget->setTabPosition(QTabWidget::West);
    // 侧向主Tab负责方形工作区；纯账户子Tab由共享层沿单一承载关系继承。
    ks::ui::StyleSquarePageTabsFrame(m_tabWidget);
    m_rootLayout->addWidget(m_tabWidget, 1);

    initializeAccountTab();
    // Keep existing create/reset tools together with the full account manager.
    // Account工具先构建，最终只由accounts父Tab登记一次。
    auto* accounts = new QWidget(m_tabWidget);
    auto* accountLayout = new QVBoxLayout(accounts);
    accountLayout->setContentsMargins(0, 0, 0, 0);
    auto* accountTabs = new QTabWidget(accounts);
    m_accountTabs = accountTabs;
    ks::ui::StylePageTabsContentFrame(accountTabs);
    accountLayout->addWidget(accountTabs);
    auto* manager = ks::privilege::createAccountManagementPage(accountTabs,
        [this](const QString& target, const QString& account) {
            const QString name = QStringLiteral("privilege.workbench.tab.") + target;
            QWidget* holder = m_tabWidget->findChild<QWidget*>(name, Qt::FindDirectChildrenOnly);
            // QTabWidget reparents pages to its stacked widget; select by stable object name.
            if (!holder)
                for (int index = 0; index < m_tabWidget->count(); ++index)
                    if (m_tabWidget->widget(index)->objectName() == name) { holder = m_tabWidget->widget(index); break; }
            if (!holder) return;
            m_tabWidget->setCurrentWidget(holder);
            QWidget* content = qobject_cast<QWidget*>(holder->property("ks_privilege_content").value<QObject*>());
            if (target == QLatin1String("groups")) ks::privilege::selectGroupAccount(content, account);
            else if (target == QLatin1String("rights")) ks::privilege::selectRightsAccount(content, account);
            else if (target == QLatin1String("sessions")) ks::privilege::focusSessionsAccount(content, account);
        });
    m_navigationTabs.add(kPrivilegeNavigation[1], accountTabs, manager);
    m_navigationTabs.add(kPrivilegeNavigation[2], accountTabs, m_accountPage);
    m_navigationTabs.add(kPrivilegeNavigation[0], m_tabWidget, accounts);
    initializePermissionTab();
    addWorkbenchPage(m_tabWidget, kPrivilegeNavigation[4], m_navigationTabs, ks::privilege::createGroupsPage);
    addWorkbenchPage(m_tabWidget, kPrivilegeNavigation[5], m_navigationTabs, ks::privilege::createRightsPage);
    addWorkbenchPage(m_tabWidget, kPrivilegeNavigation[6], m_navigationTabs, ks::privilege::createTokenComparePage);
    addWorkbenchPage(m_tabWidget, kPrivilegeNavigation[7], m_navigationTabs, ks::privilege::createAccessDiagnosticPage);
    addWorkbenchPage(m_tabWidget, kPrivilegeNavigation[8], m_navigationTabs, [](QWidget* parent) {
        return ks::privilege::createSessionsPage(parent, [](DWORD pid, quint64 creationTime) {
            ks::ui::OpenProcessDetailByIdentity(pid, creationTime);
        });
    });
    addWorkbenchPage(m_tabWidget, kPrivilegeNavigation[9], m_navigationTabs, ks::privilege::createIdentityLaunchPage);
    addWorkbenchPage(m_tabWidget, kPrivilegeNavigation[10], m_navigationTabs, ks::privilege::createPermissionSnapshotPage);
}

void PrivilegeDock::initializeAccountTab()
{
    ks::i18n::LanguageManager& languageManager = ks::i18n::LanguageManager::instance();
    m_accountPage = new QWidget(m_tabWidget);
    m_accountLayout = new QVBoxLayout(m_accountPage);
    m_accountLayout->setContentsMargins(4, 4, 4, 4);
    m_accountLayout->setSpacing(6);
    // 两个账号操作独立成组并各带原动作；右侧账号表只辅助选取目标。
    auto* workspace = new ks::ui::ParameterWorkspace(m_accountPage, 420);
    m_accountLayout->addWidget(workspace);

    m_accountToolbarLayout = new QHBoxLayout();
    m_accountToolbarLayout->setContentsMargins(0, 0, 0, 0);
    m_accountToolbarLayout->setSpacing(6);

    m_accountRefreshButton = new QPushButton(QIcon(":/Icon/process_refresh.svg"), QString(), m_accountPage);
    languageManager.bindToolTip(
        m_accountRefreshButton,
        QStringLiteral("privilege.account.refresh.tooltip"),
        QStringLiteral("刷新本地账号列表"));
    m_accountRefreshButton->setStyleSheet(blueButtonStyle());
    m_accountRefreshButton->setFixedWidth(34);

    m_accountStatusLabel = new QLabel(privilegeText(
        "privilege.account.status.pending", QStringLiteral("状态：待刷新")), m_accountPage);
    m_accountToolbarLayout->addWidget(m_accountRefreshButton, 0);
    m_accountToolbarLayout->addWidget(m_accountStatusLabel, 1);
    ks::ui::NormalizeToolbarRow(m_accountToolbarLayout);
    workspace->results()->addLayout(m_accountToolbarLayout, 0);

    m_accountTable = new ks::ui::TableActionTableView(m_accountPage);
    ks::ui::SetTableActionBarMode(m_accountTable, ks::ui::TableActionBarMode::Compact);
    using AccountModel = ks::ui::FlatTableModel<ks::privilege::legacy::LocalUser>;
    m_accountModel = new AccountModel({{QStringLiteral("用户名")},{QStringLiteral("全名")},
        {QStringLiteral("已禁用")},{QStringLiteral("最后登录")}},
        [this](const ks::privilege::legacy::LocalUser& entry, int column, int role) -> QVariant
        {
            return accountCell(entry, column, role);
        }, m_accountTable);
    m_accountProxy = new LegacyHeaderProxy([](int column) -> QString
    {
        switch (column)
        {
        case 0: return privilegeText("privilege.account.header.username", QStringLiteral("用户名"));
        case 1: return privilegeText("privilege.account.header.full_name", QStringLiteral("全名"));
        case 2: return privilegeText("privilege.account.header.disabled", QStringLiteral("已禁用"));
        case 3: return privilegeText("privilege.account.header.last_logon", QStringLiteral("最后登录"));
        default: return {};
        }
    }, m_accountTable);
    m_accountProxy->setSourceModel(m_accountModel);
    m_accountProxy->setSortRole(Qt::UserRole + 1);
    m_accountProxy->setDynamicSortFilter(false);
    m_accountTable->setModel(m_accountProxy);
    m_accountTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_accountTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_accountTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_accountTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    m_accountTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_accountTable->setSortingEnabled(true);
    m_accountTable->sortByColumn(-1, Qt::AscendingOrder);
    ks::ui::ApplyTablePresentation(m_accountTable);
    installPrivilegeTableCopyMenu(m_accountTable);
    workspace->results()->addWidget(m_accountTable, 1);

    // 新建用户输入区：包含用户名、密码、确认密码。
    auto* createGroup = new QGroupBox(m_accountPage);
    languageManager.bindText(createGroup, QStringLiteral("privilege.dialog.create.title"), QStringLiteral("创建用户"));
    auto* createSection = new QVBoxLayout(createGroup);
    ks::ui::StyleParameterSection(createGroup);
    QFormLayout* createLayout = new QFormLayout();
    createLayout->setRowWrapPolicy(QFormLayout::WrapAllRows);
    createLayout->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    // 组内保持紧凑，两个不同账号动作之间额外留白，避免误读为连续步骤。
    createLayout->setVerticalSpacing(8);
    createLayout->setHorizontalSpacing(12);
    m_createUserNameEdit = new QLineEdit(m_accountPage);
    m_createPasswordEdit = new QLineEdit(m_accountPage);
    m_createPasswordConfirmEdit = new QLineEdit(m_accountPage);
    m_createPasswordEdit->setEchoMode(QLineEdit::Password);
    m_createPasswordConfirmEdit->setEchoMode(QLineEdit::Password);
    languageManager.bindPlaceholder(
        m_createUserNameEdit,
        QStringLiteral("privilege.account.create.username.placeholder"),
        QStringLiteral("输入新用户名"));
    languageManager.bindPlaceholder(
        m_createPasswordEdit,
        QStringLiteral("privilege.account.create.password.placeholder"),
        QStringLiteral("输入密码"));
    languageManager.bindPlaceholder(
        m_createPasswordConfirmEdit,
        QStringLiteral("privilege.account.create.confirm_password.placeholder"),
        QStringLiteral("再次输入密码"));
    m_createUserNameEdit->setStyleSheet(blueInputStyle());
    m_createPasswordEdit->setStyleSheet(blueInputStyle());
    m_createPasswordConfirmEdit->setStyleSheet(blueInputStyle());
    QLabel* createUserLabel = new QLabel(QStringLiteral("新用户"), m_accountPage);
    QLabel* createPasswordLabel = new QLabel(QStringLiteral("密码"), m_accountPage);
    QLabel* createConfirmPasswordLabel = new QLabel(QStringLiteral("确认密码"), m_accountPage);
    languageManager.bindText(createUserLabel, QStringLiteral("privilege.account.create.username.label"), QStringLiteral("新用户"));
    languageManager.bindText(createPasswordLabel, QStringLiteral("privilege.account.create.password.label"), QStringLiteral("密码"));
    languageManager.bindText(createConfirmPasswordLabel, QStringLiteral("privilege.account.create.confirm_password.label"), QStringLiteral("确认密码"));
    createLayout->addRow(createUserLabel, m_createUserNameEdit);
    createLayout->addRow(createPasswordLabel, m_createPasswordEdit);
    createLayout->addRow(createConfirmPasswordLabel, m_createPasswordConfirmEdit);
    createSection->addLayout(createLayout);

    m_createUserButton = new QPushButton(QIcon(":/Icon/process_start.svg"), QString(), m_accountPage);
    languageManager.bindToolTip(
        m_createUserButton,
        QStringLiteral("privilege.account.create.tooltip"),
        QStringLiteral("创建本地用户（会弹出二次确认）"));
    m_createUserButton->setStyleSheet(blueButtonStyle());
    m_createUserButton->setFixedWidth(34);
    ks::ui::NormalizeToolbarControl(m_createUserButton);
    createSection->addWidget(m_createUserButton, 0, Qt::AlignLeft);
    workspace->parameters()->addWidget(createGroup);

    // 重置密码输入区：支持指定账号并确认密码。
    auto* resetGroup = new QGroupBox(m_accountPage);
    languageManager.bindText(resetGroup, QStringLiteral("privilege.dialog.reset.title"), QStringLiteral("重置密码"));
    auto* resetSection = new QVBoxLayout(resetGroup);
    ks::ui::StyleParameterSection(resetGroup);
    QFormLayout* resetLayout = new QFormLayout();
    resetLayout->setRowWrapPolicy(QFormLayout::WrapAllRows);
    resetLayout->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    resetLayout->setVerticalSpacing(8);
    resetLayout->setHorizontalSpacing(12);
    m_resetUserNameEdit = new QLineEdit(m_accountPage);
    m_resetPasswordEdit = new QLineEdit(m_accountPage);
    m_resetPasswordConfirmEdit = new QLineEdit(m_accountPage);
    m_resetPasswordEdit->setEchoMode(QLineEdit::Password);
    m_resetPasswordConfirmEdit->setEchoMode(QLineEdit::Password);
    languageManager.bindPlaceholder(
        m_resetUserNameEdit,
        QStringLiteral("privilege.account.reset.username.placeholder"),
        QStringLiteral("输入要重置密码的用户名"));
    languageManager.bindPlaceholder(
        m_resetPasswordEdit,
        QStringLiteral("privilege.account.reset.password.placeholder"),
        QStringLiteral("输入新密码"));
    languageManager.bindPlaceholder(
        m_resetPasswordConfirmEdit,
        QStringLiteral("privilege.account.reset.confirm_password.placeholder"),
        QStringLiteral("再次输入新密码"));
    m_resetUserNameEdit->setStyleSheet(blueInputStyle());
    m_resetPasswordEdit->setStyleSheet(blueInputStyle());
    m_resetPasswordConfirmEdit->setStyleSheet(blueInputStyle());
    QLabel* resetUserLabel = new QLabel(QStringLiteral("目标用户"), m_accountPage);
    QLabel* resetPasswordLabel = new QLabel(QStringLiteral("新密码"), m_accountPage);
    QLabel* resetConfirmPasswordLabel = new QLabel(QStringLiteral("确认新密码"), m_accountPage);
    languageManager.bindText(resetUserLabel, QStringLiteral("privilege.account.reset.username.label"), QStringLiteral("目标用户"));
    languageManager.bindText(resetPasswordLabel, QStringLiteral("privilege.account.reset.password.label"), QStringLiteral("新密码"));
    languageManager.bindText(resetConfirmPasswordLabel, QStringLiteral("privilege.account.reset.confirm_password.label"), QStringLiteral("确认新密码"));
    resetLayout->addRow(resetUserLabel, m_resetUserNameEdit);
    resetLayout->addRow(resetPasswordLabel, m_resetPasswordEdit);
    resetLayout->addRow(resetConfirmPasswordLabel, m_resetPasswordConfirmEdit);
    resetSection->addLayout(resetLayout);

    m_resetPasswordButton = new QPushButton(QIcon(":/Icon/process_priority.svg"), QString(), m_accountPage);
    languageManager.bindToolTip(
        m_resetPasswordButton,
        QStringLiteral("privilege.account.reset.tooltip"),
        QStringLiteral("重置用户密码（会弹出二次确认）"));
    m_resetPasswordButton->setStyleSheet(blueButtonStyle());
    m_resetPasswordButton->setFixedWidth(34);
    ks::ui::NormalizeToolbarControl(m_resetPasswordButton);
    resetSection->addWidget(m_resetPasswordButton, 0, Qt::AlignLeft);
    workspace->parameters()->addWidget(resetGroup);
    // 账号和密码输入均为单行表单，采用与查询工具一致的触达高度。
    for (QLineEdit* edit : {m_createUserNameEdit, m_createPasswordEdit,
        m_createPasswordConfirmEdit, m_resetUserNameEdit, m_resetPasswordEdit,
        m_resetPasswordConfirmEdit})
    {
        ks::ui::NormalizeToolbarControl(edit);
    }

    // 账号创建与密码重置不共用固定动作栏，防止误把相邻账号表单当成同一操作。
    workspace->finish();
    // 此页随后直接挂入accounts子Tab；不再创建稍后立刻移除的临时根入口。
}

void PrivilegeDock::initializePermissionTab()
{
    ks::i18n::LanguageManager& languageManager = ks::i18n::LanguageManager::instance();
    m_permissionPage = new QWidget(m_tabWidget);
    m_permissionLayout = new QVBoxLayout(m_permissionPage);
    m_permissionLayout->setContentsMargins(4, 4, 4, 4);
    m_permissionLayout->setSpacing(6);

    m_permissionToolbarLayout = new QHBoxLayout();
    m_permissionToolbarLayout->setContentsMargins(0, 0, 0, 0);
    m_permissionToolbarLayout->setSpacing(6);

    m_permissionRefreshButton = new QPushButton(QIcon(":/Icon/process_refresh.svg"), QString(), m_permissionPage);
    languageManager.bindToolTip(
        m_permissionRefreshButton,
        QStringLiteral("privilege.permission.refresh.tooltip"),
        QStringLiteral("刷新用户/组/权限快照"));
    m_permissionRefreshButton->setStyleSheet(blueButtonStyle());
    m_permissionRefreshButton->setFixedWidth(34);

    m_permissionStatusLabel = new QLabel(privilegeText(
        "privilege.permission.status.pending", QStringLiteral("状态：待刷新")), m_permissionPage);
    m_permissionToolbarLayout->addWidget(m_permissionRefreshButton, 0);
    m_permissionToolbarLayout->addWidget(m_permissionStatusLabel, 1);
    ks::ui::NormalizeToolbarRow(m_permissionToolbarLayout);
    m_permissionLayout->addLayout(m_permissionToolbarLayout, 0);

    m_permissionTable = new ks::ui::TableActionTableView(m_permissionPage);
    ks::ui::SetTableActionBarMode(m_permissionTable, ks::ui::TableActionBarMode::Full);
    using PermissionModel = ks::ui::FlatTableModel<PermissionSnapshotRow>;
    m_permissionModel = new PermissionModel({{QStringLiteral("类型")},{QStringLiteral("名称")},
        {QStringLiteral("状态")},{QStringLiteral("详情")}},
        [this](const PermissionSnapshotRow& row, int column, int role) -> QVariant
        {
            return role == Qt::DisplayRole || role == Qt::UserRole + 1 ? permissionCell(row, column) : QVariant();
        }, m_permissionTable);
    m_permissionProxy = new LegacyHeaderProxy([](int column) -> QString
    {
        switch (column)
        {
        case 0: return privilegeText("privilege.permission.header.type", QStringLiteral("类型"));
        case 1: return privilegeText("privilege.permission.header.name", QStringLiteral("名称"));
        case 2: return privilegeText("privilege.permission.header.status", QStringLiteral("状态"));
        case 3: return privilegeText("privilege.permission.header.detail", QStringLiteral("详情"));
        default: return {};
        }
    }, m_permissionTable);
    m_permissionProxy->setSourceModel(m_permissionModel);
    m_permissionProxy->setSortRole(Qt::UserRole + 1);
    m_permissionProxy->setDynamicSortFilter(false);
    m_permissionTable->setModel(m_permissionProxy);
    m_permissionTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_permissionTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_permissionTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_permissionTable->setWordWrap(false);
    m_permissionTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    m_permissionTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    m_permissionTable->setSortingEnabled(true);
    m_permissionTable->sortByColumn(-1, Qt::AscendingOrder);
    ks::ui::ApplyTablePresentation(m_permissionTable);
    installPrivilegeTableCopyMenu(m_permissionTable);
    m_permissionLayout->addWidget(m_permissionTable, 1);

    m_navigationTabs.add(kPrivilegeNavigation[3], m_tabWidget, m_permissionPage);
}

void PrivilegeDock::applyTranslatedHeaders()
{
    if (m_accountProxy) static_cast<LegacyHeaderProxy*>(m_accountProxy)->retranslate();
    if (m_permissionProxy) static_cast<LegacyHeaderProxy*>(m_permissionProxy)->retranslate();
}

void PrivilegeDock::initializeConnections()
{
    connect(m_accountRefreshButton, &QPushButton::clicked, this, [this]() {
        refreshLocalUserList();
    });
    connect(m_permissionRefreshButton, &QPushButton::clicked, this, [this]() {
        refreshPermissionSnapshot();
    });
    connect(m_createUserButton, &QPushButton::clicked, this, [this]() {
        createUserByInputs();
    });
    connect(m_resetPasswordButton, &QPushButton::clicked, this, [this]() {
        resetPasswordByInputs();
    });
    connect(m_accountTable->selectionModel(), &QItemSelectionModel::selectionChanged, this, [this]()
    {
        if (m_writeBusy) return;
        QModelIndex source = m_accountTable->currentIndex();
        while (source.isValid() && source.model() != m_accountModel)
        {
            const auto* proxy = qobject_cast<const QAbstractProxyModel*>(source.model());
            if (proxy == nullptr) return;
            source = proxy->mapToSource(source);
        }
        const auto* user = source.isValid() && source.model() == m_accountModel ? m_accountModel->rowAt(source.row()) : nullptr;
        if (user) m_resetUserNameEdit->setText(user->name);
    });
    connect(m_tabWidget, &QTabWidget::currentChanged, this, [this]()
        { QTimer::singleShot(0, this, [this]() { ensureVisibleLegacyData(); }); });
    connect(m_accountTabs, &QTabWidget::currentChanged, this, [this]()
        { QTimer::singleShot(0, this, [this]() { ensureVisibleLegacyData(); }); });
    m_readOperation = new ks::ui::AsyncOperation(this);
    m_prepareOperation = new ks::ui::AsyncOperation(this);
    m_mutationOperation = new ks::ui::AsyncOperation(this);
    m_readOperation->setStateChangedCallback([this]()
    {
        if (!m_readOperation->isInFlight() && m_readPending)
        {
            m_readPending = false;
            if (m_readFields == ks::privilege::legacy::Users) m_usersDirty = true;
            else m_permissionsDirty = true;
            if (m_readFields == ks::privilege::legacy::Users) m_accountStatusMode = StatusMode::Missing;
            else m_permissionStatusMode = StatusMode::Missing;
            updateStatusLabels();
        }
    });
    m_prepareOperation->setStateChangedCallback([this]()
        { if (!m_prepareOperation->isInFlight() && m_preparePending) failPendingOperation(); });
    m_mutationOperation->setStateChangedCallback([this]()
        { if (!m_mutationOperation->isInFlight() && m_transactionPending) failPendingOperation(); });

}

std::shared_ptr<PrivilegeDock::LegacyReadResult> PrivilegeDock::makeReadResult(
    std::shared_ptr<ks::privilege::legacy::ReadSnapshot> snapshot)
{
    using namespace ks::privilege::legacy;
    auto result = std::make_shared<LegacyReadResult>();
    result->snapshot = std::move(snapshot);
    result->capturedAt = QDateTime::currentDateTime();
    const auto& source = *result->snapshot;
    if (!source.groupStatus.requested && !source.tokenStatus.requested) return result;
    result->rows.reserve(source.users.size() + source.groups.size() + source.privileges.size() + 3);
    for (const auto& user : source.users)
    {
        PermissionSnapshotRow row;
        row.kind = PermissionSnapshotRow::Kind::User; row.user = user;
        result->rows.push_back(std::move(row));
    }
    if (source.userStatus.requested && !source.userStatus.complete)
    {
        PermissionSnapshotRow row; row.kind = PermissionSnapshotRow::Kind::UserError;
        row.error = source.userStatus; result->rows.push_back(std::move(row));
    }
    for (const auto& group : source.groups)
    {
        PermissionSnapshotRow row;
        row.kind = PermissionSnapshotRow::Kind::Group; row.group = group;
        result->rows.push_back(std::move(row));
    }
    if (source.groupStatus.requested && !source.groupStatus.complete)
    {
        PermissionSnapshotRow row; row.kind = PermissionSnapshotRow::Kind::GroupError;
        row.error = source.groupStatus; result->rows.push_back(std::move(row));
    }
    for (const auto& privilege : source.privileges)
    {
        PermissionSnapshotRow row;
        row.kind = PermissionSnapshotRow::Kind::Privilege; row.privilege = privilege;
        result->rows.push_back(std::move(row));
    }
    if (source.tokenStatus.requested && !source.tokenStatus.complete)
    {
        PermissionSnapshotRow row; row.kind = PermissionSnapshotRow::Kind::TokenError;
        row.error = source.tokenStatus; result->rows.push_back(std::move(row));
    }
    else if (source.tokenStatus.requested && source.privileges.empty())
    {
        PermissionSnapshotRow row; row.kind = PermissionSnapshotRow::Kind::TokenEmpty;
        result->rows.push_back(std::move(row));
    }
    return result;
}

void PrivilegeDock::updateStatusLabels()
{
    if (!m_accountStatusLabel || !m_permissionStatusLabel) return;
    const auto common = [](const StatusMode mode) -> QString
    {
        switch (mode)
        {
        case StatusMode::Loading: return ks::i18n::text(QStringLiteral("privilege.legacy.status.loading"), QStringLiteral("状态：正在后台刷新…"));
        case StatusMode::Missing: return ks::i18n::text(QStringLiteral("misc.async.receipt_missing"), QStringLiteral("后台任务未返回有效回执，请刷新状态。"));
        case StatusMode::Writing: return ks::i18n::text(QStringLiteral("privilege.legacy.status.operation"), QStringLiteral("状态：正在执行账号操作…"));
        case StatusMode::ReadbackFailed: return ks::i18n::text(QStringLiteral("privilege.legacy.readback_failed"), QStringLiteral("操作回执已返回，但最终状态回读失败，请刷新确认。"));
        default: return {};
        }
    };
    if (m_accountStatusMode == StatusMode::Ready)
        m_accountStatusLabel->setText(m_userReadStatus.complete
            ? privilegeText("privilege.account.status.loaded", QStringLiteral("状态：已加载 %1 / %2 个账号"))
                .arg(m_accountModel->rowCount()).arg(static_cast<qulonglong>(m_userTotal))
            : privilegeText("privilege.account.status.refresh_failed", QStringLiteral("状态：刷新失败 - %1")).arg(readErrorText(m_userReadStatus)));
    else if (m_accountStatusMode == StatusMode::Pending)
        m_accountStatusLabel->setText(privilegeText("privilege.account.status.pending", QStringLiteral("状态：待刷新")));
    else m_accountStatusLabel->setText(common(m_accountStatusMode));
    if (m_permissionStatusMode == StatusMode::Ready)
        m_permissionStatusLabel->setText((m_permissionComplete
            ? privilegeText("privilege.permission.status.completed", QStringLiteral("状态：%1 刷新完成，共 %2 项"))
            : ks::i18n::text(QStringLiteral("privilege.legacy.status.partial"), QStringLiteral("状态：%1 读取含失败或部分结果，共 %2 项")))
            .arg(m_permissionCapturedAt.toString(QStringLiteral("HH:mm:ss"))).arg(m_permissionModel->rowCount()));
    else if (m_permissionStatusMode == StatusMode::Pending)
        m_permissionStatusLabel->setText(privilegeText("privilege.permission.status.pending", QStringLiteral("状态：待刷新")));
    else m_permissionStatusLabel->setText(common(m_permissionStatusMode));
}

void PrivilegeDock::ensureVisibleLegacyData()
{
    if (!isVisible() || m_writeBusy || !m_readOperation) return;
    if (m_accountPage->isVisible())
    {
        publishUsers();
        if ((!m_usersLoaded || m_usersDirty) && !(m_readPending && m_readFields == ks::privilege::legacy::Users))
            refreshLocalUserList();
    }
    else if (m_permissionPage->isVisible())
    {
        publishPermissions();
        if ((!m_permissionsLoaded || m_permissionsDirty) && !(m_readPending && m_readFields == ks::privilege::legacy::All))
            refreshPermissionSnapshot();
    }
}

void PrivilegeDock::refreshLocalUserList() { requestLegacySnapshot(ks::privilege::legacy::Users); }
void PrivilegeDock::refreshPermissionSnapshot() { requestLegacySnapshot(ks::privilege::legacy::All); }

void PrivilegeDock::requestLegacySnapshot(const unsigned fields)
{
    using namespace ks::privilege::legacy;
    const bool account = fields == Users;
    if (m_writeBusy)
    {
        if (account) m_usersDirty = true; else m_permissionsDirty = true;
        return;
    }
    if (m_readPending && m_readFields != fields)
    {
        if (m_readFields == Users) m_usersDirty = true; else m_permissionsDirty = true;
    }
    if (account) m_usersDirty = false; else m_permissionsDirty = false;
    const auto epoch = ++m_readEpoch;
    m_readFields = fields;
    m_readPending = true;
    if (account) m_accountStatusMode = StatusMode::Loading;
    else m_permissionStatusMode = StatusMode::Loading;
    updateStatusLabels();
    const std::string startLog = account
        ? privilegeLogText("privilege.log.refresh_users.start", QStringLiteral("[PrivilegeDock] 开始刷新本地用户列表。"))
        : privilegeLogText("privilege.log.refresh_privileges.start", QStringLiteral("[PrivilegeDock] 开始刷新权限快照。"));
    const std::string completeLog = account
        ? privilegeLogText("privilege.log.refresh_users.completed", QStringLiteral("[PrivilegeDock] 用户列表刷新完成, entriesRead="))
        : privilegeLogText("privilege.log.refresh_privileges.completed", QStringLiteral("[PrivilegeDock] 权限快照刷新完成, rows="));
    const std::string failedLog = privilegeLogText("privilege.log.refresh_users.failed", QStringLiteral("[PrivilegeDock] 刷新用户失败, status="));
    const QString errorCodeTemplate = privilegeText("privilege.error.code", QStringLiteral("错误码 %1"));
    const auto accepted = m_readOperation->submit<ReadRequest, std::shared_ptr<LegacyReadResult>>(
        {fields, 65536},
        [account, startLog, completeLog, failedLog, errorCodeTemplate](const ReadRequest& request, const ks::ui::AsyncOperationToken& token)
        {
            kLogEvent event;
            info << event << startLog << eol;
            auto result = makeReadResult(std::make_shared<ReadSnapshot>(queryNative(request, [&token]() { return token.isCanceled(); })));
            if (account && !result->snapshot->userStatus.complete)
            {
                const auto& status = result->snapshot->userStatus;
                const QString error = status.nativeError.isEmpty() || status.nativeError == QString::number(status.code)
                    ? errorCodeTemplate.arg(status.code) : status.nativeError;
                err << event << failedLog << status.code << ", error=" << error.toStdString() << eol;
            }
            else if (account)
                info << event << completeLog << result->snapshot->users.size()
                    << ", totalEntries=" << result->snapshot->totalUsers << eol;
            else
                info << event << completeLog << result->rows.size() << eol;
            return result;
        },
        [this, epoch, account](const std::shared_ptr<LegacyReadResult>& result, std::uint64_t)
        {
            if (epoch != m_readEpoch) return true;
            m_readPending = false;
            if (account)
            {
                m_usersLoaded = true; m_pendingUsers = result;
                publishUsers();
            }
            else
            {
                m_permissionsLoaded = true; m_pendingPermissions = result;
                publishPermissions();
            }
            return true;
        },
        [this, account](std::exception_ptr)
        {
            m_readPending = false;
            if (account) m_usersDirty = true; else m_permissionsDirty = true;
            if (account) m_accountStatusMode = StatusMode::Missing;
            else m_permissionStatusMode = StatusMode::Missing;
            updateStatusLabels();
        });
    if (accepted == 0)
    {
        m_readPending = false;
        if (account) m_usersDirty = true; else m_permissionsDirty = true;
    }
}

bool PrivilegeDock::publishUsers()
{
    if (!m_pendingUsers || !m_accountPage->isVisible()) return false;
    const QPointer<PrivilegeDock> guardThis(this);
    if (QApplication::activeModalWidget() != nullptr)
    {
        QTimer::singleShot(25, this, [guardThis]() { if (guardThis) guardThis->publishUsers(); });
        return false;
    }
    if (ks::ui::DeferTableUiCommitIfContextMenuOpen(this, QStringLiteral("privilege-local-user-publish"),
        {m_accountTable}, [guardThis]() { if (guardThis) guardThis->publishUsers(); })) return false;
    auto result = std::move(m_pendingUsers);
    m_userReadStatus = result->snapshot->userStatus;
    m_userTotal = result->snapshot->totalUsers;
    m_accountModel->setRows(std::move(result->snapshot->users));
    if (guardThis.isNull()) return true;
    m_accountStatusMode = StatusMode::Ready;
    updateStatusLabels();
    return true;
}

bool PrivilegeDock::publishPermissions()
{
    if (!m_pendingPermissions || !m_permissionPage->isVisible()) return false;
    const QPointer<PrivilegeDock> guardThis(this);
    if (QApplication::activeModalWidget() != nullptr)
    {
        QTimer::singleShot(25, this, [guardThis]() { if (guardThis) guardThis->publishPermissions(); });
        return false;
    }
    if (ks::ui::DeferTableUiCommitIfContextMenuOpen(this, QStringLiteral("privilege-permission-publish"),
        {m_permissionTable}, [guardThis]() { if (guardThis) guardThis->publishPermissions(); })) return false;
    auto result = std::move(m_pendingPermissions);
    const auto& snapshot = *result->snapshot;
    m_permissionComplete = snapshot.userStatus.complete && snapshot.groupStatus.complete && snapshot.tokenStatus.complete;
    m_permissionCapturedAt = result->capturedAt;
    m_permissionModel->setRows(std::move(result->rows));
    if (guardThis.isNull()) return true;
    m_permissionStatusMode = StatusMode::Ready;
    updateStatusLabels();
    return true;
}

void PrivilegeDock::refreshLocalUserTable()
{
    if (!m_accountModel) return;
    if (m_accountPage->isVisible()) publishUsers();
    if (m_accountModel->rowCount() > 0)
        emit m_accountModel->dataChanged(m_accountModel->index(0,0), m_accountModel->index(m_accountModel->rowCount()-1,3));
}
void PrivilegeDock::refreshPermissionTable()
{
    if (!m_permissionModel) return;
    if (m_permissionPage->isVisible()) publishPermissions();
    if (m_permissionModel->rowCount() > 0)
        emit m_permissionModel->dataChanged(m_permissionModel->index(0,0), m_permissionModel->index(m_permissionModel->rowCount()-1,3));
}

QVariant PrivilegeDock::accountCell(const ks::privilege::legacy::LocalUser& entry, const int column, const int role) const
{

            if (column == 0 && role == Qt::UserRole) return entry.name;
            if (role != Qt::DisplayRole && role != Qt::UserRole + 1) return {};
            switch (column)
            {
            case 0: return entry.name;
            case 1: return entry.fullName;
            case 2: return boolText((entry.flags & UF_ACCOUNTDISABLE) != 0);
            case 3: return fileTimeToDateTimeText(entry.lastLogon);
            default: return {};
            }

}

QString PrivilegeDock::readErrorText(const ks::privilege::legacy::ReadStatus& status) const
{
    return status.nativeError.isEmpty() || status.nativeError == QString::number(status.code)
        ? privilegeText("privilege.error.code", QStringLiteral("错误码 %1")).arg(status.code)
        : status.nativeError;
}

QVariant PrivilegeDock::permissionCell(const PermissionSnapshotRow& row, const int column) const
{
    using Kind = PermissionSnapshotRow::Kind;
    if (column == 0)
    {
        if (row.kind == Kind::User || row.kind == Kind::UserError)
            return privilegeText("privilege.row.type.user", QStringLiteral("用户"));
        if (row.kind == Kind::Group || row.kind == Kind::GroupError)
            return privilegeText("privilege.row.type.group", QStringLiteral("组"));
        return privilegeText("privilege.row.type.process_privilege", QStringLiteral("进程权限"));
    }
    if (row.kind == Kind::User)
    {
        if (column == 1) return row.user.name;
        if (column == 2) return privilegeText("privilege.row.user.status", QStringLiteral("禁用:%1  最后登录:%2"))
            .arg(boolText((row.user.flags & UF_ACCOUNTDISABLE) != 0)).arg(fileTimeToDateTimeText(row.user.lastLogon));
        if (column == 3) return row.user.fullName.isEmpty()
            ? privilegeText("privilege.row.no_full_name", QStringLiteral("<无全名>"))
            : privilegeText("privilege.row.full_name", QStringLiteral("全名:%1")).arg(row.user.fullName);
    }
    else if (row.kind == Kind::Group)
    {
        if (column == 1) return row.group.name;
        if (column == 2) return privilegeText("privilege.row.group.local", QStringLiteral("本地组"));
        if (column == 3) return row.group.comment.isEmpty()
            ? privilegeText("privilege.row.no_description", QStringLiteral("<无说明>")) : row.group.comment;
    }
    else if (row.kind == Kind::Privilege)
    {
        if (column == 1) return row.privilege.nameKnown ? row.privilege.name
            : privilegeText("privilege.row.lookup_privilege_name_failed", QStringLiteral("<LookupPrivilegeName失败>"));
        if (column == 2) return privilegeText("privilege.row.process_privilege.status", QStringLiteral("启用:%1 默认:%2 移除:%3"))
            .arg(boolText((row.privilege.attributes & SE_PRIVILEGE_ENABLED) != 0))
            .arg(boolText((row.privilege.attributes & SE_PRIVILEGE_ENABLED_BY_DEFAULT) != 0))
            .arg(boolText((row.privilege.attributes & SE_PRIVILEGE_REMOVED) != 0));
        if (column == 3) return QStringLiteral("%1, Attributes=0x%2")
            .arg(row.privilege.displayNameKnown ? row.privilege.displayName
                : privilegeText("privilege.row.no_display_name", QStringLiteral("<无显示名>")))
            .arg(QString::number(row.privilege.attributes,16).toUpper());
    }
    else if (row.kind == Kind::TokenEmpty)
    {
        if (column == 1) return privilegeText("privilege.row.empty", QStringLiteral("<空>"));
        if (column == 2) return privilegeText("privilege.row.no_privileges", QStringLiteral("无权限项"));
        if (column == 3) return privilegeText("privilege.row.no_privileges.detail", QStringLiteral("当前进程令牌未返回任何权限条目。"));
    }
    else
    {
        if (column == 1) return privilegeText("privilege.row.read_failed", QStringLiteral("<读取失败>"));
        if (column == 2) return privilegeText("privilege.row.error", QStringLiteral("错误"));
        if (column == 3)
        {
            const QString error = readErrorText(row.error);
            if (row.kind != Kind::TokenError) return error;
            return row.error.stage == ks::privilege::legacy::FailureStage::OpenToken
                ? privilegeText("privilege.error.open_process_token", QStringLiteral("OpenProcessToken失败: %1")).arg(error)
                : privilegeText("privilege.error.get_token_information", QStringLiteral("GetTokenInformation失败: %1")).arg(error);
        }
    }
    return {};
}


void PrivilegeDock::createUserByInputs()
{
    if (m_writeBusy) return;
    const QString userName = m_createUserNameEdit->text().trimmed();
    const QString password = m_createPasswordEdit->text();
    const QString confirmPassword = m_createPasswordConfirmEdit->text();

    if (userName.isEmpty())
    {
        QMessageBox::warning(
            this,
            privilegeText("privilege.dialog.create.title", QStringLiteral("创建用户")),
            privilegeText("privilege.dialog.create.username_empty", QStringLiteral("用户名不能为空。")));
        return;
    }
    if (password.isEmpty())
    {
        QMessageBox::warning(
            this,
            privilegeText("privilege.dialog.create.title", QStringLiteral("创建用户")),
            privilegeText("privilege.dialog.create.password_empty", QStringLiteral("密码不能为空。")));
        return;
    }
    if (password != confirmPassword)
    {
        QMessageBox::warning(
            this,
            privilegeText("privilege.dialog.create.title", QStringLiteral("创建用户")),
            privilegeText("privilege.dialog.create.password_mismatch", QStringLiteral("两次密码输入不一致。")));
        return;
    }

    if (!ks::ui::isCurrentProcessElevated())
    {
        (void)ks::ui::requestAdministratorRestartForFeature(
            this,
            QStringLiteral("创建本地用户"));
        return;
    }

    // 二次确认：避免误触导致系统新增账号。
    const QPointer<PrivilegeDock> guardThis(this);
    const int confirm = QMessageBox::question(
        this,
        privilegeText("privilege.dialog.create.title", QStringLiteral("创建用户")),
        privilegeText(
            "privilege.dialog.create.confirm",
            QStringLiteral("确定创建本地用户“%1”吗？")).arg(userName),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);
    if (guardThis.isNull() || confirm != QMessageBox::Yes)
    {
        return;
    }

    ks::privilege::legacy::MutationRequest request;
    request.kind = ks::privilege::legacy::MutationKind::CreateUser;
    request.userName = userName;
    request.password = std::make_shared<ks::privilege::legacy::PasswordValue>(password.toStdWString());
    beginMutation(std::move(request));
}

void PrivilegeDock::resetPasswordByInputs()
{
    if (m_writeBusy) return;
    const QString userName = m_resetUserNameEdit->text().trimmed();
    const QString password = m_resetPasswordEdit->text();
    const QString confirmPassword = m_resetPasswordConfirmEdit->text();

    if (userName.isEmpty())
    {
        QMessageBox::warning(
            this,
            privilegeText("privilege.dialog.reset.title", QStringLiteral("重置密码")),
            privilegeText("privilege.dialog.reset.username_empty", QStringLiteral("目标用户名不能为空。")));
        return;
    }
    if (password.isEmpty())
    {
        QMessageBox::warning(
            this,
            privilegeText("privilege.dialog.reset.title", QStringLiteral("重置密码")),
            privilegeText("privilege.dialog.reset.password_empty", QStringLiteral("新密码不能为空。")));
        return;
    }
    if (password != confirmPassword)
    {
        QMessageBox::warning(
            this,
            privilegeText("privilege.dialog.reset.title", QStringLiteral("重置密码")),
            privilegeText("privilege.dialog.reset.password_mismatch", QStringLiteral("两次密码输入不一致。")));
        return;
    }

    if (!ks::ui::isCurrentProcessElevated())
    {
        (void)ks::ui::requestAdministratorRestartForFeature(
            this,
            QStringLiteral("重置本地用户密码"));
        return;
    }

    using namespace ks::privilege::legacy;
    const auto secret = std::make_shared<PasswordValue>(password.toStdWString());
    setWriteBusy(true);
    m_preparePending = true;
    const auto accepted = m_prepareOperation->submit<QString, UserIdentity>(userName,
        [](const QString& name, const ks::ui::AsyncOperationToken&) { return queryNativeIdentity(name); },
        [this, userName, secret](const UserIdentity& identity, std::uint64_t)
        {
            m_preparePending = false;
            const QPointer<PrivilegeDock> guardThis(this);
            if (!identity.valid || identity.code != Success)
            {
                const QString error = privilegeText("privilege.error.net_user_set_info", QStringLiteral("NetUserSetInfo失败: %1"))
                    .arg(readErrorText({true, false, identity.code, FailureStage::Enumeration, identity.nativeError}));
                const bool handled = ks::ui::promptForPrivilegeFailure(this, QStringLiteral("重置本地用户密码"), error);
                if (guardThis.isNull()) return true;
                if (!handled) QMessageBox::warning(this,
                    privilegeText("privilege.dialog.reset.title", QStringLiteral("重置密码")), error);
                if (guardThis.isNull()) return true;
                setWriteBusy(false);
                return true;
            }
            const int confirm = QMessageBox::question(this,
                privilegeText("privilege.dialog.reset.title", QStringLiteral("重置密码")),
                privilegeText("privilege.dialog.reset.confirm", QStringLiteral("确定为“%1”重置密码吗？")).arg(userName),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
            if (guardThis.isNull()) return true;
            if (confirm != QMessageBox::Yes) { setWriteBusy(false); return true; }
            beginMutation({MutationKind::ResetPassword, userName, secret, identity});
            return true;
        },
        [this](std::exception_ptr) { failPendingOperation(); });
    if (accepted == 0) failPendingOperation();

}

void PrivilegeDock::setWriteBusy(const bool busy)
{
    if (busy && !m_writeBusy)
    {
        m_statusBeforeWrite = m_accountStatusMode;
        m_accountStatusMode = StatusMode::Writing;
    }
    if (!busy && m_accountStatusMode == StatusMode::Writing) m_accountStatusMode = m_statusBeforeWrite;
    m_writeBusy = busy;
    if (busy)
    {
        if (m_readPending)
        {
            if (m_readFields == ks::privilege::legacy::Users) m_usersDirty = true;
            else m_permissionsDirty = true;
        }
        if (m_pendingUsers) m_usersDirty = true;
        if (m_pendingPermissions) m_permissionsDirty = true;
        m_pendingUsers.reset();
        m_pendingPermissions.reset();
        ++m_readEpoch;
        m_readPending = false;
        m_readOperation->cancel();
    }
    for (auto* widget : {static_cast<QWidget*>(m_accountRefreshButton), static_cast<QWidget*>(m_permissionRefreshButton),
        static_cast<QWidget*>(m_createUserButton), static_cast<QWidget*>(m_resetPasswordButton),
        static_cast<QWidget*>(m_createUserNameEdit), static_cast<QWidget*>(m_createPasswordEdit),
        static_cast<QWidget*>(m_createPasswordConfirmEdit), static_cast<QWidget*>(m_resetUserNameEdit),
        static_cast<QWidget*>(m_resetPasswordEdit), static_cast<QWidget*>(m_resetPasswordConfirmEdit)})
        widget->setEnabled(!busy);
    updateStatusLabels();
}

void PrivilegeDock::failPendingOperation()
{
    m_preparePending = false;
    m_transactionPending = false;
    m_usersDirty = true;
    m_permissionsDirty = true;
    setWriteBusy(false);
    m_accountStatusMode = StatusMode::Missing;
    updateStatusLabels();
}

void PrivilegeDock::beginMutation(ks::privilege::legacy::MutationRequest request)
{
    using namespace ks::privilege::legacy;
    if (m_transactionPending) return;
    setWriteBusy(true);
    m_transactionPending = true;
    const bool create = request.kind == MutationKind::CreateUser;
    const std::string requestLog = create
        ? privilegeLogText("privilege.log.create_user.request", QStringLiteral("[PrivilegeDock] 创建用户请求, user="))
        : privilegeLogText("privilege.log.reset_password.request", QStringLiteral("[PrivilegeDock] 重置密码请求, user="));
    const std::string successLog = create
        ? privilegeLogText("privilege.log.create_user.succeeded", QStringLiteral("[PrivilegeDock] 创建用户成功, user="))
        : privilegeLogText("privilege.log.reset_password.succeeded", QStringLiteral("[PrivilegeDock] 重置密码成功, user="));
    const std::string failureLog = create
        ? privilegeLogText("privilege.log.create_user.failed", QStringLiteral("[PrivilegeDock] 创建用户失败, user="))
        : privilegeLogText("privilege.log.reset_password.failed", QStringLiteral("[PrivilegeDock] 重置密码失败, user="));
    const QString errorTemplate = create
        ? privilegeText("privilege.error.net_user_add", QStringLiteral("NetUserAdd失败: %1, 参数索引=%2"))
        : privilegeText("privilege.error.net_user_set_info", QStringLiteral("NetUserSetInfo失败: %1"));
    const QString errorCodeTemplate = privilegeText("privilege.error.code", QStringLiteral("错误码 %1"));
    const QString identityError = ks::i18n::text(QStringLiteral("privilege.legacy.error.identity_changed"),
        QStringLiteral("目标账号身份已变化，未重置密码。请重新选择并确认。"));
    const QString unknownError = ks::i18n::text(QStringLiteral("misc.async.receipt_missing"),
        QStringLiteral("后台任务未返回有效回执，请刷新状态。"));
    const auto accepted = m_mutationOperation->submitTransaction<MutationRequest, std::shared_ptr<LegacyMutationResult>>(
        std::move(request),
        [create, requestLog, successLog, failureLog, errorTemplate, errorCodeTemplate, identityError, unknownError]
        (const MutationRequest& frozen)
        {
            kLogEvent event;
            info << event << requestLog << frozen.userName.toStdString() << eol;
            auto result = std::make_shared<LegacyMutationResult>();
            result->create = create;
            result->receipt = executeNativeMutation(frozen);
            if (!result->receipt.outcomeKnown) result->errorText = unknownError;
            else if (result->receipt.identityChanged) result->errorText = identityError;
            else if (!result->receipt.succeeded())
            {
                QString error = nativeErrorText(result->receipt.status.code);
                if (error == QString::number(result->receipt.status.code)) error = errorCodeTemplate.arg(result->receipt.status.code);
                result->errorText = create ? errorTemplate.arg(error).arg(result->receipt.status.parameter) : errorTemplate.arg(error);
            }
            if (result->receipt.succeeded())
                info << event << successLog << frozen.userName.toStdString() << eol;
            else
                err << event << failureLog << frozen.userName.toStdString()
                    << ", error=" << result->errorText.toStdString() << eol;
            if (result->receipt.after) result->after = makeReadResult(result->receipt.after);
            return result;
        },
        [this](const std::shared_ptr<LegacyMutationResult>& result, std::uint64_t ticket)
        {
            return applyMutationResult(result, ticket);
        },
        [this](std::exception_ptr) { failPendingOperation(); });
    if (accepted == 0) failPendingOperation();
}

bool PrivilegeDock::applyMutationResult(const std::shared_ptr<LegacyMutationResult>& result, const std::uint64_t ticket)
{
    const QPointer<PrivilegeDock> guardThis(this);
    const auto resume = [guardThis, result, ticket]()
    {
        if (guardThis && guardThis->applyMutationResult(result, ticket) && guardThis)
            guardThis->m_mutationOperation->completeDeferred(ticket);
    };
    if (QApplication::activeModalWidget() != nullptr)
    {
        QTimer::singleShot(25, this, resume);
        return false;
    }
    if (ks::ui::DeferTableUiCommitIfContextMenuOpen(this, QStringLiteral("privilege-account-mutation-receipt"),
        {m_accountTable, m_permissionTable}, resume)) return false;
    if (result->after)
    {
        m_pendingUsers = result->after; m_pendingPermissions = result->after;
        m_usersLoaded = true; m_permissionsLoaded = true;
        m_usersDirty = false; m_permissionsDirty = false;
        publishUsers();
        if (guardThis.isNull()) return true;
        publishPermissions();
        if (guardThis.isNull()) return true;
    }
    if (!result->after)
    {
        m_usersDirty = true; m_permissionsDirty = true;
        m_accountStatusMode = StatusMode::ReadbackFailed;
        m_permissionStatusMode = StatusMode::ReadbackFailed;
        updateStatusLabels();
    }
    // The operation kind is read from the frozen receipt, never from currently edited widgets.
    const bool create = result->create;
    const QString title = create
        ? privilegeText("privilege.dialog.create.title", QStringLiteral("创建用户"))
        : privilegeText("privilege.dialog.reset.title", QStringLiteral("重置密码"));
    if (!result->receipt.succeeded())
    {
        const bool handled = ks::ui::promptForPrivilegeFailure(this,
            create ? QStringLiteral("创建本地用户") : QStringLiteral("重置本地用户密码"), result->errorText);
        if (guardThis.isNull()) return true;
        if (!handled) QMessageBox::warning(this, title, result->errorText);
        if (guardThis.isNull()) return true;
    }
    else
    {
        QString message = create
            ? privilegeText("privilege.dialog.create.succeeded", QStringLiteral("创建成功。"))
            : privilegeText("privilege.dialog.reset.succeeded", QStringLiteral("重置成功。"));
        if (result->receipt.readbackFailed)
            message += QLatin1Char('\n') + ks::i18n::text(QStringLiteral("privilege.legacy.readback_failed"),
                QStringLiteral("操作回执已返回，但最终状态回读失败，请刷新确认。"));
        QMessageBox::information(this, title, message);
        if (guardThis.isNull()) return true;
    }
    m_transactionPending = false;
    setWriteBusy(false);
    return true;
}


QString PrivilegeDock::fileTimeToDateTimeText(const std::uint32_t secondsSince1970) const
{
    if (secondsSince1970 == 0)
    {
        return privilegeText("privilege.time.never", QStringLiteral("从未"));
    }
    return QDateTime::fromSecsSinceEpoch(secondsSince1970).toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
}

ks::ui::PageNavigationCatalog PrivilegeDock::navigationCatalog(const PrivilegeDock* existing)
{
    ks::ui::PageNavigationCatalog schema;
    for (const auto& definition : kPrivilegeNavigation)
        schema.append(ks::ui::PageNavigationTabDescriptor(definition));
    return existing ? existing->m_navigationTabs.describe(std::move(schema)) : schema;
}
QWidget* PrivilegeDock::activateNavigationDescriptor(const QString& id)
{
    return m_navigationTabs.activate(id, navigationCatalog(this));
}
namespace { const bool kPrivilegeNavigationRegistered =
    ks::ui::RegisterTypedPageNavigationCatalog<PrivilegeDock>(QStringLiteral("privilege")); }
