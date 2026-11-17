// SPDX-License-Identifier: GPL-3.0-only
#include "conductor_reference.hpp"
#include "dielectric_reference.hpp"
#include "metal_render.hpp"
#include "rgb_spectrum.hpp"
#include <algorithm>
#include <bit>
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
            "projection fixture frame");
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
    ProjectionLight l;
    l.position = p;
    l.image_scale = intensity;
    l.inv_tan_half_fov = .001f;
    if (p.x != 0)
        l.light_from_world = {0, 0, 1, 0, -1, 0, -std::copysign(1.f, p.x), 0, 0};
    else if (p.z > 1)
        l.light_from_world = {1, 0, 0, 0, -1, 0, 0, 0, -1};
    DecodedImage image;
    image.width = image.height = 4;
    image.source_channels = 3;
    image.sample_format = ImageSampleFormat::float32;
    image.floating_rgb.assign(std::size_t{4} * 4 * 3, 1);
    auto built = build_image_base(image, ImageEncoding::linear, ImageWrap::clamp);
    if (!built.value)
        throw std::runtime_error(built.diagnostic);
    ImageTexture texture;
    texture.image = std::make_shared<const ImagePyramid>(std::move(built.value.value()));
    l.image_texture = static_cast<unsigned>(s.image_textures.size());
    s.image_textures.push_back(std::move(texture));
    s.projection_lights.push_back(l);
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
    require(result.guard_count > 0 && result.samples.size() == count,
            "projection sample accounting");
    for (const auto &v : result.samples)
        require(std::isfinite(v.radiance) && v.radiance >= 0,
                "finite nonnegative projection radiance");
    // Compare gray-map transport after removing the independently tabulated
    // D65 illuminant. The renderer still carries its physical spectral value.
    for (auto &v : result.samples)
        v.radiance /= pbrt_d65()[std::size_t(std::lround(v.wavelength) - 360)];
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
// Combined-light NEE draws one source and divides by its selection PMF.
// The analytic sum remains its expectation, rather than a per-path identity.
// Freeze the independent-path budget and six-standard-error rule before dispatch.
void selected_mean(const RenderScene &s, double expected, std::string_view label) {
    const auto values = sample(s, 32768);
    double sum = 0, square = 0;
    for (const auto &value : values) {
        const double residual = value.radiance - expected;
        sum += residual;
        square += residual * residual;
    }
    const double n = static_cast<double>(values.size()), residual_mean = sum / n;
    const double variance = std::max(0., (square - sum * residual_mean) / (n - 1));
    const double se = std::sqrt(variance / n);
    const double limit = 6 * se + 1e-4 * std::abs(expected);
    std::cout << label << " expected=" << expected << " residual=" << residual_mean
              << " standard_error=" << se << " limit=" << limit << '\n';
    near(residual_mean, 0, limit, label);
}
// Ordinary-case projection oracle uses double affine and perspective arithmetic.
std::array<float, 3> color(unsigned x, unsigned y) {
    return {.125f + .0625f * float((3 * x + y) % 9), .125f + .0625f * float((x + 5 * y) % 9),
            .125f + .0625f * float((7 * x + 3 * y) % 9)};
}
void colored_map(RenderScene &s, unsigned width, unsigned height) {
    DecodedImage im;
    im.width = width;
    im.height = height;
    im.source_channels = 3;
    im.sample_format = ImageSampleFormat::float32;
    for (unsigned y = 0; y < height; ++y)
        for (unsigned x = 0; x < width; ++x) {
            const auto rgb = color(x, y);
            im.floating_rgb.insert(im.floating_rgb.end(), rgb.begin(), rgb.end());
        }
    auto built = build_image_base(im, ImageEncoding::linear, ImageWrap::clamp);
    if (!built.value)
        throw std::runtime_error(built.diagnostic);
    auto &l = s.projection_lights[0];
    s.image_textures[l.image_texture].image =
        std::make_shared<const ImagePyramid>(std::move(built.value.value()));
    const float aspect = float(width) / float(height);
    l.screen_half_width = aspect > 1 ? aspect : 1;
    l.screen_half_height = aspect > 1 ? 1 : 1 / aspect;
}
double oracle(const RenderScene &s, float wavelength) {
    const auto &l = s.projection_lights[0];
    const double x = -double(l.position.x), y = -double(l.position.y), z = 1 - double(l.position.z);
    const double d2 = x * x + y * y + z * z, d = std::sqrt(d2);
    const auto &m = l.light_from_world;
    const double a = (m[0] * x + m[1] * y + m[2] * z) / d;
    const double b = (m[3] * x + m[4] * y + m[5] * z) / d;
    const double c = (m[6] * x + m[7] * y + m[8] * z) / d;
    if (c < .001f)
        return 0;
    const double px = a * l.inv_tan_half_fov / c, py = b * l.inv_tan_half_fov / c;
    if (std::abs(px) > l.screen_half_width || std::abs(py) > l.screen_half_height)
        return 0;
    const auto &im = *s.image_textures[l.image_texture].image;
    const auto &level = im.levels[0];
    const auto ix = unsigned(
        std::clamp(int(level.width * (px + l.screen_half_width) / (2 * l.screen_half_width)), 0,
                   int(level.width) - 1));
    const auto iy = unsigned(
        std::clamp(int(level.height * (py + l.screen_half_height) / (2 * l.screen_half_height)), 0,
                   int(level.height) - 1));
    const auto rgb_value = image_base_texel_linear_rgb(im, ix, iy);
    if (!rgb_value)
        throw std::runtime_error("projection oracle texel lookup failed");
    const auto rgb = *rgb_value;
    const double amplitude = 2 * double(*std::ranges::max_element(rgb));
    const std::array<float, 3> normalized{float(rgb[0] / amplitude), float(rgb[1] / amplitude),
                                          float(rgb[2] / amplitude)};
    const auto spectrum = reconstruct_srgb(normalized);
    if (!spectrum)
        throw std::runtime_error("independent RGB reconstruction failed");
    return l.image_scale * amplitude * evaluate_srgb(spectrum.value(), wavelength) *
           double(s.materials[0].constant) / std::numbers::pi * z / (d2 * d);
}
void profiles_and_transforms() {
    for (const auto size : {std::array<unsigned, 2>{8, 8}, {8, 4}, {4, 8}}) {
        auto s = base();
        light(s, {.31f, .27f, -.2f});
        colored_map(s, size[0], size[1]);
        s.projection_lights[0].inv_tan_half_fov = 1;
        for (const auto m : {std::array<float, 9>{1, 0, 0, 0, -1, 0, 0, 0, 1},
                             std::array<float, 9>{2, 0, 0, 0, -.75f, 0, 0, 0, .8f},
                             std::array<float, 9>{1, .3f, .2f, 0, -1, .1f, .1f, .2f, 1},
                             std::array<float, 9>{-1, 0, 0, 0, -1, 0, 0, 0, 1}}) {
            s.projection_lights[0].light_from_world = m;
            exact(
                s, [&](const auto &v) { return oracle(s, v.wavelength); },
                "double projection nearest color/aspect/scale/shear/reflection oracle");
        }
    }
    auto s = base();
    light(s, {0, 0, 0});
    const auto projected = sample(s);
    PointLight p;
    p.position = {0, 0, 0};
    for (unsigned i = 0; i < 471; ++i)
        p.intensity[i] = 2 * pbrt_d65()[i];
    s.projection_lights.clear();
    s.point_lights.push_back(p);
    const auto points = sample(s);
    for (std::size_t i = 0; i < points.size(); ++i) {
        near(projected[i].radiance, points[i].radiance, 1e-6,
             "gray image point spectral equivalence");
        require(projected[i].random_words == points[i].random_words,
                "projection uses no random dimensions");
    }
}
void actual_inverse_boundary() {
    auto s = base();
    light(s, {0, 0, 0});
    auto &l = s.projection_lights[0];
    l.light_from_world = {1, 0, 0, 0, -1, 0, 0, 0, .002f};
    constant(s, 2 * double(s.materials[0].constant) / std::numbers::pi,
             "scale 500 remains on axis");
    l.light_from_world[8] = .0005f;
    constant(s, 0, "scale 2000 is clipped by local hither");
    l.light_from_world[8] = .001f;
    constant(s, 2 * double(s.materials[0].constant) / std::numbers::pi,
             "exact hither is inclusive");
    l.light_from_world[8] = std::nextafter(.001f, 0.f);
    constant(s, 0, "one ULP below hither is clipped");
    // Exact axis directions make these binary32 screen and texel ties stable.
    s = base();
    light(s, {0, 0, 0});
    colored_map(s, 4, 4);
    auto &b = s.projection_lights[0];
    b.inv_tan_half_fov = 1;
    for (float x : {-1.f, -.5f, 0.f, .5f, 1.f}) {
        b.light_from_world = {1, 0, x, 0, -1, 0, 0, 0, 1};
        exact(
            s, [&](const auto &v) { return oracle(s, v.wavelength); },
            "inclusive bounds and exact texel tie");
    }
    b.light_from_world[2] = std::nextafter(1.f, 2.f);
    constant(s, 0, "one ULP beyond screen bound is black");
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
    selected_mean(s, diffuse + 3 * double(s.materials[0].constant) / (4 * pi),
                  "combined projection selection preserves additive expectation");
    PointLight point;
    point.position = {0, 0, -1};
    for (unsigned i = 0; i < 471; ++i)
        point.intensity[i] = 4 * pbrt_d65()[i];
    s.point_lights.push_back(point);
    selected_mean(s, diffuse + 7 * double(s.materials[0].constant) / (4 * pi),
                  "point and projection selection preserves additive expectation");
    SpotLight spot;
    spot.position = point.position;
    spot.cos_falloff_start = spot.cos_falloff_end = 0;
    for (unsigned i = 0; i < 471; ++i)
        spot.intensity[i] = 5 * pbrt_d65()[i];
    s.spot_lights.push_back(spot);
    GoniometricLight gonio;
    gonio.position = point.position;
    for (unsigned i = 0; i < 471; ++i)
        gonio.intensity[i] = 6 * pbrt_d65()[i];
    DecodedImage scalar;
    scalar.width = scalar.height = 1;
    scalar.source_channels = 1;
    scalar.sample_format = ImageSampleFormat::float32;
    scalar.floating_rgb = {1, 1, 1};
    auto scalar_image = build_goniometric_image_base(scalar);
    require(bool(scalar_image.value), scalar_image.diagnostic);
    gonio.image_texture = static_cast<unsigned>(s.image_textures.size());
    ImageTexture scalar_texture;
    scalar_texture.image = std::make_shared<const ImagePyramid>(std::move(*scalar_image.value));
    s.image_textures.push_back(scalar_texture);
    s.goniometric_lights.push_back(gonio);
    selected_mean(s, diffuse + 18 * double(s.materials[0].constant) / (4 * pi),
                  "all four positional kinds share selection and unit delta MIS");
    s.spot_lights.clear();
    s.goniometric_lights.clear();
    s.point_lights.clear();
    s.projection_lights.resize(1);
    s = base();
    light(s, {1, 0, 0});
    const double conic = 1;
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
    s.projection_lights[0].position = {1, 0, 2};

    constant(s, dielectric_reference::evaluate(dp, {0, 0, 1}, {c, 0, -c}).f * c * conic,
             "rough dielectric radiance transmission");
    for (auto kind :
         {ScatteringKind::conductor, ScatteringKind::dielectric, ScatteringKind::thin_dielectric}) {
        m = SpectralMaterial{};
        m.scattering = kind;
        m.eta.knots = {{360, 1.5f}, {830, 1.5f}};
        if (kind == ScatteringKind::conductor)
            m.k.knots = {{360, 2.f}, {830, 2.f}};
        constant(s, 0, "delta-only surfaces have no projection NEE");
    }
}
void bounded_and_inactive() {
    auto s = base();
    for (unsigned i = 0; i < 64; ++i)
        light(s, {1, 0, 0});
    const double one = 2 * double(s.materials[0].constant) / (2 * std::sqrt(2.) * std::numbers::pi);
    constant(s, 64 * one, "all 64 projection lights contribute");
    light(s, {1, 0, 0});
    auto all_projections_oversized = build_metal_renderer(prepared(s));
    require(all_projections_oversized.status == GeometryStatus::failure &&
                !all_projections_oversized.scene,
            "direct renderer rejects 65 projections");
    s = base();
    for (unsigned i = 0; i < 32; ++i) {
        light(s, {1, 0, 0});
        PointLight p;
        p.position = {1, 0, 0};
        for (unsigned j = 0; j < 471; ++j)
            p.intensity[j] = 2 * pbrt_d65()[j];
        s.point_lights.push_back(p);
    }
    selected_mean(s, 64 * one,
                  "all 64 combined point and projection lights contribute in expectation");
    light(s, {1, 0, 0});
    auto oversized = build_metal_renderer(prepared(s));
    require(oversized.status == GeometryStatus::failure && !oversized.scene,
            "direct renderer must reject 65 combined positional lights");
    s = base();
    light(s, {1, 0, 0});
    s.max_depth = 0;
    constant(s, 0, "depth zero excludes projection NEE");
    s = base();
    s.environment.fill(1);
    s.max_depth = 3;
    const auto without = sample(s);
    light(s, {1, 0, 0}, 0);
    const auto inactive = sample(s);
    require(without.size() == inactive.size(), "inactive projection sample accounting");
    for (std::size_t i = 0; i < without.size(); ++i) {
        const auto &a = without[i];
        const auto &b = inactive[i];
        require(a.pixel == b.pixel && a.sample == b.sample && a.wavelength == b.wavelength &&
                    a.radiance == b.radiance && a.scatterings == b.scatterings &&
                    a.random_words == b.random_words,
                "zero-intensity projections preserve the no-projection sample stream");
    }
}
void visibility() {
    auto s = base();
    light(s, {1, 0, 0});
    const double e = 2 * double(s.materials[0].constant) / (2 * std::sqrt(2.) * std::numbers::pi);
    // A vertical blocker intersects only the projection segment, never the camera.
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
        constant(s, 0, "ordinary glass occludes projection shadow connections");
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
    near(average, 0, 6 * standard_error + 2e-5,
         "Beer camera and finite projection shadow segments");
}
void bounded_medium() {
    auto s = base();
    light(s, {1, 0, 0});
    s.volume_transport = true;
    HomogeneousMedium medium;
    medium.sigma_a.fill(.4f);
    s.media.push_back(medium);
    SpectralMaterial boundary;
    boundary.scattering = ScatteringKind::null_interface;
    s.materials.push_back(boundary);
    plane(s, .25f, 1);
    plane(s, .75f, 1);
    // Outward slab normals are -Z at entry and +Z at exit.
    for (auto &t : s.geometry.meshes.back().triangles)
        std::swap(t[0], t[1]);
    s.instance_media = {{}, {0, no_medium}, {0, no_medium}};
    const double e = 2 * double(s.materials[0].constant) / (2 * std::sqrt(2.) * std::numbers::pi);
    const double expected = e * std::exp(-double(.4f) * .5 * (1 + std::sqrt(2.)));
    const auto paths = sample(s, 32768);
    double sum = 0, square = 0;
    for (const auto &path : paths) {
        const double residual = path.radiance - expected;
        sum += residual;
        square += residual * residual;
    }
    const double n = double(paths.size()), mean = sum / n;
    const double se = std::sqrt(std::max(0., (square - sum * mean) / (n - 1)) / n);
    near(mean, 0, 6 * se + 2e-5,
         "bounded slab Beer camera and finite light connection through nulls");
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
        const double conic = 1;
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
             "projection quadrature refinement");
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
            std::cout << "projection volume g=" << g << " geometry=" << geometry
                      << " expected=" << expected << " residual=" << mean << " SE=" << se << '\n';
            near(mean, 0, 6 * se + 2e-5 * std::max(1., expected),
                 "independent single-scatter projection quadrature");
        }
    }
}
void invalid_inputs() {
    auto s = base();
    light(s, {0, 0, 0});
    const auto original = s.projection_lights[0];
    auto rejected = [&](ProjectionLight l) {
        s.projection_lights[0] = l;
        require(!validate_render_scene(prepared(s)).empty(), "invalid projection rejected");
    };
    auto l = original;
    l.light_from_world[8] = 0;
    rejected(l);
    l = original;
    l.light_from_world[0] = std::numeric_limits<float>::denorm_min();
    rejected(l);
    l = original;
    l.position.x = std::numeric_limits<float>::infinity();
    rejected(l);
    l = original;
    l.image_scale = -1;
    rejected(l);
    l = original;
    l.image_scale = std::numeric_limits<float>::denorm_min();
    rejected(l);
    l = original;
    l.inv_tan_half_fov = 0;
    rejected(l);
    l = original;
    l.screen_half_width = 2;
    rejected(l);
    l = original;
    l.image_texture = UINT32_MAX;
    rejected(l);
    s.projection_lights[0] = original;
    auto image = *s.image_textures[0].image;
    require(image.storage == ImageStorage::linear_float, "float fixture storage");
    image.texels[0] = std::bit_cast<std::uint32_t>(std::numeric_limits<float>::denorm_min());
    require(!validate_projection_image(image).empty(), "subnormal source RGB rejected");
    image = *s.image_textures[0].image;
    image.texels[0] = std::bit_cast<std::uint32_t>(std::numeric_limits<float>::max());
    require(!validate_projection_image(image).empty(), "RGB amplitude overflow rejected");
}
void arithmetic_failures() {
    auto s = base();
    light(s, {0, 0, 0});
    auto failure = [&](std::string_view why) {
        require(validate_render_scene(prepared(s)).empty(), validate_render_scene(prepared(s)));
        auto result = render_progressive(
            prepared(s), {.width = 1, .height = 1, .samples = 256, .batch_size = 256, .seed = 42});
        require(result.status == GeometryStatus::failure, why);
        for (const auto &p : result.film.pixels)
            require(p.count == 0, "failed projection batch never commits Film");
    };
    s.projection_lights[0].light_from_world[2] = std::numeric_limits<float>::max();
    s.projection_lights[0].inv_tan_half_fov = 2;
    failure("projective multiply overflow rejects entire batch");
    s = base();
    light(s, {0, 0, 0});
    auto image = *s.image_textures[0].image;
    // All RGB values are normal. Reconstruction multiplies amplitude by .5
    // before D65 and a huge scale; a lost intermediate must fail, never black.
    for (std::size_t i = 0; i < image.texels.size(); i += 4) {
        image.texels[i] = std::bit_cast<std::uint32_t>(std::numeric_limits<float>::min());
        image.texels[i + 1] = image.texels[i];
        image.texels[i + 2] = image.texels[i];
    }
    s.image_textures[0].image = std::make_shared<const ImagePyramid>(image);
    s.projection_lights[0].image_scale = 1e30f;
    failure("tiny normal RGB with huge positive scale rejects lost D65 intermediate");
    s = base();
    light(s, {0, 0, 0});
    image = *s.image_textures[0].image;
    for (std::size_t i = 0; i < image.texels.size(); i += 4)
        image.texels[i] = std::bit_cast<std::uint32_t>(std::numeric_limits<float>::min());
    s.image_textures[0].image = std::make_shared<const ImagePyramid>(image);
    s.projection_lights[0].image_scale = 1e30f;
    failure("RGB amplitude division cannot silently lose a nonzero channel");
    s = base();
    light(s, {0, 0, 0});
    s.projection_lights[0].position.x = .25f;
    s.projection_lights[0].light_from_world[0] = std::numeric_limits<float>::min();
    failure("normal inverse coefficient times tiny direction cannot silently flush");
}
void coincidence() {
    auto s = base();
    light(s, {0, 0, 1});
    auto failed = render_progressive(
        prepared(s), {.width = 1, .height = 1, .samples = 1, .batch_size = 1, .seed = 42});
    require(failed.status == GeometryStatus::failure,
            "active coincident projection must fail batch");
    for (const auto &p : failed.film.pixels)
        require(p.count == 0, "failed projection batch must not commit Film");
    s.projection_lights[0].image_scale = 0;
    constant(s, 0, "zero-intensity coincident projection is skipped");
}
} // namespace
int main(int argc, char **argv) try {
    bool no_device = false, no_rt = false;
    for (int i = 1; i < argc; ++i) {
        std::string_view a(argv[i]);
        if (!a.starts_with("--"))
            continue;
        require(a == "--allow-no-device" || a == "--allow-no-ray-tracing",
                "unknown projection test option");
        no_device |= a == "--allow-no-device";
        no_rt |= a == "--allow-no-ray-tracing";
    }
    auto capability = build_metal_renderer(prepared(base()));
    if ((no_device && capability.status == GeometryStatus::no_device) ||
        (no_rt && capability.status == GeometryStatus::no_ray_tracing)) {
        std::cout << "SKIP Metal projections: " << capability.diagnostic << '\n';
        return 77;
    }
    require(capability.status == GeometryStatus::success && capability.scene,
            capability.diagnostic);
    std::cerr << "projection phase: profiles_and_transforms\n";
    profiles_and_transforms();
    std::cerr << "projection phase: actual_inverse_boundary\n";
    actual_inverse_boundary();
    std::cerr << "projection phase: surfaces\n";
    surfaces();
    std::cerr << "projection phase: bounded_and_inactive\n";
    bounded_and_inactive();
    std::cerr << "projection phase: visibility\n";
    visibility();
    std::cerr << "projection phase: bounded_medium\n";
    bounded_medium();
    std::cerr << "projection phase: volume\n";
    volume();
    std::cerr << "projection phase: invalid_inputs\n";
    invalid_inputs();
    std::cerr << "projection phase: coincidence\n";
    coincidence();
    arithmetic_failures();
    std::cout << "PASS projection perspective nearest RGB/aspect/transforms, "
                 "inverse-square/cosine/additive/spectra, "
                 "rough BSDFs, finite "
                 "visibility, alpha/null/Beer, HG quadrature and failed Film\n";
    return 0;
} catch (const std::exception &e) {
    std::cerr << "Projection render: " << e.what() << '\n';
    return 1;
}
