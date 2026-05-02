#include "KernelDock.CallbackIntercept.h"

#include <QHash>
#include <QSet>

namespace
{
    bool isSupportedCallbackType(const quint32 callbackType)
    {
        return callbackType == KSWORD_ARK_CALLBACK_TYPE_REGISTRY ||
            callbackType == KSWORD_ARK_CALLBACK_TYPE_PROCESS_CREATE ||
            callbackType == KSWORD_ARK_CALLBACK_TYPE_THREAD_CREATE ||
            callbackType == KSWORD_ARK_CALLBACK_TYPE_IMAGE_LOAD ||
            callbackType == KSWORD_ARK_CALLBACK_TYPE_OBJECT ||
            callbackType == KSWORD_ARK_CALLBACK_TYPE_MINIFILTER;
    }

    bool isSupportedMatchMode(const quint32 matchMode)
    {
        return matchMode == KSWORD_ARK_MATCH_MODE_EXACT ||
            matchMode == KSWORD_ARK_MATCH_MODE_PREFIX ||
            matchMode == KSWORD_ARK_MATCH_MODE_WILDCARD ||
            matchMode == KSWORD_ARK_MATCH_MODE_REGEX;
    }

    bool isActionValidForType(
        const quint32 callbackType,
        const quint32 actionType,
        const quint32 matchMode)
    {
        switch (callbackType)
        {
        case KSWORD_ARK_CALLBACK_TYPE_REGISTRY:
            if (actionType != KSWORD_ARK_RULE_ACTION_ALLOW &&
                actionType != KSWORD_ARK_RULE_ACTION_DENY &&
                actionType != KSWORD_ARK_RULE_ACTION_ASK_USER &&
                actionType != KSWORD_ARK_RULE_ACTION_LOG_ONLY)
            {
                return false;
            }
            if (matchMode == KSWORD_ARK_MATCH_MODE_REGEX &&
                actionType != KSWORD_ARK_RULE_ACTION_ASK_USER)
            {
                return false;
            }
            return true;

        case KSWORD_ARK_CALLBACK_TYPE_PROCESS_CREATE:
            return actionType == KSWORD_ARK_RULE_ACTION_ALLOW ||
                actionType == KSWORD_ARK_RULE_ACTION_DENY ||
                actionType == KSWORD_ARK_RULE_ACTION_LOG_ONLY;

        case KSWORD_ARK_CALLBACK_TYPE_THREAD_CREATE:
        case KSWORD_ARK_CALLBACK_TYPE_IMAGE_LOAD:
            return actionType == KSWORD_ARK_RULE_ACTION_LOG_ONLY;

        case KSWORD_ARK_CALLBACK_TYPE_OBJECT:
            return actionType == KSWORD_ARK_RULE_ACTION_ALLOW ||
                actionType == KSWORD_ARK_RULE_ACTION_STRIP_ACCESS ||
                actionType == KSWORD_ARK_RULE_ACTION_LOG_ONLY;

        case KSWORD_ARK_CALLBACK_TYPE_MINIFILTER:
            if (actionType != KSWORD_ARK_RULE_ACTION_ALLOW &&
                actionType != KSWORD_ARK_RULE_ACTION_DENY &&
                actionType != KSWORD_ARK_RULE_ACTION_ASK_USER &&
                actionType != KSWORD_ARK_RULE_ACTION_LOG_ONLY)
            {
                return false;
            }
            if (matchMode == KSWORD_ARK_MATCH_MODE_REGEX &&
                actionType != KSWORD_ARK_RULE_ACTION_ASK_USER)
            {
                return false;
            }
            return true;

        default:
            return false;
        }
    }
}

