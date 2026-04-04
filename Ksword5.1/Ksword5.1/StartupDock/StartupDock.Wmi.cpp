#include "StartupDock.Internal.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>

using namespace startup_dock_detail;

namespace
{
    // buildWmiPersistenceScript：
    // - 作用：构造 PowerShell 脚本，统一枚举 root\subscription 下常见永久事件项；
    // - 调用：appendWmiEntries 中通过 powershell.exe 执行；
    // - 传入：无；
    // - 传出：返回 JSON 输出脚本文本。
    QString buildWmiPersistenceScript()
    {
        return QString::fromUtf8(R"PS(
$ErrorActionPreference = 'SilentlyContinue'
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$items = @()
Get-CimInstance -Namespace root/subscription -ClassName CommandLineEventConsumer | ForEach-Object {
    $items += [PSCustomObject]@{
        Type = 'WMI-CommandLineConsumer'
        Name = $_.Name
        Command = $_.CommandLineTemplate
        Image = $_.ExecutablePath
        Location = 'root\subscription\CommandLineEventConsumer'
        Detail = ('ExecutablePath=' + $_.ExecutablePath + '; WorkingDirectory=' + $_.WorkingDirectory)
    }
}
Get-CimInstance -Namespace root/subscription -ClassName ActiveScriptEventConsumer | ForEach-Object {
    $items += [PSCustomObject]@{
        Type = 'WMI-ActiveScriptConsumer'
        Name = $_.Name
        Command = $_.ScriptText
        Image = ''
        Location = 'root\subscription\ActiveScriptEventConsumer'
        Detail = ('ScriptingEngine=' + $_.ScriptingEngine)
    }
}
Get-CimInstance -Namespace root/subscription -ClassName LogFileEventConsumer | ForEach-Object {
    $items += [PSCustomObject]@{
        Type = 'WMI-LogFileConsumer'
        Name = $_.Name
        Command = $_.Filename
        Image = ''
        Location = 'root\subscription\LogFileEventConsumer'
        Detail = ('Text=' + $_.Text)
    }
}
Get-CimInstance -Namespace root/subscription -ClassName NTEventLogEventConsumer | ForEach-Object {
    $items += [PSCustomObject]@{
        Type = 'WMI-NTEventLogConsumer'
        Name = $_.Name
        Command = $_.SourceName
        Image = ''
        Location = 'root\subscription\NTEventLogEventConsumer'
        Detail = ('EventId=' + $_.EventID + '; Category=' + $_.Category)
    }
}
Get-CimInstance -Namespace root/subscription -ClassName __EventFilter | ForEach-Object {
    $items += [PSCustomObject]@{
        Type = 'WMI-EventFilter'
        Name = $_.Name
        Command = $_.Query
        Image = ''
        Location = 'root\subscription\__EventFilter'
        Detail = ('QueryLanguage=' + $_.QueryLanguage + '; EventNamespace=' + $_.EventNamespace)
    }
}
Get-CimInstance -Namespace root/subscription -ClassName __FilterToConsumerBinding | ForEach-Object {
    $items += [PSCustomObject]@{
        Type = 'WMI-FilterToConsumerBinding'
        Name = $_.Consumer
        Command = $_.Filter
        Image = ''
        Location = 'root\subscription\__FilterToConsumerBinding'
        Detail = ('Consumer=' + $_.Consumer + '; Filter=' + $_.Filter + '; DeliveryQoS=' + $_.DeliveryQoS)
    }
}
if ($items.Count -eq 0) {
    '[]'
} else {
    $items | ConvertTo-Json -Compress -Depth 4
}
)PS");
    }

    // jsonValueToText：
    // - 作用：把 QJsonValue 转成稳定的显示文本；
    // - 调用：解析 WMI JSON 字段时复用；
    // - 传入 jsonValue：JSON 值；
    // - 传出：返回字符串文本。
    QString jsonValueToText(const QJsonValue& jsonValue)
    {
        if (jsonValue.isString())
        {
            return jsonValue.toString().trimmed();
        }
        if (jsonValue.isDouble())
        {
            return QString::number(jsonValue.toDouble());
        }
        if (jsonValue.isBool())
        {
            return jsonValue.toBool() ? QStringLiteral("true") : QStringLiteral("false");
        }
        if (jsonValue.isNull() || jsonValue.isUndefined())
        {
            return QString();
        }
        return QString::fromUtf8(QJsonDocument(jsonValue.toObject()).toJson(QJsonDocument::Compact));
    }

