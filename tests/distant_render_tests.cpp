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
            "distant fixture frame");
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
void light(RenderScene &s, GeometryPoint direction = {0, 0, -1}, float radiance = 2) {
    DistantLight l;
    l.direction = direction;
    l.radiance.fill(radiance);
    s.distant_lights.push_back(l);
    s.distant_scene_radius = 1;
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
    require(result.guard_count > 0 && result.samples.size() == count, "distant sample accounting");
    for (const auto &v : result.samples)
        require(std::isfinite(v.radiance) && v.radiance >= 0,
                "finite nonnegative distant radiance");
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
void surfaces() {
    constexpr double pi = std::numbers::pi;
    auto s = base();
    light(s);
    const double full = 2 * double(s.materials[0].constant) / pi;
    constant(s, full, "Lambert directional radiance");
    s.distant_scene_radius = 20;
    constant(s, full, "no inverse-square attenuation");
    s.distant_lights[0].direction = {.6f, 0, -.8f};
    constant(s, full * double(.8f), "world-space direction cosine");
    light(s, {0, 0, -1}, 3);
    PointLight p;
    p.position = {0, 0, 0};
    p.intensity.fill(1);
    s.point_lights.push_back(p);
    constant(s, (2 * double(.8f) + 3 + 1) * double(s.materials[0].constant) / pi,
             "distant and positional contributions add");
    s.point_lights.clear();
    s.distant_lights.resize(1);
    for (unsigned i = 0; i < 471; ++i)
        s.distant_lights[0].radiance[i] = .25f + float(i) / 470;
    exact(
        s,
        [&](const auto &v) {
            const auto i = std::clamp(std::lround(v.wavelength) - 360L, 0L, 470L);
            return double(s.distant_lights[0].radiance[static_cast<std::size_t>(i)]) *
                   double(s.materials[0].constant) * double(.8f) / pi;
        },
        "nearest-wavelength directional radiance");
    s = base();
    light(s, {.6f, 0, -.8f});
    auto &m = s.materials[0];
    m = SpectralMaterial{};
    m.scattering = ScatteringKind::conductor;
    m.eta.knots = {{360, 1.2f}, {830, 1.2f}};
    m.k.knots = {{360, 2.f}, {830, 2.f}};
    m.u_roughness = m.v_roughness = .3f;
    m.remap_roughness = false;
    auto cp = conductor_reference::prepare(double(1.2f), 2, double(.3f), double(.3f), false);
    constant(s, 2 * conductor_reference::evaluate(cp, {0, 0, 1}, {.6, 0, .8}).f * .8,
             "rough conductor direct evaluation");
    m.scattering = ScatteringKind::dielectric;
    m.k = {};
    m.eta.knots = {{360, 1.5f}, {830, 1.5f}};
    auto dp = dielectric_reference::prepare(1.5, double(.3f), double(.3f), false);
    constant(s, 2 * dielectric_reference::evaluate(dp, {0, 0, 1}, {.6, 0, .8}).f * .8,
             "rough dielectric reflection");
    s.distant_lights[0].direction.z = .8f;
    constant(s, 2 * dielectric_reference::evaluate(dp, {0, 0, 1}, {.6, 0, -.8}).f * .8,
             "rough dielectric transmission");
    for (auto kind :
         {ScatteringKind::conductor, ScatteringKind::dielectric, ScatteringKind::thin_dielectric}) {
        m = SpectralMaterial{};
        m.scattering = kind;
        m.eta.knots = {{360, 1.5f}, {830, 1.5f}};
        if (kind == ScatteringKind::conductor)
            m.k.knots = {{360, 2.f}, {830, 2.f}};
        constant(s, 0, "delta-only surfaces exclude distant NEE");
    }
    s = base();
    light(s);
    s.max_depth = 0;
    constant(s, 0, "depth zero excludes directional NEE");
    s = base();
    for (unsigned i = 0; i < 64; ++i)
        light(s);
    constant(s, 64 * full, "all 64 distant sources contribute");
    s = base();
    s.environment.fill(1);
    s.max_depth = 3;
    auto a = sample(s);
    light(s, {0, 0, -1}, 0);
    auto b = sample(s);
    for (std::size_t i = 0; i < a.size(); ++i)
        require(a[i].radiance == b[i].radiance && a[i].random_words == b[i].random_words &&
                    a[i].scatterings == b[i].scatterings && a[i].wavelength == b[i].wavelength,
                "zero distant light preserves random streams and radiance");
    s.distant_lights[0].radiance.fill(2);
    auto active = sample(s);
    for (std::size_t i = 0; i < a.size(); ++i)
        require(a[i].random_words == active[i].random_words &&
                    a[i].scatterings == active[i].scatterings &&
                    a[i].wavelength == active[i].wavelength,
                "active distant light consumes no additional random draws");
}
void blocker(RenderScene &s, float z, unsigned material = 0) {
    // Camera at x=0 misses this patch; the oblique directional connection hits it.
    mesh(s, {{.1f, -2, z}, {3, -2, z}, {3, 2, z}, {.1f, 2, z}}, material);
}
void visibility() {
    auto s = base();
    light(s, {.6f, 0, -.8f});
    const double full = 2 * double(s.materials[0].constant) * double(.8f) / std::numbers::pi;
    blocker(s, .5f);
    constant(s, 0, "opaque directional shadow");
    SpectralMaterial glass;
    glass.scattering = ScatteringKind::dielectric;
    glass.eta.knots = {{360, 1.5f}, {830, 1.5f}};
    s.materials.push_back(glass);
    s.instance_materials[1] = 1;
    constant(s, 0, "glass blocks direct directional connection");
    s.materials[1].scattering = ScatteringKind::null_interface;
    constant(s, full, "null boundary preserves fixed distant endpoint");
    // Original receiver z=1, endpoint z=-.6. The first leg's cutoff
    // and a re-spawned leg's cutoff both lie strictly between these planes.
    blocker(s, -.5998f);
    constant(s, 0, "blocker before PBRT cutoff after oblique null crossing");
    s.geometry.meshes.pop_back();
    s.geometry.instances.pop_back();
    s.instance_materials.pop_back();
    s.instance_surface_transforms.pop_back();
    s.instance_texture_frames.pop_back();
    blocker(s, -.59995f);
    constant(s, full, "blocker beyond PBRT cutoff after oblique null crossing");
    s = base();
    light(s, {.6f, 0, -.8f});
    blocker(s, .5f);
    s.materials.emplace_back();
    s.materials[1].scattering = ScatteringKind::gltf;
    s.materials[1].gltf.alpha_mode = 2;
    s.materials[1].gltf.base_color[3] = 0;
    s.materials[1].gltf.double_sided = true;
    s.instance_materials[1] = 1;
    constant(s, full, "zero alpha directional shadow coverage");
    s.materials[1].gltf.base_color[3] = .25f;
    s.raster_camera = RasterCamera{{}, {-.001f, -.001f, 1}, {.002f, 0, 0}, {0, .002f, 0}};
    auto covered = sample(s, 32768);
    double residual_sum = 0, residual_square = 0;
    for (const auto &v : covered) {
        const double residual = v.radiance - .75 * full;
        residual_sum += residual;
        residual_square += residual * residual;
    }
    const double n = static_cast<double>(covered.size()), mean = residual_sum / n;
    const double se =
        std::sqrt(std::max(0., (residual_square - residual_sum * mean) / (n - 1)) / n);
    near(mean, 0, 6 * se + 2e-5, "stochastic directional alpha coverage expectation");
    s.materials[1].gltf.base_color[3] = 1;
    constant(s, 0, "opaque alpha directional shadow coverage");
    s = base();
    light(s, {.6f, 0, -.8f});
    blocker(s, -.5997f);
    constant(s, 0, "blocker before initial PBRT cutoff");
    s = base();
    light(s, {.6f, 0, -.8f});
    blocker(s, -.59995f);
    constant(s, full, "blocker beyond initial PBRT cutoff");
}
void volume() {
    // Single scattering in an infinite homogeneous medium: camera flight density
    // sigma_s exp(-sigma_t x); directional HG is constant along the camera ray,
    // and its finite shadow transmittance is exp(-sigma_t * 2R * .9999).
    for (float g : {0.f, .65f, -.55f}) {
        auto s = base(false);
        s.volume_transport = true;
        s.camera_medium = 0;
        HomogeneousMedium m;
        m.sigma_a.fill(.3f);
        m.sigma_s.fill(.7f);
        m.g = g;
        s.media.push_back(m);
        s.materials[0].scattering = ScatteringKind::null_interface;
        plane(s, -10000);
        light(s, {.6f, 0, -.8f});
        const double total = double(.3f) + double(.7f);
        const double den = 1 + double(g) * g + 2 * double(g) * double(.8f);
        const double phase = (1 - double(g) * g) / (4 * std::numbers::pi * std::pow(den, 1.5));
        const double expected = double(.7f) / total * phase * 2 * std::exp(-total * 2 * .9999);
        const auto paths = sample(s, 4096);
        double sum = 0;
        for (const auto &v : paths) {
            require(v.radiance > 0, "unbounded fog receives positive finite distant signal");
            sum += v.radiance;
        }
        near(sum / static_cast<double>(paths.size()), expected, 2e-5 * expected,
             "independent HG and Beer single-scatter integral");
    }
    auto s = base();
    light(s);
    s.volume_transport = true;
    s.camera_medium = 0;
    HomogeneousMedium m;
    m.sigma_a.fill(.4f);
    s.media.push_back(m);
    const double expected = 2 * double(s.materials[0].constant) / std::numbers::pi *
                            std::exp(-double(.4f) * (1 + 2 * .9999));
    auto paths = sample(s, 32768);
    double sum = 0, square = 0;
    for (const auto &v : paths) {
        double residual = v.radiance - expected;
        sum += residual;
        square += residual * residual;
    }
    const double n = static_cast<double>(paths.size()), mean = sum / n;
    const double se = std::sqrt(std::max(0., (square - sum * mean) / (n - 1)) / n);
    near(mean, 0, 6 * se + 2e-5, "Beer camera and finite directional shadow");
}
void invalid_inputs() {
    auto valid = base();
    light(valid);
    auto rejected = [](RenderScene s) {
        require(!validate_render_scene(prepared(s)).empty(), "invalid distant input rejected");
        auto built = build_metal_renderer(prepared(s));
        require(built.status == GeometryStatus::failure && !built.scene,
                "direct renderer rejects invalid distant input");
    };
    for (float r : {0.f, -1.f, std::numeric_limits<float>::denorm_min(),
                    std::numeric_limits<float>::infinity(), std::numeric_limits<float>::max()}) {
        auto s = valid;
        s.distant_scene_radius = r;
        rejected(s);
    }
    for (GeometryPoint d : {GeometryPoint{}, GeometryPoint{0, 0, -2},
                            GeometryPoint{std::numeric_limits<float>::quiet_NaN(), 0, -1},
                            GeometryPoint{std::numeric_limits<float>::denorm_min(), 0, -1}}) {
        auto s = valid;
        s.distant_lights[0].direction = d;
        rejected(s);
    }
    auto s = valid;
    s.distant_lights[0].radiance[10] = -1;
    rejected(s);
    s = valid;
    s.distant_lights[0].radiance[10] = std::numeric_limits<float>::infinity();
    rejected(s);
    s = valid;
    s.distant_lights.resize(65, s.distant_lights[0]);
    rejected(s);
    s = base(false);
    light(s);
    rejected(s);
    s = valid;
    s.geometry.meshes[0].triangles.clear();
    rejected(s);
    s = valid;
    s.distant_scene_radius = std::numeric_limits<float>::min();
    require(validate_render_scene(prepared(s)).empty(), "tiny positive radius structurally valid");
    auto failed = render_progressive(
        prepared(s), {.width = 1, .height = 1, .samples = 1, .batch_size = 1, .seed = 42});
    require(failed.status == GeometryStatus::failure, "collapsed endpoint fails arithmetic");
    for (const auto &p : failed.film.pixels)
        require(p.count == 0, "failed distant batch commits no Film samples");
}
} // namespace
int main(int argc, char **argv) try {
    bool no_device = false, no_rt = false;
    for (int i = 1; i < argc; ++i) {
        std::string_view a(argv[i]);
        require(a == "--allow-no-device" || a == "--allow-no-ray-tracing",
                "unknown distant test option");
        no_device |= a == "--allow-no-device";
        no_rt |= a == "--allow-no-ray-tracing";
    }
    auto capability = build_metal_renderer(prepared(base()));
    if ((no_device && capability.status == GeometryStatus::no_device) ||
        (no_rt && capability.status == GeometryStatus::no_ray_tracing)) {
        std::cout << "SKIP Metal distant: " << capability.diagnostic << '\n';
        return 77;
    }
    require(capability.status == GeometryStatus::success && capability.scene,
            capability.diagnostic);
    surfaces();
    visibility();
    volume();
    invalid_inputs();
    std::cout << "PASS distant radiance, BSDFs, bounded visibility, HG/Beer and failed Film\n";
    return 0;
} catch (const std::exception &e) {
    std::cerr << "Distant render: " << e.what() << '\n';
    return 1;
}
