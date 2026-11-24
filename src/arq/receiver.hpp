#pragma once
// Receiver side: sequence validation, buffering (SR), ACK/NAK generation and
// file reassembly. Corrupted packets (CRC failure) are silently discarded,
// or answered with a NAK when NAK mode is enabled.
#include <atomic>
#include <cstdint>
#include <vector>

#include "arq/protocol.hpp"

namespace arq {

struct ReceiverStats {
    bool finished = false;
    std::uint64_t dataReceived = 0;   // DATA packets that passed the CRC
    std::uint64_t corrupted = 0;      // datagrams failing the CRC
    std::uint64_t duplicates = 0;     // already delivered / buffered
    std::uint64_t outOfOrder = 0;     // discarded (GBN/SW) or buffered (SR)
    std::uint64_t acksSent = 0;
    std::uint64_t naksSent = 0;
    std::uint16_t session = 0;
};

class ArqReceiver {
public:
    explicit ArqReceiver(const ArqConfig& cfg);

    /// Receives until FIN (+ linger) or until *stop is set / idle timeout.
    /// The reassembled file is returned via data().
    ReceiverStats run(const std::atomic<bool>& stop, double idleTimeoutMs = 60000.0);

    const std::vector<std::uint8_t>& data() const { return data_; }

    std::atomic<bool> ready{false};

private:
    ArqConfig cfg_;
    std::vector<std::uint8_t> data_;
};

}  // namespace arq
