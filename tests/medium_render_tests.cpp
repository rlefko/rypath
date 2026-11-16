// SPDX-License-Identifier: GPL-3.0-only
#include "metal_render.hpp"
#include "pbrt_render.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <string_view>

using namespace rypath;
namespace {
void require(bool value, std::string_view reason) {
    if (!value)
        throw std::runtime_error(std::string(reason));
}
void near(double actual, double expected, double tolerance, std::string_view label) {
    require(std::abs(actual - expected) <= tolerance,
            std::string(label) + ": observed " + std::to_string(actual) + ", expected " +
                std::to_string(expected) + ", tolerance " + std::to_string(tolerance));
}
// Conventional absorption/scattering/source/asymmetry order used by these test fixtures.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
HomogeneousMedium medium(float a, float s, float emission = 0, float g = 0) {
    HomogeneousMedium m;
    m.sigma_a.fill(a);
    m.sigma_s.fill(s);
    m.emission.fill(emission);
    m.g = g;
    return m;
}
void plane(RenderScene &scene, float z, bool positive, MediumInterface interface = {},
           unsigned material = 0, bool reverse = false) {
    GeometryMesh mesh;
    mesh.positions = {{-16, -16, z}, {16, -16, z}, {16, 16, z}, {-16, 16, z}};
    mesh.triangles = positive ? std::vector<std::array<unsigned, 3>>{{0, 1, 2}, {0, 2, 3}}
                              : std::vector<std::array<unsigned, 3>>{{0, 2, 1}, {0, 3, 2}};
    const auto index = static_cast<unsigned>(scene.geometry.meshes.size());
    scene.geometry.meshes.push_back(std::move(mesh));
    scene.geometry.instances.push_back({index, {}});
    scene.instance_materials.push_back(material);
    scene.instance_media.push_back(interface);
    scene.instance_reverse_orientation.push_back(reverse ? 1u : 0u);
}
RenderScene base() {
    RenderScene scene;
    scene.model = RenderModel::pbrt;
    scene.volume_transport = true;
    scene.raster_camera = RasterCamera{{}, {-1e-7f, -1e-7f, 1}, {2e-7f, 0, 0}, {0, 2e-7f, 0}};
    SpectralMaterial boundary;
    boundary.kind = SpectralMaterialKind::constant;
    boundary.scattering = ScatteringKind::null_interface;
    scene.materials.push_back(boundary);
    scene.environment.fill(1);
    scene.max_depth = 1;
    return scene;
}
RenderRequest request(unsigned count = 32768) {
    return {.width = 1,
            .height = 1,
            .seed = 19317,
            .pixel_count = 1,
            .sample_count = count,
            .total_samples = count,
            .threadgroup_width = 32};
}
std::unique_ptr<MetalRenderScene> build(const RenderScene &scene) {
    auto result = build_metal_renderer(scene);
    require(result.status == GeometryStatus::success && result.scene, result.diagnostic);
    return std::move(result.scene);
}
std::vector<RenderSample> sample(MetalRenderScene &renderer, const RenderRequest &query) {
    auto result = renderer.sample(query);
    require(result.status == GeometryStatus::success, result.diagnostic);
    require(result.guard_count > 0 &&
                result.samples.size() == std::size_t(query.pixel_count) * query.sample_count,
            "Medium sample accounting");
    for (const auto &s : result.samples)
        require(std::isfinite(s.radiance) && s.radiance >= 0, "Invalid medium raw radiance");
    return std::move(result.samples);
}
template <typename Expected>
void mean_gate(const std::vector<RenderSample> &paths, Expected expected, std::string_view label,
               double numerical = 2e-5) {
    // Independent Philox paths only. Numerical tolerance and six-SE gate are
    // declared before observing samples. Wavelength-dependent oracles compare
    // paired residuals so wavelength variation cannot hide transport bias.
    double sum = 0, square = 0;
    for (const auto &s : paths) {
        const double residual = s.radiance - expected(s);
        sum += residual;
        square += residual * residual;
    }
    const double n = static_cast<double>(paths.size()), mean = sum / n;
    const double variance = std::max(0., (square - sum * mean) / (n - 1));
    near(mean, 0, 6 * std::sqrt(variance / n) + numerical, label);
}
void slabs() {
    auto scene = base();
    scene.media = {medium(.6f, 0, 2)};
    plane(scene, 1, false, {0, no_medium});
    plane(scene, 2, true, {0, no_medium});
    mean_gate(
        sample(*build(scene), request()), [](const auto &) { return 2 - std::exp(-.6); },
        "Beer plus analytic slab emission");
    // At depth zero the volume source is excluded, but attenuation still acts
    // before an environment endpoint can contribute.
    scene.max_depth = 0;
    mean_gate(
        sample(*build(scene), request()), [](const auto &) { return std::exp(-.6); },
        "Depth-zero Beer transmission");
    scene.max_depth = 1;
    scene.media[0].emission.fill(0);
    for (unsigned i = 0; i < 471; ++i)
        scene.media[0].sigma_a[i] = .1f + .8f * static_cast<float>(i) / 470;
    mean_gate(
        sample(*build(scene), request()),
        [&](const auto &s) {
            const auto i = std::clamp(std::lround(s.wavelength) - 360L, 0L, 470L);
            return std::exp(-double(scene.media[0].sigma_a[static_cast<std::size_t>(i)]));
        },
        "Wavelength-dependent Beer transmission");
}
void bounded_fog_regression() {
    const auto path = std::string(RYPATH_TEST_SOURCE_DIR) +
                      "/fixtures/pbrt-render/bounded-fog/point-boundary.pbrt";
    const auto checked = pbrt::check(path);
    auto translated = pbrt::translate(checked, pbrt::Location{path});
    if (!translated.scene)
        throw std::runtime_error("Translate bounded fog regression");
    auto renderer = build(*translated.scene);
    // Actual native closest-hit/visibility path: the parent rejects this valid
    // near-tangent connection after reconstructing a null hit off its ray.
    const auto paths = sample(*renderer, {.width = 768,
                                          .height = 512,
                                          .seed = 17,
                                          .pixel_begin = 45101,
                                          .pixel_count = 1,
                                          .sample_begin = 41,
                                          .sample_count = 1,
                                          .total_samples = 512,
                                          .sampler = translated.settings.sampler});
    require(paths.front().pixel == 45101 && paths.front().sample == 41,
            "Bounded fog regression sample identity");
}
void grazing_beer() {
    // x=y follows the shared diagonal of each quad. Shallow incidence tests
    // both sides, including translated and nonuniformly scaled instances.
    for (bool reverse : {false, true}) {
        for (bool transformed : {false, true}) {
            auto scene = base();
            scene.media = {medium(.6f, 0)};
            scene.max_depth = 0;
            plane(scene, .001f, false, {0, no_medium});
            plane(scene, .002f, true, {0, no_medium});
            const float z_scale = transformed ? 2.f : 1.f;
            const float translation = transformed ? .25f : 0.f;
            for (auto &instance : scene.geometry.instances) {
                instance.transform.values[0] = transformed ? 2.f : 1.f;
                instance.transform.values[5] = transformed ? 2.f : 1.f;
                instance.transform.values[10] = z_scale;
                instance.transform.values[11] = translation;
            }
            const float low = translation + z_scale * .001f;
            const float high = translation + z_scale * .002f;
            scene.raster_camera =
                RasterCamera{{0, 0, reverse ? high + z_scale * .001f : translation},
                             {1, 1, reverse ? -.001f : .001f},
                             {1e-10f, 0, 0},
                             {0, 1e-10f, 0}};
            // Independent geometric chord length; account for binary32 authored
            // plane placement without borrowing any renderer offset arithmetic.
            const double chord = double(high - low) * std::sqrt(2. + .001 * .001) / .001;
            mean_gate(
                sample(*build(scene), request(16384)),
                [=](const auto &) { return std::exp(-.6 * chord); },
                "Grazing shared-edge Beer chord");
        }
    }
}
void finite_point_endpoint() {
    auto scene = base();
    scene.environment.fill(0);
    scene.media = {medium(.4f, 0)};
    SpectralMaterial diffuse;
    diffuse.kind = SpectralMaterialKind::constant;
    diffuse.scattering = ScatteringKind::diffuse;
    diffuse.constant = .5f;
    scene.materials.push_back(diffuse);
    plane(scene, 1, false, {}, 1);
    plane(scene, .25f, false, {0, no_medium});
    plane(scene, .5f, true, {0, no_medium});
    PointLight light;
    light.position = {0, 0, 0};
    light.intensity.fill(2);
    scene.point_lights.push_back(light);
    // Opaque geometry just beyond the point light must never enter the finite
    // connection after a null crossing. It is behind the camera as well.
    plane(scene, -5e-6f, true, {}, 1);
    // Camera and shadow each traverse .25 units of absorber; distance and cosine
    // to the unit-distance point light are one. Lambert's BRDF is .5/pi.
    mean_gate(
        sample(*build(scene), request()),
        [](const auto &) { return std::exp(-.4 * .5) / std::numbers::pi; },
        "Finite point endpoint and two Beer segments");
}
void oblique_point_endpoint() {
    auto scene = base();
    scene.environment.fill(0);
    scene.media = {medium(0, 0)};
    SpectralMaterial diffuse;
    diffuse.kind = SpectralMaterialKind::constant;
    diffuse.scattering = ScatteringKind::diffuse;
    diffuse.constant = .5f;
    scene.materials.push_back(diffuse);
    plane(scene, 1, false, {}, 1);
    // x+z=.5. Large tangential extent makes the conservative normal offset
    // resolvable independently of binary32 endpoint rounding. The offset has a
    // transverse component relative to the oblique connection to (1,0,-1).
    GeometryMesh boundary;
    boundary.positions = {
        {-128, -128, 128.5f}, {128, -128, -127.5f}, {128, 128, -127.5f}, {-128, 128, 128.5f}};
    boundary.triangles = {{0, 1, 2}, {0, 2, 3}};
    const auto index = static_cast<unsigned>(scene.geometry.meshes.size());
    scene.geometry.meshes.push_back(std::move(boundary));
    scene.geometry.instances.push_back({index, {}});
    scene.instance_materials.push_back(0);
    scene.instance_media.push_back({});
    scene.instance_reverse_orientation.push_back(0);
    PointLight light;
    light.position = {1, 0, -1};
    light.intensity.fill(2);
    scene.point_lights.push_back(light);
    const auto unobstructed = sample(*build(scene), request(64));
    // The true endpoint is x=1. The ideal receiver connection reaches x=.995
    // at z=-.99; the camera offset moves that intersection further right.
    // Subtracting projected progress while keeping the old direction instead
    // translates that endpoint left through this blocker (x<=.993).
    GeometryMesh blocker;
    blocker.positions = {{-2, -2, -.99f}, {.993f, -2, -.99f}, {.993f, 2, -.99f}, {-2, 2, -.99f}};
    blocker.triangles = {{0, 1, 2}, {0, 2, 3}};
    scene.geometry.meshes.push_back(std::move(blocker));
    scene.geometry.instances.push_back({index + 1, {}});
    scene.instance_materials.push_back(1);
    scene.instance_media.push_back({});
    scene.instance_reverse_orientation.push_back(0);
    const auto protected_endpoint = sample(*build(scene), request(64));
    for (std::size_t i = 0; i < unobstructed.size(); ++i) {
        require(unobstructed[i].radiance > .01f, "Oblique point oracle is illuminated");
        near(protected_endpoint[i].radiance, unobstructed[i].radiance, 0,
             "Oblique finite endpoint remains clear of adjacent blocker");
    }
}
void infinite_emission() {
    for (bool empty : {false, true}) {
        auto scene = base();
        // A distant null plane supplies valid native geometry without changing the
        // infinite medium; its identical-sided interface retains the ray medium.
        if (!empty)
            plane(scene, -100000, true);
        scene.camera_medium = 0;
        scene.environment.fill(0);
        scene.media = {medium(.4f, .6f, 2, .65f)};
        for (unsigned depth : {0u, 1u, 2u, 3u}) {
            scene.max_depth = depth;
            const double expected = 2 * (1 - std::pow(.6, depth));
            const auto paths = sample(*build(scene), request(4096));
            mean_gate(
                paths, [=](const auto &) { return expected; }, "Infinite source finite depth");
        }
        // Le alone does not emit when sigma_a is zero under PBRT's convention.
        scene.media = {medium(0, .7f, 4)};
        scene.max_depth = 2;
        for (const auto &s : sample(*build(scene), request(256)))
            near(s.radiance, 0, 0, "Pure scattering has no absorption-coupled emission");
        scene.media = {medium(.7f, 0)};
        scene.environment.fill(3);
        for (const auto &s : sample(*build(scene), request(256)))
            near(s.radiance, 0, 0, "Infinite positive extinction cannot escape to environment");
        scene.media = {medium(0, 0, 4)};
        scene.environment.fill(3);
        for (const auto &s : sample(*build(scene), request(256)))
            near(s.radiance, 3, 0, "Infinite zero-extinction medium is transparent");
    }
}

void nested_and_orientation() {
    auto scene = base();
    scene.media = {medium(.2f, 0), medium(.8f, 0)};
    plane(scene, 1, false, {0, no_medium});
    plane(scene, 1.5f, false, {1, 0});
    plane(scene, 2.5f, true, {1, 0});
    plane(scene, 3, true, {0, no_medium});
    mean_gate(
        sample(*build(scene), request()), [](const auto &) { return std::exp(-1.); },
        "Nested explicit medium sides");
    // Reversing geometric orientation and swapping the authored sides must
    // preserve physical regions, without relying on a face-forward normal.
    std::fill(scene.instance_reverse_orientation.begin(), scene.instance_reverse_orientation.end(),
              1);
    for (auto &sides : scene.instance_media)
        std::swap(sides.inside, sides.outside);
    mean_gate(
        sample(*build(scene), request()), [](const auto &) { return std::exp(-1.); },
        "Reversed medium orientation");
    // Mirroring placement flips triangle winding. Keep the swapped interface
    // sides and remove ReverseOrientation to preserve the same physical slab.
    std::fill(scene.instance_reverse_orientation.begin(), scene.instance_reverse_orientation.end(),
              0);
    for (auto &instance : scene.geometry.instances)
        instance.transform.values[0] = -1;
    mean_gate(
        sample(*build(scene), request()), [](const auto &) { return std::exp(-1.); },
        "Mirrored placement medium orientation");
    scene = base();
    scene.media = {medium(1, 0)};
    plane(scene, 1, false, {0, no_medium});
    plane(scene, 2, true, {0, no_medium});
    // Eight equal segments strongly expose accidental reuse of one flight
    // variate per bounce: that error approaches exp(-1/8), rather than exp(-1).
    for (unsigned i = 1; i < 8; ++i)
        plane(scene, 1 + static_cast<float>(i) / 8, true);
    scene.max_depth = 0;
    mean_gate(
        sample(*build(scene), request()), [](const auto &) { return std::exp(-1.); },
        "Depth-neutral split null interfaces");
}
double single_scatter_slab(double g) {
    // Independent deterministic quadrature of an infinite lateral slab z=[-1,1].
    // Camera at its center looks along +z. A first collision at t has density
    // sigma_s exp(-t); HG uses both-away directions, hence cosTheta=-mu.
    constexpr unsigned n = 800;
    double integral = 0;
    for (unsigned i = 0; i < n; ++i) {
        const double t = (double(i) + .5) / n;
        double angular = 0;
        for (unsigned j = 0; j < n; ++j) {
            const double mu = -1 + 2 * (double(j) + .5) / n;
            const double distance = mu > 0 ? (1 - t) / mu : (-1 - t) / mu;
            const double denom = 1 + g * g - 2 * g * mu;
            const double phase_times_2pi = .5 * (1 - g * g) / std::pow(denom, 1.5);
            angular += phase_times_2pi * std::exp(-distance) * 2 / n;
        }
        integral += .7 * std::exp(-t) * angular / n;
    }
    return std::exp(-1.) + integral;
}
void phase_light_mis() {
    for (const float g : {0.f, .65f, -.55f}) {
        auto scene = base();
        scene.media = {medium(.3f, .7f, 0, g)};
        scene.camera_medium = 0;
        plane(scene, -1, false, {0, no_medium});
        plane(scene, 1, true, {0, no_medium});
        const double expected = single_scatter_slab(g);
        mean_gate(
            sample(*build(scene), request()), [=](const auto &) { return expected; },
            "Phase/light MIS versus independent single-scatter slab quadrature", 4e-5);
    }
}
void glass_boundary() {
    auto scene = base();
    scene.media = {medium(1, 0, 1)};
    scene.environment.fill(0);
    scene.max_depth = 2;
    SpectralMaterial glass;
    glass.kind = SpectralMaterialKind::constant;
    glass.scattering = ScatteringKind::dielectric;
    glass.eta.knots = {{360, 1.5f}, {830, 1.5f}};
    scene.materials.push_back(glass);
    plane(scene, 1, false, {0, no_medium}, 1);
    scene.instance_surface_transforms.emplace_back();
    scene.instance_texture_frames.resize(scene.geometry.instances.size());
    require(prepare_texture_triangle_frames(
                scene.geometry.meshes[0], scene.instance_surface_transforms[0].shape,
                scene.instance_surface_transforms[0].placement, scene.instance_texture_frames[0])
                .empty(),
            "Prepare glass interface frame");
    // Reflected paths escape to black vacuum; transmitted paths enter the
    // infinite emitting absorber with a radiance eta^-2 weight.
    mean_gate(
        sample(*build(scene), request()), [](const auto &) { return .96 / 2.25; },
        "Glass reflection/transmission chooses the correct medium");
}
void partitions() {
    auto scene = base();
    scene.media = {medium(.2f, .8f, .4f, .6f)};
    plane(scene, 1, false, {0, no_medium});
    plane(scene, 2, true, {0, no_medium});
    plane(scene, 1.5f, true);
    scene.max_depth = 3;
    auto renderer = build(scene);
    constexpr std::array samplers{
        RenderSampler{}, RenderSampler{SamplerKind::sobol, SobolRandomization::none},
        RenderSampler{SamplerKind::sobol, SobolRandomization::permutedigits},
        RenderSampler{SamplerKind::sobol, SobolRandomization::fastowen},
        RenderSampler{SamplerKind::sobol, SobolRandomization::owen}};
    for (const auto sampler : samplers) {
        auto query = request(73);
        query.width = query.height = 2;
        query.pixel_count = 4;
        query.sampler = sampler;
        const auto whole = sample(*renderer, query);
        for (unsigned pixel = 0; pixel < 4; pixel += 2) {
            query.pixel_begin = pixel;
            query.pixel_count = 2;
            for (unsigned begin : {0u, 31u}) {
                query.sample_begin = begin;
                query.sample_count = begin == 0 ? 31 : 42;
                query.threadgroup_width = begin == 0 ? 1 : 64;
                for (const auto &a : sample(*renderer, query)) {
                    const auto b = std::ranges::find_if(whole, [&](const auto &s) {
                        return s.pixel == a.pixel && s.sample == a.sample;
                    });
                    require(b != whole.end() && a.wavelength == b->wavelength &&
                                a.radiance == b->radiance && a.scatterings == b->scatterings &&
                                a.random_words == b->random_words,
                            "Medium stream changed across pixel/sample/threadgroup partitions");
                }
            }
        }
    }
}
void surface_stream() {
    auto scene = base();
    scene.materials[0].scattering = ScatteringKind::diffuse;
    scene.materials[0].constant = .6f;
    plane(scene, 1, false);
    scene.max_depth = 3;
    scene.volume_transport = false;
    auto original = build(scene);
    scene.volume_transport = true;
    scene.camera_medium = 0;
    scene.media = {medium(0, 0)};
    auto transparent = build(scene);
    for (const auto sampler :
         {RenderSampler{}, RenderSampler{SamplerKind::sobol, SobolRandomization::fastowen}}) {
        auto query = request(257);
        query.sampler = sampler;
        const auto a = sample(*original, query), b = sample(*transparent, query);
        for (std::size_t i = 0; i < a.size(); ++i)
            require(a[i].radiance == b[i].radiance && a[i].wavelength == b[i].wavelength &&
                        a[i].scatterings == b[i].scatterings &&
                        a[i].random_words == b[i].random_words,
                    "Zero extinction changed the existing surface sampling stream");
    }
}
void failures() {
    auto scene = base();
    scene.media = {medium(0, 0)};
    plane(scene, 1, true);
    scene.camera_medium = 1;
    require(!validate_render_scene(scene).empty(), "Invalid camera medium accepted");
    scene.camera_medium = 0;
    scene.media[0].sigma_a[5] = std::numeric_limits<float>::infinity();
    require(!validate_render_scene(scene).empty(), "Nonfinite extinction accepted");
    scene.media[0] = medium(0, 0);
    scene.instance_media[0].inside = 8;
    require(!validate_render_scene(scene).empty(), "Invalid interface medium accepted");
    scene.instance_media[0] = {};
    // Failure is a failed batch, never a replacement black path or successful
    // partial Film. This deliberately crosses the declared null safety limit.
    for (unsigned i = 1; i <= medium_boundary_limit; ++i)
        plane(scene, 1 + .01f * static_cast<float>(i), true);
    const auto failed = render_progressive(
        scene, {.width = 1, .height = 1, .samples = 1, .batch_size = 1, .seed = 42});
    require(failed.status == GeometryStatus::failure, "Null boundary overflow did not fail");
    for (const auto &pixel : failed.film.pixels)
        require(pixel.count == 0, "Failed medium batch committed samples to Film");
}
} // namespace
int main(int argc, char **argv) {
    try {
        bool allow_no_device = false, allow_no_ray_tracing = false;
        for (int i = 1; i < argc; ++i) {
            const std::string_view option(argv[i]);
            if (!option.starts_with("--"))
                continue; // Optional scratch path, matching other native test drivers.
            require(option == "--allow-no-device" || option == "--allow-no-ray-tracing",
                    "Unknown medium render test option");
            allow_no_device |= option == "--allow-no-device";
            allow_no_ray_tracing |= option == "--allow-no-ray-tracing";
        }
        auto scene = base();
        scene.media = {medium(0, 0)};
        plane(scene, 1, true);
        auto capability = build_metal_renderer(scene);
        if ((allow_no_device && capability.status == GeometryStatus::no_device) ||
            (allow_no_ray_tracing && capability.status == GeometryStatus::no_ray_tracing)) {
            std::cout << "SKIP Metal media: " << capability.diagnostic << '\n';
            return 77;
        }
        require(capability.status == GeometryStatus::success && capability.scene,
                capability.diagnostic);
        bounded_fog_regression();
        grazing_beer();
        finite_point_endpoint();
        oblique_point_endpoint();
        slabs();
        infinite_emission();
        nested_and_orientation();
        phase_light_mis();
        glass_boundary();
        partitions();
        surface_stream();
        failures();
        std::cout
            << "PASS bounded fog, grazing Beer, finite point endpoints, Beer/emission, infinite "
               "source/depth, nested/split/oriented media, "
               "independent phase/MIS quadrature, glass, sampler partitions and failed Film\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "Medium render: " << e.what() << '\n';
        return 1;
    }
}
