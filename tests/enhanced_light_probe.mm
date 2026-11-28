// SPDX-License-Identifier: GPL-3.0-only
#include "enhanced_wire.h"
#include "metal_execution.hpp"
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace rypath;
namespace ew = enhanced_wire;
using Word = std::uint32_t;
using Query = std::array<Word, 32>;
using Four = std::array<float, 4>;
constexpr unsigned stride = 16, guard = 256;
static_assert(sizeof(Query) == 128 && sizeof(ew::Record) == 48);
Word bits(float x) { return std::bit_cast<Word>(x); }
float fp(Word x) { return std::bit_cast<float>(x); }
void need(bool condition, const std::string &message) {
    if (!condition)
        throw std::runtime_error(message);
}
struct Case {
    Query query{};
    ew::Record record = ew::empty_record();
    std::string label;
    bool valid = true, support = true, exact = false;
    long double geometry = 1;
    std::array<float, 4> physical{};
    render_wire::Parameters parameters{};
    render_wire::TextureGraphParameters graph{};
    std::vector<Word> data{0};
    std::vector<float> cdf{0};
};
std::vector<Case> cases() {
    std::vector<Case> result;
    Case layout;
    layout.query[0] = 3;
    layout.label = "wire layout";
    result.push_back(layout);
    const auto record = [&](ew::Record value, bool valid, std::string label, unsigned event = 3) {
        Case c;
        c.record = value;
        c.query[1] = event;
        c.valid = valid;
        c.label = std::move(label);
        result.push_back(c);
    };
    auto v = ew::empty_record();
    record(v, true, "generic canonical sentinel");
    v.version = 0;
    record(v, false, "stale version");
    v = ew::empty_record();
    v.light_id = 0;
    record(v, false, "generic stale light");
    v = {ew::record_version,
         ew::triangle_area,
         0,
         3,
         0,
         0,
         render_wire::triangle_primitive,
         0,
         {bits(.25f), bits(.25f), 0, 0}};
    record(v, true, "triangle interior");
    for (const auto &uv : {std::array<float, 2>{0, 0}, {1, 0}, {0, 1}, {.5f, 0x1.000004p-1f}}) {
        auto edge = v;
        edge.coordinate_bits[0] = bits(uv[0]);
        edge.coordinate_bits[1] = bits(uv[1]);
        record(edge, true, "triangle valid boundary/one-ulp sum slab");
    }
    for (const auto &uv : {std::array<float, 2>{-1, .25f},
                           {-0x1p-149f, .25f},
                           {.25f, -0x1p-149f},
                           {2, 2},
                           {.5f, 0x1.000008p-1f},
                           {NAN, .25f},
                           {.25f, INFINITY}}) {
        auto bad = v;
        bad.coordinate_bits[0] = bits(uv[0]);
        bad.coordinate_bits[1] = bits(uv[1]);
        record(bad, false, "triangle invalid coordinates");
    }
    auto bad = v;
    bad.instance = ew::invalid;
    record(bad, false, "missing instance");
    bad = v;
    bad.primitive = ew::invalid;
    record(bad, false, "missing primitive");
    bad = v;
    bad.kind = render_wire::patch_primitive;
    record(bad, false, "triangle wrong primitive kind");
    bad = v;
    bad.coordinate_bits[2] = 1;
    record(bad, false, "triangle poisoned unused coordinate");
    record(v, false, "wrong terminal event", 4);
    for (unsigned tag : {ew::positional_mass, ew::distant_mass, ew::environment_omega}) {
        v = {ew::record_version, tag, 2, 3, ew::invalid, ew::invalid, ew::invalid, 0, {0, 0, 0, 0}};
        if (tag == ew::environment_omega)
            v.coordinate_bits[2] = bits(1);
        record(v, true, "mass/environment canonical record");
        bad = v;
        bad.coordinate_bits[3] = 1;
        record(bad, false, "poisoned reserved coordinate");
        bad = v;
        bad.instance = 0;
        record(bad, false, "nonarea stale geometry ID");
    }
    v = {ew::record_version, ew::environment_omega, 2, 3, ew::invalid, ew::invalid, ew::invalid, 0,
         {0, 0, bits(1), 0}};
    for (float z : {0.f, 1.0001f, INFINITY, NAN}) {
        bad = v;
        bad.coordinate_bits[2] = bits(z);
        record(bad, false, "invalid environment direction");
    }
    v.coordinate_bits[2] = bits(0x1.000002p0f);
    record(v, true, "near-unit environment bits retained");
    bad = v;
    bad.tag = 5;
    record(bad, false, "unknown tag");
    bad = v;
    bad.reserved = 1;
    record(bad, false, "poisoned reserved word");
    const auto geometry = [&](unsigned tag, std::array<float, 3> to, std::array<float, 3> normal,
                              bool valid, bool support, std::string label,
                              std::array<float, 3> from = {}) {
        Case c;
        c.query[0] = 1;
        c.query[1] = tag;
        c.valid = valid;
        c.support = support;
        c.label = std::move(label);
        for (unsigned k = 0; k < 3; ++k) {
            c.query[2 + k] = bits(from[k]);
            c.query[5 + k] = bits(to[k]);
            c.query[8 + k] = bits(normal[k]);
        }
        if (valid && support && (tag == ew::positional_mass || tag == ew::triangle_area)) {
            long double squared = 0, dot = 0;
            for (unsigned k = 0; k < 3; ++k) {
                const long double delta = static_cast<long double>(to[k]) - from[k];
                squared += delta * delta;
                dot += static_cast<long double>(normal[k]) * delta;
            }
            c.geometry = 1 / squared;
            if (tag == ew::triangle_area)
                c.geometry *= std::abs(dot) / std::sqrt(squared);
        }
        result.push_back(c);
    };
    geometry(ew::positional_mass, {0, 0, 2}, {0, 0, 1}, true, true, "point inverse square");
    geometry(ew::positional_mass, {10, 20, 32}, {0, 0, 1}, true, true,
             "translated receiver inverse square", {10, 20, 30});
    geometry(ew::triangle_area, {0, 0, 2}, {0, 0, 1}, true, true, "area one light cosine");
    geometry(ew::triangle_area, {0, 0, 2}, {0, 0, -1}, true, true, "two-sided geometric cosine");
    geometry(ew::triangle_area, {3, 4, 0}, {0, 1, 0}, true, true, "oblique area geometry");
    geometry(ew::positional_mass, {3, 4, 0}, {0, 1, 0}, true, true, "oblique point geometry");
    geometry(ew::triangle_area, {0, 0, 1}, {1, 0, 0}, true, false, "exact grazing support zero");
    geometry(ew::triangle_area, {0, 0, 1}, {1, 0, 0x1p-20f}, true, true, "near grazing");
    for (unsigned tag : {ew::positional_mass, ew::triangle_area}) {
        geometry(tag, {0, 0, 0}, {0, 0, 1}, true, false, "exact coincident support zero");
        geometry(tag, {0, 0, 0x1p-80f}, {0, 0, 1}, false, false, "nonzero norm underflow fails");
        geometry(tag, {0, 0, 0x1p-60f}, {0, 0, 1}, true, true, "small admitted norm");
        geometry(tag, {0, 0, 0x1p60f}, {0, 0, 1}, true, true, "large admitted norm");
        geometry(tag, {0, 0, 0x1p100f}, {0, 0, 1}, false, false, "norm overflow fails");
        geometry(tag, {0, 0, NAN}, {0, 0, 1}, false, false, "nonfinite position fails");
    }
    geometry(ew::triangle_area, {0, 0, 2}, {0, NAN, 1}, false, false, "nonfinite normal fails");
    geometry(ew::environment_omega, {0, 0, 1000}, {0, 0, -1}, true, true, "environment has unit G");
    geometry(ew::distant_mass, {0, 0, 1000}, {0, 0, -1}, true, true, "distant has unit G");
    const auto physical = [&](Four beta, Four f, Four radiance, float cosine, int geometry_power,
                              bool valid, bool exact, std::string label) {
        Case c;
        c.query[0] = 2;
        c.valid = valid;
        c.exact = exact;
        c.label = std::move(label);
        for (unsigned k = 0; k < 4; ++k) {
            c.query[2 + k] = bits(beta[k]);
            c.query[6 + k] = bits(f[k]);
            c.query[10 + k] = bits(radiance[k]);
            if (valid)
                c.physical[k] =
                    static_cast<float>(static_cast<long double>(beta[k]) * f[k] * radiance[k] *
                                       cosine * std::ldexp(1.L, geometry_power));
        }
        c.query[14] = bits(cosine);
        c.query[15] = bits(.5f);
        c.query[16] = std::bit_cast<Word>(geometry_power + 1);
        result.push_back(c);
    };
    constexpr Four one{1, 1, 1, 1};
    physical({1, 2, 4, 8}, {.5f, .25f, .125f, .0625f}, {1, 2, 4, 8}, .5f, -2, true, true,
             "four distinct lanes and receiver cosine once");
    physical({0, 1, 2, 3}, {1, 0, 2, 3}, {1, 2, 0, 3}, 1, 0, true, true,
             "independent zero factors");
    physical(one, {.7f, .3f, .2f, .9f}, {.8f, .6f, .4f, .2f}, .37f, -3, true, false,
             "non-power-of-two product");
    physical(one, one, one, 0, 0, true, true, "zero receiver cosine");
    physical({0x1p100f, 0x1p100f, 0x1p100f, 0x1p100f}, {0x1p100f, 0x1p100f, 0x1p100f, 0x1p100f},
             {0x1p-100f, 0x1p-100f, 0x1p-100f, 0x1p-100f}, 1, -100, true, true,
             "scaled HDR intermediate rescue");
    const Four tiny{fp(1), fp(2), fp(4), fp(8)};
    physical(tiny, one, one, 1, 120, true, true, "subnormal inputs rescale to normal");
    const Four low{0x1p-120f, 0x1p-120f, 0x1p-120f, 0x1p-120f};
    physical(low, one, one, 1, -20, true, true, "exact subnormal outputs");
    const Four low125{0x1p-125f, 0x1p-125f, 0x1p-125f, 0x1p-125f};
    physical(low125, one, {0x1p-25f, 0x1.8p-24f, 0x1p-24f, 0x1.4p-23f}, 1, 0, true, true,
             "subnormal rounding ties");
    physical({0x1p100f, 0x1p100f, 0x1p100f, 0x1p100f}, one, one, 0x1p-100f, 0, true, true,
             "grazing factor before materialization");
    physical(one, one, one, fp(1), 120, true, true, "subnormal receiver cosine rescued");
    physical({0x1p120f, 0x1p120f, 0x1p120f, 0x1p120f}, {0x1p120f, 0x1p120f, 0x1p120f, 0x1p120f},
             one, 1, 0, false, false, "final overflow fails");
    for (float invalid : {-1.f, INFINITY, NAN}) {
        physical({invalid, 1, 1, 1}, one, one, 1, 0, false, false, "invalid beta fails");
        physical(one, {1, invalid, 1, 1}, one, 1, 0, false, false, "invalid BSDF fails");
        physical(one, one, {1, 1, invalid, 1}, 1, 0, false, false, "invalid radiance fails");
        physical(one, one, one, invalid, 0, false, false, "invalid cosine fails");
    }
    return result;
}
std::vector<Case> density_cases() {
    namespace w = render_wire;
    std::vector<Case> result;
    const auto selection = [](std::string label) {
        Case c;
        c.query[0] = 4;
        c.query[4] = bits(1); // Context at (0,0,1), normal0.
        c.label = std::move(label);
        return c;
    };
    const auto pack = []<typename T>(Case &c, const std::vector<T> &values, Word &offset) {
        offset = Word(c.data.size());
        const auto size = c.data.size();
        c.data.resize(size + values.size() * sizeof(T) / sizeof(Word));
        std::memcpy(c.data.data() + size, values.data(), values.size() * sizeof(T));
    };
    auto uniform = selection("uniform marginal probability");
    uniform.parameters.light_sampler = 2;
    uniform.parameters.light_count = 3;
    uniform.geometry = 1.L / 3;
    result.push_back(uniform);
    uniform.query[1] = 2;
    result.push_back(uniform);
    uniform.query[1] = 3;
    uniform.valid = false;
    uniform.label = "uniform out-of-range ID fails";
    result.push_back(uniform);
    auto alias = selection("alias aggregate PMF, not route threshold");
    alias.parameters.light_sampler = 1;
    alias.parameters.light_count = 3;
    const std::vector<w::LightAlias> aliases{
        {.375f, .125f, 2, 0}, {1, .375f, 1, 0}, {.875f, .5f, 1, 0}};
    pack(alias, aliases, alias.parameters.light_alias_begin);
    for (unsigned id = 0; id < 3; ++id) {
        alias.query[1] = id;
        alias.geometry = 0;
        for (unsigned route = 0; route < aliases.size(); ++route) {
            const long double threshold = aliases[route].threshold;
            if (route == id)
                alias.geometry += threshold / 3;
            if (aliases[route].alias == id)
                alias.geometry += (1 - threshold) / 3;
        }
        result.push_back(alias);
    }
    for (float value : {0.f, -1.f, fp(0x80000001u), INFINITY, NAN}) {
        auto c = alias;
        c.query[1] = 0;
        c.data[c.parameters.light_alias_begin + 1] = bits(value);
        c.valid = value == 0;
        c.support = false;
        c.label = value == 0 ? "alias true zero support" : "alias invalid PMF fails";
        result.push_back(c);
    }
    const auto bounds = [](float power, Word parent = ew::invalid) {
        // Point bounds at0, direction+Z, cosOuter1/cosEmission-1 and ctx+Z:
        // exact importance is power/1, independent of production importance().
        w::LightBounds b{};
        b.direction.z = 1;
        b.power = power;
        b.cos_outer = 1;
        b.cos_emission = -1;
        b.parent = parent;
        b.light = ew::invalid;
        return b;
    };
    const auto tree_case = [&](std::vector<w::LightBounds> nodes,
                               std::vector<w::LightRecord> records, unsigned infinite,
                               std::string label) {
        auto c = selection(std::move(label));
        c.parameters.light_count = Word(records.size());
        c.parameters.light_node_count = Word(nodes.size());
        c.parameters.infinite_light_count = infinite;
        pack(c, records, c.parameters.light_records_begin);
        pack(c, nodes, c.parameters.light_nodes_begin);
        return c;
    };
    std::vector<w::LightBounds> tree{bounds(4), bounds(1, 0), bounds(3, 0)};
    tree[0].child = 2;
    tree[1].light = 0;
    tree[2].light = 1;
    const std::vector<w::LightRecord> leaves{{1, 0, 0, 0}, {2, 0, 0, 0}};
    for (unsigned infinite : {0u, 1u}) {
        auto records = leaves;
        if (infinite)
            records.push_back({ew::invalid, 1, 0, 0});
        for (unsigned id = 0; id < records.size(); ++id) {
            auto c = tree_case(tree, records, infinite, "unequal BVH branches/infinite mixture");
            c.query[1] = id;
            c.geometry = id == 2 ? .5L : (id == 0 ? .25L : .75L) / (infinite + 1.L);
            result.push_back(c);
        }
    }
    auto zero_tree = tree;
    zero_tree[1].power = 0;
    auto c = tree_case(zero_tree, leaves, 0, "represented zero leaf importance is proposal zero");
    c.support = false;
    result.push_back(c);
    auto bad_leaf = leaves;
    bad_leaf[0].leaf = Word(tree.size());
    c = tree_case(tree, bad_leaf, 0, "invalid BVH leaf fails");
    c.valid = false;
    result.push_back(c);
    auto cycle = tree;
    cycle[0].parent = 0;
    c = tree_case(cycle, leaves, 0, "BVH parent cycle fails bounded walk");
    c.valid = false;
    result.push_back(c);
    auto huge = tree;
    huge[1].power = INFINITY;
    c = tree_case(huge, leaves, 0, "nonfinite BVH importance fails");
    c.valid = false;
    result.push_back(c);
    auto underflow = tree;
    underflow[1].power = 0x1p-126f;
    underflow[2].power = 0x1p120f;
    c = tree_case(underflow, leaves, 0, "positive BVH division underflow fails, not zero support");
    c.valid = false;
    result.push_back(c);
    constexpr unsigned levels = 80;
    std::vector<w::LightBounds> chain(2 * levels + 1);
    for (unsigned level = 0; level < levels; ++level) {
        chain[level] = bounds(1, level ? level - 1 : ew::invalid);
        chain[level].child = levels + 1 + level;
        chain[levels + 1 + level] = bounds(3, level);
    }
    chain[levels] = bounds(1, levels - 1);
    chain[levels].light = 0;
    c = tree_case(chain, {{levels, 0, 0, 0}}, 0,
                  "80 positive BVH factors retain scaled 2^-160 PMF");
    c.geometry = std::ldexp(1.L, -160);
    result.push_back(c);

    const std::vector<float> cdf{0, .25f, 1, 0, .125f, 1, 0, .75f,  1,
                                 0, .5f,  1, 0, .25f,  1, 0, .875f, 1};
    const auto environment = [&](bool compensated, unsigned column, unsigned row,
                                 std::array<float, 3> direction, std::string label) {
        Case e;
        e.query[0] = 5;
        e.query[1] = unsigned(compensated);
        for (unsigned k = 0; k < 3; ++k)
            e.query[2 + k] = bits(direction[k]);
        e.label = std::move(label);
        e.graph.environment_width = 2;
        e.graph.environment_cdf_count = Word(cdf.size());
        e.graph.environment_compensated_offset = 9;
        e.graph.environment_from_world[0] = e.graph.environment_from_world[4] =
            e.graph.environment_from_world[8] = 1;
        e.cdf = cdf;
        const unsigned offset = compensated ? 9 : 0;
        const long double marginal =
            static_cast<long double>(cdf[offset + row + 1]) - cdf[offset + row];
        const auto index = offset + (row + 1) * 3 + column;
        const long double conditional = static_cast<long double>(cdf[index + 1]) - cdf[index];
        const long double importance = 4 * marginal * conditional;
        e.geometry = (compensated ? importance : 1.L / 16 + 15.L / 16 * importance) /
                     (4 * std::numbers::pi_v<long double>);
        return e;
    };
    for (bool compensated : {false, true})
        for (unsigned row = 0; row < 2; ++row)
            for (unsigned column = 0; column < 2; ++column)
                result.push_back(environment(compensated, column, row,
                                             {column ? 1.f : -1.f, row ? 1.f : -1.f, 0},
                                             "unequal 2x2 environment CDF and mixture"));
    c = environment(true, 0, 1, {1, 1, 0}, "rotated environment known quadrant");
    c.graph.environment_from_world[0] = 0;
    c.graph.environment_from_world[1] = -1;
    c.graph.environment_from_world[3] = 1;
    c.graph.environment_from_world[4] = 0;
    result.push_back(c);
    for (const auto &seam :
         {std::array<float, 5>{1, 0, 0, 1, 1}, {-1, 0, 0, 0, 1}, {0, 1, 0, 1, 1}, {0, -1, 0, 1, 0}})
        result.push_back(environment(true, unsigned(seam[3]), unsigned(seam[4]),
                                     {seam[0], seam[1], seam[2]},
                                     "analytic environment axis seam"));
    for (bool compensated : {false, true}) {
        c = environment(compensated, 0, 0, {-1, -1, 0}, "zero CDF support versus uniform mixture");
        const auto offset = compensated ? 9u : 0u;
        c.cdf[offset + 1] = 0;
        c.support = !compensated;
        c.geometry = 1.L / (64 * std::numbers::pi_v<long double>);
        result.push_back(c);
        c = environment(compensated, 0, 0, {-1, -1, 0},
                        "positive subnormal CDF mass product retained");
        c.cdf[offset + 1] = fp(1);
        c.cdf[offset + 4] = fp(1);
        const long double product = std::ldexp(4.L, -298);
        c.geometry = (compensated ? product : 1.L / 16 + 15.L / 16 * product) /
                     (4 * std::numbers::pi_v<long double>);
        result.push_back(c);
    }
    for (unsigned fault = 0; fault < 5; ++fault) {
        c = environment(true, 0, 0, {-1, -1, 0}, "invalid environment fixture fails");
        c.valid = false;
        if (fault == 0)
            c.graph.environment_width = 0;
        else if (fault == 1)
            c.graph.environment_cdf_count = 17;
        else if (fault == 2)
            c.query[2] = bits(NAN);
        else if (fault == 3)
            c.cdf[10] = -.5f;
        else
            c.cdf[13] = INFINITY;
        result.push_back(c);
    }
    c = environment(true, 0, 0, {-1, -1, 0}, "negative subnormal CDF mass fails bitwise");
    c.cdf[10] = fp(0x80000001u);
    c.valid = false;
    result.push_back(c);
    for (unsigned factor = 0; factor < 4; ++factor) {
        Case invalid;
        invalid.query[0] = 2;
        invalid.valid = false;
        invalid.label = "negative minimum-subnormal physical factor fails bitwise";
        for (unsigned word = 2; word <= 14; ++word)
            invalid.query[word] = bits(1);
        invalid.query[factor == 3 ? 14 : 2 + 4 * factor] = 0x80000001u;
        invalid.query[15] = bits(.5f);
        invalid.query[16] = 1;
        result.push_back(invalid);
    }
    return result;
}
int run(const std::filesystem::path &path) {
    auto device = MTLCreateSystemDefaultDevice();
#ifdef RYPATH_ALLOW_NO_METAL_DEVICE
    if (!device)
        return 77;
#endif
    need(device != nil, "No Metal device for Enhanced light math probe");
    std::string error;
    auto library = metal::load_library(device, path, error);
    need(library != nil, error);
    auto pipeline = metal::load_pipeline(device, library, @"rypath_enhanced_light_math", error);
    need(pipeline != nil, error);
    auto queue = [device newCommandQueue];
    need(queue != nil, "Enhanced light probe queue");
    auto fixtures = cases();
    need(fixtures.size() == 83, "Original 81 light fixtures plus two negative-subnormal UV cases");
    auto extra = density_cases();
    fixtures.insert(fixtures.end(), extra.begin(), extra.end());
    std::vector<Query> queries;
    std::vector<ew::Record> records;
    std::vector<render_wire::Parameters> parameters;
    std::vector<render_wire::TextureGraphParameters> graphs;
    std::vector<Word> words;
    std::vector<float> cdfs;
    for (const auto &c : fixtures) {
        auto query = c.query;
        query[17] = Word(words.size());
        query[18] = Word(cdfs.size());
        queries.push_back(query);
        records.push_back(c.record);
        parameters.push_back(c.parameters);
        graphs.push_back(c.graph);
        words.insert(words.end(), c.data.begin(), c.data.end());
        cdfs.insert(cdfs.end(), c.cdf.begin(), c.cdf.end());
    }
    const unsigned count = unsigned(fixtures.size());
    auto inputs = [device newBufferWithBytes:queries.data()
                                      length:queries.size() * sizeof(Query)
                                     options:MTLResourceStorageModeShared];
    auto record_buffer = [device newBufferWithBytes:records.data()
                                             length:records.size() * sizeof(ew::Record)
                                            options:MTLResourceStorageModeShared];
    const std::size_t output_bytes = std::size_t(count + 1) * stride * sizeof(Word);
    auto output = [device newBufferWithLength:output_bytes + 2 * guard
                                      options:MTLResourceStorageModeShared];
    auto parameter_buffer = [device newBufferWithBytes:parameters.data()
                                                length:parameters.size() * sizeof(parameters[0])
                                               options:MTLResourceStorageModeShared];
    auto graph_buffer = [device newBufferWithBytes:graphs.data()
                                            length:graphs.size() * sizeof(graphs[0])
                                           options:MTLResourceStorageModeShared];
    auto data_buffer = [device newBufferWithBytes:words.data()
                                           length:words.size() * sizeof(Word)
                                          options:MTLResourceStorageModeShared];
    auto cdf_buffer = [device newBufferWithBytes:cdfs.data()
                                          length:cdfs.size() * sizeof(float)
                                         options:MTLResourceStorageModeShared];
    need(inputs && record_buffer && output && parameter_buffer && graph_buffer && data_buffer &&
             cdf_buffer,
         "Enhanced light probe buffers");
    std::memset(output.contents, 0xa5, output.length);
    auto command = [queue commandBuffer];
    auto encoder = [command computeCommandEncoder];
    need(command && encoder, "Enhanced light probe encoder");
    [encoder setComputePipelineState:pipeline];
    [encoder setBuffer:inputs offset:0 atIndex:0];
    [encoder setBuffer:record_buffer offset:0 atIndex:1];
    [encoder setBuffer:output offset:guard atIndex:2];
    [encoder setBytes:&count length:sizeof(count) atIndex:3];
    [encoder setBuffer:parameter_buffer offset:0 atIndex:4];
    [encoder setBuffer:data_buffer offset:0 atIndex:5];
    [encoder setBuffer:graph_buffer offset:0 atIndex:6];
    [encoder setBuffer:cdf_buffer offset:0 atIndex:7];
    [encoder dispatchThreads:MTLSizeMake(count + 1, 1, 1)
        threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
    [encoder endEncoding];
    error = metal::complete(command, "Enhanced light pure helper probe");
    need(error.empty(), error);
    const auto *bytes = static_cast<const unsigned char *>(output.contents);
    const auto *out = reinterpret_cast<const Word *>(bytes + guard);
    std::array<unsigned, 6> covered{};
    for (unsigned n = 0; n < count; ++n) {
        const auto &c = fixtures[n];
        const auto *v = out + n * stride;
        const std::string label = "Enhanced light case " + std::to_string(n) + " " + c.label;
        ++covered[c.query[0]];
        if (c.query[0] == 3) {
            need(v[0] == 0x454c5031u && v[1] == 48 && v[2] == 4 && v[3] == 32 && v[4] == 16,
                 label + " wire layout");
            continue;
        }
        need(v[0] == unsigned(c.valid), label + " validity");
        if (!c.valid)
            continue;
        if (c.query[0] == 1 || c.query[0] == 4 || c.query[0] == 5) {
            need(v[1] == unsigned(c.support), label + " support");
            if (c.support) {
                need(std::isfinite(fp(v[2])) && fp(v[2]) >= .5f && fp(v[2]) < 1,
                     label + " normalized scaled value");
                const long double actual = std::ldexp(static_cast<long double>(fp(v[2])),
                                                      std::bit_cast<std::int32_t>(v[3]));
                // Allow rounded dot/reciprocal-square-root/scaled quotient operations;
                // the expected cosine and inverse square are computed independently.
                const long double tolerance =
                    16.L * std::numeric_limits<float>::epsilon() * std::abs(c.geometry);
                need(std::isfinite(actual) && std::abs(actual - c.geometry) <= tolerance,
                     label + " independent geometry/proposal density");
            }
        } else if (c.query[0] == 2) {
            for (unsigned lane = 0; lane < 4; ++lane) {
                if (c.exact)
                    need(v[2 + lane] == bits(c.physical[lane]),
                         label + " exact lane " + std::to_string(lane));
                else {
                    const double actual = fp(v[2 + lane]), expected = c.physical[lane];
                    // Five factor operations plus final materialization; exact binary
                    // fixtures above retain bit equality, including underflow ties.
                    const double tolerance =
                        12 * double(std::numeric_limits<float>::epsilon()) * std::abs(expected);
                    need(std::isfinite(actual) && std::abs(actual - expected) <= tolerance,
                         label + " independent product lane " + std::to_string(lane));
                }
            }
        }
    }
    need(covered == std::array<unsigned, 6>{36, 23, 27, 1, 22, 23},
         "Enhanced light fixture inventory");
    for (unsigned k = 0; k < guard; ++k)
        need(bytes[k] == 0xa5 && bytes[guard + output_bytes + k] == 0xa5,
             "Enhanced light output outer guard");
    for (unsigned k = 0; k < stride; ++k)
        need(out[count * stride + k] == 0xa5a5a5a5u, "Enhanced light padded invocation guard");
    need(std::memcmp(inputs.contents, queries.data(), queries.size() * sizeof(Query)) == 0 &&
             std::memcmp(record_buffer.contents, records.data(),
                         records.size() * sizeof(ew::Record)) == 0,
         "Enhanced light inputs/owned coordinate bits unchanged");
    need(std::memcmp(parameter_buffer.contents, parameters.data(), parameter_buffer.length) == 0 &&
             std::memcmp(graph_buffer.contents, graphs.data(), graph_buffer.length) == 0 &&
             std::memcmp(data_buffer.contents, words.data(), data_buffer.length) == 0 &&
             std::memcmp(cdf_buffer.contents, cdfs.data(), cdf_buffer.length) == 0,
         "Enhanced light proposal inputs unchanged");
    std::cout
        << count << " Enhanced light pure helper cases: records=" << covered[0]
        << " geometry=" << covered[1] << " physical=" << covered[2]
        << " selection_density=" << covered[4] << " environment_density=" << covered[5]
        << "; no Resources aggregate, AS, BSDF, light sampling-frequency or visibility coverage\n";
    return 0;
}
} // namespace
int main(int argc, char **argv) {
    @autoreleasepool {
        try {
            need(argc == 2, "Expected Enhanced light probe metallib");
            return run(argv[1]);
        } catch (const std::exception &error) {
            std::cerr << error.what() << '\n';
            return 1;
        }
    }
}
