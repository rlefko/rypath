// SPDX-License-Identifier: GPL-3.0-only
#include "conductor_reference.hpp"
#include "dielectric_reference.hpp"
#include "metal_render.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <string_view>

using namespace rypath;
namespace {
void require(bool ok, std::string_view why) {
    if (!ok)
        throw std::runtime_error(std::string(why));
}
void near(double a, double b, double tolerance, std::string_view why) {
    if (!std::isfinite(a) || !std::isfinite(b) || !std::isfinite(tolerance) || tolerance < 0 ||
        std::abs(a - b) > tolerance)
        throw std::runtime_error(std::string(why) + ": observed " + std::to_string(a) +
                                 ", expected " + std::to_string(b) + ", tolerance " +
                                 std::to_string(tolerance));
}
void mesh(RenderScene &s, std::vector<GeometryPoint> points, unsigned material = 0) {
    GeometryMesh m;
    m.positions = std::move(points);
    m.triangles = {{0, 2, 1}, {0, 3, 2}};
    const auto index = static_cast<unsigned>(s.geometry.meshes.size());
    s.geometry.meshes.push_back(m);
    s.geometry.instances.push_back({index, {}});
    s.instance_materials.push_back(material);
    s.instance_surface_transforms.emplace_back();
    s.instance_texture_frames.emplace_back();
    require(prepare_texture_triangle_frames(m, s.instance_surface_transforms.back().shape,
                                            s.instance_surface_transforms.back().placement,
                                            s.instance_texture_frames.back())
                .empty(),
            "spot fixture frame");
}
void plane(RenderScene &s, float z, unsigned material = 0) {
    mesh(s, {{-8, -8, z}, {8, -8, z}, {8, 8, z}, {-8, 8, z}}, material);
}
RenderScene base(bool surface = true) {
    RenderScene s;
    s.model = RenderModel::pbrt;
    s.max_depth = 1;
    s.environment.fill(0);
    // A nondegenerate but negligible raster footprint isolates axial equations.
    s.raster_camera = RasterCamera{{}, {-1e-25f, -1e-25f, 1}, {2e-25f, 0, 0}, {0, 2e-25f, 0}};
    SpectralMaterial m;
    m.kind = SpectralMaterialKind::constant;
    m.constant = .6f;
    s.materials.push_back(m);
    if (surface)
        plane(s, 1);
    return s;
}
void light(RenderScene &s, GeometryPoint p, float intensity = 2) {
    SpotLight l;
    l.position = p;
    l.intensity.fill(intensity);
    l.cos_falloff_start = l.cos_falloff_end = -1;
    s.spot_lights.push_back(l);
}
RenderRequest request(unsigned count = 256) {
    return {.width = 1,
            .height = 1,
            .seed = 19317,
            .pixel_count = 1,
            .sample_count = count,
            .total_samples = count,
            .threadgroup_width = 32};
}
RenderScene prepared(RenderScene s) {
    // Frames are required for microfacet/glTF materials, but prohibited on the
    // ordinary untextured diffuse/null path. Geometry-free volume has no frames.
    const bool needs_frames = std::ranges::any_of(s.materials, [](const auto &m) {
        return m.scattering != ScatteringKind::diffuse &&
               m.scattering != ScatteringKind::null_interface;
    });
    if (!needs_frames && s.image_textures.empty() && s.texture_graph.nodes.empty())
        s.instance_texture_frames.clear();
    else
        for (std::size_t i = 0; i < s.instance_texture_frames.size(); ++i)
            if (!instance_uses_texture_context(s, i))
                s.instance_texture_frames[i].clear();
    return s;
}
std::vector<RenderSample> sample(const RenderScene &s, unsigned count = 256) {
    const auto input = prepared(s);
    require(validate_render_scene(input).empty(), validate_render_scene(input));
    auto renderer = build_metal_renderer(input);
    require(renderer.status == GeometryStatus::success && renderer.scene, renderer.diagnostic);
    auto result = renderer.scene->sample(request(count));
    require(result.status == GeometryStatus::success, result.diagnostic);
    require(result.guard_count > 0 && result.samples.size() == count, "spot sample accounting");
    for (const auto &v : result.samples)
        require(std::isfinite(v.radiance) && v.radiance >= 0, "finite nonnegative spot radiance");
    return result.samples;
}
template <class E> void exact(const RenderScene &s, E expected, std::string_view label) {
    for (const auto &v : sample(s)) {
        const double e = expected(v);
        near(v.radiance, e, e == 0 ? 0 : 1e-4 * std::abs(e), label);
    }
}
void constant(const RenderScene &s, double e, std::string_view label) {
    exact(s, [=](const auto &) { return e; }, label);
}
// This oracle uses double arithmetic, an explicit row-major matrix-vector
// product and a normalized light-space direction; no renderer light helpers.
double profile(const SpotLight &l, double x, double y, double z) {
    const auto &a = l.light_from_world;
    const double u = double(a[0]) * x + double(a[1]) * y + double(a[2]) * z;
    const double v = double(a[3]) * x + double(a[4]) * y + double(a[5]) * z;
    const double w = double(a[6]) * x + double(a[7]) * y + double(a[8]) * z;
    const double cosine = w / std::sqrt(u * u + v * v + w * w);
    const double start = l.cos_falloff_start, end = l.cos_falloff_end;
    if (cosine < end)
        return 0;
    if (cosine >= start)
        return 1;
    const double t = (cosine - end) / (start - end);
    return t * t * (3 - 2 * t);
}
void hard_sphere(SpotLight &l) { l.cos_falloff_start = l.cos_falloff_end = -1; }
void profiles_and_transforms() {
    constexpr double pi = std::numbers::pi;
    auto s = base();
    light(s, {0, 0, 0});
    auto &l = s.spot_lights[0];
    l.cos_falloff_start = 1;
    l.cos_falloff_end = 0;
    const double full = 2 * double(s.materials[0].constant) / pi;
    // These transforms put the axial receiver at t=1/4, 1/2 and 3/4.
    // Expected values account for authored binary32 matrix entries in double.
    for (double t : {.25, .5, .75}) {
        l.light_from_world = {1, 0, float(std::sqrt(1 - t * t)), 0, 1, 0, 0, 0, float(t)};
        near(profile(l, 0, 0, 1), t * t * (3 - 2 * t), 1e-7,
             "independent cubic smoothstep reference");
        constant(s, full * profile(l, 0, 0, 1), "cubic conic penumbra at quarter intervals");
    }
    l.light_from_world = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    constant(s, full, "full cone at inclusive inner boundary");
    l.light_from_world = {0, 0, 1, 0, 1, 0, -1, 0, 0};
    constant(s, 0, "zero at outer boundary");
    l.light_from_world = {1, 0, 0, 0, 1, 0, 0, 0, -1};
    constant(s, 0, "reflected cone points away");
    l.cos_falloff_start = l.cos_falloff_end = -1;
    constant(s, full, "hard cone includes exact negative axial boundary");
    l.cos_falloff_start = l.cos_falloff_end = 0;
    constant(s, 0, "hard cone excludes outside");
    l.light_from_world[8] = 1;
    constant(s, full, "hard cone includes inside");
    l.cos_falloff_start = 1;
    l.cos_falloff_end = 0;
    // Inverse maps for rotation, reflected nonuniform scale, and a shear.
    // The nonunit map must be applied before normalization.
    for (const std::array<float, 9> &a :
         {std::array<float, 9>{.8f, 0, .6f, 0, 1, 0, -.6f, 0, .8f},
          std::array<float, 9>{-2, 0, 1, 0, .5f, 0, 0, 0, 2},
          std::array<float, 9>{1, .3f, .7f, 0, 2, -.4f, 0, 0, .6f}}) {
        l.light_from_world = a;
        constant(s, full * profile(l, 0, 0, 1), "inverse-map rotation/reflection/scale/shear");
    }
    l.light_from_world = {1e30f, 0, 6e29f, 0, 8e29f, 0, 0, 0, 8e29f};
    constant(s, full * profile(l, 0, 0, 1), "large coefficients normalize safely");
    // A 180-degree hard cone is exactly the delta-position point model.
    hard_sphere(l);
    l.position = {1, 0, 0};
    const auto spots = sample(s);
    PointLight p;
    p.position = l.position;
    p.intensity = l.intensity;
    s.spot_lights.clear();
    s.point_lights.push_back(p);
    const auto points = sample(s);
    for (std::size_t i = 0; i < spots.size(); ++i)
        require(spots[i].radiance == points[i].radiance &&
                    spots[i].random_words == points[i].random_words,
                "180-degree hard spot matches point and consumes no extra words");
}
void surfaces() {
    constexpr double pi = std::numbers::pi;
    for (float distance : {1.f, 2.f, 4.f}) {
        auto s = base();
        light(s, {0, 0, 1 - distance});
        constant(s, 2 * double(s.materials[0].constant) / (pi * distance * distance),
                 "inverse-square radiance");
    }
    auto s = base();
    light(s, {1, 0, 0});
    const double diffuse = 2 * double(s.materials[0].constant) / (2 * std::sqrt(2.) * pi);
    constant(s, diffuse, "oblique diffuse cosine");
    light(s, {0, 0, -1}, 3);
    constant(s, diffuse + 3 * double(s.materials[0].constant) / (4 * pi),
             "sum every spot, no selection or MIS");
    PointLight point;
    point.position = {0, 0, -1};
    point.intensity.fill(4);
    s.point_lights.push_back(point);
    constant(s, diffuse + 7 * double(s.materials[0].constant) / (4 * pi),
             "point and spot contributions add without selection or MIS");
    s.point_lights.clear();
    s.spot_lights.resize(1);
    for (unsigned i = 0; i < 471; ++i)
        s.spot_lights[0].intensity[i] = .25f + float(i) / 470;
    exact(
        s,
        [&](const auto &v) {
            const auto i = std::clamp(std::lround(v.wavelength) - 360L, 0L, 470L);
            return s.spot_lights[0].intensity[static_cast<std::size_t>(i)] *
                   double(s.materials[0].constant) / (2 * std::sqrt(2.) * pi);
        },
        "raw wavelength intensity, independent of sensor normalization");
    s = base();
    light(s, {1, 0, 0});
    s.spot_lights[0].cos_falloff_start = 1;
    s.spot_lights[0].cos_falloff_end = 0;
    const double conic = profile(s.spot_lights[0], -1, 0, 1);
    auto &m = s.materials[0];
    m = SpectralMaterial{};
    m.scattering = ScatteringKind::conductor;
    m.eta.knots = {{360, 1.2f}, {830, 1.2f}};
    m.k.knots = {{360, 2.f}, {830, 2.f}};
    m.u_roughness = m.v_roughness = .3f;
    m.remap_roughness = false;
    const double c = 1 / std::sqrt(2.);
    auto cp = conductor_reference::prepare(double(1.2f), 2, double(.3f), double(.3f), false);
    constant(s, conductor_reference::evaluate(cp, {0, 0, 1}, {c, 0, c}).f * c * conic,
             "rough conductor direct evaluation");
    m.scattering = ScatteringKind::dielectric;
    m.k = {};
    m.eta.knots = {{360, 1.5f}, {830, 1.5f}};
    auto dp = dielectric_reference::prepare(1.5, double(.3f), double(.3f), false);
    constant(s, dielectric_reference::evaluate(dp, {0, 0, 1}, {c, 0, c}).f * c * conic,
             "rough dielectric reflection");
    s.spot_lights[0].position = {1, 0, 2};
    s.spot_lights[0].light_from_world[8] = -1;
    constant(s, dielectric_reference::evaluate(dp, {0, 0, 1}, {c, 0, -c}).f * c * conic,
             "rough dielectric radiance transmission");
    for (auto kind :
         {ScatteringKind::conductor, ScatteringKind::dielectric, ScatteringKind::thin_dielectric}) {
        m = SpectralMaterial{};
        m.scattering = kind;
        m.eta.knots = {{360, 1.5f}, {830, 1.5f}};
        if (kind == ScatteringKind::conductor)
            m.k.knots = {{360, 2.f}, {830, 2.f}};
        constant(s, 0, "delta-only surfaces have no spot NEE");
    }
}
void bounded_and_inactive() {
    auto s = base();
    for (unsigned i = 0; i < 64; ++i)
        light(s, {1, 0, 0});
    const double one = 2 * double(s.materials[0].constant) / (2 * std::sqrt(2.) * std::numbers::pi);
    constant(s, 64 * one, "all 64 spot lights contribute");
    light(s, {1, 0, 0});
    auto all_spots_oversized = build_metal_renderer(prepared(s));
    require(all_spots_oversized.status == GeometryStatus::failure && !all_spots_oversized.scene,
            "direct renderer rejects 65 spots");
    s = base();
    for (unsigned i = 0; i < 32; ++i) {
        light(s, {1, 0, 0});
        PointLight p;
        p.position = {1, 0, 0};
        p.intensity.fill(2);
        s.point_lights.push_back(p);
    }
    constant(s, 64 * one, "all 64 combined point and spot lights contribute");
    light(s, {1, 0, 0});
    auto oversized = build_metal_renderer(prepared(s));
    require(oversized.status == GeometryStatus::failure && !oversized.scene,
            "direct renderer must reject 65 combined positional lights");
    s = base();
    light(s, {1, 0, 0});
    s.max_depth = 0;
    constant(s, 0, "depth zero excludes spot NEE");
    s = base();
    s.environment.fill(1);
    s.max_depth = 3;
    const auto without = sample(s);
    light(s, {1, 0, 0}, 0);
    const auto inactive = sample(s);
    require(without.size() == inactive.size(), "inactive spot sample accounting");
    for (std::size_t i = 0; i < without.size(); ++i) {
        const auto &a = without[i];
        const auto &b = inactive[i];
        require(a.pixel == b.pixel && a.sample == b.sample && a.wavelength == b.wavelength &&
                    a.radiance == b.radiance && a.scatterings == b.scatterings &&
                    a.random_words == b.random_words,
                "zero-intensity spots preserve the no-spot sample stream");
    }
}
void visibility() {
    auto s = base();
    light(s, {1, 0, 0});
    s.spot_lights[0].cos_falloff_start = 1;
    s.spot_lights[0].cos_falloff_end = .4f;
    const double e = 2 * double(s.materials[0].constant) / (2 * std::sqrt(2.) * std::numbers::pi) *
                     profile(s.spot_lights[0], -1, 0, 1);
    // A vertical blocker intersects only the spot segment, never the camera.
    mesh(s, {{.5f, -2, -2}, {.5f, 2, -2}, {.5f, 2, 2}, {.5f, -2, 2}});
    constant(s, 0, "blocker before endpoint");
    for (auto &p : s.geometry.meshes[1].positions)
        p.x = 1.5f;
    constant(s, e, "blocker beyond endpoint is irrelevant");
    for (auto &p : s.geometry.meshes[1].positions)
        p.x = .5f;
    SpectralMaterial null;
    null.kind = SpectralMaterialKind::constant;
    null.scattering = ScatteringKind::null_interface;
    s.materials.push_back(null);
    s.instance_materials[1] = 1;
    constant(s, e, "null surface shadow traversal");
    s.materials[1].scattering = ScatteringKind::dielectric;
    s.materials[1].eta.knots = {{360, 1.5f}, {830, 1.5f}};
    for (bool volume_transport : {false, true}) {
        s.volume_transport = volume_transport;
        constant(s, 0, "ordinary glass occludes spot shadow connections");
    }
    s.volume_transport = false;
    s.materials[1] = null;
    s.instance_materials[1] = 0;
    // Native glTF BLEND alpha is an independently exercised shadow coverage path.
    s.materials.emplace_back();
    s.materials[2].scattering = ScatteringKind::gltf;
    s.materials[2].gltf.alpha_mode = 2;
    s.materials[2].gltf.base_color[3] = 0;
    s.materials[2].gltf.double_sided = true;
    s.instance_materials[1] = 2;
    constant(s, e, "zero alpha shadow coverage");
    s.materials[2].gltf.base_color[3] = .25f;
    // Coverage hashes the geometric ray, so a finite raster footprint supplies
    // independent jittered shadow rays. Its symmetric radiometric variation is
    // negligible compared with the predeclared numerical floor.
    s.raster_camera = RasterCamera{{}, {-.001f, -.001f, 1}, {.002f, 0, 0}, {0, .002f, 0}};
    const auto covered = sample(s, 32768);
    double residual_sum = 0, residual_square = 0;
    for (const auto &v : covered) {
        const double residual = v.radiance - .75 * e;
        residual_sum += residual;
        residual_square += residual * residual;
    }
    const double n = static_cast<double>(covered.size()), mean = residual_sum / n;
    const double se =
        std::sqrt(std::max(0., (residual_square - residual_sum * mean) / (n - 1)) / n);
    near(mean, 0, 6 * se + 2e-5, "stochastic alpha shadow coverage expectation");
    s.materials[2].gltf.base_color[3] = 1;
    constant(s, 0, "opaque alpha shadow coverage");
    s = base();
    light(s, {1, 0, 0});
    s.spot_lights[0].cos_falloff_start = 1;
    s.spot_lights[0].cos_falloff_end = .4f;
    s.volume_transport = true;
    s.camera_medium = 0;
    HomogeneousMedium medium;
    medium.sigma_a.fill(.4f);
    s.media.push_back(medium);
    // Camera free-flight uses analog survival; finite shadow transport evaluates
    // Beer attenuation. Test the mean rather than requiring each path to equal it.
    const double expected = e * std::exp(-double(.4f) * (1 + std::sqrt(2.)));
    const auto attenuated = sample(s, 32768);
    double sum = 0, square = 0;
    for (const auto &v : attenuated) {
        const double residual = v.radiance - expected;
        sum += residual;
        square += residual * residual;
    }
    const double count = static_cast<double>(attenuated.size()), average = sum / count;
    const double standard_error =
        std::sqrt(std::max(0., (square - sum * average) / (count - 1)) / count);
    near(average, 0, 6 * standard_error + 2e-5, "Beer camera and finite spot shadow segments");
}
struct QuadratureSettings {
    double g;
    unsigned intervals;
};
double quadrature(QuadratureSettings settings) {
    const auto [g, n] = settings;
    // Composite Simpson on [0,32]; omitted exponential tail < exp(-64).
    const double a = double(.3f), scatter = double(.7f), t = a + scatter;
    auto f = [&](double x) {
        double r = std::sqrt(1 + x * x), cosine = x / r;
        double den = 1 + g * g + 2 * g * cosine;
        double phase = (1 - g * g) / (4 * std::numbers::pi * std::pow(den, 1.5));
        const double u = std::clamp((cosine + double(.2f)) / (double(.8f) + double(.2f)), 0., 1.);
        const double conic = u * u * (3 - 2 * u);
        return scatter * std::exp(-t * x) * phase * 2 * conic * std::exp(-t * r) / (r * r);
    };
    const double h = 32. / n;
    double sum = f(0) + f(32);
    for (unsigned i = 1; i < n; ++i)
        sum += (i % 2 ? 4 : 2) * f(i * h);
    return sum * h / 3;
}
void volume() {
    for (float g : {0.f, .65f, -.55f}) {
        const double expected = quadrature({.g = g, .intervals = 32768});
        near(quadrature({.g = g, .intervals = 16384}), expected, 1e-9 * std::max(1., expected),
             "spot quadrature refinement");
        for (bool geometry : {false, true}) {
            auto s = base(false);
            s.volume_transport = true;
            s.camera_medium = 0;
            HomogeneousMedium m;
            m.sigma_a.fill(.3f);
            m.sigma_s.fill(.7f);
            m.g = g;
            s.media.push_back(m);
            light(s, {1, 0, 0});
            s.spot_lights[0].cos_falloff_start = .8f;
            s.spot_lights[0].cos_falloff_end = -.2f;
            if (geometry) {
                s.materials[0] = SpectralMaterial{};
                s.materials[0].scattering = ScatteringKind::null_interface;
                plane(s, -10000);
            }
            auto paths = sample(s, 32768);
            double sum = 0, square = 0;
            for (const auto &p : paths) {
                double r = p.radiance - expected;
                sum += r;
                square += r * r;
            }
            const double n = static_cast<double>(paths.size()), mean = sum / n;
            double se = std::sqrt(std::max(0., (square - sum * mean) / (n - 1)) / n);
            std::cout << "spot volume g=" << g << " geometry=" << geometry
                      << " expected=" << expected << " residual=" << mean << " SE=" << se << '\n';
            near(mean, 0, 6 * se + 2e-5 * std::max(1., expected),
                 "independent single-scatter spot quadrature");
        }
    }
}
void invalid_inputs() {
    auto valid = base();
    light(valid, {1, 0, 0});
    auto rejected = [&](const SpotLight &l, std::string_view why) {
        auto s = valid;
        s.spot_lights[0] = l;
        require(!validate_render_scene(prepared(s)).empty(), why);
        const auto result = build_metal_renderer(prepared(s));
        require(result.status == GeometryStatus::failure && !result.scene, why);
    };
    const auto original = valid.spot_lights[0];
    for (auto matrix :
         {std::array<float, 9>{1, 0, 0, 0, 0, 0, 0, 0, 1},
          std::array<float, 9>{1, 0, 0, 0, 1, 0, 0, 0, std::numeric_limits<float>::denorm_min()},
          std::array<float, 9>{std::numeric_limits<float>::max(), 0, 0, 0, 1, 0, 0, 0,
                               std::numeric_limits<float>::min()},
          std::array<float, 9>{1, 0, 0, 0, 1, 0, 0, 0, std::numeric_limits<float>::infinity()}}) {
        auto l = original;
        l.light_from_world = matrix;
        rejected(l, "invalid/singular/underflowing spot direction map rejected");
    }
    auto l = original;
    const float a = std::nextafter(1.5f, 2.f), b = std::nextafter(a, 2.f);
    // This nonsingular binary32 map collapses only after common-scale packing:
    // float(a/3) == float(b/3). It must fail before any GPU work.
    l.light_from_world = {1, a, 0, 1, b, 0, 0, 0, 3};
    rejected(l, "map made singular by GPU packing rejected");
    for (const auto &ends : {std::array<float, 2>{0, .5f}, std::array<float, 2>{1, 1},
                             std::array<float, 2>{1, -1.1f}, std::array<float, 2>{1.1f, 0},
                             std::array<float, 2>{std::numeric_limits<float>::quiet_NaN(), 0}}) {
        l = original;
        l.cos_falloff_start = ends[0];
        l.cos_falloff_end = ends[1];
        rejected(l, "invalid cone cosine thresholds rejected");
    }
    const float tiny = std::numeric_limits<float>::denorm_min();
    const float normal = std::numeric_limits<float>::min();
    for (const auto &ends :
         {std::array<float, 2>{tiny, -tiny}, std::array<float, 2>{normal + tiny, normal},
          std::array<float, 2>{tiny, tiny}}) {
        l = original;
        l.cos_falloff_start = ends[0];
        l.cos_falloff_end = ends[1];
        rejected(l, "GPU subnormal flushing must not change the cone or feather");
    }
    l = original;
    l.intensity[117] = -1;
    rejected(l, "negative spot spectral intensity rejected");
    l = original;
    l.position.x = std::numeric_limits<float>::quiet_NaN();
    rejected(l, "nonfinite spot position rejected");
    l = original;
    l.position.x = tiny;
    rejected(l, "subnormal source position cannot silently change hard-cone support");
}
void coincidence() {
    auto s = base();
    light(s, {0, 0, 1});
    auto failed = render_progressive(
        prepared(s), {.width = 1, .height = 1, .samples = 1, .batch_size = 1, .seed = 42});
    require(failed.status == GeometryStatus::failure, "active coincident spot must fail batch");
    for (const auto &p : failed.film.pixels)
        require(p.count == 0, "failed spot batch must not commit Film");
    s.spot_lights[0].intensity.fill(0);
    constant(s, 0, "zero-intensity coincident spot is skipped");
    s = base();
    light(s, {1, 0, 0});
    s.spot_lights[0].light_from_world = {
        1, 0, 0, 0, 1, 0, 0, 0, -std::numeric_limits<float>::min()};
    s.spot_lights[0].cos_falloff_start = s.spot_lights[0].cos_falloff_end = 0;
    require(validate_render_scene(prepared(s)).empty(), "normal packed map is structurally valid");
    failed = render_progressive(
        prepared(s), {.width = 1, .height = 1, .samples = 1, .batch_size = 1, .seed = 42});
    require(failed.status == GeometryStatus::failure,
            "active mapped component flushing at a hard cone must fail the batch");
    for (const auto &p : failed.film.pixels)
        require(p.count == 0, "failed angular arithmetic must not commit Film");
}
} // namespace
int main(int argc, char **argv) try {
    bool no_device = false, no_rt = false;
    for (int i = 1; i < argc; ++i) {
        std::string_view a(argv[i]);
        if (!a.starts_with("--"))
            continue;
        require(a == "--allow-no-device" || a == "--allow-no-ray-tracing",
                "unknown spot test option");
        no_device |= a == "--allow-no-device";
        no_rt |= a == "--allow-no-ray-tracing";
    }
    auto capability = build_metal_renderer(prepared(base()));
    if ((no_device && capability.status == GeometryStatus::no_device) ||
        (no_rt && capability.status == GeometryStatus::no_ray_tracing)) {
        std::cout << "SKIP Metal spots: " << capability.diagnostic << '\n';
        return 77;
    }
    require(capability.status == GeometryStatus::success && capability.scene,
            capability.diagnostic);
    profiles_and_transforms();
    surfaces();
    bounded_and_inactive();
    visibility();
    volume();
    invalid_inputs();
    coincidence();
    std::cout << "PASS spot cubic profile/transforms, inverse-square/cosine/additive/spectra, "
                 "rough BSDFs, finite "
                 "visibility, alpha/null/Beer, HG quadrature and failed Film\n";
    return 0;
} catch (const std::exception &e) {
    std::cerr << "Spot render: " << e.what() << '\n';
    return 1;
}
