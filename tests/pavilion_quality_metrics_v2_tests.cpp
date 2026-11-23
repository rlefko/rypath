// SPDX-License-Identifier: GPL-3.0-only
// Reuse the actual v2 bootstrap stream generator and the unchanged metric core.
#include "pavilion_quality_bootstrap_v2.hpp"
#include "pavilion_quality_metrics.hpp"
#include <iostream>
#include <numeric>

namespace pq = pavilion_quality;
using pavilion_quality_v2::bootstrap_counts;

namespace {
void require_fixture(bool condition) {
    if (!condition)
        throw std::runtime_error("64-replicate v2 numerical fixture failed");
}
void near(double actual, double expected) {
    require_fixture(std::abs(actual - expected) <=
                    2e-12 * std::max(std::numeric_limits<double>::min(), std::abs(expected)));
}
void numerical64() {
    pq::Ensemble ramp(64, std::vector<double>(1));
    for (unsigned i = 0; i < 64; ++i)
        ramp[i][0] = i;
    std::vector<unsigned> ones(64, 1);
    auto value = pq::estimate(ramp, ones, 1, 1, {{0, 0, 1, 1}})[0];
    // Independent closed forms for the integer sequence0..63 and positive LOO half.
    near(value.variance, 64. * 65. / 12.);
    near(value.tail, (32. * (4. * 32. * 32. - 1.) / 12.) * 64. / (63. * 63.));
    near(value.epsilon, 1e-12 * 31.5 * 31.5);

    pq::Ensemble binary(64, std::vector<double>(1, 0));
    binary[1][0] = 64;
    std::vector<unsigned> counts(64, 0);
    counts[0] = 63;
    counts[1] = 1;
    value = pq::estimate(binary, counts, 1, 1, {{0, 0, 1, 1}})[0];
    near(value.variance, 64);
    near(value.tail, 64);
    require_fixture(value.epsilon == 1e-12);
    counts[0] = 0;
    counts[1] = 64;
    auto resampled = pq::estimate(binary, counts, 1, 1, {{0, 0, 1, 1}})[0];
    require_fixture(resampled.variance == 0 && resampled.tail == 0);
    near(resampled.epsilon, 4096e-12); // Epsilon must be recomputed inside the bootstrap.

    pq::Ensemble spatial(64, std::vector<double>(1001, 0));
    spatial[63][0] = 3;
    spatial[63][500] = 2;
    spatial[63][1000] = 1;
    auto ranked = pq::estimate(spatial, ones, 1001, 1, {{0, 0, 1001, 1}})[0];
    near(ranked.variance, 14. / (64. * 1001.));
    near(ranked.tail, 13. / (64. * 1001.)); // ceil(.001*1001)=2, never rounded down.
    require_fixture(pq::score(0, 0, 1e-12) < -600);
    near(pq::score(2e-12, 0, 1e-12), std::log(2.));
    require_fixture(!pq::mean_gate(std::vector<double>(64, 1), std::vector<double>(64, 1.01)));

    std::vector<double> ranks(20000);
    std::iota(ranks.begin(), ranks.end(), -100.);
    near(pq::quantile95(ranks), 18899.); // ceil(.95*20000)-1, then nonnegative.
    require_fixture(pq::quantile95(std::vector<double>(20000, -1)) == 0);
    const auto first = bootstrap_counts({.replicates = 64, .seed = 1380798800, .draw = 0});
    require_fixture(first == bootstrap_counts({.replicates = 64, .seed = 1380798800, .draw = 0}));
    require_fixture(first != bootstrap_counts({.replicates = 64, .seed = 1380798800, .draw = 1}));
    for (const auto &arm : first)
        require_fixture(arm.size() == 64 && std::accumulate(arm.begin(), arm.end(), 0u) == 64);
    require_fixture(first[0] != first[1]);
    bool rejected = false;
    try {
        (void)bootstrap_counts({.replicates = 32, .seed = 1380798800, .draw = 0});
    } catch (const std::runtime_error &) {
        rejected = true;
    }
    require_fixture(rejected);
}
} // namespace
int main() {
    try {
        numerical64();
        std::cout << "64-replicate variance/LOO/rank/epsilon/bootstrap fixtures passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
