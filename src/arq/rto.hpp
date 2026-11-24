#pragma once
// Retransmission timeout estimation.
//
//  Fixed    : RTO = factor * nominal RTT   (the policy from the proposal)
//  Adaptive : RFC 6298 / Jacobson-Karels estimator
//               SRTT   <- 7/8 SRTT + 1/8 R
//               RTTVAR <- 3/4 RTTVAR + 1/4 |SRTT - R|
//               RTO    =  SRTT + max(G, 4 RTTVAR)
//             with Karn's algorithm (no samples from retransmitted packets)
//             and exponential back-off on every timeout.
#include "arq/protocol.hpp"

namespace arq {

class RtoEstimator {
public:
    RtoEstimator(RtoMode mode, double nominalRttMs, double factor);

    double rtoMs() const { return rto_; }
    void onSample(double rttMs);  // caller enforces Karn's rule
    void onTimeout();             // exponential back-off (adaptive only)

    double srttMs() const { return srtt_; }
    int samples() const { return samples_; }
    double meanSampleMs() const { return samples_ ? sampleSum_ / samples_ : 0.0; }

private:
    RtoMode mode_;
    double rto_;
    double srtt_ = 0.0;
    double rttvar_ = 0.0;
    int samples_ = 0;
    double sampleSum_ = 0.0;
    static constexpr double kMinRto = 5.0;
    static constexpr double kMaxRto = 4000.0;
    static constexpr double kGranularity = 1.0;
};

}  // namespace arq
