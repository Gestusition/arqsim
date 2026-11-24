#pragma once
// Physical-layer model: digital modulation over an AWGN channel.
//
// Every DATA/ACK datagram that crosses the emulated link is actually
// modulated bit-by-bit into constellation symbols, Gaussian noise with the
// configured SNR (Es/N0) is added, and the symbols are hard-decision
// demodulated back into bytes. Bit errors therefore appear naturally and are
// later caught by the CRC-32 check in the ARQ receiver.
//
// Supported schemes (Gray-coded):
//   BPSK   1 bit/symbol
//   QPSK   2 bit/symbol  (= 4-QAM)
//   16-QAM 4 bit/symbol
//   64-QAM 6 bit/symbol
//
// The link bit rate is R = symbolRate * bitsPerSymbol, so higher-order
// modulation shortens serialisation time (L/R) but needs a higher SNR.
#include <cstdint>
#include <random>
#include <string>
#include <vector>

namespace arq {

enum class Modulation { None, BPSK, QPSK, QAM16, QAM64 };

const char* toString(Modulation m);
bool parseModulation(const std::string& s, Modulation& out, bool& isAuto);
int bitsPerSymbol(Modulation m);
std::vector<Modulation> allModulations();

/// Gaussian Q-function Q(x) = 0.5 * erfc(x / sqrt(2)).
double qfunc(double x);

/// Theoretical bit-error probability for Gray-coded modulation in AWGN,
/// with snrDb interpreted as Es/N0 (energy per symbol to noise density).
double theoreticalBer(Modulation m, double snrDb);

/// Probability that a frame of `bits` bits contains at least one bit error.
double frameErrorRate(double ber, std::size_t bits);

/// Adaptive Modulation and Coding (AMC) selection: picks the scheme that
/// maximises expected goodput  bitsPerSymbol * (1 - FER(frameBytes)).
Modulation selectAdaptive(double snrDb, std::size_t frameBytes);

struct Symbol {
    double i;
    double q;
};

class Modem {
public:
    Modem(Modulation m, double snrDb, std::uint32_t seed);

    Modulation modulation() const { return mod_; }
    double snrDb() const { return snrDb_; }

    std::vector<Symbol> modulate(const std::vector<std::uint8_t>& bytes) const;
    std::vector<std::uint8_t> demodulate(const std::vector<Symbol>& symbols, std::size_t byteCount) const;
    void addNoise(std::vector<Symbol>& symbols);

    /// Full pass through modulator -> AWGN -> demodulator. Returns the number
    /// of bit errors introduced into `bytes` (which is modified in place).
    std::size_t transmit(std::vector<std::uint8_t>& bytes);

private:
    Modulation mod_;
    double snrDb_;
    int k_;           // bits per symbol
    int bitsPerAxis_; // for square QAM; BPSK uses I axis only
    int levels_;      // PAM levels per axis
    double norm_;     // scale so that average symbol energy Es = 1
    double sigma_;    // noise std-dev per dimension
    std::mt19937 rng_;
    std::normal_distribution<double> gauss_{0.0, 1.0};
};

struct BerPoint {
    double snrDb;
    double theory;
    double simulated;
    std::uint64_t bits;
    std::uint64_t errors;
};

/// Monte-Carlo BER measurement (used by the `ber` CLI command and tests).
BerPoint measureBer(Modulation m, double snrDb, std::uint64_t bits, std::uint32_t seed);

}  // namespace arq
