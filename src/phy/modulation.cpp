#include "phy/modulation.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace arq {

namespace {

unsigned grayToBinary(unsigned g) {
    unsigned b = g;
    while (g >>= 1) b ^= g;
    return b;
}
unsigned binaryToGray(unsigned b) { return b ^ (b >> 1); }

inline int getBit(const std::vector<std::uint8_t>& bytes, std::size_t bit) {
    return (bytes[bit >> 3] >> (7 - (bit & 7))) & 1;
}
inline void setBit(std::vector<std::uint8_t>& bytes, std::size_t bit, int v) {
    if (v) bytes[bit >> 3] |= std::uint8_t(1U << (7 - (bit & 7)));
}

}  // namespace

const char* toString(Modulation m) {
    switch (m) {
        case Modulation::None: return "none";
        case Modulation::BPSK: return "bpsk";
        case Modulation::QPSK: return "qpsk";
        case Modulation::QAM16: return "16qam";
        case Modulation::QAM64: return "64qam";
    }
    return "?";
}

bool parseModulation(const std::string& raw, Modulation& out, bool& isAuto) {
    std::string s;
    for (char c : raw) s.push_back(char(std::tolower(static_cast<unsigned char>(c))));
    s.erase(std::remove(s.begin(), s.end(), '-'), s.end());
    isAuto = false;
    if (s == "none" || s == "off" || s == "ideal") out = Modulation::None;
    else if (s == "bpsk") out = Modulation::BPSK;
    else if (s == "qpsk" || s == "4qam") out = Modulation::QPSK;
    else if (s == "16qam" || s == "qam16") out = Modulation::QAM16;
    else if (s == "64qam" || s == "qam64") out = Modulation::QAM64;
    else if (s == "auto" || s == "amc" || s == "adaptive") {
        out = Modulation::BPSK;
        isAuto = true;
    } else return false;
    return true;
}

int bitsPerSymbol(Modulation m) {
    switch (m) {
        case Modulation::BPSK: return 1;
        case Modulation::QPSK: return 2;
        case Modulation::QAM16: return 4;
        case Modulation::QAM64: return 6;
        default: return 0;
    }
}

std::vector<Modulation> allModulations() {
    return {Modulation::BPSK, Modulation::QPSK, Modulation::QAM16, Modulation::QAM64};
}

double qfunc(double x) { return 0.5 * std::erfc(x / std::sqrt(2.0)); }

double theoreticalBer(Modulation m, double snrDb) {
    const double es = std::pow(10.0, snrDb / 10.0);  // Es/N0 (linear)
    switch (m) {
        case Modulation::None: return 0.0;
        case Modulation::BPSK: return qfunc(std::sqrt(2.0 * es));
        case Modulation::QPSK: return qfunc(std::sqrt(es));  // Eb/N0 = Es/2N0 -> Q(sqrt(2Eb/N0))
        case Modulation::QAM16:
        case Modulation::QAM64: {
            const double M = (m == Modulation::QAM16) ? 16.0 : 64.0;
            const double k = std::log2(M);
            // Gray-coded square M-QAM, nearest-neighbour approximation
            double p = (4.0 / k) * (1.0 - 1.0 / std::sqrt(M)) * qfunc(std::sqrt(3.0 * es / (M - 1.0)));
            return std::min(p, 0.5);
        }
    }
    return 0.0;
}

double frameErrorRate(double ber, std::size_t bits) {
    if (ber <= 0.0) return 0.0;
    return 1.0 - std::pow(1.0 - ber, double(bits));
}

Modulation selectAdaptive(double snrDb, std::size_t frameBytes) {
    Modulation best = Modulation::BPSK;
    double bestGoodput = -1.0;
    for (Modulation m : allModulations()) {
        double fer = frameErrorRate(theoreticalBer(m, snrDb), frameBytes * 8);
        double goodput = bitsPerSymbol(m) * (1.0 - fer);
        if (goodput > bestGoodput + 1e-12) {
            bestGoodput = goodput;
            best = m;
        }
    }
    return best;
}

