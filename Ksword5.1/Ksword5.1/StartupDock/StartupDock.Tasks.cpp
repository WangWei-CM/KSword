#include "StartupDock.Internal.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

using namespace startup_dock_detail;

void StartupDock::appendTaskEntries(std::vector<StartupEntry>* entryListOut)
{
    if (entryListOut == nullptr)
    {
        return;
    }

    QProcess processObject;
    const QString scriptText = QStringLiteral(
        "$ErrorActionPreference='SilentlyContinue'; "
        "$taskList = Get-ScheduledTask | ForEach-Object { "
        "  $actions = ($_.Actions | ForEach-Object { ($_.Execute + ' ' + $_.Arguments).Trim() }) -join ' | '; "
        "  $triggers = ($_.Triggers | ForEach-Object { $_.CimClass.CimClassName }) -join ' | '; "
        "  [PSCustomObject]@{ "
        "    TaskPath = $_.TaskPath; "
        "    TaskName = $_.TaskName; "
        "    State = [string]$_.State; "
        "    Author = $_.Author; "
        "    Description = $_.Description; "
        "    Actions = $actions; "
        "    Triggers = $triggers; "
        "    UserId = $_.Principal.UserId "
        "  } "
        "}; "
        "$taskList | ConvertTo-Json -Depth 5 -Compress");

    processObject.setProgram(QStringLiteral("powershell.exe"));
    processObject.setArguments({
        QStringLiteral("-NoProfile"),
        QStringLiteral("-ExecutionPolicy"),
        QStringLiteral("Bypass"),
        QStringLiteral("-Command"),
        scriptText
        });
    processObject.start();
    if (!processObject.waitForStarted(1500))
    {
        return;
    }
    if (!processObject.waitForFinished(20000))
    {
        processObject.kill();
        processObject.waitForFinished(1500);
        return;
    }

    const QString jsonText = QString::fromLocal8Bit(processObject.readAllStandardOutput()).trimmed();
    if (jsonText.isEmpty())
    {
        return;
    }

    QJsonParseError parseError{};
    const QJsonDocument jsonDocument = QJsonDocument::fromJson(jsonText.toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError || jsonDocument.isNull())
    {
        return;
    }

    QJsonArray taskArray;
    if (jsonDocument.isArray())
    {
        taskArray = jsonDocument.array();
    }
    else if (jsonDocument.isObject())
    {
        taskArray.push_back(jsonDocument.object());
    }

    for (const QJsonValue& taskValue : taskArray)
    {
        if (!taskValue.isObject())
        {
            continue;
        }

        const QJsonObject taskObject = taskValue.toObject();
        const QString actionText = taskObject.value(QStringLiteral("Actions")).toString().trimmed();
        const QString taskPathText = taskObject.value(QStringLiteral("TaskPath")).toString();
        const QString taskNameText = taskObject.value(QStringLiteral("TaskName")).toString();
        if (taskNameText.trimmed().isEmpty())
        {
            continue;
        }

        StartupEntry entry;
        entry.category = StartupCategory::Tasks;
        entry.categoryText = categoryToText(entry.category);
        entry.itemNameText = taskNameText;
        entry.commandText = actionText;
        entry.imagePathText = normalizeFilePathText(actionText);
        entry.publisherText = queryPublisherTextByPath(entry.imagePathText);
        entry.locationText = taskPathText + taskNameText;
        entry.userText = taskObject.value(QStringLiteral("UserId")).toString();
        entry.enabled = !taskObject.value(QStringLiteral("State")).toString().contains(
            QStringLiteral("Disabled"),
            Qt::CaseInsensitive);
        entry.sourceTypeText = QStringLiteral("ScheduledTask");
        entry.detailText = QStringLiteral("状态=%1; 触发器=%2; 描述=%3")
            .arg(taskObject.value(QStringLiteral("State")).toString())
            .arg(taskObject.value(QStringLiteral("Triggers")).toString())
            .arg(taskObject.value(QStringLiteral("Description")).toString());
        entry.canOpenFileLocation = !entry.imagePathText.isEmpty();
        entry.canDelete = true;
        entry.uniqueIdText = QStringLiteral("TASK|%1").arg(entry.locationText);
        entryListOut->push_back(entry);
    }
}
