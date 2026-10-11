#include "../Framework.h"
#include "./PortDiagnosticsPage.h"
#include "../UI/AsyncOperation.h"
#include "../UI/CodeEditorWidget.h"
#include "../UI/FlatButtonTheme.h"
#include "../UI/PrimaryPageStyle.h"
#include "../UI/ParameterWorkspace.h"
#include "../UI/ToolbarMetrics.h"
#include "../UI/VisibleTableWidget.h"
#include "../Internationalization/LanguageManager.h"

#include <QComboBox>
#include <QGroupBox>
#include <QFormLayout>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QStringList>
#include <QVBoxLayout>

namespace
{
    // 翻译只在 UI 线程求值；后台请求与结果不保存 LanguageManager 或 QWidget。
    QString portText(const char* source)
    {
        return ks::i18n::sourceText(QString::fromUtf8(source));
    }

    QString protocolText(bool tcp, bool ipv6)
    {
        return QStringLiteral("%1 / %2").arg(tcp ? QStringLiteral("TCP") : QStringLiteral("UDP"),
            ipv6 ? QStringLiteral("IPv6") : QStringLiteral("IPv4"));
    }

    QString endpointText(const std::string& address, std::uint16_t port, bool ipv6)
    {
        const QString host = QString::fromUtf8(address.c_str());
        return ipv6 ? QStringLiteral("[%1]:%2").arg(host).arg(port)
            : QStringLiteral("%1:%2").arg(host).arg(port);
    }

    // 只有全部证据完整且全部探测通过才显示可绑定；这仍只是当次检查的结论。
    QString snapshotSummary(const ks::network::PortDiagnosticSnapshot& snapshot)
    {
        if (!snapshot.errors.empty())
        {
            return portText("端口 %1：诊断不完整，不能确认是否空闲。" ).arg(snapshot.port);
        }
        const bool available = !snapshot.probes.empty()
            && std::all_of(snapshot.probes.begin(), snapshot.probes.end(),
                [](const auto& probe) { return probe.error == 0; });
        return available ? portText("端口 %1：当前独占绑定检测通过，占用记录 %2 条。").arg(snapshot.port).arg(snapshot.occupants.size())
            : portText("端口 %1：存在占用或绑定限制，占用记录 %2 条。").arg(snapshot.port).arg(snapshot.occupants.size());
    }

    // 各表由业务区块自己提供标题和动作，隐藏冻结/快照等重复且无用途的通用操作条。
    QTableWidget* createEvidenceTable(QWidget* parent, const QStringList& headers)
    {
        auto* table = new ks::ui::VisibleTableWidget(parent);
        ks::ui::SetTableActionBarMode(table, ks::ui::TableActionBarMode::None);
        table->setColumnCount(headers.size());
        table->setHorizontalHeaderLabels(headers);
        table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table->setSelectionBehavior(QAbstractItemView::SelectRows);
        table->setSelectionMode(QAbstractItemView::SingleSelection);
        table->verticalHeader()->hide();
        table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
        table->horizontalHeader()->setStretchLastSection(true);
        return table;
    }
}

