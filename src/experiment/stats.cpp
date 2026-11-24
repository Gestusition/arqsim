#include "experiment/stats.hpp"

#include <algorithm>
#include <cmath>

namespace arq {

double tCritical95(int df) {
    static const double table[] = {0,      12.706, 4.303, 3.182, 2.776, 2.571, 2.447, 2.365, 2.306, 2.262,
                                   2.228,  2.201,  2.179, 2.160, 2.145, 2.131, 2.120, 2.110, 2.101, 2.093,
                                   2.086,  2.080,  2.074, 2.069, 2.064, 2.060, 2.056, 2.052, 2.048, 2.045,
                                   2.042};
    if (df <= 0) return 0.0;
    if (df <= 30) return table[df];
    if (df <= 40) return 2.021;
    if (df <= 60) return 2.000;
    if (df <= 120) return 1.980;
    return 1.960;
}

Summary summarize(const std::vector<double>& xs) {
    Summary s;
    s.n = int(xs.size());
    if (xs.empty()) return s;
    double sum = 0.0;
    for (double x : xs) sum += x;
    s.mean = sum / s.n;
    s.min = *std::min_element(xs.begin(), xs.end());
    s.max = *std::max_element(xs.begin(), xs.end());
    if (s.n > 1) {
        double ss = 0.0;
        for (double x : xs) ss += (x - s.mean) * (x - s.mean);
        s.stddev = std::sqrt(ss / (s.n - 1));
        s.ci95 = tCritical95(s.n - 1) * s.stddev / std::sqrt(double(s.n));
    }
    return s;
}

}  // namespace arq
