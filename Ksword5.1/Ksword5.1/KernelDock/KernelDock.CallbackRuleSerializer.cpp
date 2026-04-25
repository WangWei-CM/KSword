#include "KernelDock.CallbackIntercept.h"

#include <QDateTime>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QtGlobal>

#include <algorithm>

namespace
{
    QJsonObject buildGroupJsonObject(const CallbackRuleGroupModel& groupModel)
    {
        QJsonObject groupObject;
        groupObject.insert(QStringLiteral("groupId"), static_cast<qint64>(groupModel.groupId));
        groupObject.insert(QStringLiteral("name"), groupModel.groupName);
        groupObject.insert(QStringLiteral("enabled"), groupModel.enabled);
        groupObject.insert(QStringLiteral("priority"), groupModel.priority);
        groupObject.insert(QStringLiteral("comment"), groupModel.comment);
        return groupObject;
    }

    QJsonObject buildRuleJsonObject(const CallbackRuleModel& ruleModel)
    {
        QJsonObject ruleObject;
        ruleObject.insert(QStringLiteral("ruleId"), static_cast<qint64>(ruleModel.ruleId));
        ruleObject.insert(QStringLiteral("groupId"), static_cast<qint64>(ruleModel.groupId));
        ruleObject.insert(QStringLiteral("name"), ruleModel.ruleName);
        ruleObject.insert(QStringLiteral("enabled"), ruleModel.enabled);
        ruleObject.insert(QStringLiteral("callbackType"), static_cast<qint64>(ruleModel.callbackType));
        ruleObject.insert(QStringLiteral("operationMask"), static_cast<qint64>(ruleModel.operationMask));
        ruleObject.insert(QStringLiteral("initiatorPattern"), ruleModel.initiatorPattern);
        ruleObject.insert(QStringLiteral("targetPattern"), ruleModel.targetPattern);
        ruleObject.insert(QStringLiteral("matchMode"), static_cast<qint64>(ruleModel.matchMode));
        ruleObject.insert(QStringLiteral("action"), static_cast<qint64>(ruleModel.action));
        ruleObject.insert(QStringLiteral("timeoutMs"), static_cast<qint64>(ruleModel.timeoutMs));
        ruleObject.insert(QStringLiteral("timeoutDefaultDecision"), static_cast<qint64>(ruleModel.timeoutDefaultDecision));
        ruleObject.insert(QStringLiteral("priority"), ruleModel.priority);
        ruleObject.insert(QStringLiteral("comment"), ruleModel.comment);
        return ruleObject;
    }

    QJsonObject buildRootObjectWithoutCrc(const CallbackConfigDocument& configDocument)
    {
        QJsonObject rootObject;
        rootObject.insert(QStringLiteral("schemaVersion"), static_cast<qint64>(configDocument.schemaVersion));
        rootObject.insert(QStringLiteral("exportedAtUtc"), configDocument.exportedAtUtc);
        rootObject.insert(QStringLiteral("appVersion"), configDocument.appVersion);

        QJsonObject globalSettingsObject;
        globalSettingsObject.insert(QStringLiteral("globalEnabled"), configDocument.globalEnabled);
        globalSettingsObject.insert(QStringLiteral("ruleVersion"), static_cast<qint64>(configDocument.ruleVersion));
        rootObject.insert(QStringLiteral("globalSettings"), globalSettingsObject);

        QJsonArray groupArray;
        for (const CallbackRuleGroupModel& groupModel : configDocument.groups)
        {
            groupArray.push_back(buildGroupJsonObject(groupModel));
        }
        rootObject.insert(QStringLiteral("groups"), groupArray);

        QJsonArray ruleArray;
        for (const CallbackRuleModel& ruleModel : configDocument.rules)
        {
            ruleArray.push_back(buildRuleJsonObject(ruleModel));
        }
        rootObject.insert(QStringLiteral("rules"), ruleArray);
        return rootObject;
    }

