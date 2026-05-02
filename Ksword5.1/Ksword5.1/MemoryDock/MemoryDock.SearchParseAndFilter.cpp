#include "MemoryDock.Internal.h"

// 说明：由原聚合式实现迁移为独立 .cpp，成员函数实现保持原样。
using namespace ksword::memory_dock_internal;

// ============================================================
// MemoryDock.SearchParseAndFilter.cpp
// 作用：承载搜索值解析与扫描区域收集逻辑。
// ============================================================

// ============================================================
// MemoryDock.SearchParseAndFilter.cpp（由原 Search 拆分）
// 作用：
// - 负责首次扫描、再次扫描、后台并发扫描与结果表刷新。
// - 聚焦“扫描规则解析 + 扫描任务执行 + 扫描状态管理”。
// ============================================================

bool MemoryDock::parseSearchPatternFromUi(
    ParsedSearchPattern& patternOut,
    QString& errorTextOut) const
{
    // 解析入口日志：记录当前类型索引与原始输入文本，便于复现解析异常。
    kLogEvent parsePatternStartEvent;
    dbg << parsePatternStartEvent
        << "[MemoryDock] parseSearchPatternFromUi: 开始解析, typeIndex="
        << m_searchTypeCombo->currentIndex()
        << ", rawText="
        << m_searchValueEdit->text().trimmed().toStdString()
        << eol;

    // 每次解析前都先清空输出结构体，避免上一轮数据污染本轮匹配规则。
    patternOut = ParsedSearchPattern{};

    // 数据类型来自下拉框 itemData，定义与 SearchValueType 枚举一一对应。
    const int typeIndex = m_searchTypeCombo->currentIndex();
    if (typeIndex < 0)
    {
        errorTextOut = "请选择有效的数据类型。";
        kLogEvent parsePatternTypeFailEvent;
        warn << parsePatternTypeFailEvent
            << "[MemoryDock] parseSearchPatternFromUi: 类型索引无效。"
            << eol;
        return false;
    }
    patternOut.valueType = static_cast<SearchValueType>(m_searchTypeCombo->itemData(typeIndex).toInt());

    // 搜索值文本是首次扫描的核心输入；为空时直接拒绝并提示用户。
    const QString valueText = m_searchValueEdit->text().trimmed();
    if (valueText.isEmpty())
    {
        errorTextOut = "搜索值不能为空。";
        kLogEvent parsePatternEmptyValueEvent;
        warn << parsePatternEmptyValueEvent
            << "[MemoryDock] parseSearchPatternFromUi: 搜索值为空。"
            << eol;
        return false;
    }

    // 按数据类型分别解析，最终都转成“exactBytes + wildcardMask”统一结构。
    switch (patternOut.valueType)
    {
    case SearchValueType::Byte:
    {
        std::uint64_t value = 0;
        if (!parseUnsignedNumber(valueText, value) || value > 0xFF)
        {
            errorTextOut = "字节类型请输入 0~255（支持十进制或 0x 十六进制）。";
            kLogEvent parsePatternByteFailEvent;
            warn << parsePatternByteFailEvent
                << "[MemoryDock] parseSearchPatternFromUi: Byte 解析失败, text="
                << valueText.toStdString()
                << eol;
            return false;
        }
        const std::uint8_t byteValue = static_cast<std::uint8_t>(value);
        patternOut.exactBytes = QByteArray(reinterpret_cast<const char*>(&byteValue), sizeof(byteValue));
        patternOut.lowerBound = static_cast<double>(byteValue);
        patternOut.upperBound = static_cast<double>(byteValue);
        break;
    }
    case SearchValueType::Int16:
    {
        bool parseOk = false;
        const qint16 value = static_cast<qint16>(valueText.toLongLong(&parseOk, 0));
        if (!parseOk)
        {
            errorTextOut = "2字节整数解析失败。";
            kLogEvent parsePatternI16FailEvent;
            warn << parsePatternI16FailEvent
                << "[MemoryDock] parseSearchPatternFromUi: Int16 解析失败, text="
                << valueText.toStdString()
                << eol;
            return false;
        }
        patternOut.exactBytes = QByteArray(reinterpret_cast<const char*>(&value), sizeof(value));
        patternOut.lowerBound = static_cast<double>(value);
        patternOut.upperBound = static_cast<double>(value);
        break;
    }
    case SearchValueType::Int32:
    {
        bool parseOk = false;
        const qint32 value = static_cast<qint32>(valueText.toLongLong(&parseOk, 0));
        if (!parseOk)
        {
            errorTextOut = "4字节整数解析失败。";
            kLogEvent parsePatternI32FailEvent;
            warn << parsePatternI32FailEvent
                << "[MemoryDock] parseSearchPatternFromUi: Int32 解析失败, text="
                << valueText.toStdString()
                << eol;
            return false;
        }
        patternOut.exactBytes = QByteArray(reinterpret_cast<const char*>(&value), sizeof(value));
        patternOut.lowerBound = static_cast<double>(value);
        patternOut.upperBound = static_cast<double>(value);
        break;
    }
    case SearchValueType::Int64:
    {
        bool parseOk = false;
        const qint64 value = static_cast<qint64>(valueText.toLongLong(&parseOk, 0));
        if (!parseOk)
        {
            errorTextOut = "8字节整数解析失败。";
            kLogEvent parsePatternI64FailEvent;
            warn << parsePatternI64FailEvent
                << "[MemoryDock] parseSearchPatternFromUi: Int64 解析失败, text="
                << valueText.toStdString()
                << eol;
            return false;
        }
        patternOut.exactBytes = QByteArray(reinterpret_cast<const char*>(&value), sizeof(value));
        patternOut.lowerBound = static_cast<double>(value);
        patternOut.upperBound = static_cast<double>(value);
        break;
    }
    case SearchValueType::Float32:
    {
        bool parseOk = false;
        const float value = valueText.toFloat(&parseOk);
        if (!parseOk)
        {
            errorTextOut = "浮点数解析失败。";
            kLogEvent parsePatternF32FailEvent;
            warn << parsePatternF32FailEvent
                << "[MemoryDock] parseSearchPatternFromUi: Float32 解析失败, text="
                << valueText.toStdString()
                << eol;
            return false;
        }
        patternOut.exactBytes = QByteArray(reinterpret_cast<const char*>(&value), sizeof(value));
        patternOut.lowerBound = static_cast<double>(value);
        patternOut.upperBound = static_cast<double>(value);
        patternOut.epsilon = 0.00001;
        break;
    }
    case SearchValueType::Float64:
    {
        bool parseOk = false;
        const double value = valueText.toDouble(&parseOk);
        if (!parseOk)
        {
            errorTextOut = "双精度浮点数解析失败。";
            kLogEvent parsePatternF64FailEvent;
            warn << parsePatternF64FailEvent
                << "[MemoryDock] parseSearchPatternFromUi: Float64 解析失败, text="
                << valueText.toStdString()
                << eol;
            return false;
        }
        patternOut.exactBytes = QByteArray(reinterpret_cast<const char*>(&value), sizeof(value));
        patternOut.lowerBound = value;
        patternOut.upperBound = value;
        patternOut.epsilon = 0.0000001;
        break;
    }
    case SearchValueType::ByteArray:
    {
        // 字节数组支持“AA BB CC”与“AA??CC”混合写法，这里统一按空格切词。
        QString normalizedText = valueText;
        normalizedText.replace(',', ' ');
        normalizedText.replace(';', ' ');
        const QStringList tokens = normalizedText.split(' ', Qt::SkipEmptyParts);
        if (tokens.isEmpty())
        {
            errorTextOut = "字节数组不能为空。示例：48 8B ?? ?? 89";
            kLogEvent parsePatternArrayEmptyEvent;
            warn << parsePatternArrayEmptyEvent
                << "[MemoryDock] parseSearchPatternFromUi: ByteArray token 为空。"
                << eol;
            return false;
        }

        for (const QString& tokenText : tokens)
        {
            const QString token = tokenText.trimmed().toUpper();
            if (token == "??")
            {
                patternOut.exactBytes.push_back('\0');
                patternOut.wildcardMask.push_back('\0');
                continue;
            }

            std::uint8_t parsedByte = 0;
            if (!parseHexByte(token, parsedByte))
            {
                errorTextOut = QString("无效字节项：%1").arg(tokenText);
                kLogEvent parsePatternArrayTokenFailEvent;
                warn << parsePatternArrayTokenFailEvent
                    << "[MemoryDock] parseSearchPatternFromUi: ByteArray token 无效, token="
                    << tokenText.toStdString()
                    << eol;
                return false;
            }

            patternOut.exactBytes.push_back(static_cast<char>(parsedByte));
            patternOut.wildcardMask.push_back('\1');
        }
        break;
    }
    case SearchValueType::StringAscii:
    {
        patternOut.exactBytes = valueText.toLatin1();
        break;
    }
    case SearchValueType::StringUnicode:
    {
        // QString 内部是 UTF-16，直接拷贝到底层字节即可得到 LE 序列。
        const QString unicodeText = valueText;
        const auto* utf16Data = reinterpret_cast<const char*>(unicodeText.utf16());
        patternOut.exactBytes = QByteArray(utf16Data, unicodeText.size() * static_cast<int>(sizeof(char16_t)));
        break;
    }
    default:
        errorTextOut = "未知数据类型。";
        kLogEvent parsePatternUnknownTypeEvent;
        err << parsePatternUnknownTypeEvent
            << "[MemoryDock] parseSearchPatternFromUi: 未知类型, enum="
            << static_cast<int>(patternOut.valueType)
            << eol;
        return false;
    }

    // 任何类型都必须形成至少 1 字节的匹配模式，否则无法执行扫描。
    if (patternOut.exactBytes.isEmpty())
    {
        errorTextOut = "解析后匹配模式为空，请检查输入值。";
        kLogEvent parsePatternEmptyResultEvent;
        warn << parsePatternEmptyResultEvent
            << "[MemoryDock] parseSearchPatternFromUi: 解析后字节序列为空。"
            << eol;
        return false;
    }

    // 解析成功日志：记录最终类型与模式长度。
    kLogEvent parsePatternFinishEvent;
    info << parsePatternFinishEvent
        << "[MemoryDock] parseSearchPatternFromUi: 解析成功, valueType="
        << static_cast<int>(patternOut.valueType)
        << ", exactBytesSize="
        << patternOut.exactBytes.size()
        << ", wildcardSize="
        << patternOut.wildcardMask.size()
        << eol;
    return true;
}

