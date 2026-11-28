#include "experiment/table.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>

#include "experiment/stats.hpp"

namespace arq {

namespace {
std::string escape(const std::string& s) {
    if (s.find_first_of(",\"\n") == std::string::npos) return s;
    std::string out = "\"";
    for (char c : s) {
        if (c == '"') out += "\"\"";
        else out += c;
    }
    return out + "\"";
}

std::vector<std::string> splitCsvLine(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    bool quoted = false;
    for (std::size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (quoted) {
            if (c == '"' && i + 1 < line.size() && line[i + 1] == '"') {
                cur += '"';
                ++i;
            } else if (c == '"') quoted = false;
            else cur += c;
        } else if (c == '"') quoted = true;
        else if (c == ',') {
            out.push_back(cur);
            cur.clear();
        } else if (c != '\r') cur += c;
    }
    out.push_back(cur);
    return out;
}

std::string fmt(double v) {
    char b[64];
    std::snprintf(b, sizeof(b), "%.6g", v);
    return b;
}
}  // namespace

std::vector<std::string> configColumns() {
    return {"protocol", "window", "payload", "file_bytes", "rtt_ms", "jitter_ms", "loss", "ack_loss",
            "loss_model", "burst", "rto_mode", "nak", "modulation", "mod_used", "snr_db", "link_kbps"};
}

std::vector<std::string> metricColumns() {
    return {"integrity",   "transfer_ms", "throughput_kbps", "efficiency", "utilization",   "theory_util",
            "data_tx",     "retransmissions", "timeouts",    "naks",       "ch_data_drop",  "ch_ack_drop",
            "ch_data_corrupt", "ch_ack_corrupt", "ber",      "mean_rtt_ms", "final_rto_ms",
            "late_mean_ms", "late_max_ms"};
}

int Table::col(const std::string& name) const {
    for (std::size_t i = 0; i < columns_.size(); ++i)
        if (columns_[i] == name) return int(i);
    return -1;
}

std::string Table::get(std::size_t row, const std::string& name) const {
    int c = col(name);
    if (c < 0 || std::size_t(c) >= rows_[row].size()) return "";
    return rows_[row][std::size_t(c)];
}

double Table::num(std::size_t row, const std::string& name, double fallback) const {
    std::string s = get(row, name);
    if (s.empty()) return fallback;
    char* end = nullptr;
    double v = std::strtod(s.c_str(), &end);
    return end == s.c_str() ? fallback : v;
}

void Table::writeCsv(const std::string& path) const {
    std::ofstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot write " + path);
    for (std::size_t i = 0; i < columns_.size(); ++i) f << (i ? "," : "") << escape(columns_[i]);
    f << "\n";
    for (const auto& r : rows_) {
        for (std::size_t i = 0; i < r.size(); ++i) f << (i ? "," : "") << escape(r[i]);
        f << "\n";
    }
}

Table Table::readCsv(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot read " + path);
    std::string line;
    Table t;
    if (!std::getline(f, line)) return t;
    t.columns_ = splitCsvLine(line);
    while (std::getline(f, line)) {
        if (line.empty() || line == "\r") continue;
        t.rows_.push_back(splitCsvLine(line));
    }
    return t;
}

Table Table::aggregate(const std::vector<std::string>& keys, const std::vector<std::string>& metrics) const {
    std::vector<std::string> usedKeys, usedMetrics;
    for (const auto& k : keys)
        if (col(k) >= 0) usedKeys.push_back(k);
    for (const auto& m : metrics)
        if (col(m) >= 0) usedMetrics.push_back(m);

    std::vector<std::string> outCols = usedKeys;
    outCols.push_back("runs");
    for (const auto& m : usedMetrics) {
        outCols.push_back(m + "_mean");
        outCols.push_back(m + "_std");
        outCols.push_back(m + "_ci95");
    }
    Table out(outCols);

    std::vector<std::vector<std::string>> groupKeys;
    std::map<std::vector<std::string>, std::vector<std::size_t>> groups;
    for (std::size_t r = 0; r < rows_.size(); ++r) {
        std::vector<std::string> key;
        for (const auto& k : usedKeys) key.push_back(get(r, k));
        auto it = groups.find(key);
        if (it == groups.end()) {
            groupKeys.push_back(key);
            groups[key] = {r};
        } else it->second.push_back(r);
    }
    for (const auto& key : groupKeys) {
        const auto& idx = groups[key];
        std::vector<std::string> row = key;
        row.push_back(std::to_string(idx.size()));
        for (const auto& m : usedMetrics) {
            std::vector<double> xs;
            for (std::size_t r : idx) xs.push_back(num(r, m));
            Summary s = summarize(xs);
            row.push_back(fmt(s.mean));
            row.push_back(fmt(s.stddev));
            row.push_back(fmt(s.ci95));
        }
        out.addRow(std::move(row));
    }
    return out;
}

Table Table::filter(const std::map<std::string, std::string>& eq) const {
    Table out(columns_);
    for (std::size_t r = 0; r < rows_.size(); ++r) {
        bool ok = true;
        for (const auto& kv : eq) {
            std::string v = get(r, kv.first);
            if (v == kv.second) continue;
            // numeric equality (e.g. "0.05" == "0.050")
            char* e1 = nullptr;
            char* e2 = nullptr;
            double a = std::strtod(v.c_str(), &e1), b = std::strtod(kv.second.c_str(), &e2);
            if (!v.empty() && !kv.second.empty() && *e1 == '\0' && *e2 == '\0' && a == b) continue;
            ok = false;
            break;
        }
        if (ok) out.addRow(rows_[r]);
    }
    return out;
}

std::vector<std::string> Table::distinct(const std::string& name) const {
    std::vector<std::string> out;
    for (std::size_t r = 0; r < rows_.size(); ++r) {
        std::string v = get(r, name);
        bool seen = false;
        for (const auto& o : out) seen = seen || (o == v);
        if (!seen) out.push_back(v);
    }
    return out;
}

}  // namespace arq
