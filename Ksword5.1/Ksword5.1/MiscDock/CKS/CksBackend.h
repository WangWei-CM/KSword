#pragma once

// CKS 后端只通过公开的 Windows 用户态接口读取状态与执行明确的系统操作。
// 页面不直接修改 ProductPolicy、UEFI 变量、BCD 或受保护的 CI 注册表键。

#include <QString>
#include <QVector>

#include <cstdint>

namespace ks::misc::cks
{
    // Check：区分“明确关闭”和“查询失败”，避免把缺少权限误报为未启用。
    struct Check
    {
        bool known = false;        // known：本次查询是否取得可信的布尔值。
        bool enabled = false;      // enabled：查询成功时的状态。
        std::uint32_t error = 0;  // error：查询失败时的 Win32/HRESULT 错误码。
    };

    // Service：只记录名为 ssde 的服务；不会把其他驱动服务识别成 CKS。
    struct Service
    {
        bool installed = false;       // installed：SCM 中是否存在 ssde 服务。
        bool queryFailed = false;     // queryFailed：存在以外的查询错误。
        bool running = false;         // running：驱动服务是否实际处于 RUNNING。
        bool bootStart = false;       // bootStart：下次启动是否配置为 BOOT_START。
        bool expectedImage = false;   // expectedImage：镜像文件名是否为 ssde.sys。
        QString imagePath;            // imagePath：SCM 回报的镜像路径，仅用于显示。
        QString errorText;            // errorText：失败原因。
    };

    // Policy：CiTool 报告的一条策略，状态只代表 CiTool 当前读回。
    struct Policy
    {
        QString id;               // id：策略 GUID。
        QString name;             // name：策略友好名称。
        bool enforcedKnown = false; // enforcedKnown：CiTool 是否给出 IsEnforced。
        bool enforced = false;      // enforced：CiTool 的 IsEnforced，表示当前生效。
        bool signedKnown = false;   // signedKnown：CiTool 是否给出签名字段。
        bool signedPolicy = false;  // signedPolicy：CiTool 的 IsSignedPolicy 值。
    };

    // Snapshot：页面一次刷新取得的系统证据，不推断“CKS 完全可用”。
    struct Snapshot
    {
        bool elevated = false;       // elevated：当前进程令牌是否已提升。
        std::uint32_t build = 0;     // build：Windows 内部构建号。
        Check secureBoot;            // secureBoot：系统报告的安全启动状态。
        Check hvci;                  // hvci：注册表配置的 HVCI 状态，不代表运行位。
        Check productPolicy;         // productPolicy：许可 API 中 CKS 产品策略值。
        Check licensed;              // licensed：CI/Protected/Licensed 注册表值。
        Service ssde;                // ssde：持久化驱动服务状态。
        QVector<Policy> policies;    // policies：CiTool 返回的策略列表。
        QString policyError;         // policyError：CiTool 不可用或查询失败原因。
    };

    // QuerySnapshot：供后台线程调用；返回状态证据与明确的查询失败信息。
    Snapshot QuerySnapshot();

    // DeploySignedPolicy：把用户提供的已签名 .cip 交给系统 CiTool；
    // 成功只表示部署命令成功，不表示已通过重置或驱动加载验收。
    bool DeploySignedPolicy(const QString& path, QString* detailOut);

    // LaunchSsdeEnable：以独立控制台启动用户指定的 SSDE 启用程序。
    // 程序自行询问重启；此调用不把启动成功解释为 CKS 已启用。
    bool LaunchSsdeEnable(const QString& path, QString* detailOut);

    // InstallSsdeDriver：复制已被 Windows 信任的 ssde.sys，并创建按需启动的服务。
    // 只有读取到 CKS 与 Licensed 均为 1 时才允许安装；不覆盖现存服务/文件。
    bool InstallSsdeDriver(const QString& path, QString* detailOut);

    // StartSsdeDriver：启动已安装的 ssde 服务，并等待 SCM 报告 RUNNING。
    bool StartSsdeDriver(QString* detailOut);

    // SetSsdeBootStart：仅修改已核验 ssde 服务的启动类型。
    // 启用开机启动需要服务运行且 CKS 已获许可；停用不会删除镜像或策略。
    bool SetSsdeBootStart(bool enable, QString* detailOut);
}
