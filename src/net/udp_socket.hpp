#pragma once
// Minimal portable UDP socket (Winsock2 on Windows, BSD sockets elsewhere).
#include <cstdint>
#include <string>
#include <vector>

namespace arq {

struct Endpoint {
    std::string host = "127.0.0.1";
    std::uint16_t port = 0;
};

class UdpSocket {
public:
    UdpSocket();
    ~UdpSocket();
    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;

    /// Binds to host:port. Throws std::runtime_error on failure.
    void bind(const Endpoint& local);

    void sendTo(const std::vector<std::uint8_t>& data, const Endpoint& dst);

    /// Waits up to timeoutMs (0 = poll) for a datagram. Returns false on timeout.
    bool receive(std::vector<std::uint8_t>& out, int timeoutMs);

    /// Waits on several sockets at once; returns index of a readable socket or -1.
    static int waitAny(UdpSocket* const* sockets, int count, int timeoutMs);

    void close();

private:
    std::intptr_t fd_;
};

/// RAII guard for WSAStartup/WSACleanup (no-op on POSIX).
class NetworkInit {
public:
    NetworkInit();
    ~NetworkInit();
};

}  // namespace arq
