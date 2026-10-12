#include "ThemeColorRemap.h"
#include "ThemeBinding.h"

#include "../theme.h"

#include <QApplication>
#include <QAbstractItemView>
#include <QBrush>
#include <QElapsedTimer>
#include <QHash>
#include <QPalette>
#include <QPointer>
#include <QSet>
#include <QThread>
#include <QTimer>
#include <QWidget>
#include <QWidgetList>
#include <memory>

namespace ks::ui
{
    namespace
    {
        // AppendThemeColorSeries 作用：把带枚举的成组角色按固定顺序追加进 token 表。
        // 两次采集必须走同一段代码，下标才能一一对应。
        void AppendThemeColorSeries(QList<QColor>& colors)
        {
            using KswordTheme::AccentRole;
            using KswordTheme::PerformanceRole;
            using KswordTheme::TimelineRole;

            const AccentRole accentRoles[] = {
                AccentRole::Blue, AccentRole::Purple, AccentRole::Green, AccentRole::Orange,
                AccentRole::Cyan, AccentRole::Yellow, AccentRole::Red, AccentRole::Teal,
                AccentRole::Indigo, AccentRole::Brown, AccentRole::Lime, AccentRole::Slate,
                AccentRole::Violet
            };
            for (const AccentRole role : accentRoles)
            {
                colors.append(KswordTheme::AccentColor(role));
                colors.append(KswordTheme::AccentTextColor(role));
            }

            const PerformanceRole performanceRoles[] = {
                PerformanceRole::Cpu, PerformanceRole::Memory, PerformanceRole::Disk,
                PerformanceRole::Network, PerformanceRole::Gpu, PerformanceRole::Read,
                PerformanceRole::Write, PerformanceRole::DedicatedMemory,
                PerformanceRole::SharedMemory, PerformanceRole::VideoEncode,
                PerformanceRole::VideoDecode, PerformanceRole::Copy
            };
            for (const PerformanceRole role : performanceRoles)
            {
                colors.append(KswordTheme::PerformanceColor(role));
            }

            const TimelineRole timelineRoles[] = {
                TimelineRole::Process, TimelineRole::Thread, TimelineRole::Image,
                TimelineRole::File, TimelineRole::Registry, TimelineRole::Network,
                TimelineRole::Dns, TimelineRole::PowerShell, TimelineRole::Wmi,
                TimelineRole::Security, TimelineRole::Storage, TimelineRole::Kernel
            };
            for (const TimelineRole role : timelineRoles)
            {
                colors.append(KswordTheme::TimelineColor(role));
            }
        }

        // CollectThemeColors 作用：求值全部“调用即固化”的主题角色。
        // 只收录静态 token；palette(...) 形式的动态 token 由 Qt 自行跟随，不需要重写。
        // 登记顺序即撞色时的优先级，从大面积到小面积：中性表面 -> 文字 -> 交互控件
        // -> 强调色 -> 语义色 -> 编辑器与行状态 -> 图表配色。新增角色请按这个梯度插入。
        QList<QColor> CollectThemeColors()
        {
            QList<QColor> colors;
            colors.reserve(96);

            colors.append(KswordTheme::MainBackgroundColor());
            colors.append(KswordTheme::MainBackgroundTextColor());
            colors.append(KswordTheme::SurfaceColor());
            colors.append(KswordTheme::SurfaceAltColor());
            colors.append(KswordTheme::SurfaceMutedColor());
            colors.append(KswordTheme::BorderColor());
            colors.append(KswordTheme::BorderStrongColor());
            colors.append(KswordTheme::PaletteDarkColor());
            colors.append(KswordTheme::TextPrimaryColor());
            colors.append(KswordTheme::TextSecondaryColor());
            colors.append(KswordTheme::TextDisabledColor());
            colors.append(KswordTheme::OnAccentColor());

            colors.append(KswordTheme::ControlOutlineColor());
            colors.append(KswordTheme::ControlAccentColor());
            colors.append(KswordTheme::ControlAccentHoverColor());
            colors.append(KswordTheme::ControlAccentPressedColor());
            colors.append(KswordTheme::ControlDisabledOutlineColor());
            colors.append(KswordTheme::ControlDisabledFillColor());

            colors.append(KswordTheme::PrimaryAccentColor());
            // AccentColor(Blue) 是全项目用量最大的强调色写法（AccentHex(AccentRole::Blue)）。
            // 它在深色主题里与 InfoColor 取值相同，却在换主题时走向不同的新值，
            // 必须排在语义色之前，否则这批调用会被判给只有个位数用量的 InfoColor。
            colors.append(KswordTheme::AccentColor(KswordTheme::AccentRole::Blue));
            colors.append(KswordTheme::AccentButtonTextColor());
            colors.append(KswordTheme::PrimaryBlueSubtleColor());
            colors.append(KswordTheme::PrimaryBlueSurfacePressedColor());
            colors.append(KswordTheme::ActiveTabBackgroundColor());
            colors.append(KswordTheme::ActiveTabTextColor());

            colors.append(KswordTheme::SuccessColor());
            colors.append(KswordTheme::WarningColor());
            colors.append(KswordTheme::ErrorColor());
            colors.append(KswordTheme::InfoColor());
            colors.append(KswordTheme::SuccessBackgroundColor());
            colors.append(KswordTheme::WarningBackgroundColor());
            colors.append(KswordTheme::ErrorBackgroundColor());

            colors.append(KswordTheme::EditorMatchColor());
            colors.append(KswordTheme::EditorCurrentMatchColor());
            colors.append(KswordTheme::EditorSelectionColor());
            colors.append(KswordTheme::NewRowBackgroundColor());
            colors.append(KswordTheme::ExitedRowBackgroundColor());
            colors.append(KswordTheme::ExitedRowForegroundColor());

            // 项目里实际用到的强调色亮度变体：它们不是默认偏移，必须显式登记。
            colors.append(KswordTheme::AccentColor(KswordTheme::AccentRole::Blue, 6, -20));
            colors.append(KswordTheme::AccentColor(KswordTheme::AccentRole::Blue, -12, -38));
            colors.append(KswordTheme::AccentColor(KswordTheme::AccentRole::Blue, -18, -26));
            colors.append(KswordTheme::AccentTextColor(
                KswordTheme::AccentRole::Red,
                KswordTheme::BlackColor()));

            AppendThemeColorSeries(colors);
            return colors;
        }

