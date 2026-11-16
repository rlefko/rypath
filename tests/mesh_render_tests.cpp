// SPDX-License-Identifier: GPL-3.0-only
#include "checkpoint.hpp"
#include "metal_render.hpp"
#include "pbrt_render.hpp"
#include "render_identity.hpp"
#include <algorithm>
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
void require(bool condition, const std::string &message) {
    if (!condition)
        throw std::runtime_error(message);
}
template <typename T> const T &checked_value(const std::optional<T> &value) {
    if (!value)
        throw std::runtime_error("Expected an owned optional result");
    return *value;
}
struct Scratch {
    fs::path dir = fs::temp_directory_path() / ("rypath-mesh-render-" + std::to_string(getpid()));
    Scratch() { fs::create_directories(dir); }
    ~Scratch() { fs::remove_all(dir); }
};
RenderScene translate(const Scratch &tmp, const std::string &body) {
    const auto path = tmp.dir / "scene.pbrt";
    std::ofstream(path)
        << "Camera \"perspective\" \"float fov\" [20]\nFilm \"rgb\"\nPixelFilter \"box\"\n"
           "Sampler \"independent\"\nIntegrator \"path\" \"integer maxdepth\" [1]\n"
           "WorldBegin\n"
        << body << '\n';
    const auto checked = pbrt::check(path);
    require(checked.exit_code() == 0, pbrt::report(checked));
    auto result = pbrt::translate(checked, pbrt::Location{path.string()});
    require(result.scene.has_value(), "Mesh translation failed");
    return checked_value(result.scene);
}
constexpr auto emission = "AreaLightSource \"diffuse\" \"spectrum L\" [360 1 830 1]\n";
constexpr auto quad = "Shape \"bilinearmesh\" \"point3 P\" [-2 -2 3 2 -2 3 -2 2 3 2 2 3]\n";
constexpr auto loop = "Shape \"loopsubdiv\" \"point3 P\" [-6 -6 3 18 -6 3 -6 18 3] "
                      "\"integer indices\" [0 1 2] \"integer levels\" [0]\n";
