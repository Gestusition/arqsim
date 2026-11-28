#pragma once
// "arqsim conclude": reads the CSV files of a finished sweep and prints the
// findings and a data-driven verdict (best protocol / RTO / modulation and why).
#include <string>

namespace arq::cli {

/// `dir` is either the output root of `sweep --preset all` (one sub-folder per
/// preset) or a single sweep folder containing summary.csv. Returns 0 on success,
/// 1 if no results were found.
int printConclusion(const std::string& dir);

}  // namespace arq::cli