    bool parseGroupObject(
        const QJsonObject& groupObject,
        CallbackRuleGroupModel* groupOut,
        QStringList* warningListOut)
    {
        if (groupOut == nullptr)
        {
            return false;
        }

        static const QSet<QString> supportedKeys{
            QStringLiteral("groupId"),
            QStringLiteral("name"),
            QStringLiteral("enabled"),
            QStringLiteral("priority"),
            QStringLiteral("comment")
        };
        for (auto iterator = groupObject.begin(); iterator != groupObject.end(); ++iterator)
        {
            if (!supportedKeys.contains(iterator.key()) && warningListOut != nullptr)
            {
                warningListOut->push_back(QStringLiteral("忽略未知 group 字段：%1").arg(iterator.key()));
            }
        }

        groupOut->groupId = static_cast<quint32>(groupObject.value(QStringLiteral("groupId")).toInteger(0));
        groupOut->groupName = groupObject.value(QStringLiteral("name")).toString();
        groupOut->enabled = groupObject.value(QStringLiteral("enabled")).toBool(true);
        groupOut->priority = static_cast<qint32>(groupObject.value(QStringLiteral("priority")).toInt(0));
        groupOut->comment = groupObject.value(QStringLiteral("comment")).toString();
        return true;
    }

    bool parseRuleObject(
        const QJsonObject& ruleObject,
        CallbackRuleModel* ruleOut,
        QStringList* warningListOut)
    {
        if (ruleOut == nullptr)
        {
            return false;
        }

        static const QSet<QString> supportedKeys{
            QStringLiteral("ruleId"),
            QStringLiteral("groupId"),
            QStringLiteral("name"),
            QStringLiteral("enabled"),
            QStringLiteral("callbackType"),
            QStringLiteral("operationMask"),
            QStringLiteral("initiatorPattern"),
            QStringLiteral("targetPattern"),
            QStringLiteral("matchMode"),
            QStringLiteral("action"),
            QStringLiteral("timeoutMs"),
            QStringLiteral("timeoutDefaultDecision"),
            QStringLiteral("priority"),
            QStringLiteral("comment")
        };
        for (auto iterator = ruleObject.begin(); iterator != ruleObject.end(); ++iterator)
        {
            if (!supportedKeys.contains(iterator.key()) && warningListOut != nullptr)
            {
                warningListOut->push_back(QStringLiteral("忽略未知 rule 字段：%1").arg(iterator.key()));
            }
        }

        ruleOut->ruleId = static_cast<quint32>(ruleObject.value(QStringLiteral("ruleId")).toInteger(0));
        ruleOut->groupId = static_cast<quint32>(ruleObject.value(QStringLiteral("groupId")).toInteger(0));
        ruleOut->ruleName = ruleObject.value(QStringLiteral("name")).toString();
        ruleOut->enabled = ruleObject.value(QStringLiteral("enabled")).toBool(true);
        ruleOut->callbackType = static_cast<quint32>(ruleObject.value(QStringLiteral("callbackType")).toInteger(KSWORD_ARK_CALLBACK_TYPE_REGISTRY));
        ruleOut->operationMask = static_cast<quint32>(ruleObject.value(QStringLiteral("operationMask")).toInteger(0));
        ruleOut->initiatorPattern = ruleObject.value(QStringLiteral("initiatorPattern")).toString();
        ruleOut->targetPattern = ruleObject.value(QStringLiteral("targetPattern")).toString();
        ruleOut->matchMode = static_cast<quint32>(ruleObject.value(QStringLiteral("matchMode")).toInteger(KSWORD_ARK_MATCH_MODE_EXACT));
        ruleOut->action = static_cast<quint32>(ruleObject.value(QStringLiteral("action")).toInteger(KSWORD_ARK_RULE_ACTION_ALLOW));
        ruleOut->timeoutMs = static_cast<quint32>(ruleObject.value(QStringLiteral("timeoutMs")).toInteger(5000));
        ruleOut->timeoutDefaultDecision = static_cast<quint32>(ruleObject.value(QStringLiteral("timeoutDefaultDecision")).toInteger(KSWORD_ARK_DECISION_ALLOW));
        ruleOut->priority = static_cast<qint32>(ruleObject.value(QStringLiteral("priority")).toInt(0));
        ruleOut->comment = ruleObject.value(QStringLiteral("comment")).toString();
        return true;
    }

