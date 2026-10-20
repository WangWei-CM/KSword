#include "DumpPoolTag.h"

#include "DumpSymbolIndex.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSet>

#include <algorithm>

namespace ks::minidump
{
    namespace
    {
        // kMaxScanBytes：单个映像最多扫描的字节数。驱动映像通常远小于此，
        // 设上限只是为了挡住异常大的文件把解析拖住。
        constexpr qint64 kMaxScanBytes = 64LL * 1024LL * 1024LL;

        // kMaxScanModules：最多扫描多少个模块。内核转储里驱动动辄一两百个，
        // 全扫是几百 MB 的磁盘读；命中通常出现在前面，且这只是辅助线索。
        constexpr int kMaxScanModules = 200;

        // PoolTagCharOk 作用：判断单个字节能否出现在池标记里。
        // 传入 value 字节；返回是否合法。
        // 约定里标记是可打印 ASCII，实际多为字母数字，末位常用空格补齐。
        bool PoolTagCharOk(const unsigned char value)
        {
            if (value == ' ')
            {
                return true;
            }
            if (value >= '0' && value <= '9')
            {
                return true;
            }
            if (value >= 'A' && value <= 'Z')
            {
                return true;
            }
            if (value >= 'a' && value <= 'z')
            {
                return true;
            }
            // 少数标记会用这几个符号，放行但不放宽到全部可打印字符：
            // 判据一松，随便一个整数都会被当成标记，结论就没价值了。
            return value == '_' || value == '.' || value == '?';
        }

        // LoadKnownPoolTags 作用：读取 WDK 调试器自带的 pooltag.txt。
        // 返回“标记 → 用途说明”的映射；文件不存在时返回空表。
        // 用官方表而不是自己硬编一份：池标记数以千计，凭记忆写下来必然有错，
        // 而一条错误的归属会把排查引向完全无关的组件。
        const QHash<QString, QString>& LoadKnownPoolTags()
        {
            static const QHash<QString, QString> table = []()
            {
                QHash<QString, QString> result; // result：待填充的映射。
                const QStringList candidates = {
                    QStringLiteral("C:\\Program Files (x86)\\Windows Kits\\10\\Debuggers\\x64\\triage\\pooltag.txt"),
                    QStringLiteral("C:\\Program Files\\Windows Kits\\10\\Debuggers\\x64\\triage\\pooltag.txt"),
                    QStringLiteral("C:\\Program Files (x86)\\Windows Kits\\10\\Debuggers\\x86\\triage\\pooltag.txt"),
                };
                for (const QString& path : candidates)
                {
                    CheckDumpCancellation();
                    QFile file(path);
                    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
                    {
                        continue;
                    }
                    while (!file.atEnd())
                    {
                        CheckDumpCancellation();
                        // pooltag.txt 的行格式为： Tag  - Binary - Description
                        const QString line = QString::fromLatin1(file.readLine()).trimmed();
                        if (line.isEmpty() || line.startsWith(QLatin1Char('/')))
                        {
                            continue;
                        }
                        const int firstDash = line.indexOf(QLatin1Char('-'));
                        if (firstDash <= 0)
                        {
                            continue;
                        }
                        const QString tag = line.left(firstDash).trimmed();
                        if (tag.isEmpty() || tag.size() > 4)
                        {
                            continue;
                        }
                        const QString rest = line.mid(firstDash + 1).trimmed();
                        if (!result.contains(tag))
                        {
                            result.insert(tag, rest);
                        }
                    }
                    break;
                }
                return result;
            }();
            return table;
        }