        // BuildColorMapping 作用：由“旧值 -> 新值”建立重写表。
        // 不同角色在某个主题下取到同一个旧值是常态，例如内置深色主题里
        // BorderColor 与 ActiveTabBackgroundColor 的偏移完全相同；换主题后两者却走向
        // 不同的新值。此时按 CollectThemeColors 的登记顺序取第一个新值：该顺序是
        // 刻意排的优先级，中性表面、边框和文字排在强调色与图表色之前，
        // 因为它们决定了界面绝大部分面积。整条跳过会让这些主色一起失去跟随，
        // 代价远大于让个别小面积特例跟着主色走。
        QHash<QRgb, QRgb> BuildColorMapping(
            const QList<QString>& previousColorTexts,
            const QList<QColor>& currentColors,
            int* ambiguousColorCount)
        {
            QHash<QRgb, QRgb> preferredMapping;
            QSet<QRgb> ambiguousColors;
            const int pairCount = qMin(previousColorTexts.size(), currentColors.size());
            for (int index = 0; index < pairCount; ++index)
            {
                const QColor previousColor(previousColorTexts.at(index));
                const QColor currentColor = currentColors.at(index);
                if (!previousColor.isValid() || !currentColor.isValid())
                {
                    continue;
                }
                const QRgb previousValue =
                    qRgb(previousColor.red(), previousColor.green(), previousColor.blue());
                const QRgb currentValue =
                    qRgb(currentColor.red(), currentColor.green(), currentColor.blue());

                const auto existingIterator = preferredMapping.constFind(previousValue);
                if (existingIterator == preferredMapping.constEnd())
                {
                    preferredMapping.insert(previousValue, currentValue);
                    continue;
                }
                if (existingIterator.value() != currentValue)
                {
                    ambiguousColors.insert(previousValue);
                }
            }

            QHash<QRgb, QRgb> colorMapping;
            for (auto iterator = preferredMapping.constBegin();
                iterator != preferredMapping.constEnd();
                ++iterator)
            {
                // 取值没变的角色不需要重写，留在表里只会拖慢扫描。
                if (iterator.value() != iterator.key())
                {
                    colorMapping.insert(iterator.key(), iterator.value());
                }
            }

            if (ambiguousColorCount != nullptr)
            {
                *ambiguousColorCount = static_cast<int>(ambiguousColors.size());
            }
            return colorMapping;
        }

        bool IsHexDigit(const QChar character)
        {
            return (character >= QLatin1Char('0') && character <= QLatin1Char('9'))
                || (character >= QLatin1Char('a') && character <= QLatin1Char('f'))
                || (character >= QLatin1Char('A') && character <= QLatin1Char('F'));
        }

        // FormatHexChannels 作用：按原写法的大小写和位宽回写颜色，避免样式文本无谓抖动。
        QString FormatHexChannels(const QRgb colorValue, const bool upperCase)
        {
            const QString hexText = QStringLiteral("%1%2%3")
                .arg(qRed(colorValue), 2, 16, QLatin1Char('0'))
                .arg(qGreen(colorValue), 2, 16, QLatin1Char('0'))
                .arg(qBlue(colorValue), 2, 16, QLatin1Char('0'));
            return upperCase ? hexText.toUpper() : hexText;
        }