    // appendWmiJsonObject：
    // - 作用：把单个 JSON 对象转换成 StartupEntry；
    // - 调用：appendWmiEntries 解析 PowerShell 输出时逐项复用；
    // - 传入 jsonObject：单个 WMI 持久化对象；
    // - 传出：写入 entryListOut。
    void appendWmiJsonObject(
        std::vector<StartupDock::StartupEntry>* entryListOut,
        const QJsonObject& jsonObject)
    {
        if (entryListOut == nullptr)
        {
            return;
        }

        const QString typeText = jsonValueToText(jsonObject.value(QStringLiteral("Type")));
        const QString nameText = jsonValueToText(jsonObject.value(QStringLiteral("Name")));
        const QString commandText = jsonValueToText(jsonObject.value(QStringLiteral("Command")));
        const QString imagePathText = normalizeFilePathText(
            jsonValueToText(jsonObject.value(QStringLiteral("Image"))));
        const QString locationText = jsonValueToText(jsonObject.value(QStringLiteral("Location")));
        const QString detailText = jsonValueToText(jsonObject.value(QStringLiteral("Detail")));

        if (typeText.trimmed().isEmpty() && nameText.trimmed().isEmpty() && commandText.trimmed().isEmpty())
        {
            return;
        }

        StartupDock::StartupEntry entry;
        entry.category = StartupDock::StartupCategory::Wmi;
        entry.categoryText = QStringLiteral("WMI");
        entry.itemNameText = nameText.trimmed().isEmpty() ? QStringLiteral("(未命名WMI项)") : nameText;
        entry.publisherText = queryPublisherTextByPath(imagePathText);
        entry.imagePathText = imagePathText;
        entry.commandText = commandText;
        entry.locationText = locationText;
        entry.locationGroupText.clear();
        entry.userText = QStringLiteral("本机");
        entry.detailText = detailText;
        entry.sourceTypeText = typeText;
        entry.enabled = true;
        entry.canOpenFileLocation = !entry.imagePathText.trimmed().isEmpty();
        entry.canOpenRegistryLocation = false;
        entry.canDelete = false;
        entry.deleteRegistryTree = false;
        entry.uniqueIdText = QStringLiteral("WMI|%1|%2|%3")
            .arg(typeText, entry.itemNameText, locationText);
        entryListOut->push_back(entry);
    }
}

void StartupDock::appendWmiEntries(std::vector<StartupEntry>* entryListOut)
{
    if (entryListOut == nullptr)
    {
        return;
    }

    QProcess processObject;
    processObject.setProgram(QStringLiteral("powershell.exe"));
    processObject.setArguments({
        QStringLiteral("-NoProfile"),
        QStringLiteral("-ExecutionPolicy"),
        QStringLiteral("Bypass"),
        QStringLiteral("-Command"),
        buildWmiPersistenceScript()
        });
    processObject.start();
    if (!processObject.waitForStarted(2000))
    {
        return;
    }
    if (!processObject.waitForFinished(15000))
    {
        processObject.kill();
        processObject.waitForFinished(2000);
        return;
    }
    if (processObject.exitStatus() != QProcess::NormalExit)
    {
        return;
    }

    const QByteArray stdOutBytes = processObject.readAllStandardOutput();
    if (stdOutBytes.trimmed().isEmpty())
    {
        return;
    }

    QJsonParseError parseError;
    const QJsonDocument jsonDocument = QJsonDocument::fromJson(stdOutBytes, &parseError);
    if (parseError.error != QJsonParseError::NoError)
    {
        StartupEntry errorEntry;
        errorEntry.category = StartupCategory::Wmi;
        errorEntry.categoryText = QStringLiteral("WMI");
        errorEntry.itemNameText = QStringLiteral("WMI 枚举解析失败");
        errorEntry.commandText = QString::fromUtf8(stdOutBytes);
        errorEntry.locationText = QStringLiteral("root\\subscription");
        errorEntry.userText = QStringLiteral("本机");
        errorEntry.detailText = QStringLiteral("JSON 解析错误：%1").arg(parseError.errorString());
        errorEntry.sourceTypeText = QStringLiteral("WMI-ParseError");
        errorEntry.enabled = false;
        errorEntry.uniqueIdText = QStringLiteral("WMI|ParseError");
        entryListOut->push_back(errorEntry);
        return;
    }

    if (jsonDocument.isArray())
    {
        const QJsonArray jsonArray = jsonDocument.array();
        for (const QJsonValue& jsonValue : jsonArray)
        {
            if (jsonValue.isObject())
            {
                appendWmiJsonObject(entryListOut, jsonValue.toObject());
            }
        }
    }
    else if (jsonDocument.isObject())
    {
        appendWmiJsonObject(entryListOut, jsonDocument.object());
    }
}
