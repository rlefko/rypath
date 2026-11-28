// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "area_pair_atlas_shared.h"
#include "sha256.hpp"
#include <cstdint>
#include <span>
#include <string>
#include <vector>
namespace rypath::area_pair_atlas {
namespace shared = rypath_area_pair_atlas;
using Label = std::uint16_t;
struct Asset {
    std::vector<shared::Entry> entries;
    Sha256Digest digest{};
    std::string diagnostic;
    explicit operator bool() const {
        return diagnostic.empty() && entries.size() == shared::total_entries;
    }
};
bool initialize_labels(std::span<Label> labels, shared::Word width, std::string &diagnostic);
bool shuffle_labels(std::span<Label> labels, shared::Word width, shared::replay::Words2 seed,
                    shared::Word atlas, shared::Word pass, std::string &diagnostic);
bool link_labels(std::span<const Label> labels, shared::Word width,
                 std::span<shared::Entry> entries, std::string &diagnostic);
bool validate_tile(std::span<const shared::Entry> entries, shared::Word width,
                   std::string &diagnostic);
// Sigma16 / Eq3 FLOOR ->128 passes; integer-only shuffles and endpoint finding.
// Digest encodes each packed Entry low-byte then high-byte, with no padding.
Asset generate();
} // namespace rypath::area_pair_atlas
