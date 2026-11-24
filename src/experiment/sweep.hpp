#pragma once
// Experiment matrix (cartesian product of parameter lists), built-in presets
// that reproduce the proposal's plan plus the extension studies, and the
// figure definitions that turn aggregated results into SVG charts.
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "experiment/runner.hpp"
#include "experiment/svg_chart.hpp"
#include "experiment/table.hpp"

namespace arq {

struct SweepPlan {
    std::vector<ProtocolKind> protocols{ProtocolKind::StopAndWait, ProtocolKind::GoBackN};
    std::vector<int> windows{8};
    std::vector<double> losses{0.0};
    std::vector<double> rtts{50.0};
    std::vector<std::string> modulations{"none"};
    std::vector<double> snrs{20.0};
    std::vector<RtoMode> rtoModes{RtoMode::Fixed};
    std::vector<LossKind> lossKinds{LossKind::Bernoulli};
    std::vector<int> naks{0};
    double ackLossFactor = 1.0;  // ack loss = factor * data loss
    double meanBurst = 4.0;
    double jitterMs = 0.0;
    std::size_t payloadBytes = 1000;
    std::size_t fileBytes = 50 * 1000;
    double rateBps = 2e6;
    double symbolRate = 1e6;
    int repetitions = 5;
    std::uint32_t baseSeed = 2026;
    double maxTimeMs = 300000.0;
    double stallMs = 60000.0;
    int jobs = 1;  // experiments run in parallel, each on its own port block

    std::vector<RunSpec> expand() const;  // one spec per (config, repetition)
};

struct FigureSpec {
    std::string file;
    std::string title;
    std::string x;        // column in the summary table (without suffix)
    std::string y;        // metric name, e.g. "throughput_kbps" (mean/ci added)
    std::string xLabel;
    std::string yLabel;
    std::vector<std::string> series;            // columns forming the legend
    std::map<std::string, std::string> filter;  // fixed values
    std::string overlay;                        // optional dashed metric (e.g. theory_util)
    bool logY = false;
};

struct Preset {
    std::string name;
    std::string description;
    SweepPlan plan;
    std::vector<FigureSpec> figures;
};

std::vector<Preset> builtinPresets();
const Preset* findPreset(const std::string& name);

/// Builds a chart from an aggregated summary table.
LineChart buildFigure(const Table& summary, const FigureSpec& f);

/// Generic figure set for custom sweeps: one chart per varying axis.
std::vector<FigureSpec> autoFigures(const Table& summary);

/// Human-readable label for a series (e.g. "GBN N=8 adaptive").
std::string seriesLabel(const Table& t, std::size_t row, const std::vector<std::string>& cols);

using ProgressFn = std::function<void(std::size_t done, std::size_t total, const RunResult& r)>;

/// Runs a whole plan and returns the raw result table.
Table runSweep(const SweepPlan& plan, const ProgressFn& progress);

}  // namespace arq
