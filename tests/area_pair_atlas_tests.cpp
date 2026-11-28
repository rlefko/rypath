// SPDX-License-Identifier: GPL-3.0-only
#include "area_pair_atlas.hpp"
#include "area_pair_atlas_oracle.hpp"
#include <array>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <set>
#include <vector>
namespace {
namespace shared = rypath_area_pair_atlas;
namespace cpu = rypath::area_pair_atlas;
namespace ref = atlas_oracle;
void need(bool value, const char *why) { ref::require(value, why); }
std::vector<std::uint8_t> bytes(const std::vector<shared::Entry> &entries) {
    std::vector<std::uint8_t> result;
    for (auto e : entries) {
        result.push_back(e & 255);
        result.push_back(e >> 8);
    }
    return result;
}
void rng_and_construction() {
    const auto expected = ref::permutations();
    std::set<std::array<unsigned, 4>> distinct;
    for (unsigned rank = 0; rank < 24; ++rank) {
        shared::Permutation p{};
        need(shared::permutation(rank, p), "Every24 permutation rank admitted");
        std::array<unsigned, 4> actual{};
        for (unsigned j = 0; j < 4; ++j)
            actual[j] = p.index[j];
        need(actual == expected[rank], "Independent lexicographic old[permutation[j]] convention");
        distinct.insert(actual);
    }
    need(distinct.size() == 24, "All24 permutations are distinct bijections");
    shared::Permutation unused{};
    need(!shared::permutation(24, unused), "Invalid permutation rank rejected");
    unsigned result = 99;
    for (unsigned word = 0; word < 16; ++word)
        need(!shared::bounded(word, 24, result), "Uniform24 rejects0..15");
    std::set<unsigned> residues;
    for (unsigned word = 16; word < 40; ++word) {
        need(shared::bounded(word, 24, result) && result == word % 24,
             "Accepted boundary word modulo24");
        residues.insert(result);
    }
    need(residues.size() == 24 && ((1ull << 32) - 16) % 24 == 0,
         "Accepted32bit range contains each residue equally often");
    need(shared::bounded(0xffffffffu, 24, result) && result == 15 && !shared::bounded(5, 0, result),
         "Bounded max word and invalid bound");
    const std::array<unsigned, 3> script{0, 15, 40};
    unsigned retries = 0, scripted = 0;
    for (const auto word : script) {
        ++retries;
        if (shared::bounded(word, 24, scripted))
            break;
    }
    need(retries == 3 && scripted == 16,
         "Scripted bounded draws reject two words before accepting the third");
    const auto reference = ref::philox({0, 0, 0, 0}, {0, 0});
    need(reference == std::array<unsigned, 4>{0x6627e8d5u, 0xe169c58du, 0xbc57ac4cu, 0x9b00dbd8u},
         "Published Philox4x32-10 zero golden");
    const auto actual = rypath_replay::philox({0, 0, 0, 0}, {0, 0});
    need(actual.x == reference[0] && actual.y == reference[1] && actual.z == reference[2] &&
             actual.w == reference[3],
         "ProductionPhilox golden");
    need(shared::shuffle_count == 128 && 32921161 / 256000 == 128,
         "Sigma16 full rational FLOOR is128, not129");
    for (unsigned width : {2u, 4u, 6u, 8u}) {
        std::vector<cpu::Label> labels(std::size_t(width) * width);
        std::vector<shared::Entry> linked(labels.size());
        std::string why;
        need(cpu::initialize_labels(labels, width, why), why.c_str());
        for (unsigned p = 0; p < labels.size(); ++p)
            need(labels[p] == p / 2, "Initial adjacent horizontal labels appear twice");
        for (unsigned pass = 0; pass < 8; ++pass) {
            need(cpu::shuffle_labels(labels, width,
                                     {shared::atlas_seed_low, shared::atlas_seed_high}, 2, pass,
                                     why),
                 why.c_str());
            const auto independent = ref::labels(width, 2, pass + 1);
            for (unsigned p = 0; p < labels.size(); ++p)
                need(labels[p] == independent[p],
                     "Every pass agrees with independent disjoint wrapped-block oracle");
            need(cpu::link_labels(labels, width, linked, why), why.c_str());
            need(linked == ref::endpoints(independent, width),
                 "Independent endpoint-table linkage");
        }
        need(cpu::initialize_labels(labels, width, why), why.c_str());
        labels[2] = labels[0];
        need(!cpu::link_labels(labels, width, linked, why), "Third/missing endpoint rejected");
    }
    // Exact half-period signs must be opposite, including zero on the other axis.
    std::vector<cpu::Label> ties(36);
    for (unsigned y = 0; y < 6; ++y)
        for (unsigned x = 0; x < 6; ++x)
            ties[y * 6 + x] = cpu::Label(y * 3 + x % 3);
    std::vector<shared::Entry> tied(36);
    std::string why;
    need(cpu::link_labels(ties, 6, tied, why), why.c_str());
    for (unsigned y = 0; y < 6; ++y)
        for (unsigned x = 0; x < 3; ++x)
            need(tied[y * 6 + x] == ref::pack(3, 0) && tied[y * 6 + x + 3] == ref::pack(-3, 0),
                 "Half-period tie preserves exact negative reverse sign");
    tied[3] = tied[0];
    need(!cpu::validate_tile(tied, 6, why), "One-sided half-period sign convention is rejected");
}
void lookup(const std::vector<shared::Entry> &tile, unsigned L) {
    std::string why;
    need(cpu::validate_tile(tile, L, why), why.c_str());
    // Every packed entry, partner and lifted signed delta is checked independently.
    for (unsigned p = 0; p < tile.size(); ++p) {
        const auto d = ref::decode(tile[p]);
        need((d[0] || d[1]) && std::abs(d[0]) <= int(L / 2) && std::abs(d[1]) <= int(L / 2),
             "Packed signed range and no atlas self-link");
        const unsigned q =
            unsigned(ref::modulo(int(p / L) + d[1], L)) * L + ref::modulo(int(p % L) + d[0], L);
        const auto back = ref::decode(tile[q]);
        need(q != p && back[0] == -d[0] && back[1] == -d[1],
             "Exact antisymmetry at every tile endpoint");
    }
    for (unsigned orientation = 0; orientation < 8; ++orientation)
        for (unsigned offset : {0u, 1u, L / 2, L - 1}) {
            shared::Pose pose{orientation, offset, (offset + L - 1) % L, 0};
            // One complete tile plus a halo: every entry is visited for every
            // orientation/adversarial offset without redundant huge images.
            const unsigned width = L + 3, height = L + 1;
            for (unsigned p = 0; p < width * height; ++p) {
                const auto expected = ref::neighbor(tile, L, width, height, p, orientation,
                                                    pose.offset_x, pose.offset_y);
                const auto value = shared::neighbor_tile({0, L}, pose, unsigned(tile.size()),
                                                         tile.data(), p, width, height);
                need(value.status == shared::Status::ok && value.pixel == expected,
                     "Independent transformed endpoint lookup");
                const auto back = shared::neighbor_tile({0, L}, pose, unsigned(tile.size()),
                                                        tile.data(), value.pixel, width, height);
                need(back.status == shared::Status::ok && back.pixel == p,
                     "Finite-image integer involution");
            }
        }
}
// Handwritten half-period pairs exercise exact +/-ties on either tile axis.
// These fixtures do not change the generated production asset or its digest.
std::vector<shared::Entry> half_period_tile(unsigned L, bool vertical) {
    std::vector<shared::Entry> tile(std::size_t(L) * L);
    for (unsigned y = 0; y < L; ++y)
        for (unsigned x = 0; x < L; ++x) {
            const int delta = (vertical ? y : x) < L / 2 ? int(L / 2) : -int(L / 2);
            tile[y * L + x] = vertical ? ref::pack(0, delta) : ref::pack(delta, 0);
        }
    return tile;
}
void wide_lookup(const std::vector<shared::Entry> &tile, unsigned L) {
    std::string why;
    need(cpu::validate_tile(tile, L, why), why.c_str());
    for (const auto dims :
         {std::array<unsigned, 2>{0xffffffffu, 1}, {1, 0xffffffffu}, {65535, 65537}})
        for (unsigned orientation = 0; orientation < 8; ++orientation)
            for (const auto offsets :
                 {std::array<unsigned, 2>{0, 0}, {L - 1, L - 1}, {L / 2, 1}, {1, L / 2}})
                for (unsigned pixel : {0u, 1u, L - 1, L, L + 1, 0x7ffffffeu, 0x7fffffffu,
                                       0x80000000u, 0x80000001u, 0xfffffffdu, 0xfffffffeu}) {
                    const shared::Pose pose{orientation, offsets[0], offsets[1], 0};
                    const auto expected = ref::neighbor(tile, L, dims[0], dims[1], pixel,
                                                        orientation, offsets[0], offsets[1]);
                    const auto value = shared::neighbor_tile({0, L}, pose, unsigned(tile.size()),
                                                             tile.data(), pixel, dims[0], dims[1]);
                    need(value.status == shared::Status::ok && value.pixel == expected,
                         "Production-width extreme raster matches unchanged signed-wide oracle");
                    const auto back =
                        shared::neighbor_tile({0, L}, pose, unsigned(tile.size()), tile.data(),
                                              value.pixel, dims[0], dims[1]);
                    need(back.status == shared::Status::ok && back.pixel == pixel,
                         "Extreme raster seam/half-period pair reverses exactly");
                }
}
void narrow_lookup_controls() {
    // Compare the bounded fold to independent generic signed-wide remainder,
    // including its full admitted interval, without executing atlas generation.
    for (unsigned L = 2; L <= 254; L += 2)
        for (int value = -int(L); value < 2 * int(L); ++value)
            need(shared::fold_tile(value, L) == unsigned(ref::modulo(value, L)),
                 "One-fold normalization equals independent floor modulo");
    for (unsigned L : {254u, 230u, 210u, 194u}) {
        const auto horizontal = half_period_tile(L, false);
        wide_lookup(horizontal, L);
        wide_lookup(half_period_tile(L, true), L);
        constexpr unsigned pixel = 0x80000000u;
        const auto expected = ref::neighbor(horizontal, L, 0xffffffffu, 1, pixel, 0, 0, 0);
        // Model the WRONG signed32 cast before reduction without implementation-
        // defined conversion or negation of INT_MIN. L4 would hide this residue
        // error because it divides2^32; every production width distinguishes it.
        const long long wrong_signed = static_cast<long long>(pixel) - (1ll << 32);
        const auto wrong_delta = ref::decode(horizontal[unsigned(ref::modulo(wrong_signed, L))]);
        const auto wrong_neighbor = unsigned(static_cast<long long>(pixel) + wrong_delta[0]);
        need(expected != wrong_neighbor,
             "Non-power-of-two half-period witness rejects signed cast before reduction");
        const auto actual = shared::neighbor_tile({0, L}, {0, 0, 0, 0}, L * L, horizontal.data(),
                                                  pixel, 0xffffffffu, 1);
        need(actual.status == shared::Status::ok && actual.pixel == expected &&
                 actual.pixel != wrong_neighbor,
             "Production lookup distinguishes the wrong signed-cast neighbor");
    }
}
void finite_and_poses() {
    const auto tile = ref::generate(4, 0, 8);
    for (unsigned orientation = 0; orientation < 8; ++orientation)
        for (unsigned ox = 0; ox < 4; ++ox)
            for (unsigned oy = 0; oy < 4; ++oy)
                for (auto dims : {std::array{1u, 1u}, std::array{3u, 5u}, std::array{7u, 3u},
                                  std::array{19u, 1u}}) {
                    const unsigned N = dims[0] * dims[1];
                    const auto explicit_pairs =
                        ref::lifted_pairs(tile, 4, dims[0], dims[1], orientation, ox, oy);
                    std::vector<unsigned> swapped(N), twice(N);
                    for (unsigned p = 0; p < N; ++p) {
                        auto v = shared::neighbor_tile({0, 4}, {orientation, ox, oy, 0}, 16,
                                                       tile.data(), p, dims[0], dims[1]);
                        need(v.status == shared::Status::ok &&
                                 v.pixel == ref::neighbor(tile, 4, dims[0], dims[1], p, orientation,
                                                          ox, oy),
                             "Small exact transformed finite image oracle");
                        need(v.pixel == explicit_pairs[p],
                             "Independent transformed two-endpoint finite pairing construction");
                        swapped[p] = v.pixel;
                    }
                    for (unsigned p = 0; p < N; ++p) {
                        twice[p] = swapped[swapped[p]];
                        need(twice[p] == p, "Two-swap image recovery including edge singleton");
                    }
                }
    for (auto serial :
         {std::array<unsigned, 2>{0, 0}, {17, 0}, {0, 1}, {0xffffffffu, 0xffffffffu}}) {
        const auto value = shared::frame_config(serial[0], serial[1], 37, 41);
        need(value.status == shared::Status::ok && shared::valid_config(value.config),
             "Full64bit serial yields valid frozen poses");
        for (unsigned k = 0; k < 4; ++k)
            for (unsigned site = 0; site < 3; ++site) {
                const unsigned bound = site == 0 ? 8 : shared::tile_width(k);
                unsigned expected = 0;
                bool accepted = false;
                const auto threshold = (1ull << 32) % bound;
                for (unsigned retry = 0; retry < 0x1000000u; ++retry) {
                    const auto word = ref::philox({serial[0], serial[1], k, (site << 24) | retry},
                                                  {37u ^ 0x504f5345u, 41u ^ 0x41544c31u})[0];
                    if (word >= threshold) {
                        expected = word % bound;
                        accepted = true;
                        break;
                    }
                }
                const auto p = value.config.poses[k];
                const unsigned actual = site == 0   ? p.transform
                                        : site == 1 ? p.offset_x
                                                    : p.offset_y;
                need(accepted && actual == expected,
                     "Pose has independent Philox key/counter/site/serial and rejection oracle");
            }
    }
    for (const auto dims : {std::array<unsigned, 2>{0xffffffffu, 1}, {1, 0xffffffffu}})
        for (unsigned transform = 0; transform < 8; ++transform) {
            const unsigned p = 0xfffffffeu;
            const auto value = shared::neighbor_tile({0, 4}, {transform, 3, 1, 0}, 16, tile.data(),
                                                     p, dims[0], dims[1]);
            need(value.status == shared::Status::ok &&
                     value.pixel == ref::neighbor(tile, 4, dims[0], dims[1], p, transform, 3, 1),
                 "Signed-wide transform and endpoint addition preserve extreme raster coordinates");
        }
    auto bad = shared::base_config();
    bad.poses[0].reserved = 1;
    need(!shared::valid_config(bad), "Malformed pose rejected");
    bad = shared::base_config();
    bad.tiles[1].first_entry++;
    need(!shared::valid_config(bad), "Malformed atlas span rejected");
    auto malformed = tile;
    malformed[0] = 0;
    need(shared::neighbor_tile({0, 4}, {0, 0, 0, 0}, 16, malformed.data(), 0, 4, 4).status ==
             shared::Status::invalid_entry,
         "Zero malformed entry cannot become boundary fallback");
}
void production_asset() {
    auto actual = cpu::generate();
    need(bool(actual), actual.diagnostic.c_str());
    std::vector<shared::Entry> expected;
    for (unsigned k = 0; k < 4; ++k) {
        const unsigned L = shared::tile_width(k);
        const auto tile = ref::generate(L, k);
        expected.insert(expected.end(), tile.begin(), tile.end());
        lookup(tile, L);
        wide_lookup(tile, L);
        long double sx = 0, sy = 0, xx = 0, yy = 0, xy = 0, radius = 0;
        std::array<unsigned, 5> histogram{};
        for (auto entry : tile) {
            const auto d = ref::decode(entry);
            const long double x = d[0], y = d[1], r = std::sqrt(x * x + y * y);
            sx += x;
            sy += y;
            xx += x * x;
            yy += y * y;
            xy += x * y;
            radius += r;
            ++histogram[r < 16 ? 0 : r < 32 ? 1 : r < 48 ? 2 : r < 64 ? 3 : 4];
        }
        const long double N = static_cast<long double>(tile.size()), rx = std::sqrt(xx / N),
                          ry = std::sqrt(yy / N), radial = radius / N;
        const long double radial_expected = 16 * std::sqrt(std::acos(-1.L) / 2);
        std::cout << "atlas_distribution tile=" << k << " mean=" << double(sx / N) << ','
                  << double(sy / N) << " RMS=" << double(rx) << ',' << double(ry)
                  << " covariance=" << double(xy / std::sqrt(xx * yy))
                  << " radial=" << double(radial) << " histogram=";
        for (auto count : histogram)
            std::cout << count << ',';
        std::cout << '\n';
        need(sx == 0 && sy == 0, "Antisymmetric atlas signed means exactly zero");
        need(std::abs(rx - 16) <= 1.6L && std::abs(ry - 16) <= 1.6L &&
                 std::abs(radial - radial_expected) <= .1L * radial_expected &&
                 std::abs(xy / std::sqrt(xx * yy)) <= .1L,
             "Predeclared fixed-asset10percent RMS/radial and.1 covariance screen");
    }
    need(actual.entries == expected && expected.size() == 199152,
         "All production atlas bytes equal independent integer construction");
    const auto expected_bytes = bytes(expected);
    const auto digest = rypath::sha256(expected_bytes);
    need(actual.digest == digest,
         "Production hash is independently serialized lowbyte/highbyte atlas order");
    need(rypath::digest_hex(digest) ==
             "6d877656f1338b07f72e2a917fb5f491cfb1952a5a476fba981b027b3e2605db",
         "Version-one production atlas has the frozen independently verified byte hash");
    std::cout << "atlas_v1_reference_sha256=" << rypath::digest_hex(digest) << '\n';
    const auto small = ref::generate(4, 0, 8);
    const std::vector<shared::Entry> small_golden{0x0201, 0x0101, 0x0101, 0x02ff, 0x0202, 0x0202,
                                                  0xffff, 0xffff, 0x0100, 0xfeff, 0xfe01, 0x01fe,
                                                  0xff00, 0xff02, 0xfefe, 0xfefe};
    need(small == small_golden, "Eight-pass four-square independent literal endpoint bytes");
    std::cout << "atlas_small_reference=";
    for (auto e : small)
        std::cout << std::hex << e << ',';
    std::cout << std::dec << '\n';
}
} // namespace
int main() {
    try {
        for (unsigned operation = 0; operation < 3; ++operation) {
            bool rejected = false;
            try {
                if (operation == 0)
                    (void)ref::labels(65536, 0, 0);
                else if (operation == 1)
                    (void)ref::lifted_pairs({}, 2, 65536, 65536, 0, 0, 0);
                else
                    (void)ref::handwritten(65536, 0);
            } catch (const std::runtime_error &) {
                rejected = true;
            }
            need(rejected, "Oracle rejects extent beyond 32-bit IDs before allocation");
        }
        rng_and_construction();
        finite_and_poses();
        narrow_lookup_controls();
        production_asset();
        std::cout << "Paired atlas independent integer, lookup and distribution controls passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
