#pragma once
#include <cstdint>
#include <string>

#include "net/udp_socket.hpp"

namespace arq {

enum class ProtocolKind { StopAndWait, GoBackN, SelectiveRepeat };
enum class RtoMode { Fixed, Adaptive };

const char* toString(ProtocolKind p);
const char* toString(RtoMode m);
bool parseProtocol(const std::string& s, ProtocolKind& out);
bool parseRtoMode(const std::string& s, RtoMode& out);

struct ArqConfig {
    ProtocolKind protocol = ProtocolKind::GoBackN;
    int window = 8;                  // forced to 1 for Stop-and-Wait
    std::size_t payloadBytes = 1000; // useful bytes per DATA packet
    double rttMs = 50.0;             // nominal RTT (used for fixed RTO = k * RTT)
    double rtoFactor = 2.0;          // proposal: RTO = 2 x RTT
    RtoMode rtoMode = RtoMode::Fixed;
    bool useNak = false;             // receiver sends NAKs, sender retransmits early
    std::uint16_t session = 0;       // 0 on the receiver = accept first session seen
    double maxTimeMs = 300000.0;     // abort guard

    Endpoint senderLocal{"127.0.0.1", 50000};
    Endpoint channelData{"127.0.0.1", 50010};
    Endpoint receiverLocal{"127.0.0.1", 50020};
    Endpoint channelAck{"127.0.0.1", 50011};

    int effectiveWindow() const { return protocol == ProtocolKind::StopAndWait ? 1 : (window < 1 ? 1 : window); }
};

}  // namespace arq