std::vector<RenderSample> sample(const RenderScene &scene, RenderSampler sampler = {}) {
    auto built = build_metal_renderer(scene);
    require(built.status == GeometryStatus::success && built.scene, built.diagnostic);
    auto result = built.scene->sample({.width = 2,
                                       .height = 2,
                                       .seed = 17,
                                       .pixel_count = 4,
                                       .sample_count = 256,
                                       .total_samples = 256,
                                       .threadgroup_width = 32,
                                       .sampler = sampler});
    require(result.status == GeometryStatus::success && result.samples.size() == 1024 &&
                result.guard_count > 0,
            result.diagnostic);
    for (std::size_t i = 0; i < result.samples.size(); ++i)
        require(result.samples[i].pixel == i % 4 && result.samples[i].sample == i / 4,
                "Mesh dispatch lost absolute sample indexing");
    return result.samples;
}
void compare(const RenderScene &actual, const RenderScene &reference, RenderSampler sampler = {}) {
    const auto a = sample(actual, sampler);
    const auto b = sample(reference, sampler);
    double energy = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        require(a[i].pixel == b[i].pixel && a[i].sample == b[i].sample &&
                    a[i].wavelength == b[i].wavelength && a[i].random_words == b[i].random_words,
                "Mesh translation changed sampling identities");
        require(std::isfinite(a[i].radiance) && a[i].radiance >= 0 &&
                    std::abs(a[i].radiance - b[i].radiance) <=
                        2e-5F * std::max(1.F, std::abs(b[i].radiance)),
                "Translated planar mesh differs from independent triangle reference: sample " +
                    std::to_string(i) + " actual=" + std::to_string(a[i].radiance) +
                    " reference=" + std::to_string(b[i].radiance) +
                    " triangles=" + std::to_string(actual.geometry.meshes[0].triangles.size()) +
                    " sampler=" + std::to_string(static_cast<unsigned>(sampler.kind)));
        energy += a[i].radiance;
    }
    require(energy > 0, "Geometry comparison exercised no illumination");
}
void emission_and_orientation(const Scratch &tmp) {
    for (bool subdivided : {false, true}) {
        auto scene =
            translate(tmp, std::string(emission) + (subdivided ? "" : "ReverseOrientation\n") +
                               (subdivided ? loop : quad));
        scene.max_depth = 0;
        auto reference = scene;
        // Independently evaluated level-zero boundary limit mask: .6*p + .2*(a+b).
        reference.geometry.meshes[0] =
            subdivided
                ? GeometryMesh{.positions = {{-1.2F, -1.2F, 3}, {8.4F, -1.2F, 3}, {-1.2F, 8.4F, 3}},
                               .triangles = {{0, 1, 2}}}
                : GeometryMesh{.positions = {{-2, -2, 3}, {2, -2, 3}, {-2, 2, 3}, {2, 2, 3}},
                               .triangles = {{0, 1, 3}, {0, 3, 2}}};
        if (subdivided)
            reference.geometry.meshes[0].normals.assign(3, {0, 0, -1});
        for (auto kind : {SamplerKind::independent, SamplerKind::sobol})
            compare(scene, reference, {kind});
        if (subdivided) {
            // PBRT uses the generated shading-normal hemisphere for emission.
            for (auto &normal : scene.geometry.meshes[0].normals)
                normal.z = -normal.z;
        } else
            scene.instance_reverse_orientation[0] = 0;
        for (const auto &s : sample(scene))
            require(s.radiance == 0, "Back face emitted through a one-sided mesh light");
        if (subdivided)
            for (auto &normal : scene.geometry.meshes[0].normals)
                normal.z = -normal.z;
        scene.instance_reverse_orientation[0] = subdivided ? 1U : 0U;
        scene.geometry.instances[0].transform.values[0] = -1;
        if (!scene.instance_surface_transforms.empty())
            scene.instance_surface_transforms[0].shape[0] = -1;
        // ReverseOrientation XOR handedness keeps the reflected emitter facing the camera.
        require(std::ranges::any_of(sample(scene), [](const auto &s) { return s.radiance > 0; }),
                "Reflected one-sided mesh emitter lost its front side");
    }
}
void direct_lighting(const Scratch &tmp) {
    auto scene = translate(
        tmp, "Material \"diffuse\" \"spectrum reflectance\" [360 .5 830 .5]\nReverseOrientation\n" +
                 std::string(quad) +
                 "LightSource \"point\" \"point3 from\" [0 0 0] "
                 "\"spectrum I\" [360 4 830 4]\n");
    auto reference = scene;
    reference.geometry.meshes[0] = {.positions = {{-2, -2, 3}, {2, -2, 3}, {-2, 2, 3}, {2, 2, 3}},
                                    .triangles = {{0, 1, 3}, {0, 3, 2}}};
    for (auto kind : {SamplerKind::independent, SamplerKind::sobol})
        compare(scene, reference, {kind});
}
void area_lighting(const Scratch &tmp) {
    auto scene = translate(
        tmp, "AttributeBegin\nMaterial \"diffuse\" \"spectrum reflectance\" [360 .5 830 .5]\n"
             "ReverseOrientation\n" +
                 std::string(quad) + "AttributeEnd\n" + emission +
                 "Shape \"loopsubdiv\" \"point3 P\" [2 0 1 4 0 1 2 2 1] "
                 "\"integer indices\" [0 2 1] \"integer levels\" [0]\n");
    auto reference = scene;
    reference.geometry.meshes[1] = {.positions = {{2.4F, .4F, 1}, {3.2F, .4F, 1}, {2.4F, 1.2F, 1}},
                                    .triangles = {{0, 2, 1}}};
    reference.geometry.meshes[1].normals.assign(3, {0, 0, 1});
    for (auto kind : {SamplerKind::independent, SamplerKind::sobol})
        compare(scene, reference, {kind});
}

