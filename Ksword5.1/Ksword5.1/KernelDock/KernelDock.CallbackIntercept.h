#pragma once

#include "../Framework.h"

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

#include "../../../shared/driver/KswordArkCallbackIoctl.h"

struct CallbackRuleGroupModel
{
    quint32 groupId = 0;
    QString groupName;
    bool enabled = true;
    qint32 priority = 0;
    QString comment;
};

struct CallbackRuleModel
{
    quint32 ruleId = 0;
    quint32 groupId = 0;
    QString ruleName;
    bool enabled = true;
    quint32 callbackType = KSWORD_ARK_CALLBACK_TYPE_REGISTRY;
    quint32 operationMask = 0;
    QString initiatorPattern;
    QString targetPattern;
    quint32 matchMode = KSWORD_ARK_MATCH_MODE_EXACT;
    quint32 action = KSWORD_ARK_RULE_ACTION_ALLOW;
    quint32 timeoutMs = 5000;
    quint32 timeoutDefaultDecision = KSWORD_ARK_DECISION_ALLOW;
    qint32 priority = 0;
    QString comment;
};

struct CallbackConfigDocument
{
    quint32 schemaVersion = KSWORD_ARK_CALLBACK_RULE_SCHEMA_VERSION;
    QString exportedAtUtc;
    QString appVersion;
    bool globalEnabled = true;
    quint64 ruleVersion = 1;
    QList<CallbackRuleGroupModel> groups;
    QList<CallbackRuleModel> rules;
    quint32 crc32 = 0;
};

struct CallbackValidationResult
{
    bool success = false;
    QStringList errorList;
    QStringList warningList;
};

QString callbackTypeToDisplayText(quint32 callbackType);
QString callbackActionToDisplayText(quint32 actionType);
QString callbackMatchModeToDisplayText(quint32 matchMode);
QString callbackDecisionToDisplayText(quint32 decision);
QString callbackGuidToString(const KSWORD_ARK_GUID128& guidValue);

CallbackValidationResult validateCallbackConfig(const CallbackConfigDocument& configDocument);

bool exportCallbackConfigToJson(
    const CallbackConfigDocument& configDocument,
    QByteArray* jsonOut,
    QString* errorTextOut);

bool importCallbackConfigFromJson(
    const QByteArray& jsonBytes,
    CallbackConfigDocument* configOut,
    QStringList* warningListOut,
    QString* errorTextOut);

bool buildCallbackRuleBlobFromConfig(
    const CallbackConfigDocument& configDocument,
    QByteArray* blobOut,
    QString* errorTextOut);

quint32 computeCallbackCrc32(const QByteArray& rawBytes);

