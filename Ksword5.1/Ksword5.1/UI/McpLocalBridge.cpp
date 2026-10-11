#include "../Framework.h"
#include "./McpLocalBridge.h"
#include "../../../shared/usermode/KswordMcpProtocol.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QLocalServer>
#include <QLocalSocket>
#include <QTimer>
#include <QWidget>
#include <Windows.h>
#include <vector>

namespace ks::ui
{
    namespace
    {
        // 读取 TOKEN_USER 到调用方拥有的缓冲；所有分支都释放令牌句柄。
        bool readUser(HANDLE process, std::vector<unsigned char>& buffer)
        {
            HANDLE token = nullptr; // 本次查询持有的进程令牌句柄。
            if (!::OpenProcessToken(process, TOKEN_QUERY, &token))
            {
                return false;
            }
            DWORD bytes = 0; // TOKEN_USER 与 SID 所需的精确字节数。
            ::GetTokenInformation(token, TokenUser, nullptr, 0, &bytes);
            if (bytes == 0 || bytes > 64 * 1024)
            {
                ::CloseHandle(token);
                return false;
            }
            // 缓冲仅在函数成功后解释，避免失败的查询留下部分身份数据。
            buffer.resize(bytes);
            const BOOL queried = ::GetTokenInformation(token, TokenUser,
                buffer.data(), bytes, &bytes);
            ::CloseHandle(token);
            return queried && ::IsValidSid(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid);
        }

        // 读取进程完整性级别；本地桥不把普通客户端自动提升到管理员 GUI 的权限。
        bool readIntegrity(HANDLE process, DWORD* level)
        {
            HANDLE token = nullptr; // 只借 TOKEN_QUERY 查询完整性标签。
            if (!::OpenProcessToken(process, TOKEN_QUERY, &token))
            {
                return false;
            }
            DWORD bytes = 0; // 标签及 SID 的精确容量。
            ::GetTokenInformation(token, TokenIntegrityLevel, nullptr, 0, &bytes);
            if (bytes == 0 || bytes > 64 * 1024)
            {
                ::CloseHandle(token);
                return false;
            }
            std::vector<unsigned char> buffer(bytes); // 函数拥有查询结果，解析不跨越缓冲生命周期。
            const BOOL queried = ::GetTokenInformation(token, TokenIntegrityLevel,
                buffer.data(), bytes, &bytes);
            ::CloseHandle(token);
            if (!queried)
            {
                return false;
            }
            const PSID sid = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(buffer.data())->Label.Sid;
            if (!::IsValidSid(sid) || *::GetSidSubAuthorityCount(sid) == 0)
            {
                return false;
            }
            *level = *::GetSidSubAuthority(sid, *::GetSidSubAuthorityCount(sid) - 1);
            return true;
        }

        // 校验真实管道客户：同机、同会话、同 SID、同完整性级别才进入 GUI 分派。
        bool isLocalPeer(QLocalSocket* socket)
        {
            const HANDLE pipe = reinterpret_cast<HANDLE>(socket->socketDescriptor()); // Qt 原生管道句柄。
            ULONG peerPid = 0; // 由操作系统提供，不相信 JSON 中的 PID。
            wchar_t peerComputer[MAX_COMPUTERNAME_LENGTH + 3] = {}; // 客户端计算机名。
            wchar_t ownComputer[MAX_COMPUTERNAME_LENGTH + 1] = {}; // 本机 NetBIOS 名称。
            DWORD ownLength = static_cast<DWORD>(std::size(ownComputer)); // 查询缓冲容量。
            if (!::GetNamedPipeClientProcessId(pipe, &peerPid))
            {
                return false;
            }
            const BOOL hasPeerName = ::GetNamedPipeClientComputerNameW(pipe, peerComputer,
                static_cast<ULONG>(std::size(peerComputer))); // Win32 本机连接可返回 ERROR_PIPE_LOCAL。
            const DWORD peerNameError = hasPeerName ? ERROR_SUCCESS : ::GetLastError();
            if (!hasPeerName && peerNameError != ERROR_PIPE_LOCAL)
            {
                return false;
            }
            // 部分 Windows 返回的计算机名含前导反斜杠，规范化后仍要求本机。
            const wchar_t* peerName = peerComputer;
            while (*peerName == L'\\')
            {
                ++peerName;
            }
            if (hasPeerName && (!::GetComputerNameW(ownComputer, &ownLength)
                || _wcsicmp(peerName, ownComputer) != 0))
            {
                return false;
            }
            DWORD peerSession = 0; // 客户进程所属 Windows 会话。
            DWORD ownSession = 0; // GUI 主程序所属 Windows 会话。
            if (!::ProcessIdToSessionId(peerPid, &peerSession)
                || !::ProcessIdToSessionId(::GetCurrentProcessId(), &ownSession)
                || peerSession != ownSession)
            {
                return false;
            }
            // UserAccessOption 的 ACL 是第一层；真实令牌 SID 是第二层身份复核。
            HANDLE peer = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, peerPid);
            if (!peer)
            {
                return false;
            }
            std::vector<unsigned char> peerUser; // 客户 TokenUser 值快照。
            std::vector<unsigned char> ownUser; // 服务 TokenUser 值快照。
            DWORD peerIntegrity = 0; // 客户端的权限完整性标签。
            DWORD ownIntegrity = 0; // 主程序的权限完整性标签。
            const bool queried = readUser(peer, peerUser) && readUser(::GetCurrentProcess(), ownUser)
                && readIntegrity(peer, &peerIntegrity)
                && readIntegrity(::GetCurrentProcess(), &ownIntegrity)
                && peerIntegrity == ownIntegrity;
            ::CloseHandle(peer);
            return queried && ::EqualSid(
                reinterpret_cast<TOKEN_USER*>(peerUser.data())->User.Sid,
                reinterpret_cast<TOKEN_USER*>(ownUser.data())->User.Sid);
        }

