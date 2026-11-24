#include "net/udp_socket.hpp"

#include <stdexcept>

#include "core/packet.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
using socklen_t = int;
#define ARQ_INVALID_SOCKET INVALID_SOCKET
#define ARQ_CLOSE closesocket
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#define ARQ_INVALID_SOCKET (-1)
#define ARQ_CLOSE ::close
#endif

namespace arq {

namespace {
sockaddr_in toSockaddr(const Endpoint& ep) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(ep.port);
    if (inet_pton(AF_INET, ep.host.c_str(), &addr.sin_addr) != 1)
        throw std::runtime_error("invalid IPv4 address: " + ep.host);
    return addr;
}
}  // namespace

NetworkInit::NetworkInit() {
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) throw std::runtime_error("WSAStartup failed");
#endif
}

NetworkInit::~NetworkInit() {
#ifdef _WIN32
    WSACleanup();
#endif
}

UdpSocket::UdpSocket() {
    auto s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == ARQ_INVALID_SOCKET) throw std::runtime_error("socket() failed");
    fd_ = static_cast<std::intptr_t>(s);
    int bufSize = 4 * 1024 * 1024;
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&bufSize), sizeof(bufSize));
    setsockopt(s, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&bufSize), sizeof(bufSize));
#ifdef _WIN32
    // Windows reports ICMP "port unreachable" as WSAECONNRESET on the next
    // recvfrom of a UDP socket. That is meaningless for our experiment (a peer
    // process may simply not be started yet), so disable the behaviour.
    BOOL newBehavior = FALSE;
    DWORD bytesReturned = 0;
    WSAIoctl(s, _WSAIOW(IOC_VENDOR, 12) /*SIO_UDP_CONNRESET*/, &newBehavior, sizeof(newBehavior), nullptr, 0,
             &bytesReturned, nullptr, nullptr);
#endif
}

UdpSocket::~UdpSocket() { close(); }

void UdpSocket::close() {
    if (fd_ != static_cast<std::intptr_t>(ARQ_INVALID_SOCKET)) {
        ARQ_CLOSE(static_cast<decltype(::socket(0, 0, 0))>(fd_));
        fd_ = static_cast<std::intptr_t>(ARQ_INVALID_SOCKET);
    }
}

void UdpSocket::bind(const Endpoint& local) {
    sockaddr_in addr = toSockaddr(local);
    auto s = static_cast<decltype(::socket(0, 0, 0))>(fd_);
    if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
        throw std::runtime_error("bind() failed on " + local.host + ":" + std::to_string(local.port) +
                                 " (port in use?)");
}

void UdpSocket::sendTo(const std::vector<std::uint8_t>& data, const Endpoint& dst) {
    sockaddr_in addr = toSockaddr(dst);
    auto s = static_cast<decltype(::socket(0, 0, 0))>(fd_);
    ::sendto(s, reinterpret_cast<const char*>(data.data()), static_cast<int>(data.size()), 0,
             reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
}

int UdpSocket::waitAny(UdpSocket* const* sockets, int count, int timeoutMs) {
    fd_set set;
    FD_ZERO(&set);
    decltype(::socket(0, 0, 0)) maxFd = 0;
    for (int i = 0; i < count; ++i) {
        auto s = static_cast<decltype(::socket(0, 0, 0))>(sockets[i]->fd_);
        FD_SET(s, &set);
        if (s > maxFd) maxFd = s;
    }
    timeval tv{};
    if (timeoutMs < 0) timeoutMs = 0;
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    int rc = ::select(static_cast<int>(maxFd) + 1, &set, nullptr, nullptr, &tv);
    if (rc <= 0) return -1;
    for (int i = 0; i < count; ++i) {
        auto s = static_cast<decltype(::socket(0, 0, 0))>(sockets[i]->fd_);
        if (FD_ISSET(s, &set)) return i;
    }
    return -1;
}

bool UdpSocket::receive(std::vector<std::uint8_t>& out, int timeoutMs) {
    UdpSocket* self = this;
    if (waitAny(&self, 1, timeoutMs) != 0) return false;
    out.resize(kMaxDatagram);
    sockaddr_in from{};
    socklen_t fromLen = sizeof(from);
    auto s = static_cast<decltype(::socket(0, 0, 0))>(fd_);
    int n = ::recvfrom(s, reinterpret_cast<char*>(out.data()), static_cast<int>(out.size()), 0,
                       reinterpret_cast<sockaddr*>(&from), &fromLen);
    if (n <= 0) {
        out.clear();
        return false;
    }
    out.resize(static_cast<std::size_t>(n));
    return true;
}

}  // namespace arq
