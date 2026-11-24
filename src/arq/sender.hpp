#pragma once
// Sender side of the three ARQ protocols. All share one event loop
// (ArqSender::run); protocol behaviour lives in the virtual hooks, mirroring
// the abstract ArqProtocol / StopAndWait / GoBackN class design of the
// proposal, extended with SelectiveRepeat.
#include <cstdint>
#include <memory>
#include <vector>

#include "arq/protocol.hpp"
#include "arq/rto.hpp"
#include "core/clock.hpp"
#include "core/packet.hpp"
#include "net/udp_socket.hpp"

namespace arq {

struct SenderStats {
    bool completed = false;
    double transferMs = 0.0;         // first DATA sent -> last DATA acknowledged
    std::uint64_t packets = 0;       // distinct DATA packets in the file
    std::uint64_t dataTransmissions = 0;
    std::uint64_t retransmissions = 0;
    std::uint64_t timeouts = 0;
    std::uint64_t acksReceived = 0;
    std::uint64_t naksReceived = 0;
    std::uint64_t corruptedFeedback = 0;  // ACK/NAK failing CRC
    std::uint64_t payloadBytesSent = 0;   // incl. retransmissions
    std::uint64_t wireBytesSent = 0;      // payload + headers
    double meanRttMs = 0.0;
    double finalRtoMs = 0.0;
};

class ArqSender {
public:
    explicit ArqSender(const ArqConfig& cfg);
    virtual ~ArqSender() = default;

    /// Transfers `file` through the channel and returns statistics.
    SenderStats run(const std::vector<std::uint8_t>& file);

    static std::unique_ptr<ArqSender> create(const ArqConfig& cfg);

protected:
    // ---- protocol hooks ----
    virtual void fillWindow(TimePoint now) = 0;
    virtual void onAck(std::uint32_t ackNo, TimePoint now) = 0;
    virtual void onNak(std::uint32_t seq, TimePoint now) = 0;
    virtual TimePoint nextDeadline() const = 0;  // TimePoint::max() if no timer
    virtual void onTimers(TimePoint now) = 0;
    virtual bool allAcked() const = 0;

    // ---- helpers for subclasses ----
    void sendData(std::uint32_t seq, bool retransmission, TimePoint now);
    void sampleRtt(std::uint32_t seq, TimePoint now);  // applies Karn's rule
    std::uint32_t total() const { return total_; }

    ArqConfig cfg_;
    RtoEstimator rto_;
    SenderStats stats_;
    std::vector<TimePoint> sentAt_;
    std::vector<bool> retransmitted_;

private:
    void finHandshake();

    UdpSocket sock_;
    const std::vector<std::uint8_t>* file_ = nullptr;
    std::uint32_t total_ = 0;
};

}  // namespace arq
