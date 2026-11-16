// SPDX-License-Identifier: GPL-3.0-only
#include "metal_render.hpp"
#include "packet_wire.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
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
int main() {
    try {
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
