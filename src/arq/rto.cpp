#include "arq/rto.hpp"

#include <algorithm>
#include <cmath>

namespace arq {

RtoEstimator::RtoEstimator(RtoMode mode, double nominalRttMs, double factor)
    : mode_(mode),
      // Fixed mode uses the proposal's k x RTT. Adaptive mode seeds the
      // estimator with a cached path estimate (SRTT = RTT, RTTVAR = RTT/2, so
      // RTO = 3 RTT) the way Linux reuses per-destination tcp_metrics. RFC
      // 6298's cold-start value of 1 s would dominate 50-packet transfers.
      base_(mode == RtoMode::Adaptive ? std::max(kMinRto, 3.0 * nominalRttMs)
                                      : std::max(kMinRto, factor * nominalRttMs)) {}

double RtoEstimator::rtoMs() const { return std::min(kMaxRto, base_ * double(1U << backoff_)); }

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
    base_ = std::clamp(srtt_ + std::max(kGranularity, 4.0 * rttvar_), kMinRto, kMaxRto);
    backoff_ = 0;
}

void RtoEstimator::onTimeout() {
    if (mode_ != RtoMode::Adaptive) return;
    if (rtoMs() < kMaxRto) ++backoff_;
}

void RtoEstimator::onProgress() {
    // An ACK for new data proves the path works again, so the exponential
    // back-off is cleared (as Linux TCP does). Without this, Karn's rule
    // starves Go-Back-N of RTT samples (every packet in the window was
    // retransmitted) and the back-off keeps growing across unrelated losses.
    backoff_ = 0;
}

}  // namespace arq
