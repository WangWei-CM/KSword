#include "CksBackend.h"

#include "../../ksword/service/service.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>

#include <Windows.h>
#include <slpublic.h>
#include <softpub.h>
#include <wintrust.h>

#pragma comment(lib, "Slc.lib")
#pragma comment(lib, "Wintrust.lib")

namespace
{
    // kCksPolicyName：Windows 许可 API 的正式 CKS 产品策略名。
    constexpr wchar_t kCksPolicyName[] =
        L"CodeIntegrity-AllowConfigurablePolicy-CustomKernelSigners";

    // Win32Error：只向界面传错误码，避免后台线程生成不可翻译的系统消息。
    QString Win32Error(const unsigned long code)
    {
        return QStringLiteral("Win32 %1 (0x%2)")
            .arg(code)
            .arg(code, 8, 16, QChar('0'));
    }

    // ReadRegistryDword：读取单个 DWORD；失败保留错误码及“未知”状态。
    ks::misc::cks::Check ReadRegistryDword(const wchar_t* keyPath, const wchar_t* valueName)
    {
        ks::misc::cks::Check result;
        DWORD value = 0; // value：RegGetValueW 写入的原始 DWORD。
        DWORD size = sizeof(value); // size：要求完整 DWORD，避免误读短值。
        const LSTATUS status = RegGetValueW(
            HKEY_LOCAL_MACHINE, keyPath, valueName,
            RRF_RT_REG_DWORD, nullptr, &value, &size);
        result.known = status == ERROR_SUCCESS && size == sizeof(value);
        result.enabled = result.known && value != 0;
        result.error = result.known ? 0U : static_cast<std::uint32_t>(status);
        return result;
    }

    // IsElevated：读取进程令牌的提升位，不把管理员组成员身份等同于已提升。
    bool IsElevated()
    {
        HANDLE token = nullptr; // token：当前进程令牌，由本函数关闭。
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        {
            return false;
        }
        TOKEN_ELEVATION elevation{}; // elevation：Windows 返回的提升状态。
        DWORD size = 0; // size：GetTokenInformation 实际写入的字节数。
        const BOOL ok = GetTokenInformation(
            token, TokenElevation, &elevation, sizeof(elevation), &size);
        CloseHandle(token);
        return ok && size == sizeof(elevation) && elevation.TokenIsElevated != 0;
    }

    // CurrentBuild：直接读取系统版本，避免应用清单影响 GetVersionEx 结果。
    std::uint32_t CurrentBuild()
    {
        using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll"); // ntdll：系统已加载模块，不需要释放。
        if (ntdll == nullptr)
        {
            return 0;
        }
        const auto query = reinterpret_cast<RtlGetVersionFn>(
            GetProcAddress(ntdll, "RtlGetVersion"));
        OSVERSIONINFOW version{}; // version：系统真实版本与 build。
        version.dwOSVersionInfoSize = sizeof(version);
        return query != nullptr && query(&version) >= 0 ? version.dwBuildNumber : 0;
    }

    // CiToolPath：只调用 Windows 系统目录中的 CiTool，不搜索 PATH。
    QString CiToolPath()
    {
        wchar_t windowsDir[MAX_PATH]{}; // windowsDir：GetWindowsDirectoryW 的受限输出缓冲。
        const UINT count = GetWindowsDirectoryW(windowsDir, MAX_PATH);
        if (count == 0 || count >= MAX_PATH)
        {
            return {};
        }
        return QDir(QString::fromWCharArray(windowsDir))
            .filePath(QStringLiteral("System32/CiTool.exe"));
    }

    // JsonBool：兼容 CiTool 各版本的 JSON 布尔值和字符串值。
    bool JsonBool(const QJsonValue& value)
    {
        if (value.isBool())
        {
            return value.toBool();
        }
        return value.toString().compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0;
    }

