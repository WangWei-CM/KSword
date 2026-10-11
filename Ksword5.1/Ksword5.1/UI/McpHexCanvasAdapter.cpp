#include "../Framework.h"
#include "./McpHexCanvasAdapter.h"
#include "./McpUiController.h"
#include "./McpLocalBridge.h"
#include "MemoryWorkbench/HexCanvas.h"
#include <QJsonArray>
#include <QJsonObject>
#include <cmath>
#include <limits>

namespace ks::ui
{
    namespace
    {
        // 地址保持十六进制字符串，避免 MCP/JSON 的双精度数值丢失高地址位。
        QString hexAddress(std::uint64_t address)
        {
            return QStringLiteral("0x") + QString::number(address, 16);
        }

        // 只接受无符号十进制或 0x 十六进制字符串，拒绝符号、空白和溢出。
        bool parseAddress(const QJsonValue& value, std::uint64_t* address)
        {
            if (!value.isString())
            {
                return false;
            }
            const QString text = value.toString(); // 受长度约束的外部地址表示。
            const bool hexadecimal = text.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive);
            const QString digits = hexadecimal ? text.mid(2) : text;
            if (digits.isEmpty() || digits.size() > (hexadecimal ? 16 : 20))
            {
                return false;
            }
            for (QChar character : digits)
            {
                const ushort code = character.unicode(); // 只允许 ASCII 数字与十六进制字母。
                const bool decimal = code >= '0' && code <= '9';
                const bool letter = hexadecimal && ((code >= 'a' && code <= 'f')
                    || (code >= 'A' && code <= 'F'));
                if (!decimal && !letter)
                {
                    return false;
                }
            }
            bool parsed = false; // Qt 在超过 uint64 范围时返回失败。
            const quint64 result = digits.toULongLong(&parsed, hexadecimal ? 16 : 10);
            if (parsed)
            {
                *address = result;
            }
            return parsed;
        }

        // 查询当前画布缓存状态；不调用 provider、fetchMore 或任何驱动读取。
        QJsonObject readCanvas(QObject* object, const QJsonObject& options)
        {
            auto* canvas = qobject_cast<HexCanvas*>(object); // 类注册后仍复核真实 Qt 类型。
            const QJsonObject invalid{{QStringLiteral("status"), QStringLiteral("error")},
                {QStringLiteral("data"), QJsonObject{}},
                {QStringLiteral("error"), QJsonObject{{QStringLiteral("code"), QStringLiteral("invalid_params")}}}};
            if (!canvas)
            {
                return invalid;
            }
            for (const QString& key : options.keys())
            {
                if (key != QStringLiteral("address") && key != QStringLiteral("length"))
                {
                    return invalid;
                }
            }
            std::uint64_t address = canvas->caretAddress(); // 默认读取当前插入点附近。
            if (options.contains(QStringLiteral("address"))
                && !parseAddress(options.value(QStringLiteral("address")), &address))
            {
                return invalid;
            }
            const QJsonValue lengthValue = options.value(QStringLiteral("length")); // 当前缓存切片预算。
            const double number = lengthValue.isUndefined() ? 16 : lengthValue.toDouble();
            if ((!lengthValue.isUndefined() && !lengthValue.isDouble())
                || !std::isfinite(number) || std::floor(number) != number || number < 1 || number > 256)
            {
                return invalid;
            }
            const int length = static_cast<int>(number); // 经验证的最多 256 个字节位置。
            QJsonArray cells; // 值为当前所见字节，包含暂存变更，与实际目标内存可能不同。
            bool overflow = false; // 地址到 uint64 上限时明确停止，绝不回绕到零。
            for (int offset = 0; offset < length; ++offset)
            {
                if (address > std::numeric_limits<std::uint64_t>::max() - static_cast<unsigned>(offset))
                {
                    overflow = true;
                    break;
                }
                const std::uint64_t current = address + static_cast<unsigned>(offset);
                const HexCanvas::CellState cell = canvas->cellStateAt(current);
                cells.append(QJsonObject{{QStringLiteral("address"), hexAddress(current)},
                    {QStringLiteral("in_space"), cell.inSpace},
                    {QStringLiteral("has_value"), cell.hasValue},
                    {QStringLiteral("value"), cell.hasValue ? QJsonValue(static_cast<int>(cell.value)) : QJsonValue{}},
                    {QStringLiteral("byte_state"), static_cast<int>(cell.byteState)},
                    {QStringLiteral("change_kind"), static_cast<int>(cell.change)},
                    {QStringLiteral("selected"), cell.selected},
                    {QStringLiteral("caret"), cell.isCaret},
                    {QStringLiteral("hex_text"), cell.hexText}});
            }
            const auto rangeJson = [](const std::optional<HexCanvas::AddressRange>& range) -> QJsonValue
            {
                // 区间为闭区间；未知地址空间与空选区用 null，而不是伪造 0..0。
                return range ? QJsonValue(QJsonObject{
                    {QStringLiteral("first"), hexAddress(range->first)},
                    {QStringLiteral("last"), hexAddress(range->last)}}) : QJsonValue{};
            };
            const QJsonObject data{{QStringLiteral("adapter"), QStringLiteral("hex_canvas")},
                {QStringLiteral("source"), QStringLiteral("loaded_gui_cache")},
                {QStringLiteral("requested_address"), hexAddress(address)},
                {QStringLiteral("requested_length"), length},
                {QStringLiteral("caret_address"), hexAddress(canvas->caretAddress())},
                {QStringLiteral("address_space"), rangeJson(canvas->addressSpaceRange())},
                {QStringLiteral("selection"), rangeJson(canvas->selectedRange())},
                {QStringLiteral("active_pane"), static_cast<int>(canvas->activePane())},
                {QStringLiteral("bytes_per_row"), canvas->bytesPerRow()},
                {QStringLiteral("editable"), canvas->isEditable()},
                {QStringLiteral("address_overflow"), overflow},
                {QStringLiteral("cells"), cells}};
            return QJsonObject{{QStringLiteral("status"), QStringLiteral("ok")},
                {QStringLiteral("data"), data}};
        }
    }

    // 首个自绘控件适配实例：只在内建代码中注册，客户端不能指定类名或函数地址。
    void registerMcpHexCanvasAdapter(McpLocalBridge* bridge)
    {
        if (bridge)
        {
            bridge->registerReadAdapter(HexCanvas::staticMetaObject.className(), readCanvas);
        }
    }
}
