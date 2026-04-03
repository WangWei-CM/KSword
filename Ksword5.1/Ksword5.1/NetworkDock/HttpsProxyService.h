#pragma once

// ============================================================
// HttpsProxyService.h
// 作用：
// 1) 提供本地 HTTPS 解析代理服务；
// 2) 负责根证书生成、信任导入与站点证书缓存；
// 3) 通过回调把解析结果和状态文本返回给 NetworkDock。
// ============================================================

#include <QtCore/QByteArray>
#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtNetwork/QHostAddress>

#include <cstdint>    // std::uint*_t：会话编号、时间戳与端口。
#include <functional> // std::function：UI 回调桥接。
#include <memory>     // std::unique_ptr：内部对象生命周期托管。
#include <mutex>      // std::recursive_mutex：证书生成与导出串行化。

class QSslCertificate;
class QSslKey;
class QTcpServer;

namespace ks::network
{
    // HttpsProxyParsedEntry 作用：
    // - 描述一条 HTTPS 解析结果；
    // - UI 可直接据此生成一行表格数据。
    struct HttpsProxyParsedEntry
    {
        std::uint64_t timestampMs = 0;  // timestampMs：事件时间戳（Unix ms）。
        std::uint64_t sessionId = 0;    // sessionId：代理会话编号。
        QString clientEndpointText;     // clientEndpointText：客户端端点文本。
        QString targetHostText;         // targetHostText：目标主机名。
        int targetPort = 0;             // targetPort：目标端口号。
        QString eventTypeText;          // eventTypeText：事件类型（CONNECT/REQUEST/RESPONSE/ERROR）。
        QString methodText;             // methodText：HTTP 请求方法。
        QString pathText;               // pathText：HTTP 请求路径。
        int statusCode = 0;             // statusCode：HTTP 响应状态码。
        QString tlsVersionText;         // tlsVersionText：TLS 版本文本。
        QString alpnText;               // alpnText：ALPN 协商结果。
        QString sniText;                // sniText：SNI 或 CONNECT 主机名。
        QString detailText;             // detailText：补充说明或错误详情。
        QByteArray rawBytes;            // rawBytes：本次事件对应的原始明文字节。
    };

    class HttpsMitmProxyService final : public QObject
    {
    public:
        // ParsedCallback 作用：
        // - 把单条 HTTPS 解析结果推送给 UI。
        using ParsedCallback = std::function<void(const HttpsProxyParsedEntry&)>;

        // StatusCallback 作用：
        // - 把状态文本和错误文本推送给 UI。
        using StatusCallback = std::function<void(const QString&)>;

    public:
        // 构造函数作用：
        // - 初始化证书目录、默认状态和内部对象指针。
        explicit HttpsMitmProxyService();

        // 析构函数作用：
        // - 停止监听并清理会话。
        ~HttpsMitmProxyService() override;

        // start 作用：
        // - 启动本地 HTTPS 代理监听。
        // 参数 listenAddress：监听地址。
        // 参数 listenPort：监听端口。
        // 参数 errorTextOut：失败时输出错误文本。
        // 返回：true=成功；false=失败。
        bool start(const QHostAddress& listenAddress, std::uint16_t listenPort, QString* errorTextOut);

        // stop 作用：
        // - 停止本地 HTTPS 代理并断开会话。
        void stop();

        // isRunning 作用：
        // - 返回代理是否处于监听状态。
        [[nodiscard]] bool isRunning() const;

        // setParsedCallback 作用：
        // - 设置解析结果回调。
        void setParsedCallback(ParsedCallback callbackValue);

        // setStatusCallback 作用：
        // - 设置状态文本回调。
        void setStatusCallback(StatusCallback callbackValue);

        // ensureRootCertificate 作用：
        // - 确保根证书存在；
        // - installToTrustStore=true 时自动导入当前用户信任根。
        // 参数 installToTrustStore：是否导入当前用户信任根。
        // 参数 errorTextOut：失败时输出错误文本。
        // 返回：true=成功；false=失败。
        bool ensureRootCertificate(bool installToTrustStore, QString* errorTextOut);

        // isRootTrusted 作用：
        // - 检查根证书是否已在当前用户信任根中。
        [[nodiscard]] bool isRootTrusted() const;