        // TryRewriteHexColor 作用：处理 #RRGGBB 与 #AARRGGBB 两种写法。
        // 只在整段十六进制恰好是 6 位或 8 位时改写，位数不符的写法原样保留。
        bool TryRewriteHexColor(
            const QHash<QRgb, QRgb>& colorMapping,
            const QString& styleText,
            const int hashIndex,
            QString* rewrittenText,
            int* nextIndex)
        {
            int scanIndex = hashIndex + 1;
            while (scanIndex < styleText.size() && IsHexDigit(styleText.at(scanIndex)))
            {
                ++scanIndex;
            }
            const int digitCount = scanIndex - hashIndex - 1;
            if (digitCount != 6 && digitCount != 8)
            {
                return false;
            }

            // 8 位写法的前两位是 alpha 通道，颜色匹配只看后面的 RGB。
            const int rgbOffset = hashIndex + 1 + (digitCount == 8 ? 2 : 0);
            const QString rgbText = styleText.mid(rgbOffset, 6);
            bool parsedOk = false;
            const uint parsedValue = rgbText.toUInt(&parsedOk, 16);
            if (!parsedOk)
            {
                return false;
            }

            const auto mappedIterator = colorMapping.constFind(
                qRgb((parsedValue >> 16) & 0xFF, (parsedValue >> 8) & 0xFF, parsedValue & 0xFF));
            if (mappedIterator == colorMapping.constEnd())
            {
                return false;
            }

            const bool upperCase = (rgbText == rgbText.toUpper());
            rewrittenText->append(styleText.mid(hashIndex, rgbOffset - hashIndex));
            rewrittenText->append(FormatHexChannels(mappedIterator.value(), upperCase));
            *nextIndex = scanIndex;
            return true;
        }

        // TryRewriteFunctionalColor 作用：处理 rgb(r,g,b) 与 rgba(r,g,b,a) 两种写法。
        // 只改写前三个通道，alpha 参数按原文保留，透明度设计不会被主题切换改掉。
        bool TryRewriteFunctionalColor(
            const QHash<QRgb, QRgb>& colorMapping,
            const QString& styleText,
            const int startIndex,
            QString* rewrittenText,
            int* nextIndex)
        {
            int scanIndex = startIndex;
            if (styleText.mid(scanIndex, 4).compare(QStringLiteral("rgba"), Qt::CaseInsensitive) == 0)
            {
                scanIndex += 4;
            }
            else if (styleText.mid(scanIndex, 3).compare(QStringLiteral("rgb"), Qt::CaseInsensitive) == 0)
            {
                scanIndex += 3;
            }
            else
            {
                return false;
            }
            while (scanIndex < styleText.size() && styleText.at(scanIndex).isSpace())
            {
                ++scanIndex;
            }
            if (scanIndex >= styleText.size() || styleText.at(scanIndex) != QLatin1Char('('))
            {
                return false;
            }
            const int openParenIndex = scanIndex;
            const int closeParenIndex = styleText.indexOf(QLatin1Char(')'), openParenIndex + 1);
            if (closeParenIndex < 0)
            {
                return false;
            }

            const QString argumentText =
                styleText.mid(openParenIndex + 1, closeParenIndex - openParenIndex - 1);
            const QList<QStringView> argumentParts = QStringView(argumentText).split(QLatin1Char(','));
            if (argumentParts.size() < 3 || argumentParts.size() > 4)
            {
                return false;
            }

            int channelValues[3] = { 0, 0, 0 };
            for (int channelIndex = 0; channelIndex < 3; ++channelIndex)
            {
                bool parsedOk = false;
                channelValues[channelIndex] = argumentParts.at(channelIndex).trimmed().toInt(&parsedOk);
                if (!parsedOk || channelValues[channelIndex] < 0 || channelValues[channelIndex] > 255)
                {
                    return false;
                }
            }

            const auto mappedIterator = colorMapping.constFind(
                qRgb(channelValues[0], channelValues[1], channelValues[2]));
            if (mappedIterator == colorMapping.constEnd())
            {
                return false;
            }

            const QRgb mappedColor = mappedIterator.value();
            rewrittenText->append(styleText.mid(startIndex, openParenIndex + 1 - startIndex));
            rewrittenText->append(QStringLiteral("%1,%2,%3")
                .arg(qRed(mappedColor))
                .arg(qGreen(mappedColor))
                .arg(qBlue(mappedColor)));
            if (argumentParts.size() == 4)
            {
                rewrittenText->append(QLatin1Char(','));
                rewrittenText->append(argumentParts.at(3).trimmed().toString());
            }
            rewrittenText->append(QLatin1Char(')'));
            *nextIndex = closeParenIndex + 1;
            return true;
        }

        // RewriteColorsInText 作用：单趟扫描完成全部改写。
        // 单趟是必要的：逐条 QString::replace 会让 A->B、B->C 两条规则串成 A->C。
        QString RewriteColorsInText(const QHash<QRgb, QRgb>& colorMapping, const QString& styleText)
        {
            if (colorMapping.isEmpty() || styleText.isEmpty())
            {
                return styleText;
            }

            QString rewrittenText;
            rewrittenText.reserve(styleText.size());
            int scanIndex = 0;
            while (scanIndex < styleText.size())
            {
                const QChar currentCharacter = styleText.at(scanIndex);
                int nextIndex = scanIndex;
                if (currentCharacter == QLatin1Char('#')
                    && TryRewriteHexColor(colorMapping, styleText, scanIndex, &rewrittenText, &nextIndex))
                {
                    scanIndex = nextIndex;
                    continue;
                }
                if ((currentCharacter == QLatin1Char('r') || currentCharacter == QLatin1Char('R'))
                    && TryRewriteFunctionalColor(
                        colorMapping, styleText, scanIndex, &rewrittenText, &nextIndex))
                {
                    scanIndex = nextIndex;
                    continue;
                }
                rewrittenText.append(currentCharacter);
                ++scanIndex;
            }
            return rewrittenText;
        }