    bool appendUtf16StringToPool(
        const QString& sourceText,
        QByteArray* stringPoolOut,
        quint32* offsetBytesOut,
        quint16* lengthCharsOut)
    {
        if (stringPoolOut == nullptr || offsetBytesOut == nullptr || lengthCharsOut == nullptr)
        {
            return false;
        }

        QString textValue = sourceText;
        if (textValue.size() > static_cast<int>(std::numeric_limits<quint16>::max() - 1))
        {
            textValue = textValue.left(static_cast<int>(std::numeric_limits<quint16>::max() - 1));
        }

        *offsetBytesOut = static_cast<quint32>(stringPoolOut->size());
        *lengthCharsOut = static_cast<quint16>(textValue.size());
        if (!textValue.isEmpty())
        {
            stringPoolOut->append(
                reinterpret_cast<const char*>(textValue.utf16()),
                textValue.size() * static_cast<int>(sizeof(char16_t)));
        }
        stringPoolOut->append('\0');
        stringPoolOut->append('\0');
        return true;
    }
}

QString callbackTypeToDisplayText(const quint32 callbackType)
{
    switch (callbackType)
    {
    case KSWORD_ARK_CALLBACK_TYPE_REGISTRY: return QStringLiteral("注册表");
    case KSWORD_ARK_CALLBACK_TYPE_PROCESS_CREATE: return QStringLiteral("进程创建");
    case KSWORD_ARK_CALLBACK_TYPE_THREAD_CREATE: return QStringLiteral("线程创建");
    case KSWORD_ARK_CALLBACK_TYPE_IMAGE_LOAD: return QStringLiteral("镜像加载");
    case KSWORD_ARK_CALLBACK_TYPE_OBJECT: return QStringLiteral("对象管理器");
    case KSWORD_ARK_CALLBACK_TYPE_MINIFILTER_RESERVED: return QStringLiteral("文件系统微过滤器（预留）");
    default: return QStringLiteral("未知");
    }
}

QString callbackActionToDisplayText(const quint32 actionType)
{
    switch (actionType)
    {
    case KSWORD_ARK_RULE_ACTION_ALLOW: return QStringLiteral("允许");
    case KSWORD_ARK_RULE_ACTION_DENY: return QStringLiteral("拒绝");
    case KSWORD_ARK_RULE_ACTION_ASK_USER: return QStringLiteral("询问用户");
    case KSWORD_ARK_RULE_ACTION_LOG_ONLY: return QStringLiteral("记录日志");
    case KSWORD_ARK_RULE_ACTION_STRIP_ACCESS: return QStringLiteral("降权拦截");
    default: return QStringLiteral("未知");
    }
}

QString callbackMatchModeToDisplayText(const quint32 matchMode)
{
    switch (matchMode)
    {
    case KSWORD_ARK_MATCH_MODE_EXACT: return QStringLiteral("精确匹配");
    case KSWORD_ARK_MATCH_MODE_PREFIX: return QStringLiteral("前缀匹配");
    case KSWORD_ARK_MATCH_MODE_WILDCARD: return QStringLiteral("通配符匹配");
    case KSWORD_ARK_MATCH_MODE_REGEX: return QStringLiteral("正则匹配");
    default: return QStringLiteral("未知");
    }
}

QString callbackDecisionToDisplayText(const quint32 decision)
{
    switch (decision)
    {
    case KSWORD_ARK_DECISION_ALLOW: return QStringLiteral("允许");
    case KSWORD_ARK_DECISION_DENY: return QStringLiteral("拒绝");
    default: return QStringLiteral("未知");
    }
}

