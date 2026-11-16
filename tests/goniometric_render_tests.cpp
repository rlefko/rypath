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
            "goniometric fixture frame");
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
    GoniometricLight l;
    l.position = p;
    l.intensity.fill(intensity);
    DecodedImage image;
    image.width = image.height = 4;
    image.source_channels = 1;
    image.sample_format = ImageSampleFormat::float32;
    image.floating_rgb.assign(std::size_t{4} * 4 * 3, 1);
    auto built = build_goniometric_image_base(image);
    if (!built.value)
        throw std::runtime_error(built.diagnostic);
    ImageTexture texture;
    texture.image = std::make_shared<const ImagePyramid>(std::move(built.value.value()));
    l.image_texture = static_cast<unsigned>(s.image_textures.size());
    s.image_textures.push_back(std::move(texture));
    s.goniometric_lights.push_back(l);
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
            "goniometric sample accounting");
    for (const auto &v : result.samples)
        require(std::isfinite(v.radiance) && v.radiance >= 0,
                "finite nonnegative goniometric radiance");
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
// Independent double oracle uses exact atan rather than the renderer's minimax
// polynomial. Tests stay away from texel boundaries, where the two agree.
std::array<double, 2> equal_area(double x, double y, double z) {
    const double radius = std::sqrt(std::max(0., 1 - std::abs(z)));
    const double phi = std::atan2(std::abs(y), std::abs(x)) * 2 / std::numbers::pi;
    double u = radius * (1 - phi), v = radius * phi;
    if (z < 0) {
        const double old_u = u;
        u = 1 - v;
        v = 1 - old_u;
    }
    return {.5 * (1 + std::copysign(u, x)), .5 * (1 + std::copysign(v, y))};
}
double asymmetric_value(unsigned x, unsigned y) {
    return .125 + double((x * 37 + y * 71 + x * y * 13) % 224) / 256;
}
void asymmetric_map(RenderScene &s) {
    DecodedImage image;
    image.width = image.height = 8;
    image.source_channels = 1;
    image.sample_format = ImageSampleFormat::float32;
    for (unsigned y = 0; y < 8; ++y)
        for (unsigned x = 0; x < 8; ++x)
            for (unsigned c = 0; c < 3; ++c)
                image.floating_rgb.push_back(float(asymmetric_value(x, y)));
    auto built = build_goniometric_image_base(image);
    if (!built.value)
        throw std::runtime_error(built.diagnostic);
    s.image_textures[s.goniometric_lights[0].image_texture].image =
        std::make_shared<const ImagePyramid>(std::move(built.value.value()));
}
double map_reference(const GoniometricLight &l, double x, double y, double z) {
    const auto &a = l.light_from_world;
    const double length = std::sqrt(x * x + y * y + z * z);
    x /= length;
    y /= length;
    z /= length;
    const auto uv = equal_area(a[0] * x + a[1] * y + a[2] * z, a[3] * x + a[4] * y + a[5] * z,
                               a[6] * x + a[7] * y + a[8] * z);
    const auto col = static_cast<unsigned>(std::clamp(int(8 * uv[0]), 0, 7));
    const auto row = static_cast<unsigned>(std::clamp(int(8 * uv[1]), 0, 7));
    return asymmetric_value(col, row);
}
void profiles_and_transforms() {
    auto s = base();
    light(s, {1, .37f, -.2f});
    const auto goniometrics = sample(s);
    PointLight p;
    p.position = s.goniometric_lights[0].position;
    p.intensity = s.goniometric_lights[0].intensity;
    s.goniometric_lights.clear();
    s.point_lights.push_back(p);
    const auto points = sample(s);
    for (std::size_t i = 0; i < points.size(); ++i)
        require(points[i].radiance == goniometrics[i].radiance &&
                    points[i].random_words == goniometrics[i].random_words,
                "constant map exactly matches point and its random stream");
    s.point_lights.clear();
    light(s, p.position);
    asymmetric_map(s);
    for (const auto &matrix : {std::array<float, 9>{1, 0, 0, 0, 0, 1, 0, 1, 0},
                               std::array<float, 9>{1, 0, 0, 0, 1, 0, 0, 0, 1},
                               std::array<float, 9>{-1, 0, 0, 0, 0, 1, 0, 1, 0},
                               std::array<float, 9>{.8f, 0, .6f, -.6f, 0, .8f, 0, 1, 0}}) {
        s.goniometric_lights[0].light_from_world = matrix;
        for (const GeometryPoint position :
             {GeometryPoint{1, .37f, -.2f}, GeometryPoint{-.63f, .21f, -.7f},
              GeometryPoint{.31f, -.77f, .2f}}) {
            s.goniometric_lights[0].position = position;
            const double x = -double(position.x), y = -double(position.y),
                         z = 1 - double(position.z);
            const double distance2 = x * x + y * y + z * z;
            const double e = 2 * double(s.materials[0].constant) / std::numbers::pi * z /
                             std::pow(distance2, 1.5) *
                             map_reference(s.goniometric_lights[0], x, y, z);
            constant(s, e, "asymmetric nearest equal-area map, YZ/orientation/reflection");
        }
    }
}
void actual_inverse_boundary() {
    auto s = base();
    // These binary32 components have unit length after the renderer's ordinary
    // point-direction normalization. The accepted inverse is slightly nonunit,
    // as a narrowed inverse of an accepted almost-rigid declaration may be.
    // PBRT's pinned polynomial maps the raw vector to column 6, row 4 in an
    // 8-square map. Renormalization instead selects column 5, row 4. The raw
    // and normalized u coordinates are .7500007153 and .7499994040, respectively.
    struct Boundary {
        float x, z, inverse_z;
    };
    for (const Boundary boundary :
         {Boundary{.6614366173744202f, .750001072883606f, .999994f},
          // This second inverse comes from a declaration within 64 eps.
          // Raw/normalized u are .7500002384/.7499996424. Keeping more
          // than a rounding ULP on either side avoids dependence on a
          // sqrt/dot implementation's final rounding at an exact seam.
          Boundary{.6614382266998291f, .7499996423721313f, .9999970197677612f}}) {
        const auto [x, z, inverse_z] = boundary;
        s = base();
        light(s, {-x, 0, 1 - z});
        asymmetric_map(s);
        s.goniometric_lights[0].light_from_world = {1, 0, 0, 0, 1, 0, 0, 0, inverse_z};
        require(validate_render_scene(prepared(s)).empty(),
                "accepted actual inverse boundary fixture");
        const double distance2 = double(x) * x + double(z) * z;
        const double e = 2 * double(s.materials[0].constant) / std::numbers::pi * double(z) /
                         std::pow(distance2, 1.5) * asymmetric_value(6, 4);
        constant(s, e, "actual inverse must preserve nearest-map boundary without renormalization");
    }
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
             "sum every goniometric, no selection or MIS");
    PointLight point;
    point.position = {0, 0, -1};
    point.intensity.fill(4);
    s.point_lights.push_back(point);
    constant(s, diffuse + 7 * double(s.materials[0].constant) / (4 * pi),
             "point and goniometric contributions add without selection or MIS");
    s.point_lights.clear();
    s.goniometric_lights.resize(1);
    for (unsigned i = 0; i < 471; ++i)
        s.goniometric_lights[0].intensity[i] = .25f + float(i) / 470;
    exact(
        s,
        [&](const auto &v) {
            const auto i = std::clamp(std::lround(v.wavelength) - 360L, 0L, 470L);
            return s.goniometric_lights[0].intensity[static_cast<std::size_t>(i)] *
                   double(s.materials[0].constant) / (2 * std::sqrt(2.) * pi);
        },
        "raw wavelength intensity, independent of sensor normalization");
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
    s.goniometric_lights[0].position = {1, 0, 2};

    constant(s, dielectric_reference::evaluate(dp, {0, 0, 1}, {c, 0, -c}).f * c * conic,
             "rough dielectric radiance transmission");
    for (auto kind :
         {ScatteringKind::conductor, ScatteringKind::dielectric, ScatteringKind::thin_dielectric}) {
        m = SpectralMaterial{};
        m.scattering = kind;
        m.eta.knots = {{360, 1.5f}, {830, 1.5f}};
        if (kind == ScatteringKind::conductor)
            m.k.knots = {{360, 2.f}, {830, 2.f}};
        constant(s, 0, "delta-only surfaces have no goniometric NEE");
    }
}
void bounded_and_inactive() {
    auto s = base();
    for (unsigned i = 0; i < 64; ++i)
        light(s, {1, 0, 0});
    const double one = 2 * double(s.materials[0].constant) / (2 * std::sqrt(2.) * std::numbers::pi);
    constant(s, 64 * one, "all 64 goniometric lights contribute");
    light(s, {1, 0, 0});
    auto all_goniometrics_oversized = build_metal_renderer(prepared(s));
    require(all_goniometrics_oversized.status == GeometryStatus::failure &&
                !all_goniometrics_oversized.scene,
            "direct renderer rejects 65 goniometrics");
    s = base();
    for (unsigned i = 0; i < 32; ++i) {
        light(s, {1, 0, 0});
        PointLight p;
        p.position = {1, 0, 0};
        p.intensity.fill(2);
        s.point_lights.push_back(p);
    }
    constant(s, 64 * one, "all 64 combined point and goniometric lights contribute");
    light(s, {1, 0, 0});
    auto oversized = build_metal_renderer(prepared(s));
    require(oversized.status == GeometryStatus::failure && !oversized.scene,
            "direct renderer must reject 65 combined positional lights");
    s = base();
    light(s, {1, 0, 0});
    s.max_depth = 0;
    constant(s, 0, "depth zero excludes goniometric NEE");
    s = base();
    s.environment.fill(1);
    s.max_depth = 3;
    const auto without = sample(s);
    light(s, {1, 0, 0}, 0);
    const auto inactive = sample(s);
    require(without.size() == inactive.size(), "inactive goniometric sample accounting");
    for (std::size_t i = 0; i < without.size(); ++i) {
        const auto &a = without[i];
        const auto &b = inactive[i];
        require(a.pixel == b.pixel && a.sample == b.sample && a.wavelength == b.wavelength &&
                    a.radiance == b.radiance && a.scatterings == b.scatterings &&
                    a.random_words == b.random_words,
                "zero-intensity goniometrics preserve the no-goniometric sample stream");
    }
}
void visibility() {
    auto s = base();
    light(s, {1, 0, 0});
    const double e = 2 * double(s.materials[0].constant) / (2 * std::sqrt(2.) * std::numbers::pi);
    // A vertical blocker intersects only the goniometric segment, never the camera.
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
        constant(s, 0, "ordinary glass occludes goniometric shadow connections");
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
         "Beer camera and finite goniometric shadow segments");
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
             "goniometric quadrature refinement");
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
            std::cout << "goniometric volume g=" << g << " geometry=" << geometry
                      << " expected=" << expected << " residual=" << mean << " SE=" << se << '\n';
            near(mean, 0, 6 * se + 2e-5 * std::max(1., expected),
                 "independent single-scatter goniometric quadrature");
        }
    }
}
void invalid_inputs() {
    auto valid = base();
    light(valid, {1, 0, 0});
    auto rejected = [&](const GoniometricLight &l, std::string_view why) {
        auto s = valid;
        s.goniometric_lights[0] = l;
        require(!validate_render_scene(prepared(s)).empty(), why);
        const auto result = build_metal_renderer(prepared(s));
        require(result.status == GeometryStatus::failure && !result.scene, why);
    };
    const auto original = valid.goniometric_lights[0];
    for (auto matrix : {std::array<float, 9>{2, 0, 0, 0, 1, 0, 0, 0, 1},
                        std::array<float, 9>{1, .2f, 0, 0, 1, 0, 0, 0, 1},
                        std::array<float, 9>{1, 0, 0, 0, 0, 0, 0, 0, 1}}) {
        auto l = original;
        l.light_from_world = matrix;
        rejected(l, "nonrigid goniometric direction map rejected");
    }
    auto l = original;
    l.intensity[117] = -1;
    rejected(l, "negative spectral intensity rejected");
    l = original;
    l.position.x = std::numeric_limits<float>::quiet_NaN();
    rejected(l, "nonfinite position rejected");
    l = original;
    l.image_texture = UINT32_MAX;
    rejected(l, "missing angular map rejected");
}
void coincidence() {
    auto s = base();
    light(s, {0, 0, 1});
    auto failed = render_progressive(
        prepared(s), {.width = 1, .height = 1, .samples = 1, .batch_size = 1, .seed = 42});
    require(failed.status == GeometryStatus::failure,
            "active coincident goniometric must fail batch");
    for (const auto &p : failed.film.pixels)
        require(p.count == 0, "failed goniometric batch must not commit Film");
    s.goniometric_lights[0].intensity.fill(0);
    constant(s, 0, "zero-intensity coincident goniometric is skipped");
}
} // namespace
int main(int argc, char **argv) try {
    bool no_device = false, no_rt = false;
    for (int i = 1; i < argc; ++i) {
        std::string_view a(argv[i]);
        if (!a.starts_with("--"))
            continue;
        require(a == "--allow-no-device" || a == "--allow-no-ray-tracing",
                "unknown goniometric test option");
        no_device |= a == "--allow-no-device";
        no_rt |= a == "--allow-no-ray-tracing";
    }
    auto capability = build_metal_renderer(prepared(base()));
    if ((no_device && capability.status == GeometryStatus::no_device) ||
        (no_rt && capability.status == GeometryStatus::no_ray_tracing)) {
        std::cout << "SKIP Metal goniometrics: " << capability.diagnostic << '\n';
        return 77;
    }
    require(capability.status == GeometryStatus::success && capability.scene,
            capability.diagnostic);
    std::cerr << "goniometric phase: profiles_and_transforms\n";
    profiles_and_transforms();
    std::cerr << "goniometric phase: actual_inverse_boundary\n";
    actual_inverse_boundary();
    std::cerr << "goniometric phase: surfaces\n";
    surfaces();
    std::cerr << "goniometric phase: bounded_and_inactive\n";
    bounded_and_inactive();
    std::cerr << "goniometric phase: visibility\n";
    visibility();
    std::cerr << "goniometric phase: bounded_medium\n";
    bounded_medium();
    std::cerr << "goniometric phase: volume\n";
    volume();
    std::cerr << "goniometric phase: invalid_inputs\n";
    invalid_inputs();
    std::cerr << "goniometric phase: coincidence\n";
    coincidence();
    std::cout << "PASS goniometric equal-area nearest/transforms, "
                 "inverse-square/cosine/additive/spectra, "
                 "rough BSDFs, finite "
                 "visibility, alpha/null/Beer, HG quadrature and failed Film\n";
    return 0;
} catch (const std::exception &e) {
    std::cerr << "Goniometric render: " << e.what() << '\n';
    return 1;
}