    // QueryPolicies：CiTool 的列表只是系统当前读回，不证明策略被 CKS 信任。
    void QueryPolicies(ks::misc::cks::Snapshot* snapshot)
    {
        const QString tool = CiToolPath(); // tool：固定的系统工具绝对路径。
        if (tool.isEmpty() || !QFileInfo::exists(tool))
        {
            snapshot->policyError = QStringLiteral("CiTool.exe unavailable");
            return;
        }
        QProcess process; // process：仅执行系统工具，不经 cmd 或 PowerShell。
        process.start(tool, {QStringLiteral("--list-policies"), QStringLiteral("-json")});
        if (!process.waitForStarted(3000) || !process.waitForFinished(12000))
        {
            process.kill();
            process.waitForFinished(1000);
            snapshot->policyError = QStringLiteral("CiTool query timed out or could not start");
            return;
        }
        if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
        {
            snapshot->policyError = QStringLiteral("CiTool exit code %1").arg(process.exitCode());
            return;
        }
        QJsonParseError parseError{}; // parseError：保留 JSON 无法解析的具体位置。
        const QJsonDocument document = QJsonDocument::fromJson(
            process.readAllStandardOutput(), &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject())
        {
            snapshot->policyError = QStringLiteral("CiTool JSON: %1")
                .arg(parseError.errorString());
            return;
        }
        const QJsonArray rows = document.object().value(QStringLiteral("Policies")).toArray();
        for (const QJsonValue& value : rows)
        {
            const QJsonObject row = value.toObject(); // row：一条 CiTool 策略记录。
            ks::misc::cks::Policy policy;
            policy.id = row.value(QStringLiteral("PolicyID")).toString();
            policy.name = row.value(QStringLiteral("FriendlyName")).toString();
            const QJsonValue enforced = row.value(QStringLiteral("IsEnforced"));
            const QJsonValue signedValue = row.contains(QStringLiteral("IsSignedPolicy"))
                ? row.value(QStringLiteral("IsSignedPolicy"))
                : row.value(QStringLiteral("IsSigned"));
            policy.enforcedKnown = !enforced.isUndefined();
            policy.enforced = JsonBool(enforced);
            policy.signedKnown = !signedValue.isUndefined();
            policy.signedPolicy = JsonBool(signedValue);
            snapshot->policies.push_back(policy);
        }
    }

    // QuerySsdeService：调用仓库共享服务层，保留错误与不存在的区别。
    ks::misc::cks::Service QuerySsdeService()
    {
        ks::misc::cks::Service result;
        ks::service::ServiceRecord record; // record：SCM 的完整只读回执。
        std::string error; // error：共享服务层的失败说明。
        std::uint32_t code = 0; // code：用于识别服务不存在。
        if (!ks::service::QueryServiceRecord(L"ssde", &record, &error, &code))
        {
            result.queryFailed = code != ERROR_SERVICE_DOES_NOT_EXIST;
            result.errorText = QString::fromStdString(error);
            return result;
        }
        result.installed = true;
        result.running = record.hasStatus && record.status.currentState == SERVICE_RUNNING;
        result.bootStart = record.hasConfig && record.config.startType == SERVICE_BOOT_START;
        result.imagePath = QString::fromStdWString(record.config.binaryPath);
        wchar_t windowsDir[MAX_PATH]{}; // windowsDir：用于核对镜像必须位于系统驱动目录。
        const UINT count = GetWindowsDirectoryW(windowsDir, MAX_PATH);
        if (count != 0 && count < MAX_PATH)
        {
            const QString root = QString::fromWCharArray(windowsDir);
            const QString expected = QDir(root).filePath(QStringLiteral("System32/drivers/ssde.sys"));
            QString image = result.imagePath.trimmed(); // image：展开 SCM 常见的系统目录写法。
            image.remove(QLatin1Char('"'));
            image.replace(QStringLiteral("%SystemRoot%"), root, Qt::CaseInsensitive);
            image.replace(QStringLiteral("%windir%"), root, Qt::CaseInsensitive);
            if (image.startsWith(QStringLiteral("\\SystemRoot\\"), Qt::CaseInsensitive))
            {
                image = root + image.mid(QStringLiteral("\\SystemRoot").size());
            }
            result.expectedImage = QDir::cleanPath(image).compare(
                QDir::cleanPath(expected), Qt::CaseInsensitive) == 0;
        }
        return result;
    }