QString callbackGuidToString(const KSWORD_ARK_GUID128& guidValue)
{
    const unsigned char* data = guidValue.bytes;
    return QStringLiteral("%1%2%3%4-%5%6-%7%8-%9%10-%11%12%13%14%15%16")
        .arg(data[0], 2, 16, QChar('0'))
        .arg(data[1], 2, 16, QChar('0'))
        .arg(data[2], 2, 16, QChar('0'))
        .arg(data[3], 2, 16, QChar('0'))
        .arg(data[4], 2, 16, QChar('0'))
        .arg(data[5], 2, 16, QChar('0'))
        .arg(data[6], 2, 16, QChar('0'))
        .arg(data[7], 2, 16, QChar('0'))
        .arg(data[8], 2, 16, QChar('0'))
        .arg(data[9], 2, 16, QChar('0'))
        .arg(data[10], 2, 16, QChar('0'))
        .arg(data[11], 2, 16, QChar('0'))
        .arg(data[12], 2, 16, QChar('0'))
        .arg(data[13], 2, 16, QChar('0'))
        .arg(data[14], 2, 16, QChar('0'))
        .arg(data[15], 2, 16, QChar('0'))
        .toUpper();
}

quint32 computeCallbackCrc32(const QByteArray& rawBytes)
{
    quint32 crcValue = 0xFFFFFFFFu;
    for (const unsigned char valueByte : rawBytes)
    {
        crcValue ^= static_cast<quint32>(valueByte);
        for (int bitIndex = 0; bitIndex < 8; ++bitIndex)
        {
            if ((crcValue & 1u) != 0u)
            {
                crcValue = (crcValue >> 1u) ^ 0xEDB88320u;
            }
            else
            {
                crcValue >>= 1u;
            }
        }
    }
    return ~crcValue;
}

bool exportCallbackConfigToJson(
    const CallbackConfigDocument& configDocument,
    QByteArray* jsonOut,
    QString* errorTextOut)
{
    if (jsonOut == nullptr)
    {
        if (errorTextOut != nullptr)
        {
            *errorTextOut = QStringLiteral("导出失败：输出缓冲区为空。");
        }
        return false;
    }

    CallbackConfigDocument exportDocument = configDocument;
    if (exportDocument.exportedAtUtc.trimmed().isEmpty())
    {
        exportDocument.exportedAtUtc = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    }

    QJsonObject rootWithoutCrc = buildRootObjectWithoutCrc(exportDocument);
    QByteArray canonicalBytes = QJsonDocument(rootWithoutCrc).toJson(QJsonDocument::Indented);
    exportDocument.crc32 = computeCallbackCrc32(canonicalBytes);

    QJsonObject rootObject = rootWithoutCrc;
    rootObject.insert(QStringLiteral("crc32"), static_cast<qint64>(exportDocument.crc32));
    *jsonOut = QJsonDocument(rootObject).toJson(QJsonDocument::Indented);
    return true;
}

