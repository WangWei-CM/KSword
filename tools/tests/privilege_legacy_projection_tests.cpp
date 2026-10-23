// Core-only snapshot/model fixture. Production DTO builder/formatters/proxy are exact excerpts.
#include "PrivilegeDock/PrivilegeLegacyReadService.h"
#include "UI/FlatTableModel.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QSortFilterProxyModel>
#include <QPersistentModelIndex>
#include <cstdio>
#include <memory>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <Lm.h>
namespace ks::i18n {
struct LanguageManager::State {};
LanguageManager::LanguageManager() = default;
LanguageManager::~LanguageManager() = default;
LanguageManager& LanguageManager::instance() { static LanguageManager value; return value; }
// Deliberately hostile translation proves business values bypass this generic source-text layer.
QString LanguageManager::sourceText(const QString& value) const { return QStringLiteral("WRONG:") + value; }
QString LanguageManager::displayText(const QString& value) const { return value; }
bool LanguageManager::eventFilter(QObject*, QEvent*) { return false; }
}
#include "privilege_legacy_projection_helpers.inc"
int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    int checks = 0, failures = 0;
    const auto check = [&](bool value) { ++checks; if (!value) ++failures; };
    using namespace ks::privilege::legacy;
    PrivilegeDock dock;
    for (const int count : {1000,10000,100000})
    {
        auto snapshot = std::make_shared<ReadSnapshot>();
        snapshot->userStatus = {true,true,0,FailureStage::None,{}};
        snapshot->groupStatus = snapshot->tokenStatus = snapshot->userStatus;
        snapshot->users.reserve(count);
        for (int i = 0; i < count; ++i)
            snapshot->users.push_back({QStringLiteral("user_%1").arg(count - i,6,10,QChar('0')),
                QStringLiteral("full_%1").arg(i),{},i%2 ? UF_ACCOUNTDISABLE : 0U,static_cast<std::uint32_t>(i)});
        snapshot->groups.push_back({QStringLiteral("Administrators"),QStringLiteral("group detail")});
        snapshot->privileges.push_back({QStringLiteral("SeDebugPrivilege"),QStringLiteral("OS native display"),7,true,true});
        auto result = PrivilegeDock::makeReadResult(snapshot);
        check(result->rows.size() == static_cast<std::size_t>(count + 2));
        int resolved = 0;
        using AccountModel = ks::ui::FlatTableModel<LocalUser>;
        AccountModel accounts({{QStringLiteral("n")},{QStringLiteral("f")},{QStringLiteral("d")},{QStringLiteral("t")}},
            [&](const LocalUser& row,int column,int role) { ++resolved; return dock.accountCell(row,column,role); });
        const auto* storage = snapshot->users.data();
        accounts.setRows(std::move(snapshot->users));
        check(accounts.rowCount() == count && accounts.rows().data() == storage && resolved == 0);
        LegacyHeaderProxy proxy([](int col) { return QString::number(col); },nullptr);
        proxy.setSourceModel(&accounts); proxy.setSortRole(Qt::UserRole+1);
        check(proxy.index(0,0).data().toString() == QStringLiteral("user_%1").arg(count,6,10,QChar('0')));
        check(accounts.index(0,0).data().toString().startsWith(QStringLiteral("WRONG:")));
        proxy.sort(0,Qt::AscendingOrder);
        check(proxy.index(0,0).data().toString() == QStringLiteral("user_000001"));
        const auto source = proxy.mapToSource(proxy.index(0,0));
        check(accounts.rowAt(source.row())->fullName == QStringLiteral("full_%1").arg(count - 1));
        check(proxy.index(0,0).data(Qt::UserRole).toString() == accounts.rowAt(source.row())->name);
        const auto rawName = proxy.index(0,0).data().toString();
        const auto oldDisabled = proxy.index(0,2).data().toString();
        fixtureEnglish = true;
        emit accounts.dataChanged(accounts.index(0,0),accounts.index(count-1,3));
        check(proxy.index(0,0).data().toString() == rawName);
        check(proxy.index(0,2).data().toString().startsWith(QStringLiteral("EN:")));
        check(proxy.index(0,2).data().toString() != oldDisabled);
        fixtureEnglish = false;
        using PermissionModel = ks::ui::FlatTableModel<PrivilegeDock::PermissionSnapshotRow>;
        PermissionModel permissions({{QStringLiteral("t")},{QStringLiteral("n")},{QStringLiteral("s")},{QStringLiteral("d")}},
            [&](const PrivilegeDock::PermissionSnapshotRow& row,int col,int role) -> QVariant {
                return role == Qt::DisplayRole || role == Qt::UserRole+1 ? dock.permissionCell(row,col) : QVariant(); });
        permissions.setRows(std::move(result->rows));
        LegacyHeaderProxy values([](int col) { return QString::number(col); },nullptr);
        values.setSourceModel(&permissions); values.setSortRole(Qt::UserRole+1);
        check(values.rowCount() == count + 2);
        check(values.index(count,1).data().toString() == QStringLiteral("Administrators"));
        check(values.index(count,3).data().toString() == QStringLiteral("group detail"));
        check(values.index(count+1,1).data().toString() == QStringLiteral("SeDebugPrivilege"));
        check(values.index(count+1,2).data().toString() == QStringLiteral("启用:是 默认:是 移除:是"));
        check(values.index(count+1,3).data().toString() == QStringLiteral("OS native display, Attributes=0x7"));
        QPersistentModelIndex old(proxy.index(0,0)); accounts.clearRows();
        check(!old.isValid() && proxy.rowCount() == 0);
    }
    auto partial = std::make_shared<ReadSnapshot>();
    partial->userStatus={true,false,5,FailureStage::Enumeration,QStringLiteral("denied users")};
    partial->groupStatus={true,false,5,FailureStage::Enumeration,QStringLiteral("denied groups")};
    partial->tokenStatus={true,false,5,FailureStage::OpenToken,QStringLiteral("denied token")};
    auto errors = PrivilegeDock::makeReadResult(partial);
    check(errors->rows.size() == 3);
    check(dock.permissionCell(errors->rows[0],3).toString() == QStringLiteral("denied users"));
    check(dock.permissionCell(errors->rows[1],3).toString() == QStringLiteral("denied groups"));
    check(dock.permissionCell(errors->rows[2],3).toString() == QStringLiteral("OpenProcessToken失败: denied token"));
    auto empty = std::make_shared<ReadSnapshot>(); empty->tokenStatus={true,true,0,FailureStage::None,{}};
    auto noPrivileges = PrivilegeDock::makeReadResult(empty);
    check(noPrivileges->rows.size() == 1);
    check(dock.permissionCell(noPrivileges->rows[0],2).toString() == QStringLiteral("无权限项"));
    LocalUser longUser{QStringLiteral("Enabled"),QString(1024*1024,QChar('X'))+QStringLiteral("TAIL"),{},UF_ACCOUNTDISABLE,0};
    check(dock.accountCell(longUser,0,Qt::UserRole+1).toString() == QStringLiteral("Enabled"));
    check(dock.accountCell(longUser,1,Qt::UserRole+1).toString().endsWith(QStringLiteral("TAIL")));
    check(dock.accountCell(longUser,1,Qt::UserRole+1).toString().size() == 1024*1024+4);
    check(dock.accountCell(longUser,3,Qt::UserRole+1).toString() == QStringLiteral("从未"));
    ReadStatus unknown{true,false,0xDEADU,FailureStage::Enumeration,QString::number(0xDEADU)};
    check(dock.readErrorText(unknown) == QStringLiteral("错误码 57005"));
    fixtureEnglish = true;
    check(dock.readErrorText(unknown) == QStringLiteral("EN:错误码 57005"));
    std::printf("PRIVILEGE_LEGACY_PROJECTION_CHECKS=%d FAILURES=%d GUI=0 NATIVE_ACCOUNT_TOKEN_CALLS=0\n",checks,failures);
    return failures ? 1 : 0;
}