bool MemoryDock::collectSearchRegionsFromUi(
    std::vector<RegionEntry>& regionsOut,
    QString& errorTextOut)
{
    // 收集扫描区域入口日志：记录范围模式与过滤开关。
    kLogEvent collectRegionStartEvent;
    info << collectRegionStartEvent
        << "[MemoryDock] collectSearchRegionsFromUi: 开始收集扫描区域, rangeModeIndex="
        << m_searchRangeCombo->currentIndex()
        << ", imageOnly="
        << (m_searchImageOnlyCheck->isChecked() ? "true" : "false")
        << ", heapOnly="
        << (m_searchHeapOnlyCheck->isChecked() ? "true" : "false")
        << ", stackOnly="
        << (m_searchStackOnlyCheck->isChecked() ? "true" : "false")
        << eol;

    // 扫描前必须已附加目标进程，否则 ReadProcessMemory 无法进行。
    if (m_attachedProcessHandle == nullptr || m_attachedPid == 0)
    {
        errorTextOut = "请先附加目标进程。";
        kLogEvent collectRegionNoAttachEvent;
        warn << collectRegionNoAttachEvent
            << "[MemoryDock] collectSearchRegionsFromUi: 未附加进程。"
            << eol;
        return false;
    }

    // 区域缓存为空时主动刷新一次，避免首次进入扫描页时没有范围数据。
    if (m_regionCache.empty())
    {
        refreshMemoryRegionList(true);
    }

    if (m_regionCache.empty())
    {
        errorTextOut = "当前没有可用的内存区域，请先刷新区域列表。";
        kLogEvent collectRegionEmptyCacheEvent;
        warn << collectRegionEmptyCacheEvent
            << "[MemoryDock] collectSearchRegionsFromUi: 区域缓存为空。"
            << eol;
        return false;
    }

    // 自定义范围模式下，需要把用户输入地址解析成闭区间 [start, end]。
    bool useCustomRange = (m_searchRangeCombo->currentIndex() == 1);
    std::uint64_t rangeStart = 0;
    std::uint64_t rangeEnd = std::numeric_limits<std::uint64_t>::max();
    if (useCustomRange)
    {
        if (!parseAddressText(m_searchRangeStartEdit->text().trimmed(), rangeStart))
        {
            errorTextOut = "起始地址格式无效。";
            kLogEvent collectRegionStartParseFailEvent;
            warn << collectRegionStartParseFailEvent
                << "[MemoryDock] collectSearchRegionsFromUi: 起始地址解析失败, text="
                << m_searchRangeStartEdit->text().trimmed().toStdString()
                << eol;
            return false;
        }
        if (!parseAddressText(m_searchRangeEndEdit->text().trimmed(), rangeEnd))
        {
            errorTextOut = "结束地址格式无效。";
            kLogEvent collectRegionEndParseFailEvent;
            warn << collectRegionEndParseFailEvent
                << "[MemoryDock] collectSearchRegionsFromUi: 结束地址解析失败, text="
                << m_searchRangeEndEdit->text().trimmed().toStdString()
                << eol;
            return false;
        }
        if (rangeEnd < rangeStart)
        {
            errorTextOut = "结束地址不能小于起始地址。";
            kLogEvent collectRegionRangeInvalidEvent;
            warn << collectRegionRangeInvalidEvent
                << "[MemoryDock] collectSearchRegionsFromUi: 自定义范围非法, start="
                << formatAddress(rangeStart).toStdString()
                << ", end="
                << formatAddress(rangeEnd).toStdString()
                << eol;
            return false;
        }
    }

    const bool imageOnly = m_searchImageOnlyCheck->isChecked();
    const bool heapOnly = m_searchHeapOnlyCheck->isChecked();
    const bool stackOnly = m_searchStackOnlyCheck->isChecked();

    // “仅堆”和“仅栈”同时开启没有明确业务意义，这里直接提示用户二选一。
    if (heapOnly && stackOnly)
    {
        errorTextOut = "“仅堆”与“仅栈”不能同时启用。";
        kLogEvent collectRegionFilterConflictEvent;
        warn << collectRegionFilterConflictEvent
            << "[MemoryDock] collectSearchRegionsFromUi: heapOnly 与 stackOnly 同时启用。"
            << eol;
        return false;
    }

    regionsOut.clear();
    regionsOut.reserve(m_regionCache.size());

    for (const RegionEntry& region : m_regionCache)
    {
        // 只扫描已提交 + 可读区域，避免大量无效访问或 NOACCESS 报错。
        if (region.state != MEM_COMMIT || !isReadableProtect(region.protect))
        {
            continue;
        }

        // 按类型过滤：仅映像 -> MEM_IMAGE；仅堆 -> MEM_PRIVATE（近似）。
        if (imageOnly && region.type != MEM_IMAGE)
        {
            continue;
        }
        if (heapOnly && region.type != MEM_PRIVATE)
        {
            continue;
        }

        // 栈区域无法用单一字段准确识别，这里采用“PRIVATE + GUARD 或 RW”近似。
        if (stackOnly)
        {
            const bool maybeStack =
                (region.type == MEM_PRIVATE) &&
                (((region.protect & PAGE_GUARD) != 0) || ((region.protect & PAGE_READWRITE) != 0));
            if (!maybeStack)
            {
                continue;
            }
        }

        // 非自定义范围时直接入选；自定义范围时按交集裁剪区域边界。
        if (!useCustomRange)
        {
            regionsOut.push_back(region);
            continue;
        }

        if (region.regionSize == 0)
        {
            continue;
        }

        const std::uint64_t regionStart = region.baseAddress;
        const std::uint64_t regionEnd = region.baseAddress + region.regionSize - 1;
        if (regionEnd < rangeStart || regionStart > rangeEnd)
        {
            continue;
        }

        RegionEntry clippedRegion = region;
        clippedRegion.baseAddress = std::max(regionStart, rangeStart);
        const std::uint64_t clippedEnd = std::min(regionEnd, rangeEnd);
        clippedRegion.regionSize = clippedEnd - clippedRegion.baseAddress + 1;
        regionsOut.push_back(std::move(clippedRegion));
    }

    if (regionsOut.empty())
    {
        errorTextOut = "过滤后没有可扫描区域，请调整范围/过滤条件。";
        kLogEvent collectRegionEmptyResultEvent;
        warn << collectRegionEmptyResultEvent
            << "[MemoryDock] collectSearchRegionsFromUi: 过滤后区域为空。"
            << eol;
        return false;
    }

    // 收集成功日志：记录区域条目数。
    kLogEvent collectRegionFinishEvent;
    info << collectRegionFinishEvent
        << "[MemoryDock] collectSearchRegionsFromUi: 收集完成, regionCount="
        << regionsOut.size()
        << eol;

    return true;
}

