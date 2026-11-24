#include "arq/rto.hpp"

#include <algorithm>
#include <cmath>

namespace arq {

RtoEstimator::RtoEstimator(RtoMode mode, double nominalRttMs, double factor)
    : mode_(mode), rto_(std::max(kMinRto, factor * nominalRttMs)) {}

void RtoEstimator::onSample(double r) {
    ++samples_;
    sampleSum_ += r;
    if (mode_ != RtoMode::Adaptive) return;
    if (samples_ == 1) {
        srtt_ = r;
        rttvar_ = r / 2.0;
    } else {
        rttvar_ = 0.75 * rttvar_ + 0.25 * std::fabs(srtt_ - r);
        srtt_ = 0.875 * srtt_ + 0.125 * r;
    }
    rto_ = std::clamp(srtt_ + std::max(kGranularity, 4.0 * rttvar_), kMinRto, kMaxRto);
}

void RtoEstimator::onTimeout() {
    if (mode_ != RtoMode::Adaptive) return;
    rto_ = std::min(kMaxRto, rto_ * 2.0);
}

}  // namespace arq