bool importCallbackConfigFromJson(
    const QByteArray& jsonBytes,
    CallbackConfigDocument* configOut,
    QStringList* warningListOut,
    QString* errorTextOut)
{
    if (configOut == nullptr)
    {
        if (errorTextOut != nullptr)
        {
            *errorTextOut = QStringLiteral("导入失败：输出配置对象为空。");
        }
        return false;
    }

    QJsonParseError parseError;
    const QJsonDocument jsonDocument = QJsonDocument::fromJson(jsonBytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !jsonDocument.isObject())
    {
        if (errorTextOut != nullptr)
        {
            *errorTextOut = QStringLiteral("导入失败：JSON 解析错误（%1）。").arg(parseError.errorString());
        }
        return false;
    }

    const QJsonObject rootObject = jsonDocument.object();
    static const QSet<QString> rootKeys{
        QStringLiteral("schemaVersion"),
        QStringLiteral("exportedAtUtc"),
        QStringLiteral("appVersion"),
        QStringLiteral("globalSettings"),
        QStringLiteral("groups"),
        QStringLiteral("rules"),
        QStringLiteral("crc32")
    };
    for (auto iterator = rootObject.begin(); iterator != rootObject.end(); ++iterator)
    {
        if (!rootKeys.contains(iterator.key()) && warningListOut != nullptr)
        {
            warningListOut->push_back(QStringLiteral("忽略未知顶层字段：%1").arg(iterator.key()));
        }
    }

    CallbackConfigDocument importedDocument;
    importedDocument.schemaVersion = static_cast<quint32>(
        rootObject.value(QStringLiteral("schemaVersion"))
        .toInteger(KSWORD_ARK_CALLBACK_RULE_SCHEMA_VERSION));
    if (importedDocument.schemaVersion != KSWORD_ARK_CALLBACK_RULE_SCHEMA_VERSION)
    {
        if (errorTextOut != nullptr)
        {
            *errorTextOut = QStringLiteral("导入失败：schemaVersion=%1 与当前版本 %2 不兼容。")
                .arg(importedDocument.schemaVersion)
                .arg(KSWORD_ARK_CALLBACK_RULE_SCHEMA_VERSION);
        }
        return false;
    }

    importedDocument.exportedAtUtc = rootObject.value(QStringLiteral("exportedAtUtc")).toString();
    importedDocument.appVersion = rootObject.value(QStringLiteral("appVersion")).toString();

    if (!rootObject.value(QStringLiteral("globalSettings")).isObject())
    {
        if (errorTextOut != nullptr)
        {
            *errorTextOut = QStringLiteral("导入失败：globalSettings 字段缺失或格式错误。");
        }
        return false;
    }
    const QJsonObject globalSettingsObject = rootObject.value(QStringLiteral("globalSettings")).toObject();
    importedDocument.globalEnabled = globalSettingsObject.value(QStringLiteral("globalEnabled")).toBool(true);
    importedDocument.ruleVersion = static_cast<quint64>(globalSettingsObject.value(QStringLiteral("ruleVersion")).toInteger(1));

    if (!rootObject.value(QStringLiteral("groups")).isArray() ||
        !rootObject.value(QStringLiteral("rules")).isArray())
    {
        if (errorTextOut != nullptr)
        {
            *errorTextOut = QStringLiteral("导入失败：groups 或 rules 字段缺失或格式错误。");
        }
        return false;
    }

    for (const QJsonValue& groupValue : rootObject.value(QStringLiteral("groups")).toArray())
    {
        if (!groupValue.isObject())
        {
            continue;
        }
        CallbackRuleGroupModel groupModel;
        if (parseGroupObject(groupValue.toObject(), &groupModel, warningListOut))
        {
            importedDocument.groups.push_back(groupModel);
        }
    }

    for (const QJsonValue& ruleValue : rootObject.value(QStringLiteral("rules")).toArray())
    {
        if (!ruleValue.isObject())
        {
            continue;
        }
        CallbackRuleModel ruleModel;
        if (parseRuleObject(ruleValue.toObject(), &ruleModel, warningListOut))
        {
            importedDocument.rules.push_back(ruleModel);
        }
    }

    importedDocument.crc32 = static_cast<quint32>(rootObject.value(QStringLiteral("crc32")).toInteger(0));
    if (importedDocument.crc32 != 0U)
    {
        QJsonObject rootWithoutCrc = rootObject;
        rootWithoutCrc.remove(QStringLiteral("crc32"));
        const quint32 calculatedCrc = computeCallbackCrc32(QJsonDocument(rootWithoutCrc).toJson(QJsonDocument::Indented));
        if (calculatedCrc != importedDocument.crc32)
        {
            if (errorTextOut != nullptr)
            {
                *errorTextOut = QStringLiteral("导入失败：CRC32 校验不通过（expected=%1, actual=%2）。")
                    .arg(importedDocument.crc32)
                    .arg(calculatedCrc);
            }
            return false;
        }
    }

    *configOut = importedDocument;
    return true;
}