        // RewritePaletteColors 作用：改写一个控件自己设过的 QPalette。
        // 只遍历 isBrushSet 为真的 group/role：未显式设置的角色仍然继承 QApplication
        // 调色板，本来就会跟随主题，重写反而会把继承关系固化下来。
        bool RewritePaletteColors(const QHash<QRgb, QRgb>& colorMapping, QPalette& widgetPalette)
        {
            bool paletteChanged = false;
            for (int groupIndex = 0; groupIndex < QPalette::NColorGroups; ++groupIndex)
            {
                const auto colorGroup = static_cast<QPalette::ColorGroup>(groupIndex);
                for (int roleIndex = 0; roleIndex < QPalette::NColorRoles; ++roleIndex)
                {
                    const auto colorRole = static_cast<QPalette::ColorRole>(roleIndex);
                    if (!widgetPalette.isBrushSet(colorGroup, colorRole))
                    {
                        continue;
                    }
                    const QBrush roleBrush = widgetPalette.brush(colorGroup, colorRole);
                    // 渐变或纹理画刷不是单一颜色，按颜色查表会误改，整体跳过。
                    if (roleBrush.style() != Qt::SolidPattern)
                    {
                        continue;
                    }
                    const QColor roleColor = roleBrush.color();
                    const auto mappedIterator = colorMapping.constFind(
                        qRgb(roleColor.red(), roleColor.green(), roleColor.blue()));
                    if (mappedIterator == colorMapping.constEnd())
                    {
                        continue;
                    }
                    const QRgb mappedColor = mappedIterator.value();
                    widgetPalette.setColor(
                        colorGroup,
                        colorRole,
                        QColor(
                            qRed(mappedColor),
                            qGreen(mappedColor),
                            qBlue(mappedColor),
                            roleColor.alpha()));
                    paletteChanged = true;
                }
            }
            return paletteChanged;
        }

        using ColorMapping = QHash<QRgb, QRgb>; // 单趟颜色转换，保持原文本/画刷匹配边界。
        using SharedColorMapping = std::shared_ptr<const ColorMapping>; // 同一原主题的控件共享转换链。

        // 只识别共享表格组件唯一且完整的拥有块，缺失/重复/不完整时保持整串所有权保护。
        struct TableStyleParts
        {
            QString before, block, after; // 原顺序保留，框架的新块不能被旧主题映射污染。
            bool complete = false;       // 完整拥有块才允许独立比较业务字串。
        };

        TableStyleParts SplitTablePresentationStyle(const QString& style)
        {
            const QString begin = QStringLiteral("/*KSWORD_TABLE_PRESENTATION_BEGIN*/"); // 精确组件起点。
            const QString end = QStringLiteral("/*KSWORD_TABLE_PRESENTATION_END*/"); // 精确组件终点。
            const qsizetype first = style.indexOf(begin);
            const qsizetype last = style.indexOf(end);
            if (first < 0 || last < first || style.indexOf(begin, first + begin.size()) >= 0
                || style.indexOf(end, last + end.size()) >= 0)
            {
                return {style, {}, {}, false};
            }
            const qsizetype length = last + end.size() - first; // 拥有标记也随当前块原位保留。
            return {style.left(first), style.mid(first, length), style.mid(first + length), true};
        }

        bool SameBusinessStyle(const QString& expected, const QString& current)
        {
            if (expected == current)
            {
                return true;
            }
            const TableStyleParts oldParts = SplitTablePresentationStyle(expected); // 排队来源。
            const TableStyleParts newParts = SplitTablePresentationStyle(current); // 组件可能已经换肤。
            return oldParts.complete && newParts.complete
                && oldParts.before + oldParts.after == newParts.before + newParts.after;
        }

        // 仅对实际呈现表格启用逐角色保护；普通窗口仍采用整份显式 palette 基线检查。
        bool HasTablePresentation(QWidget* widget, const QString& style)
        {
            return qobject_cast<QAbstractItemView*>(widget) != nullptr
                && SplitTablePresentationStyle(style).complete;
        }

