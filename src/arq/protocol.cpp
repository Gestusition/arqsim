#include "arq/protocol.hpp"

#include <algorithm>
#include <cctype>

namespace arq {

namespace {
std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    s.erase(std::remove(s.begin(), s.end(), '-'), s.end());
    s.erase(std::remove(s.begin(), s.end(), '_'), s.end());
    return s;
}
}  // namespace

const char* toString(ProtocolKind p) {
    switch (p) {
        case ProtocolKind::StopAndWait: return "sw";
        case ProtocolKind::GoBackN: return "gbn";
        case ProtocolKind::SelectiveRepeat: return "sr";
    }
    return "?";
}

const char* toString(RtoMode m) { return m == RtoMode::Fixed ? "fixed" : "adaptive"; }

bool parseProtocol(const std::string& raw, ProtocolKind& out) {
    std::string s = lower(raw);
    if (s == "sw" || s == "stopandwait" || s == "saw") out = ProtocolKind::StopAndWait;
    else if (s == "gbn" || s == "gobackn") out = ProtocolKind::GoBackN;
    else if (s == "sr" || s == "selectiverepeat") out = ProtocolKind::SelectiveRepeat;
    else return false;
    return true;
}

bool parseRtoMode(const std::string& raw, RtoMode& out) {
    std::string s = lower(raw);
    if (s == "fixed" || s == "static") out = RtoMode::Fixed;
    else if (s == "adaptive" || s == "jacobson" || s == "rfc6298") out = RtoMode::Adaptive;
    else return false;
    return true;
}

}  // namespace arq
