#include "DumpSymbolResolver.h"

// BaseModuleName 在这里：符号解析要反复把"完整路径"化简成"文件名"。
#include "DumpSymbolIndex.h"
#include "../ksword/dbghelp_serialization.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QStringList>

#include <algorithm>
#include <array>
#include <mutex>

#include <windows.h>
#include <dbghelp.h>

#pragma comment(lib, "Dbghelp.lib")

namespace ks::minidump
{
    namespace
    {
        // g_dbgHelpMutex：DbgHelp 的会话状态是进程级的，同一时刻只允许一个会话。
        // 解析在线程池 worker 里跑，多次解析可能并发，这把锁是必需的。
        std::mutex& g_dbgHelpMutex = ks::dbghelp::SerializationMutex();

        // kFakeProcess：SymInitialize 需要一个唯一句柄做会话标识。离线符号化没有
        // 真实进程可用，按 DbgHelp 的惯例传一个不与任何真实句柄冲突的常量即可。
        void* FakeProcessHandle()
        {
            return reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x4B53574DU)); // 'KSWM'
        }

        // ImageIdentity：从磁盘 PE 头读出的身份字段，用于与转储记录比对。
        struct ImageIdentity
        {
            bool valid = false;              // valid：是否成功读出。
            std::uint32_t timeDateStamp = 0; // timeDateStamp：PE FileHeader.TimeDateStamp。
            std::uint32_t sizeOfImage = 0;   // sizeOfImage：OptionalHeader.SizeOfImage。
        };

        // ReadLe32 作用：从字节缓冲按小端读一个 32 位值。
        // 传入 data 缓冲首指针；返回读出的数值。调用方负责保证至少 4 字节可读。
        std::uint32_t ReadLe32(const unsigned char* const data)
        {
            return static_cast<std::uint32_t>(data[0]) |
                   (static_cast<std::uint32_t>(data[1]) << 8) |
                   (static_cast<std::uint32_t>(data[2]) << 16) |
                   (static_cast<std::uint32_t>(data[3]) << 24);
        }

        // ReadImageIdentity 作用：只读 PE 头，取出 TimeDateStamp 与 SizeOfImage。
        // 传入 path 磁盘文件路径；返回身份字段，任何一步不合法都返回 valid=false。
        // 不用 Windows 的映像加载 API：这里只需要两个字段，手工读比映射整个映像
        // 更省、也不会因为文件被占用而失败。
        ImageIdentity ReadImageIdentity(const QString& path)
        {
            ImageIdentity identity; // identity：待返回的身份字段。

            CheckDumpCancellation();
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly))
            {
                return identity;
            }

            // header：PE 头部区域。0x400 字节足以覆盖 DOS 头 + NT 头 + 可选头。
            const QByteArray header = file.read(0x400);
            CheckDumpCancellation();
            const auto* const bytes = reinterpret_cast<const unsigned char*>(header.constData());
            const int available = static_cast<int>(header.size());
            if (available < 0x40 || bytes[0] != 'M' || bytes[1] != 'Z')
            {
                return identity;
            }

            // ntOffset：IMAGE_DOS_HEADER.e_lfanew。
            const std::uint32_t ntOffset = ReadLe32(bytes + 0x3C);
            // NT 签名(4) + FileHeader(20) + 可选头到 SizeOfImage 为止(60)。
            if (ntOffset > static_cast<std::uint32_t>(available) ||
                static_cast<std::uint64_t>(ntOffset) + 4 + 20 + 60 >
                    static_cast<std::uint64_t>(available))
            {
                return identity;
            }
            if (ReadLe32(bytes + ntOffset) != 0x00004550U) // 'PE\0\0'
            {
                return identity;
            }

            // TimeDateStamp 位于 IMAGE_FILE_HEADER 起始后 4 字节处。
            identity.timeDateStamp = ReadLe32(bytes + ntOffset + 4 + 4);
            // SizeOfImage 在 PE32 与 PE32+ 的可选头里偏移都是 56，无需区分位宽。
            identity.sizeOfImage = ReadLe32(bytes + ntOffset + 4 + 20 + 56);
            identity.valid = true;
            return identity;
        }

        // NormalizeNativePath 作用：把内核风格路径换算成可直接打开的 Win32 路径。
        // 传入 raw 模块记录里的路径；返回换算结果，无法换算时返回空串。
        // 内核转储里的驱动路径形如 \SystemRoot\System32\drivers\x.sys 或 \??\C:\...，
        // 直接拿去 QFile::open 是打不开的。
        QString NormalizeNativePath(const QString& raw)
        {
            if (raw.isEmpty())
            {
                return QString();
            }

            QString path = raw; // path：逐步换算中的路径。
            if (path.startsWith(QStringLiteral("\\??\\"), Qt::CaseInsensitive))
            {
                path = path.mid(4);
            }
            else if (path.startsWith(QStringLiteral("\\SystemRoot\\"), Qt::CaseInsensitive))
            {
                // systemRoot：真实的 Windows 目录，不写死 C:\Windows。
                const QString systemRoot =
                    QString::fromLocal8Bit(qgetenv("SystemRoot"));
                if (systemRoot.isEmpty())
                {
                    return QString();
                }
                path = systemRoot + QStringLiteral("\\") + path.mid(12);
            }
            else if (path.startsWith(QStringLiteral("\\Device\\"), Qt::CaseInsensitive))
            {
                // \Device\HarddiskVolumeN\... 需要卷设备映射才能换算，这里不做猜测，
                // 交给调用方按文件名去搜索路径里找。
                return QString();
            }

            // 到这里必须已经是带盘符或 UNC 的路径，否则不认。
            if (path.size() >= 2 && path.at(1) == QLatin1Char(':'))
            {
                return QDir::toNativeSeparators(path);
            }
            if (path.startsWith(QStringLiteral("\\\\")))
            {
                return QDir::toNativeSeparators(path);
            }
            return QString();
        }

        // SplitSearchPath 作用：把分号分隔的搜索路径拆成目录列表。
        // 传入 searchPath；返回去空后的目录列表。
        QStringList SplitSearchPath(const QString& searchPath)
        {
            QStringList directories; // directories：拆分结果。
            const QStringList parts = searchPath.split(QLatin1Char(';'), Qt::SkipEmptyParts);
            for (const QString& part : parts)
            {
                const QString trimmed = part.trimmed();
                if (!trimmed.isEmpty())
                {
                    directories.append(QDir::toNativeSeparators(trimmed));
                }
            }
            return directories;
        }

        // LocateImage 作用：在磁盘上找到某个模块对应的映像文件。
        // 传入 module 模块记录与 searchDirs 搜索目录；返回找到的完整路径，找不到返回空串。
        // 先试模块自己记录的路径，再按文件名在搜索目录里找——后者覆盖了
        // “把驱动从别处拷过来分析”这种常见场景。
        QString LocateImage(const ModuleEntry& module, const QStringList& searchDirs)
        {
            const QString native = NormalizeNativePath(module.name);
            if (!native.isEmpty() && QFileInfo::exists(native))
            {
                return native;
            }

            const QString baseName = BaseModuleName(module.name);
            if (baseName.isEmpty())
            {
                return QString();
            }
            for (const QString& directory : searchDirs)
            {
                CheckDumpCancellation();
                const QString candidate =
                    QDir(directory).absoluteFilePath(baseName);
                if (QFileInfo::exists(candidate))
                {
                    return QDir::toNativeSeparators(candidate);
                }
            }
            return QString();
        }
    }

    QString ResolvedSymbol::functionText() const
    {
        if (!valid || functionName.isEmpty())
        {
            return QString();
        }
        // 偏移为 0 时不缀 +0x0，读起来更干净。
        if (displacement == 0)
        {
            return moduleName.isEmpty()
                       ? functionName
                       : QStringLiteral("%1!%2").arg(moduleName, functionName);
        }
        const QString suffix =
            QStringLiteral("+0x%1").arg(displacement, 0, 16);
        return moduleName.isEmpty()
                   ? functionName + suffix
                   : QStringLiteral("%1!%2%3").arg(moduleName, functionName, suffix);
    }

    QString ResolvedSymbol::sourceText() const
    {
        // 映像不匹配时一律不给行号：错的行号比没有行号更有害，
        // 它会让人笃定地去读一段根本没被执行到的代码。
        if (!valid || match != SymbolMatchState::Matched ||
            sourceFile.isEmpty() || sourceLine == 0)
        {
            return QString();
        }
        return QStringLiteral("%1:%2").arg(sourceFile).arg(sourceLine);
    }

    SymbolResolver::SymbolResolver() = default;

    SymbolResolver::~SymbolResolver()
    {
        if (m_initialized)
        {
            ::SymCleanup(static_cast<HANDLE>(m_handle));
            m_initialized = false;
        }
    }

    bool SymbolResolver::begin(
        const QString& searchPath,
        const std::vector<ModuleEntry>& modules)
    {
        if (m_initialized)
        {
            return true;
        }

        m_dbgHelpLock = LockDumpMutex(g_dbgHelpMutex);
        m_handle = FakeProcessHandle();

        // 不设 SYMOPT_DEFERRED_LOADS：延迟加载会让“符号是否真的加载成功”变得
        // 不可知，而本模块的全部价值就在于给出一个确定的匹配结论。
        ::SymSetOptions(SYMOPT_UNDNAME | SYMOPT_LOAD_LINES | SYMOPT_NO_PROMPTS);
        CheckDumpCancellation();

        // 第三参数 fInvadeProcess 传 FALSE：离线符号化不枚举本进程模块。
        const std::wstring pathBuffer = searchPath.toStdWString();
        if (::SymInitializeW(
                static_cast<HANDLE>(m_handle),
                pathBuffer.empty() ? nullptr : pathBuffer.c_str(),
                FALSE) == FALSE)
        {
            m_dbgHelpLock.unlock();
            m_handle = nullptr;
            return false;
        }
        m_initialized = true;
        CheckDumpCancellation();

        const QStringList searchDirs = SplitSearchPath(searchPath);
        m_status.reserve(modules.size());
        m_loaded.reserve(modules.size());

        for (const ModuleEntry& module : modules)
        {
            CheckDumpCancellation();
            ModuleSymbolStatus status; // status：本模块的结论。
            status.moduleName = BaseModuleName(module.name);
            if (module.base == 0 || module.size == 0)
            {
                status.state = SymbolMatchState::NotChecked;
                status.detail = QStringLiteral("模块缺少基址或大小，无法定位。");
                m_status.push_back(status);
                continue;
            }

            const QString imagePath = LocateImage(module, searchDirs);
            if (imagePath.isEmpty())
            {
                status.state = SymbolMatchState::ImageMissing;
                status.detail = QStringLiteral(
                    "磁盘上找不到该映像；把它连同 PDB 放进符号搜索路径即可符号化。");
                m_status.push_back(status);
                continue;
            }
            status.imagePath = imagePath;

            // 映像身份比对：这一步是整个模块的立身之本。转储记录的是崩溃当时
            // 加载的那一份映像，磁盘上的那份完全可能已经被重新编译过。
            const ImageIdentity identity = ReadImageIdentity(imagePath);
            if (!identity.valid)
            {
                status.state = SymbolMatchState::ImageMissing;
                status.detail = QStringLiteral("文件存在但不是有效的 PE 映像。");
                m_status.push_back(status);
                continue;
            }

            const bool stampDiffers =
                module.timeDateStamp != 0 &&
                identity.timeDateStamp != module.timeDateStamp;
            const bool sizeDiffers =
                identity.sizeOfImage != 0 &&
                module.size != 0 &&
                identity.sizeOfImage != static_cast<std::uint32_t>(module.size);
            if (stampDiffers || sizeDiffers)
            {
                status.state = SymbolMatchState::ImageMismatch;
                QStringList differences; // differences：逐项差异，写清楚才好判断是不是自己重编译过。
                if (stampDiffers)
                {
                    differences.append(QStringLiteral("时间戳 转储 0x%1 / 磁盘 0x%2")
                                           .arg(module.timeDateStamp, 8, 16, QLatin1Char('0'))
                                           .arg(identity.timeDateStamp, 8, 16, QLatin1Char('0')));
                }
                if (sizeDiffers)
                {
                    differences.append(QStringLiteral("映像大小 转储 0x%1 / 磁盘 0x%2")
                                           .arg(module.size, 0, 16)
                                           .arg(identity.sizeOfImage, 0, 16));
                }
                status.detail = QStringLiteral(
                                    "磁盘上的映像不是崩溃时加载的那一份（%1）。"
                                    "该模块的函数名与行号一律不可信，"
                                    "请换回崩溃时那次构建的映像与 PDB。")
                                    .arg(differences.join(QStringLiteral("；")));
                m_status.push_back(status);
                // 明确不加载符号：宁可只给模块+偏移，也不给会误导人的函数名。
                Loaded loaded;
                loaded.base = module.base;
                loaded.end = module.base + module.size;
                loaded.name = status.moduleName;
                loaded.state = SymbolMatchState::ImageMismatch;
                m_loaded.push_back(loaded);
                continue;
            }

            CheckDumpCancellation();
            const std::wstring imageBuffer = imagePath.toStdWString();
            const std::wstring nameBuffer = status.moduleName.toStdWString();
            const DWORD64 loadedBase = ::SymLoadModuleExW(
                static_cast<HANDLE>(m_handle),
                nullptr,
                imageBuffer.c_str(),
                nameBuffer.empty() ? nullptr : nameBuffer.c_str(),
                static_cast<DWORD64>(module.base),
                static_cast<DWORD>(module.size),
                nullptr,
                0);
            CheckDumpCancellation();
            if (loadedBase == 0)
            {
                status.state = SymbolMatchState::NoSymbols;
                status.detail = QStringLiteral("映像匹配，但 DbgHelp 未能登记该模块。");
                m_status.push_back(status);
                continue;
            }

            IMAGEHLP_MODULEW64 information{};
            information.SizeOfStruct = sizeof(information);
            if (::SymGetModuleInfoW64(
                    static_cast<HANDLE>(m_handle),
                    loadedBase,
                    &information) != FALSE &&
                information.SymType == SymPdb)
            {
                status.state = SymbolMatchState::Matched;
                status.pdbPath = QString::fromWCharArray(information.LoadedPdbName);
                status.detail = QStringLiteral("映像与 PDB 均匹配，函数名与行号可信。");
            }
            else
            {
                status.state = SymbolMatchState::NoSymbols;
                status.detail = QStringLiteral(
                    "映像匹配但没有找到配套 PDB，只能给到模块+偏移。");
            }

            Loaded loaded;
            loaded.base = module.base;
            loaded.end = module.base + module.size;
            loaded.name = status.moduleName;
            loaded.state = status.state;
            m_loaded.push_back(loaded);
            m_status.push_back(status);
        }

        std::sort(
            m_loaded.begin(),
            m_loaded.end(),
            [](const Loaded& left, const Loaded& right) { return left.base < right.base; });
        return true;
    }

    ResolvedSymbol SymbolResolver::resolve(const std::uint64_t address) const
    {
        CheckDumpCancellation();
        ResolvedSymbol resolved; // resolved：待返回的结果。
        if (!m_initialized || address == 0)
        {
            return resolved;
        }

        // 先定位模块，才能带出匹配结论——没有结论的函数名是不可用的。
        const auto hit = std::find_if(
            m_loaded.begin(),
            m_loaded.end(),
            [address](const Loaded& range)
            { return address >= range.base && address < range.end; });
        if (hit == m_loaded.end())
        {
            return resolved;
        }
        resolved.moduleName = hit->name;
        resolved.match = hit->state;
        if (hit->state != SymbolMatchState::Matched)
        {
            return resolved;
        }

        constexpr std::size_t symbolBufferSize =
            sizeof(SYMBOL_INFOW) + (MAX_SYM_NAME * sizeof(wchar_t));
        alignas(SYMBOL_INFOW) std::array<unsigned char, symbolBufferSize> symbolBuffer{};
        auto* const symbolInfo = reinterpret_cast<SYMBOL_INFOW*>(symbolBuffer.data());
        symbolInfo->SizeOfStruct = sizeof(SYMBOL_INFOW);
        symbolInfo->MaxNameLen = MAX_SYM_NAME;

        DWORD64 displacement = 0;
        if (::SymFromAddrW(
                static_cast<HANDLE>(m_handle),
                static_cast<DWORD64>(address),
                &displacement,
                symbolInfo) == FALSE)
        {
            return resolved;
        }
        CheckDumpCancellation();
        resolved.valid = true;
        resolved.functionName = QString::fromWCharArray(symbolInfo->Name);
        resolved.displacement = static_cast<std::uint64_t>(displacement);

        IMAGEHLP_LINEW64 line{};
        line.SizeOfStruct = sizeof(line);
        DWORD lineDisplacement = 0;
        if (::SymGetLineFromAddrW64(
                static_cast<HANDLE>(m_handle),
                static_cast<DWORD64>(address),
                &lineDisplacement,
                &line) != FALSE)
        {
            resolved.sourceFile = QString::fromWCharArray(line.FileName);
            resolved.sourceLine = static_cast<std::uint32_t>(line.LineNumber);
        }
        CheckDumpCancellation();
        return resolved;
    }

    const std::vector<ModuleSymbolStatus>& SymbolResolver::moduleStatus() const
    {
        return m_status;
    }

    QString BuildDefaultSymbolSearchPath(const QString& dumpFilePath)
    {
        QStringList directories; // directories：候选目录，按命中概率从高到低。

        // 转储文件自己所在目录：把 .sys/.pdb 和 .dmp 放一起是最省事的用法。
        if (!dumpFilePath.isEmpty())
        {
            const QString dumpDirectory = QFileInfo(dumpFilePath).absolutePath();
            if (!dumpDirectory.isEmpty())
            {
                directories.append(QDir::toNativeSeparators(dumpDirectory));
            }
        }

        // 本机常见的符号缓存目录。只找本地，不配任何符号服务器：
        // 解析跑在 worker 上，一旦走网络就可能长时间无响应。
        directories.append(QStringLiteral("C:\\Symbols"));
        directories.append(QStringLiteral("C:\\ProgramData\\Dbg\\sym"));

        const QString systemRoot = QString::fromLocal8Bit(qgetenv("SystemRoot"));
        if (!systemRoot.isEmpty())
        {
            directories.append(QDir::toNativeSeparators(
                systemRoot + QStringLiteral("\\System32\\drivers")));
            directories.append(QDir::toNativeSeparators(
                systemRoot + QStringLiteral("\\System32")));
        }

        QStringList existing; // existing：只保留真实存在的目录，避免搜索路径里塞满死路径。
        for (const QString& directory : directories)
        {
            if (!existing.contains(directory, Qt::CaseInsensitive) &&
                QFileInfo(directory).isDir())
            {
                existing.append(directory);
            }
        }
        return existing.join(QLatin1Char(';'));
    }

    QString SymbolMatchStateText(const SymbolMatchState state)
    {
        switch (state)
        {
        case SymbolMatchState::ImageMissing:
            return QStringLiteral("映像缺失");
        case SymbolMatchState::ImageMismatch:
            return QStringLiteral("映像不匹配");
        case SymbolMatchState::NoSymbols:
            return QStringLiteral("无符号");
        case SymbolMatchState::Matched:
            return QStringLiteral("已匹配");
        case SymbolMatchState::NotChecked:
        default:
            return QStringLiteral("未检查");
        }
    }

    void ApplySymbols(
        const QString& searchPath, DumpParseResult& result,
        const DumpCancellationToken& cancellation)
    {
        const ScopedDumpCancellation cancellationScope(cancellation);
        CheckDumpCancellation();
        // effectivePath：调用方没给就用默认本地路径。
        const QString effectivePath =
            searchPath.trimmed().isEmpty()
                ? BuildDefaultSymbolSearchPath(result.filePath)
                : searchPath.trimmed();
        result.symbolSearchPath = effectivePath;
        if (effectivePath.isEmpty() || result.modules.empty())
        {
            return;
        }

        // 只对调用栈与肇事候选里真正出现过的模块加载符号：内核转储动辄一两百个
        // 驱动，全量加载 PDB 会把解析拖成分钟级，而这些模块绝大多数与本次崩溃无关。
        QSet<QString> wanted; // wanted：需要符号的模块名集合（小写便于比对）。
        for (const StackFrameEntry& frame : result.stackFrames)
        {
            CheckDumpCancellation();
            if (!frame.moduleName.isEmpty())
            {
                wanted.insert(frame.moduleName.toLower());
            }
        }
        for (const BlameEntry& blame : result.analysis.blame)
        {
            CheckDumpCancellation();
            if (!blame.moduleName.isEmpty())
            {
                wanted.insert(blame.moduleName.toLower());
            }
        }
        if (wanted.isEmpty())
        {
            return;
        }

        std::vector<ModuleEntry> selected; // selected：待符号化的模块子集。
        selected.reserve(wanted.size());
        for (const ModuleEntry& module : result.modules)
        {
            CheckDumpCancellation();
            if (wanted.contains(BaseModuleName(module.name).toLower()))
            {
                selected.push_back(module);
            }
        }
        if (selected.empty())
        {
            return;
        }

        SymbolResolver resolver;
        if (!resolver.begin(effectivePath, selected))
        {
            result.diagnostics.append(
                QStringLiteral("符号会话建立失败，本次结果只有模块+偏移。"));
            return;
        }
        result.symbolStatus = resolver.moduleStatus();

        for (StackFrameEntry& frame : result.stackFrames)
        {
            CheckDumpCancellation();
            const ResolvedSymbol symbol = resolver.resolve(frame.address);
            frame.functionText = symbol.functionText();
            frame.sourceText = symbol.sourceText();
        }
        for (BlameEntry& blame : result.analysis.blame)
        {
            CheckDumpCancellation();
            const ResolvedSymbol symbol = resolver.resolve(blame.address);
            blame.functionText = symbol.functionText();
        }

        // 不匹配必须顶到用户面前。这类问题最阴险的地方在于：它不报错，
        // 只是悄悄给出错位的行号，而使用者会照着去改一段根本没执行到的代码。
        QStringList mismatched; // mismatched：映像不匹配的模块名。
        for (const ModuleSymbolStatus& status : result.symbolStatus)
        {
            CheckDumpCancellation();
            if (status.state == SymbolMatchState::ImageMismatch)
            {
                mismatched.append(status.moduleName);
            }
        }
        if (!mismatched.isEmpty())
        {
            result.diagnostics.append(
                QStringLiteral("以下模块磁盘映像与崩溃时不是同一份构建，"
                               "已拒绝对其符号化：%1")
                    .arg(mismatched.join(QStringLiteral("、"))));
        }
    }
}
