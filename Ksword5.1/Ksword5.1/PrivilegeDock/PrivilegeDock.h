#pragma once

// ============================================================
// PrivilegeDock.h
// 作用：
// 1) 提供账号、组、权限分配、令牌对比、访问诊断、会话、启动与快照工作台；
// 2) 账号页支持本地用户创建、密码重置（含二次确认）；
// 3) 权限页展示用户、组与当前进程权限明细。
// ============================================================

#include "../Framework.h"
#include "PrivilegeLegacyValueOperations.h"
#include "../UI/PageNavigationTabCatalog.h"

#include <QWidget>
#include <QDateTime>
#include <memory>

#include <vector>  // std::vector：缓存本地账号与权限快照信息。

class QHBoxLayout;
class QEvent;
class QLabel;
class QLineEdit;
class QPushButton;
class QShowEvent;
class QTableView;
class QSortFilterProxyModel;
namespace ks::ui { class AsyncOperation; template<typename RowT> class FlatTableModel; }
class QTabWidget;
class QVBoxLayout;

class PrivilegeDock final : public QWidget
{
    Q_OBJECT

public:
    static ks::ui::PageNavigationCatalog navigationCatalog(const PrivilegeDock* existing = nullptr);
    QWidget* activateNavigationDescriptor(const QString& id);

