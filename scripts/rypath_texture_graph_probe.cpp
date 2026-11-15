// SPDX-License-Identifier: GPL-3.0-only

#include "texture_graph_probe_inputs.hpp"

#include "pbrt_spectra.hpp"
#include "rgb_spectrum.hpp"
#include "spectrum_file.hpp"
#include "texture_graph.h"
#include "texture_graph.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

using namespace rypath;
namespace wire = texture_graph_wire;
namespace probe = graph_probe;
using wire::Word;

wire::Point point(const std::array<float, 3> &p) { return {p[0], p[1], p[2]}; }

wire::Mapping mapping(Word kind) {
    wire::Mapping m{.kind = kind};
    if (kind != wire::uv) {
        for (std::size_t i = 0; i < 3; ++i) {
            m.texture_from_render[5 * i] = probe::stretch[i];
            m.texture_from_render[4 * i + 3] = probe::translate[i];
        }
    }
    if (kind == wire::uv) {
        std::ranges::copy(probe::uv, m.values);
    } else if (kind == wire::planar) {
        std::ranges::copy(probe::vs, m.values);
        std::ranges::copy(probe::vt, m.values + 3);
        m.values[6] = probe::planar_ds;
        m.values[7] = probe::planar_dt;
    }
    return m;
}

std::shared_ptr<const ImagePyramid> read_png(const std::filesystem::path &path) {
    std::ifstream file(path, std::ios::binary);
    const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(file), {}};
    const auto decoded = decode_png(bytes);
    if (!decoded.value) {
        throw std::runtime_error(decoded.diagnostic);
    }
    auto pyramid = build_image_pyramid(*decoded.value, ImageEncoding::linear, ImageWrap::repeat);
    if (!pyramid.value) {
        throw std::runtime_error(pyramid.diagnostic);
    }
    return std::make_shared<const ImagePyramid>(std::move(*pyramid.value));
}

std::optional<float> sampled_image(const void *data, Word index, bool spectral, wire::Mapped2D c,
                                   float wavelength) {
    const auto &images = *static_cast<const std::vector<ImageTexture> *>(data);
    const auto &texture = images.at(index);
    const auto &image = *texture.image;
    if (!spectral) {
        const auto scalar = image_linear_scalar(image, c.s, 1 - c.t, c.dsdx, c.dtdx, c.dsdy, c.dtdy,
                                                texture.filter);
        if (!scalar) {
            return std::nullopt;
        }
        const float scaled = *scalar * texture.scale;
        return texture.invert ? std::max(0.0f, 1.0f - scaled) : scaled;
    }
    auto rgb =
        image_linear_rgb(image, c.s, 1 - c.t, c.dsdx, c.dtdx, c.dsdy, c.dtdy, texture.filter);
    if (!rgb) {
        return std::nullopt;
    }
    for (float &component : *rgb) {
        component *= texture.scale;
        if (texture.invert) {
            component = 1.0f - component;
        }
    }
    for (float &component : *rgb) {
        component = std::clamp(component, 0.0f, 1.0f);
    }
    const auto reconstructed = reconstruct_srgb(*rgb);
    return reconstructed ? std::optional<float>(evaluate_srgb(*reconstructed, wavelength))
                         : std::nullopt;
}

struct ProbePaths {
    std::filesystem::path rgb;
    std::filesystem::path grayscale;
    std::filesystem::path spectrum;
};

