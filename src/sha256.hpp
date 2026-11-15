// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <array>
#include <cstdint>
#include <span>
#include <string>
namespace rypath {
using Sha256Digest = std::array<std::uint8_t, 32>;
// FIPS 180-4 SHA-256. finish() is repeatable; update() after finish() is invalid.
class Sha256 {
  public:
    void update(std::span<const std::uint8_t> bytes);
    Sha256Digest finish();

  private:
    void block(const std::uint8_t *bytes);
    std::array<std::uint32_t, 8> state_{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::array<std::uint8_t, 64> buffer_{};
    std::uint64_t size_ = 0;
    std::size_t used_ = 0;
    bool finished_ = false;
    Sha256Digest digest_{};
};
Sha256Digest sha256(std::span<const std::uint8_t> bytes);
std::string digest_hex(const Sha256Digest &digest);
} // namespace rypath
