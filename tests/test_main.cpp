// Unit and end-to-end tests (no external test framework required).
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#include "arq/rto.hpp"
#include "channel/loss_model.hpp"
#include "core/checksum.hpp"
#include "core/clock.hpp"
#include "core/packet.hpp"
#include "experiment/runner.hpp"
#include "experiment/stats.hpp"
#include "experiment/table.hpp"
#include "net/udp_socket.hpp"
#include "phy/modulation.hpp"

using namespace arq;

namespace {
int g_failed = 0, g_checks = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        ++g_checks;                                                              \
        if (!(cond)) {                                                           \
            ++g_failed;                                                          \
            std::printf("    FAILED %s:%d  %s\n", __FILE__, __LINE__, #cond);    \
        }                                                                        \
    } while (0)

#define CHECK_NEAR(a, b, tol) CHECK(std::fabs((a) - (b)) <= (tol))

void test(const char* name, const std::function<void()>& fn) {
    int before = g_failed;
    fn();
    std::printf("  [%s] %s\n", g_failed == before ? " ok " : "FAIL", name);
}

std::vector<std::uint8_t> bytes(const std::string& s) { return {s.begin(), s.end()}; }
}  // namespace

int main(int argc, char** argv) {
    const bool quick = argc > 1 && std::string(argv[1]) == "--quick";
    std::printf("arqsim tests\n");

    test("CRC-32 check value (\"123456789\" -> CBF43926)", [] {
        auto d = bytes("123456789");
        CHECK(crc32(d.data(), d.size()) == 0xCBF43926U);
    });

    test("SHA-256 FIPS 180-4 vectors", [] {
        CHECK(Sha256::hex(bytes("abc")) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
        CHECK(Sha256::hex({}) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
        CHECK(Sha256::hex(bytes("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")) ==
              "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    });

    test("packet encode/decode round trip", [] {
        Packet p;
        p.type = PacketType::Data;
        p.session = 4242;
        p.seq = 123456;
        p.payload = bytes("hello ARQ");
        auto wire = encode(p);
        CHECK(wire.size() == kHeaderSize + 9);
        auto q = decode(wire.data(), wire.size());
        CHECK(q.has_value());
        CHECK(q && q->seq == 123456 && q->session == 4242 && q->payload == p.payload);
    });

    test("CRC detects every single-bit error in a packet", [] {
        Packet p;
        p.seq = 7;
        p.payload.assign(200, 0x5A);
        auto wire = encode(p);
        int undetected = 0;
        for (std::size_t bit = 0; bit < wire.size() * 8; ++bit) {
            auto w = wire;
            w[bit / 8] ^= std::uint8_t(1U << (bit % 8));
            if (decode(w.data(), w.size())) ++undetected;
        }
        CHECK(undetected == 0);
    });

    test("noise-free modulation is lossless for all schemes", [] {
        std::vector<std::uint8_t> data(1013);
        for (std::size_t i = 0; i < data.size(); ++i) data[i] = std::uint8_t(i * 37 + 11);
        for (Modulation m : allModulations()) {
            Modem modem(m, 200.0, 1);  // practically infinite SNR
            auto copy = data;
            CHECK(modem.transmit(copy) == 0);
            CHECK(copy == data);
        }
    });

    test("constellations are normalised to unit average energy", [] {
        std::vector<std::uint8_t> data(3000);
        for (std::size_t i = 0; i < data.size(); ++i) data[i] = std::uint8_t((i * 2654435761U) >> 13);
        for (Modulation m : allModulations()) {
            Modem modem(m, 200.0, 1);
            auto sym = modem.modulate(data);
            double e = 0;
            for (auto& s : sym) e += s.i * s.i + s.q * s.q;
            CHECK_NEAR(e / double(sym.size()), 1.0, 0.05);
        }
    });

    test("Monte-Carlo BER matches theory (within 15 %)", [] {
        struct Case { Modulation m; double snr; };
        for (Case c : {Case{Modulation::BPSK, 6}, Case{Modulation::QPSK, 8}, Case{Modulation::QAM16, 14},
                       Case{Modulation::QAM64, 20}}) {
            BerPoint p = measureBer(c.m, c.snr, 2000000, 99);
            CHECK(p.errors > 200);
            CHECK(std::fabs(p.simulated - p.theory) / p.theory < 0.15);
        }
    });

    test("higher-order modulation needs more SNR (BER ordering)", [] {
        for (double snr : {8.0, 14.0, 20.0}) {
            CHECK(theoreticalBer(Modulation::BPSK, snr) < theoreticalBer(Modulation::QPSK, snr));
            CHECK(theoreticalBer(Modulation::QPSK, snr) < theoreticalBer(Modulation::QAM16, snr));
            CHECK(theoreticalBer(Modulation::QAM16, snr) < theoreticalBer(Modulation::QAM64, snr));
        }
    });

    test("adaptive modulation climbs with SNR", [] {
        CHECK(selectAdaptive(5, 1014) == Modulation::BPSK);
        CHECK(selectAdaptive(15, 1014) == Modulation::QPSK);
        CHECK(selectAdaptive(24, 1014) == Modulation::QAM16);
        CHECK(selectAdaptive(32, 1014) == Modulation::QAM64);
    });

    test("loss models hit the configured average loss rate", [] {
        for (LossKind k : {LossKind::Bernoulli, LossKind::GilbertElliott}) {
            LossModel lm(k, 0.1, 4.0, 5);
            int drops = 0;
            const int n = 400000;
            for (int i = 0; i < n; ++i) drops += lm.drop() ? 1 : 0;
            CHECK_NEAR(double(drops) / n, 0.1, 0.01);
        }
    });

    test("Gilbert-Elliott produces longer loss bursts than Bernoulli", [] {
        auto meanBurst = [](LossKind k) {
            LossModel lm(k, 0.1, 4.0, 9);
            int bursts = 0, lost = 0;
            bool prev = false;
            for (int i = 0; i < 200000; ++i) {
                bool d = lm.drop();
                if (d) ++lost;
                if (d && !prev) ++bursts;
                prev = d;
            }
            return double(lost) / bursts;
        };
        CHECK(meanBurst(LossKind::GilbertElliott) > 3.0);
        CHECK(meanBurst(LossKind::Bernoulli) < 1.3);
    });

    test("RFC 6298 RTO estimator", [] {
        RtoEstimator fixed(RtoMode::Fixed, 50, 2.0);
        CHECK_NEAR(fixed.rtoMs(), 100.0, 1e-9);
        fixed.onTimeout();
        CHECK_NEAR(fixed.rtoMs(), 100.0, 1e-9);  // fixed never backs off

        RtoEstimator a(RtoMode::Adaptive, 50, 2.0);
        CHECK_NEAR(a.rtoMs(), 150.0, 1e-9);      // seeded: SRTT=RTT, RTTVAR=RTT/2
        a.onSample(40);                           // SRTT=40, RTTVAR=20 -> 120
        CHECK_NEAR(a.rtoMs(), 120.0, 1e-9);
        for (int i = 0; i < 50; ++i) a.onSample(40);
        CHECK(a.rtoMs() < 50.0);                  // converges towards SRTT + G
        double before = a.rtoMs();
        a.onTimeout();
        CHECK_NEAR(a.rtoMs(), 2 * before, 1e-9);  // exponential back-off
        a.onProgress();
        CHECK_NEAR(a.rtoMs(), before, 1e-9);      // cleared when new data is ACKed
    });

    test("95% confidence interval (Student t)", [] {
        Summary s = summarize({10, 12, 14, 16, 18});
        CHECK_NEAR(s.mean, 14.0, 1e-12);
        CHECK_NEAR(s.stddev, std::sqrt(10.0), 1e-12);
        CHECK_NEAR(s.ci95, 2.776 * std::sqrt(10.0) / std::sqrt(5.0), 1e-9);
    });

    test("table aggregation groups by configuration", [] {
        Table t({"protocol", "loss", "throughput_kbps"});
        t.addRow({"gbn", "0.1", "100"});
        t.addRow({"gbn", "0.1", "200"});
        t.addRow({"sw", "0.1", "50"});
        Table s = t.aggregate({"protocol", "loss"}, {"throughput_kbps"});
        CHECK(s.size() == 2);
        CHECK_NEAR(s.num(0, "throughput_kbps_mean"), 150.0, 1e-9);
        CHECK(s.get(0, "runs") == "2");
        CHECK(s.filter({{"loss", "0.100"}}).size() == 2);
    });

    test("theoretical utilisation formulas (1) and (2)", [] {
        // L = 1014 B, R = 2 Mbit/s -> L/R = 4.056 ms; RTT = 50 ms
        CHECK_NEAR(theoreticalUtilization(1, 1014, 2e6, 50), 4.056 / 54.056, 1e-6);
        CHECK_NEAR(theoreticalUtilization(8, 1014, 2e6, 50), 8 * 4.056 / 54.056, 1e-6);
        CHECK_NEAR(theoreticalUtilization(16, 1014, 2e6, 50), 1.0, 1e-12);
    });

    // ---------------- end-to-end transfers over real UDP sockets ----------------
    NetworkInit net;
    HighResTimerScope timer;
    struct E2E { ProtocolKind p; double loss; LossKind lk; const char* mod; double snr; RtoMode rto; bool nak; };
    std::vector<E2E> cases = {
        {ProtocolKind::StopAndWait, 0.0, LossKind::Bernoulli, "none", 0, RtoMode::Fixed, false},
        {ProtocolKind::GoBackN, 0.0, LossKind::Bernoulli, "none", 0, RtoMode::Fixed, false},
        {ProtocolKind::SelectiveRepeat, 0.0, LossKind::Bernoulli, "none", 0, RtoMode::Fixed, false},
        {ProtocolKind::StopAndWait, 0.15, LossKind::Bernoulli, "none", 0, RtoMode::Fixed, false},
        {ProtocolKind::GoBackN, 0.15, LossKind::GilbertElliott, "none", 0, RtoMode::Adaptive, true},
        {ProtocolKind::SelectiveRepeat, 0.15, LossKind::Bernoulli, "none", 0, RtoMode::Adaptive, true},
        {ProtocolKind::GoBackN, 0.0, LossKind::Bernoulli, "16qam", 19, RtoMode::Adaptive, false},
        {ProtocolKind::SelectiveRepeat, 0.0, LossKind::Bernoulli, "auto", 13, RtoMode::Adaptive, true},
    };
    if (quick) cases.resize(3);
    for (const auto& c : cases) {
        char name[160];
        std::snprintf(name, sizeof(name), "e2e %s loss=%.2f %s mod=%s%s, %s RTO%s: file arrives intact",
                      toString(c.p), c.loss, toString(c.lk), c.mod, c.snr > 0 ? ("@" + std::to_string(int(c.snr)) + "dB").c_str() : "",
                      toString(c.rto), c.nak ? ", NAK" : "");
        test(name, [&] {
            RunSpec s;
            s.arq.protocol = c.p;
            s.arq.window = 8;
            s.arq.rtoMode = c.rto;
            s.arq.useNak = c.nak;
            s.arq.maxTimeMs = 60000;
            // RTT chosen so that 2xRTT exceeds N*L/R + RTT: with smaller RTTs a
            // fixed RTO causes (correct but spurious) SR timeouts from queueing.
            s.channel.rttMs = 40;
            s.channel.dataLoss = c.loss;
            s.channel.ackLoss = c.loss;
            s.channel.lossKind = c.lk;
            bool isAuto = false;
            parseModulation(c.mod, s.channel.modulation, isAuto);
            s.channel.adaptiveModulation = isAuto;
            s.channel.snrDb = c.snr;
            s.fileBytes = 30000 + 123;  // non-multiple of payload: last packet is short
            s.seed = 77;
            RunResult r = runExperiment(s);
            CHECK(r.completed);
            CHECK(r.integrityPass);
            CHECK(r.bytesReceived == s.fileBytes);
            // Lossless, ideal PHY: no retransmissions expected (one tolerated for a
            // rare OS scheduling stall on a loaded test machine).
            if (c.loss == 0.0 && std::string(c.mod) == "none") CHECK(r.retransmissions <= 1);
            if (c.loss > 0.0) CHECK(r.retransmissions > 0);
        });
    }

    std::printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