    // 构造函数：
    // - parent：Qt 父控件；
    // - 作用：初始化账号与权限页面并触发首轮数据加载。
    explicit PrivilegeDock(QWidget* parent = nullptr);
    ~PrivilegeDock() override;

protected:
    // showEvent：
    // - 首次显示时再刷新账号与权限快照；
    // - 避免主窗口启动阶段同步访问本地账号与权限信息。
    void showEvent(QShowEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    ks::ui::PageNavigationTabCatalog m_navigationTabs;

    struct PermissionSnapshotRow
    {
        enum class Kind { User, Group, Privilege, UserError, GroupError, TokenError, TokenEmpty };
        Kind kind = Kind::User;
        ks::privilege::legacy::LocalUser user;
        ks::privilege::legacy::LocalGroup group;
        ks::privilege::legacy::TokenPrivilege privilege;
        ks::privilege::legacy::ReadStatus error;
    };
    struct LegacyReadResult
    {
        std::shared_ptr<ks::privilege::legacy::ReadSnapshot> snapshot;
        std::vector<PermissionSnapshotRow> rows;
        QDateTime capturedAt;
    };
    struct LegacyMutationResult
    {
        ks::privilege::legacy::MutationReceipt receipt;
        std::shared_ptr<LegacyReadResult> after;
        QString errorText;
        bool create = false;
    };

private:
    // ===================== UI 初始化 =====================
    void initializeUi();
    void initializeAccountTab();
    void initializePermissionTab();
    void initializeConnections();
    void applyTranslatedHeaders();

    // ===================== 账号功能 =====================
    void refreshLocalUserList();
    void refreshLocalUserTable();
    void createUserByInputs();
    void resetPasswordByInputs();

    // ===================== 权限页功能 =====================
    void refreshPermissionSnapshot();
    void refreshPermissionTable();
    void ensureVisibleLegacyData();
    void updateStatusLabels();
    void requestLegacySnapshot(unsigned fields);
    static std::shared_ptr<LegacyReadResult> makeReadResult(std::shared_ptr<ks::privilege::legacy::ReadSnapshot> snapshot);
    bool publishUsers();
    bool publishPermissions();
    QVariant accountCell(const ks::privilege::legacy::LocalUser& entry, int column, int role) const;
    QVariant permissionCell(const PermissionSnapshotRow& row, int column) const;
    void setWriteBusy(bool busy);
    void beginMutation(ks::privilege::legacy::MutationRequest request);
    bool applyMutationResult(const std::shared_ptr<LegacyMutationResult>& result, std::uint64_t ticket);
    void failPendingOperation();
    QString fileTimeToDateTimeText(std::uint32_t secondsSince1970) const;
    QString readErrorText(const ks::privilege::legacy::ReadStatus& status) const;

private:
    // 顶层控件。
    QVBoxLayout* m_rootLayout = nullptr;  // m_rootLayout：根布局。
    QTabWidget* m_tabWidget = nullptr;    // m_tabWidget：账号/权限 Tab。

    // 账号页。
    QWidget* m_accountPage = nullptr;                // m_accountPage：账号页容器。
    QVBoxLayout* m_accountLayout = nullptr;          // m_accountLayout：账号页布局。
    QHBoxLayout* m_accountToolbarLayout = nullptr;   // m_accountToolbarLayout：账号页工具栏布局。
    QPushButton* m_accountRefreshButton = nullptr;   // m_accountRefreshButton：刷新账号按钮。
    QLabel* m_accountStatusLabel = nullptr;          // m_accountStatusLabel：账号状态标签。
    QTableView* m_accountTable = nullptr;          // m_accountTable：本地用户列表。
    QLineEdit* m_createUserNameEdit = nullptr;       // m_createUserNameEdit：新建用户名输入框。
    QLineEdit* m_createPasswordEdit = nullptr;       // m_createPasswordEdit：新建密码输入框。
    QLineEdit* m_createPasswordConfirmEdit = nullptr; // m_createPasswordConfirmEdit：新建密码确认框。
    QPushButton* m_createUserButton = nullptr;       // m_createUserButton：执行创建按钮。
    QLineEdit* m_resetUserNameEdit = nullptr;        // m_resetUserNameEdit：重置密码目标用户输入框。
    QLineEdit* m_resetPasswordEdit = nullptr;        // m_resetPasswordEdit：重置后密码输入框。
    QLineEdit* m_resetPasswordConfirmEdit = nullptr; // m_resetPasswordConfirmEdit：重置密码确认框。
    QPushButton* m_resetPasswordButton = nullptr;    // m_resetPasswordButton：执行重置按钮。

    // 权限页。
    QWidget* m_permissionPage = nullptr;            // m_permissionPage：权限页容器。
    QVBoxLayout* m_permissionLayout = nullptr;      // m_permissionLayout：权限页布局。
    QHBoxLayout* m_permissionToolbarLayout = nullptr; // m_permissionToolbarLayout：权限页工具栏。
    QPushButton* m_permissionRefreshButton = nullptr; // m_permissionRefreshButton：刷新权限按钮。
    QLabel* m_permissionStatusLabel = nullptr;      // m_permissionStatusLabel：权限页状态标签。
    QTableView* m_permissionTable = nullptr;      // m_permissionTable：权限快照列表。

    QTabWidget* m_accountTabs = nullptr;
    ks::ui::FlatTableModel<ks::privilege::legacy::LocalUser>* m_accountModel = nullptr;
    ks::ui::FlatTableModel<PermissionSnapshotRow>* m_permissionModel = nullptr;
    QSortFilterProxyModel* m_accountProxy = nullptr;
    QSortFilterProxyModel* m_permissionProxy = nullptr;
    ks::ui::AsyncOperation* m_readOperation = nullptr;
    ks::ui::AsyncOperation* m_prepareOperation = nullptr;
    ks::ui::AsyncOperation* m_mutationOperation = nullptr;
    std::shared_ptr<LegacyReadResult> m_pendingUsers;
    std::shared_ptr<LegacyReadResult> m_pendingPermissions;
    std::uint64_t m_readEpoch = 0;
    unsigned m_readFields = 0;
    bool m_readPending = false;
    bool m_usersLoaded = false;
    bool m_permissionsLoaded = false;
    bool m_usersDirty = true;
    bool m_permissionsDirty = true;
    bool m_writeBusy = false;
    bool m_preparePending = false;
    bool m_transactionPending = false;
    enum class StatusMode { Pending, Loading, Ready, Missing, Writing, ReadbackFailed };
    StatusMode m_accountStatusMode = StatusMode::Pending;
    StatusMode m_permissionStatusMode = StatusMode::Pending;
    StatusMode m_statusBeforeWrite = StatusMode::Pending;
    ks::privilege::legacy::ReadStatus m_userReadStatus;
    std::uint64_t m_userTotal = 0;
    QDateTime m_permissionCapturedAt;
    bool m_permissionComplete = false;
};
