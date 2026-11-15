// SPDX-License-Identifier: GPL-3.0-only

#include "texture_graph.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace {

using namespace rypath;
namespace wire = texture_graph_wire;

void require(bool condition, std::string_view description) {
    if (!condition) {
        throw std::runtime_error(std::string(description));
    }
}

void close(std::optional<float> actual, float expected, std::string_view description,
           float tolerance = 5e-6f) {
    if (!actual || !std::isfinite(*actual) || std::abs(*actual - expected) > tolerance) {
        throw std::runtime_error(std::string(description) + ": expected " +
                                 std::to_string(expected) + ", got " +
                                 (actual ? std::to_string(*actual) : "failure"));
    }
}

wire::Node constant(float value, wire::Word type = wire::scalar) {
    return {.type = type, .values = {value}};
}

constexpr wire::Context context{
    .p = {.31f, -1.7f, 2.4f},
    .n = {0, 0, 1},
    .dpdx = {.061f, -.12f, .09f},
    .dpdy = {-.22f, .031f, .08f},
    .u = -.41f,
    .v = .9f,
    .dudx = .078f,
    .dvdx = .05f,
    .dudy = -.02f,
    .dvdy = .14f,
};

wire::Mapping transformed_mapping(wire::Word kind) {
    return {.kind = kind, .texture_from_render = {2, 0, 0, .1f, 0, .5f, 0, .2f, 0, 0, -1, -.3f}};
}

struct ObservedImage {
    wire::Mapped2D coordinates{};
    int calls = 0;
    bool spectrum = false;
};

std::optional<float> track_image(const void *user, wire::Word image, bool spectrum,
                                 wire::Mapped2D c, float wavelength) {
    auto &observer = *const_cast<ObservedImage *>(static_cast<const ObservedImage *>(user));
    observer.coordinates = c;
    observer.spectrum = spectrum;
    ++observer.calls;
    return image == 3 && wavelength == 550.0f ? 1.25f : std::optional<float>{};
}

void test_mappings() {
    TextureGraph graph{.nodes = {{.kind = wire::image, .payload = {3}}}, .knots = {}};
    auto &node = graph.nodes.front();
    ObservedImage observer;
    const TextureImageSampler sampler{.user = &observer, .sample = track_image};
    std::array mappings{
        wire::Mapping{.kind = wire::uv, .values = {2, -3, .2f, -.4f}},
        transformed_mapping(wire::planar),
        transformed_mapping(wire::spherical),
        transformed_mapping(wire::cylindrical),
    };
    constexpr std::array planar{.3f, -.6f, .9f, -.4f, -.1f, .7f, .23f, -.4f};
    std::ranges::copy(planar, mappings[1].values);
    // From the pinned PBRT UVMapping, PlanarMapping, SphericalMapping and
    // CylindricalMapping, evaluated with the same context and affine transform.
    constexpr std::array references{
        std::array{-.62f, -3.1f, .156f, -.15f, -.04f, -.420000017f},
        std::array{-1.594f, -2.513f, -.00839999318f, -.105800003f, -.21329999f, .118450001f},
        std::array{.89021492f, .883124888f, .0061063813f, -.0102776065f, -.046489682f,
                   .0381875224f},
        std::array{.383124948f, -2.70000005f, .0061063813f, -.0900000036f, -.046489682f,
                   -.0799999982f},
    };
    for (std::size_t i = 0; i < mappings.size(); ++i) {
        node.mapping = mappings[i];
        node.type = i % 2 == 0 ? wire::spectrum : wire::scalar;
        require(validate_texture_graph(graph, 4).empty(), "valid image mapping node");
        close(evaluate_texture_graph(graph, 0, context, 550.0f, sampler), 1.25f, "image sample");
        require(observer.spectrum == (node.type == wire::spectrum),
                "image callback sees output type");
        const auto c = observer.coordinates;
        const std::array actual{c.s, c.t, c.dsdx, c.dtdx, c.dsdy, c.dtdy};
        for (std::size_t j = 0; j < actual.size(); ++j) {
            close(actual[j], references[i][j], "mapped coordinate or derivative");
        }
    }
    node.mapping = {.kind = wire::cylindrical};
    require(!evaluate_texture_graph(graph, 0, {}, 550.0f, sampler) && observer.calls == 4,
            "a cylindrical axis hit fails before accessing the image");
}

