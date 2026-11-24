#include "arq/sender.hpp"

#include <algorithm>
#include <cmath>

#include "core/log.hpp"

namespace arq {

namespace {
constexpr TimePoint kNever = TimePoint::max();

// ---------------------------------------------------------------------------
// Stop-and-Wait: exactly one outstanding packet. ACK carries "next expected".
// ---------------------------------------------------------------------------
class StopAndWaitSender : public ArqSender {
public:
    using ArqSender::ArqSender;

protected:
    void fillWindow(TimePoint now) override {
        if (!awaiting_ && next_ < total()) {
            sendData(next_, false, now);
            deadline_ = afterMs(now, rto_.rtoMs());
            awaiting_ = true;
        }
    }
    void onAck(std::uint32_t ackNo, TimePoint now) override {
        if (awaiting_ && ackNo == next_ + 1) {
            sampleRtt(next_, now);
            ++next_;
            awaiting_ = false;
            deadline_ = kNever;
        }
    }
    void onNak(std::uint32_t seq, TimePoint now) override {
        if (awaiting_ && seq == next_) {
            sendData(next_, true, now);
            deadline_ = afterMs(now, rto_.rtoMs());
        }
    }
    TimePoint nextDeadline() const override { return deadline_; }
    void onTimers(TimePoint now) override {
        if (awaiting_ && now >= deadline_) {
            ++stats_.timeouts;
            rto_.onTimeout();
            ARQ_TRACE("SEND", "timeout seq=%u, rto=%.1f ms", next_, rto_.rtoMs());
            sendData(next_, true, now);
            deadline_ = afterMs(now, rto_.rtoMs());
        }
    }
    bool allAcked() const override { return next_ >= total(); }

private:
    std::uint32_t next_ = 0;
    bool awaiting_ = false;
    TimePoint deadline_ = kNever;
};

// ---------------------------------------------------------------------------
// Go-Back-N: window of N, cumulative ACKs, single timer on the oldest
// unacknowledged packet, retransmit everything from base on timeout.
// ---------------------------------------------------------------------------
class GoBackNSender : public ArqSender {
public:
    using ArqSender::ArqSender;

protected:
    void fillWindow(TimePoint now) override {
        const std::uint32_t n = std::uint32_t(cfg_.effectiveWindow());
        while (nextSeq_ < base_ + n && nextSeq_ < total()) {
            sendData(nextSeq_, false, now);
            if (base_ == nextSeq_) deadline_ = afterMs(now, rto_.rtoMs());
            ++nextSeq_;
        }
    }
    void onAck(std::uint32_t ackNo, TimePoint now) override {
        if (ackNo > base_ && ackNo <= nextSeq_) {
            sampleRtt(ackNo - 1, now);
            base_ = ackNo;
            deadline_ = (base_ == nextSeq_) ? kNever : afterMs(now, rto_.rtoMs());
        }
    }
    void onNak(std::uint32_t seq, TimePoint now) override {
        // Receiver reports a gap at `seq` (== its next expected). Go back once per base.
        if (seq == base_ && base_ < nextSeq_ && nakHandledFor_ != base_) {
            nakHandledFor_ = base_;
            goBack(now);
        }
    }
    TimePoint nextDeadline() const override { return deadline_; }
    void onTimers(TimePoint now) override {
        if (base_ < nextSeq_ && now >= deadline_) {
            ++stats_.timeouts;
            rto_.onTimeout();
            ARQ_TRACE("SEND", "timeout base=%u, resend %u pkts, rto=%.1f ms", base_, nextSeq_ - base_,
                      rto_.rtoMs());
            goBack(now);
        }
    }
    bool allAcked() const override { return base_ >= total(); }

private:
    void goBack(TimePoint now) {
        for (std::uint32_t s = base_; s < nextSeq_; ++s) sendData(s, true, now);
        deadline_ = afterMs(now, rto_.rtoMs());
    }
    std::uint32_t base_ = 0;
    std::uint32_t nextSeq_ = 0;
    std::uint32_t nakHandledFor_ = UINT32_MAX;
    TimePoint deadline_ = kNever;
};

// ---------------------------------------------------------------------------
// Selective Repeat: window of N, individual ACKs, one logical timer per
// packet, only the timed-out (or NAKed) packet is retransmitted.
// ---------------------------------------------------------------------------
class SelectiveRepeatSender : public ArqSender {
public:
    using ArqSender::ArqSender;

protected:
    void fillWindow(TimePoint now) override {
        if (acked_.size() != total()) {
            acked_.assign(total(), false);
            deadline_.assign(total(), kNever);
        }
        const std::uint32_t n = std::uint32_t(cfg_.effectiveWindow());
        while (nextSeq_ < base_ + n && nextSeq_ < total()) {
            sendData(nextSeq_, false, now);
            deadline_[nextSeq_] = afterMs(now, rto_.rtoMs());
            ++nextSeq_;
        }
    }
    void onAck(std::uint32_t seq, TimePoint now) override {
        if (seq >= base_ && seq < nextSeq_ && !acked_[seq]) {
            acked_[seq] = true;
            deadline_[seq] = kNever;
            sampleRtt(seq, now);
            while (base_ < total() && acked_[base_]) ++base_;
        }
    }
    void onNak(std::uint32_t seq, TimePoint now) override {
        if (seq >= base_ && seq < nextSeq_ && !acked_[seq]) {
            // Avoid NAK storms: ignore if this packet was (re)sent very recently.
            if (msBetween(sentAt_[seq], now) < 0.5 * rto_.rtoMs()) return;
            sendData(seq, true, now);
            deadline_[seq] = afterMs(now, rto_.rtoMs());
        }
    }
    TimePoint nextDeadline() const override {
        TimePoint d = kNever;
        for (std::uint32_t s = base_; s < nextSeq_; ++s)
            if (!acked_[s]) d = std::min(d, deadline_[s]);
        return d;
    }
    void onTimers(TimePoint now) override {
        bool any = false;
        for (std::uint32_t s = base_; s < nextSeq_; ++s) {
            if (!acked_[s] && now >= deadline_[s]) {
                if (!any) rto_.onTimeout();  // back off once per timer event
                any = true;
                ++stats_.timeouts;
                ARQ_TRACE("SEND", "timeout seq=%u, rto=%.1f ms", s, rto_.rtoMs());
                sendData(s, true, now);
                deadline_[s] = afterMs(now, rto_.rtoMs());
            }
        }
    }
    bool allAcked() const override { return base_ >= total(); }

private:
    std::uint32_t base_ = 0;
    std::uint32_t nextSeq_ = 0;
    std::vector<bool> acked_;
    std::vector<TimePoint> deadline_;
};

}  // namespace

ArqSender::ArqSender(const ArqConfig& cfg) : cfg_(cfg), rto_(cfg.rtoMode, cfg.rttMs, cfg.rtoFactor) {}

std::unique_ptr<ArqSender> ArqSender::create(const ArqConfig& cfg) {
    switch (cfg.protocol) {
        case ProtocolKind::StopAndWait: return std::make_unique<StopAndWaitSender>(cfg);
        case ProtocolKind::GoBackN: return std::make_unique<GoBackNSender>(cfg);
        case ProtocolKind::SelectiveRepeat: return std::make_unique<SelectiveRepeatSender>(cfg);
    }
    return nullptr;
}

void ArqSender::sendData(std::uint32_t seq, bool retransmission, TimePoint now) {
    Packet p;
    p.type = PacketType::Data;
    p.session = cfg_.session;
    p.seq = seq;
    const std::size_t off = std::size_t(seq) * cfg_.payloadBytes;
    const std::size_t len = std::min(cfg_.payloadBytes, file_->size() - off);
    p.payload.assign(file_->begin() + std::ptrdiff_t(off), file_->begin() + std::ptrdiff_t(off + len));
    auto bytes = encode(p);
    sock_.sendTo(bytes, cfg_.channelData);

    ++stats_.dataTransmissions;
    stats_.payloadBytesSent += len;
    stats_.wireBytesSent += bytes.size();
    sentAt_[seq] = now;
    if (retransmission) {
        ++stats_.retransmissions;
        retransmitted_[seq] = true;
    }
    ARQ_TRACE("SEND", "DATA seq=%u%s", seq, retransmission ? " (retx)" : "");
}

void ArqSender::sampleRtt(std::uint32_t seq, TimePoint now) {
    if (seq < total_ && !retransmitted_[seq]) rto_.onSample(msBetween(sentAt_[seq], now));
}

SenderStats ArqSender::run(const std::vector<std::uint8_t>& file) {
    file_ = &file;
    total_ = std::uint32_t((file.size() + cfg_.payloadBytes - 1) / cfg_.payloadBytes);
    stats_ = SenderStats{};
    stats_.packets = total_;
    sentAt_.assign(total_, TimePoint{});
    retransmitted_.assign(total_, false);
    sock_.bind(cfg_.senderLocal);

    const TimePoint start = Clock::now();
    std::vector<std::uint8_t> buf;
    TimePoint now = start;
    while (!allAcked()) {
        fillWindow(now);
        TimePoint deadline = nextDeadline();
        int waitMs = 50;
        if (deadline != TimePoint::max())
            waitMs = std::max(0, std::min(waitMs, int(std::ceil(msBetween(Clock::now(), deadline)))));
        if (sock_.receive(buf, waitMs)) {
            now = Clock::now();
            auto pkt = decode(buf.data(), buf.size());
            if (!pkt) {
                ++stats_.corruptedFeedback;
                ARQ_TRACE("SEND", "corrupted feedback discarded");
            } else if (pkt->session == cfg_.session) {
                if (pkt->type == PacketType::Ack) {
                    ++stats_.acksReceived;
                    ARQ_TRACE("SEND", "ACK %u", pkt->seq);
                    onAck(pkt->seq, now);
                } else if (pkt->type == PacketType::Nak) {
                    ++stats_.naksReceived;
                    ARQ_TRACE("SEND", "NAK %u", pkt->seq);
                    onNak(pkt->seq, now);
                }
            }
        }
        now = Clock::now();
        onTimers(now);
        if (msBetween(start, now) > cfg_.maxTimeMs) {
            stats_.completed = false;
            stats_.transferMs = msBetween(start, now);
            stats_.finalRtoMs = rto_.rtoMs();
            return stats_;
        }
    }
    stats_.transferMs = msBetween(start, Clock::now());
    stats_.completed = true;
    stats_.meanRttMs = rto_.meanSampleMs();
    stats_.finalRtoMs = rto_.rtoMs();
    finHandshake();
    return stats_;
}

void ArqSender::finHandshake() {
    Packet fin;
    fin.type = PacketType::Fin;
    fin.session = cfg_.session;
    fin.seq = total_;
    auto bytes = encode(fin);
    std::vector<std::uint8_t> buf;
    const int waitMs = std::max(20, int(rto_.rtoMs()));
    for (int attempt = 0; attempt < 40; ++attempt) {
        sock_.sendTo(bytes, cfg_.channelData);
        ARQ_TRACE("SEND", "FIN (attempt %d)", attempt + 1);
        TimePoint until = afterMs(Clock::now(), waitMs);
        while (Clock::now() < until) {
            int left = std::max(0, int(std::ceil(msBetween(Clock::now(), until))));
            if (!sock_.receive(buf, left)) break;
            auto pkt = decode(buf.data(), buf.size());
            if (pkt && pkt->session == cfg_.session && pkt->type == PacketType::FinAck) {
                ARQ_TRACE("SEND", "FINACK received, done");
                return;
            }
        }
    }
}

}  // namespace arq
