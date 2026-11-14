// SPDX-License-Identifier: GPL-3.0-only
#include "reflectance_curve.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>

namespace rypath {
namespace {

// Error-free TwoSum expansion, following Shewchuk (1997), as in geometry.cpp.
// https://www.cs.cmu.edu/~quake/robust.html
// The six terms below are exact products of at most two binary32 values. Their
// exponents fit comfortably in binary64, even for subnormals and FLT_MAX.
class Expansion {
  public:
    void add(double term) {
        std::size_t output = 0;
        for (std::size_t i = 0; i < size_; ++i) {
            const double sum = term + values_[i];
            const double rounded = sum - term;
            const double error = (term - (sum - rounded)) + (values_[i] - rounded);
            if (error != 0) {
                values_[output++] = error;
            }
            term = sum;
        }
        if (term != 0) {
            values_[output++] = term;
        }
        size_ = output;
    }

    int sign() const { return size_ == 0 ? 0 : (values_[size_ - 1] > 0 ? 1 : -1); }

    double estimate() const {
        double value = 0;
        for (std::size_t i = 0; i < size_; ++i) {
            value += values_[i];
        }
        return value;
    }

  private:
    std::array<double, 6> values_{};
    std::size_t size_ = 0;
};

enum class Threshold { zero, one };

Expansion numerator(SpectralKnot a, SpectralKnot b, float wavelength, Threshold threshold) {
    // (b.x - a.x) * (raw(wavelength) - threshold). Expanding the
    // determinant before summation preserves small residuals when the source
    // interval and signed ordinates span most of the binary32 range.
    Expansion value;
    value.add(double{b.wavelength} * a.value);
    value.add(-double{a.wavelength} * b.value);
    value.add(double{wavelength} * b.value);
    value.add(-double{wavelength} * a.value);
    if (threshold == Threshold::one) {
        value.add(-double{b.wavelength});
        value.add(double{a.wavelength});
    }
    return value;
}

float clamp_ordinate(float value) { return value <= 0 ? 0.f : std::min(value, 1.f); }

float evaluate(SpectralKnot a, SpectralKnot b, float wavelength) {
    const auto value = numerator(a, b, wavelength, Threshold::zero);
    if (value.sign() <= 0) {
        return 0;
    }
    if (numerator(a, b, wavelength, Threshold::one).sign() >= 0) {
        return 1;
    }
    return std::clamp(static_cast<float>(value.estimate() / (double{b.wavelength} - a.wavelength)),
                      0.f, 1.f);
}

class SegmentPoints {
  public:
    SegmentPoints(float left, float right) {
        add(left);
        add(right);
    }

    void crossing(SpectralKnot a, SpectralKnot b, Threshold threshold) {
        const auto left = points_[0];
        const auto right = points_[1];
        const int left_sign = numerator(a, b, left, threshold).sign();
        const int right_sign = numerator(a, b, right, threshold).sign();
        if (left_sign * right_sign != -1) {
            return;
        }
        // All searched wavelengths are positive and normal; their bit patterns
        // have numerical order. [360,830] requires at most 24 bisections.
        auto low = std::bit_cast<std::uint32_t>(left);
        auto high = std::bit_cast<std::uint32_t>(right);
        while (high - low > 1) {
            const auto middle = low + (high - low) / 2;
            const auto wavelength = std::bit_cast<float>(middle);
            const int sign = numerator(a, b, wavelength, threshold).sign();
            if (sign == 0) {
                add(wavelength);
                return;
            }
            if (sign == left_sign) {
                low = middle;
            } else {
                high = middle;
            }
        }
        add(std::bit_cast<float>(low));
        add(std::bit_cast<float>(high));
    }

    std::span<const float> sorted() {
        const auto end = points_.begin() + static_cast<std::ptrdiff_t>(size_);
        std::sort(points_.begin(), end);
        const auto unique_end = std::unique(points_.begin(), end);
        return {points_.data(), static_cast<std::size_t>(unique_end - points_.begin())};
    }

  private:
    void add(float wavelength) { points_[size_++] = wavelength; }
    std::array<float, 6> points_{};
    std::size_t size_ = 0;
};

template <typename Visitor>
bool visit_curve(std::span<const SpectralKnot> source, Visitor visitor) {
    // Equal endpoints mark original knots, which need no interpolation.
    if (!visitor(source.front(), source.front(), source.front().wavelength)) {
        return false;
    }
    for (std::size_t i = 1; i < source.size(); ++i) {
        const auto a = source[i - 1];
        const auto b = source[i];
        const float left = std::max(a.wavelength, 360.f);
        const float right = std::min(b.wavelength, 830.f);
        if (left <= right) {
            SegmentPoints points(left, right);
            points.crossing(a, b, Threshold::zero);
            points.crossing(a, b, Threshold::one);
            for (const auto wavelength : points.sorted()) {
                if (wavelength > a.wavelength && wavelength < b.wavelength &&
                    !visitor(a, b, wavelength)) {
                    return false;
                }
            }
        }
        if (!visitor(b, b, b.wavelength)) {
            return false;
        }
    }
    return true;
}

} // namespace

ReflectanceCurveResult compile_reflectance_curve(std::span<const SpectralKnot> source,
                                                 std::size_t max_knots) {
    float previous = -std::numeric_limits<float>::infinity();
    for (const auto [wavelength, value] : source) {
        if (!(std::isfinite(wavelength) && wavelength > previous && std::isfinite(value))) {
            return {{}, ReflectanceCurveError::invalid_knots};
        }
        previous = wavelength;
    }
    if (source.size() < 2) {
        return {{}, ReflectanceCurveError::invalid_knots};
    }
    if (source.size() > max_knots) {
        return {{}, ReflectanceCurveError::limit_exceeded};
    }
    // Count before allocation, then construct final-sized storage. Geometric
    // vector growth would retain uncharged spare capacity across materials.
    std::size_t count = 0;
    if (!visit_curve(source, [&](SpectralKnot, SpectralKnot, float) {
            if (count == max_knots) {
                return false;
            }
            ++count;
            return true;
        })) {
        return {{}, ReflectanceCurveError::limit_exceeded};
    }
    std::vector<SpectralKnot> knots(count);
    std::size_t index = 0;
    const bool complete =
        visit_curve(source, [&](SpectralKnot a, SpectralKnot b, float wavelength) {
            if (index == count) {
                return false;
            }
            const float value =
                a.wavelength == b.wavelength ? clamp_ordinate(a.value) : evaluate(a, b, wavelength);
            knots[index++] = {wavelength, value};
            return true;
        });
    if (!complete || index != count) {
        return {{}, ReflectanceCurveError::invalid_knots};
    }
    return {std::move(knots), ReflectanceCurveError::none};
}

} // namespace rypath
