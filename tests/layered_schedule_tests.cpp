// SPDX-License-Identifier: GPL-3.0-only
// Fixed-direction proposal-measure checks, independent of LayeredBxDF walks.
#include "dielectric_reference.hpp"
#include "layered_reference.hpp"
#include <array>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
namespace d = dielectric_reference;
void require(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
double conditional_transmission(d::P material, d::V from, d::V to) {
    require(from.z * to.z < 0 && !material.smooth, "continuous transmission domain");
    const double eta = from.z > 0 ? material.eta : 1 / material.eta;
    auto normal = d::unit(from + eta * to);
    if (normal.z < 0)
        normal = -normal;
    const double transmission = 1 - d::fresnel(d::dot(from, normal), material.eta);
    require(transmission > 0, "positive conditional transmission support");
    // evaluate() contains the all-lobe branch probability. The actual exit
    // proposal conditions Sample_f on Transmission, so remove that probability.
    return d::evaluate(material, from, to).pdf / transmission;
}
double power(double a, double b) { return a * a / (a * a + b * b); }
void partition(double exit_reverse, double exit_forward, double competing) {
    require(exit_reverse > 0 && exit_forward > 0 && competing > 0 &&
                std::isfinite(exit_reverse + exit_forward + competing),
            "finite positive actual proposal densities");
    // Virtual-light arm: sample the exit from external wi in the opposite mode.
    // Forward-walk arm: sample an internal surface or phase direction. Both
    // densities must be evaluated in that SAME internal solid-angle measure.
    const double virtual_weight = power(exit_reverse, competing);
    const double corrected_walk_weight = power(competing, exit_reverse);
    const double stock_walk_weight = power(competing, exit_forward);
    constexpr double eps = std::numeric_limits<double>::epsilon();
    // Two squares, one sum and a division per weight plus final sum: gamma(16)
    // conservatively bounds roundoff for these finite normal-sized fixed inputs.
    constexpr double gamma16 = 16 * eps / (1 - 16 * eps);
    require(std::abs(virtual_weight + corrected_walk_weight - 1) <= gamma16,
            "correct forward/reverse proposal partition");
    require(std::abs(exit_reverse - exit_forward) > 64 * eps * std::max(exit_reverse, exit_forward),
            "fixture must expose forward/reverse density difference");
    require(std::abs(virtual_weight + stock_walk_weight - 1) > gamma16,
            "stock forward-exit density must expose partition failure");
}
void directions() {
    const auto material = d::prepare(1.5, double(0.102019608f), double(0.102019608f), false);
    const auto near =
        d::unit({double(-0.00149562897f), double(-0.04186568409f), double(0.999122083f)});
    const auto grazing = d::V{std::sin(75 * d::pi / 180), 0, std::cos(75 * d::pi / 180)};
    unsigned surface = 0, phase = 0;
    for (auto external : std::array{near, grazing}) {
        // Fixed internal directions around the Snell centre. No Sample_f, RNG,
        // acceptance selection, post-outcome binning or PDF array normalization.
        const double x = -external.x / material.eta, y = -external.y / material.eta;
        const double z = -std::sqrt(1 - x * x - y * y);
        for (double radius : {0.002, 0.02, 0.05, 0.1})
            for (unsigned azimuth = 0; azimuth < 8; ++azimuth) {
                const double phi = 2 * d::pi * azimuth / 8;
                const auto inward =
                    d::unit({x + radius * std::cos(phi), y + radius * std::sin(phi), z});
                const double reverse = conditional_transmission(material, external, inward);
                const double forward = conditional_transmission(material, inward, external);
                // Diffuse base outgoing direction is -inward, on its upper side.
                partition(reverse, forward, -inward.z / d::pi);
                ++surface;
                // Actual HG phase PDF, with a fixed incoming propagation state;
                // g=0 and .35 exercise isotropic and directional alternatives.
                const auto incoming = d::unit({0.3, -0.2, -1});
                for (double g : {0., .35}) {
                    partition(reverse, forward,
                              layered_reference::hg(d::dot(-incoming, -inward), g));
                    ++phase;
                }
            }
    }
    require(surface == 64 && phase == 128, "complete fixed direction inventory");
    std::cout << "PASS 64 surface and 128 phase proposal partitions\n";
}
} // namespace
int main() {
    try {
        directions();
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
