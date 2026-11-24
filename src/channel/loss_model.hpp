#pragma once
// Packet-loss models used by the channel emulator.
//
//  * Bernoulli: every packet is dropped independently with probability p.
//  * Gilbert-Elliott: two-state Markov chain (Good / Bad). Packets are lost
//    only in the Bad state, which produces bursts of consecutive losses.
//    The chain is parameterised so that the long-run loss rate equals p and
//    the mean burst length equals B:
//        P(Bad -> Good) = 1 / B
//        P(Good -> Bad) = p / (B (1 - p))
//    so the stationary probability of Bad is exactly p.
#include <random>
#include <string>

namespace arq {

enum class LossKind { Bernoulli, GilbertElliott };

const char* toString(LossKind k);

class LossModel {
public:
    LossModel(LossKind kind, double lossRate, double meanBurst, std::uint32_t seed);
    bool drop();
    LossKind kind() const { return kind_; }

private:
    LossKind kind_;
    double p_;
    double pGoodToBad_ = 0.0;
    double pBadToGood_ = 1.0;
    bool bad_ = false;
    std::mt19937 rng_;
    std::uniform_real_distribution<double> uni_{0.0, 1.0};
};

}  // namespace arq
