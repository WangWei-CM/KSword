#pragma once

#include "../UI/PageNavigationCatalog.h"
#include "../Internationalization/LanguageManager.h"
#include <QIcon>
#include <QObject>
#include <QPointer>
#include <QTabWidget>
#include <QVariant>

namespace ksword::kernel_navigation
{
    struct Definition
    {
        const char* id;
        const char* parent;
        const char* context;
        const char* source;
        const char* aliases;
        bool current;
    };

    inline const QVector<Definition>& definitions()
    {
        static const QVector<Definition> values = {
            { "object", "", "kernel.main.tab.object_namespace.title", "对象命名空间", "Object Manager namespace 目录 对象 属性 刷新", true },
            { "atom", "", "kernel.main.tab.atom.title", "原子表遍历", "GlobalAtom ClipboardFormat 原子 校验", false },
            { "misc.ntquery", "misc", "kernel.main.tab.nt_query.title", "历史NtQuery", "NtQuery System Process Thread Object Token 查询", true },
            { "misc", "", "kernel.main.tab.misc.title", "杂项", "NtQuery knowledge integrity VBS HVCI RXPF", false },
            { "io", "", "kernel.main.tab.io_management.title", "I/O管理", "SSDT ShadowSSDT IDT GDT IOCTLS DriverStartIo 控制码", false },
            { "audit.inline", "audit", "", "Inline Hook", "Inline Hook 内核 导出 扫描 NOP 摘除", true },
            { "audit.iat", "audit", "", "IAT/EAT", "IAT EAT 导入 导出 指针 Hook", false },
            { "audit.callbacks", "audit", "kernel.main.tab.callback_enum.title", "回调遍历", "Callback Minifilter 回调 枚举 移除 驱动 文件", false },
            { "audit.intercept", "audit", "kernel.main.tab.callback.title", "驱动回调", "Registry Process Thread Image Object Minifilter 拦截 规则 询问 进程保护 FSCTL Oplock 文件监控", false },
            { "audit.unified", "audit", "kernel.main.tab.patch_hook_audit.title", "统一审计", "Patch Hook Inline IAT EAT SSDT ObjectType 驱动 完整性 风险", false },
            { "audit.shadow", "audit", "kernel.main.tab.shadow_comparison.title", "Shadow 比较", "SSDT ShadowSSDT 比较 被修改 模块", false },
            { "audit", "", "kernel.main.tab.audit_callbacks.title", "内核审计与回调", "Hook callback 回调 审计", false },
            { "hal", "", "kernel.main.tab.hal_audit.title", "HAL", "HalDispatchTable HalPrivateDispatchTable HalAcpiDispatchTable HalSubComponents", false },
            { "wdf", "", "kernel.main.tab.wdf_audit.title", "WDF", "KMDF WDF 函数 回调 绑定 模块", false },
            { "slat", "", "kernel.main.tab.slat_iommu.title", "SLAT/IOMMU", "EPT NPT SLAT IOMMU DMAR IVRS Hypervisor CPUID 交叉视图", false },
            { "misc.integrity", "misc", "kernel.main.tab.text_integrity.title", "代码完整性", "executable section relocation 字节 比较 完整性", false },
            { "misc.vbs", "misc", "kernel.main.tab.vbs_posture.title", "VBS/HVCI", "VBS HVCI CI 测试签名 审计模式 运行时", false },
            { "misc.hvci", "misc", "kernel.main.tab.hvci_bypass.title", "HVCI 绕过", "HVCI W^X MmChangeImageProtection MDL", false },
            { "misc.rxpf", "misc", "kernel.main.tab.rxpf.title", "RXPF 模拟", "RXPF IDT NX 指令 页错误 模拟", false },
            { "timers", "", "kernel.main.tab.timer_dpc.title", "定时器/DPC", "KTIMER KDPC CPU TimerTable 定时器 DPC", false },
            { "workers", "", "kernel.main.tab.work_queue_threads.title", "内核工作队列线程", "WrQueue ActiveExWorker 工作队列 线程", false },
            { "cid", "", "kernel.main.tab.cid.title", "CID表", "CID cross-view 交叉视图 进程 线程", false },
            { "ipc", "", "", "IPC", "NamedPipe NPFS ALPC RPC 通信 端口", false },
            { "misc.knowledge", "misc", "@kernel.knowledge.tab.title", "", "kernel knowledge 内核 知识 专题 原理 目录 搜索", false },
            { "io.ssdt", "io", "kernel.main.inner_tab.ssdt", "SSDT", "SSDT syscall Nt Zw 服务 索引", true },
            { "io.shadow", "io", "kernel.main.inner_tab.shadow_ssdt", "ShadowSSDT", "ShadowSSDT win32k win32u 服务", false },
            { "io.idt", "io", "kernel.main.inner_tab.idt", "IDT", "IDT CPU Handler 中断 完整性", false },
            { "io.gdt", "io", "kernel.main.inner_tab.gdt", "GDT", "GDT CPU 段描述符", false },
            { "io.ioctls", "io", "kernel.main.inner_tab.ioctls", "IOCTLS", "CTL_CODE Device Function Access Method 解码", false },
            { "io.startio", "io", "kernel.main.inner_tab.driver_start_io", "DriverStartIo", "DriverStartIo DriverObject 模块", false },
            { "object.overview", "object", "kernel.main.inner_tab.overview", "总览", "Object namespace 对象 目录 树 属性", true },
            { "object.directory", "object", "kernel.main.inner_tab.directory_recursion", "目录递归", "Directory recursion 对象 目录 递归", false },
            { "object.named", "object", "", "BaseNamedObjects", "BaseNamedObjects Session mutex event semaphore", false },
            { "object.links", "object", "kernel.main.inner_tab.symbolic_link", "符号链接", "SymbolicLink DOS Device 目标 符号链接", false },
            { "object.drivers", "object", "kernel.main.inner_tab.device_driver", "设备与驱动", "Device DriverObject DEVICE_OBJECT IRP MajorFunction FastIo 镜像 字节 编辑", false },
            { "object.iotimer", "object", "kernel.main.inner_tab.io_timer", "IoTimer", "IoTimer IoStartTimer IoStopTimer 设备 启动 停止", false },
            { "object.ioctl", "object", "kernel.main.inner_tab.ioctl_audit", "IOCTL 派遣表", "IOCTL Driver Device MajorFunction dispatch 注册表", false },
            { "object.procedures", "object", "kernel.main.inner_tab.object_type_procedures", "ObjectType 方法指针", "ObjectType Open Parse Close Delete 方法 指针", false },
            { "object.types", "object", "kernel.main.inner_tab.object_type", "对象类型", "ObjectType Matrix ObTypeIndexTable 类型", false },
            { "ipc.pipe", "ipc", "", "NamedPipe", "NamedPipe NPFS pipe 管道 目录", true },
            { "ipc.alpc", "ipc", "", "ALPC", "ALPC PID Handle Query Connection Server Client 端口", false },
            { "ipc.communication", "ipc", "kernel.ipc.tab.communication.title", "通信对象", "ALPC RPC 通信对象 端点", false },
            { "object.ioctl.drivers", "object.ioctl", "kernel.ioctl_audit.tab.drivers", "驱动概览", "DriverObject Ioctl 归属 派遣", true },
            { "object.ioctl.devices", "object.ioctl", "kernel.ioctl_audit.tab.devices", "设备对象", "DEVICE_OBJECT Device 设备 栈", false },
            { "object.ioctl.dispatch", "object.ioctl", "kernel.ioctl_audit.tab.dispatch", "MajorFunction", "IRP MajorFunction FastIo StartIo", false },
            { "object.ioctl.registry", "object.ioctl", "kernel.ioctl_audit.tab.registry", "KswordARK IOCTL 注册表", "KswordARK IOCTL handler 控制码", false },
            { "slat.probes", "slat", "kernel.slat_iommu.probe.tab", "EPT/NPT 交叉视图", "EPT NPT virtual physical hash 探针 交叉视图", true },
            { "slat.iommu", "slat", "kernel.slat_iommu.iommu.tab", "IOMMU / DMAR / IVRS", "IOMMU DMAR IVRS firmware 证据", false },
            { "audit.callbacks.list", "audit.callbacks", "kernel.callback.enum.view.list", "回调列表", "Callback 类别 来源 地址 文件 移除", true },
            { "audit.callbacks.tree", "audit.callbacks", "kernel.callback.enum.view.minifilter_tree", "Minifilter 回调树", "Minifilter IRP_MJ Pre Post 回调 树", false },
            { "hal.dispatch", "hal", "kernel.platform.hal.dispatch", "HalDispatchTable", "HAL WDF function address module verification 函数 地址 模块 完整性", true },
            { "hal.private", "hal", "kernel.platform.hal.private", "HalPrivateDispatchTable", "HAL WDF function address module verification 函数 地址 模块 完整性", false },
            { "hal.acpi", "hal", "kernel.platform.hal.acpi", "HalAcpiDispatchTable", "HAL WDF function address module verification 函数 地址 模块 完整性", false },
            { "hal.components", "hal", "kernel.platform.hal.subcomponents", "HalSubComponents", "HAL WDF function address module verification 函数 地址 模块 完整性", false },
            { "wdf.functions", "wdf", "kernel.platform.wdf.functions", "WDF 函数", "HAL WDF function address module verification 函数 地址 模块 完整性", true },
            { "wdf.callbacks", "wdf", "kernel.platform.wdf.callbacks", "WDF 回调", "HAL WDF function address module verification 函数 地址 模块 完整性", false },
            { "audit.intercept.registry", "audit.intercept", "kernel.callback.intercept.tab.registry", "注册表", "Callback rule GroupID 规则 匹配 动作 超时 决策 优先级", true },
            { "audit.intercept.process", "audit.intercept", "kernel.callback.intercept.tab.process", "进程创建", "Callback rule GroupID 规则 匹配 动作 超时 决策 优先级", false },
            { "audit.intercept.thread", "audit.intercept", "kernel.callback.intercept.tab.thread", "线程创建", "Callback rule GroupID 规则 匹配 动作 超时 决策 优先级", false },
            { "audit.intercept.image", "audit.intercept", "kernel.callback.intercept.tab.image", "镜像加载", "Callback rule GroupID 规则 匹配 动作 超时 决策 优先级", false },
            { "audit.intercept.object", "audit.intercept", "kernel.callback.intercept.tab.object", "对象管理器", "Callback rule GroupID 规则 匹配 动作 超时 决策 优先级", false },
            { "audit.intercept.minifilter", "audit.intercept", "kernel.callback.intercept.tab.minifilter", "文件系统微过滤器", "Callback rule GroupID 规则 匹配 动作 超时 决策 优先级", false },
            { "audit.intercept.application", "audit.intercept", "kernel.callback.intercept.log_tab.application", "应用日志", "application log 应用日志", true },
            { "audit.intercept.events", "audit.intercept", "kernel.callback.intercept.log_tab.events", "事件日志", "event archive 日志 事件", false },
            { "audit.intercept.bypass", "audit.intercept", "kernel.callback.intercept.minifilter.tab", "Minifilter PID 放行", "Minifilter PID bypass 放行", false },
            { "audit.intercept.protect", "audit.intercept", "kernel.callback.intercept.process_protect.tab", "进程保护", "process protection trusted 进程保护 可信 PID", false },
        };
        return values;
    }