void test_coordinate_only_axes() {
    TextureGraph graph{.nodes = {constant(0),
                                 constant(1),
                                 constant(2),
                                 constant(3),
                                 {.kind = wire::bilerp, .children = {0, 1, 2, 3}},
                                 {.kind = wire::dots, .children = {0, 1}},
                                 {.kind = wire::checkerboard, .children = {0, 1}}},
                       .knots = {}};
    for (const auto kind : {wire::cylindrical, wire::spherical}) {
        for (const auto node : {4u, 5u, 6u}) {
            graph.nodes[node].mapping.kind = kind;
        }
        for (const float z : {.25f, -.25f}) {
            const wire::Context axis{.p = {0, 0, z}, .dpdx = {1, 0, 0}, .dpdy = {0, 1, 0}};
            const float s = kind == wire::cylindrical ? .5f : (z > 0 ? 0.f : 1.f);
            const float t = kind == wire::cylindrical ? z : 0.f;
            close(evaluate_texture_graph(graph, 4, axis, 550), 2 * s + t,
                  "bilerp uses the pinned PBRT angular axis coordinates");
            graph.nodes[5].mapping.kind = wire::uv;
            const auto uv = evaluate_texture_graph(graph, 5, {.u = s, .v = t}, 550);
            if (!uv) {
                throw std::runtime_error("dots UV control failed to evaluate");
            }
            graph.nodes[5].mapping.kind = kind;
            close(evaluate_texture_graph(graph, 5, axis, 550), *uv,
                  "dots uses angular coordinates without singular derivatives");
            require(!evaluate_texture_graph(graph, 6, axis, 550),
                    "checkerboard requires defined angular derivatives");
        }
    }
    require(!evaluate_texture_graph(graph, 4, {}, 550) &&
                !evaluate_texture_graph(graph, 5, {}, 550),
            "spherical origin has no defined coordinates");
}