        // RewriteTablePalette：共享 Base/AlternateBase 留给表格主题，其余角色须逐项仍等于旧源。
        bool RewriteTablePalette(const QPalette& expected, const QHash<int, SharedColorMapping>& mappings,
            QPalette& current)
        {
            bool changed = false; // 只有仍自行拥有的纯色画刷实际映射才写 palette。
            for (auto iterator = mappings.constBegin(); iterator != mappings.constEnd(); ++iterator)
            {
                const auto group = static_cast<QPalette::ColorGroup>(iterator.key() / QPalette::NColorRoles);
                const auto role = static_cast<QPalette::ColorRole>(iterator.key() % QPalette::NColorRoles);
                const QBrush brush = current.brush(group, role); // 业务当前角色画刷。
                if (!expected.isBrushSet(group, role) || !current.isBrushSet(group, role)
                    || expected.brush(group, role) != brush || brush.style() != Qt::SolidPattern)
                {
                    continue;
                }
                const QColor color = brush.color(); // 保持该角色原始透明度。
                const auto mapped = iterator.value()->constFind(qRgb(color.red(), color.green(), color.blue()));
                if (mapped != iterator.value()->constEnd())
                {
                    current.setColor(group, role, QColor(qRed(mapped.value()), qGreen(mapped.value()),
                        qBlue(mapped.value()), color.alpha()));
                    changed = true;
                }
            }
            return changed;
        }

        // ComposeColorMappings：等价于先运行 previous 再运行 next，但最终只写一次控件。
        // 原键优先保留首次匹配，不能用逐颜色 replace 把同一轮 A→B 意外串成 A→C。
        SharedColorMapping ComposeColorMappings(
            const SharedColorMapping& previous, const SharedColorMapping& next)
        {
            if (!previous || previous->isEmpty())
            {
                return next;
            }
            if (!next || next->isEmpty())
            {
                return previous;
            }
            ColorMapping composed = *previous; // 旧基线直接到最新主题的合成映射。
            for (auto iterator = composed.begin(); iterator != composed.end(); ++iterator)
            {
                iterator.value() = next->value(iterator.value(), iterator.value());
            }
            for (auto iterator = next->constBegin(); iterator != next->constEnd(); ++iterator)
            {
                if (!previous->contains(iterator.key()))
                {
                    composed.insert(iterator.key(), iterator.value());
                }
            }
            // 往返主题后已经回到原值的键不需要触发 Qt 样式重算。
            for (auto iterator = composed.begin(); iterator != composed.end();)
            {
                if (iterator.key() == iterator.value())
                {
                    iterator = composed.erase(iterator);
                }
                else
                {
                    ++iterator;
                }
            }
            return std::make_shared<const ColorMapping>(std::move(composed));
        }

        // SameExplicitPalette：只比较自行设置的角色，忽略 QApplication 已更新的继承角色。
        // 渐变/纹理也参与所有权比较，但实际重写仍由 RewritePaletteColors 跳过这些画刷。
        bool SameExplicitPalette(const QPalette& expected, const QPalette& current)
        {
            if (expected.resolveMask() != current.resolveMask())
            {
                return false;
            }
            if (expected == current)
            {
                return true;
            }
            for (int groupIndex = 0; groupIndex < QPalette::NColorGroups; ++groupIndex)
            {
                const auto group = static_cast<QPalette::ColorGroup>(groupIndex); // 当前比较的色组。
                for (int roleIndex = 0; roleIndex < QPalette::NColorRoles; ++roleIndex)
                {
                    const auto role = static_cast<QPalette::ColorRole>(roleIndex); // 当前显式角色。
                    if (expected.isBrushSet(group, role) != current.isBrushSet(group, role)
                        || (expected.isBrushSet(group, role)
                            && expected.brush(group, role) != current.brush(group, role)))
                    {
                        return false;
                    }
                }
            }
            return true;
        }

        // ThemeColorRemapQueue：每片只操作已登记控件，完成后释放基线而不常驻全部页面。
        // 不安装叶控件过滤器，不触碰模型或文本格式；全应用请求只读一次控件/值快照。
        class ThemeColorRemapQueue final : public QObject
        {
        public:
            explicit ThemeColorRemapQueue(QApplication* app) : QObject(app)
            {
            }

