// SPDX-License-Identifier: GPL-3.0-only
#include "bsdf_table.hpp"
#include "bsdf_table_fixture.hpp"
#include "checkpoint.hpp"
#include "metal_render.hpp"
#include "packet_wire.h"
#include "render_driver.hpp"
#include "render_identity.hpp"
#include "shading_normal_cases.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numbers>
#include <sstream>
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
// Required extended native check for Importance-continuation changes:
//   rypath_packet_render_tests --shading-normal-spot-extended
// Fixed 8 seeds (19317+k*7919), 16x16,8192 spp, flat/tilted spotlight corners.
// Gate: abs(mean paired BDPT-PT) <= 6 paired SE + 2% PT mean, with a zero-direct
// control. This explicit minutes-scale check is not a default CTest. The lower
// 256-spp diagnostic did not detect the continuation-disabled mutation; retain
// that limitation rather than substituting it for the extended check.
void shading_normal_native(bool spotlight = false, bool extended = false) {
    // Optional evidence output is fresh-only; ordinary CTest runs create no files.
    std::filesystem::path evidence;
    if (const auto *path = std::getenv("RYPATH_SHADING_NORMAL_EVIDENCE")) {
        evidence = path;
        require(!evidence.empty() && !std::filesystem::exists(evidence),
                "Shading-normal evidence path must be fresh");
        require(std::filesystem::create_directories(evidence),
                "Cannot create shading-normal evidence directory");
    }
    auto save = [&](const RenderScene &scene, const RenderSettings &settings,
                    const RenderResult &result, const std::string &name) {
        if (evidence.empty())
            return;
        RenderCheckpoint checkpoint{
            result.film,
            settings,
            render_identity(scene, settings),
            {{0, settings.samples, "focused-test; external execution receipt binds build",
              result.device.name, result.device.architecture, ""}}};
        require(write_checkpoint(checkpoint, evidence / (name + ".rycp")).empty(),
                "Cannot retain shading-normal raw checkpoint");
    };
    const auto queries = shading_normal_test::queries();
    const auto result = probe_shading_normal(queries);
    require(result.status == MetalStatus::success, result.diagnostic);
    require(result.values.size() == queries.size() && result.guard_count == 128,
            "Shading-normal probe accounting/guards");
    if (!evidence.empty()) {
        std::ofstream input(evidence / "probe-input.bin", std::ios::binary);
        std::ofstream output(evidence / "probe-output.bin", std::ios::binary);
        input.write(reinterpret_cast<const char *>(queries.data()),
                    static_cast<std::streamsize>(queries.size() * sizeof(queries[0])));
        output.write(reinterpret_cast<const char *>(result.values.data()),
                     static_cast<std::streamsize>(result.values.size() * sizeof(result.values[0])));
        require(bool(input) && bool(output), "Cannot retain native shading-normal probe records");
    }
    std::size_t flushed_subnormal_outputs = 0;
    std::ostringstream ftz_report;
    auto native_check = [&](float observed, long double exact, std::size_t row,
                            const char *operation) {
        // Metal does not promise binary32 subnormal results. Retain these cases
        // and accept only the correct subnormal (normal oracle below) or zero.
        // NORMAL-range checks retain the independent 8-epsilon gate.
        if (exact > 0 && exact < std::numeric_limits<float>::min() && observed == 0) {
            ++flushed_subnormal_outputs;
            ftz_report << "row " << row << " " << operation << " exact " << std::setprecision(21)
                       << exact << " observed zero\n";
            return;
        }
        shading_normal_test::check(observed, exact);
    };
    for (std::size_t i = 0; i < queries.size(); ++i) {
        const auto &v = result.values[i];
        native_check(v.applied, shading_normal_test::oracle(queries[i], false), i, "apply");
        native_check(v.multiplied, shading_normal_test::oracle(queries[i], true), i, "multiply");
        shading_normal_test::check_identity(queries[i], v.applied, v.multiplied);
        native_check(v.endpoint_applied, shading_normal_test::endpoint_oracle(queries[i], false), i,
                     "endpoint_apply");
        native_check(v.endpoint_multiplied, shading_normal_test::endpoint_oracle(queries[i], true),
                     i, "endpoint_multiply");
        require(v.reserved == 0, "Shading-normal probe ABI reserved field");
    }
    std::cout << "Native shading-normal FTZ outputs " << flushed_subnormal_outputs << '\n'
              << ftz_report.str();
    if (!evidence.empty()) {
        std::ofstream report(evidence / "native-ftz.txt");
        report << "subnormal-output policy: correct within oracle tolerance or zero; "
                  "no normal-range tolerance change\n"
               << "observed flushed outputs " << flushed_subnormal_outputs << '\n'
               << ftz_report.str();
        require(bool(report), "Cannot retain native FTZ evidence");
    }
    // Fixed before execution: 8 independent seeds, 16x16 raster; 64 spp for
    // original fixtures; spotlight uses 256 spp, or the explicit 8192 spp
    // extended mode for the final fixed-budget continuation-power check.
    // No clamp/tone mapping. Flat and tilted twins share the geometric scene.
    // The depth-two corner also exercises accepted importance continuation before
    // an endpoint connection/splat; directional PDFs and MIS remain production.
    std::array<std::array<double, 8>, 2> direct_path_means{};
    for (bool corner : {false, true}) {
        if (spotlight && !corner)
            continue;
        for (bool tilted : {false, true}) {
            RenderScene scene;
            scene.model = RenderModel::pbrt;
            scene.spectral_transport = SpectralTransport::packet;
            scene.max_depth = corner ? 2 : 1;
            scene.raster_camera = RasterCamera{{2, 0, 2}, {-2.5f, -.5f, -2}, {1, 0, 0}, {0, 1, 0}};
            SpectralMaterial matte;
            matte.kind = SpectralMaterialKind::constant;
            matte.constant = .6f;
            scene.materials.push_back(matte);
            mesh(scene, {{-.5f, -.5f, 0}, {.5f, -.5f, 0}, {.5f, .5f, 0}, {-.5f, .5f, 0}}, true);
            scene.geometry.meshes[0].normals.assign(4, tilted ? GeometryPoint{.8f, 0, .6f}
                                                              : GeometryPoint{0, 0, 1});
            if (corner) {
                // A low wall clears camera rays but redirects the side light.
                // The tilted floor has zero direct-light BSDF support everywhere:
                // .8*(-2-x)+.6*.5 < 0 for x in [-.5,.5]. All PT signal therefore
                // requires the wall bounce. The tilted wall tests Importance
                // continuation before the floor endpoint/splat.
                mesh(scene, {{.75f, -.75f, .05f},
                             {.75f, .75f, .05f},
                             {.75f, .75f, .25f},
                             {.75f, -.75f, .25f}});
                scene.geometry.meshes[1].normals.assign(4, tilted ? GeometryPoint{-.8f, 0, .6f}
                                                                  : GeometryPoint{-1, 0, 0});
            }
            PointLight point;
            point.position = corner ? GeometryPoint{-2, 0, .5f} : GeometryPoint{-.25f, 0, 2};
            point.intensity.fill(2);
            if (spotlight) {
                // NEW continuation-power fixture, fixed before measurement:
                // axis points at (.75,0,.15); inner 2deg / outer 4deg cone. All
                // inner-cone rays hit the finite wall; smoothstep falloff puts
                // at least 40% of emitted flux there. PT retains a broad wall patch.
                const double length = std::hypot(2.75, .35);
                const float x = static_cast<float>(2.75 / length);
                const float z = static_cast<float>(-.35 / length);
                SpotLight light;
                light.position = point.position;
                light.intensity = point.intensity;
                light.light_from_world = {0, 1, 0, -z, 0, x, x, 0, z};
                light.cos_falloff_start = static_cast<float>(std::cos(2 * std::numbers::pi / 180));
                light.cos_falloff_end = static_cast<float>(std::cos(4 * std::numbers::pi / 180));
                scene.spot_lights.push_back(light);
            } else {
                scene.point_lights.push_back(point);
            }
            if (corner && tilted) {
                scene.integrator = IntegratorKind::path;
                scene.max_depth = 1;
                const RenderSettings direct_settings{16, 16, 1, 1, 19317};
                const auto direct = render_progressive(scene, direct_settings);
                require(direct.completion == RenderCompletion::complete, direct.diagnostic);
                save(scene, direct_settings, direct, "corner-tilted-direct-zero");
                require(mean_y(direct.film) == 0,
                        "Indirect fixture leaked direct illumination/camera-visible wall");
                scene.max_depth = 2;
            }
            Moments difference, reference;
            bool splat_signal = false;
            for (unsigned seed = 0; seed < 8; ++seed) {
                RenderSettings settings{16, 16, spotlight ? (extended ? 8192u : 256u) : 64u, 16,
                                        19317u + seed * 7919u};
                scene.integrator = IntegratorKind::path;
                auto path = render_progressive(scene, settings);
                require(path.completion == RenderCompletion::complete, path.diagnostic);
                const auto name = std::string(corner ? "corner-" : "direct-") +
                                  (tilted ? "tilted-" : "flat-") + std::to_string(settings.seed);
                save(scene, settings, path, name + "-pt");
                scene.integrator = IntegratorKind::bdpt;
                auto bdpt = render_progressive(scene, settings);
                require(bdpt.completion == RenderCompletion::complete, bdpt.diagnostic);
                save(scene, settings, bdpt, name + "-bdpt");
                require(validate_film_accumulation(path.film).empty() &&
                            validate_film_accumulation(bdpt.film).empty(),
                        "Shading-normal raw Film invalid");
                const double a = mean_y(path.film), b = mean_y(bdpt.film);
                require(a > 0 && b > 0, "Shading-normal fixture lost illumination");
                difference.add(b - a);
                reference.add(a);
                if (!corner)
                    direct_path_means[tilted][seed] = a;
                splat_signal |= std::ranges::any_of(
                    bdpt.film.pixels, [](const auto &p) { return p.splat_sum[1] > 0; });
                std::cout << "Shading-normal seed " << settings.seed << " corner " << corner
                          << " tilted " << tilted << " PT " << a << " BDPT " << b << '\n';
            }
            const double tolerance =
                6 * std::sqrt(difference.variance_mean()) + .02 * reference.mean();
            require(splat_signal, "Shading-normal fixture has no light-camera splats");
            require(std::abs(difference.mean()) <= tolerance,
                    "Shading-normal PT/BDPT mean regression");
            std::cout << "Shading-normal corner " << corner << " tilted " << tilted
                      << " paired difference " << difference.mean() << " tolerance " << tolerance
                      << '\n';
        }
    }
    if (spotlight)
        return;
    // Independent area quadrature of rho*I*cos(light,ns)/(pi*r^2).
    // Uniform raster positions map affinely to the unit square. Sensor/spectrum
    // factors cancel between the otherwise identical tilted and flat fixtures.
    double flat = 0, tilt = 0;
    constexpr unsigned divisions = 256;
    for (unsigned y = 0; y < divisions; ++y)
        for (unsigned x = 0; x < divisions; ++x) {
            const double dx = -.25 - ((x + .5) / divisions - .5);
            const double dy = -((y + .5) / divisions - .5);
            const double r2 = dx * dx + dy * dy + 4;
            const double denominator = r2 * std::sqrt(r2);
            flat += 2 / denominator;
            tilt += (.8 * dx + 1.2) / denominator;
        }
    const double ratio = tilt / flat;
    Moments quadrature_difference, quadrature_reference;
    for (unsigned seed = 0; seed < 8; ++seed) {
        quadrature_difference.add(direct_path_means[1][seed] - ratio * direct_path_means[0][seed]);
        quadrature_reference.add(direct_path_means[0][seed]);
    }
    const double tolerance =
        6 * std::sqrt(quadrature_difference.variance_mean()) + .002 * quadrature_reference.mean();
    require(std::abs(quadrature_difference.mean()) <= tolerance,
            "Native PT tilted/flat means disagree with independent diffuse-plane integral");
    std::cout << "Shading-normal analytic tilted/flat PT ratio " << ratio << " paired residual "
              << quadrature_difference.mean() << " tolerance " << tolerance << '\n';
}
} // namespace
int main(int argc, char **argv) {
    try {
        if (argc == 2 && (std::string_view(argv[1]) == "--shading-normal" ||
                          std::string_view(argv[1]) == "--shading-normal-spot" ||
                          std::string_view(argv[1]) == "--shading-normal-spot-extended")) {
            auto capability = query_metal_device();
#if defined(RYPATH_ALLOW_NO_METAL_DEVICE)
            if (capability.status == MetalStatus::no_device)
                return 77;
#endif
            require(capability.status == MetalStatus::success, capability.diagnostic);
#if defined(RYPATH_ALLOW_NO_METAL_RAY_TRACING)
            if (!capability.device.compute_ray_tracing)
                return 77;
#endif
            shading_normal_native(std::string_view(argv[1]) != "--shading-normal",
                                  std::string_view(argv[1]) == "--shading-normal-spot-extended");
            std::cout << "PASS native shading-normal oracle and PT/BDPT means\n";
            return 0;
        }
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
