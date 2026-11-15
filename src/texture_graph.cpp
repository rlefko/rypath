// SPDX-License-Identifier: GPL-3.0-only

#include "texture_graph.hpp"

#include "rgb_spectrum.hpp"
#include "texture_graph.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <ranges>
#include <utility>

namespace rypath {
namespace {

namespace wire = texture_graph_wire;
using wire::Word;

const char *node_error(const TextureGraph &graph, const wire::Node &node, std::size_t image_count) {
    if (node.kind > wire::marble || node.type > wire::spectrum) {
        return "unknown node or value type";
    }
    const auto finite = [](float value) { return std::isfinite(value); };
    if (!std::ranges::all_of(node.values, finite) ||
        !std::ranges::all_of(node.mapping.values, finite) ||
        !std::ranges::all_of(node.mapping.texture_from_render, finite)) {
        return "non-finite node parameters";
    }
    for (Word slot = 0; slot < texture_graph::child_count(node.kind); ++slot) {
        const Word child = node.children[slot];
        if (child >= graph.nodes.size()) {
            return "child index out of range";
        }
        if (graph.nodes[child].type != texture_graph::child_type(node, slot)) {
            return "child type mismatch";
        }
    }

    switch (node.kind) {
    case wire::knots: {
        const auto begin = node.payload[0];
        const auto count = node.payload[1];
        if (begin > graph.knots.size() || count == 0 || count > graph.knots.size() - begin) {
            return "spectral knot range is invalid";
        }
        const auto end = begin + count;
        for (auto i = begin; i < end; ++i) {
            if (!finite(graph.knots[i].wavelength) || !finite(graph.knots[i].value) ||
                (i > begin && graph.knots[i - 1].wavelength >= graph.knots[i].wavelength)) {
                return "spectral knots require finite, strictly increasing wavelengths and finite "
                       "values";
            }
        }
        [[fallthrough]];
    }
    case wire::rgb_sigmoid:
        if (node.type != wire::spectrum) {
            return "spectral leaf requires a spectrum node";
        }
        break;
    case wire::blackbody:
        if (node.type != wire::spectrum || node.values[0] <= 0.0f) {
            return "blackbody requires a spectrum node and a positive temperature";
        }
        break;
    case wire::image:
        if (node.payload[0] >= image_count) {
            return "image index out of range";
        }
        [[fallthrough]];
    case wire::dots:
    case wire::bilerp:
        if (node.mapping.kind > wire::cylindrical) {
            return "node requires a supported 2D mapping";
        }
        break;
    case wire::checkerboard:
        if (node.mapping.kind > wire::spatial) {
            return "unknown checkerboard mapping";
        }
        break;
    case wire::fbm:
    case wire::wrinkled:
    case wire::windy:
    case wire::marble:
        if (node.mapping.kind != wire::spatial) {
            return "procedural node requires a 3D mapping";
        }
        if (node.type != (node.kind == wire::marble ? wire::spectrum : wire::scalar)) {
            return "procedural node type mismatch";
        }
        if (node.kind != wire::windy && node.payload[0] > wire::max_octaves) {
            return "procedural octave limit exceeded";
        }
        break;
    default:
        break;
    }
    return nullptr;
}

struct Cost {
    Word depth = 0; // Zero for unvisited nodes, no_texture while active.
    Word visits = 0;
};

struct ValidationFrame {
    Word index;
    Word next = 0;
};

const char *measure_graph(const TextureGraph &graph, Word root, std::vector<Cost> &costs) {
    if (costs[root].depth != 0) {
        return nullptr;
    }
    std::array<ValidationFrame, wire::max_depth> stack{};
    stack[0] = {root};
    Word size = 1;
    costs[root].depth = wire::no_texture;
    while (size != 0) {
        auto &frame = stack[size - 1];
        const auto &node = graph.nodes[frame.index];
        const auto count = texture_graph::child_count(node.kind);
        if (frame.next < count) {
            const Word child = node.children[frame.next++];
            auto &cost = costs[child];
            if (cost.depth == wire::no_texture) {
                return "cycle";
            }
            if (cost.depth == 0) {
                if (size == stack.size()) {
                    return "depth exceeds 64 nodes";
                }
                stack[size++] = {child};
                cost.depth = wire::no_texture;
            }
            continue;
        }

        Cost result{.depth = 1, .visits = 1};
        for (Word slot = 0; slot < count; ++slot) {
            const auto cost = costs[node.children[slot]];
            result.depth = std::max(result.depth, 1 + cost.depth);
            if (node.kind == wire::dots) {
                result.visits = std::max(result.visits, 1 + cost.visits);
            } else {
                result.visits += cost.visits;
            }
        }
        if (result.depth > wire::max_depth) {
            return "depth exceeds 64 nodes";
        }
        if (result.visits > wire::max_visits) {
            return "evaluation exceeds 4096 node visits in the worst case";
        }
        costs[frame.index] = result;
        --size;
    }
    return nullptr;
}

struct ImageAdapter {
    TextureImageSampler sampler;

