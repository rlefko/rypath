// SPDX-License-Identifier: GPL-3.0-only
#include "pavilion_quality_metrics.hpp"
#include <iostream>
using namespace pavilion_quality;
void require(bool v) {
    if (!v)
        throw std::runtime_error("Quality metric fixture failed");
}
void test_quality_metrics() {
    Ensemble x{{0, 0, 0, 0}, {2, 2, 2, 2}};
    auto e = estimate(x, {1, 1}, 2, 2, {{0, 0, 2, 2}})[0];
    require(e.variance == 2 && e.tail == .5 && e.epsilon == 1e-12);
    // Duplicating a source replicate changes the in-bootstrap leave-one-out
    // mean and ranks; reusing precomputed residual energies would fail this.
    auto d = estimate(x, {2, 0}, 2, 2, {{0, 0, 2, 2}})[0];
    require(d.variance == 0 && d.tail == 0);
    Ensemble outlier(4, std::vector<double>(1000, 0));
    outlier[3][999] = 100;
    auto o = estimate(outlier, {1, 1, 1, 1}, 1000, 1, {{0, 0, 1000, 1}})[0];
    require(std::abs(o.variance - 2.5) < 1e-12 && std::abs(o.tail - 2.5) < 1e-12);
    require(std::isfinite(score(0, 0, 1e-12)) && score(0, 0, 1e-12) < -600 &&
            score(2e-12, 0, 1e-12) > std::log(1.25));
    require(!mean_gate({1, 1, 1}, {1.01, 1.01, 1.01}));
    require(mean_gate({1, 2, 3}, {1, 2, 3}));
    require(quantile95({0, 0, 0, 10}) == 10);
    bool rejected = false;
    try {
        estimate({{NAN}, {0}}, {1, 1}, 1, 1, {{0, 0, 1, 1}});
    } catch (...) {
        rejected = true;
    }
    require(rejected);
    std::cout
        << "Variance, leave-one-out tail, resampling, zero energy, mean bias and bounds passed\n";
}
int main() {
    try {
        test_quality_metrics();
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