    // TrustedSignature：Windows Authenticode 信任验证，只确认当前系统信任签名。
    // 这个检查不能代替 PK/KEK 链、CI 策略和内核实际加载验证。
    bool TrustedSignature(const QString& path)
    {
        const std::wstring nativePath = QDir::toNativeSeparators(path).toStdWString();
        WINTRUST_FILE_INFO fileInfo{}; // fileInfo：待验证的固定驱动文件路径。
        fileInfo.cbStruct = sizeof(fileInfo);
        fileInfo.pcwszFilePath = nativePath.c_str();
        WINTRUST_DATA trust{}; // trust：禁止 UI 与联网吊销查询的验证参数。
        trust.cbStruct = sizeof(trust);
        trust.dwUIChoice = WTD_UI_NONE;
        trust.fdwRevocationChecks = WTD_REVOKE_NONE;
        trust.dwUnionChoice = WTD_CHOICE_FILE;
        trust.pFile = &fileInfo;
        trust.dwStateAction = WTD_STATEACTION_VERIFY;
        trust.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;
        GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2; // action：标准 Authenticode 动作。
        const LONG result = WinVerifyTrust(nullptr, &action, &trust);
        trust.dwStateAction = WTD_STATEACTION_CLOSE;
        WinVerifyTrust(nullptr, &action, &trust);
        return result == ERROR_SUCCESS;
    }

    // FileHash：用于复制前后的同一性回读，不把哈希当作签名验证。
    QByteArray FileHash(const QString& path)
    {
        QFile file(path); // file：只读打开源文件或刚复制的目标文件。
        if (!file.open(QIODevice::ReadOnly))
        {
            return {};
        }
        QCryptographicHash hash(QCryptographicHash::Sha256);
        return hash.addData(&file) ? hash.result() : QByteArray();
    }
}

namespace ks::misc::cks
{
    Snapshot QuerySnapshot()
    {
        Snapshot snapshot;
        snapshot.elevated = IsElevated();
        snapshot.build = CurrentBuild();
        snapshot.secureBoot = ReadRegistryDword(
            L"SYSTEM\\CurrentControlSet\\Control\\SecureBoot\\State",
            L"UEFISecureBootEnabled");
        snapshot.hvci = ReadRegistryDword(
            L"SYSTEM\\CurrentControlSet\\Control\\DeviceGuard\\Scenarios\\HypervisorEnforcedCodeIntegrity",
            L"Enabled");

        // 许可 API 返回运行中产品策略；Licensed 是独立的受保护状态。
        DWORD policyValue = 0; // policyValue：SLGetWindowsInformationDWORD 的输出。
        const HRESULT policyResult = SLGetWindowsInformationDWORD(kCksPolicyName, &policyValue);
        snapshot.productPolicy.known = SUCCEEDED(policyResult);
        snapshot.productPolicy.enabled = SUCCEEDED(policyResult) && policyValue != 0;
        snapshot.productPolicy.error = SUCCEEDED(policyResult)
            ? 0U : static_cast<std::uint32_t>(policyResult);
        snapshot.licensed = ReadRegistryDword(
            L"SYSTEM\\CurrentControlSet\\Control\\CI\\Protected", L"Licensed");
        snapshot.ssde = QuerySsdeService();
        QueryPolicies(&snapshot);
        return snapshot;
    }

