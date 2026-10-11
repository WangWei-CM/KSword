// 独立真实 Qt 夹具：直接调用生产 MCP 控件引擎，不启动 KSword 或驱动。
// 标准文本控件仅作为协议测试目标，不作为产品详情/日志窗口的新实现。
#include "../Ksword5.1/Ksword5.1/UI/McpUiController.h"
#include "../Ksword5.1/Ksword5.1/UI/McpLocalBridge.h"
#include <QApplication>
#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QElapsedTimer>
#include <QFile>
#include <QIntValidator>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QTableView>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <cstdio>
#include <functional>
#include <memory>
#include <thread>

namespace
{
int g_checks = 0; // 所有已执行断言数，汇总必须与退出码一致。
int g_failures = 0; // 行为失败计数，缺少汇总不能被当作回归通过。

// check：记录条件和可读用例名；输入为真实状态，输出为日志与累计结果。
void check(bool condition, const char* description)
{
    ++g_checks;
    if (!condition)
    {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", description);
    }
}

// payload：抽取协议 data 对象；类型错误保留为空，供后续断言发现。
QJsonObject payload(const QJsonObject& response)
{
    return response.value(QStringLiteral("data")).toObject();
}

// success/error：只判断顶层协议状态，绝不推断业务或驱动操作已经完成。
bool success(const QJsonObject& response)
{
    return response.value(QStringLiteral("status")) == QStringLiteral("ok");
}

bool rejected(const QJsonObject& response, const QString& expected = {})
{
    const QString code = response.value(QStringLiteral("error")).toObject()
        .value(QStringLiteral("code")).toString(); // 固定机器错误码。
    return response.value(QStringLiteral("status")) == QStringLiteral("error")
        && !code.isEmpty() && (expected.isEmpty() || code == expected);
}

// drain：排空单层 queued 回调，不使用猜测弹窗出现时间的嵌套定时器。
void drain()
{
    for (int pass = 0; pass < 6; ++pass) // 有界事件循环轮次，夹具不启动周期采样器。
    {
        QCoreApplication::sendPostedEvents(nullptr, 0);
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
}

// 标识所有测试对象；机器 ID 与可翻译展示文字分开，销毁重建由桥编号区分。
void identify(QWidget* widget, const QString& identifier)
{
    widget->setObjectName(identifier);
    widget->setAccessibleIdentifier(identifier);
    widget->setAccessibleName(identifier);
}

class Fixture final
{
public:
    QWidget host; // 夹具根窗口，所有测试目标必须属于它。
    QVBoxLayout* layout = nullptr; // 根布局，仅布置虚拟控件。
    QPushButton* button = nullptr; // 排队点击目标，记录真实 clicked 信号。
    QPushButton* disabled = nullptr; // 初始禁用目标，验证桥不能绕过状态。
    QPushButton* quitButton = nullptr; // 仅服务模式用来停止夹具事件循环。
    QLineEdit* integerEdit = nullptr; // 带 validator 的真实字段。
    QLineEdit* password = nullptr; // 合成密码字段，验证任何返回路径都遮蔽。
    QComboBox* combo = nullptr; // 非编辑下拉框，验证 activated 语义。
    QComboBox* editableCombo = nullptr; // 编辑下拉框，测试自由文本输入。
    QSpinBox* number = nullptr; // 有界整数，越界请求不得被静默夹紧。
    QCheckBox* checked = nullptr; // 状态目标，观察真实 clicked/toggled。
    QTabWidget* tabs = nullptr; // 普通页签，测试有限索引切换。
    QTableView* table = nullptr; // 动态模型 view，不加载系统进程数据。
    QStandardItemModel* model = nullptr; // 已构造的内存表模型。
    QPlainTextEdit* text = nullptr; // 标准只读协议文本，不访问文件或用户日志。
    QLabel* customLabel = nullptr; // 只读扩展用合成目标，不代表实际 HexCanvas 验收。
    QToolButton* menuButton = nullptr; // 关联动态菜单，验证无父菜单的明确归属。
    std::unique_ptr<QMenu> menu; // 无 QObject 父级菜单，生命周期由夹具持有。
    QPointer<QWidget> lazyPage; // 仅 open_page 回调才构造的惰性页面。
    std::unique_ptr<ks::ui::McpUiController> controller; // 真实生产引擎。
    int openings = 0; // 惰性构造次数，列目录不得递增。
    int clicks = 0; // 真实按钮点击次数。
    int edits = 0; // 用户编辑信号次数，setText 的假成功会使断言失败。
    int submits = 0; // returnPressed 提交次数。
    int activations = 0; // 下拉框用户激活次数。
    int menuTriggers = 0; // QAction 真实触发次数。
    QString lastEdited; // 最后一次 textEdited 的完整字段值。
    QHash<QString, QString> knownIds; // 模态/菜单缩小快照范围时仍可验证已登记 ID 的拒绝语义。

    // 构造：创建纯 Qt 控件和生产引擎，所有字段/回调只留在夹具进程中。
    Fixture()
    {
        identify(&host, QStringLiteral("fixture.host"));
        host.setAttribute(Qt::WA_DontShowOnScreen);
        host.setAttribute(Qt::WA_ShowWithoutActivating);
        host.resize(640, 900);
        layout = new QVBoxLayout(&host);
        button = new QPushButton(QStringLiteral("Run"), &host);
        disabled = new QPushButton(QStringLiteral("Disabled"), &host);
        quitButton = new QPushButton(QStringLiteral("Exit fixture"), &host);
        identify(button, QStringLiteral("fixture.run"));
        identify(disabled, QStringLiteral("fixture.disabled"));
        identify(quitButton, QStringLiteral("fixture.quit"));
        disabled->setEnabled(false);
        layout->addWidget(button);
        layout->addWidget(disabled);
        layout->addWidget(quitButton);
        QObject::connect(button, &QPushButton::clicked, &host, [this]() { ++clicks; });
        QObject::connect(quitButton, &QPushButton::clicked, &host, []() { qApp->quit(); });

        // 数字编辑器使用真实 QIntValidator，编辑和提交各自计数。
        integerEdit = new QLineEdit(QStringLiteral("10"), &host);
        identify(integerEdit, QStringLiteral("fixture.integer"));
        integerEdit->setValidator(new QIntValidator(0, 99, integerEdit));
        password = new QLineEdit(QStringLiteral("fixture-secret-value"), &host);
        identify(password, QStringLiteral("fixture.password"));
        password->setEchoMode(QLineEdit::Password);
        layout->addWidget(integerEdit);
        layout->addWidget(password);
        QObject::connect(integerEdit, &QLineEdit::textEdited, &host,
            [this](const QString& value) { ++edits; lastEdited = value; });
        QObject::connect(integerEdit, &QLineEdit::returnPressed, &host, [this]() { ++submits; });

        // Combo 的 activated 与程序性改值分离，直接 setCurrentIndex 无法通过本用例。
        combo = new QComboBox(&host);
        identify(combo, QStringLiteral("fixture.combo"));
        combo->addItems({QStringLiteral("Alpha"), QStringLiteral("Beta"), QStringLiteral("Gamma")});
        editableCombo = new QComboBox(&host);
        identify(editableCombo, QStringLiteral("fixture.editable_combo"));
        editableCombo->addItem(QStringLiteral("Initial"));
        editableCombo->setEditable(true);
        number = new QSpinBox(&host);
        identify(number, QStringLiteral("fixture.number"));
        number->setRange(0, 100);
        number->setValue(12);
        checked = new QCheckBox(QStringLiteral("Check"), &host);
        identify(checked, QStringLiteral("fixture.check"));
        layout->addWidget(combo);
        layout->addWidget(editableCombo);
        layout->addWidget(number);
        layout->addWidget(checked);
        QObject::connect(combo, &QComboBox::activated, &host, [this](int) { ++activations; });

        // 表格仅存合成实体；模型变化应产生 revision，禁止按旧行号选错对象。
        tabs = new QTabWidget(&host);
        identify(tabs, QStringLiteral("fixture.tabs"));
        tabs->addTab(new QWidget(tabs), QStringLiteral("First"));
        tabs->addTab(new QWidget(tabs), QStringLiteral("Second"));
        table = new QTableView(&host);
        identify(table, QStringLiteral("fixture.table"));
        model = new QStandardItemModel(2, 3, &host);
        model->setHorizontalHeaderLabels({QStringLiteral("Name"), QStringLiteral("PID"), QStringLiteral("State")});
        model->setData(model->index(0, 0), QStringLiteral("Alpha"));
        model->setData(model->index(0, 1), 101);
        model->setData(model->index(0, 2), QStringLiteral("ready"));
        model->setData(model->index(1, 0), QStringLiteral("Beta"));
        model->setData(model->index(1, 1), 202);
        model->setData(model->index(1, 2), QStringLiteral("running"));
        table->setModel(model);
        table->setSelectionBehavior(QAbstractItemView::SelectRows);
        layout->addWidget(tabs);
        layout->addWidget(table);

        // 只读文本验证 UTF-16 范围与预算，不读取任何实际文件或日志。
        text = new QPlainTextEdit(&host);
        identify(text, QStringLiteral("fixture.text"));
        text->setReadOnly(true);
        text->setPlainText(QStringLiteral("0123456789abcdefghij") + QString(6000, QLatin1Char('x')));
        layout->addWidget(text);
        customLabel = new QLabel(QStringLiteral("Adapter sample"), &host);
        identify(customLabel, QStringLiteral("fixture.custom_label"));
        layout->addWidget(customLabel);
        menuButton = new QToolButton(&host);
        identify(menuButton, QStringLiteral("fixture.menu_button"));
        menuButton->setText(QStringLiteral("Menu"));
        menuButton->setPopupMode(QToolButton::InstantPopup);
        menu = std::make_unique<QMenu>();
        menu->setObjectName(QStringLiteral("fixture.menu"));
        menu->setStyleSheet(QStringLiteral("QMenu { background:#ffffff; color:#202020; } QMenu::item:selected { background:#d8e5ff; } QMenu::item:disabled { color:#777777; }"));
        menuButton->setMenu(menu.get());
        layout->addWidget(menuButton);

        // 惰性目录和打开回调直接供引擎使用，目录阶段不触发构造。
        controller = std::make_unique<ks::ui::McpUiController>(&host,
            [this]() { return pages(); }, [this](const QString& id) { return open(id); });
        host.show();
        drain();
    }

    // pages：返回固定目录与当前初始化状态，不访问系统或修改任何页面。
    QJsonArray pages() const
    {
        return {QJsonObject{{QStringLiteral("id"), QStringLiteral("fixture.main")},
                    {QStringLiteral("title"), QStringLiteral("Main")}, {QStringLiteral("initialized"), true}},
            QJsonObject{{QStringLiteral("id"), QStringLiteral("fixture.lazy")},
                {QStringLiteral("title"), QStringLiteral("Lazy")}, {QStringLiteral("initialized"), !lazyPage.isNull()}}};
    }

    // open：仅匹配页面 ID 时构造惰性节点，返回真实根 QWidget。
    QWidget* open(const QString& id)
    {
        if (id == QStringLiteral("fixture.main"))
        {
            return &host;
        }
        if (id != QStringLiteral("fixture.lazy"))
        {
            return nullptr;
        }
        if (!lazyPage)
        {
            ++openings;
            lazyPage = new QWidget(&host);
            identify(lazyPage, QStringLiteral("fixture.lazy_root"));
            new QLabel(QStringLiteral("Lazy content"), lazyPage);
        }
        return lazyPage;
    }

    // call：输入固定方法和 JSON 对象，直接返回生产引擎协议，不制作模拟结果。
    QJsonObject call(const QString& method, const QJsonObject& params = {})
    {
        return controller->request(method, params);
    }

    // node：按稳定标识找最新快照节点；未找到返回空对象供测试失败。
    QJsonObject node(const QString& name)
    {
        const QJsonArray nodes = payload(call(QStringLiteral("snapshot")))
            .value(QStringLiteral("nodes")).toArray(); // 本次真实已构造树。
        for (const QJsonValue& value : nodes) // 不依赖位置、展示语言或裸指针。
        {
            const QJsonObject candidate = value.toObject(); // 单节点描述。
            if (candidate.value(QStringLiteral("accessible_id")) == name
                || candidate.value(QStringLiteral("object_name")) == name)
            {
                return candidate;
            }
        }
        return {};
    }

    // objectId：只接受引擎登记产生的 ID，不向生产接口传 QWidget 指针。
    QString objectId(const QString& name)
    {
        const QString id = node(name).value(QStringLiteral("id")).toString(); // 最新可见范围中登记的身份。
        if (!id.isEmpty())
        {
            knownIds.insert(name, id);
        }
        return id;
    }

    // field：固定字段入口；confirm/submit 明确传入，不自动跳过确认。
    QJsonObject field(const QString& name, const QJsonValue& value, bool submit = false, bool confirm = true)
    {
        return call(QStringLiteral("set_field"), {{QStringLiteral("object_id"), objectId(name)},
            {QStringLiteral("value"), value}, {QStringLiteral("submit"), submit},
            {QStringLiteral("confirmed"), confirm}});
    }

    // act：按固定动作名请求；回执只表示排队调用，真实结果由槽记录。
    QJsonObject act(const QString& name, const QString& action = QStringLiteral("press"), bool confirm = true)
    {
        const QString current = objectId(name); // 模态/菜单可能限制当前快照范围。
        const QString id = current.isEmpty() ? knownIds.value(name) : current; // 使用之前真实登记的 ID 检查现场门禁。
        return call(QStringLiteral("action"), {{QStringLiteral("object_id"), id},
            {QStringLiteral("action"), action}, {QStringLiteral("confirmed"), confirm}});
    }
};

// settle：对 accepted 回执推动事件循环；返回真实 operation_status data。
QJsonObject settle(Fixture& fixture, const QJsonObject& response)
{
    const QString operation = payload(response).value(QStringLiteral("operation_id")).toString(); // 调用编号。
    if (operation.isEmpty())
    {
        drain();
        return payload(response);
    }
    drain();
    return payload(fixture.call(QStringLiteral("operation_status"),
        {{QStringLiteral("operation_id"), operation}}));
}

// discovery：验证列目录无副作用、显式打开、稳定对象身份与快照预算。
void discovery(Fixture& fixture)
{
    check(success(fixture.call(QStringLiteral("pages"))), "pages uses production callback");
    check(fixture.openings == 0, "listing pages does not construct lazy content");
    const QString initialId = fixture.objectId(QStringLiteral("fixture.run")); // 初始运行按钮 ID。
    check(!initialId.isEmpty(), "snapshot exposes stable machine identifier");
    fixture.button->setText(QStringLiteral("运行"));
    check(fixture.objectId(QStringLiteral("fixture.run")) == initialId, "translated text preserves node identity");
    // 图标按钮常只提供 tooltip；元数据回退必须让 MCP 能判断原按钮的用途。
    const QString originalText = fixture.button->text(); // 用例结束后恢复已有展示文字。
    const QString originalTooltip = fixture.button->toolTip(); // 保存原提示，避免影响后续操作测试。
    const QString originalAccessibleName = fixture.button->accessibleName(); // 模拟真实仅图标的空名称按钮。
    fixture.button->setText({});
    fixture.button->setAccessibleName({});
    fixture.button->setToolTip(QStringLiteral("Run synthetic fixture"));
    const QJsonObject iconNode = fixture.node(QStringLiteral("fixture.run")); // 原对象的真实生产快照。
    check(iconNode.value(QStringLiteral("name")) == QStringLiteral("Run synthetic fixture")
        && iconNode.value(QStringLiteral("description")) == QStringLiteral("Run synthetic fixture"),
        "icon-only button tooltip supplies discoverable name and description");
    fixture.button->setText(originalText);
    fixture.button->setToolTip(originalTooltip);
    fixture.button->setAccessibleName(originalAccessibleName);
    check(fixture.openings == 0, "snapshot does not construct unopened lazy pages");
    const QJsonObject opened = fixture.call(QStringLiteral("open_page"),
        {{QStringLiteral("page_id"), QStringLiteral("fixture.lazy")}}); // 唯一允许构造的入口。
    check(success(opened), "explicit lazy page open succeeds");
    check(fixture.openings == 1, "lazy page is constructed exactly once");
    fixture.call(QStringLiteral("open_page"), {{QStringLiteral("page_id"), QStringLiteral("fixture.lazy")}});
    check(fixture.openings == 1, "reopening page reuses original controller state");
    check(rejected(fixture.call(QStringLiteral("open_page"),
        {{QStringLiteral("page_id"), QStringLiteral("fixture.missing")}})), "unknown page is rejected");

    // 节点与深度限制必须在真实树上有效，不只检查参数范围。
    const QJsonObject small = fixture.call(QStringLiteral("snapshot"),
        {{QStringLiteral("max_nodes"), 3}, {QStringLiteral("depth"), 2}}); // 小预算快照。
    check(success(small), "bounded snapshot succeeds");
    check(payload(small).value(QStringLiteral("nodes")).toArray().size() <= 3, "snapshot enforces node budget");
    check(rejected(fixture.call(QStringLiteral("snapshot"), {{QStringLiteral("max_nodes"), 1.5}})), "fractional node budget is rejected");
    check(rejected(fixture.call(QStringLiteral("snapshot"), {{QStringLiteral("depth"), -1}})), "negative depth is rejected");
    const QByteArray snapshotBytes = QJsonDocument(fixture.call(QStringLiteral("snapshot"))).toJson(); // 密码泄漏检测。
    check(!snapshotBytes.contains("fixture-secret-value"), "snapshot masks synthetic password value");
}

// failedCall：同步拒绝或排队后的失败都必须有明确机器码，不能以 accepted 冒充完成。
bool failedCall(Fixture& fixture, const QJsonObject& response, const QString& code = {})
{
    if (rejected(response, code))
    {
        return true;
    }
    const QJsonObject operation = settle(fixture, response); // 真实调用回执状态。
    return operation.value(QStringLiteral("state")) == QStringLiteral("failed")
        && (code.isEmpty() || operation.value(QStringLiteral("error_code")) == code);
}

// fields：检测真实字段语义、validator、索引边界以及禁止未经确认的改值。
void fields(Fixture& fixture)
{
    check(rejected(fixture.field(QStringLiteral("fixture.integer"), QStringLiteral("42"), false, false),
        QStringLiteral("confirmation_required")), "field edits require explicit confirmation");
    check(fixture.integerEdit->text() == QStringLiteral("10"), "unconfirmed edits leave field unchanged");
    const QJsonObject edited = fixture.field(QStringLiteral("fixture.integer"), QStringLiteral("42")); // 有效用户编辑。
    check(edited.value(QStringLiteral("status")) == QStringLiteral("accepted"), "field edit returns queued receipt");
    check(fixture.edits == 0, "field edit has not run at receipt time");
    check(settle(fixture, edited).value(QStringLiteral("state")) == QStringLiteral("dispatched"), "edit is eventually dispatched");
    check(fixture.integerEdit->text() == QStringLiteral("42"), "real line edit receives value");
    check(fixture.edits == 1 && fixture.lastEdited == QStringLiteral("42"), "edit preserves textEdited semantics");
    check(fixture.submits == 0, "ordinary edit does not submit implicitly");

    // 无效输入必须由原 validator 拒绝；原文本和值语义不能悄悄被破坏。
    check(failedCall(fixture, fixture.field(QStringLiteral("fixture.integer"), QStringLiteral("999"))), "validator rejects out-of-range input");
    check(fixture.integerEdit->text() == QStringLiteral("42"), "rejected validator input preserves text");
    check(failedCall(fixture, fixture.field(QStringLiteral("fixture.integer"), 55), QStringLiteral("invalid_value")), "line edit rejects numeric JSON value");
    const QJsonObject submitted = fixture.field(QStringLiteral("fixture.integer"), QStringLiteral("43"), true); // 显式 Enter。
    settle(fixture, submitted);
    check(fixture.submits == 1, "submit sends original Return signal");
    check(fixture.integerEdit->hasAcceptableInput(), "submitted value satisfies original validator");
    check(failedCall(fixture, fixture.field(QStringLiteral("fixture.integer"), QStringLiteral("44\n45"))), "single-line field rejects newline payload");

    // Combo 用户激活是独立业务入口，值变化之外还必须观察真实 activated。
    settle(fixture, fixture.field(QStringLiteral("fixture.combo"), 1));
    check(fixture.combo->currentIndex() == 1, "combo accepts validated item index");
    check(fixture.activations == 1, "combo edit preserves activated signal");
    settle(fixture, fixture.field(QStringLiteral("fixture.combo"), QStringLiteral("Gamma")));
    check(fixture.combo->currentText() == QStringLiteral("Gamma"), "combo accepts exact existing item text");
    check(fixture.activations == 2, "text selection also activates original combo slot");
    check(failedCall(fixture, fixture.field(QStringLiteral("fixture.combo"), 99)), "combo rejects invalid index");
    check(fixture.combo->currentIndex() == 2, "invalid index does not select an unintended item");
    check(failedCall(fixture, fixture.field(QStringLiteral("fixture.combo"), QStringLiteral("Absent"))), "non-editable combo rejects unknown text");
    auto* disabledItem = qobject_cast<QStandardItemModel*>(fixture.combo->model()); // Qt 默认项模型。
    disabledItem->item(0)->setEnabled(false);
    check(failedCall(fixture, fixture.field(QStringLiteral("fixture.combo"), 0)), "disabled combo item cannot be selected through bridge");
    settle(fixture, fixture.field(QStringLiteral("fixture.editable_combo"), QStringLiteral("Custom")));
    check(fixture.editableCombo->currentText() == QStringLiteral("Custom"), "editable combo uses native editor path");

    // 数值与 Tab 请求必须拒绝越界，而不是依靠 Qt 的静默 clamp 或默认零值。
    settle(fixture, fixture.field(QStringLiteral("fixture.number"), 61));
    check(fixture.number->value() == 61, "spin box receives bounded integer");
    check(failedCall(fixture, fixture.field(QStringLiteral("fixture.number"), 101)), "spin box rejects out-of-range value");
    check(fixture.number->value() == 61, "rejected spin value is not silently clamped");
    check(failedCall(fixture, fixture.field(QStringLiteral("fixture.number"), 1.5)), "integer spin rejects fractional number");
    settle(fixture, fixture.field(QStringLiteral("fixture.tabs"), 1));
    check(fixture.tabs->currentIndex() == 1, "tab adapter selects permitted page");
    fixture.tabs->setTabEnabled(0, false);
    check(failedCall(fixture, fixture.field(QStringLiteral("fixture.tabs"), 0)), "disabled tab cannot be activated");
    check(fixture.tabs->currentIndex() == 1, "failed tab request preserves current tab");
    check(failedCall(fixture, fixture.field(QStringLiteral("fixture.text"), QStringLiteral("overwrite"))), "read-only text cannot be overwritten");
}

// actions：真实 clicked/QAction 连接验证生命周期、禁用/模态边界和调用回执。
void actions(Fixture& fixture)
{
    check(rejected(fixture.act(QStringLiteral("fixture.run"), QStringLiteral("press"), false),
        QStringLiteral("confirmation_required")), "button action requires confirmation");
    const QJsonObject accepted = fixture.act(QStringLiteral("fixture.run")); // 排队动作不应立刻点击。
    check(accepted.value(QStringLiteral("status")) == QStringLiteral("accepted"), "button action returns accepted receipt");
    check(fixture.clicks == 0, "accepted receipt is not a completed button action");
    const QString operationId = payload(accepted).value(QStringLiteral("operation_id")).toString(); // 固定回执身份。
    check(!operationId.isEmpty(), "accepted action has operation identity");
    const QJsonObject pending = payload(fixture.call(QStringLiteral("operation_status"),
        {{QStringLiteral("operation_id"), operationId}})); // 派发前的真实状态。
    check(pending.value(QStringLiteral("state")) == QStringLiteral("queued"), "receipt initially reports queued state");
    const QJsonObject dispatched = settle(fixture, accepted); // 推动事件循环并取得派发回执。
    check(fixture.clicks == 1, "original clicked slot executes exactly once");
    check(dispatched.value(QStringLiteral("state")) == QStringLiteral("dispatched"), "status records UI dispatch");
    check(dispatched.value(QStringLiteral("business_completion")) == QStringLiteral("unknown"), "dispatch does not claim business completion");
    check(failedCall(fixture, fixture.act(QStringLiteral("fixture.disabled"))), "disabled button cannot be invoked");
    check(rejected(fixture.act(QStringLiteral("fixture.run"), QStringLiteral("setEnabled"))), "arbitrary reflective method is rejected");

    // 请求登记之后按钮失效的窗口必须再检查，不能靠第一次查询的状态。
    const QJsonObject raced = fixture.act(QStringLiteral("fixture.run")); // 此时按钮仍可用。
    fixture.button->setEnabled(false);
    check(failedCall(fixture, raced), "queued action rechecks disabled state before dispatch");
    check(fixture.clicks == 1, "disabled queued action never triggers business slot");
    fixture.button->setEnabled(true);
    auto* temporary = new QPushButton(QStringLiteral("Temporary"), &fixture.host); // 可销毁目标。
    identify(temporary, QStringLiteral("fixture.temporary"));
    fixture.layout->addWidget(temporary);
    temporary->show();
    const QString obsoleteId = fixture.objectId(QStringLiteral("fixture.temporary")); // 销毁前有效身份。
    const QJsonObject doomed = fixture.act(QStringLiteral("fixture.temporary")); // 等待中的回执。
    delete temporary;
    check(failedCall(fixture, doomed), "destroyed queued target is rejected at dispatch");
    auto* replacement = new QPushButton(QStringLiteral("Replacement"), &fixture.host); // 相同标识新对象。
    identify(replacement, QStringLiteral("fixture.temporary"));
    fixture.layout->addWidget(replacement);
    replacement->show();
    check(fixture.objectId(QStringLiteral("fixture.temporary")) != obsoleteId, "recreated object does not reuse external identity");
    check(rejected(fixture.call(QStringLiteral("action"), {{QStringLiteral("object_id"), obsoleteId},
        {QStringLiteral("action"), QStringLiteral("press")}, {QStringLiteral("confirmed"), true}})), "stale object identity remains invalid");

    // 使用真实 application-modal 窗口，桥只允许它自身的受管控件交互。
    QDialog modal(&fixture.host); // 模态窗口属于夹具，不访问安全桌面。
    identify(&modal, QStringLiteral("fixture.modal"));
    modal.setStyleSheet(QStringLiteral("QDialog { background:#ffffff; color:#202020; }"));
    modal.setWindowModality(Qt::ApplicationModal);
    modal.setAttribute(Qt::WA_DontShowOnScreen);
    QVBoxLayout modalLayout(&modal); // 模态窗口内容，仅用于已确认点击。
    QPushButton modalButton(QStringLiteral("Modal action"), &modal); // 合成模态动作。
    identify(&modalButton, QStringLiteral("fixture.modal_button"));
    modalLayout.addWidget(&modalButton);
    int modalClicks = 0; // 真实模态槽调用次数。
    QObject::connect(&modalButton, &QPushButton::clicked, &modal, [&modalClicks]() { ++modalClicks; });
    modal.show();
    drain();
    check(QApplication::activeModalWidget() == &modal, "fixture enters real Qt modal state");
    check(rejected(fixture.act(QStringLiteral("fixture.run")), QStringLiteral("modal_blocked")), "host action is blocked by current modal");
    check(rejected(fixture.call(QStringLiteral("open_page"),
        {{QStringLiteral("page_id"), QStringLiteral("fixture.main")}}), QStringLiteral("modal_blocked")), "navigation preserves modal ownership");
    settle(fixture, fixture.act(QStringLiteral("fixture.modal_button")));
    check(modalClicks == 1, "owned modal button remains operable");
    modal.hide();
    drain();

    // 菜单每次打开时创建 QAction；action ID 只在对应菜单对象存活时有效。
    QObject::connect(fixture.menu.get(), &QMenu::aboutToShow, &fixture.host, [&fixture]()
    {
        if (fixture.menu->actions().isEmpty())
        {
            QAction* dynamic = fixture.menu->addAction(QStringLiteral("Dynamic")); // 本次实际菜单动作。
            dynamic->setObjectName(QStringLiteral("fixture.dynamic_action"));
            QObject::connect(dynamic, &QAction::triggered, &fixture.host, [&fixture]() { ++fixture.menuTriggers; });
        }
    });
    fixture.menu->popup(QPoint(20, 20));
    drain();
    const QString dynamicId = fixture.objectId(QStringLiteral("fixture.dynamic_action")); // 无父菜单关联动作。
    check(!dynamicId.isEmpty(), "snapshot discovers dynamically constructed associated menu action");
    settle(fixture, fixture.act(QStringLiteral("fixture.dynamic_action"), QStringLiteral("press")));
    check(fixture.menuTriggers == 1, "registered dynamic QAction triggers original slot");
    fixture.menu->hide();
    fixture.menu->clear();
    check(rejected(fixture.call(QStringLiteral("action"), {{QStringLiteral("object_id"), dynamicId},
        {QStringLiteral("action"), QStringLiteral("press")}, {QStringLiteral("confirmed"), true}})), "deleted menu action ID cannot target later menus");

    // QMenu::exec 的返回值也是业务入口，单纯 triggered 信号不足以证明操作完成。
    QAction* expected = fixture.menu->addAction(QStringLiteral("Nested selection")); // 原 exec 调用期望返回的动作。
    expected->setObjectName(QStringLiteral("fixture.exec_action"));
    int nestedTriggers = 0; // 此动作只允许被原生菜单激活一次。
    QObject::connect(expected, &QAction::triggered, &fixture.host, [&nestedTriggers]() { ++nestedTriggers; });
    QTimer watchdog; // 局部看门狗，返回后停止，不能在后续用例误关菜单。
    watchdog.setSingleShot(true);
    QObject::connect(&watchdog, &QTimer::timeout, fixture.menu.get(), &QMenu::close);
    watchdog.start(2500);
    QTimer::singleShot(0, &fixture.host, [&fixture]()
    {
        fixture.act(QStringLiteral("fixture.exec_action")); // 第一轮嵌套循环再快照/请求，不猜毫秒数。
    });
    QAction* chosen = fixture.menu->exec(QPoint(20, 20)); // 真正阻塞式原菜单，返回 QAction 选择。
    watchdog.stop();
    drain(); // 即使平台提前返回，也排空单层回调，不能留下栈引用。
    check(chosen == expected, "native menu action supplies QMenu::exec chosen result");
    check(nestedTriggers == 1, "native menu press triggers QAction exactly once");
    check(!fixture.menu->isVisible(), "native menu choice closes original popup");
    fixture.menu->clear();

    // 级联菜单先通过 show_menu 打开，再通过原生 press 把子动作送回原 exec。
    QMenu* submenu = fixture.menu->addMenu(QStringLiteral("Submenu")); // 原菜单所属子菜单。
    submenu->setStyleSheet(fixture.menu->styleSheet());
    submenu->menuAction()->setObjectName(QStringLiteral("fixture.submenu_action"));
    QAction* childAction = submenu->addAction(QStringLiteral("Child")); // 原 exec 等待的子动作。
    childAction->setObjectName(QStringLiteral("fixture.submenu_child"));
    int childTriggers = 0; // 子动作真实激活次数。
    QObject::connect(childAction, &QAction::triggered, &fixture.host, [&childTriggers]() { ++childTriggers; });
    QTimer menuDriver; // 单层零间隔事件驱动器，生命周期覆盖原 exec。
    bool submenuRequested = false; // 每个原生菜单请求最多提交一次。
    bool childRequested = false; // 仅子菜单实际可见后提交子动作。
    QObject::connect(&menuDriver, &QTimer::timeout, &fixture.host, [&]()
    {
        if (!submenuRequested)
        {
            submenuRequested = true;
            fixture.act(QStringLiteral("fixture.submenu_action"), QStringLiteral("show_menu"));
        }
        else if (submenu->isVisible() && !childRequested)
        {
            childRequested = true;
            fixture.act(QStringLiteral("fixture.submenu_child"));
        }
    });
    watchdog.start(2500);
    menuDriver.start(0);
    QAction* chosenChild = fixture.menu->exec(QPoint(20, 20)); // 原父菜单必须接到真实子菜单选择。
    menuDriver.stop();
    watchdog.stop();
    drain();
    check(chosenChild == childAction, "submenu activation preserves original parent exec result");
    check(childTriggers == 1, "submenu child action triggers exactly once");
    check(!submenu->isVisible() && !fixture.menu->isVisible(), "native submenu selection closes original chain");
    fixture.menu->clear();

    // 旧无父菜单仍存活时，setMenu 更换也必须撤销它的宿主关联权限。
    QAction* oldAction = fixture.menu->addAction(QStringLiteral("Detached action")); // 随后被解除关联的动作。
    oldAction->setObjectName(QStringLiteral("fixture.detached_action"));
    const QString detachedId = fixture.objectId(QStringLiteral("fixture.detached_action")); // 更换前真实登记的 ID。
    QMenu replaced; // 新关联菜单只在当前用例存在。
    replaced.setStyleSheet(fixture.menu->styleSheet());
    fixture.menuButton->setMenu(&replaced);
    check(!detachedId.isEmpty(), "old menu action had a registered identity before replacement");
    check(rejected(fixture.call(QStringLiteral("action"), {{QStringLiteral("object_id"), detachedId},
        {QStringLiteral("action"), QStringLiteral("press")}, {QStringLiteral("confirmed"), true}}),
        QStringLiteral("unknown_object")), "menu replacement revokes old parentless menu identities");
    fixture.menuButton->setMenu(fixture.menu.get());
    fixture.menu->clear();
}

// tables：真实模型排序、dataChanged、模型替换都必须使旧选择回执失效。
void tables(Fixture& fixture)
{
    const QString tableId = fixture.objectId(QStringLiteral("fixture.table")); // 当前 view 的桥身份。
    const QJsonObject first = fixture.call(QStringLiteral("read_table"),
        {{QStringLiteral("object_id"), tableId}, {QStringLiteral("row_count"), 1},
            {QStringLiteral("column_count"), 2}}); // 窄分页读取，防止全量导出。
    check(success(first), "table range read succeeds");
    const QJsonObject tableData = payload(first); // 原始返回数据用于覆盖字段校验。
    check(tableData.value(QStringLiteral("rows")).toArray().size() == 1, "table respects requested row budget");
    check(tableData.value(QStringLiteral("headers")).toArray().size() == 2, "table respects requested column range");
    check(tableData.value(QStringLiteral("row_total")).toInt() == 2, "table reports loaded model row total");
    const QString oldRevision = tableData.value(QStringLiteral("revision")).toString(); // 读取时模型代次。
    check(!oldRevision.isEmpty(), "table returns opaque model revision");
    QJsonObject selected{{QStringLiteral("object_id"), tableId}, {QStringLiteral("row"), 1},
        {QStringLiteral("revision"), oldRevision}, {QStringLiteral("confirmed"), true}}; // 必填快照证明。
    settle(fixture, fixture.call(QStringLiteral("select_row"), selected));
    check(fixture.table->selectionModel()->isRowSelected(1, {}), "selection uses current read revision");

    // 排序前保存的 row=1 不能被解释成排序后的另一实体。
    fixture.model->sort(0, Qt::DescendingOrder);
    check(failedCall(fixture, fixture.call(QStringLiteral("select_row"), selected),
        QStringLiteral("stale_revision")), "sort invalidates previously read row identity");
    const QJsonObject sorted = payload(fixture.call(QStringLiteral("read_table"),
        {{QStringLiteral("object_id"), tableId}})); // 重读取得新 revision。
    check(sorted.value(QStringLiteral("revision")) != oldRevision, "layoutChanged advances revision");
    selected.insert(QStringLiteral("revision"), sorted.value(QStringLiteral("revision")));
    const QJsonObject queued = fixture.call(QStringLiteral("select_row"), selected); // 排队后仍可能刷新。
    fixture.model->setData(fixture.model->index(0, 2), QStringLiteral("changed"));
    check(failedCall(fixture, queued, QStringLiteral("stale_revision")), "selection rechecks data generation at dispatch");
    const QJsonObject changed = payload(fixture.call(QStringLiteral("read_table"),
        {{QStringLiteral("object_id"), tableId}})); // dataChanged 新代次。
    check(changed.value(QStringLiteral("revision")) != sorted.value(QStringLiteral("revision")), "dataChanged advances revision");

    // 相同 view 的模型替换也必须换身份，即使新模型行列完全相同。
    QStandardItemModel replacement(2, 3, &fixture.host); // 生命周期受当前套件约束。
    fixture.table->setModel(&replacement);
    selected.insert(QStringLiteral("revision"), changed.value(QStringLiteral("revision")));
    check(failedCall(fixture, fixture.call(QStringLiteral("select_row"), selected),
        QStringLiteral("stale_revision")), "new model rejects old model revision");
    fixture.table->setModel(fixture.model);
    check(rejected(fixture.call(QStringLiteral("select_row"), {{QStringLiteral("object_id"), tableId},
        {QStringLiteral("row"), 0}, {QStringLiteral("confirmed"), true}})), "select_row requires revision parameter");
    check(rejected(fixture.call(QStringLiteral("read_table"), {{QStringLiteral("object_id"), tableId},
        {QStringLiteral("row_start"), -1}})), "negative row offset is rejected");
    check(rejected(fixture.call(QStringLiteral("read_table"), {{QStringLiteral("object_id"), tableId},
        {QStringLiteral("row_count"), 200}, {QStringLiteral("column_count"), 64}})), "oversized table cell budget is rejected");
    check(rejected(fixture.call(QStringLiteral("read_table"), {{QStringLiteral("object_id"), tableId},
        {QStringLiteral("parent_rows"), QJsonArray{true}}})), "non-integer hierarchy path is rejected");

    // clear 会发出 modelReset；即使旧行号还在范围内，也不能继续复用读取代次。
    const QJsonObject beforeReset = payload(fixture.call(QStringLiteral("read_table"),
        {{QStringLiteral("object_id"), tableId}})); // reset 前有效 revision。
    fixture.model->clear();
    fixture.model->setRowCount(2);
    fixture.model->setColumnCount(3);
    selected.insert(QStringLiteral("revision"), beforeReset.value(QStringLiteral("revision")));
    check(failedCall(fixture, fixture.call(QStringLiteral("select_row"), selected),
        QStringLiteral("stale_revision")), "modelReset invalidates prior selection revision");
}

// textAndParams：只读分页和恶意 JSON 参数必须受预算/类型/归属检查约束。
void textAndParams(Fixture& fixture)
{
    const QString textId = fixture.objectId(QStringLiteral("fixture.text")); // 实际只读编辑器身份。
    const QJsonObject part = fixture.call(QStringLiteral("read_text"),
        {{QStringLiteral("object_id"), textId}, {QStringLiteral("offset"), 5},
            {QStringLiteral("length"), 9}}); // 明确 UTF-16 范围。
    check(success(part), "bounded text read succeeds");
    check(payload(part).value(QStringLiteral("text")) == QStringLiteral("56789abcd"), "text range returns actual requested content");
    check(payload(part).value(QStringLiteral("length")).toInt() == 9, "text reply stays within read budget");
    check(payload(part).value(QStringLiteral("truncated")).toBool(), "partial text read reports truncation");
    check(rejected(fixture.call(QStringLiteral("read_text"), {{QStringLiteral("object_id"), textId},
        {QStringLiteral("length"), 65537}}), QStringLiteral("invalid_params")), "text budget rejects excessive length");
    check(rejected(fixture.call(QStringLiteral("read_text"), {{QStringLiteral("object_id"), textId},
        {QStringLiteral("offset"), 0.5}}), QStringLiteral("invalid_params")), "text offset rejects fractional value");
    const QJsonObject beyond = fixture.call(QStringLiteral("read_text"), {{QStringLiteral("object_id"), textId},
        {QStringLiteral("offset"), 2147483647}, {QStringLiteral("length"), 65536}}); // 溢出边界不得读取错误位置。
    check(success(beyond) && payload(beyond).value(QStringLiteral("text")).toString().isEmpty(), "extreme offset safely returns empty tail");
    check(rejected(fixture.call(QStringLiteral("read_text"),
        {{QStringLiteral("object_id"), fixture.objectId(QStringLiteral("fixture.password"))}}),
        QStringLiteral("sensitive_field")), "password read never reveals synthetic secret");

    // 任意反射、错误 ID 类型和伪造布尔值不能成为有效控件操作。
    check(rejected(fixture.call(QStringLiteral("invokeMethod"), {{QStringLiteral("object_id"), textId},
        {QStringLiteral("method"), QStringLiteral("deleteLater")}}), QStringLiteral("unknown_method")), "arbitrary method entry is absent");
    check(rejected(fixture.call(QStringLiteral("action"), {{QStringLiteral("object_id"), 0},
        {QStringLiteral("action"), QStringLiteral("press")}, {QStringLiteral("confirmed"), true}}),
        QStringLiteral("invalid_params")), "numeric pointer-like ID is rejected");
    check(rejected(fixture.call(QStringLiteral("action"),
        {{QStringLiteral("object_id"), fixture.objectId(QStringLiteral("fixture.run"))},
            {QStringLiteral("action"), QStringLiteral("press")}, {QStringLiteral("confirmed"), QStringLiteral("true")}}),
        QStringLiteral("confirmation_required")), "string confirmation is not a boolean grant");
    check(rejected(fixture.call(QStringLiteral("set_field"), {{QStringLiteral("object_id"), textId},
        {QStringLiteral("value"), QJsonObject{{QStringLiteral("enabled"), true}}},
        {QStringLiteral("confirmed"), true}})), "object-valued property payload is rejected");
    check(rejected(fixture.call(QStringLiteral("operation_status"),
        {{QStringLiteral("operation_id"), QStringLiteral("spoofed:operation")}}),
        QStringLiteral("unknown_operation")), "unregistered operation identity is rejected");
    check(rejected(fixture.call(QStringLiteral("snapshot"),
        {{QStringLiteral("page_id"), QStringLiteral("fixture.never_opened")}}),
        QStringLiteral("page_not_opened")), "unopened page snapshot does not invoke page factory");

    // 与宿主无父链关系的顶层窗口不能因同一 QApplication 就成为可操作目标。
    QWidget foreign; // 明确位于访问边界之外的合成窗口。
    identify(&foreign, QStringLiteral("fixture.foreign"));
    foreign.setAttribute(Qt::WA_DontShowOnScreen);
    foreign.show();
    check(fixture.objectId(QStringLiteral("fixture.foreign")).isEmpty(), "snapshot excludes unrelated top-level window");
    foreign.hide();
    QString workerCode; // 独立线程只能得到拒绝码，不读取控制器内部对象。
    std::thread worker([&fixture, &workerCode]()
    {
        workerCode = fixture.call(QStringLiteral("pages")).value(QStringLiteral("error")).toObject()
            .value(QStringLiteral("code")).toString();
    });
    worker.join();
    check(workerCode == QStringLiteral("wrong_thread"), "engine rejects direct worker-thread Qt access");
}

// reentrancy：原业务槽同步销毁字段/Combo 时，适配器必须停止使用裸目标。
void reentrancy(Fixture& fixture)
{
    auto* doomedEdit = new QLineEdit(&fixture.host); // textEdited 槽主动关闭字段。
    identify(doomedEdit, QStringLiteral("fixture.self_destroy_edit"));
    fixture.layout->addWidget(doomedEdit);
    doomedEdit->show();
    QObject::connect(doomedEdit, &QLineEdit::textEdited, &fixture.host,
        [doomedEdit](const QString&) { delete doomedEdit; });
    const QJsonObject editReceipt = fixture.field(QStringLiteral("fixture.self_destroy_edit"),
        QStringLiteral("close")); // 固定编辑路径触发真实信号。
    settle(fixture, editReceipt);
    check(fixture.objectId(QStringLiteral("fixture.self_destroy_edit")).isEmpty(), "textEdited self-destruction invalidates object safely");

    auto* doomedCombo = new QComboBox(&fixture.host); // activated 槽销毁整个组合框。
    identify(doomedCombo, QStringLiteral("fixture.self_destroy_combo"));
    doomedCombo->addItems({QStringLiteral("A"), QStringLiteral("B")});
    fixture.layout->addWidget(doomedCombo);
    doomedCombo->show();
    QObject::connect(doomedCombo, &QComboBox::activated, &fixture.host,
        [doomedCombo](int) { delete doomedCombo; });
    settle(fixture, fixture.field(QStringLiteral("fixture.self_destroy_combo"), 1));
    check(fixture.objectId(QStringLiteral("fixture.self_destroy_combo")).isEmpty(), "activated self-destruction invalidates object safely");

    // 打开回调删除控制器时，调用栈只能使用已复制闭包与 QPointer。
    std::unique_ptr<ks::ui::McpUiController> navigation; // 独立回调寿命测试，不破坏主套件控制器。
    navigation = std::make_unique<ks::ui::McpUiController>(&fixture.host,
        []() { return QJsonArray{}; }, [&navigation, &fixture](const QString&)
        {
            navigation.reset();
            return &fixture.host;
        });
    ks::ui::McpUiController* active = navigation.get(); // 仅用于一次同步调用，返回后不再解引用。
    const QJsonObject destroyed = active->request(QStringLiteral("open_page"),
        {{QStringLiteral("page_id"), QStringLiteral("fixture.destroy_controller")}});
    check(!navigation && rejected(destroyed), "navigation callback can destroy controller without using cleared closure");
}

// labelAdapter：返回只读合成字段，真实类别由注册表选择，不读取客户端方法/类名。
QJsonObject labelAdapter(QObject* object, const QJsonObject&)
{
    const auto* label = qobject_cast<QLabel*>(object); // 控制器已经按 QLabel 实际元对象分派。
    return {{QStringLiteral("status"), QStringLiteral("ok")},
        {QStringLiteral("data"), QJsonObject{{QStringLiteral("kind"), QStringLiteral("label")},
            {QStringLiteral("text"), label ? label->text().left(64) : QString{}}}}};
}

// adapters：注册属于程序内部；客户端参数不能变成任意反射调用。
void adapters(Fixture& fixture)
{
    fixture.controller->registerReadAdapter(QByteArrayLiteral("QWidget"),
        [](QObject*, const QJsonObject&)
        {
            return QJsonObject{{QStringLiteral("status"), QStringLiteral("ok")},
                {QStringLiteral("data"), QJsonObject{{QStringLiteral("kind"), QStringLiteral("base")}}}};
        });
    fixture.controller->registerReadAdapter(QByteArrayLiteral("QLabel"), labelAdapter);
    const QString labelId = fixture.objectId(QStringLiteral("fixture.custom_label")); // 已登记的受管 QLabel。
    const QJsonObject read = fixture.call(QStringLiteral("read_custom"),
        {{QStringLiteral("object_id"), labelId},
            {QStringLiteral("class_name"), QStringLiteral("QWidget")},
            {QStringLiteral("options"), QJsonObject{{QStringLiteral("method"), QStringLiteral("deleteLater")}}}});
    check(success(read), "registered read-only adapter executes");
    check(payload(read).value(QStringLiteral("kind")) == QStringLiteral("label"), "real derived Qt class wins over client-supplied class name");
    check(fixture.customLabel->text() == QStringLiteral("Adapter sample"), "custom read cannot treat options as a reflective mutation");
    check(rejected(fixture.call(QStringLiteral("read_custom"),
        {{QStringLiteral("object_id"), labelId}, {QStringLiteral("options"), QJsonArray{1}}}),
        QStringLiteral("invalid_params")), "custom read options must be object-valued");
    fixture.controller->registerReadAdapter(QByteArrayLiteral("QLabel"), {});
    const QJsonObject inherited = fixture.call(QStringLiteral("read_custom"),
        {{QStringLiteral("object_id"), labelId}}); // 明确撤销派生类后才允许实际继承链回退。
    check(payload(inherited).value(QStringLiteral("kind")) == QStringLiteral("base"), "adapter registration uses actual superclass fallback");
    fixture.controller->registerReadAdapter(QByteArrayLiteral("QWidget"), {});
    check(rejected(fixture.call(QStringLiteral("read_custom"), {{QStringLiteral("object_id"), labelId}}),
        QStringLiteral("unsupported_control")), "unregistered custom reader is unavailable");
}

// closeFixture：请求排队后关闭桥，必须取消动作且拒绝后续请求。
void closeFixture(Fixture& fixture)
{
    const int before = fixture.clicks; // 关闭前真实槽计数。
    fixture.act(QStringLiteral("fixture.run"));
    fixture.controller->close();
    drain();
    check(fixture.clicks == before, "close cancels pending UI dispatch");
    check(rejected(fixture.call(QStringLiteral("pages")), QStringLiteral("bridge_closed")), "closed controller rejects subsequent requests");
    fixture.model->setData(fixture.model->index(0, 0), QStringLiteral("post-close"));
    check(rejected(fixture.call(QStringLiteral("snapshot")), QStringLiteral("bridge_closed")), "model events cannot reactivate a closed bridge");
}

// serve：启动真实本地 IPC，描述文件只含夹具 PID/管道，不含令牌或用户数据。
// descriptorPath 由测试脚本给出，返回事件循环退出码；不创建持久服务。
int serve(Fixture& fixture, const QString& descriptorPath)
{
    ks::ui::McpLocalBridge bridge(&fixture.host, [&fixture]() { return fixture.pages(); },
        [&fixture](const QString& id) { return fixture.open(id); }); // 编译实际传输实现。
    bridge.registerReadAdapter(QByteArrayLiteral("QLabel"), labelAdapter); // 仅验证统一扩展传输，不加载 HexCanvas。
    if (!bridge.start())
    {
        std::fprintf(stderr, "FIXTURE_SERVER=FAIL\n");
        return 2;
    }
    QFile descriptor(descriptorPath); // 当前独立夹具的连接信息文件。
    if (!descriptor.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        return 3;
    }
    const QJsonObject connection{{QStringLiteral("schemaVersion"), 1},
        {QStringLiteral("pid"), static_cast<double>(QCoreApplication::applicationPid())},
        {QStringLiteral("endpoint"), bridge.endpoint()},
        {QStringLiteral("exe"), QCoreApplication::applicationFilePath()}}; // 只标识当前进程。
    descriptor.write(QJsonDocument(connection).toJson(QJsonDocument::Compact));
    descriptor.close();
    std::printf("FIXTURE_SERVER=READY\n");
    std::fflush(stdout);
    QTimer::singleShot(120000, &fixture.host, []() { qApp->quit(); }); // 兜底停止，不检查局部引用。
    const int result = qApp->exec(); // Python MCP 客户端驱动原控件与原信号。
    bridge.close();
    return result;
}

// serveDestroyingHost：独立 heap 宿主在 read_custom 回调中同步析构；只用于 IPC 重入验收。
// 返回事件循环退出码；不使用上方 stack fixture，避免把错误 delete 模式混入产品证明。
int serveDestroyingHost(const QString& descriptorPath)
{
    auto* host = new QWidget(); // 此模式的唯一 heap GUI 根，回调负责销毁。
    QPointer<QWidget> guardedHost(host); // 正常超时退出时才允许夹具清理剩余宿主。
    identify(host, QStringLiteral("fixture.destroy_host"));
    host->setAttribute(Qt::WA_DontShowOnScreen);
    auto* label = new QLabel(QStringLiteral("Destroy callback target"), host); // 仅用于触发实际受管适配器。
    identify(label, QStringLiteral("fixture.destroy_label"));
    auto* bridge = new ks::ui::McpLocalBridge(host,
        []() { return QJsonArray{}; }, [host](const QString&) { return host; }); // 随 heap 宿主同步析构。
    bridge->registerReadAdapter(QByteArrayLiteral("QLabel"),
        [host](QObject*, const QJsonObject&)
        {
            std::printf("DESTROY_HOST_ADAPTER=ENTERED\n");
            std::fflush(stdout);
            delete host; // QObject 父子链同时销毁真实 Core、控制器和活动 socket。
            return QJsonObject{{QStringLiteral("status"), QStringLiteral("ok")},
                {QStringLiteral("data"), QJsonObject{}}};
        });
    QObject::connect(host, &QObject::destroyed, qApp, []()
    {
        std::printf("DESTROY_HOST=1\n");
        std::fflush(stdout);
        QTimer::singleShot(0, qApp, []() { qApp->quit(); }); // 先让当前同步分派返回，再停止夹具。
    });
    host->show();
    if (!bridge->start())
    {
        delete host;
        return 2;
    }
    QFile descriptor(descriptorPath); // 该子进程专属连接信息，只有 PID 与端点。
    if (!descriptor.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        delete host;
        return 3;
    }
    descriptor.write(QJsonDocument(QJsonObject{
        {QStringLiteral("schemaVersion"), 1},
        {QStringLiteral("pid"), static_cast<double>(QCoreApplication::applicationPid())},
        {QStringLiteral("endpoint"), bridge->endpoint()}}).toJson(QJsonDocument::Compact));
    descriptor.close();
    QTimer::singleShot(120000, qApp, []() { qApp->quit(); }); // 没有客户时也不能保留持久服务。
    const int result = qApp->exec(); // 官方 SDK 实际请求会进入上方同步销毁回调。
    if (guardedHost)
    {
        delete guardedHost.data();
    }
    return result;
}
}

// main：默认执行普通断言；--serve <descriptor> 只构造同一合成 GUI 提供 IPC 验证。
int main(int argc, char** argv)
{
    QApplication application(argc, argv); // 实际 Qt GUI 事件循环，测试不会修改 QSettings。
    application.setApplicationName(QStringLiteral("KSword-MCP-fixture"));
    if (argc == 3 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("--destroy-host"))
    {
        return serveDestroyingHost(QString::fromLocal8Bit(argv[2]));
    }
    Fixture fixture; // 全部被测对象只存在本进程，不打开任何系统目标。
    if (argc == 3 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("--serve"))
    {
        return serve(fixture, QString::fromLocal8Bit(argv[2]));
    }
    discovery(fixture);
    fields(fixture);
    actions(fixture);
    tables(fixture);
    textAndParams(fixture);
    reentrancy(fixture);
    adapters(fixture);
    closeFixture(fixture);
    std::printf("MCP_UI_CHECKS=%d\nMCP_UI_FAILURES=%d\nMCP_UI_RESULT=%s\n",
        g_checks, g_failures, g_failures == 0 ? "PASS" : "FAIL");
    return g_failures == 0 ? 0 : 1;
}
