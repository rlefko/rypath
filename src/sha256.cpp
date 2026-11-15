// SPDX-License-Identifier: GPL-3.0-only
#include "sha256.hpp"
#include <algorithm>
#include <bit>
#include <limits>
#include <stdexcept>
// SHA-256 construction and constants: NIST FIPS 180-4, sections 4.2.2 and 6.2.
// https://nvlpubs.nist.gov/nistpubs/FIPS/NIST.FIPS.180-4.pdf
namespace rypath {
namespace {
constexpr std::array<std::uint32_t, 64> constants{
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
}
void Sha256::block(const std::uint8_t *bytes) {
    std::array<std::uint32_t, 64> w{};
    for (std::size_t i = 0; i < 16; ++i)
        for (std::size_t j = 0; j < 4; ++j)
            w[i] = (w[i] << 8) | bytes[4 * i + j];
    for (std::size_t i = 16; i < 64; ++i) {
        const auto x = w[i - 15], y = w[i - 2];
        w[i] = w[i - 16] + (std::rotr(x, 7) ^ std::rotr(x, 18) ^ (x >> 3)) + w[i - 7] +
               (std::rotr(y, 17) ^ std::rotr(y, 19) ^ (y >> 10));
    }
    auto a = state_[0], b = state_[1], c = state_[2], d = state_[3];
    auto e = state_[4], f = state_[5], g = state_[6], h = state_[7];
    for (std::size_t i = 0; i < 64; ++i) {
        const auto t1 = h + (std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25)) +
                        ((e & f) ^ (~e & g)) + constants[i] + w[i];
        const auto t2 =
            (std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
}
void Sha256::update(std::span<const std::uint8_t> bytes) {
    if (finished_)
        throw std::logic_error("SHA256 update after finish");
    if (bytes.size() > std::numeric_limits<std::uint64_t>::max() / 8 - size_)
        throw std::length_error("SHA256 bit length overflow");
    size_ += bytes.size();
    while (!bytes.empty()) {
        const auto n = std::min(bytes.size(), buffer_.size() - used_);
        std::copy_n(bytes.begin(), n, buffer_.begin() + static_cast<std::ptrdiff_t>(used_));
        used_ += n;
        bytes = bytes.subspan(n);
        if (used_ == 64) {
            block(buffer_.data());
            used_ = 0;
        }
    }
}
Sha256Digest Sha256::finish() {
    if (finished_)
        return digest_;
    buffer_[used_++] = 0x80;
    if (used_ > 56) {
        std::fill(buffer_.begin() + static_cast<std::ptrdiff_t>(used_), buffer_.end(), 0);
        block(buffer_.data());
        used_ = 0;
    }
    std::fill(buffer_.begin() + static_cast<std::ptrdiff_t>(used_), buffer_.begin() + 56, 0);
    const auto bits = size_ * 8;
    for (std::size_t i = 0; i < 8; ++i)
        buffer_[63 - i] = static_cast<std::uint8_t>(bits >> (8 * i));
    block(buffer_.data());
    for (std::size_t i = 0; i < 32; ++i)
        digest_[i] = static_cast<std::uint8_t>(state_[i / 4] >> (24 - 8 * (i % 4)));
    finished_ = true;
    return digest_;
}
Sha256Digest sha256(std::span<const std::uint8_t> bytes) {
    Sha256 hash;
    hash.update(bytes);
    return hash.finish();
}
std::string digest_hex(const Sha256Digest &digest) {
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(64);
    for (auto byte : digest) {
        result += hex[byte >> 4];
        result += hex[byte & 15];
    }
    return result;
}
} // namespace rypath
