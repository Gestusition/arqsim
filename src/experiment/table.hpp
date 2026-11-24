#pragma once
// Tiny CSV table used for raw results, aggregated summaries and plotting.
#include <map>
#include <string>
#include <vector>

namespace arq {

class Table {
public:
    Table() = default;
    explicit Table(std::vector<std::string> columns) : columns_(std::move(columns)) {}

    const std::vector<std::string>& columns() const { return columns_; }
    const std::vector<std::vector<std::string>>& rows() const { return rows_; }
    std::size_t size() const { return rows_.size(); }

    void addRow(std::vector<std::string> row) { rows_.push_back(std::move(row)); }
    int col(const std::string& name) const;  // -1 if missing
    std::string get(std::size_t row, const std::string& name) const;
    double num(std::size_t row, const std::string& name, double fallback = 0.0) const;

    void writeCsv(const std::string& path) const;
    static Table readCsv(const std::string& path);  // throws on I/O error

    /// Groups rows by `keys` and aggregates `metrics` (mean, std, 95 % CI).
    /// Output columns: keys..., runs, <metric>_mean, <metric>_std, <metric>_ci95 ...
    Table aggregate(const std::vector<std::string>& keys, const std::vector<std::string>& metrics) const;

    /// Returns the subset of rows where every filter column equals the value.
    Table filter(const std::map<std::string, std::string>& eq) const;

    /// Distinct values of a column, in order of first appearance.
    std::vector<std::string> distinct(const std::string& name) const;

private:
    std::vector<std::string> columns_;
    std::vector<std::vector<std::string>> rows_;
};

/// Columns that identify an experiment configuration (used for grouping).
std::vector<std::string> configColumns();
/// Numeric metrics that are aggregated across repetitions.
std::vector<std::string> metricColumns();

}  // namespace arq