void compare(const ProbePaths &paths) {
    std::vector<ImageTexture> images;
    std::vector<std::pair<std::string, Word>> roots;
    TextureGraph graph;
    const auto add = [&](wire::Node node, std::string label = {}) {
        const auto index = static_cast<Word>(graph.nodes.size());
        graph.nodes.push_back(node);
        if (!label.empty()) {
            roots.emplace_back(std::move(label), index);
        }
        return index;
    };
    const auto constant = [&](float value, Word type, const char *label = "") {
        return add({.type = type, .values = {value}}, label);
    };
    const auto fa = constant(probe::a, wire::scalar, "scalar-constant");
    const auto fb = constant(probe::b, wire::scalar);
    const auto fc = constant(probe::c, wire::scalar);
    const auto fd = constant(probe::d, wire::scalar);
    const auto fzero = constant(0, wire::scalar);
    const auto fone = constant(1, wire::scalar);
    const auto sa = constant(probe::a, wire::spectrum);
    const auto sb = constant(probe::b, wire::spectrum);
    const auto sc = constant(probe::c, wire::spectrum);
    const auto sd = constant(probe::d, wire::spectrum, "spectrum-constant");

    const pbrt::Limits limits;
    pbrt::PlyBudget budget{limits.expanded_bytes, limits.expansions, limits.total_values};
    const auto source = pbrt::decode_spectrum_file(paths.spectrum, limits, budget);
    if (const auto *issue = std::get_if<pbrt::SpectrumFileIssue>(&source)) {
        throw std::runtime_error(issue->message);
    }
    const auto &file_knots = std::get<std::shared_ptr<const pbrt::CheckedSpectrumSource>>(source);
    for (const auto knot : file_knots->knots) {
        graph.knots.push_back({knot.wavelength, knot.value});
    }
    const auto file_count = static_cast<Word>(graph.knots.size());
    const auto file = add({.kind = wire::knots, .type = wire::spectrum, .payload = {0, file_count}},
                          "spectrum-file");
    const auto named_values = pbrt_named_spectrum_knots(probe::named_spectrum);
    for (const auto knot : named_values) {
        graph.knots.push_back({knot.wavelength, knot.value});
    }
    const auto named = add({.kind = wire::knots,
                            .type = wire::spectrum,
                            .payload = {file_count, static_cast<Word>(named_values.size())}},
                           "spectrum-named");
    const auto rgb_data = reconstruct_srgb(probe::color);
    const auto rgb = add({.kind = wire::rgb_sigmoid,
                          .type = wire::spectrum,
                          .values = {rgb_data->coefficients[0], rgb_data->coefficients[1],
                                     rgb_data->coefficients[2]}},
                         "spectrum-rgb");
    const auto planck =
        add({.kind = wire::blackbody, .type = wire::spectrum, .values = {probe::temperature}},
            "spectrum-blackbody");

    add({.kind = wire::scale, .children = {fb, fc}}, "scalar-scale");
    add({.kind = wire::scale, .type = wire::spectrum, .children = {sa, fd}}, "spectrum-scale");
    add({.kind = wire::mix, .children = {fa, fb, fc}}, "scalar-mix");
    add({.kind = wire::mix, .type = wire::spectrum, .children = {file, named, fd}}, "spectrum-mix");
    add({.kind = wire::mix, .type = wire::spectrum, .children = {rgb, planck, fzero}},
        "spectrum-mix-0");
    add({.kind = wire::mix, .type = wire::spectrum, .children = {rgb, planck, fone}},
        "spectrum-mix-1");

    wire::Node directional{.kind = wire::directionmix, .children = {fa, fb}};
    std::ranges::copy(probe::direction, directional.values);
    add(directional, "scalar-directionmix");
    directional.type = wire::spectrum;
    directional.children[0] = sa;
    directional.children[1] = sb;
    add(directional, "spectrum-directionmix");
    add({.kind = wire::bilerp, .children = {fa, fb, fc, fd}, .mapping = mapping(wire::planar)},
        "scalar-bilerp");
    add({.kind = wire::bilerp,
         .type = wire::spectrum,
         .children = {sa, sb, sc, sd},
         .mapping = mapping(wire::uv)},
        "spectrum-bilerp");
    add({.kind = wire::checkerboard, .children = {fa, fb}, .mapping = mapping(wire::spherical)},
        "scalar-checkerboard");
    add({.kind = wire::checkerboard,
         .type = wire::spectrum,
         .children = {sa, sb},
         .mapping = mapping(wire::spatial)},
        "spectrum-checkerboard");
    add({.kind = wire::dots, .children = {fa, fb}, .mapping = mapping(wire::uv)}, "scalar-dots");
    add({.kind = wire::dots,
         .type = wire::spectrum,
         .children = {sa, sb},
         .mapping = mapping(wire::cylindrical)},
        "spectrum-dots");
    add({.kind = wire::fbm,
         .payload = {probe::octaves},
         .values = {probe::roughness},
         .mapping = mapping(wire::spatial)},
        "scalar-fbm");
    add({.kind = wire::wrinkled,
         .payload = {probe::octaves},
         .values = {probe::roughness},
         .mapping = mapping(wire::spatial)},
        "scalar-wrinkled");
    add({.kind = wire::windy, .mapping = mapping(wire::spatial)}, "scalar-windy");
    add({.kind = wire::marble,
         .type = wire::spectrum,
         .payload = {probe::octaves},
         .values = {probe::roughness, probe::marble_scale, probe::marble_variation},
         .mapping = mapping(wire::spatial)},
        "spectrum-marble");

    const auto image_node = [&](ImageTexture texture, Word type, Word map, std::string label) {
        const auto index = static_cast<Word>(images.size());
        images.push_back(std::move(texture));
        return add({.kind = wire::image, .type = type, .payload = {index}, .mapping = mapping(map)},
                   std::move(label));
    };
    const auto image = read_png(paths.rgb);
    const auto gray = image_node(
        {.image = read_png(paths.grayscale), .scale = 2.5f, .filter = ImageFilter::point},
        wire::spectrum, wire::uv, "spectrum-image-gray");
    add({.kind = wire::scale, .type = wire::spectrum, .children = {gray, fa}},
        "spectrum-scale-image-gray");
    constexpr std::array filters{ImageFilter::point, ImageFilter::bilinear, ImageFilter::trilinear,
                                 ImageFilter::ewa};
    constexpr std::array maps{wire::uv, wire::planar, wire::spherical, wire::cylindrical};
    constexpr std::array names{"map-uv", "map-planar", "map-spherical", "map-cylindrical"};
    for (std::size_t i = 0; i < maps.size(); ++i) {
        image_node({.image = image, .scale = 1.4f, .invert = true, .filter = filters[i]},
                   wire::scalar, maps[i], std::string("scalar-image-") + probe::filter_names[i]);
        image_node({.image = image, .scale = .9f, .filter = filters[i]}, wire::spectrum, maps[i],
                   std::string("spectrum-image-") + probe::filter_names[i]);
    }
    const auto first_axis_node = static_cast<Word>(graph.nodes.size());
    constexpr std::array angular{wire::spherical, wire::cylindrical};
    for (const auto kind : angular) {
        graph.nodes.push_back(
            {.kind = wire::bilerp, .children = {fa, fb, fc, fd}, .mapping = {.kind = kind}});
    }
    for (const auto kind : angular) {
        graph.nodes.push_back(
            {.kind = wire::dots, .children = {fa, fb}, .mapping = {.kind = kind}});
    }
    const auto diagnostic = validate_texture_graph(graph, images.size());
    if (!diagnostic.empty()) {
        throw std::runtime_error(diagnostic);
    }
    const TextureImageSampler sampler{.user = &images, .sample = sampled_image};
    std::cout << std::setprecision(9);
    for (int i = 0; i < probe::query_count; ++i) {
        const auto q = probe::query(i);
        const wire::Context ctx{point(q.p), point(q.n), point(q.dx), point(q.dy), q.u,
                                q.v,        q.dudx,     q.dvdx,      q.dudy,      q.dvdy};
        for (std::size_t j = 0; j < maps.size(); ++j) {
            wire::Mapped2D m;
            if (!texture_graph::map2d(mapping(maps[j]), ctx, m)) {
                throw std::runtime_error("invalid mapped probe coordinate");
            }
            probe::row(i, names[j], m.s, m.t, m.dsdx, m.dtdx, m.dsdy, m.dtdy);
        }
        const auto m = texture_graph::map3d(mapping(wire::spatial), ctx);
        probe::row(i, "map-spatial", m.p.x, m.p.y, m.p.z, m.dx.x, m.dx.y, m.dx.z, m.dy.x, m.dy.y,
                   m.dy.z);
        for (const auto &[label, root] : roots) {
            const auto value = evaluate_texture_graph(graph, root, ctx, q.wavelength, sampler);
            if (!value) {
                throw std::runtime_error("invalid probe value " + label);
            }
            probe::row(i, label, *value);
        }
    }
    for (const float z : {.25f, -.25f}) {
        const wire::Context axis{.p = {0, 0, z}, .dpdx = {1, 0, 0}, .dpdy = {0, 1, 0}};
        std::array<float, 4> results{};
        for (Word i = 0; i < results.size(); ++i) {
            const auto value = evaluate_texture_graph(graph, first_axis_node + i, axis, 550);
            if (!value) {
                throw std::runtime_error("invalid coordinate-only angular axis sample");
            }
            results[i] = *value;
        }
        probe::row(z > 0 ? 0 : 1, "angular-axes", results[0], results[1], results[2], results[3]);
    }
}

} // namespace

int main(int argc, char **argv) {
    if (argc != 4) {
        std::cerr << "Expected RGB PNG, grayscale PNG, and probe spectrum file paths\n";
        return 2;
    }
    try {
        compare({.rgb = argv[1], .grayscale = argv[2], .spectrum = argv[3]});
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