bool buildCallbackRuleBlobFromConfig(
    const CallbackConfigDocument& configDocument,
    QByteArray* blobOut,
    QString* errorTextOut)
{
    if (blobOut == nullptr)
    {
        if (errorTextOut != nullptr)
        {
            *errorTextOut = QStringLiteral("编译失败：输出缓冲区为空。");
        }
        return false;
    }

    const CallbackValidationResult validationResult = validateCallbackConfig(configDocument);
    if (!validationResult.success)
    {
        if (errorTextOut != nullptr)
        {
            *errorTextOut = QStringLiteral("编译失败：%1").arg(validationResult.errorList.join(QStringLiteral("；")));
        }
        return false;
    }

    QList<CallbackRuleGroupModel> sortedGroups = configDocument.groups;
    std::sort(sortedGroups.begin(), sortedGroups.end(), [](const CallbackRuleGroupModel& leftGroup, const CallbackRuleGroupModel& rightGroup) {
        if (leftGroup.priority != rightGroup.priority)
        {
            return leftGroup.priority < rightGroup.priority;
        }
        return leftGroup.groupId < rightGroup.groupId;
        });

    QHash<quint32, qint32> groupPriorityById;
    for (const CallbackRuleGroupModel& groupModel : sortedGroups)
    {
        groupPriorityById.insert(groupModel.groupId, groupModel.priority);
    }

    QList<CallbackRuleModel> sortedRules = configDocument.rules;
    std::sort(sortedRules.begin(), sortedRules.end(), [&groupPriorityById](const CallbackRuleModel& leftRule, const CallbackRuleModel& rightRule) {
        const qint32 leftGroupPriority = groupPriorityById.value(leftRule.groupId, 0);
        const qint32 rightGroupPriority = groupPriorityById.value(rightRule.groupId, 0);
        if (leftGroupPriority != rightGroupPriority)
        {
            return leftGroupPriority < rightGroupPriority;
        }
        if (leftRule.priority != rightRule.priority)
        {
            return leftRule.priority < rightRule.priority;
        }
        return leftRule.ruleId < rightRule.ruleId;
        });

    QByteArray stringPool;
    QList<KSWORD_ARK_CALLBACK_GROUP_BLOB> groupBlobList;
    QList<KSWORD_ARK_CALLBACK_RULE_BLOB> ruleBlobList;
    groupBlobList.reserve(sortedGroups.size());
    ruleBlobList.reserve(sortedRules.size());

    for (const CallbackRuleGroupModel& groupModel : sortedGroups)
    {
        KSWORD_ARK_CALLBACK_GROUP_BLOB groupBlob{};
        quint32 nameOffset = 0;
        quint16 nameLength = 0;
        quint32 commentOffset = 0;
        quint16 commentLength = 0;
        if (!appendUtf16StringToPool(groupModel.groupName, &stringPool, &nameOffset, &nameLength) ||
            !appendUtf16StringToPool(groupModel.comment, &stringPool, &commentOffset, &commentLength))
        {
            if (errorTextOut != nullptr)
            {
                *errorTextOut = QStringLiteral("编译失败：写入 group 字符串池失败。");
            }
            return false;
        }

        groupBlob.groupId = groupModel.groupId;
        groupBlob.flags = groupModel.enabled ? KSWORD_ARK_CALLBACK_GROUP_FLAG_ENABLED : 0u;
        groupBlob.priority = static_cast<unsigned long>(groupModel.priority);
        groupBlob.nameOffsetBytes = nameOffset;
        groupBlob.nameLengthChars = nameLength;
        groupBlob.commentOffsetBytes = commentOffset;
        groupBlob.commentLengthChars = commentLength;
        groupBlobList.push_back(groupBlob);
    }

    for (const CallbackRuleModel& ruleModel : sortedRules)
    {
        KSWORD_ARK_CALLBACK_RULE_BLOB ruleBlob{};
        quint32 initiatorOffset = 0;
        quint16 initiatorLength = 0;
        quint32 targetOffset = 0;
        quint16 targetLength = 0;
        quint32 ruleNameOffset = 0;
        quint16 ruleNameLength = 0;
        quint32 commentOffset = 0;
        quint16 commentLength = 0;

        if (!appendUtf16StringToPool(ruleModel.initiatorPattern, &stringPool, &initiatorOffset, &initiatorLength) ||
            !appendUtf16StringToPool(ruleModel.targetPattern, &stringPool, &targetOffset, &targetLength) ||
            !appendUtf16StringToPool(ruleModel.ruleName, &stringPool, &ruleNameOffset, &ruleNameLength) ||
            !appendUtf16StringToPool(ruleModel.comment, &stringPool, &commentOffset, &commentLength))
        {
            if (errorTextOut != nullptr)
            {
                *errorTextOut = QStringLiteral("编译失败：写入 rule 字符串池失败。");
            }
            return false;
        }

        ruleBlob.ruleId = ruleModel.ruleId;
        ruleBlob.groupId = ruleModel.groupId;
        ruleBlob.flags = ruleModel.enabled ? KSWORD_ARK_CALLBACK_RULE_FLAG_ENABLED : 0u;
        ruleBlob.callbackType = ruleModel.callbackType;
        ruleBlob.operationMask = ruleModel.operationMask;
        ruleBlob.action = ruleModel.action;
        ruleBlob.matchMode = ruleModel.matchMode;
        ruleBlob.priority = static_cast<unsigned long>(ruleModel.priority);
        ruleBlob.initiatorOffsetBytes = initiatorOffset;
        ruleBlob.initiatorLengthChars = initiatorLength;
        ruleBlob.targetOffsetBytes = targetOffset;
        ruleBlob.targetLengthChars = targetLength;
        ruleBlob.askTimeoutMs = ruleModel.timeoutMs;
        ruleBlob.askDefaultDecision = ruleModel.timeoutDefaultDecision;
        ruleBlob.ruleNameOffsetBytes = ruleNameOffset;
        ruleBlob.ruleNameLengthChars = ruleNameLength;
        ruleBlob.commentOffsetBytes = commentOffset;
        ruleBlob.commentLengthChars = commentLength;
        ruleBlobList.push_back(ruleBlob);
    }

    const quint32 headerSize = static_cast<quint32>(sizeof(KSWORD_ARK_CALLBACK_RULE_BLOB_HEADER));
    const quint32 groupBytes = static_cast<quint32>(groupBlobList.size() * sizeof(KSWORD_ARK_CALLBACK_GROUP_BLOB));
    const quint32 ruleBytes = static_cast<quint32>(ruleBlobList.size() * sizeof(KSWORD_ARK_CALLBACK_RULE_BLOB));
    const quint32 stringBytes = static_cast<quint32>(stringPool.size());

    KSWORD_ARK_CALLBACK_RULE_BLOB_HEADER header{};
    header.size = headerSize + groupBytes + ruleBytes + stringBytes;
    header.magic = KSWORD_ARK_CALLBACK_RULE_BLOB_MAGIC;
    header.protocolVersion = KSWORD_ARK_CALLBACK_PROTOCOL_VERSION;
    header.schemaVersion = KSWORD_ARK_CALLBACK_RULE_SCHEMA_VERSION;
    header.globalFlags = configDocument.globalEnabled ? KSWORD_ARK_CALLBACK_GLOBAL_FLAG_ENABLED : 0u;
    header.groupCount = static_cast<unsigned long>(groupBlobList.size());
    header.ruleCount = static_cast<unsigned long>(ruleBlobList.size());
    header.groupOffsetBytes = headerSize;
    header.ruleOffsetBytes = header.groupOffsetBytes + groupBytes;
    header.stringOffsetBytes = header.ruleOffsetBytes + ruleBytes;
    header.stringBytes = stringBytes;
    header.crc32 = 0;
    header.ruleVersion = configDocument.ruleVersion;

    QByteArray blobBytes;
    blobBytes.reserve(static_cast<int>(header.size));
    blobBytes.append(reinterpret_cast<const char*>(&header), sizeof(header));
    if (!groupBlobList.isEmpty())
    {
        blobBytes.append(reinterpret_cast<const char*>(groupBlobList.constData()), groupBytes);
    }
    if (!ruleBlobList.isEmpty())
    {
        blobBytes.append(reinterpret_cast<const char*>(ruleBlobList.constData()), ruleBytes);
    }
    blobBytes.append(stringPool);

    if (blobBytes.size() != static_cast<int>(header.size))
    {
        if (errorTextOut != nullptr)
        {
            *errorTextOut = QStringLiteral("编译失败：blob 大小不一致。");
        }
        return false;
    }

    const quint32 crc32Value = computeCallbackCrc32(blobBytes);
    auto* writableHeader = reinterpret_cast<KSWORD_ARK_CALLBACK_RULE_BLOB_HEADER*>(blobBytes.data());
    writableHeader->crc32 = crc32Value;

    *blobOut = blobBytes;
    return true;
}