PortDiagnosticsPage::PortDiagnosticsPage(QWidget* parent) : QWidget(parent)
{
    m_operation = new ks::ui::AsyncOperation(this);
    auto* root = new QVBoxLayout(this); // 查询、占用与绑定各自分区，保持主要结果可伸展。
    root->setContentsMargins(0, 0, 0, 0);
    auto* workspace = new ks::ui::ParameterWorkspace(this, 380); // 参数与诊断结果独立使用空间。
    root->addWidget(workspace);
    auto* controls = new QFormLayout;
    controls->setRowWrapPolicy(QFormLayout::WrapAllRows);
    m_port = new QSpinBox(this);
    m_port->setRange(1, 65535);
    m_port->setValue(8080);
    m_port->setMinimumWidth(110);
    controls->addRow(portText("本地端口"), m_port);
    m_protocol = new QComboBox(this);
    m_protocol->addItems({portText("TCP 与 UDP"), QStringLiteral("TCP"), QStringLiteral("UDP")});
    ks::ui::StylePrimaryCombo(m_protocol);
    controls->addRow(m_protocol);
    m_diagnose = new QPushButton(QIcon(QStringLiteral(":/Icon/process_refresh.svg")), portText("诊断"), this);
    m_diagnose->setToolTip(portText("查询占用进程并检测 IPv4/IPv6 的独占绑定能力"));
    m_release = new QPushButton(portText("解除占用"), this);
    m_release->setToolTip(portText("结束所选占用进程，影响该进程的全部任务与连接"));
    m_disconnect = new QPushButton(portText("断开 TCP"), this);
    m_disconnect->setToolTip(portText("仅断开所选 IPv4 活动 TCP 连接，不会关闭监听端口"));
    workspace->parameters()->addLayout(controls);
    workspace->actions()->addWidget(m_diagnose);
    workspace->actions()->addStretch();
    ks::ui::ApplyFlatButtonTheme(m_diagnose);
    ks::ui::ApplyFlatButtonTheme(m_release, ks::ui::FlatButtonTone::Danger);
    ks::ui::ApplyFlatButtonTheme(m_disconnect);
    m_status = new QLabel(portText("输入本地端口后点击诊断。"), this);
    m_status->setWordWrap(true);
    workspace->parameters()->addWidget(m_status);

    auto* ownersGroup = new QGroupBox(portText("占用进程与端点"), this);
    auto* ownersLayout = new QVBoxLayout(ownersGroup);
    ks::ui::StyleParameterSection(ownersGroup);
    // 解除/断开依赖当前结果行，放在占用表旁，避免挤宽参数执行栏。
    auto* ownerActions = new QHBoxLayout;
    ownerActions->addWidget(m_release);
    ownerActions->addWidget(m_disconnect);
    ownerActions->addStretch();
    ks::ui::NormalizeToolbarRow(ownerActions);
    ownersLayout->addLayout(ownerActions);
    m_owners = createEvidenceTable(ownersGroup,
        {portText("协议 / 地址族"), portText("本地端点"), portText("状态"), QStringLiteral("PID"), portText("进程")});
    ownersLayout->addWidget(m_owners, 1);
    m_details = new QLabel(ownersGroup);
    m_details->setWordWrap(true);
    m_details->setTextFormat(Qt::PlainText);
    m_details->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_details->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    ownersLayout->addWidget(m_details);
    workspace->results()->addWidget(ownersGroup, 1);

    auto* probeGroup = new QGroupBox(portText("绑定检测"), this);
    auto* probeLayout = new QVBoxLayout(probeGroup);
    ks::ui::StyleParameterSection(probeGroup);
    m_probes = createEvidenceTable(probeGroup,
        {portText("协议 / 地址族"), portText("检查结果"), portText("错误码")});
    m_probes->setMaximumHeight(145);
    probeLayout->addWidget(m_probes);
    auto* explanation = new QLabel(portText("检测只做短暂独占绑定，不监听、不收发数据。无进程但绑定失败时，可能涉及 TIME_WAIT、系统保留或独占限制。"), probeGroup);
    explanation->setWordWrap(true);
    probeLayout->addWidget(explanation);
    workspace->results()->addWidget(probeGroup);
    m_report = new CodeEditorWidget(this);
    m_report->setReadOnly(true);
    m_report->setMaximumHeight(100);
    m_report->hide();
    workspace->results()->addWidget(m_report);
    workspace->finish();

    // 输入变更清空旧目标，所有写操作只使用诊断时保存的完整值快照。
    connect(m_port, &QSpinBox::valueChanged, this, [this]() { invalidateSnapshot(); });
    connect(m_protocol, &QComboBox::currentIndexChanged, this, [this]() { invalidateSnapshot(); });
    connect(m_diagnose, &QPushButton::clicked, this, [this]() { diagnose(); });
    connect(m_release, &QPushButton::clicked, this, [this]() { release(ks::network::PortReleaseAction::TerminateOwner); });
    connect(m_disconnect, &QPushButton::clicked, this, [this]() { release(ks::network::PortReleaseAction::DisconnectTcp); });
    connect(m_owners, &QTableWidget::itemSelectionChanged, this, [this]() { updateSelection(); });
    setBusy(false);
}

PortDiagnosticsPage::~PortDiagnosticsPage()
{
    m_operation->close(); // 先关闭异步投递，后台仅保留值请求和取消令牌。
}

