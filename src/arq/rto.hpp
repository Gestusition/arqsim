#pragma once
// Retransmission timeout estimation.
//
//  Fixed    : RTO = factor * nominal RTT   (the policy from the proposal)
//  Adaptive : RFC 6298 / Jacobson-Karels estimator
//               SRTT   <- 7/8 SRTT + 1/8 R
//               RTTVAR <- 3/4 RTTVAR + 1/4 |SRTT - R|
//               RTO    =  SRTT + max(G, 4 RTTVAR)
//             with Karn's algorithm (no samples from retransmitted packets)
//             and exponential back-off on every timeout; the back-off is
//             cleared when an ACK acknowledges new data (as in Linux TCP).
#include "arq/protocol.hpp"

namespace arq {

class RtoEstimator {
public:
    RtoEstimator(RtoMode mode, double nominalRttMs, double factor);

    double rtoMs() const;
    void onSample(double rttMs);  // caller enforces Karn's rule
    void onTimeout();             // exponential back-off (adaptive only)
    void onProgress();            // new data acknowledged: clear back-off

    double srttMs() const { return srtt_; }
    int samples() const { return samples_; }
    double meanSampleMs() const { return samples_ ? sampleSum_ / samples_ : 0.0; }

private:
    RtoMode mode_;
    double base_;       // RTO before back-off
    int backoff_ = 0;   // number of consecutive doublings
    double srtt_ = 0.0;
    double rttvar_ = 0.0;
    int samples_ = 0;
    double sampleSum_ = 0.0;
    static constexpr double kMinRto = 5.0;
    static constexpr double kMaxRto = 4000.0;
    static constexpr double kGranularity = 5.0;  // absorbs OS scheduling jitter (~1-3 ms on Windows)
};

}  // namespace arq