            // schedule 返回排队统计；同步阶段只读，不调用 setPalette/setStyleSheet。
            ThemeColorRemapResult schedule(const ThemeColorSnapshot& previousSnapshot,
                QObject* completionContext, std::function<void(const ThemeColorRemapResult&)> completion)
            {
                ThemeColorRemapResult result; // 本次请求映射和排队统计，不冒充完成计数。
                const SharedColorMapping nextMapping = std::make_shared<const ColorMapping>(BuildColorMapping(
                    previousSnapshot.colorTexts, CollectThemeColors(), &result.ambiguousColorCount));
                result.mappedColorCount = static_cast<int>(nextMapping->size());

                // 字体等设置可能重用同一颜色主题；没有未完成旧链时不能白扫全部 QWidget。
                if (nextMapping->isEmpty() && m_cursor >= m_work.size())
                {
                    ++m_generation;
                    m_work.clear();
                    m_cursor = 0;
                    m_slicePending = false;
                    m_result = result;
                    m_completionContext = completionContext;
                    m_completion = std::move(completion);
                    scheduleSlice();
                    return result;
                }

                // 只索引尚未完成且仍活着的对象；同地址重新创建的 QWidget 不继承旧转换链。
                QHash<QWidget*, const WidgetWork*> unfinished; // 上代未写入控件的真实身份。
                for (qsizetype index = m_cursor; index < m_work.size(); ++index)
                {
                    if (QWidget* widget = m_work.at(index).widget.data())
                    {
                        unfinished.insert(widget, &m_work.at(index));
                    }
                }
                QHash<const ColorMapping*, SharedColorMapping> composedCache; // 同一源映射每请求只合成一次。
                const auto continueMapping = [&composedCache, &nextMapping](const SharedColorMapping& oldMapping)
                {
                    const ColorMapping* key = oldMapping.get(); // 仅在本次请求内使用，shared_ptr 保持源寿命。
                    const auto found = composedCache.constFind(key);
                    if (found != composedCache.constEnd())
                    {
                        return found.value();
                    }
                    const SharedColorMapping composed = ComposeColorMappings(oldMapping, nextMapping);
                    composedCache.insert(key, composed);
                    return composed;
                };

                QList<WidgetWork> replacement; // 新代只保存当前仍需处理的控件，不累计历史任务。
                const QWidgetList snapshot = QApplication::allWidgets();
                replacement.reserve(snapshot.size());
                for (QWidget* widget : snapshot)
                {
                    if (widget == nullptr || HasWidgetThemeBinding(widget))
                    {
                        continue;
                    }
                    WidgetWork work; // 字串与显式 palette 分别保留基线及映射链。
                    work.widget = widget;
                    work.style = widget->styleSheet();
                    work.hasPalette = widget->testAttribute(Qt::WA_SetPalette);
                    if (work.style.isEmpty() && !work.hasPalette)
                    {
                        continue;
                    }
                    if (work.hasPalette)
                    {
                        work.palette = widget->palette();
                    }
                    work.styleMapping = nextMapping;
                    work.paletteMapping = nextMapping;
                    const WidgetWork* oldWork = unfinished.value(widget, nullptr); // 同对象的未处理旧任务。
                    if (oldWork != nullptr)
                    {
                        // 只有源仍与旧基线相同才继续旧链；业务主动重建的字段从新主题代次开始。
                        if (SameBusinessStyle(oldWork->style, work.style))
                        {
                            work.styleMapping = continueMapping(oldWork->styleMapping);
                        }
                        if (work.hasPalette && oldWork->hasPalette
                            && SameExplicitPalette(oldWork->palette, work.palette))
                        {
                            work.paletteMapping = continueMapping(oldWork->paletteMapping);
                        }
                    }
                    work.tablePresentation = HasTablePresentation(widget, work.style);
                    if (work.hasPalette && work.tablePresentation)
                    {
                        // 表格组件可先换两种底色，不能因此丢掉其它未更新角色的 A→B→C 转换链。
                        for (int groupIndex = 0; groupIndex < QPalette::NColorGroups; ++groupIndex)
                        {
                            const auto group = static_cast<QPalette::ColorGroup>(groupIndex); // 当前色组。
                            for (int roleIndex = 0; roleIndex < QPalette::NColorRoles; ++roleIndex)
                            {
                                const auto role = static_cast<QPalette::ColorRole>(roleIndex); // 当前独立角色。
                                if (role == QPalette::Base || role == QPalette::AlternateBase
                                    || !work.palette.isBrushSet(group, role))
                                {
                                    continue;
                                }
                                const int key = groupIndex * QPalette::NColorRoles + roleIndex; // 角色身份。
                                SharedColorMapping mapping = work.paletteMapping; // 整份旧源未变时保留其原转换链。
                                if (oldWork != nullptr && oldWork->tablePresentation && oldWork->hasPalette
                                    && oldWork->palette.isBrushSet(group, role)
                                    && oldWork->palette.brush(group, role) == work.palette.brush(group, role))
                                {
                                    mapping = continueMapping(oldWork->tablePaletteMappings.value(key));
                                }
                                if (!mapping->isEmpty())
                                {
                                    work.tablePaletteMappings.insert(key, mapping);
                                }
                            }
                        }
                    }
                    if ((!work.style.isEmpty() && !work.styleMapping->isEmpty())
                        || (work.hasPalette && (work.tablePresentation
                            ? !work.tablePaletteMappings.isEmpty() : !work.paletteMapping->isEmpty())))
                    {
                        replacement.append(std::move(work));
                    }
                }
                ++m_generation; // 旧计时器失效，未完成的源颜色已经合入 replacement。
                m_work = std::move(replacement);
                m_cursor = 0;
                m_slicePending = false;
                result.scheduledWidgetCount = static_cast<int>(m_work.size());
                m_result = result;
                m_completionContext = completionContext;
                m_completion = std::move(completion);
                scheduleSlice();
                return result;
            }

