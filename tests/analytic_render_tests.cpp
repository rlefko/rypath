// SPDX-License-Identifier: GPL-3.0-only
#include "metal_render.hpp"
#include "pbrt_render.hpp"
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numbers>
#include <stdexcept>
#include <unistd.h>
namespace {
using namespace rypath;
void require(bool test, const std::string &message) {
    if (!test)
        throw std::runtime_error(message);
}
struct SceneFile {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
                                 ("rypath-analytic-render-" + std::to_string(getpid()) + ".pbrt");
    ~SceneFile() { std::filesystem::remove(path); }
    void write(const std::string &camera, const std::string &body, unsigned depth) {
        std::ofstream(path) << camera
                            << "Camera \"perspective\" \"float fov\" [0.01]\nFilm \"rgb\" "
                               "\"integer xresolution\" [1] \"integer yresolution\" [1]\n"
                               "Sampler \"independent\" \"integer pixelsamples\" "
                               "[16384]\nIntegrator \"path\" \"integer maxdepth\" ["
                            << depth << "]\nWorldBegin\n"
                            << body << '\n';
    }
};
struct Prepared {
    std::unique_ptr<MetalRenderScene> scene;
    float emission;
};
Prepared prepare(SceneFile &f, const std::string &camera, const std::string &body, unsigned depth,
                 bool default_orientation = false) {
    f.write(camera, body, depth);
    auto checked = pbrt::check(f.path);
    require(checked.exit_code() == 0, pbrt::report(checked));
    auto translated = pbrt::translate(checked, {});
    std::string why;
    for (const auto &d : translated.diagnostics)
        why += d.message + "\n";
    if (translated.exit_code() != 0 || !translated.scene)
        throw std::runtime_error(why);
    const auto &source = translated.scene->area_emissions.empty()
                             ? translated.scene->environment
                             : translated.scene->area_emissions[0].radiance;
    const float reference = source[0];
    require(reference > 0, "positive reference emission");
    for (float e : source)
        require(e == reference, "flat prepared emission");
    if (default_orientation)
        translated.scene->instance_reverse_orientation.clear();
    auto built = build_metal_renderer(*translated.scene);
    require(built.status == GeometryStatus::success, built.diagnostic);
    return {std::move(built.scene), reference};
}
// This fixture API intentionally takes the sample count followed by its fixed RNG seed.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
double mean(Prepared &scene, unsigned count, unsigned seed) {
    RenderRequest request;
    request.width = 1;
    request.height = 1;
    request.pixel_count = 1;
    request.sample_count = count;
    request.total_samples = 16384;
    request.seed = seed;
    auto samples = scene.scene->sample(request);
    require(samples.status == GeometryStatus::success, samples.diagnostic);
    require(samples.samples.size() == count, "complete analytic render samples");
    double sum = 0;
    for (const auto &s : samples.samples) {
        require(std::isfinite(s.radiance) && s.radiance >= 0, "finite analytic radiance");
        sum += s.radiance / scene.emission;
    }
    return sum / count;
}
int run() {
    const auto device = query_metal_device();
    if (device.status == MetalStatus::no_device) {
#ifdef RYPATH_ALLOW_NO_METAL_DEVICE
        std::cerr << device.diagnostic << '\n';
        return 77;
#else
        require(false, device.diagnostic);
#endif
    }
    if (!device.device.compute_ray_tracing) {
#ifdef RYPATH_ALLOW_NO_METAL_RAY_TRACING
        std::cerr << "No compute ray tracing\n";
        return 77;
#else
        require(false, "Analytic render tests require compute ray tracing");
#endif
    }
    SceneFile f;
    constexpr auto emission = "AreaLightSource \"diffuse\" \"spectrum L\" [360 1 830 1]\n";
    for (const auto &shape :
         {"Translate 0 0 3\nShape \"sphere\"",
          "Translate 0 0 3\nRotate 90 0 1 0\nShape \"cylinder\"",
          "Translate 0 0 3\nReverseOrientation\nShape \"disk\"",
          "Translate 0 0 3\nScale 1 1 -1\nShape \"sphere\"",
          "Translate 0 0 3\nScale -2 3 1\nReverseOrientation\nShape \"disk\""}) {
        auto scene = prepare(f, "", std::string(emission) + shape, 0);
        const auto value = mean(scene, 32, 1);
        std::cout << "primary analytic " << shape << " radiance " << value << '\n';
        require(std::abs(value - 1) < 1e-6, "primary analytic emission");
    }
    auto direct_reflection = prepare(
        f, "", std::string(emission) + "Translate 0 0 3\nScale 1 1 -1\nShape \"sphere\"", 0, true);
    require(std::abs(mean(direct_reflection, 32, 1) - 1) < 1e-6,
            "direct API reflected outward emission");
    // Reflections in definitions and placements agree for texture/detail frames.
    for (const auto &geometry : {"Translate 0 0 3\nScale -2 3 1\nShape \"sphere\"",
                                 "ObjectBegin \"s\"\nShape \"sphere\"\nObjectEnd\nTranslate 0 0 "
                                 "3\nScale -2 3 1\nObjectInstance \"s\""}) {
        auto furnace =
            prepare(f, "",
                    std::string("LightSource \"infinite\" \"spectrum L\" [360 1 830 1]\n") +
                        "Material \"diffuse\" \"spectrum reflectance\" [360 .5 830 .5] \"float "
                        "displacement\" [0]\n" +
                        geometry,
                    1);
        for (unsigned seed : {7, 19, 41}) {
            const auto value = mean(furnace, 16384, seed);
            std::cout << "analytic reflected furnace " << value << " expected .5\n";
            require(std::abs(value - .5) < .015, "reflected analytic detail-frame furnace");
        }
    }
    // Independent irradiance identities at the center of a Lambertian receiver.
    // FOV .01 degrees makes positional error negligible relative to this budget.
    const std::string receiver = "Material \"diffuse\" \"spectrum reflectance\" [360 1 830 "
                                 "1]\nShape \"disk\" \"float radius\" [10]\n";
    const std::string camera = "LookAt 0 0 1  0 0 0  0 1 0\n";
    struct Case {
        const char *geometry;
        double expected;
    };
    const Case cases[] = {
        {"Translate 0 0 3\nShape \"sphere\" \"float radius\" [.5]", 1. / 36},
        {"Translate 0 0 3\nShape \"sphere\" \"float radius\" [.5] \"float phimax\" [180]", 1. / 72},
        {"Translate 0 0 3\nReverseOrientation\nShape \"disk\" \"float radius\" [.5]", 1. / 37},
        {"Translate 0 0 3\nReverseOrientation\nShape \"disk\" \"float radius\" [.5] \"float "
         "innerradius\" [.25] \"float phimax\" [180]",
         (.25 / 9.25 - .0625 / 9.0625) / 2}};
    for (const auto &c : cases) {
        auto scene = prepare(f, camera, receiver + emission + c.geometry, 1);
        // Three independent Philox seeds; fixed 0.003 absolute budget was chosen
        // before measurements, conservatively above variance at 16384 samples.
        for (unsigned seed : {7, 19, 41}) {
            const auto value = mean(scene, 16384, seed);
            std::cout << "analytic irradiance " << value << " expected " << c.expected << '\n';
            require(std::abs(value - c.expected) < .003, "analytic area sampling/MIS irradiance");
        }
    }
    // Reflection handedness is already present in the analytic cofactor normal.
    // Check both authored orientations from both sides; alpha-zero emitters
    // isolate direct sampling so BSDF continuation cannot rescue a wrong BVH cone.
    for (bool reverse : {false, true})
        for (bool above : {false, true})
            for (bool invisible : {false, true}) {
                const std::string oriented_receiver =
                    "AttributeBegin\nMaterial \"diffuse\" \"spectrum reflectance\" [360 1 830 "
                    "1]\n" +
                    std::string(above ? "Translate 0 0 6\nReverseOrientation\n" : "") +
                    "Shape \"disk\" \"float radius\" [10]\nAttributeEnd\n";
                const std::string reflected_source =
                    std::string("AttributeBegin\nMaterial \"diffuse\" \"spectrum reflectance\" "
                                "[360 0 830 0]\n") +
                    emission + "Translate 0 0 3\nScale -1 1 1\n" +
                    (reverse ? "ReverseOrientation\n" : "") +
                    "Shape \"disk\" \"float radius\" [.5]" +
                    (invisible ? " \"float alpha\" [0]" : "") + "\nAttributeEnd\n";
                const auto camera_side = above ? "LookAt 0 0 5  0 0 6  0 1 0\n" : camera;
                auto scene = prepare(f, camera_side, oriented_receiver + reflected_source, 1);
                const bool emitting_side = above != reverse;
                const double expected = emitting_side ? 1. / 37 : 0;
                for (unsigned seed : {7, 19, 41}) {
                    const auto value = mean(scene, 16384, seed);
                    std::cout << "reflected disk reverse=" << reverse << " above=" << above
                              << " invisible=" << invisible << " irradiance=" << value
                              << " expected=" << expected << '\n';
                    require(
                        emitting_side ? std::abs(value - expected) < .003 : value == 0,
                        "Reflected disk light bounds disagree with analytic emission orientation");
                }
            }
    // Independent midpoint integration of a planar triangle's geometric
    // Lambertian term; retained zero faces must act as null proposal slots.
    double triangle_reference = 0;
    constexpr unsigned grid = 256;
    for (unsigned i = 0; i < grid; ++i)
        for (unsigned j = 0; j < grid; ++j) {
            const double u = (i + .5) / grid, v = (j + .5) / grid;
            const double x = -.5 + .5 * u + (1 - u) * v, y = -.5 + u;
            const double squared = 9 + x * x + y * y;
            triangle_reference +=
                9 * (1 - u) / (std::numbers::pi * squared * squared * grid * grid);
        }
    for (const auto &indices : {"0 1 2", "0 0 0 0 1 2"}) {
        auto triangle = prepare(f, camera,
                                receiver + emission +
                                    "Scale -1 1 1\nShape \"trianglemesh\" \"point3 P\" [-.5 -.5 3 "
                                    "0 .5 3 .5 -.5 3] \"integer indices\" [" +
                                    indices + "]",
                                1);
        for (unsigned seed : {7, 19, 41}) {
            const auto value = mean(triangle, 16384, seed);
            std::cout << "retained triangle emitter " << value << " expected " << triangle_reference
                      << '\n';
            require(std::abs(value - triangle_reference) < .002,
                    "zero-measure emitter slot preserves MIS selection density");
        }
    }
    // Exact y=3x collinearity, despite a nonzero binary32 edge cross.
    // An emitter containing only this retained slot must contribute exactly zero.
    auto large_zero =
        prepare(f, camera,
                receiver + "AreaLightSource \"diffuse\" \"spectrum L\" [360 1 830 1] "
                           "\"bool twosided\" [true]\nTranslate 0 0 3\n"
                           "Shape \"trianglemesh\" \"point3 P\" [33554432 100663296 0 "
                           "3 9 0 0 0 0] \"integer indices\" [0 1 2]",
                1);
    for (unsigned seed : {7, 19, 41})
        require(mean(large_zero, 1024, seed) == 0,
                "large-coordinate canonical zero emitter is a null sampling slot");
    auto cavity =
        prepare(f, "",
                std::string("Material \"diffuse\" \"spectrum reflectance\" [360 .5 830 .5]\n") +
                    emission + "ReverseOrientation\nShape \"sphere\" \"float radius\" [3]",
                1);
    for (unsigned seed : {7, 19, 41}) {
        const auto value = mean(cavity, 16384, seed);
        std::cout << "analytic cavity " << value << " expected 1.5\n";
        require(std::abs(value - 1.5) < .03, "same analytic primitive self-lighting/MIS");
    }
    std::cout << "Native analytic render integration passed\n";
    return 0;
}
} // namespace
int main() {
    try {
        return run();
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
