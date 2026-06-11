#include "StartupDock.Internal.h"

#include <QFile>
#include <QTextStream>

namespace startup_dock_detail
{
    namespace
    {
        // loadRegistryLocationCatalog 作用：
        // - 读取 Qt 资源中的 Autoruns 风格注册表位置原始清单；
        // - 非 UI 的标准化、修正、去重逻辑已经迁入 ks::startup；
        // - 返回值：按资源顺序保留的 QStringList，供注册表树创建分组节点。
        QStringList loadRegistryLocationCatalog()
        {
            std::vector<std::string> rawLineList;
            QFile catalogFile(QStringLiteral(":/Data/startup_registry_locations.txt"));
            if (!catalogFile.open(QIODevice::ReadOnly | QIODevice::Text))
            {
                return QStringList();
            }

            QTextStream catalogStream(&catalogFile);
            catalogStream.setEncoding(QStringConverter::Utf8);
            while (!catalogStream.atEnd())
            {
                const QString rawLineText = catalogStream.readLine();
                const QByteArray rawLineBytes = rawLineText.toUtf8();
                rawLineList.emplace_back(rawLineBytes.constData(), static_cast<std::size_t>(rawLineBytes.size()));
            }

            QStringList locationList;
            const std::vector<std::string> normalizedList =
                ks::startup::BuildKnownStartupRegistryLocationList(rawLineList);
            for (const std::string& locationText : normalizedList)
            {
                locationList.push_back(QString::fromUtf8(locationText.c_str(), static_cast<int>(locationText.size())));
            }
            return locationList;
        }
    }

    QStringList buildKnownStartupRegistryLocationList()
    {
        // 静态缓存仍保留在 UI 层，避免注册表树每次刷新都重新读取 Qt 资源。
        static const QStringList cachedLocationList = loadRegistryLocationCatalog();
        return cachedLocationList;
    }
}