void PortDiagnosticsPage::setBusy(bool busy)
{
    m_busy = busy;
    m_port->setEnabled(!busy);
    m_protocol->setEnabled(!busy);
    m_diagnose->setEnabled(!busy);
    m_owners->setEnabled(!busy);
    updateSelection();
}

void PortDiagnosticsPage::invalidateSnapshot()
{
    m_hasSnapshot = false;
    m_snapshot = {};
    m_owners->setRowCount(0);
    m_probes->setRowCount(0);
    m_report->hide();
    m_status->setText(portText("输入已变化，请重新诊断。"));
    updateSelection();
}

int PortDiagnosticsPage::selectedRecord() const
{
    const int row = m_owners->currentRow();
    const auto* item = row < 0 || m_owners->selectedItems().isEmpty() ? nullptr : m_owners->item(row, 0);
    const int index = item == nullptr ? -1 : item->data(Qt::UserRole).toInt();
    return m_hasSnapshot && index >= 0 && static_cast<std::size_t>(index) < m_snapshot.occupants.size()
        ? index : -1;
}

void PortDiagnosticsPage::updateSelection()
{
    m_release->setEnabled(false);
    m_disconnect->setEnabled(false);
    m_details->clear();
    const int index = selectedRecord();
    if (index < 0)
    {
        return;
    }
    const auto& owner = m_snapshot.occupants[index];
    const bool actionable = !m_busy && owner.pid > 4 && owner.pid != GetCurrentProcessId()
        && owner.creationTime100ns != 0;
    m_release->setEnabled(actionable);
    m_disconnect->setEnabled(actionable && owner.tcp && ks::network::IsTcpConnectionTerminableBySetTcpEntry(owner.connection));
    QString detail = portText("映像：%1").arg(owner.imagePath.empty() ? portText("不可用") : QString::fromUtf8(owner.imagePath.c_str()));
    if (owner.tcp && owner.connection.remotePort != 0)
    {
        detail += portText("；远端：%1").arg(endpointText(owner.connection.remoteAddressText, owner.connection.remotePort, owner.ipv6));
    }
    if (!actionable && !m_busy)
    {
        detail += portText("；系统、当前程序或身份缺失的记录不提供解除操作。");
    }
    m_details->setText(detail);
}

void PortDiagnosticsPage::showSnapshot(const ks::network::PortDiagnosticSnapshot& snapshot)
{
    m_hasSnapshot = false;
    m_snapshot = snapshot;
    m_owners->setRowCount(static_cast<int>(snapshot.occupants.size()));
    for (int row = 0; row < m_owners->rowCount(); ++row)
    {
        const auto& owner = snapshot.occupants[row];
        const QStringList cells{protocolText(owner.tcp, owner.ipv6), endpointText(owner.localAddress, snapshot.port, owner.ipv6),
            owner.tcp ? QString::fromUtf8(owner.state.c_str()) : portText("已绑定"), QString::number(owner.pid),
            owner.processName.empty() ? portText("不可用") : QString::fromUtf8(owner.processName.c_str())};
        for (int column = 0; column < cells.size(); ++column)
        {
            auto* item = new QTableWidgetItem(cells[column]); // 索引随单元格排序移动，保持目标身份。
            item->setData(Qt::UserRole, row);
            item->setToolTip(cells[column]);
            m_owners->setItem(row, column, item);
        }
    }
    m_probes->setRowCount(static_cast<int>(snapshot.probes.size()));
    for (int row = 0; row < m_probes->rowCount(); ++row)
    {
        const auto& probe = snapshot.probes[row];
        const QString verdict = probe.error == 0 ? portText("可绑定")
            : probe.error == WSAEADDRINUSE ? portText("地址/端口已占用")
            : probe.error == WSAEACCES ? portText("拒绝绑定：独占占用或系统限制")
            : portText("检测失败");
        m_probes->setItem(row, 0, new QTableWidgetItem(protocolText(probe.tcp, probe.ipv6)));
        m_probes->setItem(row, 1, new QTableWidgetItem(verdict));
        m_probes->setItem(row, 2, new QTableWidgetItem(QString::number(probe.error)));
    }
    QStringList errors;
    for (const auto& error : snapshot.errors)
    {
        errors.push_back(portText(error.c_str()));
    }
    m_report->setText(errors.join(QLatin1Char('\n')));
    m_report->setVisible(!errors.isEmpty());
    m_status->setText(snapshotSummary(snapshot));
    m_hasSnapshot = true;
    if (m_owners->rowCount() > 0)
    {
        m_owners->selectRow(0);
    }
    updateSelection();
}

