#pragma once
// Channel emulator process: sits between sender and receiver and imposes
//   - packet loss (Bernoulli or Gilbert-Elliott bursts), separately for DATA/ACK
//   - finite link rate R (serialisation delay L/R, FIFO queueing per direction)
//   - one-way propagation delay RTT/2 (+ optional uniform jitter)
//   - bit errors from the modulation/AWGN physical-layer model
//
//      sender:50000  --DATA-->  channel:50010  --DATA-->  receiver:50020
//      sender:50000  <--ACK---  channel:50011  <--ACK---  receiver:50020
#include <atomic>
#include <cstdint>

#include "channel/loss_model.hpp"
#include "net/udp_socket.hpp"
#include "phy/modulation.hpp"

namespace arq {

struct ChannelConfig {
    Endpoint dataIn{"127.0.0.1", 50010};
    Endpoint ackIn{"127.0.0.1", 50011};
    Endpoint receiver{"127.0.0.1", 50020};
    Endpoint sender{"127.0.0.1", 50000};

    double rttMs = 50.0;
    double jitterMs = 0.0;

    double dataLoss = 0.0;
    double ackLoss = 0.0;
    LossKind lossKind = LossKind::Bernoulli;
    double meanBurst = 4.0;

    Modulation modulation = Modulation::None;
    bool adaptiveModulation = false;
    double snrDb = 20.0;
    double symbolRate = 1e6;  // baud, used when modulation != None
    double rateBps = 2e6;     // used when modulation == None (ideal PHY)

    std::uint32_t seed = 1;

    /// Modulation actually used (resolves AMC choice for a given frame size).
    Modulation effectiveModulation(std::size_t frameBytes) const;
};

struct ChannelStats {
    std::atomic<std::uint64_t> dataIn{0}, dataDropped{0}, dataCorrupted{0};
    std::atomic<std::uint64_t> ackIn{0}, ackDropped{0}, ackCorrupted{0};
    std::atomic<std::uint64_t> bitErrors{0}, bitsCarried{0};
    // Emulation accuracy: how late packets left the channel compared with their
    // scheduled delivery time (microseconds), and the final spin window.
    std::atomic<std::uint64_t> delivered{0}, lateSumUs{0}, lateMaxUs{0}, spinWindowUs{0};
};

class ChannelEmulator {
public:
    ChannelEmulator(ChannelConfig cfg, std::size_t frameBytesHint);

    /// Runs until *stop becomes true. Sockets are bound inside run().
    void run(const std::atomic<bool>& stop);

    const ChannelStats& stats() const { return stats_; }
    Modulation modulation() const { return mod_; }
    double linkRateBps() const { return rate_; }

    /// Signalled after sockets are bound (lets the runner start the sender safely).
    std::atomic<bool> ready{false};

private:
    ChannelConfig cfg_;
    Modulation mod_;
    double rate_;
    ChannelStats stats_;
};

}  // namespace arq