    inline const Definition* definition(const QString& id)
    {
        for (const auto& value : definitions())
            if (id == QLatin1String(value.id)) return &value;
        return nullptr;
    }

    inline QString title(const Definition& value)
    {
        if (value.context[0] == '@') return ks::i18n::text(QString::fromLatin1(value.context + 1));
        const QString source = QString::fromUtf8(value.source);
        return value.context[0] == '\0' ? source
            : ks::i18n::contextText(QString::fromLatin1(value.context), source);
    }

    // These three existing pages are transferred to DriverDock. Keep them out
    // of Kernel's directory while giving DriverDock a construction-free value source.
    inline ks::ui::PageNavigationCatalog selfDriverCatalog()
    {
        static const Definition values[] = {
            { "overview", "", "kernel.dyndata.tab.overview", "总览", "DynData dynamic offsets System Informer 精确匹配 字段 偏移", true },
            { "profile", "", "", "PDB Profile", "PDB GUID Age profile 模块 符号 配置", false },
            { "status", "", "kernel.main.tab.driver_status.title", "驱动状态", "KswordARK driver protocol capability 安全策略 协议 能力", false },
        };
        ks::ui::PageNavigationCatalog pages;
        for (const auto& value : values)
            pages.push_back({ QString::fromLatin1(value.id), QString(), title(value),
                { QString::fromUtf8(value.source), QString::fromUtf8(value.aliases) },
                true, true, value.current });
        return pages;
    }