void PortDiagnosticsPage::diagnose()
{
    if (m_busy)
    {
        return;
    }
    using Request = std::pair<std::uint16_t, ks::network::PortProtocol>;
    const Request request{static_cast<std::uint16_t>(m_port->value()),
        static_cast<ks::network::PortProtocol>(m_protocol->currentIndex())};
    setBusy(true);
    m_status->setText(portText("正在诊断端口 %1…").arg(request.first));
    m_operation->submit<Request, ks::network::PortDiagnosticSnapshot>(request,
        [](const Request& value, const ks::ui::AsyncOperationToken&)
        {
            return ks::network::DiagnoseLocalPort(value.first, value.second);
        }, [this](const auto& result, std::uint64_t)
        {
            showSnapshot(result);
            setBusy(false);
            return true;
        }, [this](std::exception_ptr)
        {
            invalidateSnapshot();
            m_status->setText(portText("诊断任务失败，请重试。"));
            setBusy(false);
        });
}

void PortDiagnosticsPage::release(ks::network::PortReleaseAction action)
{
    const int index = selectedRecord();
    if (m_busy || index < 0)
    {
        return;
    }
    // 复制快照再显示确认，不能在嵌套消息循环结束后借用可能变化的表格索引。
    struct Request
    {
        ks::network::PortOccupant owner;
        std::uint16_t port;
        ks::network::PortProtocol protocol;
        ks::network::PortReleaseAction action;
    };
    const Request request{m_snapshot.occupants[index], m_snapshot.port, m_snapshot.protocol, action};
    setBusy(true);
    const QString prompt = action == ks::network::PortReleaseAction::TerminateOwner
        ? portText("结束 %1（PID %2）以解除端口 %3 的占用？\n这会终止该进程的全部任务与连接，未保存的数据可能丢失。").arg(QString::fromUtf8(request.owner.processName.c_str())).arg(request.owner.pid).arg(request.port)
        : portText("断开所选 TCP 连接？\n这只处理该连接，不会关闭进程的监听端口。");
    const QPointer<PortDiagnosticsPage> guardedPage(this); // 嵌套循环期间宿主可能关闭。
    const QPointer<QMessageBox> confirmation(new QMessageBox(QMessageBox::Warning,
        portText("端口诊断"), prompt, QMessageBox::Yes | QMessageBox::No, this));
    confirmation->setTextFormat(Qt::PlainText);
    confirmation->setDefaultButton(QMessageBox::No);
    const int choice = confirmation->exec();
    if (confirmation)
    {
        confirmation->deleteLater();
    }
    if (!guardedPage)
    {
        return;
    }
    if (choice != QMessageBox::Yes)
    {
        setBusy(false);
        return;
    }
    m_status->setText(portText("正在解除并重新诊断端口 %1…").arg(request.port));
    m_operation->submit<Request, ks::network::PortReleaseResult>(request,
        [](const Request& value, const ks::ui::AsyncOperationToken& token)
        {
            return ks::network::ReleaseLocalPort(value.owner, value.port, value.protocol, value.action,
                [token]() { return token.isCanceled(); });
        }, [this, action](const auto& result, std::uint64_t)
        {
            showSnapshot(result.after);
            const QString outcome = result.performed
                ? (action == ks::network::PortReleaseAction::DisconnectTcp ? portText("已断开所选连接。") : portText("已提交结束进程请求。"))
                : portText("解除未执行或失败。");
            m_status->setText(outcome + QLatin1Char(' ') + snapshotSummary(result.after));
            if (!result.error.empty())
            {
                m_report->setText(portText(result.error.c_str()) + QLatin1Char('\n') + m_report->text());
                m_report->show();
            }
            setBusy(false);
            return true;
        }, [this](std::exception_ptr)
        {
            invalidateSnapshot();
            m_status->setText(portText("解除任务失败，请重新诊断确认端口状态。"));
            setBusy(false);
        });
}
