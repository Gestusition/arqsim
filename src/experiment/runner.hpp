#pragma once
// Runs one complete experiment: channel emulator, receiver and sender are
// started as three concurrent components that talk to each other only via
// real UDP datagrams on 127.0.0.1 (the same topology as the standalone
// `arqsim sender|channel|receiver` processes).
#include <cstdint>
#include <string>
#include <vector>

#include "arq/protocol.hpp"
#include "channel/emulator.hpp"

namespace arq {

struct RunSpec {
    ArqConfig arq;
    ChannelConfig channel;
    std::size_t fileBytes = 50 * 1000;
    int repetition = 1;
    std::uint32_t seed = 1;
};

struct RunResult {
    RunSpec spec;
    Modulation modulationUsed = Modulation::None;
    double linkRateBps = 0.0;

    bool completed = false;
    bool integrityPass = false;
    std::string sha256Sent;
    std::string sha256Received;
    std::uint64_t bytesReceived = 0;

    double transferMs = 0.0;
    double throughputKbps = 0.0;   // useful payload bits / transfer time
    double efficiency = 0.0;       // useful payload / total payload sent (incl. retx)
    double utilization = 0.0;      // measured: useful frame airtime / transfer time
    double theoryUtilization = 0.0;// loss-free formula (1) or (2) of the proposal

    std::uint64_t packets = 0, dataTx = 0, retransmissions = 0, timeouts = 0;
    std::uint64_t acks = 0, naks = 0, corruptedFeedback = 0;
    std::uint64_t chDataDropped = 0, chAckDropped = 0, chDataCorrupted = 0, chAckCorrupted = 0;
    std::uint64_t rxCorrupted = 0, rxDuplicates = 0, rxOutOfOrder = 0;
    double measuredBer = 0.0;
    double meanRttMs = 0.0;
    double finalRtoMs = 0.0;
    // Emulation accuracy: lateness of channel deliveries vs their schedule (ms)
    // and the spin window the emulator settled on.
    double lateMeanMs = 0.0, lateMaxMs = 0.0, spinWindowMs = 0.0;
};

/// Deterministic pseudo-random test file (so every protocol sends the same bytes).
std::vector<std::uint8_t> makeTestFile(std::size_t bytes, std::uint32_t seed);

/// Loss-free utilisation from the proposal: U = min(1, N (L/R) / (RTT + L/R)).
double theoreticalUtilization(int window, std::size_t frameBytes, double rateBps, double rttMs);

RunResult runExperiment(const RunSpec& spec, const std::vector<std::uint8_t>* fileOverride = nullptr);

/// CSV column names / row for raw per-run results.
std::vector<std::string> resultColumns();
std::vector<std::string> resultRow(const RunResult& r);

}  // namespace arq