void test_composition() {
    const float infinity = std::numeric_limits<float>::infinity();
    TextureGraph graph{
        .nodes = {constant(.25f, wire::spectrum),
                  constant(.75f, wire::spectrum),
                  constant(2),
                  constant(0),
                  {.kind = wire::image, .type = wire::spectrum, .payload = {3}},
                  {.kind = wire::mix, .type = wire::spectrum, .children = {0, 1, 2}},
                  {.kind = wire::mix, .type = wire::spectrum, .children = {0, 4, 3}},
                  {.kind = wire::scale, .type = wire::spectrum, .children = {4, 3}},
                  {.kind = wire::directionmix,
                   .type = wire::spectrum,
                   .children = {0, 4},
                   .values = {0, 0, -1}},
                  {.kind = wire::checkerboard, .type = wire::spectrum, .children = {0, 4}},
                  {.kind = wire::dots, .type = wire::spectrum, .children = {0, 4}},
                  {.kind = wire::bilerp, .type = wire::spectrum, .children = {0, 1, 4, 1}},
                  {.kind = wire::bilerp, .type = wire::spectrum, .children = {0, 1, 1, 0}},
                  {.kind = wire::knots, .type = wire::spectrum, .payload = {0, 2}},
                  {.kind = wire::scale, .type = wire::spectrum, .children = {13, 2}},
                  {.kind = wire::rgb_sigmoid, .type = wire::spectrum, .values = {0, 0, 1}},
                  {.kind = wire::blackbody, .type = wire::spectrum, .values = {6500}}},
        .knots = {{500, 2}, {600, 6}},
    };
    require(validate_texture_graph(graph, 4).empty(), "typed composition graph");
    const auto sample = [&](wire::Word root, wire::Context c = {}) {
        return evaluate_texture_graph(graph, root, c, 550.0f);
    };
    close(sample(5), 1.25f, "mix extrapolates");
    close(sample(6), .25f, "mix skips unused image");
    close(sample(7), 0.0f, "scale zero skips image");
    close(sample(8, context), .25f, "directionmix uses absolute dot and selects its first branch");
    require(!sample(8), "directionmix evaluates its selected second branch");
    close(sample(9), .25f, "checkerboard even tile skips second image");
    require(!sample(9, {.u = 1}), "checkerboard odd tile evaluates second image");
    close(sample(10), .25f, "dots outside skips inside image");
    require(!sample(10, {.u = -2, .v = -2}), "pinned dots inside selects second branch");
    require(!sample(11), "bilerp evaluates zero-weight corners");
    close(sample(12, {.u = -1}), -.25f, "bilerp extrapolates");
    close(sample(13), 4.0f, "sampled spectral leaf preserves values above one");
    close(sample(14), 8.0f, "scale preserves unclamped sampled spectral input");
    close(sample(15), .5f + .5f / std::sqrt(2.f), "RGB sigmoid spectral leaf");
    close(sample(16), .906177492f, "continuous normalized blackbody spectral leaf", 1e-5f);
    require(!sample(wire::no_texture) && !evaluate_texture_graph(graph, 13, {}, infinity),
            "out-of-range root and non-finite wavelength fail visibly");
}

void test_procedurals() {
    TextureGraph graph{
        .nodes =
            {
                {.kind = wire::fbm, .payload = {8}, .values = {.55f}},
                {.kind = wire::wrinkled, .payload = {8}, .values = {.55f}},
                {.kind = wire::windy},
                {.kind = wire::marble,
                 .type = wire::spectrum,
                 .payload = {8},
                 .values = {.55f, 1.42f, .28f}},
                constant(0),
                constant(1),
                {.kind = wire::checkerboard, .children = {4, 5}},
                {.kind = wire::checkerboard, .children = {4, 5}},
            },
        .knots = {},
    };
    for (const auto i : {0, 1, 2, 3, 7}) {
        graph.nodes[static_cast<std::size_t>(i)].mapping = transformed_mapping(wire::spatial);
    }
    graph.nodes[6].mapping = {.kind = wire::uv, .values = {2, -3, .2f, -.4f}};
    require(validate_texture_graph(graph, 0).empty(), "valid procedural graph");
    auto sharp = context;
    sharp.dpdx = sharp.dpdy = {};
    auto partial = context;
    partial.dpdx = {.061f * .2f, -.12f * .2f, .09f * .2f};
    partial.dpdy = {-.22f * .2f, .031f * .2f, .08f * .2f};
    // Direct outputs of Checkerboard, FBm, Turbulence, WindyTexture and
    // MarbleTexture linked against the pinned PBRT Release/float build.
    close(evaluate_texture_graph(graph, 6, context, 550), .64613229f, "2D filtered checkerboard");
    close(evaluate_texture_graph(graph, 7, context, 550), .165748447f, "3D filtered checkerboard");
    close(evaluate_texture_graph(graph, 0, context, 550), 0.0f, "fully filtered fBm");
    close(evaluate_texture_graph(graph, 1, context, 550), .64072293f, "filtered turbulence bias");
    close(evaluate_texture_graph(graph, 0, sharp, 550), .00457936013f, "zero-footprint fBm");
    close(evaluate_texture_graph(graph, 1, sharp, 550), .687158465f, "zero-footprint turbulence");
    close(evaluate_texture_graph(graph, 0, partial, 550), .00110583194f, "partial octave fBm");
    close(evaluate_texture_graph(graph, 1, partial, 550), .729086399f, "partial octave turbulence");
    close(evaluate_texture_graph(graph, 2, partial, 550), .00488614058f, "windy");
    close(evaluate_texture_graph(graph, 3, partial, 550), .85080111f, "marble at 550 nm", 3e-5f);
    close(evaluate_texture_graph(graph, 3, partial, 450), .874565125f, "marble at 450 nm", 3e-5f);
}

