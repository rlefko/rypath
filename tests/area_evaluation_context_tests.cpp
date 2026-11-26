// SPDX-License-Identifier: GPL-3.0-only
#include "area_evaluation_context.hpp"
#include <array>
#include <bit>
#include <iostream>
#include <stdexcept>

int main() try {
    using namespace rypath;
    const auto need = [](bool ok) {
        if (!ok)
            throw std::runtime_error("Exact Area evaluation context/accounting contract");
    };
    render_wire::Parameters p{};
    render_wire::TextureParameters t{};
    need(same_area_evaluation_parameters(p, p, t, t));
    // Check every wire word, including signed-zero and one-bit float changes:
    // future semantic fields must not silently escape the identity comparison.
    using Words = std::array<std::uint32_t, sizeof(p) / 4>;
    const auto baseline = std::bit_cast<Words>(p);
    for (std::size_t i = 0; i < baseline.size(); ++i) {
        auto changed = baseline;
        changed[i] ^= 1;
        const bool label = i == offsetof(render_wire::Parameters, sample_low) / 4 ||
                           i == offsetof(render_wire::Parameters, sample_high) / 4;
        need(same_area_evaluation_parameters(p, std::bit_cast<render_wire::Parameters>(changed), t,
                                             t) == label);
        changed = baseline;
        changed[i] ^= 0x80000000u;
        need(same_area_evaluation_parameters(p, std::bit_cast<render_wire::Parameters>(changed), t,
                                             t) == label);
    }
    using TextureWords = std::array<std::uint32_t, sizeof(t) / 4>;
    const auto texture = std::bit_cast<TextureWords>(t);
    for (std::size_t i = 0; i < texture.size(); ++i) {
        auto changed = texture;
        changed[i] ^= 1;
        need(!same_area_evaluation_parameters(
            p, p, t, std::bit_cast<render_wire::TextureParameters>(changed)));
    }
    // Bank-owned mappings may occupy different buffers yet have identical
    // contents. Equal sizes or equal camera origins alone never permit reuse.
    const std::array<std::uint32_t, 8> graph{1, 2, 3, 4, 5, 6, 7, 8};
    auto graph_copy = graph;
    need(same_area_evaluation_bytes(graph.data(), sizeof(graph), graph_copy.data(),
                                    sizeof(graph_copy)));
    graph_copy[4] ^= 1;
    need(!same_area_evaluation_bytes(graph.data(), sizeof(graph), graph_copy.data(),
                                     sizeof(graph_copy)));
    auto surfaces = graph;
    surfaces[7] ^= 0x80000000u;
    need(!same_area_evaluation_bytes(graph.data(), sizeof(graph), surfaces.data(),
                                     sizeof(surfaces)));
    need(!same_area_evaluation_bytes(graph.data(), sizeof(graph), graph.data(), sizeof(graph) - 4));
    need(!same_area_evaluation_bytes(nullptr, sizeof(graph), graph.data(), sizeof(graph)));
    need(same_area_evaluation_bytes(nullptr, 0, nullptr, 0));
    for (std::uint64_t pixels : {1ull, 21ull, 2073600ull})
        for (bool history : {false, true})
            for (bool cache : {false, true})
                for (unsigned mutations : {0u, 1u, 2u, 4u}) {
                    const auto logical = pixels * ((history ? 7u : 5u) + mutations);
                    const bool used = cache && history;
                    const auto cached = used ? 2 * pixels : 0;
                    const auto physical = logical - cached;
                    need(valid_area_evaluation_accounting(pixels, history, mutations, used,
                                                          physical, cached, logical));
                    need(!valid_area_evaluation_accounting(pixels, history, mutations, used,
                                                           physical + 1, cached, logical));
                    need(!valid_area_evaluation_accounting(pixels, history, mutations, used,
                                                           physical, cached + 1, logical));
                    need(!valid_area_evaluation_accounting(pixels, history, mutations, used,
                                                           physical, cached, logical + 1));
                }
    need(!valid_area_evaluation_accounting(1, false, 0, true, 5, 0, 5));
    std::cout << "Exact Area evaluation context and physical/cached/logical accounting passed\n";
} catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
}
