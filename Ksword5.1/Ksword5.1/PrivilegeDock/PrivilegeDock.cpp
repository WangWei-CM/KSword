#include "PrivilegeDock.h"

// ============================================================
// PrivilegeDock.cpp
// 作用：
// 1) 实现本地账号管理（创建用户、重置密码）；
// 2) 枚举用户/组/当前进程权限，用于权限审计；
// 3) 所有敏感操作均要求二次确认并写入日志。
// ============================================================

#include "../theme.h"
#include "../UI/CodeEditorWidget.h"

#include <QAbstractItemView>
#include <QDateTime>
#include <QFormLayout>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QTabWidget>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <Lm.h>

#pragma comment(lib, "Netapi32.lib")

namespace
{
    // blueButtonStyle 作用：
    // - 统一按钮风格，保证权限页视觉与其他 Dock 一致。
    QString blueButtonStyle()
    {
        return QStringLiteral(
            "QPushButton{color:%1;background:%5;border:1px solid %2;border-radius:3px;padding:3px 8px;}"
            "QPushButton:hover{background:%3;color:#FFFFFF;border:1px solid %3;}"
            "QPushButton:pressed{background:%4;color:#FFFFFF;}")
            .arg(KswordTheme::PrimaryBlueHex)
            .arg(KswordTheme::PrimaryBlueBorderHex)
            .arg(QStringLiteral("#2E8BFF"))
            .arg(KswordTheme::PrimaryBluePressedHex)
            .arg(KswordTheme::SurfaceHex());
    }

    // blueInputStyle 作用：
    // - 统一输入框边框与焦点高亮风格。
    QString blueInputStyle()
    {
        return QStringLiteral(
            "QLineEdit{border:1px solid %2;border-radius:3px;background:%3;color:%4;padding:2px 6px;}"
            "QLineEdit:focus{border:1px solid %1;}")
            .arg(KswordTheme::PrimaryBlueHex)
            .arg(KswordTheme::BorderHex())
            .arg(KswordTheme::SurfaceHex())
            .arg(KswordTheme::TextPrimaryHex());
    }

    // tableHeaderStyle 作用：
    // - 统一账号表头样式，保证深浅色模式可读性。
    QString tableHeaderStyle()
    {
        return QStringLiteral("QHeaderView::section{color:%1;background:%2;border:1px solid %3;font-weight:600;}")
            .arg(KswordTheme::PrimaryBlueHex)
            .arg(KswordTheme::SurfaceHex())
            .arg(KswordTheme::BorderHex());
    }

    // boolText 作用：
    // - 把布尔状态统一转换成“是/否”中文文本。
    QString boolText(const bool value)
    {
        return value ? QStringLiteral("是") : QStringLiteral("否");
    }
}

PrivilegeDock::PrivilegeDock(QWidget* parent)
    : QWidget(parent)
{
    kLogEvent event;
    info << event << "[PrivilegeDock] 构造开始。" << eol;

    initializeUi();
    initializeConnections();

    info << event << "[PrivilegeDock] 构造完成。" << eol;
}

void PrivilegeDock::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);

    if (m_initialRefreshDone)
    {
        return;
    }

    m_initialRefreshDone = true;
    if (m_accountStatusLabel != nullptr)
    {
        m_accountStatusLabel->setText(QStringLiteral("状态：首次打开，正在加载账号列表..."));
    }
    if (m_permissionStatusLabel != nullptr)
    {
        m_permissionStatusLabel->setText(QStringLiteral("状态：首次打开，正在加载权限快照..."));
    }

    QTimer::singleShot(0, this, [this]()
        {
            refreshLocalUserList();
            refreshPermissionSnapshot();
        });
}

void PrivilegeDock::initializeUi()
{
    m_rootLayout = new QVBoxLayout(this);
    m_rootLayout->setContentsMargins(4, 4, 4, 4);
    m_rootLayout->setSpacing(6);

    m_tabWidget = new QTabWidget(this);
    m_tabWidget->setTabPosition(QTabWidget::West);
    m_rootLayout->addWidget(m_tabWidget, 1);

    initializeAccountTab();
    initializePermissionTab();
}