    bool DeploySignedPolicy(const QString& path, QString* detailOut)
    {
        const QFileInfo file(path); // file：用户显式选择的策略文件。
        const QString tool = CiToolPath(); // tool：固定 Windows 系统工具路径。
        const Check secureBoot = ReadRegistryDword(
            L"SYSTEM\\CurrentControlSet\\Control\\SecureBoot\\State",
            L"UEFISecureBootEnabled");
        if (!IsElevated() || CurrentBuild() < 26100U
            || !secureBoot.known || !secureBoot.enabled
            || !file.isFile() || file.isSymLink()
            || file.suffix().compare(QStringLiteral("cip"), Qt::CaseInsensitive) != 0
            || file.size() <= 0 || file.size() > 64 * 1024 * 1024
            || tool.isEmpty() || !QFileInfo::exists(tool))
        {
            if (detailOut) *detailOut = QStringLiteral("Administrator, Secure Boot, supported build, .cip and CiTool required");
            return false;
        }
        QProcess process; // process：参数列表独立传递，路径不会被 shell 解析。
        process.start(tool, {QStringLiteral("--update-policy"), file.absoluteFilePath()});
        if (!process.waitForStarted(3000) || !process.waitForFinished(60000))
        {
            process.kill();
            process.waitForFinished(1000);
            if (detailOut) *detailOut = QStringLiteral("CiTool deployment timed out");
            return false;
        }
        if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
        {
            if (detailOut) *detailOut = QStringLiteral("CiTool exit %1: %2")
                .arg(process.exitCode())
                .arg(QString::fromLocal8Bit(process.readAllStandardError()).left(500));
            return false;
        }
        if (detailOut) *detailOut = QStringLiteral("CiTool accepted policy deployment; reset still required");
        return true;
    }

    bool LaunchSsdeEnable(const QString& path, QString* detailOut)
    {
        const QFileInfo file(path); // file：用户选择的上游 SSDE 启用程序。
        const QString name = file.fileName(); // name：只接受 valinet 的 SSDE 工具名。
        DWORD setupType = 0; // setupType：避免覆盖其他安装程序的 Setup 启动流程。
        DWORD size = sizeof(setupType); // size：只接受完整 DWORD。
        const LSTATUS setupStatus = RegGetValueW(HKEY_LOCAL_MACHINE, L"SYSTEM\\Setup",
            L"SetupType", RRF_RT_REG_DWORD, nullptr, &setupType, &size);
        if (!IsElevated() || !file.isFile() || file.isSymLink()
            || name.compare(QStringLiteral("ssde_enable.exe"), Qt::CaseInsensitive) != 0
            || setupStatus != ERROR_SUCCESS || setupType != 0)
        {
            if (detailOut) *detailOut = QStringLiteral("Elevated SSDE helper and idle Setup state required");
            return false;
        }
        const bool launched = QProcess::startDetached(file.absoluteFilePath(), {}, file.absolutePath());
        if (detailOut) *detailOut = launched
            ? QStringLiteral("SSDE helper started in a separate console; no result confirmed")
            : QStringLiteral("SSDE helper could not start");
        return launched;
    }