        private:
            struct WidgetWork
            {
                QPointer<QWidget> widget; // 真实控件身份，析构后基线不能转交同地址的新对象。
                QString style;           // 排队时的原始局部 QSS，变化后不再接管。
                QPalette palette;        // 仅用显式角色检查来源，写入从当前 palette 保留继承值。
                bool hasPalette = false; // Qt::WA_SetPalette 原边界，不固化原本的继承调色板。
                SharedColorMapping styleMapping;   // QSS 未完成的主题转换链。
                SharedColorMapping paletteMapping; // 显式 palette 未完成的转换链。
                bool tablePresentation = false; // 仅完整共享表格拥有块允许逐角色保护。
                QHash<int, SharedColorMapping> tablePaletteMappings; // 表格其它显式角色独立保留主题链。
            };

            // scheduleSlice：零延迟排队而不进入嵌套事件循环，首片也由事件循环调度。
            void scheduleSlice()
            {
                if (m_slicePending)
                {
                    return;
                }
                m_slicePending = true;
                const quint64 generation = m_generation; // 此计时回调所属主题请求。
                QTimer::singleShot(0, this, [this, generation]()
                {
                    if (generation != m_generation)
                    {
                        return;
                    }
                    m_slicePending = false;
                    drain(generation);
                });
            }

            // drain：目标约 6ms 后让出；单个 Qt polish 不能抢占，因此不是耗时硬上限。
            void drain(const quint64 generation)
            {
                QElapsedTimer budget; // 当前切片实际耗时，只在控件之间判断是否让出。
                budget.start();
                while (m_cursor < m_work.size() && budget.elapsed() < 6)
                {
                    const WidgetWork work = m_work.at(m_cursor); // Setter 可触发新请求，不能持有队列元素引用。
                    QWidget* widget = work.widget.data();
                    if (widget != nullptr && !HasWidgetThemeBinding(widget))
                    {
                        QPalette currentPalette = widget->palette(); // 保留当前继承值，绝不写回旧全局角色。
                        bool paletteChanged = false; // 仅同一业务基线的角色可参与转换。
                        if (work.hasPalette && widget->testAttribute(Qt::WA_SetPalette))
                        {
                            paletteChanged = work.tablePresentation
                                ? (HasTablePresentation(widget, widget->styleSheet())
                                    && RewriteTablePalette(work.palette, work.tablePaletteMappings, currentPalette))
                                : (SameExplicitPalette(work.palette, currentPalette)
                                    && RewritePaletteColors(*work.paletteMapping, currentPalette));
                        }
                        if (paletteChanged)
                        {
                            widget->setPalette(currentPalette);
                            if (generation != m_generation)
                            {
                                return;
                            }
                            ++m_result.rewrittenPaletteWidgetCount;
                        }
                        // Palette 写入可能析构控件或改写其 QSS，重新核对寿命与当前局部源。
                        widget = work.widget.data();
                        if (widget != nullptr && !HasWidgetThemeBinding(widget))
                        {
                            const QString currentStyle = widget->styleSheet(); // 当前业务仍拥有的字串基线。
                            if (!currentStyle.isEmpty() && SameBusinessStyle(work.style, currentStyle))
                            {
                                ++m_result.inspectedWidgetCount;
                                // 只映射业务前后片段，当前 TablePresentation 新块保留原顺序和颜色。
                                const TableStyleParts parts = SplitTablePresentationStyle(currentStyle);
                                const QString rewritten = RewriteColorsInText(*work.styleMapping, parts.before)
                                    + parts.block + RewriteColorsInText(*work.styleMapping, parts.after);
                                if (rewritten != currentStyle)
                                {
                                    widget->setStyleSheet(rewritten);
                                    if (generation != m_generation)
                                    {
                                        return;
                                    }
                                    ++m_result.rewrittenWidgetCount;
                                }
                            }
                        }
                    }
                    // 当前控件两种字段完成后再前进，新请求能看见部分完成控件的剩余旧源。
                    ++m_cursor;
                }
                if (m_cursor < m_work.size())
                {
                    scheduleSlice();
                    return;
                }
                const ThemeColorRemapResult completed = m_result; // 回调只报告当前请求实际完成统计。
                const QPointer<QObject> context = m_completionContext; // 回调所依赖业务对象的寿命。
                auto completion = std::move(m_completion); // 先释放队列，允许回调安全发起下一主题请求。
                m_work.clear();
                m_cursor = 0;
                if (!context.isNull() && completion)
                {
                    completion(completed);
                }
            }

            QList<WidgetWork> m_work; // 当前请求快照，完成时释放全部值和 QPointer。
            qsizetype m_cursor = 0;   // 尚未完成控件下标，处理中不提前移动。
            quint64 m_generation = 0; // 最新调度代次，旧计时器不能继续写入。
            bool m_slicePending = false; // 合并每代切片调度。
            ThemeColorRemapResult m_result; // 当前请求的实际完成计数。
            QPointer<QObject> m_completionContext; // 不延长窗口/应用业务对象的寿命。
            std::function<void(const ThemeColorRemapResult&)> m_completion; // 最新请求完成通知。
        };
    }