void PrivilegeDock::initializeAccountTab()
{
    m_accountPage = new QWidget(m_tabWidget);
    m_accountLayout = new QVBoxLayout(m_accountPage);
    m_accountLayout->setContentsMargins(4, 4, 4, 4);
    m_accountLayout->setSpacing(6);

    m_accountToolbarLayout = new QHBoxLayout();
    m_accountToolbarLayout->setContentsMargins(0, 0, 0, 0);
    m_accountToolbarLayout->setSpacing(6);

    m_accountRefreshButton = new QPushButton(QIcon(":/Icon/process_refresh.svg"), QString(), m_accountPage);
    m_accountRefreshButton->setToolTip(QStringLiteral("刷新本地账号列表"));
    m_accountRefreshButton->setStyleSheet(blueButtonStyle());
    m_accountRefreshButton->setFixedWidth(34);

    m_accountStatusLabel = new QLabel(QStringLiteral("状态：待刷新"), m_accountPage);
    m_accountToolbarLayout->addWidget(m_accountRefreshButton, 0);
    m_accountToolbarLayout->addWidget(m_accountStatusLabel, 1);
    m_accountLayout->addLayout(m_accountToolbarLayout, 0);

    m_accountTable = new QTableWidget(m_accountPage);
    m_accountTable->setColumnCount(4);
    m_accountTable->setHorizontalHeaderLabels({
        QStringLiteral("用户名"),
        QStringLiteral("全名"),
        QStringLiteral("已禁用"),
        QStringLiteral("最后登录")
        });
    m_accountTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_accountTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_accountTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_accountTable->horizontalHeader()->setStyleSheet(tableHeaderStyle());
    m_accountTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_accountTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_accountTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_accountTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_accountLayout->addWidget(m_accountTable, 1);

    // 新建用户输入区：包含用户名、密码、确认密码。
    QFormLayout* createLayout = new QFormLayout();
    m_createUserNameEdit = new QLineEdit(m_accountPage);
    m_createPasswordEdit = new QLineEdit(m_accountPage);
    m_createPasswordConfirmEdit = new QLineEdit(m_accountPage);
    m_createPasswordEdit->setEchoMode(QLineEdit::Password);
    m_createPasswordConfirmEdit->setEchoMode(QLineEdit::Password);
    m_createUserNameEdit->setPlaceholderText(QStringLiteral("输入新用户名"));
    m_createPasswordEdit->setPlaceholderText(QStringLiteral("输入密码"));
    m_createPasswordConfirmEdit->setPlaceholderText(QStringLiteral("再次输入密码"));
    m_createUserNameEdit->setStyleSheet(blueInputStyle());
    m_createPasswordEdit->setStyleSheet(blueInputStyle());
    m_createPasswordConfirmEdit->setStyleSheet(blueInputStyle());
    createLayout->addRow(QStringLiteral("新用户"), m_createUserNameEdit);
    createLayout->addRow(QStringLiteral("密码"), m_createPasswordEdit);
    createLayout->addRow(QStringLiteral("确认密码"), m_createPasswordConfirmEdit);
    m_accountLayout->addLayout(createLayout, 0);

    m_createUserButton = new QPushButton(QIcon(":/Icon/process_start.svg"), QString(), m_accountPage);
    m_createUserButton->setToolTip(QStringLiteral("创建本地用户（会弹出二次确认）"));
    m_createUserButton->setStyleSheet(blueButtonStyle());
    m_createUserButton->setFixedWidth(34);
    m_accountLayout->addWidget(m_createUserButton, 0, Qt::AlignLeft);

    // 重置密码输入区：支持指定账号并确认密码。
    QFormLayout* resetLayout = new QFormLayout();
    m_resetUserNameEdit = new QLineEdit(m_accountPage);
    m_resetPasswordEdit = new QLineEdit(m_accountPage);
    m_resetPasswordConfirmEdit = new QLineEdit(m_accountPage);
    m_resetPasswordEdit->setEchoMode(QLineEdit::Password);
    m_resetPasswordConfirmEdit->setEchoMode(QLineEdit::Password);
    m_resetUserNameEdit->setPlaceholderText(QStringLiteral("输入要重置密码的用户名"));
    m_resetPasswordEdit->setPlaceholderText(QStringLiteral("输入新密码"));
    m_resetPasswordConfirmEdit->setPlaceholderText(QStringLiteral("再次输入新密码"));
    m_resetUserNameEdit->setStyleSheet(blueInputStyle());
    m_resetPasswordEdit->setStyleSheet(blueInputStyle());
    m_resetPasswordConfirmEdit->setStyleSheet(blueInputStyle());
    resetLayout->addRow(QStringLiteral("目标用户"), m_resetUserNameEdit);
    resetLayout->addRow(QStringLiteral("新密码"), m_resetPasswordEdit);
    resetLayout->addRow(QStringLiteral("确认新密码"), m_resetPasswordConfirmEdit);
    m_accountLayout->addLayout(resetLayout, 0);

    m_resetPasswordButton = new QPushButton(QIcon(":/Icon/process_priority.svg"), QString(), m_accountPage);
    m_resetPasswordButton->setToolTip(QStringLiteral("重置用户密码（会弹出二次确认）"));
    m_resetPasswordButton->setStyleSheet(blueButtonStyle());
    m_resetPasswordButton->setFixedWidth(34);
    m_accountLayout->addWidget(m_resetPasswordButton, 0, Qt::AlignLeft);

    m_tabWidget->addTab(m_accountPage, QStringLiteral("账号"));
}

