// SPDX-License-Identifier: GPL-3.0-only
#include "metal_render.hpp"
#include "pbrt_render.hpp"
#include "stl.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <unistd.h>

namespace {
using namespace rypath;
namespace fs = std::filesystem;
void require(bool ok, std::string_view why) {
    if (!ok)
        throw std::runtime_error(std::string(why));
}
struct Temporary {
    fs::path path;
    Temporary() {
        auto name = (fs::temp_directory_path() / "rypath-stl-render-XXXXXX").string();
        require(mkdtemp(name.data()) != nullptr, "create scratch");
        path = name;
    }
    ~Temporary() {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }
};
void write(const fs::path &path, std::string_view source) {
    std::ofstream out(path, std::ios::binary);
    out.write(source.data(), static_cast<std::streamsize>(source.size()));
    require(out.good(), "write render fixture");
}
constexpr std::string_view header = R"(Camera "perspective" "float fov" 36
Film "rgb" "integer xresolution" 4 "integer yresolution" 4 "string sensor" "cie1931"
PixelFilter "box"
Sampler "independent" "integer pixelsamples" 512
Integrator "path" "integer maxdepth" 4
WorldBegin
LightSource "infinite" "rgb L" [1 .8 .6]
AttributeBegin
Material "diffuse" "rgb reflectance" [0 0 0]
AreaLightSource "diffuse" "rgb L" [20 15 10]
Shape "trianglemesh" "point3 P" [1.1 -.6 1 2.3 -.6 1 2.3 .6 1 1.1 .6 1] "integer indices" [0 1 2 0 2 3]
AttributeEnd
)";
using Point = std::array<float, 3>;
constexpr Point center{0, 0, 2.5F};
constexpr std::array<Point, 4> corners{
    {{-2, -2, 3.5F}, {2, -2, 3.5F}, {2, 2, 3.5F}, {-2, 2, 3.5F}}};
