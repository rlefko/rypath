// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "image_texture.hpp"
#include "texture_graph_wire.h"

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace rypath {

struct TextureGraph {
    std::vector<texture_graph_wire::Node> nodes;
    std::vector<render_wire::SpectralKnot> knots;
};

struct TextureImageSampler {
    const void *user = nullptr;
    // Mapped coordinates have not been vertically flipped. The image consumer applies
    // the image convention and its scale/invert options, then returns the typed value.
    std::optional<float> (*sample)(const void *user, texture_graph_wire::Word image, bool spectrum,
                                   bool unbounded, texture_graph_wire::Mapped2D coordinates,
                                   float wavelength) = nullptr;
};

// Validates each node as a potential root, with a conservative bound on branch work.
// On success, optional work contains the worst-case visit count of every root.
std::string validate_texture_graph(const TextureGraph &graph, std::size_t image_count,
                                   std::vector<texture_graph_wire::Word> *work = nullptr);

// Direct 0/1 scalar constants and unit-scale UV point imagemaps whose base inputs
// and reachable border are exactly binary at zero footprint. Inversion preserves this property.
bool certify_binary_emitter_mask(const TextureGraph &graph, texture_graph_wire::Word root,
                                 std::span<const ImageTexture> images);

// Evaluate a validated graph at one wavelength. Arithmetic, image lookup and work
// limit failures return nullopt. Spectral values remain raw until the consumer.
std::optional<float> evaluate_texture_graph(const TextureGraph &graph,
                                            texture_graph_wire::Word root,
                                            const texture_graph_wire::Context &context,
                                            float wavelength, TextureImageSampler images = {});

} // namespace rypath
