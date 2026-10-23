// Pure QObject/history checks: no QWidget, QGuiApplication or event-loop rendering.
#include "../../shared/ui/MetricChartBinding.h"

#include <QCoreApplication>
#include <cmath>
#include <cstdlib>
#include <iostream>

namespace
{
    int checks = 0;
    void check(const bool condition, const char* message)
    {
        ++checks;
        if (!condition)
        {
            std::cerr << "FAIL: " << message << '\n';
            std::exit(1);
        }
    }

    void defaultAndHiddenHistory()
    {
        QLineSeries series;
        int replacements = 0;
        series.setChangeHandler([&] { ++replacements; });
        auto* binding = ks::ui::MetricChartBinding::ForSeries(&series, 6);
        check(binding->presentationEnabled(), "default presentation changed");
        binding->append({1, 1000, 3.0, true});
        binding->append({2, 2000, 9.0, true});
        binding->append({3, 3000, 4.0, true});
        check(replacements == 3, "legacy append did not replace once per sample");
        check(series.count() == 3 && series.points().back().y() == 4.0, "legacy points changed");
        check(!binding->projectionPending(), "legacy append left stale projection");
        check(binding->history()->select(2), "selection failed");
        binding->setPresentationEnabled(false);
        binding->append({4, 4000, 12.0, true});
        binding->append({5, 5000, 0.0, true});
        binding->refreshSeries();
        check(replacements == 3 && series.count() == 3, "hidden projection was generated");
        check(binding->history()->samples().size() == 5, "hidden samples lost");
        check(binding->history()->samples().back().timestampMs == 5000, "sample timestamp lost");
        check(binding->history()->selectedId() == 2 && !binding->history()->followsLatest(), "hidden append moved selected anchor");
        check(binding->projectionPending(), "hidden history not marked pending");
        const auto hiddenRange = binding->range({});
        check(hiddenRange.hasValidSamples && hiddenRange.maximumY >= 12.0, "hidden peak not included in history range");
        binding->setPresentationEnabled(true);
        check(replacements == 4 && series.count() == 5, "first presentation did not catch up once");
        check(series.points().back().y() == 0.0, "valid zero was discarded");
        check(!binding->projectionPending(), "first presentation did not consume revision");
        binding->setPresentationEnabled(true);
        check(replacements == 4, "clean presentation repeated projection");

        binding->append({6, 6000, 8.0, true}, false);
        check(replacements == 4 && binding->projectionPending(), "append false projected while enabled");
        binding->setPresentationEnabled(true);
        check(replacements == 5 && series.count() == 6, "pending enabled presentation not flushed");
        binding->setPresentationEnabled(false);
        binding->append({6, 6000, 88.0, false});
        binding->setXMode(ks::ui::MetricXMode::RelativeSlot);
        binding->setPresentationEnabled(true);
        check(replacements == 6 && series.count() == 6, "same-id update created extra history row");
        check(std::isnan(series.points().back().y()), "invalid sample lost line break");
        check(series.points().front().x() == 0.0 && series.points().back().x() == 5.0, "deferred x mode not restored");
        check(binding->history()->selectedId() == 2, "projection changed historical selection");

        binding->setPresentationEnabled(false);
        ks::ui::MetricChartBinding::ForSeries(&series, 2);
        check(binding->history()->samples().size() == 2, "capacity trim deferred with presentation");
        check(binding->history()->followsLatest() && binding->history()->selectedId() == 6, "evicted anchor contract changed");
        binding->setPresentationEnabled(true);
        check(series.count() == 2 && series.points().front().x() == 0.0, "trimmed projection not restored");
        binding->history()->clear();
        binding->refreshSeries();
        check(series.count() == 0, "explicit refresh ignored direct history change");
    }

