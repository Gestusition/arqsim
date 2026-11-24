#include "channel/loss_model.hpp"

#include <algorithm>

namespace arq {

const char* toString(LossKind k) { return k == LossKind::Bernoulli ? "bernoulli" : "gilbert"; }

LossModel::LossModel(LossKind kind, double lossRate, double meanBurst, std::uint32_t seed)
    : kind_(kind), p_(std::clamp(lossRate, 0.0, 0.999)), rng_(seed) {
    if (kind_ == LossKind::GilbertElliott) {
        double burst = std::max(1.0, meanBurst);
        pBadToGood_ = 1.0 / burst;
        pGoodToBad_ = (p_ <= 0.0) ? 0.0 : std::min(1.0, p_ / (burst * (1.0 - p_)));
    }
}

bool LossModel::drop() {
    if (p_ <= 0.0) return false;
    if (kind_ == LossKind::Bernoulli) return uni_(rng_) < p_;
    // Gilbert-Elliott: advance the Markov chain once per packet.
    if (bad_) {
        if (uni_(rng_) < pBadToGood_) bad_ = false;
    } else {
        if (uni_(rng_) < pGoodToBad_) bad_ = true;
    }
    return bad_;
}

}  // namespace arq
