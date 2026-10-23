#include "HttpsProxyService.h"
#include "../UI/RetainedCleanupJob.h"
#include "../UI/AsyncCleanupRegistry.h"
#include "../ksword/network/network_connection_tools.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <deque>
#include <QtCore/QElapsedTimer>

#include <QtCore/QMetaObject>
#include <QtCore/QCryptographicHash>
#include <QtCore/QDateTime>
#include <QtCore/QDeadlineTimer>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QProcess>
#include <QtCore/QPointer>
#include <QtCore/QStandardPaths>
#include <QtCore/QThread>
#include <QtCore/QThreadPool>
#include <QtNetwork/QSslCertificate>
#include <QtNetwork/QSslCipher>
#include <QtNetwork/QSslConfiguration>
#include <QtNetwork/QSslError>
#include <QtNetwork/QSslKey>
#include <QtNetwork/QSslSocket>
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QTcpSocket>

#include <utility>
#include <vector>

// Windows 证书存储：
// - isRootTrusted 直接查当前用户 ROOT 存储，替代原先为了一个布尔值就拉起 powershell.exe 的做法；
// - windows.h / WinSock2.h 已由 network_connection_tools.h 引入，这里只补 wincrypt 声明。
#include <windows.h>
#include <wincrypt.h>

#pragma comment(lib, "Crypt32.lib")

namespace ks::network
{
    struct HttpsProxyStopState
    {
        std::atomic_bool listenerClosed{true};
        std::atomic_bool stopRequested{false};
        std::mutex mutex;
        std::function<void()> completion;
        std::function<void()> nativeCompletion;
        bool nativeFinished = false;
    };

    // Accepted parsed entries have one FIFO owner; only its wakeup is coalesced.
    // close and publication share the lock, so late session threads never borrow
    // a destroyed QObject. A queued drain owns only this value bridge.
    class HttpsProxyUiDispatcher final : public std::enable_shared_from_this<HttpsProxyUiDispatcher>
    {
    public:
        using ParsedDelivery = std::function<void(const HttpsProxyParsedEntry&)>;
        explicit HttpsProxyUiDispatcher(QObject* receiver, ParsedDelivery parsedDelivery)
            : m_receiver(receiver), m_parsedDelivery(std::move(parsedDelivery)) {}

        void close()
        {
            std::deque<HttpsProxyParsedEntry> retired;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_receiver = nullptr;
                m_parsedDelivery = {};
                retired.swap(m_parsedEntries);
            }
            // Release complete raw/header/certificate values outside the producer lock.
        }

