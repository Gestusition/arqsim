#include "experiment/runner.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <random>
#include <thread>

#include "arq/receiver.hpp"
#include "arq/sender.hpp"
#include "core/checksum.hpp"
#include "core/packet.hpp"

namespace arq {

namespace {
std::string fmt(double v, int prec = 4) {
    char b[64];
    std::snprintf(b, sizeof(b), "%.*f", prec, v);
    return b;
}
void waitFlag(const std::atomic<bool>& f) {
    for (int i = 0; i < 2000 && !f.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
}
}  // namespace

std::vector<std::uint8_t> makeTestFile(std::size_t bytes, std::uint32_t seed) {
    std::vector<std::uint8_t> f(bytes);
    std::mt19937 rng(seed ^ 0xA5A5F00DU);
    for (auto& b : f) b = std::uint8_t(rng() & 0xFFU);
    return f;
}

double theoreticalUtilization(int window, std::size_t frameBytes, double rateBps, double rttMs) {
    const double tx = double(frameBytes) * 8.0 / rateBps * 1000.0;  // L/R in ms
    return std::min(1.0, double(window) * tx / (rttMs + tx));
}

RunResult runExperiment(const RunSpec& spec, const std::vector<std::uint8_t>* fileOverride) {
    RunResult r;
    r.spec = spec;

    std::vector<std::uint8_t> generated;
    if (!fileOverride) generated = makeTestFile(spec.fileBytes, spec.seed);
    const std::vector<std::uint8_t>& file = fileOverride ? *fileOverride : generated;
    r.spec.fileBytes = file.size();

    ArqConfig arqCfg = spec.arq;
    if (arqCfg.session == 0) arqCfg.session = std::uint16_t(1 + (spec.seed * 7919U + spec.repetition * 131U) % 65534U);
    arqCfg.rttMs = spec.channel.rttMs;

    ChannelConfig chCfg = spec.channel;
    chCfg.seed = spec.seed;
    const std::size_t frameBytes = arqCfg.payloadBytes + kHeaderSize;

    ChannelEmulator channel(chCfg, frameBytes);
    ArqReceiver receiver(arqCfg);
    std::atomic<bool> stopChannel{false}, stopReceiver{false};
    ReceiverStats rx;

    std::thread chThread([&] { channel.run(stopChannel); });
    waitFlag(channel.ready);
    std::thread rxThread([&] { rx = receiver.run(stopReceiver, arqCfg.maxTimeMs); });
    waitFlag(receiver.ready);

    auto sender = ArqSender::create(arqCfg);
    SenderStats tx = sender->run(file);

    stopReceiver = true;
    rxThread.join();
    stopChannel = true;
    chThread.join();

    r.modulationUsed = channel.modulation();
    r.linkRateBps = channel.linkRateBps();
    r.completed = tx.completed;
    r.sha256Sent = Sha256::hex(file);
    r.sha256Received = Sha256::hex(receiver.data());
    r.bytesReceived = receiver.data().size();
    r.integrityPass = tx.completed && r.bytesReceived == file.size() && r.sha256Sent == r.sha256Received;

    r.transferMs = tx.transferMs;
    // An aborted transfer delivered nothing usable: its goodput is zero.
    r.throughputKbps = (tx.completed && tx.transferMs > 0) ? double(file.size()) * 8.0 / tx.transferMs : 0.0;  // kbit/s
    r.efficiency = tx.payloadBytesSent ? double(file.size()) / double(tx.payloadBytesSent) : 0.0;
    const double airtimeMs = double(tx.packets) * double(frameBytes) * 8.0 / r.linkRateBps * 1000.0;
    r.utilization = tx.transferMs > 0 ? std::min(1.0, airtimeMs / tx.transferMs) : 0.0;
    r.theoryUtilization = theoreticalUtilization(arqCfg.effectiveWindow(), frameBytes, r.linkRateBps, chCfg.rttMs);

    r.packets = tx.packets;
    r.dataTx = tx.dataTransmissions;
    r.retransmissions = tx.retransmissions;
    r.timeouts = tx.timeouts;
    r.acks = tx.acksReceived;
    r.naks = tx.naksReceived;
    r.corruptedFeedback = tx.corruptedFeedback;
    const ChannelStats& cs = channel.stats();
    r.chDataDropped = cs.dataDropped;
    r.chAckDropped = cs.ackDropped;
    r.chDataCorrupted = cs.dataCorrupted;
    r.chAckCorrupted = cs.ackCorrupted;
    r.measuredBer = cs.bitsCarried ? double(cs.bitErrors) / double(cs.bitsCarried) : 0.0;
    r.rxCorrupted = rx.corrupted;
    r.rxDuplicates = rx.duplicates;
    r.rxOutOfOrder = rx.outOfOrder;
    r.meanRttMs = tx.meanRttMs;
    r.finalRtoMs = tx.finalRtoMs;
    r.lateMeanMs = cs.delivered ? double(cs.lateSumUs) / double(cs.delivered) / 1000.0 : 0.0;
    r.lateMaxMs = double(cs.lateMaxUs) / 1000.0;
    r.spinWindowMs = double(cs.spinWindowUs) / 1000.0;
    return r;
}

std::vector<std::string> resultColumns() {
    return {"protocol",       "window",        "payload",        "file_bytes",      "rtt_ms",
            "jitter_ms",      "loss",          "ack_loss",       "loss_model",      "burst",
            "rto_mode",       "nak",           "modulation",     "mod_used",        "snr_db",
            "link_kbps",      "rep",           "seed",           "completed",       "integrity",
            "transfer_ms",    "throughput_kbps", "efficiency",   "utilization",     "theory_util",
            "packets",        "data_tx",       "retransmissions", "timeouts",       "acks",
            "naks",           "ch_data_drop",  "ch_ack_drop",    "ch_data_corrupt", "ch_ack_corrupt",
            "rx_corrupted",   "rx_duplicates", "rx_out_of_order", "ber",            "mean_rtt_ms",
            "final_rto_ms",   "late_mean_ms",  "late_max_ms",    "spin_window_ms",  "sha256"};
}

std::vector<std::string> resultRow(const RunResult& r) {
    const auto& a = r.spec.arq;
    const auto& c = r.spec.channel;
    const bool phy = c.modulation != Modulation::None || c.adaptiveModulation;
    return {toString(a.protocol),
            std::to_string(a.effectiveWindow()),
            std::to_string(a.payloadBytes),
            std::to_string(r.spec.fileBytes),
            fmt(c.rttMs, 1),
            fmt(c.jitterMs, 1),
            fmt(c.dataLoss, 3),
            fmt(c.ackLoss, 3),
            toString(c.lossKind),
            fmt(c.lossKind == LossKind::GilbertElliott ? c.meanBurst : 1.0, 1),
            toString(a.rtoMode),
            a.useNak ? "1" : "0",
            c.adaptiveModulation ? "auto" : toString(c.modulation),
            toString(r.modulationUsed),
            phy ? fmt(c.snrDb, 1) : "",
            fmt(r.linkRateBps / 1000.0, 1),
            std::to_string(r.spec.repetition),
            std::to_string(r.spec.seed),
            r.completed ? "1" : "0",
            r.integrityPass ? "1" : "0",
            fmt(r.transferMs, 3),
            fmt(r.throughputKbps, 3),
            fmt(r.efficiency, 4),
            fmt(r.utilization, 4),
            fmt(r.theoryUtilization, 4),
            std::to_string(r.packets),
            std::to_string(r.dataTx),
            std::to_string(r.retransmissions),
            std::to_string(r.timeouts),
            std::to_string(r.acks),
            std::to_string(r.naks),
            std::to_string(r.chDataDropped),
            std::to_string(r.chAckDropped),
            std::to_string(r.chDataCorrupted),
            std::to_string(r.chAckCorrupted),
            std::to_string(r.rxCorrupted),
            std::to_string(r.rxDuplicates),
            std::to_string(r.rxOutOfOrder),
            fmt(r.measuredBer, 8),
            fmt(r.meanRttMs, 3),
            fmt(r.finalRtoMs, 2),
            fmt(r.lateMeanMs, 3),
            fmt(r.lateMaxMs, 3),
            fmt(r.spinWindowMs, 1),
            r.sha256Received};
}

}  // namespace arq
