#pragma once

#include <QJsonObject>
#include <QString>
#include <functional>

class QWidget;
namespace ks::i18n { class SourceTextSnapshot; }

namespace ks::privilege
{
    QWidget* createAccountManagementPage(QWidget* parent,
        std::function<void(const QString& target, const QString& accountOrSid)> navigate = {});
    QWidget* createGroupsPage(QWidget* parent);
    QWidget* createRightsPage(QWidget* parent);
    void selectGroupAccount(QWidget* page, const QString& account);
    void selectRightsAccount(QWidget* page, const QString& account);

    // These backends perform Windows calls synchronously; invoke from a worker, never the UI.
    // The account setter preserves other USER_INFO flags and verifies the resulting disabled bit.
    bool setLocalAccountEnabled(const QString& account, bool enabled, QString* error);
    // Canonical stable SID keys, JSON scalar values, and explicit partial-enumeration errors.
    QJsonObject captureAccountPolicySnapshot(const ks::i18n::SourceTextSnapshot* translator = nullptr);
}