    inline ks::ui::PageNavigationCatalog catalog()
    {
        ks::ui::PageNavigationCatalog pages;
        pages.reserve(definitions().size());
        for (const auto& value : definitions())
        {
            pages.push_back({ QString::fromLatin1(value.id), QString::fromLatin1(value.parent),
                title(value), { QString::fromUtf8(value.source), QString::fromUtf8(value.aliases) },
                true, true, value.current });
        }
        return pages;
    }

    // Registered at actual factories. Reads never walk controls or create branches.
    class Registry final : public QObject
    {
    public:
        struct Binding { QPointer<QTabWidget> tabs; QPointer<QWidget> page; };
        explicit Registry(QWidget* root) : QObject(root)
        {
            root->setProperty(propertyName(), QVariant::fromValue(static_cast<QObject*>(this)));
        }
        static const char* propertyName() { return "ksword_kernel_navigation_registry"; }
        static Registry* from(QWidget* widget)
        {
            for (QWidget* current = widget; current != nullptr; current = current->parentWidget())
            {
                QObject* const owner = current->property(propertyName()).value<QObject*>();
                if (owner != nullptr) return static_cast<Registry*>(owner);
            }
            return nullptr;
        }
        void bind(const QString& id, QTabWidget* tabs, QWidget* page)
        {
            m_bindings.insert(id, { tabs, page });
        }
        Binding binding(const QString& id) const { return m_bindings.value(id); }
        ks::ui::PageNavigationCatalog snapshot() const
        {
            auto pages = catalog();
            for (auto& page : pages)
            {
                const auto bound = binding(page.id);
                if (bound.tabs.isNull() || bound.page.isNull())
                {
                    if (m_bindings.contains(page.id)) { page.visible = false; page.current = false; }
                    continue;
                }
                const int index = bound.tabs->indexOf(bound.page);
                if (index < 0) { page.visible = false; page.current = false; continue; }
                page.title = bound.tabs->tabText(index);
                page.searchTexts.append(bound.tabs->tabToolTip(index));
                page.visible = bound.tabs->isTabVisible(index);
                page.enabled = bound.tabs->isTabEnabled(index);
                page.current = bound.tabs->currentWidget() == bound.page;
            }
            return pages;
        }
    private:
        QHash<QString, Binding> m_bindings;
    };

    inline int addTab(QTabWidget* tabs, QWidget* page, const QIcon& icon, const char* id)
    {
        const auto* value = definition(QString::fromLatin1(id));
        Q_ASSERT(value != nullptr);
        if (value == nullptr) return -1;
        const int index = tabs->addTab(page, icon, title(*value));
        if (auto* registry = Registry::from(tabs)) registry->bind(QString::fromLatin1(id), tabs, page);
        return index;
    }
    inline int addTab(QTabWidget* tabs, QWidget* page, const char* id)
    {
        return addTab(tabs, page, QIcon(), id);
    }
}
