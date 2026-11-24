#pragma once
// Minimal "--key value" command-line parser.
#include <cstdlib>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace arq::cli {

class Args {
public:
    Args(int argc, char** argv, int start) {
        for (int i = start; i < argc; ++i) {
            std::string a = argv[i];
            if (a.rfind("--", 0) == 0) {
                std::string key = a.substr(2), val = "1";
                auto eq = key.find('=');
                if (eq != std::string::npos) {
                    val = key.substr(eq + 1);
                    key = key.substr(0, eq);
                } else if (i + 1 < argc && !(argv[i + 1][0] == '-' && argv[i + 1][1] == '-')) {
                    val = argv[++i];
                }
                opts_[key] = val;
            } else if (a == "-h") {
                opts_["help"] = "1";
            } else {
                positional_.push_back(a);
            }
        }
    }

    bool has(const std::string& k) const { return opts_.count(k) > 0; }
    std::string str(const std::string& k, const std::string& def = "") const {
        used_.insert(k);
        auto it = opts_.find(k);
        return it == opts_.end() ? def : it->second;
    }
    double num(const std::string& k, double def) const {
        used_.insert(k);
        auto it = opts_.find(k);
        if (it == opts_.end()) return def;
        char* end = nullptr;
        double v = std::strtod(it->second.c_str(), &end);
        if (end == it->second.c_str() || *end != '\0')
            throw std::runtime_error("option --" + k + " expects a number, got '" + it->second + "'");
        return v;
    }
    bool flag(const std::string& k) const {
        used_.insert(k);
        auto it = opts_.find(k);
        return it != opts_.end() && it->second != "0" && it->second != "false" && it->second != "off";
    }
    std::vector<std::string> list(const std::string& k, const std::string& def) const {
        std::vector<std::string> out;
        std::stringstream ss(str(k, def));
        std::string item;
        while (std::getline(ss, item, ','))
            if (!item.empty()) out.push_back(item);
        return out;
    }
    /// Numeric list; also accepts ranges "start:stop:step".
    std::vector<double> numList(const std::string& k, const std::string& def) const {
        std::vector<double> out;
        for (const auto& item : list(k, def)) {
            auto c1 = item.find(':');
            if (c1 == std::string::npos) {
                out.push_back(std::strtod(item.c_str(), nullptr));
                continue;
            }
            auto c2 = item.find(':', c1 + 1);
            double a = std::strtod(item.substr(0, c1).c_str(), nullptr);
            double b = std::strtod(item.substr(c1 + 1, c2 - c1 - 1).c_str(), nullptr);
            double step = c2 == std::string::npos ? 1.0 : std::strtod(item.substr(c2 + 1).c_str(), nullptr);
            if (step <= 0) step = 1.0;
            for (double v = a; v <= b + step * 1e-9; v += step) out.push_back(v);
        }
        return out;
    }
    const std::vector<std::string>& positional() const { return positional_; }

    /// Options that were given but never queried (typos).
    std::vector<std::string> unused() const {
        std::vector<std::string> out;
        for (const auto& kv : opts_)
            if (!used_.count(kv.first)) out.push_back(kv.first);
        return out;
    }

private:
    std::map<std::string, std::string> opts_;
    std::vector<std::string> positional_;
    mutable std::set<std::string> used_;
};

}  // namespace arq::cli
