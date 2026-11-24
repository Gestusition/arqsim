#pragma once
// Descriptive statistics for repeated runs: mean, sample standard deviation
// and the 95 % confidence half-width using Student's t distribution.
#include <vector>

namespace arq {

struct Summary {
    int n = 0;
    double mean = 0.0;
    double stddev = 0.0;
    double ci95 = 0.0;  // half-width: mean +- ci95
    double min = 0.0;
    double max = 0.0;
};

/// Two-sided 95 % critical value t_{0.975, df}.
double tCritical95(int df);

Summary summarize(const std::vector<double>& xs);

}  // namespace arq
