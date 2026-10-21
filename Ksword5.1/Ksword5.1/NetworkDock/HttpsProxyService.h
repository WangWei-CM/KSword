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
#include <memory>     // std::unique_ptr/shared_ptr：服务对象与证书状态生命周期托管。

class QSslCertificate;
class QSslKey;
class QTcpServer;
namespace ks::ui { class RetainedCleanupJob; }

namespace ks::network
{
    class HttpsProxyCertificateStore;
    class HttpsProxyUiDispatcher;
    struct HttpsProxyStopState;

    // HttpsProxyParsedEntry 作用：
    // - 描述一条 HTTPS 解析结果；
    // - UI 可直接据此生成一行表格数据。
    struct HttpsProxyParsedEntry
    {
        std::uint64_t timestampMs = 0;  // timestampMs：事件时间戳（Unix ms）。
        std::uint64_t sessionId = 0;    // sessionId：代理会话编号。
        QString clientEndpointText;     // clientEndpointText：客户端端点文本。
        QString clientProcessText;      // clientProcessText：匹配到的客户端进程名与 PID。
        QString targetHostText;         // targetHostText：目标主机名。
        int targetPort = 0;             // targetPort：目标端口号。
        QString eventTypeText;          // eventTypeText：事件类型（CONNECT/REQUEST/RESPONSE/ERROR）。
        QString methodText;             // methodText：HTTP 请求方法。
        QString pathText;               // pathText：HTTP 请求路径。
        int statusCode = 0;             // statusCode：HTTP 响应状态码。
        QString tlsVersionText;         // tlsVersionText：TLS 版本文本。
        QString alpnText;               // alpnText：ALPN 协商结果。
        QString sniText;                // sniText：SNI 或 CONNECT 主机名。
        QString contentTypeText;        // contentTypeText：HTTP Content-Type。
        qint64 contentLength = -1;      // contentLength：HTTP Content-Length，未知时为 -1。
        std::uint64_t elapsedMs = 0;    // elapsedMs：从请求发出到首个响应头的耗时。
        std::uint64_t uploadBytes = 0;  // uploadBytes：本会话已转发到远端的明文字节数。
        std::uint64_t downloadBytes = 0;// downloadBytes：本会话已转发给客户端的明文字节数。
        QString cipherSuiteText;        // cipherSuiteText：远端 TLS 协商密码套件。
        QString certificateSubjectText; // certificateSubjectText：远端证书主体。
        QString certificateIssuerText;  // certificateIssuerText：远端证书签发者。
        QString certificateExpiryText;  // certificateExpiryText：远端证书到期时间。
        QString certificateSha256Text;  // certificateSha256Text：远端证书 SHA-256 指纹。
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
        // - 停止本地 HTTPS 代理并断开会话；
        // - 调用线程只停止监听并请求会话退出；后台保留资源直到实际退出。
        void stop();

        // stopAsync 作用：
        // - 在调用线程（UI 线程）关闭监听套接字，把等待会话线程退出的耗时部分放到后台线程；
        // - 供 UI 的“停止代理”按钮使用，避免界面在会话线程收尾期间冻结。
        // 参数 completionCallback：收尾完成后的回调，固定在 UI 线程执行；服务析构会取消尚未执行的回调。
        void stopAsync(std::function<void()> completionCallback,
            std::function<void()> nativeCleanupCompleted = {});

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

        // ensureRootCertificateAsync 作用：
        // - 与 ensureRootCertificate 语义一致，但把 powershell.exe 调用放到后台线程，
        //   调用线程（UI 线程）不会被证书生成/导出阻塞；
        // - 证书文件已齐备且信任状态满足要求时不会拉起子进程，只投递一次完成回调。
        // 参数 installToTrustStore：是否导入当前用户信任根。
        // 参数 completionCallback：完成回调，固定在 UI 线程执行，入参为 (是否成功, 失败原因文本)；
        //                          服务析构会取消尚未执行的回调。
        void ensureRootCertificateAsync(bool installToTrustStore, std::function<void(bool, QString)> completionCallback);

        // isRootTrusted 作用：
        // - 检查根证书是否已在当前用户信任根中；
        // - 直接读取当前用户 ROOT 证书存储，不会拉起子进程，可在 UI 线程调用。
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

    private:
        std::unique_ptr<QTcpServer> m_server;  // m_server：本地代理监听服务器。
        std::shared_ptr<ks::ui::RetainedCleanupJob> m_sessionCleanup;
        std::shared_ptr<HttpsProxyStopState> m_stopState;
        ParsedCallback m_parsedCallback;       // m_parsedCallback：解析结果回调。
        StatusCallback m_statusCallback;       // m_statusCallback：状态文本回调。
        QHostAddress m_listenAddress;          // m_listenAddress：当前监听地址。
        std::uint16_t m_listenPort = 0;        // m_listenPort：当前监听端口。
        std::uint64_t m_nextSessionId = 1;     // m_nextSessionId：下一个分配的会话编号。
        std::shared_ptr<HttpsProxyCertificateStore> m_certificateStore; // 超时会话继续持有证书状态，服务析构不会释放它正在使用的锁。
        std::shared_ptr<HttpsProxyUiDispatcher> m_uiDispatcher; // 以服务生命周期串行化投递与关闭，晚会话不借用 QApplication。
    };
}