        // NormalizeModulePath 作用：把内核风格模块路径换算成可打开的路径。
        // 传入 raw 原始路径；返回换算结果，换算不了返回空串。
        QString NormalizeModulePath(const QString& raw)
        {
            if (raw.isEmpty())
            {
                return QString();
            }
            QString path = raw; // path：换算中的路径。
            if (path.startsWith(QStringLiteral("\\??\\"), Qt::CaseInsensitive))
            {
                path = path.mid(4);
            }
            else if (path.startsWith(QStringLiteral("\\SystemRoot\\"), Qt::CaseInsensitive))
            {
                const QString systemRoot = QString::fromLocal8Bit(qgetenv("SystemRoot"));
                if (systemRoot.isEmpty())
                {
                    return QString();
                }
                path = systemRoot + QStringLiteral("\\") + path.mid(12);
            }
            if (path.size() >= 2 && path.at(1) == QLatin1Char(':'))
            {
                return QDir::toNativeSeparators(path);
            }
            // 只带文件名时按驱动目录补全：内核转储里这种写法很常见。
            if (!path.contains(QLatin1Char('\\')) && !path.contains(QLatin1Char('/')))
            {
                const QString systemRoot = QString::fromLocal8Bit(qgetenv("SystemRoot"));
                if (systemRoot.isEmpty())
                {
                    return QString();
                }
                const QString driverPath = QDir::toNativeSeparators(
                    systemRoot + QStringLiteral("\\System32\\drivers\\") + path);
                if (QFileInfo::exists(driverPath))
                {
                    return driverPath;
                }
                return QDir::toNativeSeparators(
                    systemRoot + QStringLiteral("\\System32\\") + path);
            }
            return QString();
        }

        // FindTagOwners 作用：在已加载模块的磁盘映像里搜索池标记字节序列。
        // 传入 tagBytes 4 字节标记与 modules 模块表；返回命中的模块名列表。
        // 依据：驱动调用 ExAllocatePoolWithTag 时标记是编译期立即数，
        // 必然以这 4 个字节的形式出现在自己的映像里。
        QStringList FindTagOwners(
            const QByteArray& tagBytes,
            const std::vector<ModuleEntry>& modules)
        {
            QStringList owners;  // owners：命中的模块名。
            QSet<QString> seen;  // seen：路径去重，同名模块只扫一次。
            int scanned = 0;     // scanned：已扫描模块数。

            for (const ModuleEntry& module : modules)
            {
                CheckDumpCancellation();
                if (scanned >= kMaxScanModules)
                {
                    break;
                }
                const QString path = NormalizeModulePath(module.name);
                if (path.isEmpty() || seen.contains(path.toLower()))
                {
                    continue;
                }
                seen.insert(path.toLower());

                QFile file(path);
                if (!file.open(QIODevice::ReadOnly))
                {
                    continue;
                }
                if (file.size() > kMaxScanBytes)
                {
                    continue;
                }
                ++scanned;
                // Preserve matches straddling chunk boundaries, while allowing
                // cancellation between reads instead of one 64 MiB readAll().
                QByteArray overlap;
                while (!file.atEnd())
                {
                    CheckDumpCancellation();
                    QByteArray content = file.read(64 * 1024);
                    CheckDumpCancellation();
                    if (content.isEmpty())
                    {
                        break;
                    }
                    content.prepend(overlap);
                    if (content.contains(tagBytes))
                    {
                        owners.append(BaseModuleName(module.name));
                        break;
                    }
                    overlap = content.right(tagBytes.size() - 1);
                }
            }
            owners.removeDuplicates();
            return owners;
        }
    }

    bool LooksLikePoolTag(const std::uint32_t value)
    {
        if (value == 0)
        {
            return false;
        }
        // 逐字节检查，并且要求至少有两个字母——纯数字或纯符号的四字节组合
        // 在参数里太常见了，当成标记只会制造噪声。
        int letters = 0; // letters：字母个数。
        for (int shift = 0; shift < 32; shift += 8)
        {
            const auto part = static_cast<unsigned char>((value >> shift) & 0xFFU);
            if (!PoolTagCharOk(part))
            {
                return false;
            }
            if ((part >= 'A' && part <= 'Z') || (part >= 'a' && part <= 'z'))
            {
                ++letters;
            }
        }
        return letters >= 2;
    }