using RGB = std::array<double, 3>;
struct Case {
    const char *name;
    stl::ColorMode mode;
    bool header_color;
    std::uint8_t alpha;
    std::array<std::uint16_t, 4> words;
    // Independently authored normalized encoded channels, rather than a second
    // implementation of the production bit decoder.
    std::array<RGB, 4> encoded;
    std::array<bool, 4> inherited;
    bool transfer;
};
constexpr RGB red{1, 0, 0}, blue{0, 0, 1}, gray5{16.0 / 31, 16.0 / 31, 16.0 / 31};
constexpr RGB object_color{128.0 / 255, 64.0 / 255, 192.0 / 255};
constexpr std::array<std::uint16_t, 4> magics_words{0x8000, 0x001f, 0x7c00, 0x8000};
constexpr std::array<RGB, 4> magics_colors{object_color, red, blue, object_color};
constexpr std::array<bool, 4> magics_inherited{true, false, false, true};
constexpr Case cases[]{
    {"flat-uncolored",
     stl::ColorMode::automatic,
     false,
     255,
     {0, 0, 0, 0},
     {RGB{.5, .5, .5}, RGB{.5, .5, .5}, RGB{.5, .5, .5}, RGB{.5, .5, .5}},
     {false, false, false, false},
     false},
    // VisCAM: bit 15 valid, high five bits red, low five bits blue.
    {"viscam",
     stl::ColorMode::viscam,
     false,
     255,
     {0xfc00, 0x801f, 0xfc00, 0xc210},
     {red, blue, red, gray5},
     {false, false, false, false},
     true},
    // Magics: bit 15 inherits header RGBA, low five bits red, high bits blue.
    {"magics", stl::ColorMode::magics, true, 255, magics_words, magics_colors, magics_inherited,
     true},
    {"alpha-zero", stl::ColorMode::automatic, true, 0, magics_words, magics_colors,
     magics_inherited, true},
    {"alpha-half", stl::ColorMode::automatic, true, 128, magics_words, magics_colors,
     magics_inherited, true},
    {"alpha-full", stl::ColorMode::automatic, true, 255, magics_words, magics_colors,
     magics_inherited, true},
};
template <unsigned ByteCount> void little(std::string &out, std::uint32_t value) {
    for (unsigned i = 0; i < ByteCount; ++i)
        out.push_back(static_cast<char>((value >> (8 * i)) & 255));
}
void point(std::string &out, const Point &p) {
    for (const float v : p)
        little<4>(out, std::bit_cast<std::uint32_t>(v));
}
void binary_fixture(const fs::path &path, const Case &test) {
    std::string bytes(80, '\0');
    if (test.header_color) {
        bytes.replace(0, 6, "COLOR=");
        bytes[6] = static_cast<char>(128);
        bytes[7] = 64;
        bytes[8] = static_cast<char>(192);
        bytes[9] = static_cast<char>(test.alpha);
    }
    little<4>(bytes, 4);
    for (std::size_t i = 0; i < 4; ++i) {
        // The supplied normal is deliberately inconsistent with the bent
        // geometry. STL normals must not become shading/smoothing normals.
        point(bytes, {0, 0, 1});
        point(bytes, center);
        point(bytes, corners[(i + 1) % 4]);
        point(bytes, corners[i]);
        little<2>(bytes, test.words[i]);
    }
    write(path, bytes);
}
// IEC sRGB EOTF, evaluated independently on normalized 5-bit/8-bit channels.
// PBRT RGB parameters are linear and must not receive another transfer decode.
double linear(double encoded) {
    return encoded <= .04045 ? encoded / 12.92 : std::pow((encoded + .055) / 1.055, 2.4);
}
std::string reference(const Case &test, bool omit_transfer = false, bool ignore_alpha = false,
                      bool smooth = false) {
    std::ostringstream out;
    out << std::setprecision(17) << header;
    for (std::size_t i = 0; i < 4; ++i) {
        out << "AttributeBegin\nMaterial \"diffuse\" \"rgb reflectance\" [";
        for (const auto value : test.encoded[i])
            out << (test.transfer && !omit_transfer ? linear(value) : value) << ' ';
        out << "]\nShape \"trianglemesh\" \"point3 P\" [";
        for (const auto &p : {center, corners[(i + 1) % 4], corners[i]})
            out << p[0] << ' ' << p[1] << ' ' << p[2] << ' ';
        out << "] \"integer indices\" [0 1 2]";
        if (smooth) {
            // Area-weighted shared-vertex normals for this symmetric pyramid.
            // Each corner averages its two adjacent faces; the center is -Z.
            out << " \"normal N\" [0 0 -1 ";
            for (const auto &p : {corners[(i + 1) % 4], corners[i]})
                out << p[0] / 8 << ' ' << p[1] / 8 << " -1 ";
            out << ']';
        }
        out << " \"float alpha\" " << (ignore_alpha ? 1 : test.alpha / 255.0) << "\nAttributeEnd\n";
    }
    return out.str();
}
RenderScene translate(const fs::path &path) {
    const auto checked = pbrt::check(path);
    require(checked.exit_code() == 0, pbrt::report(checked));
    auto translated = pbrt::translate(checked, pbrt::Location{path.string()});
    if (translated.exit_code() || !translated.scene) {
        for (const auto &d : translated.diagnostics)
            std::cerr << pbrt::format_diagnostic(d);
        throw std::runtime_error("translate matched render scene");
    }
    return std::move(*translated.scene);
}
using Regions = std::array<double, 12>;
using Randomizations = std::array<Regions, 8>;
Regions render(MetalRenderScene &renderer, std::uint64_t seed) {
    constexpr std::uint32_t spp = 512;
    const auto result = renderer.sample({.width = 4,
                                         .height = 4,
                                         .seed = seed,
                                         .pixel_count = 16,
                                         .sample_count = spp,
                                         .total_samples = spp});
    require(result.status == GeometryStatus::success, result.diagnostic);
    require(result.samples.size() == std::size_t{16} * spp && result.guard_count > 0,
            "complete guarded Metal sample set");
    Regions means{};
    for (const auto &sample : result.samples) {
        require(sample.pixel < 16 && sample.sample < spp && std::isfinite(sample.radiance),
                "finite addressed raw samples");
        const std::size_t quadrant =
            (sample.pixel % 4 >= 2 ? 1U : 0U) + (sample.pixel / 4 >= 2 ? 2U : 0U);
        const auto xyz = pbrt_cie_xyz(sample.wavelength);
        for (std::size_t channel = 0; channel < 3; ++channel)
            means[3 * quadrant + channel] += 470 * xyz[channel] * sample.radiance / (4 * spp);
    }
    return means;
}
Randomizations samples(const fs::path &path, std::uint64_t first_seed) {
    auto renderer = build_metal_renderer(translate(path));
    require(renderer.status == GeometryStatus::success && renderer.scene, renderer.diagnostic);
    Randomizations result{};
    for (std::size_t i = 0; i < result.size(); ++i)
        result[i] = render(*renderer.scene, first_seed + i);
    return result;
}
void negative_control(const fs::path &path, std::string_view name, const Randomizations &expected,
                      std::string_view scene) {
    write(path, scene);
    const auto control = samples(path, 0x2923);
    bool detected = false;
    for (std::size_t r = 0; r < 12; ++r) {
        double delta = 0, variance = 0;
        for (std::size_t i = 0; i < 8; ++i)
            delta += (expected[i][r] - control[i][r]) / 8;
        for (std::size_t i = 0; i < 8; ++i) {
            const auto residual = expected[i][r] - control[i][r] - delta;
            variance += residual * residual / 56;
        }
        const auto threshold = 6 * std::sqrt(variance) + 3e-6;
        std::cout << name << " control region " << r / 3 << " channel " << "XYZ"[r % 3] << " delta "
                  << delta << " threshold " << threshold << '\n';
        detected |= std::abs(delta) > threshold;
    }
    require(detected, std::string(name) + " negative control must detect regression");
}
double compare(const Case &test, const fs::path &root) {
    const auto source = root / test.name;
    fs::create_directory(source);
    binary_fixture(source / "model.stl", test);
    auto loaded = stl::load(source / "model.stl", {.colors = test.mode}, {});
    if (loaded.exit_code() != 0 || !loaded.asset)
        throw std::runtime_error(stl::report(loaded));
    const auto &asset = *loaded.asset;
    if (!test.transfer)
        require(asset.effective_colors == stl::ColorMode::none, "uncolored effective mode");
    require(asset.facets.size() == 4, "four source facets");
    for (std::size_t i = 0; i < 4; ++i)
        require(asset.facets[i].inherited_color == test.inherited[i], "source color ownership");
    if (test.transfer) {
        // First occurrence establishes part order; repeated source colors must
        // retain source facet order without regrouping the geometric identity.
        const std::array<std::vector<std::size_t>, 3> expected =
            std::string_view(test.name) == "viscam"
                ? std::array<std::vector<std::size_t>, 3>{{{0, 2}, {1}, {3}}}
                : std::array<std::vector<std::size_t>, 3>{{{0, 3}, {1}, {2}}};
        require(asset.parts.size() == expected.size(), "three color parts");
        for (std::size_t i = 0; i < expected.size(); ++i)
            require(asset.parts[i].source_facets == expected[i],
                    "stable color grouping/source order");
    }
    auto converted = stl::prepare(asset, loaded.diagnostics, {});
    require(converted.exit_code() == 0, "prepare matched STL bundle");
    if (stl::publish(asset, converted, source / "bundle", {})) {
        for (const auto &d : converted.diagnostics)
            std::cerr << pbrt::format_diagnostic(d);
        throw std::runtime_error("publish matched STL bundle");
    }
    write(source / "bundle/scene.pbrt", std::string(header) + "Include \"model.pbrt\"\n");
    write(source / "expected.pbrt", reference(test));
    // Disjoint seeds and seed-to-seed region variance match the OBJ gate.
    const auto imported = samples(source / "bundle/scene.pbrt", 0x2823);
    const auto expected = samples(source / "expected.pbrt", 0x2923);
    double worst_z = 0;
    for (std::size_t r = 0; r < 12; ++r) {
        double a = 0, b = 0, va = 0, vb = 0;
        for (std::size_t i = 0; i < 8; ++i) {
            a += imported[i][r] / 8;
            b += expected[i][r] / 8;
        }
        for (std::size_t i = 0; i < 8; ++i) {
            va += (imported[i][r] - a) * (imported[i][r] - a) / 56;
            vb += (expected[i][r] - b) * (expected[i][r] - b) / 56;
        }
        const auto se = std::sqrt(va + vb);
        const auto tolerance = 6 * se + 2e-6 + 2e-4 * std::max(a, b);
        const auto z = se > 0 ? std::abs(a - b) / se : 0;
        worst_z = std::max(worst_z, z);
        std::cout << test.name << " region " << r / 3 << " channel " << "XYZ"[r % 3] << " imported "
                  << a << " expected " << b << " combined-SE " << se << " z " << z << " tolerance "
                  << tolerance << '\n';
        require(a > 0 && b > 0, "matched fixture must carry positive radiance");
        require(std::abs(a - b) <= tolerance, std::string(test.name) + " raw XYZ mismatch");
    }
    if (std::string_view(test.name) == "flat-uncolored")
        negative_control(source / "control.pbrt", "smoothing", expected,
                         reference(test, false, false, true));
    if (std::string_view(test.name) == "viscam")
        negative_control(source / "control.pbrt", "sRGB-transfer", expected, reference(test, true));
    if (std::string_view(test.name) == "alpha-half")
        negative_control(source / "control.pbrt", "alpha", expected, reference(test, false, true));
    return worst_z;
}
} // namespace
int main() try {
    const auto device = query_metal_device();
#if defined(RYPATH_ALLOW_NO_METAL_DEVICE)
    if (device.status == MetalStatus::no_device) {
        std::cout << "SKIP: " << device.diagnostic << '\n';
        return 77;
    }
#endif
    require(device.status == MetalStatus::success, device.diagnostic);
#if defined(RYPATH_ALLOW_NO_METAL_RAY_TRACING)
    if (!device.device.compute_ray_tracing) {
        std::cout << "SKIP: no compute ray tracing\n";
        return 77;
    }
#endif
    require(device.device.compute_ray_tracing, "Metal ray tracing required");
    std::cout << "Device: " << device.device.name << '\n';
    Temporary temporary;
    double worst_z = 0;
    for (const auto &test : cases)
        worst_z = std::max(worst_z, compare(test, temporary.path));
    std::cout << "Worst XYZ standardized difference: " << worst_z << '\n';
    return 0;
} catch (const std::exception &e) {
    std::cerr << "STL matched render: " << e.what() << '\n';
    return 1;
}
