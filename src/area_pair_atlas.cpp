// SPDX-License-Identifier: GPL-3.0-only
#include "area_pair_atlas.hpp"
#include <array>
#include <exception>
#include <limits>

namespace rypath::area_pair_atlas {
namespace {
using Word = shared::Word;
bool shape(std::size_t count, Word width, std::string &diagnostic) {
    diagnostic.clear();
    if (!shared::valid_width(width) || count != std::size_t(width) * width) {
        diagnostic = "Paired atlas requires an even width 2..254 and exact square span";
        return false;
    }
    return true;
}
bool rank(Word atlas, Word pass, Word block, shared::replay::Words2 seed, Word &result) {
    for (Word retry = 0;; ++retry) {
        const Word word = shared::replay::philox({block, pass, atlas, retry},
                                                 {seed.x ^ 0x32425348u, seed.y ^ 0x5546464cu})
                              .x;
        if (shared::bounded(word, 24, result))
            return true;
        if (retry == std::numeric_limits<Word>::max())
            return false;
    }
}
int shortest(int delta, Word width) {
    if (delta > int(width / 2))
        delta -= int(width);
    if (delta < -int(width / 2))
        delta += int(width);
    return delta;
}
} // namespace
bool initialize_labels(std::span<Label> labels, Word width, std::string &diagnostic) {
    if (!shape(labels.size(), width, diagnostic))
        return false;
    for (Word pixel = 0; pixel < labels.size(); ++pixel)
        labels[pixel] = Label(pixel >> 1);
    return true;
}
bool shuffle_labels(std::span<Label> labels, Word width, shared::replay::Words2 seed, Word atlas,
                    Word pass, std::string &diagnostic) {
    if (!shape(labels.size(), width, diagnostic))
        return false;
    const Word blocks = width / 2, parity = pass & 1u;
    for (Word by = 0; by < blocks; ++by)
        for (Word bx = 0; bx < blocks; ++bx) {
            const Word x0 = (2 * bx + parity) % width, x1 = (x0 + 1) % width;
            const Word y0 = (2 * by + parity) % width, y1 = (y0 + 1) % width;
            const Word positions[4]{y0 * width + x0, y0 * width + x1, y1 * width + x0,
                                    y1 * width + x1};
            Label old[4];
            for (Word j = 0; j < 4; ++j) {
                old[j] = labels[positions[j]];
                if (old[j] >= width * width / 2) {
                    diagnostic = "Paired atlas shuffle found an out-of-range label";
                    return false;
                }
            }
            Word selected;
            shared::Permutation permutation{};
            if (!rank(atlas, pass, by * blocks + bx, seed, selected) ||
                !shared::permutation(selected, permutation)) {
                diagnostic = "Paired atlas permutation entropy exhausted";
                return false;
            }
            for (Word j = 0; j < 4; ++j)
                labels[positions[j]] = old[permutation.index[j]];
        }
    return true;
}
bool link_labels(std::span<const Label> labels, Word width, std::span<shared::Entry> entries,
                 std::string &diagnostic) {
    if (!shape(labels.size(), width, diagnostic) || !shape(entries.size(), width, diagnostic))
        return false;
    constexpr Word absent = std::numeric_limits<Word>::max();
    std::vector<std::array<Word, 2>> endpoints(labels.size() / 2, {absent, absent});
    // Defined serial first/second finding. Unlike the paper's CUDA shortcut,
    // no data race or winning parallel write determines endpoint ownership.
    for (Word pixel = 0; pixel < labels.size(); ++pixel) {
        const Word label = labels[pixel];
        if (label >= endpoints.size()) {
            diagnostic = "Paired atlas link found an out-of-range label";
            return false;
        }
        auto &pair = endpoints[label];
        if (pair[0] == absent)
            pair[0] = pixel;
        else if (pair[1] == absent)
            pair[1] = pixel;
        else {
            diagnostic = "Paired atlas link label occurs more than twice";
            return false;
        }
    }
    for (const auto &pair : endpoints) {
        if (pair[0] == absent || pair[1] == absent || pair[0] == pair[1]) {
            diagnostic = "Paired atlas link label has no distinct second endpoint";
            return false;
        }
        const auto delta =
            shared::Offset{shortest(int(pair[1] % width) - int(pair[0] % width), width),
                           shortest(int(pair[1] / width) - int(pair[0] / width), width)};
        if (!shared::valid_delta(delta, width)) {
            diagnostic = "Paired atlas delta is not representable";
            return false;
        }
        entries[pair[0]] = shared::pack(delta);
        entries[pair[1]] = shared::pack({-delta.x, -delta.y});
    }
    return true;
}
bool validate_tile(std::span<const shared::Entry> entries, Word width, std::string &diagnostic) {
    if (!shape(entries.size(), width, diagnostic))
        return false;
    for (Word pixel = 0; pixel < entries.size(); ++pixel) {
        const auto delta = shared::decode(entries[pixel]);
        if (!shared::valid_delta(delta, width)) {
            diagnostic = "Paired atlas has a zero or out-of-range delta";
            return false;
        }
        const Word x = shared::floor_mod(shared::SignedWide(pixel % width) + delta.x, width);
        const Word y = shared::floor_mod(shared::SignedWide(pixel / width) + delta.y, width);
        const Word partner = y * width + x;
        if (partner == pixel || entries[partner] != shared::pack({-delta.x, -delta.y})) {
            diagnostic = "Paired atlas lacks an exact reciprocal endpoint";
            return false;
        }
    }
    return true;
}
Asset generate() {
    Asset asset;
    try {
        asset.entries.resize(shared::total_entries);
        const auto config = shared::base_config();
        for (Word atlas = 0; atlas < shared::tile_count; ++atlas) {
            const auto tile = config.tiles[atlas];
            std::vector<Label> labels(std::size_t(tile.width) * tile.width);
            if (!initialize_labels(labels, tile.width, asset.diagnostic)) {
                asset.entries.clear();
                return asset;
            }
            for (Word pass = 0; pass < shared::shuffle_count; ++pass)
                if (!shuffle_labels(labels, tile.width,
                                    {shared::atlas_seed_low, shared::atlas_seed_high}, atlas, pass,
                                    asset.diagnostic)) {
                    asset.entries.clear();
                    return asset;
                }
            const auto entries = std::span(asset.entries).subspan(tile.first_entry, labels.size());
            if (!link_labels(labels, tile.width, entries, asset.diagnostic) ||
                !validate_tile(entries, tile.width, asset.diagnostic)) {
                asset.entries.clear();
                return asset;
            }
        }
        std::vector<std::uint8_t> bytes;
        bytes.reserve(asset.entries.size() * sizeof(shared::Entry));
        for (auto entry : asset.entries) {
            bytes.push_back(std::uint8_t(entry & 255u));
            bytes.push_back(std::uint8_t(entry >> 8));
        }
        asset.digest = sha256(bytes);
    } catch (const std::exception &error) {
        asset.entries.clear();
        asset.diagnostic = std::string("Paired atlas generation failed: ") + error.what();
    }
    return asset;
}
} // namespace rypath::area_pair_atlas
