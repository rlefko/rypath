// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <vector>
namespace pavilion_quality {
struct Region {
    unsigned x0, y0, x1, y1;
};
struct Estimate {
    double variance = 0, tail = 0, epsilon = 0;
};
using Ensemble = std::vector<std::vector<double>>;
inline std::vector<Estimate> estimate(const Ensemble &images, const std::vector<unsigned> &counts,
                                      unsigned width, unsigned height,
                                      const std::vector<Region> &regions) {
    const unsigned n = std::accumulate(counts.begin(), counts.end(), 0u);
    const std::size_t pixels = std::size_t(width) * height;
    if (n < 2 || images.size() != counts.size() || pixels == 0)
        throw std::runtime_error("Invalid ensemble");
    std::vector<double> mean(pixels, 0), variance(pixels, 0);
    for (unsigned r = 0; r < images.size(); ++r) {
        if (images[r].size() != pixels)
            throw std::runtime_error("Invalid image dimensions");
        if (!counts[r])
            continue;
        const double w = double(counts[r]) / n;
        for (std::size_t p = 0; p < pixels; ++p) {
            if (!std::isfinite(images[r][p]))
                throw std::runtime_error("Nonfinite image");
            mean[p] += w * images[r][p];
        }
    }
    for (unsigned r = 0; r < images.size(); ++r)
        if (counts[r])
            for (std::size_t p = 0; p < pixels; ++p) {
                const double d = images[r][p] - mean[p];
                variance[p] += double(counts[r]) * d * d / (n - 1);
            }
    std::vector<Estimate> result;
    for (const auto &roi : regions) {
        if (roi.x0 >= roi.x1 || roi.y0 >= roi.y1 || roi.x1 > width || roi.y1 > height)
            throw std::runtime_error("Invalid region");
        const std::size_t size = std::size_t(roi.x1 - roi.x0) * (roi.y1 - roi.y0);
        const std::size_t k = std::max(std::size_t{1}, std::size_t(std::ceil(.001 * size)));
        Estimate out;
        double mean_square = 0;
        for (unsigned y = roi.y0; y < roi.y1; ++y)
            for (unsigned x = roi.x0; x < roi.x1; ++x) {
                const auto p = std::size_t(y) * width + x;
                out.variance += variance[p] / size;
                mean_square += mean[p] * mean[p] / size;
            }
        out.epsilon = 1e-12 * std::max(1., mean_square);
        std::vector<double> heap(k);
        for (unsigned r = 0; r < images.size(); ++r)
            if (counts[r]) {
                std::fill(heap.begin(), heap.end(), 0.);
                for (unsigned y = roi.y0; y < roi.y1; ++y)
                    for (unsigned x = roi.x0; x < roi.x1; ++x) {
                        const auto p = std::size_t(y) * width + x;
                        const double residual = (images[r][p] - mean[p]) * n / (n - 1);
                        if (residual <= 0)
                            continue;
                        const double energy = residual * residual;
                        if (energy <= heap.front())
                            continue;
                        std::pop_heap(heap.begin(), heap.end(), std::greater<double>());
                        heap.back() = energy;
                        std::push_heap(heap.begin(), heap.end(), std::greater<double>());
                    }
                out.tail +=
                    std::accumulate(heap.begin(), heap.end(), 0.) * counts[r] / (double(n) * size);
            }
        if (!std::isfinite(out.variance) || !std::isfinite(out.tail) || !std::isfinite(out.epsilon))
            throw std::runtime_error("Nonfinite metric");
        result.push_back(out);
    }
    return result;
}
inline double score(double candidate, double reference, double epsilon) {
    if (!std::isfinite(candidate) || !std::isfinite(reference) || !std::isfinite(epsilon) ||
        candidate < 0 || reference < 0 || epsilon <= 0)
        throw std::runtime_error("Invalid gate energy");
    // The epsilon selects the absolute reference gate; do not add candidate energy.
    return std::log(std::max(candidate, std::numeric_limits<double>::min())) -
           std::log(reference <= epsilon ? epsilon : reference);
}
inline double quantile95(std::vector<double> maxima) {
    if (maxima.empty())
        throw std::runtime_error("No bootstrap draws");
    for (double v : maxima)
        if (!std::isfinite(v))
            throw std::runtime_error("Inconclusive bootstrap");
    const auto index = std::min(maxima.size() - 1, std::size_t(std::ceil(.95 * maxima.size())) - 1);
    std::nth_element(maxima.begin(), maxima.begin() + index, maxima.end());
    return std::max(0., maxima[index]);
}
inline bool mean_gate(const std::vector<double> &a, const std::vector<double> &b) {
    if (a.size() < 2 || b.size() < 2)
        throw std::runtime_error("Missing means");
    auto moments = [](const auto &v) {
        double mean = std::accumulate(v.begin(), v.end(), 0.) / v.size(), var = 0;
        for (double x : v) {
            if (!std::isfinite(x))
                throw std::runtime_error("Nonfinite mean");
            var += (x - mean) * (x - mean);
        }
        return std::array{mean, var / (v.size() * (v.size() - 1))};
    };
    auto x = moments(a), y = moments(b);
    return std::abs(x[0] - y[0]) <=
           5 * std::sqrt(x[1] + y[1]) + 2e-5 * std::max(1., std::abs(y[0]));
}
} // namespace pavilion_quality