void test_limits_and_emitter_certification() {
    TextureGraph graph{.nodes = {constant(1)}, .knots = {}};
    for (wire::Word i = 1; i < 12; ++i) {
        graph.nodes.push_back({.kind = wire::scale, .children = {i - 1, i - 1}});
    }
    std::vector<wire::Word> work;
    require(validate_texture_graph(graph, 0, &work).empty() && work.back() == 4095,
            "repeated DAG edges count toward the per-evaluation limit");
    graph.nodes.push_back({.kind = wire::dots, .children = {11, 0}});
    require(validate_texture_graph(graph, 0, &work).empty() && work.back() == 4096 &&
                evaluate_texture_graph(graph, 12, {}, 550) == 1,
            "dots counts only its larger branch and can visit exactly 4096 nodes");
    graph.nodes.push_back({.kind = wire::scale, .children = {12, 0}});
    require(!validate_texture_graph(graph, 0, &work).empty() && work.size() == 13 &&
                !evaluate_texture_graph(graph, 13, {}, 550),
            "worst-case visit overflow fails at validation and runtime");
    graph.nodes = {constant(1)};
    for (wire::Word i = 1; i < wire::max_depth; ++i) {
        graph.nodes.push_back({.kind = wire::scale, .children = {i - 1, 0}});
    }
    require(validate_texture_graph(graph, 0).empty() &&
                evaluate_texture_graph(graph, wire::max_depth - 1, {}, 550) == 1,
            "exact supported graph depth evaluates");
    graph.nodes.push_back({.kind = wire::scale, .children = {wire::max_depth - 1, 0}});
    require(!validate_texture_graph(graph, 0).empty() &&
                !evaluate_texture_graph(graph, wire::max_depth, {}, 550),
            "depth overflow is rejected at validation and runtime");
    graph.nodes[0] = {.kind = wire::scale, .children = {1, 1}};
    require(validate_texture_graph(graph, 0).find("cycle") != std::string::npos,
            "an active child identifies a cycle");
    graph.nodes[0].children[1] = wire::no_texture;
    require(validate_texture_graph(graph, 0).find("index") != std::string::npos,
            "invalid child index is diagnosed");
    graph.nodes = {constant(0), constant(1), constant(.25f), {.kind = wire::image}};
    auto pyramid = std::make_shared<ImagePyramid>(ImagePyramid{
        .encoding = ImageEncoding::srgb,
        .wrap = ImageWrap::black,
        .levels = {{2, 1, 0}, {1, 1, 2}},
        .texels = {0xff000000u, 0x004080ffu, 0x00000080u},
    });
    std::array images{ImageTexture{.image = pyramid, .invert = true, .filter = ImageFilter::point}};
    require(certify_binary_emitter_mask(graph, 0, images) &&
                certify_binary_emitter_mask(graph, 1, images) &&
                !certify_binary_emitter_mask(graph, 2, images) &&
                certify_binary_emitter_mask(graph, 3, images),
            "binary constants and scalar point base/channel/wrap pass with inversion");
    images[0].filter = ImageFilter::bilinear;
    require(!certify_binary_emitter_mask(graph, 3, images), "filtered image fails binary proof");
    images[0].filter = ImageFilter::point;
    pyramid->texels[1] = 0x00000080u;
    require(!certify_binary_emitter_mask(graph, 3, images),
            "fractional source channel fails proof");
}

} // namespace

int main() {
    try {
        test_mappings();
        test_coordinate_only_axes();
        test_composition();
        test_procedurals();
        test_limits_and_emitter_certification();
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
