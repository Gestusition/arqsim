// arqsim - experimental ARQ (Stop-and-Wait / Go-Back-N / Selective Repeat)
// performance analysis over UDP with a modulation-aware channel emulator.
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "arq/receiver.hpp"
#include "arq/sender.hpp"
#include "channel/emulator.hpp"
#include "cli/args.hpp"
#include "core/checksum.hpp"
#include "core/clock.hpp"
#include "core/log.hpp"
#include "core/packet.hpp"
#include "experiment/runner.hpp"
#include "experiment/stats.hpp"
#include "experiment/svg_chart.hpp"
#include "experiment/sweep.hpp"
#include "experiment/table.hpp"
#include "net/udp_socket.hpp"
#include "phy/modulation.hpp"

namespace fs = std::filesystem;
using namespace arq;
using arq::cli::Args;

namespace {

constexpr const char* kVersion = "1.0.0";
std::atomic<bool> g_stop{false};

void onSignal(int) { g_stop = true; }

// ----------------------------------------------------------------- help text
void printHelp() {
    std::printf(
        "arqsim %s - ARQ performance lab over UDP with a modulation-aware channel\n\n"
        "USAGE\n"
        "  arqsim <command> [options]\n\n"
        "COMMANDS\n"
        "  run        Run one configuration (sender, channel, receiver over localhost UDP)\n"
        "  sweep      Run an experiment matrix or a built-in preset, write CSV + SVG figures\n"
        "  presets    List built-in experiment presets\n"
        "  ber        BER vs SNR table for BPSK/QPSK/16-QAM/64-QAM (theory + Monte Carlo)\n"
        "  amc        Show which modulation adaptive modulation picks for each SNR\n"
        "  report     Aggregate a raw results CSV (mean, std, 95%% CI) and draw figures\n"
        "  plot       Draw one SVG chart from a summary CSV\n"
        "  sender     Standalone sender process   (UDP %d -> channel %d)\n"
        "  channel    Standalone channel emulator (UDP %d/%d)\n"
        "  receiver   Standalone receiver process (UDP %d -> channel %d)\n"
        "  help       Show this help (or: arqsim <command> --help)\n\n"
        "PROTOCOL OPTIONS\n"
        "  --proto sw|gbn|sr        ARQ protocol                      (default gbn)\n"
        "  --win N                  sender/receiver window            (default 8; SW = 1)\n"
        "  --payload BYTES          payload per DATA packet           (default 1000)\n"
        "  --rto fixed|adaptive     fixed = factor x RTT, adaptive = RFC 6298 (default fixed)\n"
        "  --rto-factor K           fixed RTO multiplier              (default 2)\n"
        "  --nak                    enable NAK-triggered retransmission\n\n"
        "CHANNEL OPTIONS\n"
        "  --rtt MS                 round-trip propagation delay      (default 50)\n"
        "  --jitter MS              extra uniform one-way delay 0..MS (default 0)\n"
        "  --loss P                 DATA loss probability             (default 0)\n"
        "  --ack-loss P             ACK loss probability              (default = --loss)\n"
        "  --loss-model bernoulli|gilbert   independent or burst loss (default bernoulli)\n"
        "  --burst B                mean burst length for gilbert     (default 4)\n"
        "  --rate-mbps R            link rate when --mod none         (default 2)\n\n"
        "PHYSICAL LAYER (MODULATION) OPTIONS\n"
        "  --mod none|bpsk|qpsk|16qam|64qam|auto   modulation, auto = adaptive (AMC)\n"
        "  --snr DB                 Es/N0 of the AWGN channel         (default 20)\n"
        "  --baud SYM_PER_S         symbol rate; R = baud x bits/symbol (default 1e6)\n\n"
        "EXPERIMENT OPTIONS\n"
        "  --file-kb K | --input PATH   test payload size or a real file\n"
        "  --reps N                 repetitions                       (default 1 for run, 5 for sweep)\n"
        "  --seed S                 base pseudo-random seed           (default 2026)\n"
        "  --csv PATH               write raw per-run results\n"
        "  --max-time S, --stall S  abort a run after S s in total / S s without progress (300 / 20)\n"
        "  --verbose                print a packet-level event trace\n\n"
        "EXAMPLES\n"
        "  arqsim run --proto sr --win 8 --loss 0.1 --rtt 50 --reps 5\n"
        "  arqsim run --proto gbn --mod 16qam --snr 14 --rto adaptive\n"
        "  arqsim sweep --preset proposal --out results/proposal\n"
        "  arqsim sweep --proto sw,gbn,sr --loss 0,0.05,0.1 --rtt 20,100 --reps 3 --out results/custom\n"
        "  arqsim ber --snr 0:30:2 --svg results/ber.svg\n"
        "  arqsim channel --loss 0.1 --rtt 100      (then: arqsim receiver --out rx.bin,\n"
        "                                            arqsim sender --input file.bin)\n",
        kVersion, 50000, 50010, 50010, 50011, 50020, 50011);
}

// ------------------------------------------------------------ option mapping
ArqConfig arqFromArgs(const Args& a) {
    ArqConfig c;
    if (!parseProtocol(a.str("proto", "gbn"), c.protocol)) throw std::runtime_error("unknown --proto");
    c.window = int(a.num("win", 8));
    c.payloadBytes = std::size_t(a.num("payload", 1000));
    if (c.payloadBytes < 1 || c.payloadBytes > 60000) throw std::runtime_error("--payload must be 1..60000");
    if (!parseRtoMode(a.str("rto", "fixed"), c.rtoMode)) throw std::runtime_error("unknown --rto");
    c.rtoFactor = a.num("rto-factor", 2.0);
    c.useNak = a.flag("nak");
    c.rttMs = a.num("rtt", 50.0);
    c.maxTimeMs = a.num("max-time", 300.0) * 1000.0;
    c.stallMs = a.num("stall", 20.0) * 1000.0;
    const std::string host = a.str("host", "127.0.0.1");
    c.senderLocal = {host, std::uint16_t(a.num("sender-port", 50000))};
    c.channelData = {host, std::uint16_t(a.num("chan-data-port", 50010))};
    c.channelAck = {host, std::uint16_t(a.num("chan-ack-port", 50011))};
    c.receiverLocal = {host, std::uint16_t(a.num("recv-port", 50020))};
    return c;
}

ChannelConfig channelFromArgs(const Args& a) {
    ChannelConfig c;
    c.rttMs = a.num("rtt", 50.0);
    c.jitterMs = a.num("jitter", 0.0);
    c.dataLoss = a.num("loss", 0.0);
    c.ackLoss = a.num("ack-loss", c.dataLoss);
    if (c.dataLoss < 0 || c.dataLoss >= 1 || c.ackLoss < 0 || c.ackLoss >= 1)
        throw std::runtime_error("loss probabilities must be in [0, 1)");
    const std::string lm = a.str("loss-model", "bernoulli");
    if (lm == "bernoulli" || lm == "iid") c.lossKind = LossKind::Bernoulli;
    else if (lm == "gilbert" || lm == "ge" || lm == "burst") c.lossKind = LossKind::GilbertElliott;
    else throw std::runtime_error("unknown --loss-model");
    c.meanBurst = a.num("burst", 4.0);
    bool isAuto = false;
    if (!parseModulation(a.str("mod", "none"), c.modulation, isAuto)) throw std::runtime_error("unknown --mod");
    c.adaptiveModulation = isAuto;
    c.snrDb = a.num("snr", 20.0);
    c.symbolRate = a.num("baud", 1e6);
    c.rateBps = a.num("rate-mbps", 2.0) * 1e6;
    c.seed = std::uint32_t(a.num("seed", 2026));
    const std::string host = a.str("host", "127.0.0.1");
    c.sender = {host, std::uint16_t(a.num("sender-port", 50000))};
    c.dataIn = {host, std::uint16_t(a.num("chan-data-port", 50010))};
    c.ackIn = {host, std::uint16_t(a.num("chan-ack-port", 50011))};
    c.receiver = {host, std::uint16_t(a.num("recv-port", 50020))};
    return c;
}

std::vector<std::uint8_t> readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open input file " + path);
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

void warnUnused(const Args& a) {
    for (const auto& k : a.unused()) std::fprintf(stderr, "warning: unknown option --%s ignored\n", k.c_str());
}

void ensureParent(const std::string& path) {
    fs::path p(path);
    if (p.has_parent_path()) fs::create_directories(p.parent_path());
}

std::string describe(const RunSpec& s, Modulation used) {
    char b[256];
    const auto& c = s.channel;
    std::string phy = "ideal PHY";
    if (c.modulation != Modulation::None || c.adaptiveModulation) {
        char p[64];
        std::snprintf(p, sizeof(p), "%s%s @ %.1f dB", c.adaptiveModulation ? "AMC->" : "", toString(used), c.snrDb);
        phy = p;
    }
    std::snprintf(b, sizeof(b), "%s N=%d | loss %.3f (%s) | RTT %.0f ms | RTO %s%s | %s",
                  toString(s.arq.protocol), s.arq.effectiveWindow(), c.dataLoss, toString(c.lossKind), c.rttMs,
                  toString(s.arq.rtoMode), s.arq.useNak ? " +NAK" : "", phy.c_str());
    return b;
}

// --------------------------------------------------------------------- run
int cmdRun(const Args& a) {
    RunSpec spec;
    spec.arq = arqFromArgs(a);
    spec.channel = channelFromArgs(a);
    spec.fileBytes = std::size_t(a.num("file-kb", 50) * 1000);
    const int reps = int(a.num("reps", 1));
    const std::uint32_t seed = std::uint32_t(a.num("seed", 2026));
    const std::string csv = a.str("csv", "");
    const std::string input = a.str("input", "");
    Trace::instance().enable(a.flag("verbose"));
    warnUnused(a);

    std::vector<std::uint8_t> file;
    if (!input.empty()) file = readFile(input);

    Table raw(resultColumns());
    std::vector<double> thr, tt, retx, to, eff;
    for (int rep = 1; rep <= reps; ++rep) {
        spec.repetition = rep;
        spec.seed = seed + std::uint32_t(rep) * 1000U;
        RunResult r = runExperiment(spec, input.empty() ? nullptr : &file);
        raw.addRow(resultRow(r));
        if (rep == 1) {
            std::printf("\n  %s\n", describe(spec, r.modulationUsed).c_str());
            std::printf("  file %zu bytes in %llu packets, link rate %.0f kbit/s\n\n", r.spec.fileBytes,
                        (unsigned long long)r.packets, r.linkRateBps / 1000.0);
            std::printf("  %-4s %-9s %11s %12s %7s %6s %6s %8s %7s %9s\n", "rep", "integrity", "time(ms)",
                        "thrpt(kb/s)", "data_tx", "retx", "tmo", "effic.", "util", "util_th");
        }
        std::printf("  %-4d %-9s %11.1f %12.1f %7llu %6llu %6llu %8.3f %7.3f %9.3f\n", rep,
                    r.integrityPass ? "PASS" : (r.completed ? "FAIL" : "ABORT"), r.transferMs, r.throughputKbps,
                    (unsigned long long)r.dataTx, (unsigned long long)r.retransmissions,
                    (unsigned long long)r.timeouts, r.efficiency, r.utilization, r.theoryUtilization);
        if (r.chDataCorrupted + r.chAckCorrupted > 0 || r.measuredBer > 0)
            std::printf("       PHY: %llu DATA / %llu ACK frames corrupted, measured BER %.2e\n",
                        (unsigned long long)r.chDataCorrupted, (unsigned long long)r.chAckCorrupted, r.measuredBer);
        thr.push_back(r.throughputKbps);
        tt.push_back(r.transferMs);
        retx.push_back(double(r.retransmissions));
        to.push_back(double(r.timeouts));
        eff.push_back(r.efficiency);
    }
    if (reps > 1) {
        auto show = [](const char* name, const std::vector<double>& v, const char* unit) {
            Summary s = summarize(v);
            std::printf("  %-16s mean %10.2f  std %8.2f  95%% CI +-%8.2f %s\n", name, s.mean, s.stddev, s.ci95, unit);
        };
        std::printf("\n");
        show("throughput", thr, "kbit/s");
        show("transfer time", tt, "ms");
        show("retransmissions", retx, "");
        show("timeouts", to, "");
        show("efficiency", eff, "");
    }
    std::printf("\n");
    if (!csv.empty()) {
        ensureParent(csv);
        raw.writeCsv(csv);
        std::printf("  raw results written to %s\n", csv.c_str());
    }
    return 0;
}

// -------------------------------------------------------------- report/plot
void writeFigures(const Table& summary, const std::vector<FigureSpec>& figs, const fs::path& dir) {
    for (const auto& f : figs) {
        LineChart c = buildFigure(summary, f);
        if (c.series.empty()) continue;
        fs::path p = dir / f.file;
        c.save(p.string());
        std::printf("  figure  %s\n", p.string().c_str());
    }
}

void printSummary(const Table& summary) {
    std::vector<std::pair<std::string, double>> bars;
    std::vector<std::string> legend = {"protocol", "window", "rto_mode", "nak", "loss_model", "modulation"};
    std::vector<std::string> axes = {"loss", "rtt_ms", "snr_db"};
    std::vector<std::string> varying;
    for (const auto& c : legend)
        if (summary.col(c) >= 0 && summary.distinct(c).size() > 1) varying.push_back(c);
    if (varying.empty()) varying.push_back("protocol");
    for (const auto& c : axes)
        if (summary.col(c) >= 0 && summary.distinct(c).size() > 1) varying.push_back(c);
    std::printf("\n  %-46s %5s %12s %9s %8s %8s %9s\n", "configuration", "runs", "thrpt kb/s", "+-CI95", "retx",
                "tmo", "integrity");
    for (std::size_t r = 0; r < summary.size(); ++r) {
        std::string label = seriesLabel(summary, r, varying);
        if (label.size() > 46) label = label.substr(0, 46);
        std::printf("  %-46s %5s %12.1f %9.1f %8.1f %8.1f %8.0f%%\n", label.c_str(), summary.get(r, "runs").c_str(),
                    summary.num(r, "throughput_kbps_mean"), summary.num(r, "throughput_kbps_ci95"),
                    summary.num(r, "retransmissions_mean"), summary.num(r, "timeouts_mean"),
                    100.0 * summary.num(r, "integrity_mean"));
        if (summary.size() <= 24) bars.emplace_back(label, summary.num(r, "throughput_kbps_mean"));
    }
    if (!bars.empty()) std::printf("\n%s", asciiBars(bars, "kbit/s", 36).c_str());
}

int cmdReport(const Args& a) {
    if (a.positional().empty()) throw std::runtime_error("usage: arqsim report <raw.csv> [--out DIR]");
    const std::string in = a.positional()[0];
    fs::path dir = a.str("out", fs::path(in).parent_path().string());
    if (dir.empty()) dir = ".";
    warnUnused(a);
    fs::create_directories(dir);
    Table raw = Table::readCsv(in);
    Table summary = raw.aggregate(configColumns(), metricColumns());
    summary.writeCsv((dir / "summary.csv").string());
    printSummary(summary);
    std::printf("\n  summary %s\n", (dir / "summary.csv").string().c_str());
    writeFigures(summary, autoFigures(summary), dir);
    return 0;
}

int cmdPlot(const Args& a) {
    if (a.positional().empty())
        throw std::runtime_error(
            "usage: arqsim plot <summary.csv> --x COL --y METRIC [--series a,b] [--where k=v,...] [--log] --out f.svg");
    Table summary = Table::readCsv(a.positional()[0]);
    if (summary.col("runs") < 0) summary = summary.aggregate(configColumns(), metricColumns());
    FigureSpec f;
    f.x = a.str("x", "loss");
    f.y = a.str("y", "throughput_kbps");
    f.series = a.list("series", "protocol,window");
    for (const auto& kv : a.list("where", "")) {
        auto eq = kv.find('=');
        if (eq != std::string::npos) f.filter[kv.substr(0, eq)] = kv.substr(eq + 1);
    }
    f.logY = a.flag("log");
    f.title = a.str("title", f.y + " vs " + f.x);
    f.xLabel = a.str("xlabel", f.x);
    f.yLabel = a.str("ylabel", f.y);
    f.overlay = a.str("overlay", "");
    const std::string out = a.str("out", "plot.svg");
    warnUnused(a);
    ensureParent(out);
    LineChart c = buildFigure(summary, f);
    if (c.series.empty()) throw std::runtime_error("no rows match the given --where filter");
    c.save(out);
    std::printf("  figure %s (%zu series)\n", out.c_str(), c.series.size());
    return 0;
}

// -------------------------------------------------------------------- sweep
std::vector<ProtocolKind> protoList(const std::vector<std::string>& in) {
    std::vector<ProtocolKind> out;
    for (const auto& s : in) {
        ProtocolKind p;
        if (!parseProtocol(s, p)) throw std::runtime_error("unknown protocol " + s);
        out.push_back(p);
    }
    return out;
}

int cmdPresets() {
    std::printf("\n  Built-in presets (arqsim sweep --preset NAME):\n\n");
    for (const auto& p : builtinPresets()) {
        auto n = p.plan.expand().size();
        std::printf("  %-11s %-78s %4zu runs\n", p.name.c_str(), p.description.c_str(), n);
    }
    std::printf("\n  'all' runs every preset except 'quick'. Override repetitions with --reps,\n"
                "  file size with --file-kb.\n\n");
    return 0;
}

int runPreset(const Preset& preset, const Args& a, const fs::path& outRoot) {
    SweepPlan plan = preset.plan;
    if (a.has("reps")) plan.repetitions = int(a.num("reps", plan.repetitions));
    if (a.has("file-kb")) plan.fileBytes = std::size_t(a.num("file-kb", 50) * 1000);
    if (a.has("seed")) plan.baseSeed = std::uint32_t(a.num("seed", plan.baseSeed));
    fs::path dir = outRoot;
    fs::create_directories(dir);

    auto specs = plan.expand();
    std::printf("\n== preset '%s': %s\n   %zu runs -> %s\n\n", preset.name.c_str(), preset.description.c_str(),
                specs.size(), dir.string().c_str());
    const auto t0 = Clock::now();
    Table raw = runSweep(plan, [&](std::size_t done, std::size_t total, const RunResult& r) {
        double elapsed = msBetween(t0, Clock::now()) / 1000.0;
        double eta = elapsed / double(done) * double(total - done);
        std::printf("  [%4zu/%zu] %-80s %9.1f kb/s %s  (eta %4.0fs)\n", done, total,
                    describe(r.spec, r.modulationUsed).c_str(), r.throughputKbps, r.integrityPass ? "ok" : "FAIL",
                    eta);
        std::fflush(stdout);
    });
    raw.writeCsv((dir / "raw.csv").string());
    Table summary = raw.aggregate(configColumns(), metricColumns());
    summary.writeCsv((dir / "summary.csv").string());
    printSummary(summary);
    std::printf("\n  raw     %s\n  summary %s\n", (dir / "raw.csv").string().c_str(),
                (dir / "summary.csv").string().c_str());
    writeFigures(summary, preset.figures.empty() ? autoFigures(summary) : preset.figures, dir);
    if (preset.name == "modulation") {
        // Companion theoretical/simulated BER curves for the report.
        LineChart ber;
        ber.title = "BER vs SNR in AWGN (lines: theory, dashed: simulation)";
        ber.xLabel = "SNR Es/N0 (dB)";
        ber.yLabel = "Bit error rate";
        ber.logY = true;
        for (Modulation m : allModulations()) {
            ChartSeries th{std::string(toString(m)) + " theory", {}, false};
            ChartSeries sim{std::string(toString(m)) + " sim", {}, true};
            for (double snr = 0; snr <= 30; snr += 2) {
                BerPoint p = measureBer(m, snr, 400000, 7);
                if (p.theory > 1e-7) th.points.push_back({snr, p.theory, 0});
                if (p.simulated > 0) sim.points.push_back({snr, p.simulated, 0});
            }
            ber.series.push_back(th);
            ber.series.push_back(sim);
        }
        ber.save((dir / "ber_vs_snr.svg").string());
        std::printf("  figure  %s\n", (dir / "ber_vs_snr.svg").string().c_str());
    }
    std::printf("  finished in %.1f s\n", msBetween(t0, Clock::now()) / 1000.0);
    return 0;
}

int cmdSweep(const Args& a) {
    const fs::path out = a.str("out", "results");
    if (a.has("preset")) {
        const std::string name = a.str("preset");
        a.str("reps"); a.str("file-kb"); a.str("seed");
        warnUnused(a);
        if (name == "all") {
            for (const auto& p : builtinPresets())
                if (p.name != "quick") runPreset(p, a, out / p.name);
            return 0;
        }
        const Preset* p = findPreset(name);
        if (!p) throw std::runtime_error("unknown preset '" + name + "' (see: arqsim presets)");
        return runPreset(*p, a, out);
    }
    Preset custom;
    custom.name = "custom";
    custom.description = "custom sweep";
    SweepPlan& plan = custom.plan;
    plan.protocols = protoList(a.list("proto", "sw,gbn,sr"));
    plan.windows.clear();
    for (double v : a.numList("win", "8")) plan.windows.push_back(int(v));
    plan.losses = a.numList("loss", "0,0.05,0.1");
    plan.rtts = a.numList("rtt", "50");
    plan.modulations = a.list("mod", "none");
    plan.snrs = a.numList("snr", "20");
    plan.rtoModes.clear();
    for (const auto& s : a.list("rto", "fixed")) {
        RtoMode m;
        if (!parseRtoMode(s, m)) throw std::runtime_error("unknown rto mode " + s);
        plan.rtoModes.push_back(m);
    }
    plan.lossKinds.clear();
    for (const auto& s : a.list("loss-model", "bernoulli"))
        plan.lossKinds.push_back(s == "gilbert" || s == "ge" || s == "burst" ? LossKind::GilbertElliott
                                                                             : LossKind::Bernoulli);
    plan.naks.clear();
    for (double v : a.numList("nak", "0")) plan.naks.push_back(int(v));
    plan.meanBurst = a.num("burst", 4.0);
    plan.jitterMs = a.num("jitter", 0.0);
    plan.payloadBytes = std::size_t(a.num("payload", 1000));
    plan.rateBps = a.num("rate-mbps", 2.0) * 1e6;
    plan.symbolRate = a.num("baud", 1e6);
    plan.ackLossFactor = a.num("ack-loss-factor", 1.0);
    plan.repetitions = int(a.num("reps", 5));
    plan.fileBytes = std::size_t(a.num("file-kb", 50) * 1000);
    plan.baseSeed = std::uint32_t(a.num("seed", 2026));
    plan.maxTimeMs = a.num("max-time", 300.0) * 1000.0;
    plan.stallMs = a.num("stall", 20.0) * 1000.0;
    warnUnused(a);
    return runPreset(custom, a, out);
}

// ---------------------------------------------------------------- ber / amc
int cmdBer(const Args& a) {
    std::vector<Modulation> mods;
    for (const auto& s : a.list("mod", "bpsk,qpsk,16qam,64qam")) {
        Modulation m;
        bool isAuto;
        if (!parseModulation(s, m, isAuto) || m == Modulation::None || isAuto)
            throw std::runtime_error("ber: --mod must list bpsk,qpsk,16qam,64qam");
        mods.push_back(m);
    }
    auto snrs = a.numList("snr", "0:24:2");
    const auto bits = std::uint64_t(a.num("bits", 1e6));
    const std::string csv = a.str("csv", ""), svg = a.str("svg", "");
    const auto seed = std::uint32_t(a.num("seed", 2026));
    warnUnused(a);

    Table t({"modulation", "snr_db", "ber_theory", "ber_sim", "bits", "errors", "fer_1014B"});
    LineChart chart;
    chart.title = "BER vs SNR in AWGN (lines: theory, dashed: Monte Carlo)";
    chart.xLabel = "SNR Es/N0 (dB)";
    chart.yLabel = "Bit error rate";
    chart.logY = true;

    std::printf("\n  %-8s", "SNR(dB)");
    for (Modulation m : mods) std::printf(" | %-9s theory %-6s sim", toString(m), "");
    std::printf("\n");
    std::vector<std::vector<BerPoint>> pts(mods.size());
    for (std::size_t i = 0; i < mods.size(); ++i)
        for (double s : snrs) pts[i].push_back(measureBer(mods[i], s, bits, seed));
    for (std::size_t k = 0; k < snrs.size(); ++k) {
        std::printf("  %-8.1f", snrs[k]);
        for (std::size_t i = 0; i < mods.size(); ++i)
            std::printf(" | %12.3e %12.3e", pts[i][k].theory, pts[i][k].simulated);
        std::printf("\n");
    }
    for (std::size_t i = 0; i < mods.size(); ++i) {
        ChartSeries th{std::string(toString(mods[i])) + " theory", {}, false};
        ChartSeries sim{std::string(toString(mods[i])) + " sim", {}, true};
        for (const auto& p : pts[i]) {
            char b1[32], b2[32], b3[32];
            std::snprintf(b1, sizeof(b1), "%.6e", p.theory);
            std::snprintf(b2, sizeof(b2), "%.6e", p.simulated);
            std::snprintf(b3, sizeof(b3), "%.6e", frameErrorRate(p.theory, 1014 * 8));
            t.addRow({toString(mods[i]), std::to_string(p.snrDb), b1, b2, std::to_string(p.bits),
                      std::to_string(p.errors), b3});
            if (p.theory > 1e-8) th.points.push_back({p.snrDb, p.theory, 0});
            if (p.simulated > 0) sim.points.push_back({p.snrDb, p.simulated, 0});
        }
        chart.series.push_back(th);
        chart.series.push_back(sim);
    }
    if (!csv.empty()) {
        ensureParent(csv);
        t.writeCsv(csv);
        std::printf("\n  table  %s", csv.c_str());
    }
    if (!svg.empty()) {
        ensureParent(svg);
        chart.save(svg);
        std::printf("\n  figure %s", svg.c_str());
    }
    std::printf("\n\n");
    return 0;
}

int cmdAmc(const Args& a) {
    auto snrs = a.numList("snr", "0:30:2");
    const auto frame = std::size_t(a.num("payload", 1000)) + kHeaderSize;
    const double baud = a.num("baud", 1e6);
    warnUnused(a);
    std::printf("\n  Adaptive modulation: maximise bits/symbol x (1 - FER) for %zu-byte frames, %.0f baud\n\n",
                frame, baud);
    std::printf("  %-8s", "SNR(dB)");
    for (Modulation m : allModulations()) std::printf(" %9s FER", toString(m));
    std::printf("   %-7s %10s\n", "choice", "goodput");
    for (double s : snrs) {
        std::printf("  %-8.1f", s);
        for (Modulation m : allModulations())
            std::printf(" %13.2e", frameErrorRate(theoreticalBer(m, s), frame * 8));
        Modulation c = selectAdaptive(s, frame);
        double g = baud * bitsPerSymbol(c) * (1 - frameErrorRate(theoreticalBer(c, s), frame * 8));
        std::printf("   %-7s %7.0f kb/s\n", toString(c), g / 1000.0);
    }
    std::printf("\n");
    return 0;
}

// ------------------------------------------------- standalone 3-process mode
int cmdChannel(const Args& a) {
    ChannelConfig cfg = channelFromArgs(a);
    const std::size_t frame = std::size_t(a.num("payload", 1000)) + kHeaderSize;
    Trace::instance().enable(a.flag("verbose"));
    warnUnused(a);
    ChannelEmulator ch(cfg, frame);
    std::printf("channel: DATA in :%u -> receiver :%u | ACK in :%u -> sender :%u\n", cfg.dataIn.port,
                cfg.receiver.port, cfg.ackIn.port, cfg.sender.port);
    std::printf("channel: RTT %.1f ms, loss %.3f/%.3f (%s), link %.0f kbit/s, PHY %s%s\n", cfg.rttMs, cfg.dataLoss,
                cfg.ackLoss, toString(cfg.lossKind), ch.linkRateBps() / 1000.0, toString(ch.modulation()),
                ch.modulation() == Modulation::None ? "" : (" @ " + std::to_string(cfg.snrDb) + " dB").c_str());
    std::printf("channel: running, press Ctrl+C to stop\n");
    std::signal(SIGINT, onSignal);
    std::thread t([&] { ch.run(g_stop); });
    t.join();
    const auto& s = ch.stats();
    std::printf("\nchannel stats: DATA in %llu, dropped %llu, corrupted %llu | ACK in %llu, dropped %llu, corrupted %llu\n",
                (unsigned long long)s.dataIn.load(), (unsigned long long)s.dataDropped.load(),
                (unsigned long long)s.dataCorrupted.load(), (unsigned long long)s.ackIn.load(),
                (unsigned long long)s.ackDropped.load(), (unsigned long long)s.ackCorrupted.load());
    return 0;
}

int cmdReceiver(const Args& a) {
    ArqConfig cfg = arqFromArgs(a);
    cfg.session = 0;  // accept the first session we see
    const std::string out = a.str("out", "received.bin");
    const double idle = a.num("idle", 120.0) * 1000.0;
    Trace::instance().enable(a.flag("verbose"));
    warnUnused(a);
    ArqReceiver rx(cfg);
    std::signal(SIGINT, onSignal);
    std::printf("receiver: %s N=%d listening on :%u, ACKs -> channel :%u\n", toString(cfg.protocol),
                cfg.effectiveWindow(), cfg.receiverLocal.port, cfg.channelAck.port);
    ReceiverStats st = rx.run(g_stop, idle);
    ensureParent(out);
    std::ofstream f(out, std::ios::binary);
    f.write(reinterpret_cast<const char*>(rx.data().data()), std::streamsize(rx.data().size()));
    std::printf("receiver: %s, %zu bytes -> %s\n", st.finished ? "FIN received" : "stopped", rx.data().size(),
                out.c_str());
    std::printf("receiver: sha256 %s\n", Sha256::hex(rx.data()).c_str());
    std::printf("receiver: data %llu, corrupted %llu, duplicates %llu, out-of-order %llu, ACK %llu, NAK %llu\n",
                (unsigned long long)st.dataReceived, (unsigned long long)st.corrupted,
                (unsigned long long)st.duplicates, (unsigned long long)st.outOfOrder,
                (unsigned long long)st.acksSent, (unsigned long long)st.naksSent);
    return st.finished ? 0 : 1;
}

int cmdSender(const Args& a) {
    ArqConfig cfg = arqFromArgs(a);
    const std::string input = a.str("input", "");
    const std::size_t kb = std::size_t(a.num("file-kb", 50));
    const auto seed = std::uint32_t(a.num("seed", 2026));
    Trace::instance().enable(a.flag("verbose"));
    warnUnused(a);
    std::vector<std::uint8_t> file = input.empty() ? makeTestFile(kb * 1000, seed) : readFile(input);
    cfg.session = std::uint16_t(1 + (std::chrono::steady_clock::now().time_since_epoch().count() % 65534));
    std::printf("sender: %s N=%d, RTO %s, %zu bytes -> channel :%u\n", toString(cfg.protocol), cfg.effectiveWindow(),
                toString(cfg.rtoMode), file.size(), cfg.channelData.port);
    std::printf("sender: sha256 %s\n", Sha256::hex(file).c_str());
    auto s = ArqSender::create(cfg);
    SenderStats st = s->run(file);
    std::printf("sender: %s in %.1f ms, throughput %.1f kbit/s\n", st.completed ? "completed" : "ABORTED",
                st.transferMs, st.transferMs > 0 ? double(file.size()) * 8.0 / st.transferMs : 0.0);
    std::printf("sender: data tx %llu, retx %llu, timeouts %llu, ACK %llu, NAK %llu, mean RTT %.2f ms, final RTO %.1f ms\n",
                (unsigned long long)st.dataTransmissions, (unsigned long long)st.retransmissions,
                (unsigned long long)st.timeouts, (unsigned long long)st.acksReceived,
                (unsigned long long)st.naksReceived, st.meanRttMs, st.finalRtoMs);
    return st.completed ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IOLBF, 1 << 16);  // line-buffered even when redirected
    if (argc < 2) {
        printHelp();
        return 0;
    }
    const std::string cmd = argv[1];
    try {
        Args args(argc, argv, 2);
        if (cmd == "help" || cmd == "--help" || cmd == "-h" || args.has("help")) {
            printHelp();
            return 0;
        }
        if (cmd == "version" || cmd == "--version") {
            std::printf("arqsim %s\n", kVersion);
            return 0;
        }
        NetworkInit net;
        HighResTimerScope timer;
        if (cmd == "run") return cmdRun(args);
        if (cmd == "sweep") return cmdSweep(args);
        if (cmd == "presets") return cmdPresets();
        if (cmd == "ber") return cmdBer(args);
        if (cmd == "amc") return cmdAmc(args);
        if (cmd == "report") return cmdReport(args);
        if (cmd == "plot") return cmdPlot(args);
        if (cmd == "channel") return cmdChannel(args);
        if (cmd == "sender") return cmdSender(args);
        if (cmd == "receiver") return cmdReceiver(args);
        std::fprintf(stderr, "unknown command '%s' (try: arqsim help)\n", cmd.c_str());
        return 2;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