    ThemeColorSnapshot CaptureThemeColorSnapshot()
    {
        ThemeColorSnapshot snapshot;
        const QList<QColor> colors = CollectThemeColors();
        snapshot.colorTexts.reserve(colors.size());
        for (const QColor& color : colors)
        {
            snapshot.colorTexts.append(KswordTheme::ThemeColorName(color));
        }
        return snapshot;
    }

    QString RemapStaleThemeColorsInText(
        const ThemeColorSnapshot& previousSnapshot,
        const QString& styleText)
    {
        if (previousSnapshot.colorTexts.isEmpty() || styleText.isEmpty())
        {
            return styleText;
        }
        const QHash<QRgb, QRgb> colorMapping =
            BuildColorMapping(previousSnapshot.colorTexts, CollectThemeColors(), nullptr);
        return RewriteColorsInText(colorMapping, styleText);
    }

    QColor RemapStaleThemeColor(const ThemeColorSnapshot& previousSnapshot, const QColor& color)
    {
        if (previousSnapshot.colorTexts.isEmpty() || !color.isValid())
        {
            return color;
        }
        const QHash<QRgb, QRgb> colorMapping =
            BuildColorMapping(previousSnapshot.colorTexts, CollectThemeColors(), nullptr);
        const auto iterator = colorMapping.constFind(qRgb(color.red(), color.green(), color.blue()));
        return iterator == colorMapping.constEnd()
            ? color
            : QColor(qRed(iterator.value()), qGreen(iterator.value()), qBlue(iterator.value()), color.alpha());
    }

    QPalette RemapStaleThemeColorsInPalette(
        const ThemeColorSnapshot& previousSnapshot,
        const QPalette& palette)
    {
        QPalette updatedPalette = palette;
        if (!previousSnapshot.colorTexts.isEmpty())
        {
            const QHash<QRgb, QRgb> colorMapping =
                BuildColorMapping(previousSnapshot.colorTexts, CollectThemeColors(), nullptr);
            RewritePaletteColors(colorMapping, updatedPalette);
        }
        return updatedPalette;
    }

    ThemeColorRemapResult RemapStaleThemeColors(const ThemeColorSnapshot& previousSnapshot)
    {
        ThemeColorRemapResult result;
        if (previousSnapshot.colorTexts.isEmpty())
        {
            return result;
        }

        const QHash<QRgb, QRgb> colorMapping = BuildColorMapping(
            previousSnapshot.colorTexts,
            CollectThemeColors(),
            &result.ambiguousColorCount);
        result.mappedColorCount = static_cast<int>(colorMapping.size());
        if (colorMapping.isEmpty())
        {
            return result;
        }

        // allWidgets() 返回裸指针快照，而 setStyleSheet / setPalette 会触发 polish 与
        // 样式重算，途中可能有控件被延迟销毁。先整体包成 QPointer，遍历时再校验，
        // 避免后半程解引用到已析构的 QWidget。
        const QWidgetList widgetSnapshot = QApplication::allWidgets();
        QList<QPointer<QWidget>> guardedWidgets;
        guardedWidgets.reserve(widgetSnapshot.size());
        for (QWidget* const widget : widgetSnapshot)
        {
            guardedWidgets.append(QPointer<QWidget>(widget));
        }

        for (const QPointer<QWidget>& guardedWidget : guardedWidgets)
        {
            QWidget* const widget = guardedWidget.data();
            if (widget == nullptr || HasWidgetThemeBinding(widget))
            {
                continue;
            }
            // 显式 setPalette 过的控件不再接受 QApplication 调色板更新，必须单独改写。
            QPalette widgetPalette = widget->palette();
            if (widget->testAttribute(Qt::WA_SetPalette)
                && RewritePaletteColors(colorMapping, widgetPalette))
            {
                widget->setPalette(widgetPalette);
                ++result.rewrittenPaletteWidgetCount;
            }

            const QString currentStyleSheet = widget->styleSheet();
            if (currentStyleSheet.isEmpty())
            {
                continue;
            }
            ++result.inspectedWidgetCount;
            const QString rewrittenStyleSheet =
                RewriteColorsInText(colorMapping, currentStyleSheet);
            if (rewrittenStyleSheet != currentStyleSheet)
            {
                widget->setStyleSheet(rewrittenStyleSheet);
                ++result.rewrittenWidgetCount;
            }
        }
        return result;
    }

    ThemeColorRemapResult ScheduleStaleThemeColorRemap(const ThemeColorSnapshot& previousSnapshot,
        QObject* completionContext, std::function<void(const ThemeColorRemapResult&)> completion)
    {
        QApplication* app = qobject_cast<QApplication*>(QCoreApplication::instance()); // 当前 GUI 应用。
        if (app == nullptr || QThread::currentThread() != app->thread()
            || previousSnapshot.colorTexts.isEmpty())
        {
            return {};
        }
        // 队列由 QApplication 管理寿命；应用退出后不能访问旧实例的计时器或 QWidget。
        static QPointer<ThemeColorRemapQueue> queue;
        if (queue.isNull())
        {
            queue = new ThemeColorRemapQueue(app);
        }
        return queue->schedule(previousSnapshot, completionContext, std::move(completion));
    }
}