Modem::Modem(Modulation m, double snrDb, std::uint32_t seed)
    : mod_(m), snrDb_(snrDb), k_(bitsPerSymbol(m)), rng_(seed) {
    if (m == Modulation::BPSK) {
        bitsPerAxis_ = 1;
        levels_ = 2;
        norm_ = 1.0;  // symbols +-1 on I only -> Es = 1
    } else if (m != Modulation::None) {
        bitsPerAxis_ = k_ / 2;
        levels_ = 1 << bitsPerAxis_;
        const double M = double(1 << k_);
        norm_ = std::sqrt(2.0 * (M - 1.0) / 3.0);  // average energy of square M-QAM grid
    } else {
        bitsPerAxis_ = 0;
        levels_ = 0;
        norm_ = 1.0;
    }
    const double esN0 = std::pow(10.0, snrDb / 10.0);
    sigma_ = std::sqrt(1.0 / (2.0 * esN0));  // N0/2 per real dimension with Es = 1
}

std::vector<Symbol> Modem::modulate(const std::vector<std::uint8_t>& bytes) const {
    std::vector<Symbol> out;
    if (mod_ == Modulation::None) return out;
    const std::size_t totalBits = bytes.size() * 8;
    const std::size_t nSym = (totalBits + k_ - 1) / k_;
    out.reserve(nSym);
    std::size_t bit = 0;
    auto readBits = [&](int n) {
        unsigned v = 0;
        for (int i = 0; i < n; ++i) {
            v <<= 1;
            if (bit < totalBits) v |= unsigned(getBit(bytes, bit));
            ++bit;
        }
        return v;
    };
    auto level = [&](unsigned grayBits) {
        unsigned idx = grayToBinary(grayBits);
        return (2.0 * idx - (levels_ - 1)) / norm_;
    };
    for (std::size_t s = 0; s < nSym; ++s) {
        if (mod_ == Modulation::BPSK) {
            out.push_back({level(readBits(1)), 0.0});
        } else {
            double i = level(readBits(bitsPerAxis_));
            double q = level(readBits(bitsPerAxis_));
            out.push_back({i, q});
        }
    }
    return out;
}

std::vector<std::uint8_t> Modem::demodulate(const std::vector<Symbol>& symbols, std::size_t byteCount) const {
    std::vector<std::uint8_t> out(byteCount, 0);
    const std::size_t totalBits = byteCount * 8;
    std::size_t bit = 0;
    auto writeBits = [&](unsigned v, int n) {
        for (int i = n - 1; i >= 0; --i) {
            if (bit < totalBits) setBit(out, bit, int((v >> i) & 1U));
            ++bit;
        }
    };
    auto decide = [&](double y) {
        double x = (y * norm_ + (levels_ - 1)) / 2.0;
        int idx = int(std::lround(x));
        idx = std::max(0, std::min(levels_ - 1, idx));
        return binaryToGray(unsigned(idx));
    };
    for (const Symbol& s : symbols) {
        if (mod_ == Modulation::BPSK) {
            writeBits(decide(s.i), 1);
        } else {
            writeBits(decide(s.i), bitsPerAxis_);
            writeBits(decide(s.q), bitsPerAxis_);
        }
    }
    return out;
}

void Modem::addNoise(std::vector<Symbol>& symbols) {
    for (Symbol& s : symbols) {
        s.i += sigma_ * gauss_(rng_);
        if (mod_ != Modulation::BPSK) s.q += sigma_ * gauss_(rng_);
    }
}

std::size_t Modem::transmit(std::vector<std::uint8_t>& bytes) {
    if (mod_ == Modulation::None || bytes.empty()) return 0;
    auto symbols = modulate(bytes);
    addNoise(symbols);
    auto rx = demodulate(symbols, bytes.size());
    std::size_t errors = 0;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        std::uint8_t diff = std::uint8_t(bytes[i] ^ rx[i]);
        while (diff) {
            errors += diff & 1U;
            diff >>= 1;
        }
    }
    bytes.swap(rx);
    return errors;
}

BerPoint measureBer(Modulation m, double snrDb, std::uint64_t bits, std::uint32_t seed) {
    Modem modem(m, snrDb, seed);
    std::mt19937 dataRng(seed ^ 0x9E3779B9U);
    std::uniform_int_distribution<int> byteDist(0, 255);
    const std::size_t chunkBytes = 4096;
    std::uint64_t sent = 0, errors = 0;
    std::vector<std::uint8_t> buf(chunkBytes);
    while (sent < bits) {
        for (auto& b : buf) b = std::uint8_t(byteDist(dataRng));
        errors += modem.transmit(buf);
        sent += chunkBytes * 8;
    }
    return {snrDb, theoreticalBer(m, snrDb), double(errors) / double(sent), sent, errors};
}

}  // namespace arq