    bool InstallSsdeDriver(const QString& path, QString* detailOut)
    {
        const Snapshot before = QuerySnapshot(); // before：修改前重新读取，不信任页面缓存。
        const QFileInfo source(path); // source：用户明确选择的已签名驱动。
        if (!before.elevated || !before.secureBoot.known || !before.secureBoot.enabled
            || !before.productPolicy.known || !before.productPolicy.enabled
            || !before.licensed.known || !before.licensed.enabled
            || before.ssde.installed || before.ssde.queryFailed
            || !source.isFile() || source.isSymLink()
            || source.fileName().compare(QStringLiteral("ssde.sys"), Qt::CaseInsensitive) != 0
            || source.size() <= 0 || source.size() > 16 * 1024 * 1024
            || !TrustedSignature(source.absoluteFilePath()))
        {
            if (detailOut) *detailOut = QStringLiteral("CKS/secure boot/license, trusted ssde.sys and no existing service required");
            return false;
        }
        wchar_t windowsDir[MAX_PATH]{}; // windowsDir：系统目录定位，不从用户环境变量拼目标。
        const UINT count = GetWindowsDirectoryW(windowsDir, MAX_PATH);
        if (count == 0 || count >= MAX_PATH)
        {
            if (detailOut) *detailOut = QStringLiteral("Windows directory unavailable");
            return false;
        }
        const QString target = QDir(QString::fromWCharArray(windowsDir))
            .filePath(QStringLiteral("System32/drivers/ssde.sys"));
        const QByteArray sourceHash = FileHash(source.absoluteFilePath());
        if (sourceHash.isEmpty() || QFileInfo::exists(target))
        {
            if (detailOut) *detailOut = QStringLiteral("Target exists or source read failed; nothing overwritten");
            return false;
        }
        const std::wstring from = QDir::toNativeSeparators(source.absoluteFilePath()).toStdWString();
        const std::wstring to = QDir::toNativeSeparators(target).toStdWString();
        if (!CopyFileW(from.c_str(), to.c_str(), TRUE))
        {
            if (detailOut) *detailOut = Win32Error(GetLastError());
            return false;
        }
        if (sourceHash != FileHash(target))
        {
            DeleteFileW(to.c_str());
            if (detailOut) *detailOut = QStringLiteral("Copied image hash mismatch");
            return false;
        }
        SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE);
        if (manager == nullptr)
        {
            const DWORD error = GetLastError();
            DeleteFileW(to.c_str());
            if (detailOut) *detailOut = Win32Error(error);
            return false;
        }
        SC_HANDLE service = CreateServiceW(manager, L"ssde", L"SSDE CKS persistence",
            SERVICE_QUERY_STATUS | SERVICE_QUERY_CONFIG, SERVICE_KERNEL_DRIVER,
            SERVICE_DEMAND_START, SERVICE_ERROR_NORMAL, to.c_str(),
            nullptr, nullptr, nullptr, nullptr, nullptr);
        const DWORD createError = service == nullptr ? GetLastError() : ERROR_SUCCESS;
        if (service != nullptr) CloseServiceHandle(service);
        CloseServiceHandle(manager);
        if (createError != ERROR_SUCCESS)
        {
            DeleteFileW(to.c_str());
            if (detailOut) *detailOut = Win32Error(createError);
            return false;
        }
        if (detailOut) *detailOut = QStringLiteral("Signed driver installed as demand-start; start and verify before boot-start");
        return true;
    }

    bool StartSsdeDriver(QString* detailOut)
    {
        const Snapshot before = QuerySnapshot(); // before：防止许可或路径在上次刷新后改变。
        if (!before.elevated || !before.secureBoot.known || !before.secureBoot.enabled
            || !before.productPolicy.known || !before.productPolicy.enabled
            || !before.licensed.known || !before.licensed.enabled
            || !before.ssde.installed || !before.ssde.expectedImage)
        {
            if (detailOut) *detailOut = QStringLiteral("CKS license and expected ssde service required");
            return false;
        }
        ks::service::ServiceStatus status; // status：等待后的 SCM 状态回读。
        std::string error; // error：共享服务层的错误说明。
        const bool started = ks::service::StartServiceByName(
            L"ssde", 12000, SERVICE_RUNNING, &status, &error);
        if (detailOut) *detailOut = started && status.currentState == SERVICE_RUNNING
            ? QStringLiteral("SCM reports SSDE RUNNING; kernel load succeeded this boot")
            : QString::fromStdString(error);
        return started && status.currentState == SERVICE_RUNNING;
    }

    bool SetSsdeBootStart(const bool enable, QString* detailOut)
    {
        const Snapshot before = QuerySnapshot(); // before：从 SCM 与许可 API 重新核验。
        if (!before.elevated || !before.ssde.installed || !before.ssde.expectedImage
            || (enable && (!before.ssde.running || !before.secureBoot.known
                || !before.secureBoot.enabled || !before.productPolicy.known
                || !before.productPolicy.enabled || !before.licensed.known
                || !before.licensed.enabled)))
        {
            if (detailOut) *detailOut = QStringLiteral("Expected running/licensed SSDE service required");
            return false;
        }
        ks::service::ServiceConfigUpdate update; // update：只修改启动类型。
        update.changeStartType = true;
        update.startType = enable ? SERVICE_BOOT_START : SERVICE_DEMAND_START;
        std::string error; // error：ChangeServiceConfigW 的回执。
        const bool changed = ks::service::ChangeServiceConfiguration(L"ssde", update, &error);
        if (detailOut) *detailOut = changed
            ? QStringLiteral("SCM start type updated; next boot still requires validation")
            : QString::fromStdString(error);
        return changed;
    }
}