        // loadHostCertificateBundle 作用：
        // - 为指定主机加载或生成叶子证书与私钥。
        // 参数 hostName：目标主机名。
        // 参数 certificateOut：输出叶子证书。
        // 参数 privateKeyOut：输出叶子私钥。
        // 参数 errorTextOut：失败时输出错误文本。
        // 返回：true=成功；false=失败。
        bool loadHostCertificateBundle(
            const QString& hostName,
            QSslCertificate* certificateOut,
            QSslKey* privateKeyOut,
            QString* errorTextOut);

        // currentListenAddress 作用：
        // - 返回当前监听地址。
        [[nodiscard]] QHostAddress currentListenAddress() const;

        // currentListenPort 作用：
        // - 返回当前监听端口。
        [[nodiscard]] std::uint16_t currentListenPort() const;

        // rootCertificatePath 作用：
        // - 返回根证书 `.cer` 文件路径，供 UI 导入和展示。
        [[nodiscard]] QString rootCertificatePath() const;

    private:
        // emitParsedEntry 作用：
        // - 在服务内部统一派发解析结果回调。
        void emitParsedEntry(const HttpsProxyParsedEntry& parsedEntry) const;

        // emitStatus 作用：
        // - 在服务内部统一派发状态文本回调。
        void emitStatus(const QString& statusText) const;

        // certificateWorkspaceDir 作用：
        // - 返回证书工作目录并在缺失时创建。
        QString certificateWorkspaceDir() const;

        // runPowerShellScript 作用：
        // - 同步执行一段 PowerShell 脚本。
        // 参数 scriptText：脚本文本。
        // 参数 standardOutputOut：标准输出。
        // 参数 standardErrorOut：标准错误。
        // 参数 errorTextOut：失败时输出错误文本。
        // 返回：true=执行成功；false=执行失败。
        bool runPowerShellScript(
            const QString& scriptText,
            QString* standardOutputOut,
            QString* standardErrorOut,
            QString* errorTextOut) const;

        // hostCertificatePfxPath 作用：
        // - 计算指定主机的证书 PFX 路径。
        QString hostCertificatePfxPath(const QString& hostName) const;

        // hostCertificatePemPath 作用：
        // - 计算指定主机的证书 PEM 路径。
        QString hostCertificatePemPath(const QString& hostName) const;

        // hostPrivateKeyPemPath 作用：
        // - 计算指定主机的私钥 PEM 路径。
        QString hostPrivateKeyPemPath(const QString& hostName) const;

        // rootCertificatePfxPath 作用：
        // - 返回根证书 PFX 路径。
        QString rootCertificatePfxPath() const;

        // rootCertificateCerPath 作用：
        // - 返回根证书 CER 路径。
        QString rootCertificateCerPath() const;

        // ensureHostCertificateFile 作用：
        // - 确保指定主机的叶子证书 PFX 文件存在。
        // 参数 hostName：目标主机名。
        // 参数 errorTextOut：失败时输出错误文本。
        // 返回：true=成功；false=失败。
        bool ensureHostCertificateFile(const QString& hostName, QString* errorTextOut);

        // normalizedHostForFileName 作用：
        // - 把主机名转换为文件名安全片段。
        QString normalizedHostForFileName(const QString& hostName) const;

        // rootPfxPassword 作用：
        // - 返回固定 PFX 导出密码。
        QByteArray rootPfxPassword() const;

    private:
        std::unique_ptr<QTcpServer> m_server;  // m_server：本地代理监听服务器。
        ParsedCallback m_parsedCallback;       // m_parsedCallback：解析结果回调。
        StatusCallback m_statusCallback;       // m_statusCallback：状态文本回调。
        QHostAddress m_listenAddress;          // m_listenAddress：当前监听地址。
        std::uint16_t m_listenPort = 0;        // m_listenPort：当前监听端口。
        std::uint64_t m_nextSessionId = 1;     // m_nextSessionId：下一个分配的会话编号。
        mutable std::recursive_mutex m_certificateMutex; // m_certificateMutex：证书读写串行化锁。
        bool m_rootCertificatePrepared = false; // m_rootCertificatePrepared：根证书文件是否已准备完成。
    };
}