void continuation(const Scratch &tmp) {
    for (bool subdivided : {false, true}) {
        auto scene =
            translate(tmp, std::string(emission) + (subdivided ? "" : "ReverseOrientation\n") +
                               (subdivided ? loop : quad));
        for (auto kind : {SamplerKind::independent, SamplerKind::sobol}) {
            RenderSettings settings{
                .width = 2, .height = 2, .samples = 32, .batch_size = 1, .seed = 17};
            settings.sampler.kind = kind;
            const auto whole = render_progressive(scene, settings);
            require(whole.status == GeometryStatus::success, whole.diagnostic);
            settings.batch_size = 7;
            require(render_identity(scene, settings) ==
                        render_identity(scene, RenderSettings{.width = 2,
                                                              .height = 2,
                                                              .samples = 32,
                                                              .batch_size = 1,
                                                              .seed = 17,
                                                              .sampler = settings.sampler}),
                    "Batch partition changed mesh identity");
            std::uint64_t completed = 0;
            RenderControl control;
            control.progress = [&](auto count) { completed = count; };
            control.cancelled = [&] { return completed >= 7; };
            const auto partial = render_progressive(scene, settings, control);
            require(partial.status == GeometryStatus::success &&
                        partial.completion == RenderCompletion::cancelled,
                    partial.diagnostic);
            RenderCheckpoint checkpoint;
            checkpoint.film = partial.film;
            checkpoint.settings = settings;
            checkpoint.identity = render_identity(scene, settings);
            checkpoint.segments.push_back(
                {0, partial.film.pixels.front().count, "mesh-test", "Metal", "native", "macOS"});
            const auto path = tmp.dir / "mesh.checkpoint";
            require(write_checkpoint(checkpoint, path).empty(), "Mesh checkpoint write failed");
            const auto saved = read_checkpoint(path);
            require(saved.value && checked_value(saved.value).identity == checkpoint.identity,
                    "Mesh checkpoint identity failed round trip");
            RenderControl resume;
            resume.initial_film = &checked_value(saved.value).film;
            const auto resumed = render_progressive(scene, settings, resume);
            require(resumed.status == GeometryStatus::success && resumed.film == whole.film,
                    "Mesh checkpoint continuation changed raw accumulation");
            auto changed = scene;
            changed.geometry.meshes[0].positions[0].x += .125F;
            require(render_identity(changed, settings) != checkpoint.identity,
                    "Mesh vertex edit did not invalidate checkpoint identity");
        }
    }
}
} // namespace
int main(int argc, char **argv) try {
    require(argc >= 2 && std::string_view(argv[1]) == "--metal", "Expected --metal");
    bool no_device = false, no_ray_tracing = false;
    for (int i = 2; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        require(arg == "--allow-no-device" || arg == "--allow-no-ray-tracing", "Unknown option");
        no_device |= arg == "--allow-no-device";
        no_ray_tracing |= arg == "--allow-no-ray-tracing";
    }
    Scratch tmp;
    const auto probe = build_metal_renderer(translate(tmp, std::string(emission) + quad));
    if ((probe.status == GeometryStatus::no_device && no_device) ||
        (probe.status == GeometryStatus::no_ray_tracing && no_ray_tracing)) {
        std::cout << "SKIP Metal mesh rendering. " << probe.diagnostic << '\n';
        return 77;
    }
    require(probe.status == GeometryStatus::success && probe.scene, probe.diagnostic);
    std::cout << "emission_and_orientation\n";
    emission_and_orientation(tmp);
    std::cout << "direct_lighting\n";
    direct_lighting(tmp);
    std::cout << "area_lighting\n";
    area_lighting(tmp);
    std::cout << "continuation\n";
    continuation(tmp);
} catch (const std::exception &e) {
    std::cerr << "Mesh render test failure: " << e.what() << '\n';
    return 1;
}