        void post(std::function<void()> callback)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_receiver != nullptr)
            {
                if (m_parsedWake.expired()) scheduleParsedDrainLocked();
                QMetaObject::invokeMethod(m_receiver, std::move(callback), Qt::QueuedConnection);
            }
        }

        void postParsed(const HttpsProxyParsedEntry& parsedEntry)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_receiver == nullptr) return;
            m_parsedEntries.push_back(parsedEntry);
            if (m_parsedWake.expired()) scheduleParsedDrainLocked();
        }

    private:
        void scheduleParsedDrainLocked()
        {
            if (m_receiver == nullptr || m_parsedEntries.empty()) return;
            // Only the queued/running functor owns this ticket. If Qt discards
            // it, the weak ticket expires without locking/posting from a destructor;
            // the next producer/status wake can safely schedule the retained FIFO.
            const auto wake = std::make_shared<char>();
            m_parsedWake = wake;
            const auto bridge = shared_from_this();
            try { QMetaObject::invokeMethod(m_receiver, [bridge, wake]() { bridge->drainParsed(); }, Qt::QueuedConnection); }
            catch (...) { m_parsedWake.reset(); }
        }

        void drainParsed()
        {
            QElapsedTimer elapsed;
            elapsed.start();
            for (int count = 0; count < 64; ++count)
            {
                HttpsProxyParsedEntry entry;
                ParsedDelivery delivery;
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    if (m_receiver == nullptr || m_parsedEntries.empty())
                    {
                        m_parsedWake.reset();
                        return;
                    }
                    entry = std::move(m_parsedEntries.front());
                    m_parsedEntries.pop_front();
                    delivery = m_parsedDelivery;
                }
                // No lock across callbacks: a callback can publish, stop or destroy
                // the service. The next iteration revalidates the same bridge.
                if (delivery) delivery(entry);
                if (elapsed.elapsed() >= 4) break;
            }
            std::lock_guard<std::mutex> lock(m_mutex);
            m_parsedWake.reset();
            scheduleParsedDrainLocked();
        }

        std::mutex m_mutex;
        QObject* m_receiver;
        ParsedDelivery m_parsedDelivery;
        std::deque<HttpsProxyParsedEntry> m_parsedEntries;
        std::weak_ptr<char> m_parsedWake;
    };

    // 会话证书工作不依赖 QObject；慢任务可以安全地晚于服务退出。
    class HttpsProxyCertificateStore final
    {
    public:
        HttpsProxyCertificateStore();
        bool ensureRootCertificate(bool installToTrustStore, QString* errorTextOut);
        bool isRootTrusted() const;
        bool isRootCertificateReady() const;
        bool loadHostCertificateBundle(const QString& hostName, QSslCertificate* certificateOut,
            QSslKey* privateKeyOut, QString* errorTextOut);
        QString rootCertificatePfxPath() const;
        QString rootCertificateCerPath() const;
        void markRootCertificatePrepared() { m_rootCertificatePrepared.store(true); }

    private:
        QString certificateWorkspaceDir() const;
        bool runPowerShellScript(const QString& scriptText, QString* standardOutputOut,
            QString* standardErrorOut, QString* errorTextOut) const;
        QString hostCertificatePfxPath(const QString& hostName) const;
        QString hostCertificatePemPath(const QString& hostName) const;
        QString hostPrivateKeyPemPath(const QString& hostName) const;
        bool ensureHostCertificateFile(const QString& hostName, QString* errorTextOut);
        QString normalizedHostForFileName(const QString& hostName) const;
        QByteArray rootPfxPassword() const;
        QString m_workspaceDirectory;
        mutable std::recursive_mutex m_certificateMutex;
        std::atomic_bool m_rootCertificatePrepared{ false };
    };

    namespace
    {
        // kRootSubjectText 用途：统一根证书 Subject 文本。
        constexpr const char* kRootSubjectText = "CN=Ksword HTTPS Root CA";
        // kRootFriendlyText 用途：统一根证书 FriendlyName。
        constexpr const char* kRootFriendlyText = "Ksword HTTPS Root CA";
        // kPfxPasswordText 用途：导出 PFX 时使用的固定密码。
        constexpr const char* kPfxPasswordText = "KswordHttpsProxy!2026";
        // kMaxConnectHeaderBytes 用途：限制 CONNECT 头缓存大小，避免恶意堆积。
        constexpr int kMaxConnectHeaderBytes = 32 * 1024;
        // kMaxHttpHeaderBytes 用途：限制单个 HTTP 请求或响应头大小，避免异常流量持续占用会话内存。
        constexpr int kMaxHttpHeaderBytes = 64 * 1024;
        // kMaxPendingPlainBytes 用途：限制 TLS 握手完成前的待转发明文，避免慢握手造成无界缓存。
        constexpr int kMaxPendingPlainBytes = 4 * 1024 * 1024;
        // kPowerShellTimeoutMs 用途：单次 PowerShell 脚本的执行上限。
        constexpr int kPowerShellTimeoutMs = 120000;
        // kSessionStopFirstWaitMs 用途：停止代理时对单个会话线程的首轮等待上限。
        constexpr int kSessionStopFirstWaitMs = 3000;
        // kSessionStopFinalWaitMs 用途：请求中断后对单个会话线程的补充等待上限。
        constexpr int kSessionStopFinalWaitMs = 5000;

        // quoteForPowerShell 作用：
        // - 把文本包装成 PowerShell 单引号字面量。
        QString quoteForPowerShell(const QString& textValue)
        {
            QString escapedText = textValue;
            escapedText.replace('\'', QStringLiteral("''"));
            return QStringLiteral("'%1'").arg(escapedText);
        }

        // powerShellExecutionMutex 作用：
        // - 返回进程级 PowerShell 串行锁；
        // - 后台根证书任务与会话线程的叶子证书生成共用同一份工作目录，必须串行执行，
        //   否则可能出现一方正在导出 root_ca.pfx、另一方同时读取该文件的竞态。
        // 返回：进程内唯一的互斥量引用。
        std::mutex& powerShellExecutionMutex()
        {
            static std::mutex executionMutex;
            return executionMutex;
        }

        // executePowerShellScript 作用：
        // - 同步执行一段 PowerShell 脚本，只依赖值类型入参，可在任意线程调用；
        // 参数 scriptText：脚本文本。
        // 参数 standardOutputOut：标准输出，可为空。
        // 参数 standardErrorOut：标准错误，可为空。
        // 参数 errorTextOut：失败时输出错误文本，可为空。
        // 返回：true=执行成功；false=启动失败、超时或退出码非零。
        bool executePowerShellScript(
            const QString& scriptText,
            QString* standardOutputOut,
            QString* standardErrorOut,
            QString* errorTextOut)
        {
            const std::lock_guard<std::mutex> executionGuard(powerShellExecutionMutex());

            QProcess processObject;
            processObject.setProgram(QStringLiteral("powershell.exe"));

            const QByteArray utf16ScriptBytes(
                reinterpret_cast<const char*>(scriptText.utf16()),
                scriptText.size() * static_cast<int>(sizeof(char16_t)));
            processObject.setArguments({
                QStringLiteral("-NoProfile"),
                QStringLiteral("-ExecutionPolicy"),
                QStringLiteral("Bypass"),
                QStringLiteral("-EncodedCommand"),
                QString::fromLatin1(utf16ScriptBytes.toBase64())
                });
            processObject.start();

            if (!processObject.waitForStarted(2000))
            {
                if (errorTextOut != nullptr)
                {
                    *errorTextOut = QStringLiteral("PowerShell 启动失败。");
                }
                return false;
            }

            if (!processObject.waitForFinished(kPowerShellTimeoutMs))
            {
                processObject.kill();
                processObject.waitForFinished(2000);
                if (errorTextOut != nullptr)
                {
                    *errorTextOut = QStringLiteral("PowerShell 执行超时。");
                }
                return false;
            }

            const QString standardOutputText = QString::fromLocal8Bit(processObject.readAllStandardOutput()).trimmed();
            const QString standardErrorText = QString::fromLocal8Bit(processObject.readAllStandardError()).trimmed();
            if (standardOutputOut != nullptr)
            {
                *standardOutputOut = standardOutputText;
            }
            if (standardErrorOut != nullptr)
            {
                *standardErrorOut = standardErrorText;
            }

            if (processObject.exitStatus() != QProcess::NormalExit || processObject.exitCode() != 0)
            {
                if (errorTextOut != nullptr)
                {
                    *errorTextOut = QStringLiteral("PowerShell 执行失败：%1")
                        .arg(standardErrorText.isEmpty() ? QStringLiteral("unknown") : standardErrorText);
                }
                return false;
            }
            return true;
        }

        // buildRootCertificateScriptText 作用：
        // - 拼出“确保根证书存在（可选导入信任根）”的 PowerShell 脚本；
        // - 同步与异步两条路径共用同一份脚本，避免两处文本漂移。
        // 参数 rootPfxPath：根证书 PFX 路径。
        // 参数 rootCerPath：根证书 CER 路径。
        // 参数 installToTrustStore：是否把根证书导入当前用户信任根。
        // 返回：可直接交给 executePowerShellScript 的脚本文本。
        QString buildRootCertificateScriptText(
            const QString& rootPfxPath,
            const QString& rootCerPath,
            const bool installToTrustStore)
        {
            const QString installFlagText = installToTrustStore ? QStringLiteral("1") : QStringLiteral("0");
            return QStringLiteral(
                "$ErrorActionPreference='Stop'; "
                "$ProgressPreference='SilentlyContinue'; "
                "$pfxPath=%1; "
                "$cerPath=%2; "
                "$installRoot=%3; "
                "$friendly=%4; "
                "$subject=%5; "
                "$pwd=ConvertTo-SecureString %6 -AsPlainText -Force; "
                "$rootCert=$null; "
                "if(Test-Path $pfxPath){ "
                "  $pfxData=Get-PfxData -FilePath $pfxPath -Password $pwd; "
                "  $thumb=$pfxData.EndEntityCertificates[0].Thumbprint; "
                "  $rootCert=Get-ChildItem Cert:\\CurrentUser\\My | Where-Object { $_.Thumbprint -eq $thumb } | Select-Object -First 1; "
                "  if($null -eq $rootCert){ "
                "    Import-PfxCertificate -FilePath $pfxPath -CertStoreLocation Cert:\\CurrentUser\\My -Password $pwd | Out-Null; "
                "    $rootCert=Get-ChildItem Cert:\\CurrentUser\\My | Where-Object { $_.Thumbprint -eq $thumb } | Select-Object -First 1; "
                "  } "
                "} "
                "if($null -eq $rootCert){ "
                "  $rootCert=Get-ChildItem Cert:\\CurrentUser\\My | Where-Object { $_.FriendlyName -eq $friendly -or $_.Subject -eq $subject } | Sort-Object NotAfter -Descending | Select-Object -First 1; "
                "} "
                "if($null -ne $rootCert -and -not $rootCert.HasPrivateKey){ "
                "  Remove-Item -Path ('Cert:\\CurrentUser\\My\\' + $rootCert.Thumbprint) -Force -ErrorAction SilentlyContinue; "
                "  $rootCert=$null; "
                "} "
                "if($null -eq $rootCert){ "
                "  $rootCert=New-SelfSignedCertificate -Type Custom -Subject $subject -FriendlyName $friendly -KeyAlgorithm RSA -KeyLength 2048 -HashAlgorithm sha256 "
                "    -CertStoreLocation 'Cert:\\CurrentUser\\My' -KeyExportPolicy Exportable -KeyUsage CertSign,CRLSign,DigitalSignature "
                "    -TextExtension @('2.5.29.19={critical}{text}ca=true&pathlength=1') -NotAfter (Get-Date).AddYears(10); "
                "} "
                "Export-PfxCertificate -Cert $rootCert -FilePath $pfxPath -Password $pwd -Force | Out-Null; "
                "Export-Certificate -Cert $rootCert -FilePath $cerPath -Force | Out-Null; "
                "if($installRoot -eq '1'){ "
                "  $trusted=Get-ChildItem Cert:\\CurrentUser\\Root | Where-Object { $_.Thumbprint -eq $rootCert.Thumbprint } | Select-Object -First 1; "
                "  if($null -eq $trusted){ Import-Certificate -FilePath $cerPath -CertStoreLocation 'Cert:\\CurrentUser\\Root' | Out-Null; } "
                "} "
                "Write-Output $rootCert.Thumbprint;")
                .arg(quoteForPowerShell(rootPfxPath))
                .arg(quoteForPowerShell(rootCerPath))
                .arg(quoteForPowerShell(installFlagText))
                .arg(quoteForPowerShell(QString::fromLatin1(kRootFriendlyText)))
                .arg(quoteForPowerShell(QString::fromLatin1(kRootSubjectText)))
                .arg(quoteForPowerShell(QString::fromLatin1(kPfxPasswordText)));
        }

        // isThumbprintInCurrentUserRootStore 作用：
        // - 在“当前用户-受信任的根证书颁发机构”存储里按 SHA-1 指纹查证书；
        // - 纯本地 CryptoAPI 调用，微秒级返回，可在 UI 线程直接使用。
        // 参数 thumbprintBytes：证书 SHA-1 指纹（20 字节）。
        // 返回：true=已在信任根中；false=未找到或存储打不开。
        bool isThumbprintInCurrentUserRootStore(const QByteArray& thumbprintBytes)
        {
            if (thumbprintBytes.size() != 20)
            {
                return false;
            }

            const HCERTSTORE trustedRootStoreHandle = ::CertOpenSystemStoreW(0, L"ROOT");
            if (trustedRootStoreHandle == nullptr)
            {
                return false;
            }

            CRYPT_HASH_BLOB thumbprintBlob{};
            thumbprintBlob.cbData = static_cast<DWORD>(thumbprintBytes.size());
            thumbprintBlob.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(thumbprintBytes.constData()));

            const PCCERT_CONTEXT matchedCertificateContext = ::CertFindCertificateInStore(
                trustedRootStoreHandle,
                X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
                0,
                CERT_FIND_SHA1_HASH,
                &thumbprintBlob,
                nullptr);
            const bool certificateTrusted = matchedCertificateContext != nullptr;
            if (matchedCertificateContext != nullptr)
            {
                ::CertFreeCertificateContext(matchedCertificateContext);
            }
            ::CertCloseStore(trustedRootStoreHandle, 0);
            return certificateTrusted;
        }

        // currentTimestampMs 作用：
        // - 返回当前 Unix 毫秒时间戳。
        std::uint64_t currentTimestampMs()
        {
            return static_cast<std::uint64_t>(QDateTime::currentMSecsSinceEpoch());
        }

        // sslProtocolToText 作用：
        // - 把 Qt SSL 协议枚举转换为可读文本。
        QString sslProtocolToText(const QSsl::SslProtocol protocolValue)
        {
            switch (protocolValue)
            {
            case QSsl::TlsV1_2:
                return QStringLiteral("TLS 1.2");
            case QSsl::TlsV1_3:
                return QStringLiteral("TLS 1.3");
            case QSsl::TlsV1_2OrLater:
                return QStringLiteral("TLS 1.2+");
            default:
                return QStringLiteral("Unknown");
            }
        }

        // parseConnectAuthority 作用：
        // - 解析 CONNECT authority，支持 host:port 与 [ipv6]:port。
        bool parseConnectAuthority(const QByteArray& authorityText, QString* hostOut, std::uint16_t* portOut)
        {
            if (hostOut == nullptr || portOut == nullptr)
            {
                return false;
            }

            const QString trimmedAuthority = QString::fromUtf8(authorityText).trimmed();
            if (trimmedAuthority.startsWith('['))
            {
                const int closeIndex = trimmedAuthority.indexOf(']');
                const int colonIndex = trimmedAuthority.lastIndexOf(':');
                if (closeIndex <= 0 || colonIndex <= closeIndex)
                {
                    return false;
                }

                bool parseOk = false;
                const int portValue = trimmedAuthority.mid(colonIndex + 1).toInt(&parseOk, 10);
                if (!parseOk || portValue <= 0 || portValue > 65535)
                {
                    return false;
                }

                *hostOut = trimmedAuthority.mid(1, closeIndex - 1);
                *portOut = static_cast<std::uint16_t>(portValue);
                return !hostOut->isEmpty();
            }

            const int colonIndex = trimmedAuthority.lastIndexOf(':');
            if (colonIndex <= 0)
            {
                return false;
            }

            bool parseOk = false;
            const int portValue = trimmedAuthority.mid(colonIndex + 1).toInt(&parseOk, 10);
            if (!parseOk || portValue <= 0 || portValue > 65535)
            {
                return false;
            }

            *hostOut = trimmedAuthority.left(colonIndex).trimmed();
            *portOut = static_cast<std::uint16_t>(portValue);
            return !hostOut->isEmpty();
        }

        // rewriteRequestHeaderToCloseConnection 作用：
        // - 强制改写请求头为 Connection: close；
        // - 去掉 Proxy-Connection 等代理残留字段。
        QByteArray rewriteRequestHeaderToCloseConnection(const QByteArray& originalHeaderBlock)
        {
            const QList<QByteArray> rawLineList = originalHeaderBlock.split('\n');
            QList<QByteArray> outputLineList;
            outputLineList.reserve(rawLineList.size() + 3);

            bool firstLineHandled = false;
            for (const QByteArray& rawLine : rawLineList)
            {
                QByteArray lineText = rawLine;
                if (lineText.endsWith('\r'))
                {
                    lineText.chop(1);
                }

                if (!firstLineHandled)
                {
                    outputLineList.push_back(lineText);
                    firstLineHandled = true;
                    continue;
                }
                if (lineText.isEmpty())
                {
                    continue;
                }

                const int colonIndex = lineText.indexOf(':');
                if (colonIndex <= 0)
                {
                    continue;
                }

                const QByteArray headerName = lineText.left(colonIndex).trimmed().toLower();
                if (headerName == "connection" || headerName == "proxy-connection" || headerName == "keep-alive")
                {
                    continue;
                }
                outputLineList.push_back(lineText);
            }

            outputLineList.push_back(QByteArrayLiteral("Connection: close"));
            outputLineList.push_back(QByteArray());
            outputLineList.push_back(QByteArray());
            return outputLineList.join("\r\n");
        }

        // headerValue 作用：从 HTTP 头块读取指定字段值，字段名比较不区分大小写。
        QByteArray headerValue(const QByteArray& headerBlock, const QByteArray& headerName)
        {
            const QList<QByteArray> lineList = headerBlock.split('\n');
            for (int lineIndex = 1; lineIndex < lineList.size(); ++lineIndex)
            {
                const QByteArray lineText = lineList.at(lineIndex).trimmed();
                const int colonIndex = lineText.indexOf(':');
                if (colonIndex <= 0)
                {
                    continue;
                }
                if (lineText.left(colonIndex).trimmed().compare(headerName, Qt::CaseInsensitive) == 0)
                {
                    return lineText.mid(colonIndex + 1).trimmed();
                }
            }
            return {};
        }

        // contentLengthFromHeader 作用：解析 HTTP Content-Length；未知、缺失或非法时返回 -1。
        qint64 contentLengthFromHeader(const QByteArray& headerBlock)
        {
            bool parseOk = false;
            const qlonglong contentLength = QString::fromLatin1(headerValue(headerBlock, QByteArrayLiteral("Content-Length"))).toLongLong(&parseOk, 10);
            return parseOk && contentLength >= 0 ? static_cast<qint64>(contentLength) : -1;
        }

        // populateRemoteTlsMetadata 作用：把已验证远端 TLS 会话与证书信息写入解析记录。
        void populateRemoteTlsMetadata(HttpsProxyParsedEntry* parsedEntry, const QSslSocket* remoteSocket)
        {
            if (parsedEntry == nullptr || remoteSocket == nullptr)
            {
                return;
            }

            parsedEntry->tlsVersionText = sslProtocolToText(remoteSocket->sessionProtocol());
            parsedEntry->alpnText = QString::fromLatin1(remoteSocket->sslConfiguration().nextNegotiatedProtocol());
            parsedEntry->cipherSuiteText = remoteSocket->sessionCipher().name();

            const QSslCertificate peerCertificate = remoteSocket->peerCertificate();
            if (peerCertificate.isNull())
            {
                return;
            }

            parsedEntry->certificateSubjectText = peerCertificate.subjectInfo(QSslCertificate::CommonName).join(QStringLiteral(", "));
            parsedEntry->certificateIssuerText = peerCertificate.issuerInfo(QSslCertificate::CommonName).join(QStringLiteral(", "));
            parsedEntry->certificateExpiryText = peerCertificate.expiryDate().toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
            parsedEntry->certificateSha256Text = QString::fromLatin1(
                QCryptographicHash::hash(peerCertificate.toDer(), QCryptographicHash::Sha256).toHex(':'));
        }

        class ProxySession final : public QObject
        {
        public:
            using HostCertLoader = std::function<bool(const QString&, QSslCertificate*, QSslKey*, QString*)>;
            using ParsedEmitter = std::function<void(const HttpsProxyParsedEntry&)>;
            using StatusEmitter = std::function<void(const QString&)>;

            ProxySession(
                const std::uint64_t sessionIdValue,
                const qintptr socketDescriptorValue,
                HostCertLoader hostCertLoaderValue,
                ParsedEmitter parsedEmitterValue,
                StatusEmitter statusEmitterValue)
                : QObject(nullptr)
                , m_sessionId(sessionIdValue)
                , m_socketDescriptor(socketDescriptorValue)
                , m_hostCertLoader(std::move(hostCertLoaderValue))
                , m_parsedEmitter(std::move(parsedEmitterValue))
                , m_statusEmitter(std::move(statusEmitterValue))
            {
            }

            void initialize()
            {
                m_clientSocket = new QSslSocket();
                m_clientSocket->setParent(this);
                if (!m_clientSocket->setSocketDescriptor(m_socketDescriptor))
                {
                    emitStatusLine(QStringLiteral("HTTPS代理接管客户端套接字失败：%1").arg(m_clientSocket->errorString()));
                    deleteLater();
                    return;
                }

                connect(m_clientSocket, &QSslSocket::readyRead, this, [this]() { onClientReadyRead(); });
                connect(m_clientSocket, &QSslSocket::disconnected, this, [this]() { closePeerAndDelete(); });
                connect(m_clientSocket, &QSslSocket::sslErrors, this, [this](const QList<QSslError>& errorList) {
                    failWithError(QStringLiteral("客户端 TLS 失败：%1")
                        .arg(errorList.isEmpty() ? m_clientSocket->errorString() : errorList.first().errorString()));
                });
            }

            // requestStop：由服务停止路径在会话所属线程调用，先关闭套接字再销毁会话。
            void requestStop()
            {
                closePeerAndDelete();
            }

        private:
            void onClientReadyRead()
            {
                if (m_clientSocket == nullptr)
                {
                    return;
                }

                if (!m_connectReady)
                {
                    m_connectHeaderBuffer += m_clientSocket->readAll();
                    if (m_connectHeaderBuffer.size() > kMaxConnectHeaderBytes)
                    {
                        failWithError(QStringLiteral("CONNECT 请求头过大。"));
                        return;
                    }

                    const int headerEndIndex = m_connectHeaderBuffer.indexOf("\r\n\r\n");
                    if (headerEndIndex < 0)
                    {
                        return;
                    }

                    const QByteArray headerBlock = m_connectHeaderBuffer.left(headerEndIndex + 4);
                    if (!handleConnectHeader(headerBlock))
                    {
                        return;
                    }
                    m_connectHeaderBuffer.clear();
                    return;
                }

                const QByteArray plainBytes = m_clientSocket->readAll();
                if (plainBytes.isEmpty())
                {
                    return;
                }

                if (!m_requestHeaderHandled)
                {
                    m_requestHeaderBuffer += plainBytes;
                    if (m_requestHeaderBuffer.size() > kMaxHttpHeaderBytes)
                    {
                        failWithError(QStringLiteral("HTTP 请求头过大。"));
                        return;
                    }
                    const int headerEndIndex = m_requestHeaderBuffer.indexOf("\r\n\r\n");
                    if (headerEndIndex < 0)
                    {
                        return;
                    }

                    const QByteArray originalHeaderBlock = m_requestHeaderBuffer.left(headerEndIndex + 4);
                    const QByteArray bodyRemainder = m_requestHeaderBuffer.mid(headerEndIndex + 4);
                    forwardToRemote(rewriteRequestHeaderToCloseConnection(originalHeaderBlock) + bodyRemainder);
                    emitRequestParsedEvent(originalHeaderBlock);
                    m_requestHeaderBuffer.clear();
                    m_requestHeaderHandled = true;
                    return;
                }

                forwardToRemote(plainBytes);
            }

            bool handleConnectHeader(const QByteArray& headerBlock)
            {
                const QList<QByteArray> lineList = headerBlock.split('\n');
                if (lineList.isEmpty())
                {
                    failWithError(QStringLiteral("CONNECT 请求为空。"));
                    return false;
                }

                const QList<QByteArray> firstLineParts = lineList.first().trimmed().split(' ');
                if (firstLineParts.size() < 3 || firstLineParts.at(0).toUpper() != "CONNECT")
                {
                    failWithError(QStringLiteral("当前代理仅支持 HTTPS CONNECT。"));
                    return false;
                }

                if (!parseConnectAuthority(firstLineParts.at(1), &m_targetHostText, &m_targetPort))
                {
                    failWithError(QStringLiteral("CONNECT 目标解析失败。"));
                    return false;
                }

                m_clientProcessText = resolveClientProcessText();
                emitConnectEvent(headerBlock);
                m_connectReady = true;
                m_sessionStartedTimestampMs = currentTimestampMs();
                m_clientSocket->write("HTTP/1.1 200 Connection Established\r\nProxy-Agent: Ksword\r\n\r\n");
                m_clientSocket->flush();

                QSslCertificate localCertificate;
                QSslKey localPrivateKey;
                QString errorText;
                if (!m_hostCertLoader(m_targetHostText, &localCertificate, &localPrivateKey, &errorText))
                {
                    failWithError(QStringLiteral("站点证书加载失败：%1").arg(errorText));
                    return false;
                }

                QSslConfiguration clientConfiguration = QSslConfiguration::defaultConfiguration();
                clientConfiguration.setProtocol(QSsl::TlsV1_2OrLater);
                clientConfiguration.setPeerVerifyMode(QSslSocket::VerifyNone);
                clientConfiguration.setAllowedNextProtocols({ QByteArrayLiteral("http/1.1") });
                clientConfiguration.setLocalCertificate(localCertificate);
                clientConfiguration.setPrivateKey(localPrivateKey);
                m_clientSocket->setSslConfiguration(clientConfiguration);
                connect(m_clientSocket, &QSslSocket::encrypted, this, [this]() { m_clientEncrypted = true; flushPendingToClient(); });

                m_remoteSocket = new QSslSocket(this);
                connect(m_remoteSocket, &QSslSocket::encrypted, this, [this]() { onRemoteEncrypted(); });
                connect(m_remoteSocket, &QSslSocket::readyRead, this, [this]() { onRemoteReadyRead(); });
                connect(m_remoteSocket, &QSslSocket::disconnected, this, [this]() { closePeerAndDelete(); });
                connect(m_remoteSocket, &QSslSocket::sslErrors, this, [this](const QList<QSslError>& errorList) {
                    if (m_remoteSocket != nullptr)
                    {
                        failWithError(QStringLiteral("远端 TLS 证书验证失败：%1")
                            .arg(errorList.isEmpty() ? m_remoteSocket->errorString() : errorList.first().errorString()));
                    }
                });
                connect(m_remoteSocket, &QSslSocket::errorOccurred, this, [this](const QAbstractSocket::SocketError) {
                    if (m_remoteSocket != nullptr)
                    {
                        failWithError(QStringLiteral("远端 TLS 错误：%1").arg(m_remoteSocket->errorString()));
                    }
                });

                QSslConfiguration remoteConfiguration = QSslConfiguration::defaultConfiguration();
                remoteConfiguration.setProtocol(QSsl::TlsV1_2OrLater);
                remoteConfiguration.setPeerVerifyMode(QSslSocket::VerifyPeer);
                remoteConfiguration.setAllowedNextProtocols({ QByteArrayLiteral("http/1.1") });
                m_remoteSocket->setSslConfiguration(remoteConfiguration);
                m_remoteSocket->setPeerVerifyName(m_targetHostText);
                m_remoteSocket->connectToHostEncrypted(m_targetHostText, m_targetPort);

                m_clientSocket->startServerEncryption();
                return true;
            }

            void onRemoteEncrypted()
            {
                m_remoteEncrypted = true;
                HttpsProxyParsedEntry parsedEntry;
                parsedEntry.timestampMs = currentTimestampMs();
                parsedEntry.sessionId = m_sessionId;
                parsedEntry.clientEndpointText = clientEndpointText();
                parsedEntry.clientProcessText = m_clientProcessText;
                parsedEntry.targetHostText = m_targetHostText;
                parsedEntry.targetPort = static_cast<int>(m_targetPort);
                parsedEntry.eventTypeText = QStringLiteral("TLS");
                parsedEntry.sniText = m_targetHostText;
                populateRemoteTlsMetadata(&parsedEntry, m_remoteSocket);
                parsedEntry.detailText = QStringLiteral("远端 TLS 握手完成，证书已验证。");
                emitParsedEntry(parsedEntry);
                emitStatusLine(QStringLiteral("HTTPS 会话 #%1 已建立：%2:%3").arg(m_sessionId).arg(m_targetHostText).arg(m_targetPort));
                flushPendingToRemote();
            }

            void onRemoteReadyRead()
            {
                if (m_remoteSocket == nullptr)
                {
                    return;
                }

                const QByteArray plainBytes = m_remoteSocket->readAll();
                if (plainBytes.isEmpty())
                {
                    return;
                }

                if (!m_responseHeaderHandled)
                {
                    m_responseHeaderBuffer += plainBytes;
                    if (m_responseHeaderBuffer.size() > kMaxHttpHeaderBytes)
                    {
                        failWithError(QStringLiteral("HTTP 响应头过大。"));
                        return;
                    }
                    const int headerEndIndex = m_responseHeaderBuffer.indexOf("\r\n\r\n");
                    if (headerEndIndex < 0)
                    {
                        return;
                    }

                    const QByteArray headerBlock = m_responseHeaderBuffer.left(headerEndIndex + 4);
                    const QByteArray bodyRemainder = m_responseHeaderBuffer.mid(headerEndIndex + 4);
                    forwardToClient(headerBlock + bodyRemainder);
                    emitResponseParsedEvent(headerBlock);
                    m_responseHeaderBuffer.clear();
                    m_responseHeaderHandled = true;
                    return;
                }

                forwardToClient(plainBytes);
            }

            void forwardToRemote(const QByteArray& plainBytes)
            {
                if (plainBytes.isEmpty())
                {
                    return;
                }
                if (m_remoteEncrypted && m_remoteSocket != nullptr)
                {
                    m_remoteSocket->write(plainBytes);
                }
                else
                {
                    if (m_pendingToRemoteBytes.size() + plainBytes.size() > kMaxPendingPlainBytes)
                    {
                        failWithError(QStringLiteral("等待远端 TLS 握手的请求数据过多。"));
                        return;
                    }
                    m_pendingToRemoteBytes += plainBytes;
                }
                m_uploadBytes += static_cast<std::uint64_t>(plainBytes.size());
            }

            void forwardToClient(const QByteArray& plainBytes)
            {
                if (plainBytes.isEmpty())
                {
                    return;
                }
                if (m_clientEncrypted && m_clientSocket != nullptr)
                {
                    m_clientSocket->write(plainBytes);
                }
                else
                {
                    if (m_pendingToClientBytes.size() + plainBytes.size() > kMaxPendingPlainBytes)
                    {
                        failWithError(QStringLiteral("等待客户端 TLS 握手的响应数据过多。"));
                        return;
                    }
                    m_pendingToClientBytes += plainBytes;
                }
                m_downloadBytes += static_cast<std::uint64_t>(plainBytes.size());
            }

            void flushPendingToRemote()
            {
                if (m_remoteEncrypted && m_remoteSocket != nullptr && !m_pendingToRemoteBytes.isEmpty())
                {
                    m_remoteSocket->write(m_pendingToRemoteBytes);
                    m_pendingToRemoteBytes.clear();
                }
            }

            void flushPendingToClient()
            {
                if (m_clientEncrypted && m_clientSocket != nullptr && !m_pendingToClientBytes.isEmpty())
                {
                    m_clientSocket->write(m_pendingToClientBytes);
                    m_pendingToClientBytes.clear();
                }
            }

            void emitConnectEvent(const QByteArray& rawHeaderBlock) const
            {
                HttpsProxyParsedEntry parsedEntry;
                parsedEntry.timestampMs = currentTimestampMs();
                parsedEntry.sessionId = m_sessionId;
                parsedEntry.clientEndpointText = clientEndpointText();
                parsedEntry.clientProcessText = m_clientProcessText;
                parsedEntry.targetHostText = m_targetHostText;
                parsedEntry.targetPort = static_cast<int>(m_targetPort);
                parsedEntry.eventTypeText = QStringLiteral("CONNECT");
                parsedEntry.sniText = m_targetHostText;
                parsedEntry.uploadBytes = m_uploadBytes;
                parsedEntry.downloadBytes = m_downloadBytes;
                parsedEntry.detailText = QStringLiteral("收到 CONNECT 请求。");
                parsedEntry.rawBytes = rawHeaderBlock;
                emitParsedEntry(parsedEntry);
            }

            void emitRequestParsedEvent(const QByteArray& headerBlock)
            {
                QString methodText;
                QString pathText;
                const QList<QByteArray> lineList = headerBlock.split('\n');
                if (!lineList.isEmpty())
                {
                    const QList<QByteArray> firstLineParts = lineList.first().trimmed().split(' ');
                    methodText = QString::fromUtf8(firstLineParts.value(0));
                    pathText = QString::fromUtf8(firstLineParts.value(1));
                }

                HttpsProxyParsedEntry parsedEntry;
                parsedEntry.timestampMs = currentTimestampMs();
                parsedEntry.sessionId = m_sessionId;
                parsedEntry.clientEndpointText = clientEndpointText();
                parsedEntry.clientProcessText = m_clientProcessText;
                parsedEntry.targetHostText = m_targetHostText;
                parsedEntry.targetPort = static_cast<int>(m_targetPort);
                parsedEntry.eventTypeText = QStringLiteral("REQUEST");
                parsedEntry.methodText = methodText;
                parsedEntry.pathText = pathText;
                parsedEntry.sniText = m_targetHostText;
                parsedEntry.contentTypeText = QString::fromUtf8(headerValue(headerBlock, QByteArrayLiteral("Content-Type")));
                parsedEntry.contentLength = contentLengthFromHeader(headerBlock);
                parsedEntry.uploadBytes = m_uploadBytes;
                parsedEntry.downloadBytes = m_downloadBytes;
                m_requestTimestampMs = parsedEntry.timestampMs;
                parsedEntry.detailText = QStringLiteral("请求头已解析，内容正文仅转发，不保存。");
                parsedEntry.rawBytes = headerBlock;
                emitParsedEntry(parsedEntry);
            }

            void emitResponseParsedEvent(const QByteArray& headerBlock) const
            {
                int statusCode = 0;
                const QList<QByteArray> lineList = headerBlock.split('\n');
                if (!lineList.isEmpty())
                {
                    const QList<QByteArray> firstLineParts = lineList.first().trimmed().split(' ');
                    bool parseOk = false;
                    statusCode = QString::fromUtf8(firstLineParts.value(1)).toInt(&parseOk, 10);
                    if (!parseOk)
                    {
                        statusCode = 0;
                    }
                }

                HttpsProxyParsedEntry parsedEntry;
                parsedEntry.timestampMs = currentTimestampMs();
                parsedEntry.sessionId = m_sessionId;
                parsedEntry.clientEndpointText = clientEndpointText();
                parsedEntry.clientProcessText = m_clientProcessText;
                parsedEntry.targetHostText = m_targetHostText;
                parsedEntry.targetPort = static_cast<int>(m_targetPort);
                parsedEntry.eventTypeText = QStringLiteral("RESPONSE");
                parsedEntry.statusCode = statusCode;
                parsedEntry.contentTypeText = QString::fromUtf8(headerValue(headerBlock, QByteArrayLiteral("Content-Type")));
                parsedEntry.contentLength = contentLengthFromHeader(headerBlock);
                parsedEntry.uploadBytes = m_uploadBytes;
                parsedEntry.downloadBytes = m_downloadBytes;
                parsedEntry.elapsedMs = m_requestTimestampMs > 0 && parsedEntry.timestampMs >= m_requestTimestampMs
                    ? parsedEntry.timestampMs - m_requestTimestampMs
                    : 0;
                populateRemoteTlsMetadata(&parsedEntry, m_remoteSocket);
                parsedEntry.detailText = QStringLiteral("响应头已解析，内容正文仅转发，不保存。");
                parsedEntry.rawBytes = headerBlock;
                emitParsedEntry(parsedEntry);
            }

            void failWithError(const QString& errorText)
            {
                if (m_closing)
                {
                    return;
                }
                HttpsProxyParsedEntry parsedEntry;
                parsedEntry.timestampMs = currentTimestampMs();
                parsedEntry.sessionId = m_sessionId;
                parsedEntry.clientEndpointText = clientEndpointText();
                parsedEntry.clientProcessText = m_clientProcessText;
                parsedEntry.targetHostText = m_targetHostText;
                parsedEntry.targetPort = static_cast<int>(m_targetPort);
                parsedEntry.eventTypeText = QStringLiteral("ERROR");
                parsedEntry.uploadBytes = m_uploadBytes;
                parsedEntry.downloadBytes = m_downloadBytes;
                populateRemoteTlsMetadata(&parsedEntry, m_remoteSocket);
                parsedEntry.detailText = errorText;
                emitParsedEntry(parsedEntry);
                emitStatusLine(QStringLiteral("HTTPS 会话 #%1 失败：%2").arg(m_sessionId).arg(errorText));
                closePeerAndDelete();
            }

            QString clientEndpointText() const
            {
                if (m_clientSocket == nullptr)
                {
                    return QStringLiteral("N/A");
                }
                return QStringLiteral("%1:%2").arg(m_clientSocket->peerAddress().toString()).arg(m_clientSocket->peerPort());
            }

            // resolveClientProcessText 作用：按本地代理连接四元组关联 Windows TCP 所有者，补足进程维度。
            QString resolveClientProcessText() const
            {
                if (m_clientSocket == nullptr)
                {
                    return QStringLiteral("未知");
                }

                std::vector<ks::network::TcpConnectionRecord> connectionRecordList;
                std::string errorText;
                if (!ks::network::EnumerateTcpConnectionRecords(connectionRecordList, &errorText))
                {
                    return QStringLiteral("未知");
                }

                const QHostAddress clientAddress = m_clientSocket->peerAddress();
                const QHostAddress proxyAddress = m_clientSocket->localAddress();
                const std::uint16_t clientPort = m_clientSocket->peerPort();
                const std::uint16_t proxyPort = m_clientSocket->localPort();
                for (const ks::network::TcpConnectionRecord& connectionRecord : connectionRecordList)
                {
                    if (connectionRecord.localPort != clientPort || connectionRecord.remotePort != proxyPort)
                    {
                        continue;
                    }

                    const QHostAddress recordLocalAddress(QString::fromStdString(connectionRecord.localAddressText));
                    const QHostAddress recordRemoteAddress(QString::fromStdString(connectionRecord.remoteAddressText));
                    if (recordLocalAddress != clientAddress || recordRemoteAddress != proxyAddress)
                    {
                        continue;
                    }

                    const QString processNameText = QString::fromUtf8(connectionRecord.processName.c_str());
                    return processNameText.isEmpty()
                        ? QStringLiteral("PID %1").arg(connectionRecord.processId)
                        : QStringLiteral("%1 (PID %2)").arg(processNameText).arg(connectionRecord.processId);
                }
                return QStringLiteral("未知");
            }

            void emitParsedEntry(const HttpsProxyParsedEntry& parsedEntry) const
            {
                if (m_parsedEmitter)
                {
                    m_parsedEmitter(parsedEntry);
                }
            }

            void emitStatusLine(const QString& statusText) const
            {
                if (m_statusEmitter)
                {
                    m_statusEmitter(statusText);
                }
            }

            void closePeerAndDelete()
            {
                if (m_closing)
                {
                    return;
                }
                m_closing = true;
                emitSessionSummary();
                if (m_clientSocket != nullptr && m_clientSocket->state() != QAbstractSocket::UnconnectedState)
                {
                    m_clientSocket->disconnectFromHost();
                }
                if (m_remoteSocket != nullptr && m_remoteSocket->state() != QAbstractSocket::UnconnectedState)
                {
                    m_remoteSocket->disconnectFromHost();
                }
                deleteLater();
            }

            // emitSessionSummary 作用：在会话结束时补充可读的总耗时、上下行字节与 TLS 证书信息。
            void emitSessionSummary()
            {
                if (m_summaryEmitted || !m_connectReady)
                {
                    return;
                }
                m_summaryEmitted = true;

                HttpsProxyParsedEntry parsedEntry;
                parsedEntry.timestampMs = currentTimestampMs();
                parsedEntry.sessionId = m_sessionId;
                parsedEntry.clientEndpointText = clientEndpointText();
                parsedEntry.clientProcessText = m_clientProcessText;
                parsedEntry.targetHostText = m_targetHostText;
                parsedEntry.targetPort = static_cast<int>(m_targetPort);
                parsedEntry.eventTypeText = QStringLiteral("SUMMARY");
                parsedEntry.sniText = m_targetHostText;
                parsedEntry.uploadBytes = m_uploadBytes;
                parsedEntry.downloadBytes = m_downloadBytes;
                parsedEntry.elapsedMs = m_sessionStartedTimestampMs > 0 && parsedEntry.timestampMs >= m_sessionStartedTimestampMs
                    ? parsedEntry.timestampMs - m_sessionStartedTimestampMs
                    : 0;
                populateRemoteTlsMetadata(&parsedEntry, m_remoteSocket);
                parsedEntry.detailText = QStringLiteral("HTTPS 会话已结束。");
                emitParsedEntry(parsedEntry);
            }

        private:
            const std::uint64_t m_sessionId;          // m_sessionId：当前会话编号。
            const qintptr m_socketDescriptor = -1;    // m_socketDescriptor：客户端套接字描述符。
            QSslSocket* m_clientSocket = nullptr;     // m_clientSocket：客户端套接字。
            QSslSocket* m_remoteSocket = nullptr;     // m_remoteSocket：远端 TLS 套接字。
            HostCertLoader m_hostCertLoader;          // m_hostCertLoader：叶子证书加载器。
            ParsedEmitter m_parsedEmitter;            // m_parsedEmitter：解析结果回调。
            StatusEmitter m_statusEmitter;            // m_statusEmitter：状态文本回调。
            QByteArray m_connectHeaderBuffer;         // m_connectHeaderBuffer：CONNECT 头缓冲。
            QByteArray m_requestHeaderBuffer;         // m_requestHeaderBuffer：请求头解析缓冲。
            QByteArray m_responseHeaderBuffer;        // m_responseHeaderBuffer：响应头解析缓冲。
            QByteArray m_pendingToRemoteBytes;        // m_pendingToRemoteBytes：等待转发到远端的字节。
            QByteArray m_pendingToClientBytes;        // m_pendingToClientBytes：等待转发到客户端的字节。
            QString m_targetHostText;                // m_targetHostText：目标主机名。
            QString m_clientProcessText;             // m_clientProcessText：客户端进程归属文本。
            std::uint16_t m_targetPort = 0;          // m_targetPort：目标端口。
            std::uint64_t m_sessionStartedTimestampMs = 0; // m_sessionStartedTimestampMs：CONNECT 完成时间。
            std::uint64_t m_requestTimestampMs = 0;  // m_requestTimestampMs：首个请求头解析时间。
            std::uint64_t m_uploadBytes = 0;         // m_uploadBytes：已转发到远端的明文字节数。
            std::uint64_t m_downloadBytes = 0;       // m_downloadBytes：已转发给客户端的明文字节数。
            bool m_connectReady = false;             // m_connectReady：CONNECT 阶段是否完成。
            bool m_requestHeaderHandled = false;     // m_requestHeaderHandled：是否已解析首个请求头。
            bool m_responseHeaderHandled = false;    // m_responseHeaderHandled：是否已解析首个响应头。
            bool m_clientEncrypted = false;          // m_clientEncrypted：客户端 TLS 是否已建立。
            bool m_remoteEncrypted = false;          // m_remoteEncrypted：远端 TLS 是否已建立。
            bool m_summaryEmitted = false;           // m_summaryEmitted：是否已派发会话汇总记录。
            bool m_closing = false;                  // m_closing：是否正在结束会话，防止重复发出错误或汇总。
        };

        // ProxySessionRegistry：停止快照持有线程对象，避免 finished/deleteLater 与后台等待竞态。
        class ProxySessionRegistry final
        {
        public:
            struct SessionWorker
            {
                std::shared_ptr<QThread> thread; // 等待快照持有强引用，最后释放时在所属线程排队删除。
            };

            bool add(const std::shared_ptr<QThread>& threadValue)
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (m_stopping)
                {
                    return false;
                }
                m_sessionWorkerList.push_back(SessionWorker{ threadValue });
                return true;
            }

            void remove(QThread* threadValue)
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_sessionWorkerList.erase(
                    std::remove_if(
                        m_sessionWorkerList.begin(),
                        m_sessionWorkerList.end(),
                        [threadValue](const SessionWorker& worker)
                        {
                            return worker.thread.get() == threadValue;
                        }),
                    m_sessionWorkerList.end());
            }

            void reapFinished(QThread* threadValue, const std::shared_ptr<ProxySessionRegistry>& owner)
            {
                std::shared_ptr<QThread> thread;
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    for (const auto& worker : m_sessionWorkerList)
                        if (worker.thread.get() == threadValue) { thread = worker.thread; break; }
                }
                if (!thread) return;
                // finished precedes native TLS teardown. Keep the registry's
                // thread owner until a background wait proves actual termination.
                try
                {
                    QThreadPool::globalInstance()->start([owner, thread]()
                    {
                        thread->wait();
                        owner->remove(thread.get());
                    });
                }
                catch (...) {} // The next retained STOP still owns and waits this entry.
            }

            bool stopAndWait()
            {
                std::vector<SessionWorker> sessionWorkerList;
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    m_stopping = true;
                    sessionWorkerList = m_sessionWorkerList;
                }

                for (const SessionWorker& worker : sessionWorkerList)
                {
                    if (worker.thread != nullptr)
                    {
                        // 两个 QThread 入口可跨线程调用，不借用会话 QObject 指针。
                        worker.thread->requestInterruption();
                        worker.thread->quit();
                    }
                }

                // One aggregate deadline covers the whole accepted session set;
                // N blocked certificate workers must not multiply the close wait.
                const QDeadlineTimer deadline(kSessionStopFirstWaitMs + kSessionStopFinalWaitMs);
                bool allStopped = true;
                for (const SessionWorker& worker : sessionWorkerList)
                {
                    if (worker.thread == nullptr || worker.thread.get() == QThread::currentThread())
                    {
                        continue;
                    }
                    allStopped = worker.thread->wait(deadline) && allStopped;
                }

                // A timeout retains the registry and exit lease for explicit Retry.
                return allStopped;
            }

        private:
            std::mutex m_mutex; // m_mutex：保护会话登记与停止快照。
            std::vector<SessionWorker> m_sessionWorkerList; // m_sessionWorkerList：所有尚未结束的会话线程。
            bool m_stopping = false; // m_stopping：停止阶段拒绝新会话登记。
        };

        class ProxyServer final : public QTcpServer
        {
        public:
            using HostCertLoader = ProxySession::HostCertLoader;
            using ParsedEmitter = ProxySession::ParsedEmitter;
            using StatusEmitter = ProxySession::StatusEmitter;
            using SessionIdProvider = std::function<std::uint64_t()>;

            ProxyServer(
                HostCertLoader hostCertLoaderValue,
                ParsedEmitter parsedEmitterValue,
                StatusEmitter statusEmitterValue,
                SessionIdProvider sessionIdProviderValue,
                std::shared_ptr<ProxySessionRegistry> sessionRegistryValue)
                : m_hostCertLoader(std::move(hostCertLoaderValue))
                , m_parsedEmitter(std::move(parsedEmitterValue))
                , m_statusEmitter(std::move(statusEmitterValue))
                , m_sessionIdProvider(std::move(sessionIdProviderValue))
                , m_sessionRegistry(std::move(sessionRegistryValue))
            {
            }

        protected:
            void incomingConnection(qintptr socketDescriptor) override
            {
                const std::uint64_t sessionId = m_sessionIdProvider ? m_sessionIdProvider() : 0;
                const auto sessionThreadOwner = std::shared_ptr<QThread>(new QThread(), [](QThread* thread)
                {
                    thread->deleteLater();
                });
                QThread* sessionThread = sessionThreadOwner.get();
                ProxySession* session = new ProxySession(
                    sessionId,
                    socketDescriptor,
                    m_hostCertLoader,
                    m_parsedEmitter,
                    m_statusEmitter);
                if (m_sessionRegistry != nullptr && !m_sessionRegistry->add(sessionThreadOwner))
                {
                    QTcpSocket rejectedSocket;
                    rejectedSocket.setSocketDescriptor(socketDescriptor);
                    rejectedSocket.abort();
                    delete session;
                    return;
                }
                session->moveToThread(sessionThread);
                connect(sessionThread, &QThread::started, session, [session]() { session->initialize(); });
                connect(session, &QObject::destroyed, sessionThread, &QThread::quit, Qt::DirectConnection);
                connect(sessionThread, &QThread::finished, session, &QObject::deleteLater);
                connect(
                    sessionThread,
                    &QThread::finished,
                    sessionThread,
                    [sessionRegistry = m_sessionRegistry, sessionThread]()
                    {
                        if (sessionRegistry != nullptr)
                        {
                            sessionRegistry->reapFinished(sessionThread, sessionRegistry);
                        }
                    },
                    Qt::DirectConnection);
                // registry 和 stop 快照共同持有线程；不能 finished 后立刻删除正在被 wait 的对象。
                sessionThread->start();
            }

        private:
            HostCertLoader m_hostCertLoader;      // m_hostCertLoader：叶子证书加载器。
            ParsedEmitter m_parsedEmitter;        // m_parsedEmitter：解析事件回调。
            StatusEmitter m_statusEmitter;        // m_statusEmitter：状态文本回调。
            SessionIdProvider m_sessionIdProvider; // m_sessionIdProvider：会话编号分配器。
            std::shared_ptr<ProxySessionRegistry> m_sessionRegistry; // m_sessionRegistry：活动会话线程登记表。
        };
    }

    HttpsMitmProxyService::HttpsMitmProxyService()
        : QObject(nullptr)
        , m_certificateStore(std::make_shared<HttpsProxyCertificateStore>())
        , m_uiDispatcher(std::make_shared<HttpsProxyUiDispatcher>(this,
            [this](const HttpsProxyParsedEntry& entry) { emitParsedEntry(entry); }))
    {
    }

    HttpsMitmProxyService::~HttpsMitmProxyService()
    {
        // QTcpServer belongs to this GUI thread; close listening here before
        // handing only independent registry/certificate values to cleanup.
        m_uiDispatcher->close();
        if (m_server)
        {
            m_server->close();
            m_server.reset();
        }
        if (m_stopState) m_stopState->listenerClosed.store(true);
        if (m_sessionCleanup && !m_sessionCleanup->isCleanupRequested()) m_sessionCleanup->requestCleanup();
        else if (m_sessionCleanup) m_sessionCleanup->notifyReadyToAttempt();
    }

    bool HttpsMitmProxyService::start(
        const QHostAddress& listenAddress,
        const std::uint16_t listenPort,
        QString* errorTextOut)
    {
        if (listenPort == 0)
        {
            if (errorTextOut != nullptr)
            {
                *errorTextOut = QStringLiteral("监听端口不能为 0。");
            }
            return false;
        }

        // 正常流程里 UI 已经先调用 ensureRootCertificateAsync 在后台备妥根证书，
        // 这里只做一次无锁快速校验；只有证书文件被外部删除等异常情况才退回同步生成。
        if (!m_certificateStore->isRootCertificateReady())
        {
            QString errorText;
            if (!ensureRootCertificate(false, &errorText))
            {
                if (errorTextOut != nullptr)
                {
                    *errorTextOut = errorText;
                }
                return false;
            }
        }

        stop();
        if (m_sessionCleanup && !m_sessionCleanup->isComplete())
        {
            if (errorTextOut) *errorTextOut = QStringLiteral("状态：正在停止HTTPS代理...");
            return false;
        }

        const QPointer<HttpsMitmProxyService> safeThis(this);

        // 会话可能在 stop 的等待上限之后仍生成证书，只借用独立共享状态。
        auto hostCertLoader = [certificateStore = m_certificateStore](const QString& hostName, QSslCertificate* certificateOut, QSslKey* privateKeyOut, QString* certErrorOut)
            {
                return certificateStore->loadHostCertificateBundle(hostName, certificateOut, privateKeyOut, certErrorOut);
            };
        auto parsedEmitter = [dispatcher = m_uiDispatcher](const HttpsProxyParsedEntry& parsedEntry)
            {
                dispatcher->postParsed(parsedEntry);
            };
        auto statusEmitter = [safeThis, dispatcher = m_uiDispatcher](const QString& statusText)
            {
                dispatcher->post([safeThis, statusText]()
                {
                    if (!safeThis.isNull())
                    {
                        safeThis->emitStatus(statusText);
                    }
                });
            };
        auto sessionIdProvider = [safeThis]() -> std::uint64_t
            {
                if (safeThis.isNull())
                {
                    return 0;
                }
                return safeThis->m_nextSessionId++;
            };
        const std::shared_ptr<ProxySessionRegistry> sessionRegistry = std::make_shared<ProxySessionRegistry>();

        const auto state = std::make_shared<HttpsProxyStopState>();
        const auto dispatcher = m_uiDispatcher;
        const auto closeListener = [this, state]()
        {
            if (m_stopState != state) return;
            stopAsync({});
        };
        const auto cleanup = ks::ui::RetainedCleanupJob::create(
            [sessionRegistry, state, dispatcher]()
            {
                if (!sessionRegistry->stopAndWait()) return false;
                std::function<void()> completion;
                std::function<void()> nativeCompletion;
                {
                    std::lock_guard<std::mutex> lock(state->mutex);
                    completion = std::move(state->completion);
                    nativeCompletion = std::move(state->nativeCompletion);
                    state->nativeFinished = true;
                }
                if (nativeCompletion) nativeCompletion();
                if (completion) dispatcher->post(std::move(completion));
                return true;
            }, 0, [state, dispatcher, closeListener]()
            {
                state->stopRequested.store(true);
                dispatcher->post(closeListener);
            },
            [state]() { return state->listenerClosed.load(); });
        if (!cleanup) return false;
        m_stopState = state;
        m_sessionCleanup = cleanup;
        state->listenerClosed.store(false);
        if (state->stopRequested.load())
        {
            state->listenerClosed.store(true);
            cleanup->notifyReadyToAttempt();
            return false;
        }
        m_server = std::make_unique<ProxyServer>(
            std::move(hostCertLoader), std::move(parsedEmitter),
            std::move(statusEmitter), std::move(sessionIdProvider), sessionRegistry);

        if (!m_server->listen(listenAddress, listenPort))
        {
            if (errorTextOut != nullptr)
            {
                *errorTextOut = QStringLiteral("HTTPS代理监听失败：%1").arg(m_server->errorString());
            }
            m_server.reset();
            state->listenerClosed.store(true);
            cleanup->requestCleanup();
            return false;
        }

        m_listenAddress = listenAddress;
        m_listenPort = listenPort;
        emitStatus(QStringLiteral("HTTPS代理已启动：%1:%2").arg(m_listenAddress.toString()).arg(m_listenPort));
        return true;
    }

    void HttpsMitmProxyService::stop()
    {
        stopAsync({});
    }

    void HttpsMitmProxyService::stopAsync(std::function<void()> completionCallback,
        std::function<void()> nativeCleanupCompleted)
    {
        const bool hadActiveProxy = m_server != nullptr;
        if (m_server) { m_server->close(); m_server.reset(); }
        m_listenAddress = QHostAddress();
        m_listenPort = 0;
        if (!m_sessionCleanup || m_sessionCleanup->isComplete())
        {
            if (hadActiveProxy) emitStatus(QStringLiteral("HTTPS代理已停止。"));
            if (completionCallback) completionCallback();
            if (nativeCleanupCompleted) nativeCleanupCompleted();
            return;
        }
        const auto state = m_stopState;
        const QPointer<HttpsMitmProxyService> guardedSelf(this);
        const auto completed = [guardedSelf, completionCallback, hadActiveProxy]()
        {
            if (guardedSelf && hadActiveProxy)
                guardedSelf->emitStatus(QStringLiteral("HTTPS代理已停止。"));
            if (completionCallback) completionCallback();
        };
        bool nativeFinished;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            nativeFinished = state->nativeFinished;
            if (!nativeFinished)
            {
                if (nativeCleanupCompleted) state->nativeCompletion = std::move(nativeCleanupCompleted);
                if (completionCallback || hadActiveProxy) state->completion = completed;
            }
        }
        if (nativeFinished)
        {
            if (nativeCleanupCompleted) nativeCleanupCompleted();
            completed();
        }
        state->listenerClosed.store(true);
        if (!m_sessionCleanup->isCleanupRequested()) m_sessionCleanup->requestCleanup();
        else m_sessionCleanup->notifyReadyToAttempt();
    }

    bool HttpsMitmProxyService::isRunning() const
    {
        return m_server != nullptr && m_server->isListening();
    }

    void HttpsMitmProxyService::setParsedCallback(ParsedCallback callbackValue)
    {
        m_parsedCallback = std::move(callbackValue);
    }

    void HttpsMitmProxyService::setStatusCallback(StatusCallback callbackValue)
    {
        m_statusCallback = std::move(callbackValue);
    }

    bool HttpsMitmProxyService::ensureRootCertificate(const bool installToTrustStore, QString* errorTextOut)
    {
        return m_certificateStore->ensureRootCertificate(installToTrustStore, errorTextOut);
    }

    bool HttpsMitmProxyService::isRootTrusted() const
    {
        return m_certificateStore->isRootTrusted();
    }

    bool HttpsMitmProxyService::loadHostCertificateBundle(
        const QString& hostName, QSslCertificate* certificateOut,
        QSslKey* privateKeyOut, QString* errorTextOut)
    {
        return m_certificateStore->loadHostCertificateBundle(hostName, certificateOut, privateKeyOut, errorTextOut);
    }

    bool HttpsProxyCertificateStore::ensureRootCertificate(const bool installToTrustStore, QString* errorTextOut)
    {
        std::lock_guard<std::recursive_mutex> guard(m_certificateMutex);

        if (isRootCertificateReady() && (!installToTrustStore || isRootTrusted()))
        {
            return true;
        }

        const QString scriptText = buildRootCertificateScriptText(
            rootCertificatePfxPath(),
            rootCertificateCerPath(),
            installToTrustStore);

        QString standardOutputText;
        QString standardErrorText;
        QString errorText;
        if (!runPowerShellScript(scriptText, &standardOutputText, &standardErrorText, &errorText))
        {
            if (errorTextOut != nullptr)
            {
                *errorTextOut = errorText;
            }
            return false;
        }

        m_rootCertificatePrepared = true;
        return true;
    }

    void HttpsMitmProxyService::ensureRootCertificateAsync(
        const bool installToTrustStore,
        std::function<void(bool, QString)> completionCallback)
    {
        const auto certificateStore = m_certificateStore;
        const auto dispatcher = m_uiDispatcher;

        // 快速路径：证书文件齐备、信任状态也满足要求时不必拉起 powershell.exe。
        // 这里刻意不去抢证书状态锁，避免会话线程正在生成叶子证书时把 UI 线程一起拖住。
        if (certificateStore->isRootCertificateReady() && (!installToTrustStore || certificateStore->isRootTrusted()))
        {
            if (completionCallback)
            {
                // 统一成“回调总是稍后在 UI 线程触发”，调用方不必区分快慢两条路径。
                dispatcher->post([completionCallback]() { completionCallback(true, QString()); });
            }
            return;
        }

        const auto cleanupLease = ks::ui::AsyncCleanupRegistry::global().acquire();
        if (!cleanupLease) return;
        const QString scriptText = buildRootCertificateScriptText(
            certificateStore->rootCertificatePfxPath(),
            certificateStore->rootCertificateCerPath(),
            installToTrustStore);
        QThreadPool::globalInstance()->start(
            [certificateStore, dispatcher, scriptText, completionCallback, cleanupLease]()
            {
                // 后台线程只处理值类型入参与 QProcess，不触碰本服务对象的任何成员。
                QString standardOutputText;
                QString standardErrorText;
                QString errorText;
                const bool executed = executePowerShellScript(
                    scriptText,
                    &standardOutputText,
                    &standardErrorText,
                    &errorText);

                if (executed)
                {
                    certificateStore->markRootCertificatePrepared();
                }

                dispatcher->post(
                    [executed, errorText, completionCallback]()
                    {
                        if (completionCallback)
                        {
                            completionCallback(executed, errorText);
                        }
                    });
            });
    }

    bool HttpsProxyCertificateStore::isRootTrusted() const
    {
        // 只需要一个布尔值，没必要为此拉起 powershell.exe：
        // 直接读导出的根证书 DER，再按 SHA-1 指纹查当前用户 ROOT 存储。
        QFile rootCertificateFile(rootCertificateCerPath());
        if (!rootCertificateFile.open(QIODevice::ReadOnly))
        {
            return false;
        }
        const QByteArray rootCertificateBytes = rootCertificateFile.readAll();
        rootCertificateFile.close();
        if (rootCertificateBytes.isEmpty())
        {
            return false;
        }

        QSslCertificate rootCertificate(rootCertificateBytes, QSsl::Der);
        if (rootCertificate.isNull())
        {
            // 兼容历史上可能写成 PEM 的根证书文件。
            rootCertificate = QSslCertificate(rootCertificateBytes, QSsl::Pem);
        }
        if (rootCertificate.isNull())
        {
            return false;
        }

        return isThumbprintInCurrentUserRootStore(rootCertificate.digest(QCryptographicHash::Sha1));
    }

    bool HttpsProxyCertificateStore::isRootCertificateReady() const
    {
        return m_rootCertificatePrepared.load()
            && QFile::exists(rootCertificatePfxPath())
            && QFile::exists(rootCertificateCerPath());
    }

    bool HttpsProxyCertificateStore::loadHostCertificateBundle(
        const QString& hostName,
        QSslCertificate* certificateOut,
        QSslKey* privateKeyOut,
        QString* errorTextOut)
    {
        std::lock_guard<std::recursive_mutex> guard(m_certificateMutex);

        if (certificateOut == nullptr || privateKeyOut == nullptr)
        {
            if (errorTextOut != nullptr)
            {
                *errorTextOut = QStringLiteral("输出证书对象为空。");
            }
            return false;
        }

        QString errorText;
        if (!ensureRootCertificate(false, &errorText) || !ensureHostCertificateFile(hostName, &errorText))
        {
            if (errorTextOut != nullptr)
            {
                *errorTextOut = errorText;
            }
            return false;
        }

        auto tryLoadPemBundle =
            [this](const QString& targetHostName, QSslCertificate* certificateTargetOut, QSslKey* privateKeyTargetOut, QString* loadErrorTextOut) -> bool
            {
                QFile hostCertificatePemFile(hostCertificatePemPath(targetHostName));
                if (!hostCertificatePemFile.open(QIODevice::ReadOnly))
                {
                    if (loadErrorTextOut != nullptr)
                    {
                        *loadErrorTextOut = QStringLiteral("读取主机证书 PEM 失败：%1").arg(hostCertificatePemFile.errorString());
                    }
                    return false;
                }

                QFile hostPrivateKeyPemFile(hostPrivateKeyPemPath(targetHostName));
                if (!hostPrivateKeyPemFile.open(QIODevice::ReadOnly))
                {
                    if (loadErrorTextOut != nullptr)
                    {
                        *loadErrorTextOut = QStringLiteral("读取主机私钥 PEM 失败：%1").arg(hostPrivateKeyPemFile.errorString());
                    }
                    return false;
                }

                const QByteArray certificatePemBytes = hostCertificatePemFile.readAll();
                const QByteArray privateKeyPemBytes = hostPrivateKeyPemFile.readAll();
                QSslCertificate localCertificate(certificatePemBytes, QSsl::Pem);
                QSslKey localPrivateKey(privateKeyPemBytes, QSsl::Rsa, QSsl::Pem, QSsl::PrivateKey);
                if (localPrivateKey.isNull())
                {
                    // 某些 Qt/OpenSSL 组合对 PKCS#8 + 算法显式指定较挑剔，回退到 Opaque 再试一次。
                    localPrivateKey = QSslKey(privateKeyPemBytes, QSsl::Opaque, QSsl::Pem, QSsl::PrivateKey);
                }
                if (localCertificate.isNull() || localPrivateKey.isNull())
                {
                    if (loadErrorTextOut != nullptr)
                    {
                        *loadErrorTextOut = QStringLiteral("加载主机 PEM 证书或私钥失败。certNull=%1 keyNull=%2 certPath=%3 keyPath=%4")
                            .arg(localCertificate.isNull() ? QStringLiteral("true") : QStringLiteral("false"))
                            .arg(localPrivateKey.isNull() ? QStringLiteral("true") : QStringLiteral("false"))
                            .arg(hostCertificatePemPath(targetHostName))
                            .arg(hostPrivateKeyPemPath(targetHostName));
                    }
                    return false;
                }

                *certificateTargetOut = localCertificate;
                *privateKeyTargetOut = localPrivateKey;
                return true;
            };

        if (tryLoadPemBundle(hostName, certificateOut, privateKeyOut, errorTextOut))
        {
            return true;
        }

        // 兜底策略：若遇到旧格式/坏缓存，删除该主机缓存后重建一次。
        QFile::remove(hostCertificatePfxPath(hostName));
        QFile::remove(hostCertificatePemPath(hostName));
        QFile::remove(hostPrivateKeyPemPath(hostName));

        QString rebuildErrorText;
        if (!ensureHostCertificateFile(hostName, &rebuildErrorText))
        {
            if (errorTextOut != nullptr)
            {
                *errorTextOut = rebuildErrorText;
            }
            return false;
        }

        if (tryLoadPemBundle(hostName, certificateOut, privateKeyOut, errorTextOut))
        {
            return true;
        }

        if (errorTextOut != nullptr && !errorTextOut->contains(QStringLiteral("重建")))
        {
            *errorTextOut = QStringLiteral("%1；已执行一次缓存删除重建但仍失败。").arg(*errorTextOut);
        }
        return false;
    }

    QHostAddress HttpsMitmProxyService::currentListenAddress() const
    {
        return m_listenAddress;
    }

    std::uint16_t HttpsMitmProxyService::currentListenPort() const
    {
        return m_listenPort;
    }

    QString HttpsMitmProxyService::rootCertificatePath() const
    {
        return m_certificateStore->rootCertificateCerPath();
    }

    void HttpsMitmProxyService::emitParsedEntry(const HttpsProxyParsedEntry& parsedEntry) const
    {
        if (m_parsedCallback)
        {
            m_parsedCallback(parsedEntry);
        }
    }

    void HttpsMitmProxyService::emitStatus(const QString& statusText) const
    {
        if (m_statusCallback)
        {
            m_statusCallback(statusText);
        }
    }

    HttpsProxyCertificateStore::HttpsProxyCertificateStore()
    {
        QString baseDirectoryText = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        if (baseDirectoryText.isEmpty())
        {
            baseDirectoryText = QDir::currentPath();
        }

        QDir workspaceDirectory(baseDirectoryText);
        workspaceDirectory.mkpath(QStringLiteral("HttpsProxy"));
        m_workspaceDirectory = workspaceDirectory.filePath(QStringLiteral("HttpsProxy"));
    }

    QString HttpsProxyCertificateStore::certificateWorkspaceDir() const
    {
        return m_workspaceDirectory;
    }

    bool HttpsProxyCertificateStore::runPowerShellScript(
        const QString& scriptText,
        QString* standardOutputOut,
        QString* standardErrorOut,
        QString* errorTextOut) const
    {
        // 实际执行体放在匿名 namespace 的自由函数里：后台证书任务可以直接复用它，
        // 从而不必为了跑一段脚本而持有本服务对象的指针。
        return executePowerShellScript(scriptText, standardOutputOut, standardErrorOut, errorTextOut);
    }

    QString HttpsProxyCertificateStore::hostCertificatePfxPath(const QString& hostName) const
    {
        return QDir(certificateWorkspaceDir()).filePath(
            QStringLiteral("leaf_%1.pfx").arg(normalizedHostForFileName(hostName)));
    }

    QString HttpsProxyCertificateStore::hostCertificatePemPath(const QString& hostName) const
    {
        return QDir(certificateWorkspaceDir()).filePath(
            QStringLiteral("leaf_%1_cert.pem").arg(normalizedHostForFileName(hostName)));
    }

    QString HttpsProxyCertificateStore::hostPrivateKeyPemPath(const QString& hostName) const
    {
        return QDir(certificateWorkspaceDir()).filePath(
            QStringLiteral("leaf_%1_key.pem").arg(normalizedHostForFileName(hostName)));
    }

    QString HttpsProxyCertificateStore::rootCertificatePfxPath() const
    {
        return QDir(certificateWorkspaceDir()).filePath(QStringLiteral("root_ca.pfx"));
    }

    QString HttpsProxyCertificateStore::rootCertificateCerPath() const
    {
        return QDir(certificateWorkspaceDir()).filePath(QStringLiteral("root_ca.cer"));
    }

    bool HttpsProxyCertificateStore::ensureHostCertificateFile(const QString& hostName, QString* errorTextOut)
    {
        if (hostName.trimmed().isEmpty())
        {
            if (errorTextOut != nullptr)
            {
                *errorTextOut = QStringLiteral("目标主机名为空。");
            }
            return false;
        }

        const QString rootPfxPath = rootCertificatePfxPath();
        const QString hostPfxPath = hostCertificatePfxPath(hostName);
        const QString hostCertPemPath = hostCertificatePemPath(hostName);
        const QString hostKeyPemPath = hostPrivateKeyPemPath(hostName);
        const QString scriptText = QStringLiteral(
            "$ErrorActionPreference='Stop'; "
            "$ProgressPreference='SilentlyContinue'; "
            "$rootPfx=%1; "
            "$hostPfx=%2; "
            "$hostPem=%3; "
            "$hostKeyPem=%4; "
            "$hostName=%5; "
            "$plainPwd=%6; "
            "$pwd=ConvertTo-SecureString $plainPwd -AsPlainText -Force; "
            "$nl=[System.Environment]::NewLine; "
            "$rootPfxData=Get-PfxData -FilePath $rootPfx -Password $pwd; "
            "$rootThumb=$rootPfxData.EndEntityCertificates[0].Thumbprint; "
            "$rootCert=Get-ChildItem Cert:\\CurrentUser\\My | Where-Object { $_.Thumbprint -eq $rootThumb } | Select-Object -First 1; "
            "if($null -eq $rootCert){ "
            "  Import-PfxCertificate -FilePath $rootPfx -CertStoreLocation Cert:\\CurrentUser\\My -Password $pwd | Out-Null; "
            "  $rootCert=Get-ChildItem Cert:\\CurrentUser\\My | Where-Object { $_.Thumbprint -eq $rootThumb } | Select-Object -First 1; "
            "} "
            "$leafCert=$null; "
            "if(Test-Path $hostPfx){ "
            "  $leafPfxData=Get-PfxData -FilePath $hostPfx -Password $pwd; "
            "  $leafThumb=$leafPfxData.EndEntityCertificates[0].Thumbprint; "
            "  $leafCert=Get-ChildItem Cert:\\CurrentUser\\My | Where-Object { $_.Thumbprint -eq $leafThumb } | Select-Object -First 1; "
            "  if($null -eq $leafCert){ "
            "    Import-PfxCertificate -FilePath $hostPfx -CertStoreLocation Cert:\\CurrentUser\\My -Password $pwd | Out-Null; "
            "    $leafCert=Get-ChildItem Cert:\\CurrentUser\\My | Where-Object { $_.Thumbprint -eq $leafThumb } | Select-Object -First 1; "
            "  } "
            "} "
            "if($null -ne $leafCert -and -not $leafCert.HasPrivateKey){ "
            "  Remove-Item -Path ('Cert:\\CurrentUser\\My\\' + $leafCert.Thumbprint) -Force -ErrorAction SilentlyContinue; "
            "  $leafCert=$null; "
            "} "
            "if($null -eq $leafCert){ "
            "  $leafCert=New-SelfSignedCertificate -Type Custom -Subject ('CN=' + $hostName) -DnsName $hostName "
            "    -FriendlyName ('Ksword HTTPS Leaf ' + $hostName) -Signer $rootCert -CertStoreLocation 'Cert:\\CurrentUser\\My' "
            "    -KeyExportPolicy Exportable -KeyAlgorithm RSA -KeyLength 2048 -HashAlgorithm sha256 "
            "    -KeyUsage DigitalSignature,KeyEncipherment "
            "    -TextExtension @('2.5.29.19={text}ca=false','2.5.29.37={text}1.3.6.1.5.5.7.3.1') "
            "    -NotAfter (Get-Date).AddYears(2); "
            "} "
            "Export-PfxCertificate -Cert $leafCert -FilePath $hostPfx -Password $pwd -Force | Out-Null; "
            "$leafCertPem = '-----BEGIN CERTIFICATE-----' + $nl + [Convert]::ToBase64String($leafCert.Export([System.Security.Cryptography.X509Certificates.X509ContentType]::Cert), 'InsertLineBreaks') + $nl + '-----END CERTIFICATE-----' + $nl; "
            "$leafRsa = [System.Security.Cryptography.X509Certificates.RSACertificateExtensions]::GetRSAPrivateKey($leafCert); "
            "if($null -eq $leafRsa){ throw 'Leaf private key export failed'; } "
            "$leafKeyPem = $null; "
            "if($leafRsa | Get-Member -Name ExportPkcs8PrivateKeyPem -MemberType Method -ErrorAction SilentlyContinue){ "
            "  $leafKeyPem = $leafRsa.ExportPkcs8PrivateKeyPem(); "
            "} elseif(($leafRsa.PSObject.Properties.Name -contains 'Key') -and $null -ne $leafRsa.Key){ "
            "  $pkcs8Bytes = $leafRsa.Key.Export([System.Security.Cryptography.CngKeyBlobFormat]::Pkcs8PrivateBlob); "
            "  $leafKeyPem = '-----BEGIN PRIVATE KEY-----' + $nl + [Convert]::ToBase64String($pkcs8Bytes, 'InsertLineBreaks') + $nl + '-----END PRIVATE KEY-----' + $nl; "
            "} else { "
            "  throw 'Leaf private key export API unavailable'; "
            "} "
            "[System.IO.File]::WriteAllText($hostPem, $leafCertPem, [System.Text.UTF8Encoding]::new($false)); "
            "[System.IO.File]::WriteAllText($hostKeyPem, $leafKeyPem, [System.Text.UTF8Encoding]::new($false)); "
            "Write-Output $hostPfx;")
            .arg(quoteForPowerShell(rootPfxPath))
            .arg(quoteForPowerShell(hostPfxPath))
            .arg(quoteForPowerShell(hostCertPemPath))
            .arg(quoteForPowerShell(hostKeyPemPath))
            .arg(quoteForPowerShell(hostName))
            .arg(quoteForPowerShell(QString::fromLatin1(kPfxPasswordText)));

        QString standardOutputText;
        QString standardErrorText;
        QString errorText;
        if (!runPowerShellScript(scriptText, &standardOutputText, &standardErrorText, &errorText))
        {
            if (errorTextOut != nullptr)
            {
                *errorTextOut = errorText;
            }
            return false;
        }
        return QFile::exists(hostPfxPath)
            && QFile::exists(hostCertPemPath)
            && QFile::exists(hostKeyPemPath);
    }

    QString HttpsProxyCertificateStore::normalizedHostForFileName(const QString& hostName) const
    {
        const QByteArray hashBytes = QCryptographicHash::hash(
            hostName.trimmed().toLower().toUtf8(),
            QCryptographicHash::Sha1);
        return QString::fromLatin1(hashBytes.toHex());
    }

    QByteArray HttpsProxyCertificateStore::rootPfxPassword() const
    {
        return QByteArrayLiteral(kPfxPasswordText);
    }
}
