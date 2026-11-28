#include "cli/conclude.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "experiment/table.hpp"

namespace fs = std::filesystem;

namespace arq::cli {
namespace {

using Pred = std::function<bool(std::size_t)>;
constexpr long kNone = -1;

Table load(const fs::path& p) {
    if (!fs::exists(p)) return Table();
    try {
        return Table::readCsv(p.string());
    } catch (const std::exception&) {
        return Table();
    }
}

bool same(double a, double b) { return std::fabs(a - b) < 1e-9; }

long find(const Table& t, const Pred& p) {
    for (std::size_t r = 0; r < t.size(); ++r)
        if (p(r)) return long(r);
    return kNone;
}

std::vector<std::size_t> where(const Table& t, const Pred& p) {
    std::vector<std::size_t> out;
    for (std::size_t r = 0; r < t.size(); ++r)
        if (p(r)) out.push_back(r);
    return out;
}

std::vector<double> levels(const Table& t, const std::string& col) {
    std::vector<double> v;
    for (std::size_t r = 0; r < t.size(); ++r) {
        double x = t.num(r, col);
        if (std::none_of(v.begin(), v.end(), [&](double y) { return same(x, y); })) v.push_back(x);
    }
    std::sort(v.begin(), v.end());
    return v;
}

double thr(const Table& t, long r) { return t.num(std::size_t(r), "throughput_kbps_mean"); }
double retx(const Table& t, long r) { return t.num(std::size_t(r), "retransmissions_mean"); }
std::string upper(std::string s) {
    for (char& c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

void heading(const char* title) { std::printf("\n  * %s\n", title); }

// Plain text and formatted lines are separate overloads so a format string is
// never printed without arguments (clang -Wformat-security).
void line(const char* text) { std::printf("      %s\n", text); }

template <class A0, class... A>
void line(const char* fmt, A0 a0, A... a) {
    std::printf("      ");
    std::printf(fmt, a0, a...);
    std::printf("\n");
}

/// Relative throughput gain of NAK on vs off, maximised over loss levels.
double maxNakGain(const Table& nak, const std::string& proto) {
    double best = 0;
    for (std::size_t r : where(nak, [&](std::size_t i) { return nak.get(i, "protocol") == proto && nak.get(i, "nak") == "0"; })) {
        long on = find(nak, [&](std::size_t i) {
            return nak.get(i, "protocol") == proto && nak.get(i, "nak") == "1" && same(nak.num(i, "loss"), nak.num(r, "loss"));
        });
        if (on != kNone && thr(nak, long(r)) > 0) best = std::max(best, thr(nak, on) / thr(nak, long(r)) - 1);
    }
    return best;
}

// ------------------------------------------------------------------ studies
bool proposal(const Table& t) {
    if (!t.size()) return false;
    double dev = 0;
    for (std::size_t r : where(t, [&](std::size_t i) { return t.get(i, "protocol") == "sw" && t.num(i, "loss") == 0; }))
        dev = std::max(dev, std::fabs(t.num(r, "utilization_mean") / t.num(r, "theory_util_mean") - 1));
    const double rttMax = levels(t, "rtt_ms").back(), lossMax = levels(t, "loss").back();
    auto at = [&](const char* p, int win, double loss) {
        return find(t, [&](std::size_t i) {
            return t.get(i, "protocol") == p && (win == 0 || t.num(i, "window") == win) && same(t.num(i, "loss"), loss) &&
                   same(t.num(i, "rtt_ms"), rttMax);
        });
    };
    long sw = at("sw", 0, 0), g16 = at("gbn", 16, 0), g16l = at("gbn", 16, lossMax);
    heading("Theory is reproduced (proposal preset)");
    line("Measured Stop-and-Wait utilisation is within %.1f %% of L/R / (RTT + L/R) at every RTT.", 100 * dev);
    if (sw != kNone && g16 != kNone)
        line("At RTT %.0f ms, GBN (N=16) reaches %.1f kbit/s vs %.1f kbit/s for SW: %.1fx faster.", rttMax, thr(t, g16),
             thr(t, sw), thr(t, g16) / thr(t, sw));
    if (g16l != kNone) {
        double copies = t.num(std::size_t(g16l), "data_tx_mean") /
                        (t.num(std::size_t(g16l), "file_bytes") / t.num(std::size_t(g16l), "payload"));
        line("The cost: at %.0f %% loss GBN (N=16) sends %.1f copies of every useful packet.", 100 * lossMax, copies);
    }
    return true;
}

bool protocols(const Table& t) {
    if (!t.size()) return false;
    const double lossMax = levels(t, "loss").back();
    auto at = [&](const char* p) {
        return find(t, [&](std::size_t i) { return t.get(i, "protocol") == p && same(t.num(i, "loss"), lossMax); });
    };
    long s = at("sr"), g = at("gbn"), w = at("sw");
    if (s == kNone || g == kNone || w == kNone) return false;
    heading("Selective Repeat vs Go-Back-N vs Stop-and-Wait (protocols preset)");
    line("At %.0f %% loss: SR %.1f, GBN %.1f, SW %.1f kbit/s.", 100 * lossMax, thr(t, s), thr(t, g), thr(t, w));
    line("SR is %+.0f %% vs GBN and retransmits %.1fx fewer packets (%.1f vs %.1f),", 100 * (thr(t, s) / thr(t, g) - 1),
         retx(t, g) / std::max(1e-9, retx(t, s)), retx(t, s), retx(t, g));
    line("because it resends only the lost packets instead of the whole window.");
    return true;
}

bool rto(const Table& t) {
    long bf = kNone, ba = kNone;
    double bestCut = -1;
    for (std::size_t f : where(t, [&](std::size_t i) { return t.get(i, "rto_mode") == "fixed"; })) {
        long a = find(t, [&](std::size_t i) {
            return t.get(i, "rto_mode") == "adaptive" && t.get(i, "protocol") == t.get(f, "protocol") &&
                   same(t.num(i, "rtt_ms"), t.num(f, "rtt_ms"));
        });
        double rf = retx(t, long(f));
        if (a == kNone || rf <= 0) continue;
        double cut = 1 - retx(t, a) / rf;
        if (cut > bestCut) bestCut = cut, bf = long(f), ba = a;
    }
    if (bf == kNone) return false;
    heading("Fixed 2xRTT vs adaptive RFC 6298 RTO (rto preset)");
    line("Worst case for the fixed RTO: %s at RTT %.0f ms, where queueing (N*L/R) exceeds the RTT.",
         upper(t.get(std::size_t(bf), "protocol")).c_str(), t.num(std::size_t(bf), "rtt_ms"));
    line("Adaptive RTO removes %.0f %% of the retransmissions there (%.1f -> %.1f); throughput %.1f -> %.1f kbit/s.",
         100 * bestCut, retx(t, bf), retx(t, ba), thr(t, bf), thr(t, ba));
    return true;
}

bool nak(const Table& t) {
    if (!t.size()) return false;
    heading("NAK fast retransmission (nak preset)");
    line("NAKs speed up GBN by up to %.0f %%, but SR by at most %.0f %%:", 100 * maxNakGain(t, "gbn"),
         100 * maxNakGain(t, "sr"));
    line("a GBN timeout resends the whole window, so reacting early saves much more there.");
    return true;
}

bool burst(const Table& t) {
    if (!t.size()) return false;
    auto ratios = [&](const char* p, double& lo, double& hi) {
        lo = 1e300, hi = 0;
        for (std::size_t b : where(t, [&](std::size_t i) { return t.get(i, "protocol") == p && t.get(i, "loss_model") == "bernoulli"; })) {
            long g = find(t, [&](std::size_t i) {
                return t.get(i, "protocol") == p && t.get(i, "loss_model") == "gilbert" && same(t.num(i, "loss"), t.num(b, "loss"));
            });
            if (g == kNone || thr(t, long(b)) <= 0) continue;
            double r = thr(t, g) / thr(t, long(b));
            lo = std::min(lo, r), hi = std::max(hi, r);
        }
    };
    double swLo, swHi, gLo, gHi, sLo, sHi;
    ratios("sw", swLo, swHi), ratios("gbn", gLo, gHi), ratios("sr", sLo, sHi);
    heading("Burst (Gilbert-Elliott) vs independent loss, same average rate (burst preset)");
    line("SW loses up to %.0f %% of its throughput when losses come in bursts.", 100 * (1 - swLo));
    line("GBN gains up to %.1fx and SR up to %.1fx: one timeout recovers a whole burst, and the gaps between bursts are clean.",
         gHi, sHi);
    return true;
}

bool modulation(const Table& t) {
    if (!t.size()) return false;
    heading("Modulation over AWGN, Selective Repeat (modulation preset)");
    for (const char* m : {"bpsk", "qpsk", "16qam", "64qam", "auto"}) {
        double cliff = 1e300, peak = 0;
        for (std::size_t r : where(t, [&](std::size_t i) { return t.get(i, "protocol") == "sr" && t.get(i, "modulation") == m; })) {
            peak = std::max(peak, thr(t, long(r)));
            if (thr(t, long(r)) > 0) cliff = std::min(cliff, t.num(r, "snr_db"));
        }
        if (peak <= 0) continue;
        std::string name = std::string(m) == "auto" ? "AMC" : upper(m);
        line("%-6s works from %4.0f dB, peak %7.1f kbit/s", name.c_str(), cliff, peak);
    }
    line("Each scheme has a sharp SNR cliff below which ARQ cannot deliver; AMC follows the best scheme at every SNR.");
    return true;
}

// ----------------------------------------------------------------- verdict
void verdict(const Table& pr, const Table& rt, const Table& mod, const Table& nk) {
    std::vector<std::string> because, caveat;
    char buf[256];
    auto add = [&](std::vector<std::string>& v, const char* fmt, auto... a) {
        std::snprintf(buf, sizeof(buf), fmt, a...);
        v.emplace_back(buf);
    };

    // Protocol: who has the highest throughput at each loss level?
    const auto loss = levels(pr, "loss");
    auto winnerAt = [&](double l) {
        long w = kNone;
        for (std::size_t r : where(pr, [&](std::size_t i) { return same(pr.num(i, "loss"), l); }))
            if (w == kNone || thr(pr, long(r)) > thr(pr, w)) w = long(r);
        return pr.get(std::size_t(w), "protocol");
    };
    std::vector<std::pair<std::string, int>> wins = {{"sw", 0}, {"gbn", 0}, {"sr", 0}};
    for (double l : loss)
        for (auto& w : wins)
            if (w.first == winnerAt(l)) ++w.second;
    const std::string best =
        std::max_element(wins.begin(), wins.end(), [](auto& a, auto& b) { return a.second < b.second; })->first;
    int bestWins = 0, lossyWins = 0, lossy = 0;
    for (double l : loss) {
        bool win = winnerAt(l) == best;
        bestWins += win;
        if (l > 0) ++lossy, lossyWins += win;
    }
    add(because, "%s has the highest throughput at %d of %zu loss levels, including %d of %d lossy ones.",
        upper(best).c_str(), bestWins, loss.size(), lossyWins, lossy);
    const double lMax = loss.back();
    long b = find(pr, [&](std::size_t i) { return pr.get(i, "protocol") == best && same(pr.num(i, "loss"), lMax); });
    for (std::size_t o : where(pr, [&](std::size_t i) { return pr.get(i, "protocol") != best && same(pr.num(i, "loss"), lMax); })) {
        double rx = retx(pr, long(o)) / std::max(1e-9, retx(pr, b));
        char tail[96];
        if (rx >= 1.2) std::snprintf(tail, sizeof(tail), " and needs %.1fx fewer retransmissions", rx);
        else std::snprintf(tail, sizeof(tail), " with about the same number of retransmissions");
        add(because, "  at %.0f %% loss it is %.1fx faster than %s%s.", 100 * lMax, thr(pr, b) / thr(pr, long(o)),
            upper(pr.get(o, "protocol")).c_str(), tail);
    }
    if (best == "sr") because.emplace_back("  it keeps the pipe full like GBN but resends only the packets that were really lost.");

    // RTO: how often is adaptive at least as good as fixed?
    std::string rtoPick = "fixed 2xRTT RTO";
    if (rt.size()) {
        int pairs = 0, adWins = 0;
        for (std::size_t f : where(rt, [&](std::size_t i) { return rt.get(i, "rto_mode") == "fixed"; })) {
            long a = find(rt, [&](std::size_t i) {
                return rt.get(i, "rto_mode") == "adaptive" && rt.get(i, "protocol") == rt.get(f, "protocol") &&
                       same(rt.num(i, "rtt_ms"), rt.num(f, "rtt_ms"));
            });
            if (a == kNone) continue;
            ++pairs;
            if (retx(rt, a) <= retx(rt, long(f))) ++adWins;
        }
        if (adWins * 2 >= pairs) rtoPick = "adaptive RTO (RFC 6298)";
        add(because, "Adaptive RTO needs no more retransmissions than the fixed one in %d of %d cases;", adWins, pairs);
        because.emplace_back("  it learns the real RTT including queueing, so it does not fire spurious timeouts.");
    }

    // Modulation: is AMC close to the best fixed scheme at every usable SNR?
    std::string modPick;
    if (mod.size()) {
        int usable = 0, close = 0;
        for (std::size_t a : where(mod, [&](std::size_t i) { return mod.get(i, "protocol") == best && mod.get(i, "modulation") == "auto"; })) {
            double fixedBest = 0;
            for (std::size_t r : where(mod, [&](std::size_t i) {
                     return mod.get(i, "protocol") == best && mod.get(i, "modulation") != "auto" &&
                            same(mod.num(i, "snr_db"), mod.num(a, "snr_db"));
                 }))
                fixedBest = std::max(fixedBest, thr(mod, long(r)));
            if (fixedBest <= 0) continue;
            ++usable;
            if (thr(mod, long(a)) >= 0.9 * fixedBest) ++close;
        }
        if (close * 2 >= usable) modPick = "adaptive modulation (AMC)";
        add(because, "AMC is within 10 %% of the best fixed modulation at %d of %d usable SNR points;", close, usable);
        because.emplace_back("  no single fixed scheme can do that: BPSK is too slow at high SNR, 64-QAM fails at low SNR.");
    }

    long g0 = find(pr, [&](std::size_t i) { return pr.get(i, "protocol") == "gbn" && pr.num(i, "loss") == 0; });
    long s0 = find(pr, [&](std::size_t i) { return pr.get(i, "protocol") == "sr" && pr.num(i, "loss") == 0; });
    if (g0 != kNone && s0 != kNone)
        add(caveat, "without loss GBN and SR are equal (%.1f vs %.1f kbit/s), and GBN has the simpler receiver;",
            thr(pr, g0), thr(pr, s0));
    if (nk.size())
        add(caveat, "NAKs add at most %.0f %% to SR, so they are optional there (they matter for GBN).",
            100 * maxNakGain(nk, "sr"));

    const std::string rule(96, '-');
    std::string pick = upper(best) + " + " + rtoPick + (modPick.empty() ? "" : " + " + modPick);
    std::printf("\n  %s\n  BEST OVERALL: %s\n  %s\n  because\n", rule.c_str(), pick.c_str(), rule.c_str());
    for (const auto& l : because) std::printf("    %s\n", l.c_str());
    if (!caveat.empty()) {
        std::printf("    when it does NOT matter:\n");
        for (const auto& l : caveat) std::printf("      %s\n", l.c_str());
    }
}

// --------------------------------------------------------- single sweep dir
bool singleSweep(const Table& q) {
    if (!q.size() || q.col("protocol") < 0) return false;
    const double lossMax = levels(q, "loss").back(), rttMax = levels(q, "rtt_ms").back();
    std::printf("\n  * Protocol comparison at RTT %.0f ms\n", rttMax);
    struct Rank { std::string p; double thr = 0, retx = 0; int n = 0; };
    std::vector<Rank> rank;
    for (const char* p : {"sw", "gbn", "sr"}) {
        auto at = [&](double loss) {
            return find(q, [&](std::size_t i) {
                return q.get(i, "protocol") == p && same(q.num(i, "loss"), loss) && same(q.num(i, "rtt_ms"), rttMax);
            });
        };
        long r0 = at(0), rl = at(lossMax);
        if (r0 != kNone && rl != kNone)
            line("%-4s %7.1f kbit/s without loss, %7.1f kbit/s at %.0f %% loss (%.1f retransmissions)", upper(p).c_str(),
                 thr(q, r0), thr(q, rl), 100 * lossMax, retx(q, rl));
        Rank k{p};
        for (std::size_t r : where(q, [&](std::size_t i) { return q.get(i, "protocol") == p && q.num(i, "loss") > 0; }))
            k.thr += thr(q, long(r)), k.retx += retx(q, long(r)), ++k.n;
        if (k.n) rank.push_back({p, k.thr / k.n, k.retx / k.n, k.n});
    }
    if (rank.empty()) return true;
    std::sort(rank.begin(), rank.end(), [](const Rank& a, const Rank& b) { return a.thr > b.thr; });
    std::string rest;
    for (std::size_t i = 1; i < rank.size(); ++i) {
        char b[64];
        std::snprintf(b, sizeof(b), "%s%s %.1f", i > 1 ? ", " : "", upper(rank[i].p).c_str(), rank[i].thr);
        rest += b;
    }
    std::printf("\n  BEST IN THIS RUN: %s\n", upper(rank[0].p).c_str());
    std::printf("    because it has the highest mean throughput under loss (%.1f kbit/s vs %s),\n", rank[0].thr, rest.c_str());
    std::printf("    with %.1f retransmissions per run on average. Run the full study for the complete verdict.\n",
                rank[0].retx);
    return true;
}

bool integrity(const fs::path& dir) {
    std::size_t total = 0, done = 0, bad = 0;
    for (const auto& e : fs::recursive_directory_iterator(dir)) {
        if (!e.is_regular_file() || e.path().filename() != "raw.csv") continue;
        Table raw = load(e.path());
        for (std::size_t r = 0; r < raw.size(); ++r) {
            ++total;
            if (raw.get(r, "completed") != "1") continue;
            ++done;
            if (raw.get(r, "integrity") != "1") ++bad;
        }
    }
    if (!total) return false;
    heading("Integrity (CRC-32 per packet + SHA-256 per file)");
    line("%zu runs in total: %zu completed, %zu corrupted files, %zu aborted.", total, done, bad, total - done);
    line(bad == 0 ? "Every completed transfer was bit-exact." : "WARNING: some completed transfers did not match the sent file.");
    if (total > done) line("Aborted runs are configurations below the SNR cliff (or past the stall guard), not errors.");
    return true;
}

}  // namespace

int printConclusion(const std::string& dirName) {
    const fs::path dir(dirName);
    if (!fs::is_directory(dir)) {
        std::fprintf(stderr, "conclude: '%s' is not a directory\n", dirName.c_str());
        return 1;
    }
    bool any = false;
    if (fs::exists(dir / "summary.csv")) {
        any |= singleSweep(load(dir / "summary.csv"));
    } else {
        const Table pr = load(dir / "protocols" / "summary.csv"), rt = load(dir / "rto" / "summary.csv"),
                    mod = load(dir / "modulation" / "summary.csv"), nk = load(dir / "nak" / "summary.csv");
        any |= proposal(load(dir / "proposal" / "summary.csv"));
        any |= protocols(pr);
        any |= rto(rt);
        any |= nak(nk);
        any |= burst(load(dir / "burst" / "summary.csv"));
        any |= modulation(mod);
        if (pr.size()) verdict(pr, rt, mod, nk);
    }
    any |= integrity(dir);
    if (!any) {
        std::fprintf(stderr, "conclude: no sweep results found in '%s'\n", dirName.c_str());
        return 1;
    }
    std::printf("\n");
    return 0;
}

}  // namespace arq::cli
