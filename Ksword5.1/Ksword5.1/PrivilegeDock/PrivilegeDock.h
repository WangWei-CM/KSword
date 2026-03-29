#pragma once

// ============================================================
// PrivilegeDock.h
// 作用：
// 1) 提供“账号/权限”双页签；
// 2) 账号页支持本地用户创建、密码重置（含二次确认）；
// 3) 权限页展示用户、组与当前进程权限明细。
// ============================================================

#include "../Framework.h"

#include <QWidget>

#include <vector>  // std::vector：缓存本地账号与组信息。

class CodeEditorWidget;
class QHBoxLayout;
class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;
class QTabWidget;
class QVBoxLayout;

class PrivilegeDock final : public QWidget
{
    Q_OBJECT

public:
    // 构造函数：
    // - parent：Qt 父控件；
    // - 作用：初始化账号与权限页面并触发首轮数据加载。
    explicit PrivilegeDock(QWidget* parent = nullptr);

private:
    // LocalUserEntry：
    // - 作用：缓存本地用户表格展示字段；
    // - name 用于后续重置密码操作。
    struct LocalUserEntry
    {
        QString name;         // name：本地用户名。
        QString fullName;     // fullName：全名/备注。
        bool disabled = false; // disabled：是否禁用。
        QString lastLogonText; // lastLogonText：最后登录时间文本。
    };

private:
    // ===================== UI 初始化 =====================
    void initializeUi();
    void initializeAccountTab();
    void initializePermissionTab();
    void initializeConnections();

    // ===================== 账号功能 =====================
    void refreshLocalUserList();
    void refreshLocalUserTable();
    bool createLocalUser(
        const QString& userName,
        const QString& passwordText,
        QString* errorTextOut);
    bool resetLocalUserPassword(
        const QString& userName,
        const QString& newPassword,
        QString* errorTextOut);
    void createUserByInputs();
    void resetPasswordByInputs();

    // ===================== 权限页功能 =====================
    void refreshPermissionSnapshot();
    QString buildLocalUserAndGroupText() const;
    QString buildCurrentProcessPrivilegeText() const;
    QString fileTimeToDateTimeText(std::uint32_t secondsSince1970) const;
    QString winErrorText(DWORD code) const;

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
    QTableWidget* m_accountTable = nullptr;          // m_accountTable：本地用户列表。
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
    CodeEditorWidget* m_permissionEditor = nullptr; // m_permissionEditor：权限详情文本。

    // 缓存。
    std::vector<LocalUserEntry> m_localUserList;    // m_localUserList：本地用户缓存。
};

