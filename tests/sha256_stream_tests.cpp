// SPDX-License-Identifier: GPL-3.0-only
#include "sha256.hpp"
#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

int main() {
    try {
        // Independent Python hashlib SHA-256 fixtures. Data byte i is
        // (37*i + i/7) modulo256; lengths straddle padding/block/buffer boundaries.
        constexpr std::array<std::pair<std::size_t, std::string_view>, 14> cases{{
            {0, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
            {1, "6e340b9cffb37a989ca544e6bb780a2c78901d3fb33738768511a30617afa01d"},
            {55, "55b2b17d3d21f257ac42ecf3d96a4a1bbc512146a5f96cab130987e7fb7e9980"},
            {56, "aaf3fec3b4a16353384970ae903f7c4d1ef33d3dec22d56c8a4d77ae3b63278c"},
            {63, "6fefff3c6e0ed3b0b18b71fa42a643f02a62e2efaf099b94ab3e97a93fa65f44"},
            {64, "fad6596f7bd91c41537eccdc9f00c6513911007ac6a1db638761346e41ecc0a4"},
            {65, "f0cd0f9fa753bbb22d5071c2a5f2aa24504acf460ecb70e78314d03112bdf764"},
            {127, "3f61b1a79ee27e49a450c30ee635544d3738e7da1a8dd966fe66fdf80a53c896"},
            {128, "812179180224381e3f7b1102058a73c4d5ae0be663c7bf21f79208db16f3eec4"},
            {129, "36b239f552f6adc61b40787da101babd06600511b9d744316c1a3745e1843049"},
            {4095, "731d647f2d360687239f85b95a3006fdb02ad5dc631246d999676d45d350556d"},
            {4096, "42508ced58c0134ec99dec8f82a5c5cc586010039f6c007d5287f4c7166e72b2"},
            {4097, "a252c9a94608330254bd0d98408610266409ebded3457397cc096ebaf477c91c"},
            {65536, "0da02bd8f589aa0c35c025c04f302ca4856ec832e7de09e71f8cdfe224e35e8f"},
        }};
        for (const auto &[length, expected] : cases) {
            for (std::size_t offset : {0u, 1u, 3u, 15u}) {
                std::vector<std::uint8_t> storage(length + offset);
                for (std::size_t i = 0; i < length; ++i)
                    storage[offset + i] = static_cast<std::uint8_t>((37 * i + i / 7) % 256);
                const auto data = std::span<const std::uint8_t>(storage).subspan(offset);
                if (rypath::digest_hex(rypath::sha256(data)) != expected)
                    throw std::runtime_error("unaligned one-shot SHA-256 fixture");
                for (std::size_t chunk : {1u, 7u, 55u, 63u, 64u, 65u, 8192u}) {
                    rypath::Sha256 hash;
                    auto remaining = data;
                    while (!remaining.empty()) {
                        hash.update({});
                        const auto count = std::min(chunk, remaining.size());
                        hash.update(remaining.first(count));
                        remaining = remaining.subspan(count);
                    }
                    const auto result = hash.finish();
                    if (rypath::digest_hex(result) != expected || hash.finish() != result)
                        throw std::runtime_error("segmented SHA-256 fixture/repeated finish");
                    bool rejected = false;
                    try {
                        hash.update({});
                    } catch (const std::logic_error &) {
                        rejected = true;
                    }
                    if (!rejected)
                        throw std::runtime_error("SHA-256 accepted update after finish");
                }
                for (std::size_t prefix : {0u, 63u, 64u, 65u}) {
                    if (prefix > data.size())
                        continue;
                    rypath::Sha256 original;
                    original.update(data.first(prefix));
                    auto copy = original;
                    original.update(data.subspan(prefix));
                    const std::array<std::uint8_t, 3> suffix{255, 0, 17};
                    copy.update(suffix);
                    std::vector<std::uint8_t> forked(
                        data.begin(), data.begin() + static_cast<std::ptrdiff_t>(prefix));
                    forked.insert(forked.end(), suffix.begin(), suffix.end());
                    if (rypath::digest_hex(original.finish()) != expected ||
                        copy.finish() != rypath::sha256(forked))
                        throw std::runtime_error("SHA-256 copied streaming state");
                }
            }
        }
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
