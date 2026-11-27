// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "replay_shared.h"
#if defined(__METAL_VERSION__)
#define RYPATH_ATLAS_THREAD thread
#define RYPATH_ATLAS_DATA device
#else
#define RYPATH_ATLAS_THREAD
#define RYPATH_ATLAS_DATA
#endif
// Lin et al. 2026, section 3: immutable self-inverting spatial pairing textures.
// Four independent tiles and deterministic CPU endpoint finding are our choices.
// Physical transport and pairwise MIS weights are supplied by the caller.
namespace rypath_area_pair_atlas {
namespace replay = rypath_replay;
using Word = replay::Word;
using Entry = unsigned short;
#if defined(__METAL_VERSION__)
using SignedWide = long;
#else
using SignedWide = long long;
#endif
enum : Word {
    version = 1,
    tile_count = 4,
    total_entries = 199152,
    sigma_bits = 0x41800000u,
    shuffle_count = 128,
    atlas_seed_low = 0x50414952u,
    atlas_seed_high = 0x41544c31u
};
struct Tile {
    Word first_entry, width;
};
struct Pose {
    Word transform, offset_x, offset_y, reserved;
};
struct Config {
    Word version, tile_count, total_entries, sigma_bits;
    Tile tiles[4];
    Pose poses[4];
};
enum class Status : Word {
    ok,
    invalid_config,
    invalid_coordinate,
    invalid_entry,
    entropy_exhausted
};
struct Offset {
    int x, y;
};
struct Permutation {
    Word index[4];
};
struct FrameResult {
    Config config{};
    Status status = Status::invalid_config;
};
struct Lookup {
    Word pixel = 0;
    Status status = Status::invalid_config;
};
static_assert(sizeof(Entry) == 2 && sizeof(Tile) == 8 && sizeof(Pose) == 16 &&
                  sizeof(Config) == 112 && alignof(Config) == 4,
              "Paired Gaussian atlas host/Metal scalar ABI");
static_assert(__builtin_offsetof(Config, tiles) == 16 && __builtin_offsetof(Config, poses) == 48,
              "Paired atlas descriptor and immutable frame pose offsets");
inline Word tile_width(Word atlas) {
    return atlas == 0 ? 254 : atlas == 1 ? 230 : atlas == 2 ? 210 : atlas == 3 ? 194 : 0;
}
inline bool valid_width(Word width) { return width >= 2 && width <= 254 && (width & 1u) == 0; }
inline bool bounded(Word word, Word bound, RYPATH_ATLAS_THREAD Word &result) {
    if (bound == 0 || word < (Word(0) - bound) % bound)
        return false;
    result = word % bound;
    return true;
}
inline bool permutation(Word rank, RYPATH_ATLAS_THREAD Permutation &result) {
    if (rank >= 24)
        return false;
    Word remaining[4]{0, 1, 2, 3};
    for (Word j = 0; j < 4; ++j) {
        const Word factorial = j == 0 ? 6 : j == 1 ? 2 : 1;
        const Word index = rank / factorial;
        rank %= factorial;
        result.index[j] = remaining[index];
        for (Word k = index; k < 3 - j; ++k)
            remaining[k] = remaining[k + 1];
    }
    return true;
}
inline Entry pack(Offset delta) {
    return Entry((Word(delta.x) & 255u) | ((Word(delta.y) & 255u) << 8));
}
inline Offset decode(Entry entry) {
    const Word x = Word(entry) & 255u, y = Word(entry) >> 8;
    return {x < 128 ? int(x) : int(x) - 256, y < 128 ? int(y) : int(y) - 256};
}
inline bool valid_delta(Offset delta, Word width) {
    const int half_width = int(width / 2);
    return valid_width(width) && (delta.x || delta.y) && delta.x >= -half_width &&
           delta.x <= half_width && delta.y >= -half_width && delta.y <= half_width;
}
inline bool valid_pose(Pose pose, Word width) {
    return valid_width(width) && pose.transform < 8 && pose.offset_x < width &&
           pose.offset_y < width && pose.reserved == 0;
}
inline Config base_config() {
    Config result{};
    result.version = version;
    result.tile_count = tile_count;
    result.total_entries = total_entries;
    result.sigma_bits = sigma_bits;
    Word first = 0;
    for (Word atlas = 0; atlas < tile_count; ++atlas) {
        const Word width = tile_width(atlas);
        result.tiles[atlas] = {first, width};
        first += width * width;
    }
    return result;
}
inline bool valid_config(RYPATH_ATLAS_THREAD const Config &config) {
    if (config.version != version || config.tile_count != tile_count ||
        config.total_entries != total_entries || config.sigma_bits != sigma_bits)
        return false;
    Word first = 0;
    for (Word atlas = 0; atlas < tile_count; ++atlas) {
        const Word width = tile_width(atlas);
        if (config.tiles[atlas].first_entry != first || config.tiles[atlas].width != width ||
            !valid_pose(config.poses[atlas], width))
            return false;
        first += width * width;
    }
    return first == total_entries;
}
inline FrameResult frame_config(Word serial_low, Word serial_high, Word seed_low, Word seed_high) {
    FrameResult result;
    result.config = base_config();
    for (Word atlas = 0; atlas < tile_count; ++atlas) {
        Word selected[3]{};
        for (Word site = 0; site < 3; ++site) {
            const Word bound = site == 0 ? 8u : tile_width(atlas);
            bool accepted = false;
            for (Word retry = 0; retry < (1u << 24); ++retry) {
                const Word word =
                    replay::philox({serial_low, serial_high, atlas, (site << 24) | retry},
                                   {seed_low ^ 0x504f5345u, seed_high ^ 0x41544c31u})
                        .x;
                if (bounded(word, bound, selected[site])) {
                    accepted = true;
                    break;
                }
            }
            if (!accepted) {
                result.status = Status::entropy_exhausted;
                return result;
            }
        }
        result.config.poses[atlas] = {selected[0], selected[1], selected[2], 0};
    }
    result.status = Status::ok;
    return result;
}
inline Word floor_mod(SignedWide value, Word width) {
    const SignedWide remainder = value % SignedWide(width);
    return Word(remainder < 0 ? remainder + width : remainder);
}
// Call only after validating the tile, with -width <= value < 2*width.
// Reduced D4 coordinates plus offsets and reciprocal deltas need one fold.
inline Word fold_tile(int value, Word width) {
    const int period = int(width);
    if (value < 0)
        value += period;
    else if (value >= period)
        value -= period;
    return Word(value);
}
template <typename Coordinate>
inline void transform(Word pose, RYPATH_ATLAS_THREAD Coordinate &x,
                      RYPATH_ATLAS_THREAD Coordinate &y) {
    if (pose & 1u)
        x = -x;
    if (pose & 2u)
        y = -y;
    if (pose & 4u) {
        const auto old_x = x;
        x = y;
        y = old_x;
    }
}
template <typename Coordinate>
inline void inverse_transform(Word pose, RYPATH_ATLAS_THREAD Coordinate &x,
                              RYPATH_ATLAS_THREAD Coordinate &y) {
    if (pose & 4u) {
        const auto old_x = x;
        x = y;
        y = old_x;
    }
    if (pose & 1u)
        x = -x;
    if (pose & 2u)
        y = -y;
}
inline Lookup neighbor_tile(Tile tile, Pose pose, Word entry_count,
                            RYPATH_ATLAS_DATA const Entry *entries, Word pixel, Word width,
                            Word height) {
    Lookup result{pixel, Status::invalid_config};
    if (!valid_pose(pose, tile.width) || tile.first_entry > entry_count ||
        tile.width * tile.width > entry_count - tile.first_entry)
        return result;
    const replay::Wide pixels = replay::Wide(width) * height;
    if (!width || !height || pixels > 0xffffffffu || replay::Wide(pixel) >= pixels) {
        result.status = Status::invalid_coordinate;
        return result;
    }
    const Word px = pixel % width, py = pixel / width;
    // Signed permutations commute with modulo. Reduce BEFORE narrowing or
    // reflecting full uint32 raster coordinates; only final image bounds need
    // signed-wide arithmetic. Valid tiles keep these int temporaries <=506.
    int tx = int(px % tile.width), ty = int(py % tile.width);
    transform(pose.transform, tx, ty);
    const Word x = fold_tile(tx + int(pose.offset_x), tile.width);
    const Word y = fold_tile(ty + int(pose.offset_y), tile.width);
    const Offset delta = decode(entries[tile.first_entry + y * tile.width + x]);
    result.status = Status::invalid_entry;
    if (!valid_delta(delta, tile.width))
        return result;
    // Verify the reciprocal receipt before reading any reverse target. This also
    // catches damaged/injected atlases rather than hiding them as edge fallback.
    const Word qx = fold_tile(int(x) + delta.x, tile.width);
    const Word qy = fold_tile(int(y) + delta.y, tile.width);
    if (entries[tile.first_entry + qy * tile.width + qx] != pack({-delta.x, -delta.y}))
        return result;
    int dx = delta.x, dy = delta.y;
    inverse_transform(pose.transform, dx, dy);
    const SignedWide target_x = SignedWide(px) + dx, target_y = SignedWide(py) + dy;
    result.status = Status::ok;
    if (target_x >= 0 && target_y >= 0 && target_x < SignedWide(width) &&
        target_y < SignedWide(height))
        result.pixel = Word(replay::Wide(target_y) * width + replay::Wide(target_x));
    return result; // Cross-frame-boundary pairs become exact self singletons.
}
inline Lookup neighbor(RYPATH_ATLAS_THREAD const Config &config,
                       RYPATH_ATLAS_DATA const Entry *entries, Word pixel, Word width, Word height,
                       Word candidate) {
    if (!valid_config(config))
        return {pixel, Status::invalid_config};
    if (!width || !height || replay::Wide(width) * height > 0xffffffffu ||
        replay::Wide(pixel) >= replay::Wide(width) * height)
        return {pixel, Status::invalid_coordinate};
    if (candidate == 0)
        return {pixel, Status::ok};
    if (candidate > tile_count)
        return {pixel, Status::invalid_coordinate};
    return neighbor_tile(config.tiles[candidate - 1], config.poses[candidate - 1],
                         config.total_entries, entries, pixel, width, height);
}
} // namespace rypath_area_pair_atlas
#undef RYPATH_ATLAS_THREAD
#undef RYPATH_ATLAS_DATA