CallbackValidationResult validateCallbackConfig(const CallbackConfigDocument& configDocument)
{
    CallbackValidationResult result;
    QSet<quint32> groupIdSet;
    QSet<quint32> ruleIdSet;
    QHash<quint32, CallbackRuleGroupModel> groupById;

    if (configDocument.schemaVersion != KSWORD_ARK_CALLBACK_RULE_SCHEMA_VERSION)
    {
        result.errorList.push_back(
            QStringLiteral("schemaVersion=%1 与当前版本 %2 不兼容。")
            .arg(configDocument.schemaVersion)
            .arg(KSWORD_ARK_CALLBACK_RULE_SCHEMA_VERSION));
    }

    for (const CallbackRuleGroupModel& groupModel : configDocument.groups)
    {
        if (groupModel.groupId == 0U)
        {
            result.errorList.push_back(QStringLiteral("存在 groupId=0 的规则组。"));
            continue;
        }
        if (groupIdSet.contains(groupModel.groupId))
        {
            result.errorList.push_back(QStringLiteral("规则组 ID 重复：%1").arg(groupModel.groupId));
            continue;
        }
        groupIdSet.insert(groupModel.groupId);
        groupById.insert(groupModel.groupId, groupModel);

        if (groupModel.groupName.trimmed().isEmpty())
        {
            result.warningList.push_back(QStringLiteral("规则组 %1 名称为空。").arg(groupModel.groupId));
        }
    }

    for (const CallbackRuleModel& ruleModel : configDocument.rules)
    {
        if (ruleModel.ruleId == 0U)
        {
            result.errorList.push_back(QStringLiteral("存在 ruleId=0 的规则。"));
            continue;
        }
        if (ruleIdSet.contains(ruleModel.ruleId))
        {
            result.errorList.push_back(QStringLiteral("规则 ID 重复：%1").arg(ruleModel.ruleId));
            continue;
        }
        ruleIdSet.insert(ruleModel.ruleId);

        if (!groupById.contains(ruleModel.groupId))
        {
            result.errorList.push_back(
                QStringLiteral("规则 %1 引用了不存在的 groupId=%2。")
                .arg(ruleModel.ruleId)
                .arg(ruleModel.groupId));
        }

        if (!isSupportedCallbackType(ruleModel.callbackType))
        {
            result.errorList.push_back(
                QStringLiteral("规则 %1 callbackType=%2 不受支持。")
                .arg(ruleModel.ruleId)
                .arg(ruleModel.callbackType));
        }
        if (!isSupportedMatchMode(ruleModel.matchMode))
        {
            result.errorList.push_back(
                QStringLiteral("规则 %1 matchMode=%2 不受支持。")
                .arg(ruleModel.ruleId)
                .arg(ruleModel.matchMode));
        }
        if (!isActionValidForType(ruleModel.callbackType, ruleModel.action, ruleModel.matchMode))
        {
            result.errorList.push_back(
                QStringLiteral("规则 %1 动作组合非法：%2 + %3 + %4。")
                .arg(ruleModel.ruleId)
                .arg(callbackTypeToDisplayText(ruleModel.callbackType))
                .arg(callbackActionToDisplayText(ruleModel.action))
                .arg(callbackMatchModeToDisplayText(ruleModel.matchMode)));
        }

        if (ruleModel.matchMode == KSWORD_ARK_MATCH_MODE_REGEX &&
            !((ruleModel.callbackType == KSWORD_ARK_CALLBACK_TYPE_REGISTRY ||
                ruleModel.callbackType == KSWORD_ARK_CALLBACK_TYPE_MINIFILTER) &&
                ruleModel.action == KSWORD_ARK_RULE_ACTION_ASK_USER))
        {
            result.errorList.push_back(
                QStringLiteral("规则 %1 使用 Regex 仅允许“注册表/文件系统微过滤器 + 询问用户”。")
                .arg(ruleModel.ruleId));
        }

        if (ruleModel.action == KSWORD_ARK_RULE_ACTION_ASK_USER &&
            ruleModel.callbackType != KSWORD_ARK_CALLBACK_TYPE_REGISTRY &&
            ruleModel.callbackType != KSWORD_ARK_CALLBACK_TYPE_MINIFILTER)
        {
            result.errorList.push_back(
                QStringLiteral("规则 %1 使用“询问用户”仅允许注册表或文件系统微过滤器回调。")
                .arg(ruleModel.ruleId));
        }

        if (ruleModel.action == KSWORD_ARK_RULE_ACTION_ASK_USER)
        {
            if (ruleModel.timeoutMs == 0U)
            {
                result.errorList.push_back(QStringLiteral("规则 %1 询问超时必须大于 0。").arg(ruleModel.ruleId));
            }
            if (ruleModel.timeoutDefaultDecision != KSWORD_ARK_DECISION_ALLOW &&
                ruleModel.timeoutDefaultDecision != KSWORD_ARK_DECISION_DENY)
            {
                result.errorList.push_back(
                    QStringLiteral("规则 %1 询问超时默认决策非法：%2")
                    .arg(ruleModel.ruleId)
                    .arg(ruleModel.timeoutDefaultDecision));
            }
        }
        else
        {
            if (ruleModel.timeoutMs != 0U &&
                ruleModel.timeoutMs != 5000U)
            {
                result.warningList.push_back(
                    QStringLiteral("规则 %1 非询问动作设置了 timeoutMs=%2，将被忽略。")
                    .arg(ruleModel.ruleId)
                    .arg(ruleModel.timeoutMs));
            }
        }

        if (ruleModel.ruleName.trimmed().isEmpty())
        {
            result.warningList.push_back(QStringLiteral("规则 %1 名称为空。").arg(ruleModel.ruleId));
        }
    }

    result.success = result.errorList.isEmpty();
    return result;
}
