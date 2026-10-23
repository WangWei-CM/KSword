#include "PrivilegeDock/PrivilegeLegacyValueOperations.h"
#include <QCoreApplication>
#include <cstdio>
#include <stdexcept>
using namespace ks::privilege::legacy;
int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    int checks = 0, failures = 0;
    const auto check = [&](bool value) { ++checks; if (!value) ++failures; };
    for (const int total : {1000, 10000, 100000})
    {
        int userReads = 0, groupReads = 0, tokenReads = 0;
        ReadPorts ports;
        ports.error = [](std::uint32_t code) { return QString::number(code); };
        ports.users = [&](std::uint64_t resume, std::uint32_t length)
        {
            ++userReads; check(length == 4096);
            ValuePage<LocalUser> page;
            const auto end = std::min<std::uint64_t>(resume + 251, static_cast<std::uint64_t>(total));
            for (auto i = resume; i < end; ++i)
                page.rows.push_back({QStringLiteral("user_%1").arg(i), QStringLiteral("full_%1").arg(i),
                    QStringLiteral("comment_%1").arg(i), static_cast<std::uint32_t>(i), static_cast<std::uint32_t>(i + 1)});
            page.resume = end; page.total = static_cast<std::uint64_t>(total);
            page.code = end < static_cast<std::uint64_t>(total) ? MoreData : Success;
            return page;
        };
        ports.groups = [&](std::uint64_t, std::uint32_t) { ++groupReads; return ValuePage<LocalGroup>{}; };
        ports.token = [&](const CancelCheck&) { ++tokenReads; return TokenRead{}; };
        const auto snapshot = collectSnapshot({Users,4096}, ports);
        check(snapshot.userStatus.complete && snapshot.userStatus.code == Success);
        check(snapshot.users.size() == static_cast<std::size_t>(total));
        check(snapshot.totalUsers == static_cast<std::uint64_t>(total));
        check(snapshot.users.back().name == QStringLiteral("user_%1").arg(total - 1));
        check(snapshot.users.back().fullName == QStringLiteral("full_%1").arg(total - 1));
        check(snapshot.users.back().comment == QStringLiteral("comment_%1").arg(total - 1));
        check(snapshot.users.back().flags == static_cast<std::uint32_t>(total - 1));
        check(snapshot.users.back().lastLogon == static_cast<std::uint32_t>(total));
        check(userReads > 1 && groupReads == 0 && tokenReads == 0);
        const auto stopped = collectSnapshot({Users,4096}, ports, [] { return true; });
        check(stopped.userStatus.code == Canceled && stopped.users.empty());
    }
    ReadPorts broken;
    broken.error = [](std::uint32_t value) { return QStringLiteral("native_%1").arg(value); };
    broken.users = [](std::uint64_t, std::uint32_t) {
        return ValuePage<LocalUser>{{LocalUser{QStringLiteral("same"), {}, {}, 5, 99}},0,10,MoreData};
    };
    auto partial = collectSnapshot({Users}, broken);
    check(partial.users.size() == 1 && !partial.userStatus.complete);
    check(partial.userStatus.stage == FailureStage::Pagination && partial.userStatus.code == InvalidData);
    check(partial.userStatus.nativeError == QStringLiteral("native_13"));
    int pages = 0;
    broken.users = [&](std::uint64_t, std::uint32_t) {
        if (++pages == 1) return ValuePage<LocalUser>{{LocalUser{QStringLiteral("retained")}},1,20,MoreData};
        return ValuePage<LocalUser>{{},1,20,5};
    };
    partial = collectSnapshot({Users}, broken);
    check(partial.users.size() == 1 && partial.users.front().name == QStringLiteral("retained"));
    check(partial.userStatus.code == 5 && !partial.userStatus.complete);
    int createCalls = 0, resetCalls = 0, reads = 0, identityCalls = 0;
    const UserIdentity original{QStringLiteral("local_user"), QByteArray("SID_A"),0,true};
    UserIdentity current = original;
    MutationPorts operations;
    operations.identity = [&](const QString&) { ++identityCalls; return current; };
    operations.create = [&](const QString& name, const std::wstring& password) {
        ++createCalls; check(name == original.name && password == L"fake-secret"); return WriteStatus{0,0};
    };
    operations.reset = [&](const QString& name, const std::wstring& password) {
        ++resetCalls; check(name == original.name && password == L"fake-secret"); return WriteStatus{0,0};
    };
    operations.readback = [&]() { ++reads; return ReadSnapshot{}; };
    MutationRequest request{MutationKind::CreateUser,original.name,std::make_shared<PasswordValue>(L"fake-secret"),{}};
    auto receipt = executeMutation(request,operations);
    check(receipt.succeeded() && createCalls == 1 && resetCalls == 0 && reads == 1 && identityCalls == 0);
    check(request.password->value().empty() && receipt.after != nullptr);
    request = {MutationKind::ResetPassword,original.name,std::make_shared<PasswordValue>(L"fake-secret"),original};
    receipt = executeMutation(request,operations);
    check(receipt.succeeded() && resetCalls == 1 && reads == 2 && identityCalls == 1);
    check(request.password->value().empty());
    current.sid = QByteArray("SID_REPLACED");
    request.password = std::make_shared<PasswordValue>(L"fake-secret");
    receipt = executeMutation(request,operations);
    check(!receipt.writeAttempted && receipt.identityChanged && resetCalls == 1 && reads == 3);
    check(request.password->value().empty());
    current = original;
    operations.reset = [&](const QString&,const std::wstring&) { ++resetCalls; return WriteStatus{5,0}; };
    request.password = std::make_shared<PasswordValue>(L"fake-secret");
    receipt = executeMutation(request,operations);
    check(!receipt.succeeded() && receipt.writeAttempted && receipt.status.code == 5 && reads == 4 && resetCalls == 2);
    operations.reset = [&](const QString&,const std::wstring&) -> WriteStatus { ++resetCalls; throw std::runtime_error("fake"); };
    request.password = std::make_shared<PasswordValue>(L"fake-secret");
    receipt = executeMutation(request,operations);
    check(!receipt.outcomeKnown && receipt.writeAttempted && resetCalls == 3 && reads == 5);
    check(request.password->value().empty());
    operations.readback = []() -> ReadSnapshot { throw std::runtime_error("fake readback"); };
    request.password = std::make_shared<PasswordValue>(L"fake-secret");
    operations.reset = [](const QString&,const std::wstring&) { return WriteStatus{}; };
    receipt = executeMutation(request,operations);
    check(receipt.succeeded() && receipt.readbackFailed && receipt.after == nullptr);
    check(request.password->value().empty());
    current = original;
    current.valid = false; current.code = 5;
    request.password = std::make_shared<PasswordValue>(L"fake-secret");
    receipt = executeMutation(request,operations);
    check(!receipt.writeAttempted && !receipt.identityChanged && receipt.status.code == 5);
    check(request.password->value().empty());
    std::printf("PRIVILEGE_LEGACY_VALUE_CHECKS=%d FAILURES=%d GUI=0 NATIVE_ACCOUNT_TOKEN_CALLS=0\n", checks, failures);
    return failures ? 1 : 0;
}