    QString PoolTagText(const std::uint32_t value)
    {
        QString text; // text：还原出的标记。
        text.reserve(4);
        for (int shift = 0; shift < 32; shift += 8)
        {
            const auto part = static_cast<unsigned char>((value >> shift) & 0xFFU);
            text.append(QLatin1Char(static_cast<char>(part)));
        }
        return text;
    }

    void ApplyPoolTagAttribution(
        DumpParseResult& result, const DumpCancellationToken& cancellation)
    {
        const ScopedDumpCancellation cancellationScope(cancellation);
        CheckDumpCancellation();
        // candidates：按“原始值 → 来源描述”收集，同一个标记可能出现在多处。
        std::vector<PoolTagCandidate> candidates;
        QSet<std::uint32_t> seenValues; // seenValues：同一个值只登记一次。

        // addCandidate 作用：登记一个候选，重复值只补充来源描述。
        const auto addCandidate =
            [&candidates, &seenValues](const std::uint32_t value, const QString& source)
        {
            if (!LooksLikePoolTag(value))
            {
                return;
            }
            if (seenValues.contains(value))
            {
                for (PoolTagCandidate& existing : candidates)
                {
                    if (existing.rawValue == value)
                    {
                        existing.source += QStringLiteral("、") + source;
                        return;
                    }
                }
                return;
            }
            seenValues.insert(value);
            PoolTagCandidate candidate;
            candidate.rawValue = value;
            candidate.tagText = PoolTagText(value);
            candidate.source = source;
            candidates.push_back(candidate);
        };

        // 停止码参数：池类停止码会把相关地址与类型放在参数里，偶尔直接带标记。
        if (result.bugCheckCode != 0)
        {
            for (int index = 0; index < 4; ++index)
            {
                const std::uint64_t parameter = result.bugCheckParameters[index];
                // 只看低 32 位：标记是 ULONG，高位是地址的一部分时不该误判。
                if (parameter <= 0xFFFFFFFFULL)
                {
                    addCandidate(
                        static_cast<std::uint32_t>(parameter),
                        QStringLiteral("停止码参数 %1").arg(index + 1));
                }
            }
        }

        // 崩溃点寄存器：ExFreePoolWithTag 的第二参数就是标记，
        // 崩在释放路径上时它就明明白白躺在寄存器里——本项目那次 0x50 正是如此。
        for (const RegisterEntry& reg : result.registers)
        {
            if (reg.value <= 0xFFFFFFFFULL)
            {
                addCandidate(
                    static_cast<std::uint32_t>(reg.value),
                    QStringLiteral("寄存器 %1").arg(reg.name));
            }
        }

        if (candidates.empty())
        {
            return;
        }

        const QHash<QString, QString>& known = LoadKnownPoolTags();
        for (PoolTagCandidate& candidate : candidates)
        {
            CheckDumpCancellation();
            const auto hit = known.constFind(candidate.tagText.trimmed());
            if (hit != known.constEnd())
            {
                candidate.knownPurpose = hit.value();
            }
            // 已知标记就不必再扫映像了：官方表的答案比字节搜索更可靠，
            // 而扫描是几百 MB 的磁盘读，能省则省。
            if (candidate.knownPurpose.isEmpty())
            {
                candidate.ownerModules = FindTagOwners(
                    candidate.tagText.toLatin1(), result.modules);
            }
        }

        result.poolTags = candidates;

        // 把最有价值的一条写进结论：归属到具体模块的未知标记。
        for (const PoolTagCandidate& candidate : result.poolTags)
        {
            if (!candidate.ownerModules.isEmpty())
            {
                result.analysis.findings.append(
                    QStringLiteral("池标记 %1（来自%2）出现在这些模块的映像中：%3。池损坏类停止码里，被损坏内存归谁所有往往比调用栈更能指向肇事者——栈上出现的通常只是下一个来分配内存、因而撞上坏链表的发现者。")
                        .arg(candidate.tagText,
                             candidate.source,
                             candidate.ownerModules.join(QStringLiteral("、"))));
            }
        }
    }
}
