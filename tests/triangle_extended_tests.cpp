// SPDX-License-Identifier: GPL-3.0-only
#include "triangle_extended.h"
#include "triangle_extended_cases.hpp"
#include "triangle_product_guard_cases.hpp"
#include <array>
#include <bit>
#include <cmath>
#include <iostream>
#include <random>
#include <stdexcept>
using namespace triangle_extended;
using rypath::test::triangle_extended_vectors::vectors;
namespace {
void require(bool b, const char *message) {
    if (!b)
        throw std::runtime_error(message);
}
double value(Float a) {
    return std::ldexp((a.negative ? -1. : 1.) * double(a.mantissa), a.exponent);
}
double value(Pair a) { return value(a.high) + value(a.low); }

} // namespace
int main() try {
    for (auto c : vectors) {
        std::array<Float, 4> actual = {add(c.a, c.b), sub(c.a, c.b), mul(c.a, c.b), div(c.a, c.b)};
        for (unsigned i = 0; i < 4; ++i)
            require(same(actual[i], c.expected[i]), "Exact Fraction RN boundary vector");
    }
    for (auto c : rypath::test::product_guard::cases()) {
        auto a = from_bits(c.high_a), b = from_bits(c.high_b);
        double x = rypath::test::product_guard::value(c.high_a),
               y = rypath::test::product_guard::value(c.high_b);
        if (std::isfinite(x) && std::isfinite(y))
            require(value(mul(pair(a), pair(b))) == x * y, "Exact binary32 product retained");
        else
            require(!supported(a) || !supported(b), "Nonfinite domain rejected");
    }
    std::mt19937_64 rng(0xe24c0ffee);
    for (unsigned i = 0; i < 20000; ++i) {
        Float ah{(rng() & 0x7fffff) | (U{1} << 23), int(rng() % 101) - 73, bool(rng() & 1), true};
        Float bh{(rng() & 0x7fffff) | (U{1} << 23), int(rng() % 101) - 73, bool(rng() & 1), true};
        Float al = scale(ah, -24 - int(rng() % 200)), bl = scale(neg(bh), -24 - int(rng() % 200));
        Pair a = add(pair(ah), pair(al)), b = add(pair(bh), pair(bl));
        require(same(mul(a, b), mul(b, a)), "Pair multiplication commutative");
        require(same(mul(neg(a), b), neg(mul(a, b))), "Pair multiplication sign covariant");
        double x = value(ah), y = value(bh);
        require(value(add(ah, bh)) == double(float(x + y)), "Native RN addition");
        require(value(sub(ah, bh)) == double(float(x - y)), "Native RN subtraction");
        require(value(mul(ah, bh)) == double(float(x * y)), "Native RN multiplication");
        require(value(div(ah, bh)) == double(float(x / y)), "Native RN division");
    }
    auto a = from_bits(3196583936u), b = from_bits(2351762040u), p = from_bits(188209952u);
    require(value(product_error(a, b, p)) == -std::ldexp(1., -129), "Captured Area retained error");
    auto zero = pair_from_bits(0), one = pair_from_bits(0x3f800000u);
    auto tiny = pair(scale(from_bits(0x3f800000u), -1000));
    require(compare({one.high, tiny.high}, one).order == 1, "Gapped exact ordering");
    require(compare({one.high, neg(one.high)}, zero).order == 0, "Exact cancellation tie");
    require(compare({one.high, tiny.high}, {tiny.high, one.high}).order == 0,
            "Non-normalized exact tie");
    require(!compare(pair_from_bits(0x7fc00000u), zero).valid, "Invalid comparison visible");
    require(!supported(scale(one.high, 200000)), "Exponent bound visible");
    require(!supported(div(one.high, {})), "Division by zero visible");
    require(to_bits(from_bits(0x80000000u)).bits == 0, "Numerical zero canonicalization");
    require(to_bits(scale(one.high, -150)).bits == 0, "Binary32 half-minimum ties to zero");
    require(to_bits(scale(from_bits(0x3f800001u), -150)).bits == 1,
            "Above half-minimum rounds subnormal");
    require(to_bits(from_bits(0x007fffffu)).bits == 0x007fffffu,
            "Subnormal output conversion exact");
    require(!to_bits(scale(one.high, 128)).finite, "Public overflow visible");
    std::cout
        << std::size(vectors)
        << " exact Fraction RN vectors, 5049 retained inputs, 20000 symmetry/native cases passed\n";
    return 0;
} catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
}
