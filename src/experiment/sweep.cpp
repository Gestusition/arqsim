#include "experiment/sweep.hpp"

#include <algorithm>
#include <cstdio>
#include <set>
#include <tuple>

namespace arq {

std::vector<RunSpec> SweepPlan::expand() const {
    std::vector<RunSpec> configs;
    std::set<std::string> seen;
    for (auto proto : protocols)
        for (int win : windows)
            for (double loss : losses)
                for (double rtt : rtts)
                    for (const auto& modName : modulations)
                        for (double snr : snrs)
                            for (auto rto : rtoModes)
                                for (auto lk : lossKinds)
                                    for (int nak : naks) {
                                        RunSpec s;
                                        s.arq.protocol = proto;
                                        s.arq.window = proto == ProtocolKind::StopAndWait ? 1 : win;
                                        s.arq.payloadBytes = payloadBytes;
                                        s.arq.rtoMode = rto;
                                        s.arq.useNak = nak != 0;
                                        s.arq.maxTimeMs = maxTimeMs;
                                        s.arq.stallMs = stallMs;
                                        s.channel.rttMs = rtt;
                                        s.channel.jitterMs = jitterMs;
                                        s.channel.dataLoss = loss;
                                        s.channel.ackLoss = loss * ackLossFactor;
                                        s.channel.lossKind = loss > 0 ? lk : LossKind::Bernoulli;
                                        s.channel.meanBurst = meanBurst;
                                        bool isAuto = false;
                                        parseModulation(modName, s.channel.modulation, isAuto);
                                        s.channel.adaptiveModulation = isAuto;
                                        const bool phy = isAuto || s.channel.modulation != Modulation::None;
                                        s.channel.snrDb = phy ? snr : 0.0;
                                        s.channel.rateBps = rateBps;
                                        s.channel.symbolRate = symbolRate;
                                        s.fileBytes = fileBytes;

                                        char key[256];
                                        std::snprintf(key, sizeof(key), "%d|%d|%g|%g|%s|%g|%d|%d|%d",
                                                      int(proto), s.arq.window, loss, rtt, modName.c_str(),
                                                      s.channel.snrDb, int(rto), int(s.channel.lossKind), nak);
                                        if (!seen.insert(key).second) continue;  // e.g. SW ignores window
                                        configs.push_back(s);
                                    }
    std::vector<RunSpec> out;
    for (int rep = 1; rep <= repetitions; ++rep)
        for (auto s : configs) {
            s.repetition = rep;
            // Same seed for the same repetition across all protocols -> fair comparison.
            s.seed = baseSeed + std::uint32_t(rep) * 1000U;
            out.push_back(s);
        }
    return out;
}

Table runSweep(const SweepPlan& plan, const ProgressFn& progress) {
    Table t(resultColumns());
    auto specs = plan.expand();
    for (std::size_t i = 0; i < specs.size(); ++i) {
        RunResult r = runExperiment(specs[i]);
        t.addRow(resultRow(r));
        if (progress) progress(i + 1, specs.size(), r);
    }
    return t;
}

std::string seriesLabel(const Table& t, std::size_t row, const std::vector<std::string>& cols) {
    std::string out;
    for (const auto& c : cols) {
        std::string v = t.get(row, c);
        std::string part;
        if (c == "protocol") {
            for (char& ch : v) ch = char(std::toupper(static_cast<unsigned char>(ch)));
            part = v;
        } else if (c == "window") {
            if (t.get(row, "protocol") == "sw") continue;
            part = "N=" + v;
        } else if (c == "nak") part = v == "1" ? "+NAK" : "";
        else if (c == "rto_mode") part = v + " RTO";
        else if (c == "loss_model") part = v;
        else if (c == "modulation" || c == "mod_used") {
            for (char& ch : v) ch = char(std::toupper(static_cast<unsigned char>(ch)));
            part = v == "AUTO" ? "AMC" : v;
        } else if (c == "rtt_ms") part = "RTT=" + v + "ms";
        else if (c == "loss") part = "p=" + v;
        else if (c == "snr_db") part = "SNR=" + v + "dB";
        else part = c + "=" + v;
        if (part.empty()) continue;
        if (!out.empty()) out += " ";
        out += part;
    }
    return out.empty() ? "all" : out;
}

LineChart buildFigure(const Table& summary, const FigureSpec& f) {
    LineChart chart;
    chart.title = f.title;
    chart.xLabel = f.xLabel.empty() ? f.x : f.xLabel;
    chart.yLabel = f.yLabel.empty() ? f.y : f.yLabel;
    chart.logY = f.logY;
    Table sub = summary.filter(f.filter);

    std::vector<std::string> order;
    std::map<std::string, ChartSeries> byName;
    std::map<std::string, ChartSeries> overlays;
    for (std::size_t r = 0; r < sub.size(); ++r) {
        std::string name = seriesLabel(sub, r, f.series);
        if (!byName.count(name)) {
            order.push_back(name);
            byName[name].name = name;
        }
        byName[name].points.push_back({sub.num(r, f.x), sub.num(r, f.y + "_mean"), sub.num(r, f.y + "_ci95")});
        if (!f.overlay.empty()) {
            auto& ov = overlays[name];
            ov.name = name + " (theory)";
            ov.dashed = true;
            ov.points.push_back({sub.num(r, f.x), sub.num(r, f.overlay + "_mean"), 0.0});
        }
    }
    int color = 0;
    for (const auto& n : order) {
        byName[n].color = color;
        chart.series.push_back(byName[n]);
        if (!f.overlay.empty()) {
            overlays[n].color = color;  // theory curve shares its measurement's colour
            chart.series.push_back(overlays[n]);
        }
        ++color;
    }
    return chart;
}

std::vector<FigureSpec> autoFigures(const Table& summary) {
    const std::vector<std::pair<std::string, std::string>> axes = {
        {"rtt_ms", "RTT (ms)"}, {"loss", "DATA loss probability"}, {"window", "Window size N"}, {"snr_db", "SNR Es/N0 (dB)"}};
    const std::vector<std::string> legendCandidates = {"protocol", "window",   "rto_mode", "nak",
                                                       "loss_model", "modulation", "rtt_ms", "loss", "snr_db"};
    std::vector<FigureSpec> out;
    for (const auto& ax : axes) {
        if (summary.col(ax.first) < 0 || summary.distinct(ax.first).size() < 2) continue;
        FigureSpec f;
        f.x = ax.first;
        f.xLabel = ax.second;
        for (const auto& c : legendCandidates)
            if (c != ax.first && summary.col(c) >= 0 && summary.distinct(c).size() > 1) f.series.push_back(c);
        if (f.series.empty()) f.series.push_back("protocol");
        for (const auto& metric : {std::make_pair(std::string("throughput_kbps"), std::string("Throughput (kbit/s)")),
                                   std::make_pair(std::string("retransmissions"), std::string("Retransmissions"))}) {
            FigureSpec g = f;
            g.y = metric.first;
            g.yLabel = metric.second;
            g.title = metric.second + " vs " + ax.second;
            g.file = metric.first + "_vs_" + ax.first + ".svg";
            out.push_back(g);
        }
    }
    return out;
}

std::vector<Preset> builtinPresets() {
    std::vector<Preset> ps;
    const auto SW = ProtocolKind::StopAndWait, GBN = ProtocolKind::GoBackN, SR = ProtocolKind::SelectiveRepeat;

    {  // 1) Exactly the experiment matrix promised in the proposal.
        Preset p;
        p.name = "proposal";
        p.description = "Proposal matrix: SW vs GBN(4,8,16), loss 0-20 %, RTT 10-200 ms, 5 runs";
        p.plan.protocols = {SW, GBN};
        p.plan.windows = {4, 8, 16};
        p.plan.losses = {0.0, 0.01, 0.05, 0.10, 0.20};
        p.plan.rtts = {10, 50, 100, 200};
        p.figures = {
            {"throughput_vs_rtt.svg", "Throughput vs RTT (no loss)", "rtt_ms", "throughput_kbps", "RTT (ms)",
             "Throughput (kbit/s)", {"protocol", "window"}, {{"loss", "0"}}, "", false},
            {"throughput_vs_loss.svg", "Throughput vs loss (RTT = 50 ms)", "loss", "throughput_kbps",
             "DATA/ACK loss probability", "Throughput (kbit/s)", {"protocol", "window"}, {{"rtt_ms", "50"}}, "", false},
            {"retransmissions_vs_loss.svg", "Retransmissions vs loss (RTT = 50 ms)", "loss", "retransmissions",
             "DATA/ACK loss probability", "Retransmitted DATA packets", {"protocol", "window"}, {{"rtt_ms", "50"}}, "",
             false},
            {"gbn_throughput_vs_window.svg", "Go-Back-N throughput vs window (loss = 5 %)", "window", "throughput_kbps",
             "Window size N", "Throughput (kbit/s)", {"rtt_ms"}, {{"protocol", "gbn"}, {"loss", "0.05"}}, "", false},
            {"utilization_vs_rtt.svg", "Measured vs theoretical utilization (no loss)", "rtt_ms", "utilization",
             "RTT (ms)", "Link utilization", {"protocol", "window"}, {{"loss", "0"}}, "theory_util", false},
        };
        ps.push_back(p);
    }
    {  // 2) Three-protocol comparison including Selective Repeat.
        Preset p;
        p.name = "protocols";
        p.description = "SW vs GBN vs SR (N=8) under loss, RTT 50 ms, adaptive RTO";
        p.plan.protocols = {SW, GBN, SR};
        p.plan.windows = {8};
        p.plan.losses = {0.0, 0.01, 0.05, 0.10, 0.20};
        p.plan.rtts = {50};
        p.plan.rtoModes = {RtoMode::Adaptive};
        p.figures = {
            {"protocols_throughput_vs_loss.svg", "SW / GBN / SR throughput vs loss", "loss", "throughput_kbps",
             "DATA/ACK loss probability", "Throughput (kbit/s)", {"protocol", "window"}, {}, "", false},
            {"protocols_efficiency_vs_loss.svg", "Transmission efficiency vs loss", "loss", "efficiency",
             "DATA/ACK loss probability", "Useful / total DATA bytes", {"protocol", "window"}, {}, "", false},
            {"protocols_retx_vs_loss.svg", "Retransmissions vs loss", "loss", "retransmissions",
             "DATA/ACK loss probability", "Retransmitted DATA packets", {"protocol", "window"}, {}, "", false},
        };
        ps.push_back(p);
    }
    {  // 3) Physical-layer modulation study.
        Preset p;
        p.name = "modulation";
        p.description = "BPSK/QPSK/16-QAM/64-QAM/AMC over AWGN, SNR 0-30 dB, GBN & SR N=8";
        p.plan.protocols = {GBN, SR};
        p.plan.windows = {8};
        p.plan.rtts = {20};
        p.plan.modulations = {"bpsk", "qpsk", "16qam", "64qam", "auto"};
        p.plan.snrs = {6, 9, 12, 15, 18, 21, 24, 27, 30};
        p.plan.rtoModes = {RtoMode::Adaptive};
        p.plan.repetitions = 3;
        p.plan.maxTimeMs = 30000;
        p.plan.stallMs = 3000;  // hopeless SNR/modulation pairs abort quickly
        p.figures = {
            {"mod_gbn_throughput_vs_snr.svg", "GBN (N=8) throughput vs SNR per modulation", "snr_db", "throughput_kbps",
             "SNR Es/N0 (dB)", "Throughput (kbit/s)", {"modulation"}, {{"protocol", "gbn"}}, "", false},
            {"mod_sr_throughput_vs_snr.svg", "SR (N=8) throughput vs SNR per modulation", "snr_db", "throughput_kbps",
             "SNR Es/N0 (dB)", "Throughput (kbit/s)", {"modulation"}, {{"protocol", "sr"}}, "", false},
            {"mod_retx_vs_snr.svg", "Retransmissions vs SNR (GBN)", "snr_db", "retransmissions", "SNR Es/N0 (dB)",
             "Retransmitted DATA packets", {"modulation"}, {{"protocol", "gbn"}}, "", false},
            {"mod_ber_vs_snr.svg", "Measured channel BER vs SNR", "snr_db", "ber", "SNR Es/N0 (dB)",
             "Bit error rate", {"modulation"}, {{"protocol", "gbn"}}, "", true},
        };
        ps.push_back(p);
    }
    {  // 4) Fixed vs adaptive RTO.
        Preset p;
        p.name = "rto";
        p.description = "Fixed RTO (2xRTT) vs adaptive RFC 6298 RTO for GBN/SR N=16";
        p.plan.protocols = {GBN, SR};
        p.plan.windows = {16};
        p.plan.losses = {0.05};
        p.plan.rtts = {10, 25, 50, 100};
        p.plan.rtoModes = {RtoMode::Fixed, RtoMode::Adaptive};
        p.figures = {
            {"rto_throughput_vs_rtt.svg", "Fixed vs adaptive RTO: throughput", "rtt_ms", "throughput_kbps", "RTT (ms)",
             "Throughput (kbit/s)", {"protocol", "rto_mode"}, {}, "", false},
            {"rto_timeouts_vs_rtt.svg", "Fixed vs adaptive RTO: timeouts", "rtt_ms", "timeouts", "RTT (ms)",
             "Timeout events", {"protocol", "rto_mode"}, {}, "", false},
        };
        ps.push_back(p);
    }
    {  // 5) Independent vs bursty loss.
        Preset p;
        p.name = "burst";
        p.description = "Bernoulli vs Gilbert-Elliott (mean burst 4) loss, SW/GBN/SR";
        p.plan.protocols = {SW, GBN, SR};
        p.plan.windows = {8};
        p.plan.losses = {0.02, 0.05, 0.10, 0.20};
        p.plan.rtts = {50};
        p.plan.lossKinds = {LossKind::Bernoulli, LossKind::GilbertElliott};
        p.plan.rtoModes = {RtoMode::Adaptive};
        p.figures = {
            {"burst_throughput_vs_loss.svg", "Independent vs burst loss", "loss", "throughput_kbps",
             "Average loss probability", "Throughput (kbit/s)", {"protocol", "loss_model"}, {}, "", false},
        };
        ps.push_back(p);
    }
    {  // 6) NAK fast retransmission.
        Preset p;
        p.name = "nak";
        p.description = "Effect of NAK-triggered retransmission on GBN/SR (RTT 100 ms)";
        p.plan.protocols = {GBN, SR};
        p.plan.windows = {8};
        p.plan.losses = {0.01, 0.05, 0.10, 0.20};
        p.plan.rtts = {100};
        p.plan.naks = {0, 1};
        p.plan.rtoModes = {RtoMode::Adaptive};
        p.figures = {
            {"nak_throughput_vs_loss.svg", "NAK vs timeout-only recovery", "loss", "throughput_kbps",
             "DATA/ACK loss probability", "Throughput (kbit/s)", {"protocol", "nak"}, {}, "", false},
        };
        ps.push_back(p);
    }
    {  // 7) Quick smoke/demo run (~1 minute).
        Preset p;
        p.name = "quick";
        p.description = "Fast demo: SW/GBN/SR, 2 loss rates, 2 RTTs, 3 runs, 20 kB file";
        p.plan.protocols = {SW, GBN, SR};
        p.plan.windows = {8};
        p.plan.losses = {0.0, 0.05};
        p.plan.rtts = {10, 50};
        p.plan.repetitions = 3;
        p.plan.fileBytes = 20000;
        p.plan.rtoModes = {RtoMode::Adaptive};
        ps.push_back(p);
    }
    return ps;
}

const Preset* findPreset(const std::string& name) {
    static const std::vector<Preset> ps = builtinPresets();
    for (const auto& p : ps)
        if (p.name == name) return &p;
    return nullptr;
}

}  // namespace arq
