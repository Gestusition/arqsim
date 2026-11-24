#include "channel/emulator.hpp"

#include <algorithm>
#include <cmath>
#include <queue>
#include <random>
#include <thread>
#include <vector>

#include "core/clock.hpp"
#include "core/log.hpp"
#include "core/packet.hpp"

namespace arq {

Modulation ChannelConfig::effectiveModulation(std::size_t frameBytes) const {
    if (adaptiveModulation) return selectAdaptive(snrDb, frameBytes);
    return modulation;
}

namespace {

constexpr double kSpinWindowMs = 16.0;

struct Pending {
    TimePoint deliverAt;
    std::uint64_t order;
    bool toReceiver;
    std::vector<std::uint8_t> bytes;
};
struct Later {
    bool operator()(const Pending& a, const Pending& b) const {
        if (a.deliverAt != b.deliverAt) return a.deliverAt > b.deliverAt;
        return a.order > b.order;
    }
};

/// One direction of the link: loss -> FIFO serialisation at rate R -> PHY -> propagation.
class Direction {
public:
    Direction(const ChannelConfig& cfg, Modulation mod, double rate, double loss, std::uint32_t seed)
        : cfg_(cfg),
          loss_(cfg.lossKind, loss, cfg.meanBurst, seed),
          modem_(mod, cfg.snrDb, seed * 2654435761U + 17U),
          rate_(rate),
          jitterRng_(seed + 99U) {}

    // Returns false if the packet is lost; otherwise sets deliverAt and corrupts bytes.
    bool admit(std::vector<std::uint8_t>& bytes, TimePoint now, TimePoint& deliverAt, std::size_t& bitErrors) {
        if (loss_.drop()) return false;
        TimePoint start = std::max(now, busyUntil_);
        double txMs = double(bytes.size()) * 8.0 * 1000.0 / rate_;
        busyUntil_ = afterMs(start, txMs);
        double prop = cfg_.rttMs / 2.0;
        if (cfg_.jitterMs > 0.0) prop += std::uniform_real_distribution<double>(0.0, cfg_.jitterMs)(jitterRng_);
        deliverAt = afterMs(busyUntil_, prop);
        bitErrors = modem_.transmit(bytes);
        return true;
    }

private:
    const ChannelConfig& cfg_;
    LossModel loss_;
    Modem modem_;
    double rate_;
    TimePoint busyUntil_{};
    std::mt19937 jitterRng_;
};

}  // namespace

ChannelEmulator::ChannelEmulator(ChannelConfig cfg, std::size_t frameBytesHint)
    : cfg_(std::move(cfg)),
      mod_(cfg_.effectiveModulation(frameBytesHint)),
      rate_(mod_ == Modulation::None ? cfg_.rateBps : cfg_.symbolRate * bitsPerSymbol(mod_)) {}

void ChannelEmulator::run(const std::atomic<bool>& stop) {
    UdpSocket dataSock, ackSock;
    dataSock.bind(cfg_.dataIn);
    ackSock.bind(cfg_.ackIn);
    ready = true;

    Direction forward(cfg_, mod_, rate_, cfg_.dataLoss, cfg_.seed * 2U + 1U);
    Direction reverse(cfg_, mod_, rate_, cfg_.ackLoss, cfg_.seed * 2U + 2U);

    std::priority_queue<Pending, std::vector<Pending>, Later> queue;
    std::uint64_t order = 0;
    UdpSocket* socks[2] = {&dataSock, &ackSock};
    std::vector<std::uint8_t> buf;

    while (!stop.load()) {
        // Deliver everything that is due.
        TimePoint now = Clock::now();
        while (!queue.empty() && queue.top().deliverAt <= now) {
            const Pending& p = queue.top();
            if (p.toReceiver) dataSock.sendTo(p.bytes, cfg_.receiver);
            else ackSock.sendTo(p.bytes, cfg_.sender);
            queue.pop();
        }

        // select() timeouts are only accurate to the OS tick (up to ~15.6 ms on
        // Windows), which would add random delay to every packet. Sleep in
        // select() only while the next delivery is far away, then poll and
        // spin for the final stretch so deliveries happen within ~0.1 ms.
        int waitMs = 20;
        if (!queue.empty()) {
            double untilNext = msBetween(Clock::now(), queue.top().deliverAt);
            waitMs = untilNext > kSpinWindowMs ? std::min(waitMs, int(untilNext - kSpinWindowMs)) : 0;
        }
        int idx = UdpSocket::waitAny(socks, 2, waitMs);
        if (idx < 0) {
            if (waitMs == 0) std::this_thread::yield();
            continue;
        }

        bool isData = (idx == 0);
        if (!socks[idx]->receive(buf, 0)) continue;

        PacketType type{};
        std::uint32_t seq = 0;
        peekHeader(buf.data(), buf.size(), type, seq);
        const char* tname = toString(type);

        Direction& dir = isData ? forward : reverse;
        auto& in = isData ? stats_.dataIn : stats_.ackIn;
        auto& dropped = isData ? stats_.dataDropped : stats_.ackDropped;
        auto& corrupted = isData ? stats_.dataCorrupted : stats_.ackCorrupted;
        ++in;

        TimePoint deliverAt;
        std::size_t bitErr = 0;
        std::size_t bits = buf.size() * 8;
        if (!dir.admit(buf, Clock::now(), deliverAt, bitErr)) {
            ++dropped;
            ARQ_TRACE("CHAN", "%s seq=%u DROPPED", tname, seq);
            continue;
        }
        stats_.bitsCarried += bits;
        if (bitErr > 0) {
            ++corrupted;
            stats_.bitErrors += bitErr;
            ARQ_TRACE("CHAN", "%s seq=%u CORRUPTED (%zu bit errors)", tname, seq, bitErr);
        } else {
            ARQ_TRACE("CHAN", "%s seq=%u forwarded", tname, seq);
        }
        queue.push(Pending{deliverAt, order++, isData, std::move(buf)});
        buf = {};
    }
}

}  // namespace arq