    bool operator()(Word image, bool spectrum, wire::Mapped2D coords, float wavelength,
                    float &value) const {
        if (sampler.sample == nullptr) {
            return false;
        }
        const auto sampled = sampler.sample(sampler.user, image, spectrum, coords, wavelength);
        if (!sampled) {
            return false;
        }
        value = *sampled;
        return true;
    }
};

struct RgbAdapter {
    // The shared CPU/Metal evaluator calls this with three RGB components and a wavelength.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    bool operator()(float r, float g, float b, float wavelength, float &value) const {
        const auto reconstructed = reconstruct_srgb({r, g, b});
        if (!reconstructed) {
            return false;
        }
        value = evaluate_srgb(*reconstructed, wavelength);
        return true;
    }
};

} // namespace

std::string validate_texture_graph(const TextureGraph &graph, std::size_t image_count,
                                   std::vector<wire::Word> *work) {
    if (graph.nodes.size() >= wire::no_texture ||
        graph.knots.size() > std::numeric_limits<Word>::max()) {
        return "Texture graph exceeds the packed index range";
    }
    for (Word i = 0; i < graph.nodes.size(); ++i) {
        if (const char *error = node_error(graph, graph.nodes[i], image_count)) {
            return "Texture node " + std::to_string(i) + ": " + error;
        }
    }
    std::vector<Cost> costs(graph.nodes.size());
    for (Word i = 0; i < graph.nodes.size(); ++i) {
        if (const char *error = measure_graph(graph, i, costs)) {
            return "Texture graph rooted at node " + std::to_string(i) + ": " + error;
        }
    }
    if (work != nullptr) {
        std::vector<Word> counts;
        counts.reserve(costs.size());
        for (const auto cost : costs) {
            counts.push_back(cost.visits);
        }
        *work = std::move(counts);
    }
    return {};
}

bool certify_binary_emitter_mask(const TextureGraph &graph, wire::Word root,
                                 std::span<const ImageTexture> images) {
    if (root >= graph.nodes.size()) {
        return false;
    }
    const auto &node = graph.nodes[root];
    if (node.type != wire::scalar) {
        return false;
    }
    if (node.kind == wire::constant_node) {
        return node.values[0] == 0.0f || node.values[0] == 1.0f;
    }
    if (node.kind != wire::image || node.mapping.kind != wire::uv ||
        node.payload[0] >= images.size()) {
        return false;
    }
    const auto &texture = images[node.payload[0]];
    if (!texture.image || texture.filter != ImageFilter::point || texture.scale != 1.0f) {
        return false;
    }
    const auto &image = *texture.image;
    if (image.levels.empty() ||
        (image.storage != ImageStorage::encoded8 && image.storage != ImageStorage::linear_half &&
         image.storage != ImageStorage::linear_float) ||
        (image.wrap != ImageWrap::black && image.wrap != ImageWrap::repeat &&
         image.wrap != ImageWrap::clamp)) {
        return false;
    }
    const auto level = image.levels.front();
    const std::size_t stride = image_storage_words(image.storage);
    if (level.offset > image.texels.size() || level.width == 0 || level.height == 0 ||
        (image.texels.size() - level.offset) / stride / level.width < level.height) {
        return false;
    }

    // Point filtering uses channel zero even for RGBA with varying alpha.
    // Each supported wrap selects a texel or the exact zero black border.
    const std::size_t end = level.offset + stride * std::size_t{level.width} * level.height;
    if (image.storage == ImageStorage::linear_float) {
        for (std::size_t i = level.offset; i < end; i += stride) {
            const auto value = std::bit_cast<float>(image.texels[i]);
            if (value != 0.0f && value != 1.0f)
                return false;
        }
        return true;
    }
    if (image.storage == ImageStorage::linear_half) {
        for (std::size_t i = level.offset; i < end; i += stride) {
            const auto code = image.texels[i] & 0xffffu;
            if (code != 0x0000u && code != 0x8000u && code != 0x3c00u) {
                return false;
            }
        }
        return true;
    }
    if (image.encoding == ImageEncoding::gamma
            ? !image.custom_transfer
            : (image.encoding != ImageEncoding::linear && image.encoding != ImageEncoding::srgb)) {
        return false;
    }
    const auto &transfer =
        image.custom_transfer ? *image.custom_transfer : image_transfer_table(image.encoding);
    for (std::size_t i = level.offset; i < end; ++i) {
        const float value = transfer[image.texels[i] & 0xffu];
        if (value != 0.0f && value != 1.0f) {
            return false;
        }
    }
    return true;
}

std::optional<float> evaluate_texture_graph(const TextureGraph &graph, wire::Word root,
                                            const wire::Context &context, float wavelength,
                                            TextureImageSampler images) {
    if (graph.nodes.size() >= wire::no_texture ||
        graph.knots.size() > std::numeric_limits<Word>::max()) {
        return std::nullopt;
    }
    float value = 0;
    const auto node_count = static_cast<Word>(graph.nodes.size());
    const auto knot_count = static_cast<Word>(graph.knots.size());
    if (!texture_graph::evaluate(graph.nodes.data(), node_count, graph.knots.data(), knot_count,
                                 root, context, wavelength, ImageAdapter{images}, RgbAdapter{},
                                 value)) {
        return std::nullopt;
    }
    return value;
}

} // namespace rypath
