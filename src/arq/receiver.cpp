#include "arq/receiver.hpp"

#include <algorithm>
#include <map>
#include <set>

#include "core/clock.hpp"
#include "core/log.hpp"
#include "core/packet.hpp"
#include "net/udp_socket.hpp"

namespace arq {

ArqReceiver::ArqReceiver(const ArqConfig& cfg) : cfg_(cfg) {}

ReceiverStats ArqReceiver::run(const std::atomic<bool>& stop, double idleTimeoutMs) {
    preciseTimersForThisThread();
    UdpSocket sock;
    sock.bind(cfg_.receiverLocal);
    ready = true;

    ReceiverStats st;
    std::uint16_t session = cfg_.session;
    bool sessionLocked = session != 0;
    const bool selective = cfg_.protocol == ProtocolKind::SelectiveRepeat;
    const std::uint32_t window = std::uint32_t(cfg_.effectiveWindow());

    std::uint32_t expected = 0;                              // next in-order seq
    std::map<std::uint32_t, std::vector<std::uint8_t>> buffer;  // SR out-of-order buffer
    std::set<std::uint32_t> nakSent;
    data_.clear();

    auto send = [&](PacketType type, std::uint32_t seq) {
        Packet p;
        p.type = type;
        p.session = session;
        p.seq = seq;
        sock.sendTo(encode(p), cfg_.channelAck);
        if (type == PacketType::Ack) ++st.acksSent;
        if (type == PacketType::Nak) ++st.naksSent;
        ARQ_TRACE("RECV", "send %s %u", toString(type), seq);
    };
    auto nak = [&](std::uint32_t seq) {
        if (cfg_.useNak && nakSent.insert(seq).second) send(PacketType::Nak, seq);
    };

    TimePoint lastActivity = Clock::now();
    TimePoint lingerUntil = TimePoint::max();
    std::vector<std::uint8_t> buf;

    while (!stop.load()) {
        TimePoint now = Clock::now();
        if (now >= lingerUntil) break;
        if (msBetween(lastActivity, now) > idleTimeoutMs) break;
        if (!sock.receive(buf, 20)) continue;
        lastActivity = Clock::now();

        auto pkt = decode(buf.data(), buf.size());
        if (!pkt) {
            ++st.corrupted;
            ARQ_TRACE("RECV", "corrupted datagram discarded");
            // We cannot trust the seq of a corrupted packet; in NAK mode ask
            // for the oldest missing one.
            if (sessionLocked && !st.finished) nak(expected);
            continue;
        }
        if (!sessionLocked) {
            session = pkt->session;
            sessionLocked = true;
        }
        if (pkt->session != session) continue;  // stale packet from an earlier run

        if (pkt->type == PacketType::Fin) {
            send(PacketType::FinAck, pkt->seq);
            if (!st.finished) {
                st.finished = true;
                // Linger so that a lost FINACK can be repeated.
                lingerUntil = afterMs(Clock::now(), std::max(300.0, 4.0 * cfg_.rttMs * cfg_.rtoFactor));
            }
            continue;
        }
        if (pkt->type != PacketType::Data) continue;
        ++st.dataReceived;
        const std::uint32_t seq = pkt->seq;

        if (!selective) {
            // Stop-and-Wait and Go-Back-N: accept only the expected packet,
            // reply with cumulative ACK = next expected sequence number.
            if (seq == expected) {
                data_.insert(data_.end(), pkt->payload.begin(), pkt->payload.end());
                ++expected;
            } else if (seq < expected) {
                ++st.duplicates;
            } else {
                ++st.outOfOrder;
                nak(expected);
            }
            send(PacketType::Ack, expected);
        } else {
            // Selective Repeat: ACK each packet individually, buffer out-of-order.
            if (seq >= expected && seq < expected + window) {
                send(PacketType::Ack, seq);
                if (seq != expected) {
                    if (buffer.count(seq)) ++st.duplicates;
                    else {
                        ++st.outOfOrder;
                        buffer.emplace(seq, std::move(pkt->payload));
                    }
                    for (std::uint32_t m = expected; m < seq; ++m)
                        if (!buffer.count(m)) nak(m);
                } else {
                    data_.insert(data_.end(), pkt->payload.begin(), pkt->payload.end());
                    ++expected;
                    for (auto it = buffer.find(expected); it != buffer.end(); it = buffer.find(expected)) {
                        data_.insert(data_.end(), it->second.begin(), it->second.end());
                        buffer.erase(it);
                        ++expected;
                    }
                }
            } else if (seq < expected && seq + window >= expected) {
                ++st.duplicates;
                send(PacketType::Ack, seq);  // our earlier ACK was lost: re-ACK
            }
        }
    }
    st.session = session;
    return st;
}

}  // namespace arq
