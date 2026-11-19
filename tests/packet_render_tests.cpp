// SPDX-License-Identifier: GPL-3.0-only
#include "bsdf_table.hpp"
#include "bsdf_table_fixture.hpp"
#include "metal_render.hpp"
#include "packet_wire.h"
#include "render_driver.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <numbers>
#include <stdexcept>
#include <string_view>
namespace {
using namespace rypath;
void require(bool value, std::string_view message) {
    if (!value)
        throw std::runtime_error(std::string(message));
}
void mesh(RenderScene &scene, std::vector<GeometryPoint> points, bool reverse = false,
          unsigned material = 0) {
    GeometryMesh mesh;
    mesh.positions = std::move(points);
    if (reverse)
        mesh.triangles = {{0, 1, 2}, {0, 2, 3}};
    else
        mesh.triangles = {{0, 2, 1}, {0, 3, 2}};
    const auto id = static_cast<unsigned>(scene.geometry.meshes.size());
    scene.geometry.meshes.push_back(std::move(mesh));
    scene.geometry.instances.push_back({id, {}});
    scene.instance_materials.push_back(material);
    scene.instance_surface_transforms.emplace_back();
}
RenderScene base(bool receiver = true) {
    RenderScene scene;
    scene.model = RenderModel::pbrt;
    scene.spectral_transport = SpectralTransport::packet;
    scene.max_depth = 1;
    scene.raster_camera = RasterCamera{{}, {-.3f, -.3f, 1}, {.6f, 0, 0}, {0, .6f, 0}};
    SpectralMaterial matte;
    matte.kind = SpectralMaterialKind::constant;
    matte.constant = .6f;
    scene.materials.push_back(matte);
    if (receiver)
        mesh(scene, {{-8, -8, 3}, {8, -8, 3}, {8, 8, 3}, {-8, 8, 3}});
    return scene;
}
RenderSamplesResult dispatch(MetalRenderScene &scene, RenderRequest request) {
    auto result = scene.sample(request);
    require(result.status == GeometryStatus::success, result.diagnostic);
    require(result.samples.empty() && result.packet_samples.size() == request.pixel_count &&
                result.guard_count > 0,
            "Packet native accounting/guard failure");
    for (const auto &owner : result.packet_samples) {
        require(owner.pixel >= request.pixel_begin &&
                    owner.pixel < request.pixel_begin + request.pixel_count &&
                    owner.sample == request.sample_begin,
                "Packet owner identity");
        for (unsigned i = 0; i < 4; ++i)
            require(std::isfinite(owner.radiance[i]) && owner.radiance[i] >= 0 &&
                        owner.wavelength_pdfs[i] >= 0,
                    "Nonfinite packet radiance/PDF");
        require(owner.splat_begin <= result.packet_splats.size() &&
                    owner.splat_count <= result.packet_splats.size() - owner.splat_begin,
                "Packet splat ownership");
    }
    for (const auto &s : result.packet_splats)
        for (auto x : s.radiance)
            require(std::isfinite(x) && x >= 0, "Nonfinite raw splat plane");
    return result;
}
bool same(const PacketSample &a, const PacketSample &b) {
    return a.pixel == b.pixel && a.sample == b.sample && a.wavelengths == b.wavelengths &&
           a.wavelength_pdfs == b.wavelength_pdfs && a.radiance == b.radiance &&
           a.filter_weight == b.filter_weight && a.splat_count == b.splat_count;
}
void scheduling() {
    auto scene = base();
    scene.environment.fill(1);
    scene.max_depth = 2;
    auto built = build_metal_renderer(scene);
    require(built.status == GeometryStatus::success && built.scene, built.diagnostic);
    constexpr std::array samplers{
        RenderSampler{}, RenderSampler{SamplerKind::sobol},
        RenderSampler{SamplerKind::sobol, SobolRandomization::none},
        RenderSampler{SamplerKind::halton, SobolRandomization::permutedigits},
        RenderSampler{SamplerKind::zsobol}};
    for (auto sampler : samplers) {
        RenderRequest query{.width = 4,
                            .height = 4,
                            .seed = 17,
                            .pixel_count = 16,
                            .sample_count = 1,
                            .total_samples = 8,
                            .threadgroup_width = 32,
                            .sampler = sampler};
        for (unsigned absolute : {0u, 3u, 7u}) {
            query.sample_begin = absolute;
            query.pixel_begin = 0;
            query.pixel_count = 16;
            query.threadgroup_width = 32;
            auto whole = dispatch(*built.scene, query);
            for (unsigned first : {0u, 7u}) {
                query.pixel_begin = first;
                query.pixel_count = first == 0 ? 7 : 9;
                query.threadgroup_width = first == 0 ? 1 : 16;
                auto part = dispatch(*built.scene, query);
                for (unsigned i = 0; i < part.packet_samples.size(); ++i)
                    require(same(part.packet_samples[i], whole.packet_samples[first + i]),
                            "Packet raw sample changed with dispatch/threadgroup partition");
            }
        }
    }
    auto guard_scene = scene;
    mesh(guard_scene,
         {{-10000, -10000, -3}, {10000, -10000, -3}, {10000, 10000, -3}, {-10000, 10000, -3}},
         true);
    auto guard_renderer = build_metal_renderer(guard_scene);
    require(guard_renderer.status == GeometryStatus::success && guard_renderer.scene,
            guard_renderer.diagnostic);
    RenderRequest guarded{.width = 4,
                          .height = 4,
                          .seed = 17,
                          .pixel_count = 16,
                          .sample_count = 1,
                          .total_samples = 1,
                          .emergency_limit = 1};
    auto stopped = guard_renderer.scene->sample(guarded);
    require(stopped.status == GeometryStatus::failure &&
                stopped.diagnostic.find("emergency depth reached") != std::string::npos,
            "Packet lower emergency guard silently truncated a path");
    scene.perspective_lens = PerspectiveLens{.02f, 3, {1, 0, 0}, {0, 1, 0}};
    auto lens = render_progressive(scene, RenderSettings{4, 4, 8, 3, 43});
    require(lens.completion == RenderCompletion::complete, lens.diagnostic);
    require(validate_film_accumulation(lens.film).empty(), "Packet lens raw Film validation");
    for (auto depth : {0u, 64u})
        for (auto integrator : {IntegratorKind::path, IntegratorKind::bdpt}) {
            auto empty = base(false);
            empty.environment.fill(1);
            empty.max_depth = depth;
            empty.integrator = integrator;
            const auto width = depth == 64 ? 1100u : 4u;
            auto rendered = render_progressive(empty, RenderSettings{width, 1, 1, 1, 19});
            require(rendered.completion == RenderCompletion::complete, rendered.diagnostic);
            require(validate_film_accumulation(rendered.film).empty(),
                    "Packet depth limit Film validation");
            for (const auto &p : rendered.film.pixels)
                require(p.count == 1, "Packet deep workspace dispatch lost source pixels");
        }
    for (auto integrator : {IntegratorKind::path, IntegratorKind::bdpt}) {
        auto emitter = base();
        emitter.max_depth = 0;
        emitter.integrator = integrator;
        emitter.area_emissions.emplace_back().radiance.fill(2);
        emitter.area_emissions.back().two_sided = true;
        emitter.instance_emissions = {0};
        auto visible = build_metal_renderer(emitter);
        require(visible.status == GeometryStatus::success && visible.scene, visible.diagnostic);
        RenderRequest q{.width = 4,
                        .height = 4,
                        .seed = 17,
                        .pixel_count = 16,
                        .sample_count = 1,
                        .total_samples = 1};
        const auto packets = dispatch(*visible.scene, q);
        for (const auto &p : packets.packet_samples)
            for (float r : p.radiance)
                require(r == 2, "Authored depth zero dropped directly visible area emission");
    }
}
RenderScene tiled_light_scene() {
    auto scene = base();
    scene.integrator = IntegratorKind::bdpt;
    // Keep the emitter outside the camera frustum so receiver splats are visible.
    mesh(scene, {{1.2f, -.7f, 1}, {2.6f, -.7f, 1}, {2.6f, .7f, 1}, {1.2f, .7f, 1}}, true);
    scene.area_emissions.emplace_back().radiance.fill(3);
    scene.instance_emissions = {no_area_emission, 0};
    return scene;
}
bool same_splats(const RenderSamplesResult &a, const PacketSample &owner_a,
                 const RenderSamplesResult &b, const PacketSample &owner_b) {
    if (owner_a.splat_count != owner_b.splat_count)
        return false;
    for (unsigned i = 0; i < owner_a.splat_count; ++i) {
        const auto &x = a.packet_splats[owner_a.splat_begin + i];
        const auto &y = b.packet_splats[owner_b.splat_begin + i];
        if (x.raster != y.raster || x.radiance != y.radiance || x.strategy != y.strategy)
            return false;
    }
    return true;
}
void halton_light_tiles() {
    const auto scene = tiled_light_scene();
    auto renderer = build_metal_renderer(scene);
    require(renderer.status == GeometryStatus::success && renderer.scene, renderer.diagnostic);
    RenderRequest query{.width = 257,
                        .height = 3,
                        .seed = 17,
                        .pixel_count = 771,
                        .sample_count = 1,
                        .threadgroup_width = 32,
                        .sampler = {SamplerKind::halton, SobolRandomization::permutedigits}};
    bool distinct_nonzero_splats = false;
    for (std::uint64_t absolute : {0ull, 3ull, (1ull << 32) - 1, 1ull << 32}) {
        query.sample_begin = absolute;
        query.pixel_begin = 0;
        query.pixel_count = 771;
        query.threadgroup_width = 32;
        const auto whole = dispatch(*renderer.scene, query);
        for (unsigned y = 0; y < 3; ++y)
            for (unsigned x = 0; x < 128; ++x) {
                const auto &a = whole.packet_samples[y * 257 + x];
                const auto &b = whole.packet_samples[y * 257 + x + 128];
                // The old CRT light index gives these owners identical light
                // paths and splats. Require actual positive strategy signal.
                if (a.splat_count && b.splat_count && !same_splats(whole, a, whole, b))
                    for (unsigned i = 0; i < a.splat_count; ++i)
                        distinct_nonzero_splats |=
                            std::ranges::any_of(whole.packet_splats[a.splat_begin + i].radiance,
                                                [](float value) { return value > 0; });
            }
        for (const auto range :
             {std::array{0u, 127u, 1u}, std::array{127u, 133u, 16u}, std::array{260u, 511u, 64u}}) {
            query.pixel_begin = range[0];
            query.pixel_count = range[1];
            query.threadgroup_width = range[2];
            const auto part = dispatch(*renderer.scene, query);
            for (unsigned i = 0; i < query.pixel_count; ++i) {
                const auto &a = part.packet_samples[i];
                const auto &b = whole.packet_samples[query.pixel_begin + i];
                require(same(a, b) && same_splats(part, a, whole, b),
                        "Tiled Halton BDPT owners/splats changed with partition or threadgroup");
            }
        }
    }
    require(distinct_nonzero_splats,
            "Halton BDPT light splats still repeat across 128-pixel tiles");

    // This absolute index fits the camera CRT range but not the two-slot light
    // range. Exercise scene-aware admission without requesting an enormous render.
    constexpr auto stride = std::uint64_t{128};
    constexpr auto camera_limit = (std::uint64_t{INT64_MAX} - (stride - 1)) / stride;
    constexpr auto rejected_sample = (camera_limit - 1) / 2 + 1;
    query.width = 129;
    query.height = 1;
    query.pixel_begin = 0;
    query.pixel_count = 1;
    query.sample_begin = rejected_sample;
    const auto rejected = renderer.scene->sample(query);
    require(rejected.status == GeometryStatus::failure && rejected.guard_count == 0 &&
                rejected.packet_samples.empty() && rejected.packet_splats.empty() &&
                rejected.diagnostic.find("Halton light") != std::string::npos,
            "Halton BDPT light overflow reached dispatch or lost its diagnostic");
    RenderSettings settings{.width = 129,
                            .height = 1,
                            .samples = rejected_sample + 1,
                            .batch_size = 1,
                            .seed = 17,
                            .sampler = query.sampler};
    bool prepared = false, sampled = false;
    detail::RenderSampleSource source;
    source.prepare = [&] {
        prepared = true;
        RenderBuildResult result;
        result.diagnostic = "test preparation sentinel";
        return result;
    };
    source.sample = [&](const RenderRequest &) {
        sampled = true;
        return RenderSamplesResult{};
    };
    const auto preflight = detail::drive_progressive(scene, settings, {}, source);
    require(!prepared && !sampled && preflight.completion == RenderCompletion::failed &&
                preflight.diagnostic.find("Halton light") != std::string::npos,
            "Halton BDPT light range validation occurred after preparation");
    const auto progressive = render_progressive(scene, settings);
    require(progressive.completion == RenderCompletion::failed && progressive.device.name.empty() &&
                progressive.diagnostic.find("Halton light") != std::string::npos,
            "Progressive Halton BDPT overflow acquired a GPU or lost its diagnostic");
    for (unsigned control = 0; control < 3; ++control) {
        auto unaffected = scene;
        auto request = query;
        if (control == 0)
            unaffected.max_depth = 0;
        else if (control == 1)
            unaffected.integrator = IntegratorKind::path;
        else
            request.sampler = {};
        auto built = build_metal_renderer(unaffected);
        require(built.status == GeometryStatus::success && built.scene, built.diagnostic);
        dispatch(*built.scene, request);
        auto unaffected_settings = settings;
        unaffected_settings.sampler = request.sampler;
        prepared = false;
        const auto admitted =
            detail::drive_progressive(unaffected, unaffected_settings, {}, source);
        require(prepared && !sampled && admitted.diagnostic == "test preparation sentinel",
                "Halton light bound affected path, depth zero or independent full targets");
    }
}
double mean_y(const Film &film) {
    double sum = 0;
    for (const auto &pixel : film.pixels)
        sum += film_mean(film, pixel)[1];
    return sum / static_cast<double>(film.pixels.size());
}
struct Moments {
    double sum = 0, square = 0;
    unsigned n = 0;
    void add(double value) {
        sum += value;
        square += value * value;
        ++n;
    }
    double mean() const { return sum / n; }
    double variance_mean() const {
        return n > 1 ? std::max(0., (square - sum * sum / n) / (n - 1)) / n : 0;
    }
};
void area_black_environment() {
    auto scene = base();
    // The emitter lies outside the camera frustum and faces the matte receiver.
    scene.materials.push_back(scene.materials[0]);
    scene.materials.back().constant = 0;
    mesh(scene, {{1.2f, -.7f, 1}, {2.6f, -.7f, 1}, {2.6f, .7f, 1}, {1.2f, .7f, 1}}, true, 1);
    scene.area_emissions.emplace_back().radiance.fill(3);
    scene.instance_emissions = {no_area_emission, 0};
    Moments differences, reference;
    bool splatted = false;
    // Prespecified paired 6-SE gate plus 2% relative numerical/model allowance.
    // Independent seeds measure estimator variation; equal wavelength packets
    // reduce spectral noise in each paired path/BDPT difference.
    for (unsigned seed = 0; seed < 8; ++seed) {
        RenderSettings settings{16, 16, 64, 16, 19317u + seed * 7919u};
        scene.integrator = IntegratorKind::path;
        auto path = render_progressive(scene, settings);
        require(path.completion == RenderCompletion::complete, path.diagnostic);
        scene.integrator = IntegratorKind::bdpt;
        auto bdpt = render_progressive(scene, settings);
        require(bdpt.completion == RenderCompletion::complete, bdpt.diagnostic);
        require(validate_film_accumulation(path.film).empty() &&
                    validate_film_accumulation(bdpt.film).empty(),
                "Area path/BDPT raw Film invalid");
        const double a = mean_y(path.film), b = mean_y(bdpt.film);
        require(a > 0 && b > 0, "Area with black environment lost direct illumination");
        differences.add(b - a);
        reference.add(a);
        splatted |=
            std::ranges::any_of(bdpt.film.pixels, [](const auto &p) { return p.splat_sum[1] > 0; });
    }
    require(splatted, "BDPT produced no camera splat strategy signal");
    const double tolerance = 6 * std::sqrt(differences.variance_mean()) + .02 * reference.mean();
    require(std::abs(differences.mean()) <= tolerance,
            "Area/black-environment path and BDPT expectations disagree");
    std::cout << "Area path mean Y " << reference.mean() << ", paired BDPT difference "
              << differences.mean() << ", tolerance " << tolerance << '\n';
}
int specialized_surface_family(ScatteringKind kind) {
    // Each pipeline enables just one new family. Bind only a mix root so a scan
    // of instance-bound materials would miss the child and disable its code.
    auto scene = base();
    scene.environment.fill(1);
    scene.max_depth = 2;
    auto child = scene.materials.front();
    child.scattering = kind;
    child.constant = kind == ScatteringKind::thin_dielectric ? 0 : .4f;
    if (kind == ScatteringKind::thin_dielectric)
        child.eta.knots = {{360, 1.5f}, {830, 1.5f}};
    if (kind == ScatteringKind::diffuse_transmission)
        child.surface.transmittance = .6f;
    if (kind == ScatteringKind::measured || kind == ScatteringKind::fourier) {
        const bool fourier = kind == ScatteringKind::fourier;
        const auto path =
            std::filesystem::temp_directory_path() /
            ("rypath-packet-family-" +
             std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
             (fourier ? ".bsdf" : ".tensor"));
        table_fixture::write(path, fourier ? table_fixture::fourier() : table_fixture::measured());
        const auto table =
            read_bsdf_table(path, fourier ? BsdfTableKind::fourier : BsdfTableKind::measured);
        std::filesystem::remove(path);
        require(table.table != nullptr, table.diagnostic);
        child.table = table.table;
    }
    scene.materials.push_back(child);
    auto &root = scene.materials.front();
    root.scattering = ScatteringKind::mix;
    root.surface.first_material = 1;
    root.surface.second_material = 1;
    scene.instance_texture_frames.emplace_back();
    const auto &transform = scene.instance_surface_transforms.front();
    require(prepare_texture_triangle_frames(scene.geometry.meshes.front(), transform.shape,
                                            transform.placement,
                                            scene.instance_texture_frames.front())
                .empty(),
            "Packet surface fixture frame preparation failed");
    require(validate_render_scene(scene).empty(), validate_render_scene(scene));
    auto built = build_metal_renderer(scene);
#if defined(RYPATH_ALLOW_NO_METAL_DEVICE)
    if (built.status == GeometryStatus::no_device) {
        std::cout << "SKIP packet native surface: no device\n";
        return 77;
    }
#endif
#if defined(RYPATH_ALLOW_NO_METAL_RAY_TRACING)
    if (built.status == GeometryStatus::no_ray_tracing) {
        std::cout << "SKIP packet native surface: no ray tracing\n";
        return 77;
    }
#endif
    require(built.status == GeometryStatus::success && built.scene, built.diagnostic);
    bool signal = false;
    Moments illumination;
    for (unsigned absolute = 0; absolute < 8; ++absolute) {
        RenderRequest query{.width = 4,
                            .height = 4,
                            .seed = 619,
                            .pixel_count = 16,
                            .sample_begin = absolute,
                            .sample_count = 1,
                            .total_samples = 8};
        const auto output = dispatch(*built.scene, query);
        for (const auto &sample : output.packet_samples) {
            double average = 0;
            for (float radiance : sample.radiance) {
                signal |= radiance > 0;
                average += radiance / 4;
                if (kind == ScatteringKind::thin_dielectric)
                    require(std::abs(radiance - 1) < 2e-5f,
                            "Specialized packet thin sheet lost unit illumination");
            }
            illumination.add(average);
        }
    }
    // R+T=1 preserves unit illumination in expectation. The two MIS
    // strategies fluctuate per path; packet lanes share their direction,
    // so use packet averages as the independent observations.
    if (kind == ScatteringKind::diffuse_transmission)
        require(std::abs(illumination.mean() - 1) <=
                    6 * std::sqrt(illumination.variance_mean()) + .002,
                "Specialized packet diffuse transmission lost unit illumination");
    require(signal, "Specialized packet table/mix child lost transported signal");
    return 0;
}
void point_identity() {
    auto scene = base();
    scene.raster_camera = RasterCamera{{}, {-.001f, -.001f, 1}, {.002f, 0, 0}, {0, .002f, 0}};
    PointLight point;
    point.position = {0, 0, 0};
    point.intensity.fill(2);
    scene.point_lights.push_back(point);
    auto renderer = build_metal_renderer(scene);
    require(renderer.status == GeometryStatus::success && renderer.scene, renderer.diagnostic);
    Moments radiance;
    for (unsigned absolute = 0; absolute < 32; ++absolute) {
        RenderRequest query{.width = 32,
                            .height = 32,
                            .seed = 713,
                            .pixel_count = 1024,
                            .sample_begin = absolute,
                            .sample_count = 1,
                            .total_samples = 32};
        auto output = dispatch(*renderer.scene, query);
        for (const auto &sample : output.packet_samples)
            radiance.add(sample.radiance[0]);
    }
    // rho I cos(theta)/(pi distance^2), with z=3 and negligible raster spread.
    const double expected = .6 * 2 / (9 * std::numbers::pi);
    const double tolerance = 6 * std::sqrt(radiance.variance_mean()) + expected * 1e-4;
    require(std::abs(radiance.mean() - expected) <= tolerance,
            "Packet point-light analytic radiance identity");
}
} // namespace
int main(int argc, char **argv) {
    try {
        if (argc != 1) {
            require(argc == 3 && std::string_view(argv[1]) == "--surface-family",
                    "Expected --surface-family FAMILY");
            constexpr std::array names{"thin_dielectric", "diffuse_transmission", "measured",
                                       "fourier"};
            constexpr std::array kinds{ScatteringKind::thin_dielectric,
                                       ScatteringKind::diffuse_transmission,
                                       ScatteringKind::measured, ScatteringKind::fourier};
            const auto found = std::ranges::find(names, std::string_view(argv[2]));
            require(found != names.end(), "Unknown packet surface family");
            const int status =
                specialized_surface_family(kinds[static_cast<std::size_t>(found - names.begin())]);
            if (status != 0)
                return status;
            std::cout << "PASS packet specialized family/mix child " << argv[2] << '\n';
            return 0;
        }
        const auto start = std::chrono::steady_clock::now();
        auto capability = build_metal_renderer(base());
        std::cout << "Packet first-scene setup seconds "
                  << std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count()
                  << std::endl;
#if defined(RYPATH_ALLOW_NO_METAL_DEVICE)
        if (capability.status == GeometryStatus::no_device) {
            std::cout << "SKIP packet native render: no device\n";
            return 77;
        }
#endif
#if defined(RYPATH_ALLOW_NO_METAL_RAY_TRACING)
        if (capability.status == GeometryStatus::no_ray_tracing) {
            std::cout << "SKIP packet native render: no ray tracing\n";
            return 77;
        }
#endif
        require(capability.status == GeometryStatus::success && capability.scene,
                capability.diagnostic);
        std::cout << "Packet native: scheduling/samplers/lens/limits" << std::endl;
        scheduling();
        std::cout << "Packet native: Halton BDPT light tiles/ranges" << std::endl;
        halton_light_tiles();
        std::cout << "Packet native: point identity" << std::endl;
        point_identity();
        std::cout << "Packet native: area/black-environment expectation" << std::endl;
        area_black_environment();
        std::cout << "Packet native scheduling, limits, lens, point and BDPT identities passed\n";
    } catch (const std::exception &e) {
        std::cerr << "Packet native render: " << e.what() << '\n';
        return 1;
    }
}
