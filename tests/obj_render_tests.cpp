// SPDX-License-Identifier: GPL-3.0-only
#include "metal_render.hpp"
#include "obj.hpp"
#include "pbrt_render.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
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
        auto name = (fs::temp_directory_path() / "rypath-obj-render-XXXXXX").string();
        require(mkdtemp(name.data()) != nullptr, "create scratch");
        path = name;
    }
    ~Temporary() {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }
};
void write(const fs::path &path, std::string_view source) {
    std::ofstream out(path);
    out << source;
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
constexpr std::string_view shape = R"(Shape "trianglemesh"
"point3 P" [-2 -2 3 2 -2 3 2 2 3 -2 2 3]
"normal N" [0 0 -1 0 0 -1 0 0 -1 0 0 -1]
"point2 uv" [0 0 1 0 1 1 0 1]
"integer indices" [0 2 1 0 3 2]
)";
constexpr std::string_view geometry = "mtllib model.mtl\nv -2 -2 3\nv 2 -2 3\nv 2 2 3\nv -2 2 3\n"
                                      "vt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\nvn 0 0 -1\nusemtl test\n"
                                      "f 1/1/1 3/3/1 2/2/1\nf 1/1/1 4/4/1 3/3/1\n";
RenderScene translate(const fs::path &path) {
    const auto checked = pbrt::check(path);
    require(checked.exit_code() == 0, pbrt::report(checked));
    auto result = pbrt::translate(checked, pbrt::Location{path.string()});
    if (result.exit_code() || !result.scene) {
        for (const auto &d : result.diagnostics)
            std::cerr << pbrt::format_diagnostic(d);
        throw std::runtime_error("translate matched render scene");
    }
    return std::move(result.scene.value());
}
struct Case {
    const char *name;
    const char *mtl;
    const char *expected;
    const char *alpha = "";
};
// These expectations describe the documented mtl-pbr-approx-v1 model directly;
// they never inspect converter definitions or reuse derived converter textures.
constexpr Case cases[] = {
    {"diffuse", "illum 1\nKd .65 .3 .1\n", R"(Material "diffuse" "rgb reflectance" [.65 .3 .1]
)"},
    {"textured", "illum 1\nKd 1 1 1\nmap_Kd gray-pattern.png\n",
     R"(Texture "color" "spectrum" "imagemap" "string filename" "gray-pattern.png"
Material "diffuse" "texture reflectance" "color"
)"},
    {"glossy", "illum 2\nKd .4 .3 .2\nKs .35 .35 .35\nPr .4\nNi 1.45\n",
     R"(MakeNamedMaterial "base" "string type" "diffuse" "rgb reflectance" [.4 .3 .2]
MakeNamedMaterial "coat" "string type" "coateddiffuse" "rgb reflectance" [.4 .3 .2] "float eta" 1.45 "float roughness" .16 "bool remaproughness" false
Material "mix" "string materials" ["base" "coat"] "float amount" .35
)"},
    {"metal", "illum 3\nKs .8 .5 .2\nPr .25\n",
     R"(Material "conductor" "rgb reflectance" [.8 .5 .2] "float roughness" .0625 "bool remaproughness" false
)"},
    {"glass", "illum 7\nNi 1.5\nPr .2\n",
     R"(Material "dielectric" "float eta" 1.5 "float roughness" .04 "bool remaproughness" false
)"},
    {"opacity", "illum 1\nKd .4 .3 .2\nd .35\n", R"(Material "diffuse" "rgb reflectance" [.4 .3 .2]
)",
     " \"float alpha\" .35\n"},
    {"bump", "illum 1\nKd .5 .5 .5\nbump -bm .1 gray-pattern.png\n",
     R"(Texture "height" "float" "imagemap" "string filename" "gray-pattern.png" "string encoding" "linear" "float scale" .1
Material "diffuse" "rgb reflectance" [.5 .5 .5] "texture displacement" "height"
)"},
    {"normal", "illum 1\nKd .5 .5 .5\nnorm normal-tilted.png\n",
     R"(Material "diffuse" "rgb reflectance" [.5 .5 .5] "string normalmap" "normal-tilted.png"
)"},
    {"pbr-mix", "illum 2\nKd .7 .25 .1\nPm .4\nPc .3\nPcr .2\nPr .35\nNi 1.45\n",
     R"(MakeNamedMaterial "diffuse" "string type" "diffuse" "rgb reflectance" [.7 .25 .1]
MakeNamedMaterial "coated-diffuse" "string type" "coateddiffuse" "rgb reflectance" [.7 .25 .1] "float eta" 1.45 "float roughness" .04 "bool remaproughness" false
MakeNamedMaterial "dielectric-branch" "string type" "mix" "string materials" ["diffuse" "coated-diffuse"] "float amount" .3
MakeNamedMaterial "metal" "string type" "conductor" "rgb reflectance" [.7 .25 .1] "float roughness" .1225 "bool remaproughness" false
MakeNamedMaterial "coated-metal" "string type" "coatedconductor" "rgb reflectance" [.7 .25 .1] "float interface.eta" 1.45 "float interface.roughness" .04 "float conductor.roughness" .1225 "bool remaproughness" false
MakeNamedMaterial "metal-branch" "string type" "mix" "string materials" ["metal" "coated-metal"] "float amount" .3
Material "mix" "string materials" ["dielectric-branch" "metal-branch"] "float amount" .4
)"},
    {"emission", "illum 1\nKd 0 0 0\nKe 1 .4 .2\n", R"(Material "diffuse" "rgb reflectance" [0 0 0]
AreaLightSource "diffuse" "rgb L" [1 .4 .2]
)"}};
using Regions = std::array<double, 12>;
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
        const auto quadrant = (sample.pixel % 4 >= 2 ? 1U : 0U) + (sample.pixel / 4 >= 2 ? 2U : 0U);
        const auto xyz = pbrt_cie_xyz(sample.wavelength);
        for (std::size_t channel = 0; channel < 3; ++channel)
            means[std::size_t{3} * quadrant + channel] +=
                470 * xyz[channel] * sample.radiance / (4 * spp);
    }
    return means;
}
double compare(const Case &test, const fs::path &root) {
    double worst_z = 0;
    const auto source = root / test.name;
    fs::create_directory(source);
    // Project-authored assets and generation provenance:
    // tests/fixtures/pbrt-render/core-textures/README.md and generate_assets.py.
    for (const auto file : {"gray-pattern.png", "normal-tilted.png"})
        fs::copy_file(fs::path(RYPATH_SOURCE_DIR) /
                          "tests/fixtures/pbrt-render/core-textures/assets" / file,
                      source / file);
    write(source / "model.obj", geometry);
    write(source / "model.mtl", std::string("newmtl test\n") + test.mtl);
    auto asset = obj::load(source / "model.obj");
    require(asset.asset.has_value() && asset.exit_code() == 0, obj::report(asset));
    auto converted = obj::prepare_materials(asset.asset.value());
    if (obj::publish(asset.asset.value(), converted, source / "bundle")) {
        for (const auto &d : converted.diagnostics)
            std::cerr << pbrt::format_diagnostic(d);
        throw std::runtime_error(std::string(test.name) + " bundle publication failed");
    }
    write(source / "bundle/scene.pbrt", std::string(header) + "Include \"model.pbrt\"\n");
    write(source / "expected.pbrt",
          std::string(header) + test.expected + std::string(shape) + test.alpha);
    auto imported = build_metal_renderer(translate(source / "bundle/scene.pbrt"));
    auto expected = build_metal_renderer(translate(source / "expected.pbrt"));
    require(imported.status == GeometryStatus::success && imported.scene, imported.diagnostic);
    require(expected.status == GeometryStatus::success && expected.scene, expected.diagnostic);
    std::array<Regions, 8> left{}, right{};
    // Disjoint prescribed seeds provide independent randomizations. The interval
    // uses seed-to-seed variance of region means, not correlated per-path variance.
    for (std::size_t i = 0; i < left.size(); ++i) {
        left[i] = render(*imported.scene, 0x1823 + i);
        right[i] = render(*expected.scene, 0x1923 + i);
    }
    for (std::size_t region = 0; region < 12; ++region) {
        double a = 0, b = 0, va = 0, vb = 0;
        for (std::size_t i = 0; i < left.size(); ++i) {
            a += left[i][region] / 8;
            b += right[i][region] / 8;
        }
        for (std::size_t i = 0; i < left.size(); ++i) {
            va += (left[i][region] - a) * (left[i][region] - a) / 56;
            vb += (right[i][region] - b) * (right[i][region] - b) / 56;
        }
        const double se = std::sqrt(va + vb);
        // Six combined standard errors for 120 XYZ region comparisons. The small
        // absolute/relative floor covers binary32 texture resampling differences.
        const double tolerance = 6 * se + 2e-6 + 2e-4 * std::max(a, b);
        const double z = se > 0 ? std::abs(a - b) / se : 0;
        worst_z = std::max(worst_z, z);
        std::cout << test.name << " region " << region / 3 << " channel " << "XYZ"[region % 3]
                  << " imported " << a << " expected " << b << " combined-SE " << se << " z " << z
                  << " tolerance " << tolerance << '\n';
        require(a > 0 && b > 0, "matched fixture must carry positive radiance");
        require(std::abs(a - b) <= tolerance, std::string(test.name) + " raw radiance mismatch");
    }
    if (std::string_view(test.name) == "bump" || std::string_view(test.name) == "normal") {
        // Diffuse detail cases have the same camera, wavelength, light and BSDF
        // random variables under a shared seed. This paired numerical check is
        // sensitive to small shading-map effects masked by independent-seed noise.
        for (std::size_t i = 0; i < right.size(); ++i) {
            const auto paired = render(*imported.scene, 0x1923 + i);
            for (std::size_t channel = 0; channel < paired.size(); ++channel) {
                const auto tolerance = 2e-6 + 2e-4 * std::max(paired[channel], right[i][channel]);
                require(std::abs(paired[channel] - right[i][channel]) <= tolerance,
                        std::string(test.name) + " paired XYZ detail mismatch");
            }
        }
        write(source / "control.pbrt", std::string(header) +
                                           "Material \"diffuse\" \"rgb reflectance\" [.5 .5 .5]\n" +
                                           std::string(shape));
        auto control = build_metal_renderer(translate(source / "control.pbrt"));
        require(control.status == GeometryStatus::success && control.scene, control.diagnostic);
        std::array<Regions, 8> clear{};
        for (std::size_t i = 0; i < clear.size(); ++i)
            clear[i] = render(*control.scene, 0x1923 + i);
        bool detected = false;
        for (std::size_t region = 0; region < 12; ++region) {
            double mean = 0, variance = 0;
            for (std::size_t i = 0; i < clear.size(); ++i)
                mean += (right[i][region] - clear[i][region]) / 8;
            for (std::size_t i = 0; i < clear.size(); ++i) {
                const auto residual = right[i][region] - clear[i][region] - mean;
                variance += residual * residual / 56;
            }
            const auto threshold = 6 * std::sqrt(variance) + 3e-6;
            std::cout << test.name << " negative control region " << region / 3 << " channel "
                      << "XYZ"[region % 3] << " delta " << mean << " threshold " << threshold
                      << '\n';
            detected |= std::abs(mean) > threshold;
        }
        require(detected, "detail fixture must detect omitted bump/normal mapping");
    }
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
    std::cerr << "OBJ matched render: " << e.what() << '\n';
    return 1;
}