void PrivilegeDock::initializePermissionTab()
{
    m_permissionPage = new QWidget(m_tabWidget);
    m_permissionLayout = new QVBoxLayout(m_permissionPage);
    m_permissionLayout->setContentsMargins(4, 4, 4, 4);
    m_permissionLayout->setSpacing(6);

    m_permissionToolbarLayout = new QHBoxLayout();
    m_permissionToolbarLayout->setContentsMargins(0, 0, 0, 0);
    m_permissionToolbarLayout->setSpacing(6);

    m_permissionRefreshButton = new QPushButton(QIcon(":/Icon/process_refresh.svg"), QString(), m_permissionPage);
    m_permissionRefreshButton->setToolTip(QStringLiteral("刷新用户/组/权限快照"));
    m_permissionRefreshButton->setStyleSheet(blueButtonStyle());
    m_permissionRefreshButton->setFixedWidth(34);

    m_permissionStatusLabel = new QLabel(QStringLiteral("状态：待刷新"), m_permissionPage);
    m_permissionToolbarLayout->addWidget(m_permissionRefreshButton, 0);
    m_permissionToolbarLayout->addWidget(m_permissionStatusLabel, 1);
    m_permissionLayout->addLayout(m_permissionToolbarLayout, 0);

    m_permissionEditor = new CodeEditorWidget(m_permissionPage);
    m_permissionEditor->setReadOnly(true);
    m_permissionLayout->addWidget(m_permissionEditor, 1);

    m_tabWidget->addTab(m_permissionPage, QStringLiteral("权限"));
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
    connect(m_accountTable, &QTableWidget::itemSelectionChanged, this, [this]() {
        const int row = m_accountTable->currentRow();
        if (row < 0 || row >= static_cast<int>(m_localUserList.size()))
        {
            return;
        }
        m_resetUserNameEdit->setText(m_localUserList[static_cast<std::size_t>(row)].name);
    });
}