    void valueOwnedHistory()
    {
        auto history = std::make_shared<ks::ui::MetricHistory>(4);
        history->append({10, 1000, 5.0, true});
        history->append({11, 2000, 0.0, false});
        history->select(10);
        QLineSeries source;
        QLineSeries mirror;
        auto* binding = ks::ui::MetricChartBinding::ForSeries(&source, 4);
        int replacements = 0;
        source.setChangeHandler([&] { ++replacements; });
        binding->setPresentationEnabled(false);
        binding->setHistory(history);
        check(binding->history() == history && source.count() == 0, "hidden value history projected or copied");
        check(history->selectedId() == 10 && !history->followsLatest(), "history injection changed selection");
        check(binding->projectionPending(), "injected history not pending");
        binding->setPresentationEnabled(true);
        check(replacements == 1 && source.count() == 2 && std::isnan(source.points().back().y()), "value history missing/invalid values changed");
        check(history->samples().back().timestampMs == 2000, "injection changed timestamps");
        ks::ui::MetricChartBinding::MirrorSeries(&source, &mirror);
        auto* mirrored = ks::ui::MetricChartBinding::Find(&mirror);
        mirrored->setPresentationEnabled(false);
        binding->setHistory(history);
        binding->setHistory({});
        binding->append({12, 3000, 9.0, true}, false);
        check(replacements == 1 && mirrored->projectionPending(), "same/null history broke mirror revision");
        mirrored->setPresentationEnabled(true);
        check(mirror.count() == 3 && mirror.points().back().y() == 9.0, "mirror lost injected source samples");
        auto replacement = std::make_shared<ks::ui::MetricHistory>(2);
        replacement->append({99, 9000, 19.0, true});
        binding->setHistory(replacement);
        check(binding->history() == replacement && source.count() == 1, "replacement source not projected");
        check(mirrored->history() == history && history->samples().size() == 3, "replacement mutated old mirrored history");
        ks::ui::MetricChartBinding::MirrorSeries(&source, &mirror);
        check(mirrored->history() == replacement && mirror.points().back().y() == 19.0, "mirror did not reconnect source identity");
    }

    void mirrorRevision()
    {
        QLineSeries source;
        QLineSeries mirror;
        auto* binding = ks::ui::MetricChartBinding::ForSeries(&source, 4);
        binding->append({1, 1000, 1.0, true});
        ks::ui::MetricChartBinding::MirrorSeries(&source, &mirror);
        auto* mirrored = ks::ui::MetricChartBinding::Find(&mirror);
        int mirrorReplacements = 0;
        mirror.setChangeHandler([&] { ++mirrorReplacements; });
        check(mirrored && binding->history() == mirrored->history(), "mirror copied canonical history");
        mirrored->setPresentationEnabled(false);
        binding->append({2, 2000, 7.0, true}, false);
        check(mirror.count() == 1 && mirrored->projectionPending(), "mirror missed source revision");
        mirrored->setPresentationEnabled(true);
        check(mirrorReplacements == 1 && mirror.count() == 2 && mirror.points().back().y() == 7.0, "mirror did not restore hidden source update");
        mirrored->setPresentationEnabled(true);
        check(mirrorReplacements == 1, "unchanged mirror projected twice");
        mirrored->setPresentationEnabled(false);
        ks::ui::MetricChartBinding::ForSeries(&source, 1);
        check(mirrored->projectionPending(), "source capacity revision not shared");
        mirrored->setPresentationEnabled(true);
        check(mirrorReplacements == 2 && mirror.count() == 1 && mirror.points().front().y() == 7.0, "mirror capacity change stale");

        QLineSeries replacementSource;
        auto* replacement = ks::ui::MetricChartBinding::ForSeries(&replacementSource, 3);
        replacement->setXMode(ks::ui::MetricXMode::ElapsedTime);
        replacement->append({1, 10000, 10.0, true});
        replacement->append({2, 13000, 30.0, true});
        mirrored->setPresentationEnabled(false);
        mirrored->shareHistoryFrom(*replacement);
        check(mirrored->history() == replacement->history() && mirrored->projectionPending(), "new model not marked pending");
        mirrored->setPresentationEnabled(true);
        check(mirror.count() == 2 && mirror.points().back().x() == 3.0 && mirror.points().back().y() == 30.0, "new source coordinates stale");
        check(binding->history()->samples().size() == 1, "mirror replacement mutated old source");
        mirrored->setPresentationEnabled(false);
        replacement->append({3, 14000, 40.0, true}, false);
        const auto sharedRange = ks::ui::MetricChartBinding::SharedRange({&replacementSource, &mirror},
            ks::ui::MetricXMode::ElapsedTime, {});
        check(sharedRange.maximumX == 4.0 && sharedRange.maximumY >= 40.0, "shared range read display points instead of current history");
        mirrored->setPresentationEnabled(true);
        check(mirror.count() == 3 && mirror.points().back().y() == 40.0, "shared revision did not follow new model");
    }
}

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    defaultAndHiddenHistory();
    mirrorRevision();
    valueOwnedHistory();
    std::cout << "METRIC_CHART_BINDING_CHECKS=" << checks << " PASS\n";
    return 0;
}