        // 生成纯机器码错误响应；不把调用方输入或秘密写入日志。
        QJsonObject error(const QString& code)
        {
            return QJsonObject{{QStringLiteral("status"), QStringLiteral("error")},
                {QStringLiteral("data"), QJsonObject{}},
                {QStringLiteral("error"), QJsonObject{{QStringLiteral("code"), code}}}};
        }
    }

    McpLocalBridge::McpLocalBridge(QWidget* host, McpUiController::PageList list,
        McpUiController::PageOpen open)
        : QObject(host), m_host(host),
          m_controller(std::make_unique<McpUiController>(host, std::move(list), std::move(open)))
    {
        // 控制器由 unique_ptr 先销毁，QObject 父子链同时约束所有排队调用。
        m_controller->setParent(this);
        m_endpoint = QString::fromLatin1(ks::mcp::EndpointPrefix)
            + QString::number(QCoreApplication::applicationPid());
        m_server = new QLocalServer(this);
        m_server->setSocketOptions(QLocalServer::UserAccessOption);
        m_server->setMaxPendingConnections(ks::mcp::MaxConnections);
        connect(m_server, &QLocalServer::newConnection, this, [this]()
        {
            // 首帧可能已到达并同步触发页面回调；每轮先检查桥，避免宿主销毁后继续取连接。
            const QPointer<McpLocalBridge> self(this);
            while (self && !m_closed && m_server->hasPendingConnections())
            {
                acceptConnection(m_server->nextPendingConnection());
            }
        });
    }

    McpLocalBridge::~McpLocalBridge()
    {
        close();
    }

    bool McpLocalBridge::start()
    {
        if (m_closed || !m_host)
        {
            m_lastError = QStringLiteral("closed");
            return false;
        }
        if (m_server->isListening())
        {
            return true;
        }
        // 不删除碰撞名称，不修改防火墙，也不将本地管道转成 TCP 服务。
        if (!m_server->listen(m_endpoint))
        {
            m_lastError = m_server->errorString();
            return false;
        }
        m_host->setProperty("ksword_mcp_endpoint", m_endpoint);
        return true;
    }

    void McpLocalBridge::close()
    {
        if (m_closed)
        {
            return;
        }
        m_closed = true;
        m_controller->close();
        m_server->close();
        // 此步在 MainWindow 拆除驱动/页面前执行，避免生命周期末尾接受动作。
        const auto sockets = findChildren<QLocalSocket*>();
        for (QLocalSocket* socket : sockets)
        {
            socket->abort();
            socket->deleteLater();
        }
        if (m_host)
        {
            m_host->setProperty("ksword_mcp_endpoint", QVariant{});
        }
    }

    QString McpLocalBridge::endpoint() const
    {
        return m_endpoint;
    }

    QString McpLocalBridge::lastError() const
    {
        return m_lastError;
    }

    void McpLocalBridge::registerReadAdapter(const QByteArray& className, McpUiController::ReadAdapter adapter)
    {
        if (!m_closed)
        {
            m_controller->registerReadAdapter(className, std::move(adapter));
        }
    }

    void McpLocalBridge::acceptConnection(QLocalSocket* socket)
    {
        if (!socket)
        {
            return;
        }
        socket->setParent(this);
        if (m_closed || m_connections >= ks::mcp::MaxConnections || !isLocalPeer(socket))
        {
            socket->abort();
            socket->deleteLater();
            return;
        }
        ++m_connections;
        connect(socket, &QObject::destroyed, this, [this]() { --m_connections; });
        connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
        socket->setReadBufferSize(ks::mcp::MaxRequestBytes + 1);
        // 连接具有固定总预算；慢速逐字节发送不能无限延长存活时间。
        QTimer::singleShot(ks::mcp::ConnectionTimeoutMs, socket, [socket]()
        {
            socket->abort();
            socket->deleteLater();
        });
        const auto buffer = std::make_shared<QByteArray>(); // 当前连接唯一请求的 UTF-8 帧。
        const auto handled = std::make_shared<bool>(false); // 一个连接只分派一次，避免重复副作用。
        const auto receive = [this, socket, buffer, handled]()
        {
            if (*handled || m_closed)
            {
                return;
            }
            buffer->append(socket->read(ks::mcp::MaxRequestBytes + 1 - buffer->size()));
            if (buffer->size() > ks::mcp::MaxRequestBytes)
            {
                *handled = true;
                socket->abort();
                return;
            }
            const qsizetype newline = buffer->indexOf('\n'); // 帧结束位置；JSON 内换行必须转义。
            if (newline < 0)
            {
                return;
            }
            *handled = true;
            QJsonParseError parseError; // Qt JSON 解析状态，不通过文本猜测合法性。
            const QJsonDocument document = QJsonDocument::fromJson(buffer->left(newline), &parseError);
            const QJsonObject request = document.object(); // 只接受对象请求与对象参数。
            const QString id = request.value(QStringLiteral("id")).toString(); // sidecar 请求配对标识。
            const QString method = request.value(QStringLiteral("method")).toString(); // 固定方法名。
            QJsonObject response; // 控件层结果，传输层只附加配对与版本信息。
            if (parseError.error != QJsonParseError::NoError || !document.isObject()
                || id.isEmpty() || id.size() > 128 || method.isEmpty() || method.size() > 64
                || !request.value(QStringLiteral("params")).isObject()
                || !buffer->mid(newline + 1).trimmed().isEmpty())
            {
                response = error(QStringLiteral("invalid_request"));
            }
            else if (method == QStringLiteral("hello"))
            {
                response = QJsonObject{{QStringLiteral("status"), QStringLiteral("ok")},
                    {QStringLiteral("data"), QJsonObject{
                        {QStringLiteral("pid"), QCoreApplication::applicationPid()},
                        {QStringLiteral("endpoint"), m_endpoint},
                        {QStringLiteral("invocation_receipts_only"), true}}}};
            }
            else
            {
                // 页面工厂可能在嵌套事件循环中销毁宿主，返回后先核验桥与连接。
                QPointer<McpLocalBridge> guardedBridge(this);
                QPointer<QLocalSocket> guardedSocket(socket);
                response = m_controller->request(method, request.value(QStringLiteral("params")).toObject());
                if (!guardedBridge || !guardedSocket || m_closed)
                {
                    return;
                }
            }
            // 不等待 GUI 动作完成；引擎的 accepted 回执与后续操作状态相互独立。
            response.insert(QStringLiteral("schemaVersion"), ks::mcp::SchemaVersion);
            response.insert(QStringLiteral("id"), id);
            QByteArray bytes = QJsonDocument(response).toJson(QJsonDocument::Compact) + '\n';
            if (bytes.size() > ks::mcp::MaxResponseBytes)
            {
                response = error(QStringLiteral("response_too_large"));
                response.insert(QStringLiteral("schemaVersion"), ks::mcp::SchemaVersion);
                response.insert(QStringLiteral("id"), id);
                bytes = QJsonDocument(response).toJson(QJsonDocument::Compact) + '\n';
            }
            if (socket->write(bytes) != bytes.size())
            {
                socket->abort();
                return;
            }
            socket->disconnectFromServer();
        };
        connect(socket, &QLocalSocket::readyRead, this, receive);
        // 接纳前已到达的字节也走同一个处理函数，避免依赖额外一次 readyRead。
        if (socket->bytesAvailable() > 0)
        {
            receive();
        }
    }
}