void PrivilegeDock::refreshLocalUserList()
{
    // accountEvent 复用整条刷新日志链路。
    kLogEvent accountEvent;
    info << accountEvent << "[PrivilegeDock] 开始刷新本地用户列表。" << eol;

    m_localUserList.clear();

    DWORD level = 2;
    DWORD preferedLength = MAX_PREFERRED_LENGTH;
    DWORD entriesRead = 0;
    DWORD totalEntries = 0;
    DWORD resumeHandle = 0;
    LPUSER_INFO_2 userInfo = nullptr;

    const NET_API_STATUS enumStatus = ::NetUserEnum(
        nullptr,
        level,
        FILTER_NORMAL_ACCOUNT,
        reinterpret_cast<LPBYTE*>(&userInfo),
        preferedLength,
        &entriesRead,
        &totalEntries,
        &resumeHandle);
    if (enumStatus != NERR_Success && enumStatus != ERROR_MORE_DATA)
    {
        const QString errorText = winErrorText(enumStatus);
        err << accountEvent
            << "[PrivilegeDock] 刷新用户失败, status="
            << enumStatus
            << ", error="
            << errorText.toStdString()
            << eol;
        m_accountStatusLabel->setText(QStringLiteral("状态：刷新失败 - %1").arg(errorText));
        if (userInfo != nullptr)
        {
            ::NetApiBufferFree(userInfo);
        }
        refreshLocalUserTable();
        return;
    }

    for (DWORD index = 0; index < entriesRead; ++index)
    {
        const USER_INFO_2& infoData = userInfo[index];
        LocalUserEntry entry;
        entry.name = infoData.usri2_name != nullptr
            ? QString::fromWCharArray(infoData.usri2_name)
            : QStringLiteral("<null>");
        entry.fullName = infoData.usri2_full_name != nullptr
            ? QString::fromWCharArray(infoData.usri2_full_name)
            : QString();
        entry.disabled = (infoData.usri2_flags & UF_ACCOUNTDISABLE) != 0;
        entry.lastLogonText = fileTimeToDateTimeText(infoData.usri2_last_logon);
        m_localUserList.push_back(entry);
    }

    if (userInfo != nullptr)
    {
        ::NetApiBufferFree(userInfo);
    }

    refreshLocalUserTable();
    m_accountStatusLabel->setText(QStringLiteral("状态：已加载 %1 / %2 个账号")
        .arg(entriesRead)
        .arg(totalEntries));

    info << accountEvent
        << "[PrivilegeDock] 用户列表刷新完成, entriesRead="
        << entriesRead
        << ", totalEntries="
        << totalEntries
        << eol;
}

void PrivilegeDock::refreshLocalUserTable()
{
    m_accountTable->setRowCount(static_cast<int>(m_localUserList.size()));
    for (int row = 0; row < static_cast<int>(m_localUserList.size()); ++row)
    {
        const LocalUserEntry& entry = m_localUserList[static_cast<std::size_t>(row)];
        m_accountTable->setItem(row, 0, new QTableWidgetItem(entry.name));
        m_accountTable->setItem(row, 1, new QTableWidgetItem(entry.fullName));
        m_accountTable->setItem(row, 2, new QTableWidgetItem(boolText(entry.disabled)));
        m_accountTable->setItem(row, 3, new QTableWidgetItem(entry.lastLogonText));
    }
}

bool PrivilegeDock::createLocalUser(
    const QString& userName,
    const QString& passwordText,
    QString* errorTextOut)
{
    if (errorTextOut != nullptr)
    {
        errorTextOut->clear();
    }

    std::wstring userNameW = userName.toStdWString();
    std::wstring passwordW = passwordText.toStdWString();

    USER_INFO_1 userInfo{};
    userInfo.usri1_name = const_cast<wchar_t*>(userNameW.c_str());
    userInfo.usri1_password = const_cast<wchar_t*>(passwordW.c_str());
    userInfo.usri1_priv = USER_PRIV_USER;
    userInfo.usri1_flags = UF_SCRIPT;

    DWORD paramError = 0;
    const NET_API_STATUS status = ::NetUserAdd(
        nullptr,
        1,
        reinterpret_cast<LPBYTE>(&userInfo),
        &paramError);
    if (status != NERR_Success)
    {
        if (errorTextOut != nullptr)
        {
            *errorTextOut = QStringLiteral("NetUserAdd失败: %1, 参数索引=%2")
                .arg(winErrorText(status))
                .arg(paramError);
        }
        return false;
    }
    return true;
}

bool PrivilegeDock::resetLocalUserPassword(
    const QString& userName,
    const QString& newPassword,
    QString* errorTextOut)
{
    if (errorTextOut != nullptr)
    {
        errorTextOut->clear();
    }

    std::wstring passwordW = newPassword.toStdWString();
    USER_INFO_1003 userInfo{};
    userInfo.usri1003_password = const_cast<wchar_t*>(passwordW.c_str());

    const NET_API_STATUS status = ::NetUserSetInfo(
        nullptr,
        reinterpret_cast<LPCWSTR>(userName.utf16()),
        1003,
        reinterpret_cast<LPBYTE>(&userInfo),
        nullptr);
    if (status != NERR_Success)
    {
        if (errorTextOut != nullptr)
        {
            *errorTextOut = QStringLiteral("NetUserSetInfo失败: %1").arg(winErrorText(status));
        }
        return false;
    }
    return true;
}

void PrivilegeDock::createUserByInputs()
{
    const QString userName = m_createUserNameEdit->text().trimmed();
    const QString password = m_createPasswordEdit->text();
    const QString confirmPassword = m_createPasswordConfirmEdit->text();

    if (userName.isEmpty())
    {
        QMessageBox::warning(this, QStringLiteral("创建用户"), QStringLiteral("用户名不能为空。"));
        return;
    }
    if (password.isEmpty())
    {
        QMessageBox::warning(this, QStringLiteral("创建用户"), QStringLiteral("密码不能为空。"));
        return;
    }
    if (password != confirmPassword)
    {
        QMessageBox::warning(this, QStringLiteral("创建用户"), QStringLiteral("两次密码输入不一致。"));
        return;
    }

    // 二次确认：避免误触导致系统新增账号。
    const int confirm = QMessageBox::question(
        this,
        QStringLiteral("创建用户"),
        QStringLiteral("确定创建本地用户“%1”吗？").arg(userName),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);
    if (confirm != QMessageBox::Yes)
    {
        return;
    }

    kLogEvent actionEvent;
    info << actionEvent
        << "[PrivilegeDock] 创建用户请求, user="
        << userName.toStdString()
        << eol;

    QString errorText;
    if (!createLocalUser(userName, password, &errorText))
    {
        err << actionEvent
            << "[PrivilegeDock] 创建用户失败, user="
            << userName.toStdString()
            << ", error="
            << errorText.toStdString()
            << eol;
        QMessageBox::warning(this, QStringLiteral("创建用户"), errorText);
        return;
    }

    info << actionEvent
        << "[PrivilegeDock] 创建用户成功, user="
        << userName.toStdString()
        << eol;
    QMessageBox::information(this, QStringLiteral("创建用户"), QStringLiteral("创建成功。"));
    refreshLocalUserList();
    refreshPermissionSnapshot();
}

void PrivilegeDock::resetPasswordByInputs()
{
    const QString userName = m_resetUserNameEdit->text().trimmed();
    const QString password = m_resetPasswordEdit->text();
    const QString confirmPassword = m_resetPasswordConfirmEdit->text();

    if (userName.isEmpty())
    {
        QMessageBox::warning(this, QStringLiteral("重置密码"), QStringLiteral("目标用户名不能为空。"));
        return;
    }
    if (password.isEmpty())
    {
        QMessageBox::warning(this, QStringLiteral("重置密码"), QStringLiteral("新密码不能为空。"));
        return;
    }
    if (password != confirmPassword)
    {
        QMessageBox::warning(this, QStringLiteral("重置密码"), QStringLiteral("两次密码输入不一致。"));
        return;
    }

    const int confirm = QMessageBox::question(
        this,
        QStringLiteral("重置密码"),
        QStringLiteral("确定为“%1”重置密码吗？").arg(userName),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);
    if (confirm != QMessageBox::Yes)
    {
        return;
    }

    kLogEvent actionEvent;
    info << actionEvent
        << "[PrivilegeDock] 重置密码请求, user="
        << userName.toStdString()
        << eol;

    QString errorText;
    if (!resetLocalUserPassword(userName, password, &errorText))
    {
        err << actionEvent
            << "[PrivilegeDock] 重置密码失败, user="
            << userName.toStdString()
            << ", error="
            << errorText.toStdString()
            << eol;
        QMessageBox::warning(this, QStringLiteral("重置密码"), errorText);
        return;
    }

    info << actionEvent
        << "[PrivilegeDock] 重置密码成功, user="
        << userName.toStdString()
        << eol;
    QMessageBox::information(this, QStringLiteral("重置密码"), QStringLiteral("重置成功。"));
}

void PrivilegeDock::refreshPermissionSnapshot()
{
    kLogEvent event;
    info << event << "[PrivilegeDock] 开始刷新权限快照。" << eol;

    QString outputText;
    outputText += QStringLiteral("==== 本地用户与组 ====\n");
    outputText += buildLocalUserAndGroupText();
    outputText += QStringLiteral("\n\n==== 当前进程权限 ====\n");
    outputText += buildCurrentProcessPrivilegeText();

    m_permissionEditor->setText(outputText);
    m_permissionStatusLabel->setText(
        QStringLiteral("状态：%1 刷新完成")
        .arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss"))));

    info << event << "[PrivilegeDock] 权限快照刷新完成。" << eol;
}

QString PrivilegeDock::buildLocalUserAndGroupText() const
{
    QString outputText;
    outputText += QStringLiteral("[用户列表]\n");
    for (const LocalUserEntry& entry : m_localUserList)
    {
        outputText += QStringLiteral("- 用户名: %1, 全名: %2, 禁用: %3, 最后登录: %4\n")
            .arg(entry.name)
            .arg(entry.fullName.isEmpty() ? QStringLiteral("<空>") : entry.fullName)
            .arg(boolText(entry.disabled))
            .arg(entry.lastLogonText);
    }

    outputText += QStringLiteral("\n[本地组列表]\n");
    LPLOCALGROUP_INFO_1 groupInfo = nullptr;
    DWORD entriesRead = 0;
    DWORD totalEntries = 0;
    DWORD_PTR resumeHandle = 0;
    const NET_API_STATUS status = ::NetLocalGroupEnum(
        nullptr,
        1,
        reinterpret_cast<LPBYTE*>(&groupInfo),
        MAX_PREFERRED_LENGTH,
        &entriesRead,
        &totalEntries,
        &resumeHandle);
    if (status == NERR_Success || status == ERROR_MORE_DATA)
    {
        for (DWORD index = 0; index < entriesRead; ++index)
        {
            const LOCALGROUP_INFO_1& group = groupInfo[index];
            const QString groupName = group.lgrpi1_name != nullptr
                ? QString::fromWCharArray(group.lgrpi1_name)
                : QStringLiteral("<null>");
            const QString groupComment = group.lgrpi1_comment != nullptr
                ? QString::fromWCharArray(group.lgrpi1_comment)
                : QString();
            outputText += QStringLiteral("- 组名: %1, 说明: %2\n")
                .arg(groupName)
                .arg(groupComment.isEmpty() ? QStringLiteral("<空>") : groupComment);
        }
        if (groupInfo != nullptr)
        {
            ::NetApiBufferFree(groupInfo);
        }
    }
    else
    {
        outputText += QStringLiteral("读取组列表失败: %1\n").arg(winErrorText(status));
    }

    return outputText;
}

QString PrivilegeDock::buildCurrentProcessPrivilegeText() const
{
    HANDLE tokenHandle = nullptr;
    if (::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &tokenHandle) == FALSE)
    {
        return QStringLiteral("OpenProcessToken失败: %1").arg(winErrorText(::GetLastError()));
    }

    DWORD bytesNeeded = 0;
    ::GetTokenInformation(tokenHandle, TokenPrivileges, nullptr, 0, &bytesNeeded);
    if (bytesNeeded == 0)
    {
        ::CloseHandle(tokenHandle);
        return QStringLiteral("GetTokenInformation失败: %1").arg(winErrorText(::GetLastError()));
    }

    std::vector<unsigned char> tokenBuffer(bytesNeeded, 0);
    if (::GetTokenInformation(
        tokenHandle,
        TokenPrivileges,
        tokenBuffer.data(),
        bytesNeeded,
        &bytesNeeded) == FALSE)
    {
        const QString errorText = winErrorText(::GetLastError());
        ::CloseHandle(tokenHandle);
        return QStringLiteral("GetTokenInformation失败: %1").arg(errorText);
    }

    TOKEN_PRIVILEGES* privileges = reinterpret_cast<TOKEN_PRIVILEGES*>(tokenBuffer.data());
    QString outputText;
    outputText += QStringLiteral("权限数量: %1\n").arg(privileges->PrivilegeCount);

    for (DWORD index = 0; index < privileges->PrivilegeCount; ++index)
    {
        const LUID_AND_ATTRIBUTES& privilege = privileges->Privileges[index];
        wchar_t nameBuffer[256] = {};
        DWORD nameLength = static_cast<DWORD>(std::size(nameBuffer));
        QString privilegeName = QStringLiteral("<LookupPrivilegeName失败>");
        if (::LookupPrivilegeNameW(nullptr, const_cast<PLUID>(&privilege.Luid), nameBuffer, &nameLength) != FALSE)
        {
            privilegeName = QString::fromWCharArray(nameBuffer, static_cast<int>(nameLength));
        }

        wchar_t displayNameBuffer[512] = {};
        DWORD displayNameLength = static_cast<DWORD>(std::size(displayNameBuffer));
        DWORD languageId = 0;
        QString displayName = QStringLiteral("<无显示名>");
        if (::LookupPrivilegeDisplayNameW(
            nullptr,
            reinterpret_cast<LPCWSTR>(privilegeName.utf16()),
            displayNameBuffer,
            &displayNameLength,
            &languageId) != FALSE)
        {
            Q_UNUSED(languageId);
            displayName = QString::fromWCharArray(displayNameBuffer, static_cast<int>(displayNameLength));
        }

        const bool enabled = (privilege.Attributes & SE_PRIVILEGE_ENABLED) != 0;
        const bool enabledByDefault = (privilege.Attributes & SE_PRIVILEGE_ENABLED_BY_DEFAULT) != 0;
        const bool removed = (privilege.Attributes & SE_PRIVILEGE_REMOVED) != 0;

        outputText += QStringLiteral("- %1 (%2)\n")
            .arg(privilegeName)
            .arg(displayName);
        outputText += QStringLiteral("  启用: %1, 默认启用: %2, 已移除: %3, Attributes=0x%4\n")
            .arg(boolText(enabled))
            .arg(boolText(enabledByDefault))
            .arg(boolText(removed))
            .arg(QString::number(privilege.Attributes, 16).toUpper());
    }

    ::CloseHandle(tokenHandle);
    return outputText;
}

QString PrivilegeDock::fileTimeToDateTimeText(const std::uint32_t secondsSince1970) const
{
    if (secondsSince1970 == 0)
    {
        return QStringLiteral("从未");
    }
    return QDateTime::fromSecsSinceEpoch(secondsSince1970).toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
}

QString PrivilegeDock::winErrorText(const DWORD code) const
{
    wchar_t* messageBuffer = nullptr;
    const DWORD messageLength = ::FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        code,
        0,
        reinterpret_cast<LPWSTR>(&messageBuffer),
        0,
        nullptr);
    QString messageText;
    if (messageLength > 0 && messageBuffer != nullptr)
    {
        messageText = QString::fromWCharArray(messageBuffer).trimmed();
        ::LocalFree(messageBuffer);
    }
    if (messageText.isEmpty())
    {
        return QStringLiteral("错误码 %1").arg(code);
    }
    return QStringLiteral("%1 (code=%2)").arg(messageText).arg(code);
}
