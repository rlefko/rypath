// SPDX-License-Identifier: GPL-3.0-only
#include "pbrt_render.hpp"

#include "pbrt_numeric.hpp"
#include "rgb_spectrum.hpp"
#include "triangle_attributes.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <map>
#include <new>
#include <numbers>
#include <sstream>
#include <string_view>
#include <utility>

namespace rypath::pbrt {
namespace {

using std::string_view;
using Vector = std::array<double, 3>;
enum class SpectrumRole { reflectance, emission };
enum class ReflectanceSource { material, texture };

const Parameter *parameter(const Descriptor &descriptor, string_view name) {
    return descriptor.effective_parameter(name).parameter;
}

double number(const Descriptor &descriptor, string_view name, double fallback) {
    const auto *selected = parameter(descriptor, name);
    return selected ? selected->values.front().number : fallback;
}

std::int32_t integer(const Descriptor &descriptor, string_view name, std::int32_t fallback) {
    const auto *selected = parameter(descriptor, name);
    return selected ? selected->values.front().integer : fallback;
}

std::optional<string_view> string(const Descriptor &descriptor, string_view name) {
    const auto *selected = parameter(descriptor, name);
    return selected ? std::optional<string_view>(selected->values.front().text) : std::nullopt;
}

bool boolean(const Descriptor &descriptor, string_view name, bool fallback) {
    const auto *selected = parameter(descriptor, name);
    return selected ? selected->values.front().boolean : fallback;
}

const Location &location(const Descriptor &descriptor, string_view name) {
    const auto *selected = parameter(descriptor, name);
    return selected ? selected->location : descriptor.authored.location;
}

bool float_representable(double value) {
    return std::abs(value) <= std::numeric_limits<float>::max();
}

double dot(const Vector &a, const Vector &b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

Vector cross(const Vector &a, const Vector &b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}

Vector to_vector(const GeometryPoint &point) { return {point.x, point.y, point.z}; }

std::optional<GeometryPoint> narrow_point(const Vector &vector) {
    if (!std::ranges::all_of(vector, float_representable)) {
        return std::nullopt;
    }
    return GeometryPoint{static_cast<float>(vector[0]), static_cast<float>(vector[1]),
                         static_cast<float>(vector[2])};
}

int handedness(const GeometryTransform &transform) {
    Transform expanded;
    std::ranges::copy(transform.values, expanded.values.begin());
    return exact_linear_determinant_sign(expanded);
}

class Translator {
  public:
    Translator(const Scene &input, const Location &entry, const Limits &limits)
        : input_(input), entry_(entry), limits_(limits) {
        scene_.model = RenderModel::pbrt;
    }

    RenderTranslation run();

  private:
    const Scene &input_;
    const Location &entry_;
    const Limits &limits_;
    Document diagnostics_;
    RenderTranslation result_;
    RenderScene scene_;
    std::size_t spectral_values_ = 0;
    bool spectral_limit_exceeded_ = false;
    std::map<string_view, std::uint32_t> spectrum_textures_;

    void issue(Severity severity, string_view code, const Location &source, std::string message);
    void unsupported(const Location &source, std::string message);
    void invalid(const Location &source, std::string message);
    const Descriptor *required(string_view name);
    std::optional<float> narrow(const Descriptor &descriptor, string_view name, double fallback);
    void film(const Descriptor &descriptor);
    void camera(const Descriptor &descriptor, const Descriptor *film);
    void filter(const Descriptor &descriptor);
    void sampler(const Descriptor &descriptor);
    void integrator(const Descriptor &descriptor);
    bool charge_spectral_values(const Parameter &parameter);
    std::optional<std::vector<SpectralKnot>> spectrum(const Descriptor &descriptor,
                                                      string_view name, SpectrumRole role);
    bool supports_rgb_space(string_view color_space, const Location &source);
    bool emission(const Descriptor &descriptor, double light_multiplier,
                  std::array<float, 471> &dense);
    void light(const Descriptor &descriptor);
    void constant_textures();
    void image_textures();
    void texture_frames();
    std::uint32_t reflectance(const Descriptor &descriptor, ReflectanceSource source);
    std::uint32_t material(const Descriptor &descriptor);
    void geometry();
};

void Translator::issue(Severity severity, string_view code, const Location &source,
                       std::string message) {
    diagnose(diagnostics_, limits_, severity, std::string(code), source, std::move(message));
}

void Translator::unsupported(const Location &source, std::string message) {
    issue(Severity::unsupported, "unsupported-render-feature", source, std::move(message));
}

void Translator::invalid(const Location &source, std::string message) {
    issue(Severity::error, "invalid-render-value", source, std::move(message));
}

const Descriptor *Translator::required(string_view name) {
    const auto found = std::ranges::find_if(
        input_.options, [name](const Descriptor &option) { return option.authored.name == name; });
    if (found != input_.options.end()) {
        return &*found;
    }
    unsupported(entry_, "The spectral render profile requires an explicit " + std::string(name));
    return nullptr;
}

std::optional<float> Translator::narrow(const Descriptor &descriptor, string_view name,
                                        double fallback) {
    const double value = number(descriptor, name, fallback);
    if (!float_representable(value)) {
        invalid(location(descriptor, name), std::string(name) + " overflows binary32");
        return std::nullopt;
    }
    return static_cast<float>(value);
}

void Translator::film(const Descriptor &descriptor) {
    auto &settings = result_.settings;
    settings.width = static_cast<std::uint32_t>(integer(descriptor, "xresolution", 1280));
    settings.height = static_cast<std::uint32_t>(integer(descriptor, "yresolution", 720));
    if (descriptor.color_space != "srgb") {
        unsupported(descriptor.authored.location, "Only the sRGB film color space is supported");
    }
    if (string(descriptor, "sensor").value_or("cie1931") != "cie1931") {
        unsupported(location(descriptor, "sensor"), "Only the CIE 1931 film sensor is supported");
    }
    if (number(descriptor, "whitebalance", 0) != 0) {
        unsupported(location(descriptor, "whitebalance"), "Film white balance is unsupported");
    }
    if (const auto *crop = parameter(descriptor, "cropwindow")) {
        if (!std::ranges::equal(crop->values, std::array{0., 1., 0., 1.}, {}, &Value::number)) {
            unsupported(crop->location, "Only full-frame film cropwindow is supported");
        }
    } else if (const auto *bounds = parameter(descriptor, "pixelbounds")) {
        const std::array expected{0, static_cast<int>(settings.width), 0,
                                  static_cast<int>(settings.height)};
        if (!std::ranges::equal(bounds->values, expected, {}, &Value::integer)) {
            unsupported(bounds->location, "Only full-frame film pixelbounds is supported");
        }
    }
    if (const auto *limit = parameter(descriptor, "maxcomponentvalue")) {
        unsupported(limit->location, "Film sample clamping is unsupported");
    }
    result_.provenance.emplace_back("CLI PNG output overrides PBRT filename and savefp16");
}

void Translator::camera(const Descriptor &descriptor, const Descriptor *film_descriptor) {
    if (number(descriptor, "lensradius", 0) != 0) {
        unsupported(location(descriptor, "lensradius"), "The perspective camera must be pinhole");
    }
    const auto open = narrow(descriptor, "shutteropen", 0);
    const auto close = narrow(descriptor, "shutterclose", 1);
    const auto iso = film_descriptor ? narrow(*film_descriptor, "iso", 100) : std::optional{100.f};
    if (open && close && iso) {
        const double duration = static_cast<double>(*close) - *open;
        if (!float_representable(duration) || duration <= 0) {
            invalid(descriptor.authored.location,
                    "Camera shutter duration must remain finite and positive in binary32");
        } else if (*iso <= 0) {
            invalid(location(*film_descriptor, "iso"), "Film ISO collapses to zero in binary32");
        } else {
            const double product = static_cast<double>(static_cast<float>(duration)) * *iso;
            const float exposure =
                float_representable(product) ? static_cast<float>(product) / 100 : 0;
            if (exposure <= 0) {
                invalid(descriptor.authored.location,
                        "Film exposure must remain finite and positive in binary32");
            } else {
                scene_.exposure = exposure;
            }
        }
    }

    const auto &m = descriptor.transform.values;
    const Vector x{m[0], m[4], m[8]}, y{m[1], m[5], m[9]}, z{m[2], m[6], m[10]};
    constexpr double tolerance = 64 * std::numeric_limits<float>::epsilon();
    const std::array gram_errors{dot(x, x) - 1,
                                 dot(y, y) - 1,
                                 dot(z, z) - 1,
                                 dot(x, y),
                                 dot(x, z),
                                 dot(y, z),
                                 std::abs(dot(x, cross(y, z))) - 1};
    if (!std::ranges::all_of(gram_errors,
                             [](double error) { return std::abs(error) <= tolerance; })) {
        unsupported(descriptor.authored.location, "Camera transform must be rigid");
        return;
    }
    if (!input_.spectrum_image_textures.empty()) {
        const std::array linear{m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10]};
        if (const auto message = prepare_texture_camera(linear, scene_.texture_camera);
            !message.empty()) {
            invalid(descriptor.authored.location, message);
            return;
        }
    }
    const auto origin = narrow_point({m[3], m[7], m[11]});
    if (!origin) {
        invalid(descriptor.authored.location, "Camera origin overflows binary32");
        return;
    }

    const auto fov = narrow(descriptor, "fov", 90);
    const auto aspect =
        narrow(descriptor, "frameaspectratio",
               static_cast<double>(result_.settings.width) / result_.settings.height);
    if (!fov || !aspect) {
        return;
    }
    if (!(*fov > 0 && *fov < 180)) {
        invalid(location(descriptor, "fov"), "Camera FOV collapses in binary32");
        return;
    }
    if (*aspect <= 0) {
        invalid(location(descriptor, "frameaspectratio"),
                "Camera aspect ratio collapses in binary32");
        return;
    }
    const double horizontal = std::max(1., static_cast<double>(*aspect));
    const double vertical = std::max(1., 1. / *aspect);
    std::array<double, 4> screen{-horizontal, horizontal, -vertical, vertical};
    if (const auto *window = parameter(descriptor, "screenwindow")) {
        for (std::size_t i = 0; i < screen.size(); ++i) {
            if (!float_representable(window->values[i].number)) {
                invalid(window->values[i].location, "Camera screen window overflows binary32");
                return;
            }
            screen[i] = static_cast<float>(window->values[i].number);
        }
    }

    const auto source = location(descriptor, "screenwindow");
    if (!(screen[0] < screen[1] && screen[2] < screen[3]) ||
        !std::ranges::all_of(screen, float_representable)) {
        invalid(source, "Camera screen window must remain finite and ordered in binary32");
        return;
    }
    const double tangent = std::tan(*fov * std::numbers::pi / 360);
    Vector corner{}, dx{}, dy{};
    for (std::size_t i = 0; i < corner.size(); ++i) {
        corner[i] = z[i] + tangent * (screen[0] * x[i] + screen[3] * y[i]);
        dx[i] = tangent * (screen[1] - screen[0]) * x[i];
        dy[i] = tangent * (screen[2] - screen[3]) * y[i];
    }
    const auto packed_corner = narrow_point(corner);
    const auto packed_dx = narrow_point(dx);
    const auto packed_dy = narrow_point(dy);
    if (!packed_corner || !packed_dx || !packed_dy ||
        dot(to_vector(*packed_corner), cross(to_vector(*packed_dx), to_vector(*packed_dy))) == 0) {
        invalid(source, "Camera raster coefficients overflow or collapse in binary32");
        return;
    }
    const RasterCamera raster{origin.value(), packed_corner.value(), packed_dx.value(),
                              packed_dy.value()};
    if (scene_.filter.kind == PixelFilterKind::box) {
        const auto c = to_vector(raster.corner);
        const auto u = to_vector(raster.dx);
        const auto v = to_vector(raster.dy);
        for (std::size_t axis = 0; axis < c.size(); ++axis) {
            // Each component is affine on the raster, so its extrema are at the corners.
            const std::array extremes{c[axis] + u[axis], c[axis] + v[axis],
                                      c[axis] + u[axis] + v[axis]};
            if (!std::ranges::all_of(extremes, float_representable)) {
                invalid(source, "Camera raster direction overflows binary32 at a film corner");
                return;
            }
        }
    }
    scene_.raster_camera = raster;
}

void Translator::filter(const Descriptor &descriptor) {
    if (descriptor.authored.arguments.front().text == "triangle") {
        scene_.filter.kind = PixelFilterKind::triangle;
        const auto radius = [&](string_view name) {
            const auto value = narrow(descriptor, name, 2.);
            if (value && (!std::isnormal(*value) || *value <= 0)) {
                invalid(location(descriptor, name),
                        std::string(name) + " must be positive and normal in binary32");
            }
            return value.value_or(0.f);
        };
        scene_.filter.x_radius = radius("xradius");
        scene_.filter.y_radius = radius("yradius");
        return;
    }
    for (const auto name : {"xradius", "yradius"}) {
        if (number(descriptor, name, 0.5) != 0.5) {
            unsupported(location(descriptor, name), "Box filter radius must be 0.5 on each axis");
        }
    }
}

void Translator::sampler(const Descriptor &descriptor) {
    auto &settings = result_.settings;
    const bool sobol = descriptor.authored.arguments.front().text == "sobol";
    settings.samples =
        static_cast<std::uint64_t>(integer(descriptor, "pixelsamples", sobol ? 16 : 4));
    settings.seed = static_cast<std::uint64_t>(integer(descriptor, "seed", 0));
    if (!sobol) {
        return;
    }
    settings.sampler.kind = SamplerKind::sobol;
    const auto randomization = string(descriptor, "randomization").value_or("fastowen");
    if (randomization == "none") {
        settings.sampler.randomization = SobolRandomization::none;
    } else if (randomization == "permutedigits") {
        settings.sampler.randomization = SobolRandomization::permutedigits;
    } else if (randomization == "owen") {
        settings.sampler.randomization = SobolRandomization::owen;
    }
    if (!std::has_single_bit(settings.samples)) {
        issue(Severity::warning, "sobol-non-power-of-two", location(descriptor, "pixelsamples"),
              "Sobol pixelsamples is not a power of two; rendering the requested " +
                  std::to_string(settings.samples) + " samples per pixel");
    }
}

void Translator::integrator(const Descriptor &descriptor) {
    const auto depth = static_cast<std::uint32_t>(integer(descriptor, "maxdepth", 5));
    const bool sobol = result_.settings.sampler.kind == SamplerKind::sobol;
    const auto limit = sobol ? sobol_max_depth : render_emergency_limit;
    if (depth > limit) {
        unsupported(
            location(descriptor, "maxdepth"),
            std::string(sobol ? "Sobol path maxdepth exceeds the limit of "
                              : "Path maxdepth exceeds the spectral render profile limit of ") +
                std::to_string(limit));
    }
    scene_.max_depth = depth;
    if (boolean(descriptor, "regularize", false)) {
        unsupported(location(descriptor, "regularize"), "Path regularization is unsupported");
    }
    if (string(descriptor, "lightsampler").value_or("bvh") != "bvh") {
        unsupported(location(descriptor, "lightsampler"),
                    "Only the bvh light sampler is supported");
    }
}

bool Translator::charge_spectral_values(const Parameter &parameter) {
    const auto count = parameter.values.size();
    if (count > limits_.list_values || count > limits_.total_values - spectral_values_) {
        if (!spectral_limit_exceeded_) {
            issue(Severity::error, "limit-exceeded", parameter.location,
                  "Translated spectrum or texture value limit exceeded");
            spectral_limit_exceeded_ = true;
        }
        return false;
    }
    spectral_values_ += count;
    return true;
}

std::optional<std::vector<SpectralKnot>> Translator::spectrum(const Descriptor &descriptor,
                                                              string_view name, SpectrumRole role) {
    const auto *selected = parameter(descriptor, name);
    if (!selected || selected->type != "spectrum" ||
        selected->values.front().kind == ValueKind::string || selected->values.size() < 4) {
        unsupported(location(descriptor, name),
                    std::string(name) + " requires at least two numeric wavelength/value pairs");
        return std::nullopt;
    }
    if (!charge_spectral_values(*selected)) {
        return std::nullopt;
    }

    const auto &values = selected->values;
    const double maximum =
        role == SpectrumRole::reflectance ? 1. : std::numeric_limits<double>::infinity();
    std::vector<SpectralKnot> knots;
    knots.reserve(values.size() / 2);
    const Value *outside_range = nullptr;
    for (std::size_t i = 0; i < values.size(); i += 2) {
        const auto &wavelength = values[i];
        const auto &value = values[i + 1];
        if (!float_representable(wavelength.number)) {
            invalid(wavelength.location, "Spectrum wavelength overflows binary32");
            return std::nullopt;
        }
        if (!float_representable(value.number)) {
            invalid(value.location, "Spectrum value overflows binary32");
            return std::nullopt;
        }
        const float lambda = static_cast<float>(wavelength.number);
        if (!knots.empty() && lambda <= knots.back().wavelength) {
            invalid(wavelength.location, "Spectrum wavelengths must increase in binary32");
            return std::nullopt;
        }
        if (!outside_range && (value.number < 0 || value.number > maximum)) {
            outside_range = &value;
        }
        knots.push_back({lambda, static_cast<float>(value.number)});
    }
    if (outside_range) {
        unsupported(outside_range->location, role == SpectrumRole::reflectance
                                                 ? "Diffuse reflectance must be within [0, 1]"
                                                 : "Emission spectrum must be nonnegative");
        return std::nullopt;
    }
    return knots;
}

bool Translator::supports_rgb_space(string_view color_space, const Location &source) {
    if (color_space == "srgb") {
        return true;
    }
    unsupported(source, "Only linear sRGB reconstruction is supported (effective ColorSpace \"" +
                            std::string(color_space) + "\")");
    return false;
}

bool Translator::emission(const Descriptor &descriptor, double light_multiplier,
                          std::array<float, 471> &dense) {
    const auto scale = narrow(descriptor, "scale", 1);
    const auto [selected, color_space] = descriptor.effective_parameter("L");
    const auto &source = selected ? selected->location : descriptor.authored.location;
    double normalization = 0;
    float rgb_scale = 1;
    if (!selected || selected->type == "rgb") {
        if (selected) {
            if (!charge_spectral_values(*selected)) {
                return false;
            }
            const auto &peak = *std::ranges::max_element(selected->values, {}, &Value::number);
            if (peak.number > std::numeric_limits<float>::max() / 2.) {
                invalid(peak.location,
                        "Twice the maximum emission RGB component overflows binary32");
                return false;
            }
            rgb_scale = 2 * static_cast<float>(peak.number);
        }
        if (!supports_rgb_space(selected ? color_space : descriptor.color_space, source)) {
            return false;
        }
        dense = pbrt_d65();
        // PBRT's photometric normalization uses the RGB spectrum's base illuminant.
        normalization = pbrt_y_inner_product(dense);
        if (selected) {
            const auto &values = selected->values;
            std::array<float, 3> rgb{};
            if (rgb_scale > 0) {
                std::ranges::transform(values, rgb.begin(), [rgb_scale](const Value &value) {
                    return static_cast<float>(value.number) / rgb_scale;
                });
            }
            const auto tint = reconstruct_srgb(rgb);
            if (!tint) {
                invalid(source, "Emission RGB reconstruction failed");
                return false;
            }
            for (std::size_t i = 0; i < dense.size(); ++i) {
                dense[i] *= evaluate_srgb(*tint, 360.f + static_cast<float>(i));
            }
        }
    } else {
        auto knots = spectrum(descriptor, "L", SpectrumRole::emission);
        if (!knots) {
            return false;
        }
        SpectralMaterial curve;
        curve.knots = std::move(*knots);
        for (std::size_t i = 0; i < dense.size(); ++i) {
            dense[i] =
                static_cast<float>(spectral_reflectance(curve, 360. + static_cast<double>(i)));
        }
        normalization = pbrt_y_inner_product(dense);
    }
    if (!scale) {
        return false;
    }
    if (!(normalization > 0 && std::isfinite(normalization))) {
        invalid(source, "Emission spectrum requires a positive finite CIE Y inner product");
        return false;
    }
    const double amplitude =
        static_cast<double>(rgb_scale) * *scale * light_multiplier / normalization;
    for (auto &value : dense) {
        const double normalized = value * amplitude;
        if (!float_representable(normalized)) {
            invalid(source, "Normalized emission spectrum overflows binary32");
            return false;
        }
        value = static_cast<float>(normalized);
    }
    return true;
}

void Translator::light(const Descriptor &descriptor) {
    const auto illuminance = narrow(descriptor, "illuminance", -1);
    const double multiplier =
        illuminance && *illuminance > 0 ? *illuminance / std::numbers::pi : 1.;
    emission(descriptor, multiplier, scene_.environment);
}

std::uint32_t Translator::reflectance(const Descriptor &descriptor, ReflectanceSource source) {
    const string_view name = source == ReflectanceSource::texture ? "value" : "reflectance";
    const auto [selected, value_space] = descriptor.effective_parameter(name);
    // Named textures use captured value provenance; inline RGB uses its material dictionary.
    const string_view color_space =
        source == ReflectanceSource::texture ? value_space : descriptor.color_space;
    SpectralMaterial material;
    if (!selected) {
        // Named constant textures default to one, independently of the material default.
        material.kind = SpectralMaterialKind::constant;
        material.constant = 1;
    } else if (selected->type == "rgb") {
        if (!charge_spectral_values(*selected) ||
            !supports_rgb_space(color_space, selected->location)) {
            return 0;
        }
        std::array<float, 3> rgb{};
        std::ranges::transform(selected->values, rgb.begin(),
                               [](const Value &value) { return static_cast<float>(value.number); });
        const auto reflectance = reconstruct_srgb(rgb);
        if (!reflectance) {
            invalid(selected->location, "Diffuse RGB reflectance reconstruction failed");
            return 0;
        }
        material.kind = reflectance->is_constant ? SpectralMaterialKind::constant
                                                 : SpectralMaterialKind::rgb_sigmoid;
        material.constant = reflectance->constant;
        material.coefficients = reflectance->coefficients;
    } else {
        auto knots = spectrum(descriptor, name, SpectrumRole::reflectance);
        if (!knots) {
            return 0;
        }
        material.knots = std::move(*knots);
    }
    const auto index = static_cast<std::uint32_t>(scene_.materials.size());
    scene_.materials.push_back(std::move(material));
    return index;
}

void Translator::constant_textures() {
    for (const auto &descriptor : input_.float_constant_textures) {
        if (diagnostics_.diagnostics_truncated) {
            return;
        }
        if (const auto *selected = parameter(descriptor, "value");
            selected && charge_spectral_values(*selected)) {
            // Float consumers remain unsupported, but unused definitions are checked too.
            narrow(descriptor, "value", 1);
        }
    }
    for (const auto &descriptor : input_.spectrum_constant_textures) {
        if (diagnostics_.diagnostics_truncated) {
            return;
        }
        const auto index = reflectance(descriptor, ReflectanceSource::texture);
        spectrum_textures_.emplace(descriptor.authored.arguments.front().text, index);
    }
}

void Translator::image_textures() {
    for (const auto &definition : input_.spectrum_image_textures) {
        if (diagnostics_.diagnostics_truncated) {
            return;
        }
        const auto index = static_cast<std::uint32_t>(scene_.materials.size());
        SpectralMaterial material;
        material.kind = SpectralMaterialKind::image;
        material.image_texture = static_cast<std::uint32_t>(scene_.image_textures.size());
        scene_.image_textures.push_back(definition.texture);
        scene_.materials.push_back(std::move(material));
        spectrum_textures_.emplace(definition.descriptor.authored.arguments.front().text, index);
    }
}

void Translator::texture_frames() {
    if (scene_.image_textures.empty() || !scene_.raster_camera) {
        return;
    }
    try {
        scene_.instance_texture_frames.resize(scene_.geometry.instances.size());
    } catch (const std::bad_alloc &) {
        issue(Severity::error, "limit-exceeded", entry_, "Cannot allocate instance texture frames");
        return;
    }
    std::size_t remaining = limits_.total_values;
    std::size_t instance = 0;
    const auto prepare = [&](const Shape &shape, const Transform &placement,
                             const Location &source) {
        auto &frames = scene_.instance_texture_frames[instance];
        const auto material = scene_.instance_materials[instance++];
        if (scene_.materials[material].kind != SpectralMaterialKind::image) {
            return true;
        }
        const auto &mesh = input_.geometry.meshes[shape.mesh_index];
        constexpr std::size_t coordinates_per_frame = 6;
        if (mesh.triangles.size() > remaining / coordinates_per_frame) {
            issue(Severity::error, "limit-exceeded", source,
                  "Expanded triangle texture frame value limit exceeded");
            return false;
        }
        remaining -= mesh.triangles.size() * coordinates_per_frame;
        try {
            if (const auto message = prepare_texture_triangle_frames(
                    mesh, shape.descriptor.transform.values, placement.values, frames,
                    {scene_.raster_camera->origin.x, scene_.raster_camera->origin.y,
                     scene_.raster_camera->origin.z});
                !message.empty()) {
                invalid(source, message);
            }
        } catch (const std::bad_alloc &) {
            issue(Severity::error, "limit-exceeded", source,
                  "Cannot allocate triangle texture frames");
            return false;
        }
        return !diagnostics_.diagnostics_truncated;
    };
    const Transform identity;
    // Checked geometry emits the direct shapes before expanding object placements.
    for (const auto &shape : input_.shapes) {
        if (!prepare(shape, identity, shape.descriptor.authored.location)) {
            return;
        }
    }
    std::map<string_view, const Object *> objects;
    for (const auto &object : input_.objects) {
        objects.emplace(object.name, &object);
    }
    for (const auto &placement : input_.placements) {
        for (const auto &shape : objects.at(placement.object)->shapes) {
            if (!prepare(shape, placement.transform, placement.location)) {
                return;
            }
        }
    }
}

std::uint32_t Translator::material(const Descriptor &descriptor) {
    const auto *selected = parameter(descriptor, "reflectance");
    if (!selected) {
        return 0;
    }
    if (selected->type == "texture") {
        const auto found = spectrum_textures_.find(selected->values.front().text);
        if (found == spectrum_textures_.end()) {
            invalid(selected->location, "Diffuse reflectance requires a checked spectrum texture");
            return 0;
        }
        return found->second;
    }
    return reflectance(descriptor, ReflectanceSource::material);
}

void Translator::geometry() {
    const auto &geometry = input_.geometry;
    if (geometry.instances.size() > limits_.total_values ||
        geometry.meshes.size() > limits_.total_values ||
        input_.inline_materials.size() > limits_.directives ||
        input_.named_materials.size() > limits_.directives ||
        input_.float_constant_textures.size() > limits_.directives ||
        input_.spectrum_constant_textures.size() > limits_.directives ||
        input_.spectrum_image_textures.size() > limits_.directives ||
        input_.area_lights.size() > limits_.directives) {
        issue(Severity::error, "limit-exceeded", entry_,
              "Translated geometry, material, texture, or light limit exceeded");
        return;
    }
    std::size_t remaining = limits_.total_values;
    for (const auto &mesh : geometry.meshes) {
        for (const auto &[count, width] :
             {std::pair{mesh.positions.size(), 3U}, std::pair{mesh.triangles.size(), 3U},
              std::pair{mesh.normals.size(), 3U}, std::pair{mesh.uvs.size(), 2U},
              std::pair{mesh.patches.size(), 4U}}) {
            if (count > remaining / width) {
                issue(Severity::error, "limit-exceeded", entry_,
                      "Translated geometry value limit exceeded");
                return;
            }
            remaining -= count * width;
        }
    }
    if (std::ranges::none_of(geometry.instances, [&](const GeometryInstance &instance) {
            const auto &mesh = geometry.meshes[instance.mesh_index];
            return !mesh.triangles.empty() || !mesh.patches.empty();
        })) {
        unsupported(
            entry_,
            "The spectral render profile requires at least one placed triangle or bilinear patch");
    }

    SpectralMaterial default_material;
    default_material.knots = {{360, 0.5f}, {830, 0.5f}};
    scene_.materials.push_back(std::move(default_material));
    constant_textures();
    image_textures();
    std::map<string_view, std::uint32_t> named;
    std::map<const Descriptor *, std::uint32_t> inline_materials;
    for (const auto &descriptor : input_.named_materials) {
        if (diagnostics_.diagnostics_truncated) {
            return;
        }
        named.emplace(descriptor.authored.arguments.front().text, material(descriptor));
    }
    for (const auto &descriptor : input_.inline_materials) {
        if (diagnostics_.diagnostics_truncated) {
            return;
        }
        inline_materials.emplace(descriptor.get(), material(*descriptor));
    }

    struct MeshBinding {
        std::uint32_t material = 0;
        const Shape *shape = nullptr;
    };
    std::vector<MeshBinding> bindings(geometry.meshes.size());
    std::map<const Descriptor *, std::uint32_t> area_lights;
    const auto bind = [&](const Shape &shape) {
        const auto &binding = shape.material;
        auto &mesh = bindings[shape.mesh_index];
        mesh.shape = &shape;
        if (shape.area_light) {
            area_lights.try_emplace(shape.area_light.get(), no_area_emission);
        }
        if (!binding.name->empty()) {
            mesh.material = named.at(*binding.name);
        } else if (binding.inline_material) {
            mesh.material = inline_materials.at(binding.inline_material.get());
        }
    };
    std::ranges::for_each(input_.shapes, bind);
    for (const auto &object : input_.objects) {
        std::ranges::for_each(object.shapes, bind);
    }

    for (const auto &descriptor : input_.area_lights) {
        if (diagnostics_.diagnostics_truncated) {
            return;
        }
        const bool positive_power = number(*descriptor, "power", -1) > 0;
        if (positive_power) {
            unsupported(location(*descriptor, "power"),
                        "Positive power for diffuse area lights is unsupported");
        }
        AreaEmission area;
        area.two_sided = boolean(*descriptor, "twosided", false);
        const auto placed = area_lights.find(descriptor.get());
        if (!emission(*descriptor, 1., area.radiance) || positive_power ||
            placed == area_lights.end()) {
            continue;
        }
        if (area.radiance.size() > limits_.total_values - spectral_values_ ||
            scene_.area_emissions.size() >= no_area_emission) {
            issue(Severity::error, "limit-exceeded", descriptor->authored.location,
                  "Translated area light table limit exceeded");
            return;
        }
        spectral_values_ += area.radiance.size();
        placed->second = static_cast<std::uint32_t>(scene_.area_emissions.size());
        scene_.area_emissions.push_back(area);
    }

    scene_.geometry = geometry;
    scene_.instance_materials.reserve(geometry.instances.size());
    scene_.instance_reverse_orientation.reserve(geometry.instances.size());
    if (!area_lights.empty()) {
        scene_.instance_emissions.reserve(geometry.instances.size());
    }
    // Every placement prepares normals here and during portable scene validation.
    std::size_t normal_coordinates_remaining = limits_.total_values;
    std::size_t patch_corners_remaining = std::min(limits_.total_values, render_patch_corner_limit);
    for (const auto &instance : geometry.instances) {
        const auto &mesh = geometry.meshes[instance.mesh_index];
        if (mesh.patches.size() > patch_corners_remaining / 4) {
            issue(Severity::error, "limit-exceeded",
                  bindings[instance.mesh_index].shape->descriptor.authored.location,
                  "Expanded patch validation corner limit exceeded");
            return;
        }
        patch_corners_remaining -= mesh.patches.size() * 4;
    }
    std::vector<GeometryPoint> shading_normals;
    for (const auto &instance : geometry.instances) {
        if (diagnostics_.diagnostics_truncated) {
            return;
        }
        const auto &binding = bindings[instance.mesh_index];
        const auto &mesh = geometry.meshes[instance.mesh_index];
        const auto &shape = *binding.shape;
        if (const auto message = validate_patch_geometry(mesh, instance.transform);
            !message.empty()) {
            invalid(shape.descriptor.authored.location, message);
        }
        scene_.instance_materials.push_back(binding.material);
        // PBRT applies the mesh and placement transforms to normals. A world-space
        // edge cross product has an additional sign for a mirrored transform.
        const bool mirrored = handedness(instance.transform) < 0;
        const bool reverse_winding = shape.reverse_orientation != mirrored;
        scene_.instance_reverse_orientation.push_back(reverse_winding);

        if (mesh.normals.size() > normal_coordinates_remaining / 3) {
            issue(Severity::error, "limit-exceeded", shape.descriptor.authored.location,
                  "Expanded shading normal value limit exceeded");
            return;
        }
        normal_coordinates_remaining -= mesh.normals.size() * 3;
        const auto normal_error =
            prepare_shading_normals(mesh, instance.transform, reverse_winding, shading_normals);
        if (!normal_error.empty()) {
            invalid(shape.descriptor.authored.location, normal_error);
        }
        if (!area_lights.empty()) {
            const auto emission_index =
                shape.area_light ? area_lights.at(shape.area_light.get()) : no_area_emission;
            scene_.instance_emissions.push_back(emission_index);
            if (shape.area_light) {
                const auto message = validate_area_geometry(mesh, instance.transform);
                if (!message.empty()) {
                    invalid(shape.descriptor.authored.location, message);
                } else if (normal_error.empty() && emission_index != no_area_emission) {
                    const auto &area = scene_.area_emissions[emission_index];
                    const auto normal_message = validate_normal_emission(
                        mesh, instance.transform, reverse_winding, area.two_sided, shading_normals);
                    if (!normal_message.empty()) {
                        unsupported(shape.descriptor.authored.location, normal_message);
                    }
                }
            }
        }
    }
}

RenderTranslation Translator::run() {
    const auto *film_descriptor = required("Film");
    if (film_descriptor) {
        film(*film_descriptor);
    }
    const auto *filter_descriptor = required("PixelFilter");
    if (filter_descriptor) {
        filter(*filter_descriptor);
    }
    if (const auto *descriptor = required("Camera")) {
        camera(*descriptor, film_descriptor);
    }
    const auto *sampler_descriptor = required("Sampler");
    if (sampler_descriptor) {
        sampler(*sampler_descriptor);
    }
    if (const auto *descriptor = required("Integrator")) {
        integrator(*descriptor);
    }
    for (const auto &option : input_.options) {
        if (option.authored.name != "Accelerator") {
            continue;
        }
        if (option.effective_authored.empty()) {
            result_.provenance.emplace_back("PBRT bvh uses native Metal acceleration");
        } else {
            unsupported(option.authored.location, "Only a bare bvh accelerator is supported");
        }
    }

    if (input_.lights.size() > 1) {
        unsupported(input_.lights[1].authored.location, "Only one infinite light is supported");
    }
    for (const auto &descriptor : input_.lights) {
        if (diagnostics_.diagnostics_truncated) {
            break;
        }
        light(descriptor);
    }
    if (!diagnostics_.diagnostics_truncated) {
        geometry();
    }
    if (!diagnostics_.diagnostics_truncated &&
        scene_.instance_materials.size() == input_.geometry.instances.size()) {
        texture_frames();
    }
    const auto &settings = result_.settings;
    std::string sampler_error;
    if (sampler_descriptor && film_descriptor && settings.sampler.kind == SamplerKind::sobol) {
        sampler_error = validate_sampler_range(settings.sampler, settings.seed, settings.width,
                                               settings.height, 0, settings.samples);
        if (!sampler_error.empty()) {
            const auto &source =
                sobol_scale_exponent(settings.width, settings.height) > sobol_max_scale_exponent
                    ? location(*film_descriptor,
                               settings.width >= settings.height ? "xresolution" : "yresolution")
                    : location(*sampler_descriptor, "pixelsamples");
            invalid(source, sampler_error);
        }
    }
    if (const auto message = validate_render_settings(settings);
        sampler_error.empty() && !message.empty()) {
        invalid(film_descriptor ? film_descriptor->authored.location : entry_, message);
    }

    const auto has = [&](Severity severity) {
        return std::ranges::any_of(
            diagnostics_.diagnostics,
            [severity](const Diagnostic &diagnostic) { return diagnostic.severity == severity; });
    };
    if (filter_descriptor && scene_.filter.kind == PixelFilterKind::triangle &&
        scene_.raster_camera && !has(Severity::error) && !diagnostics_.diagnostics_truncated) {
        const auto message = validate_raster_support(
            *scene_.raster_camera, scene_.filter, result_.settings.width, result_.settings.height);
        if (!message.empty()) {
            invalid(filter_descriptor->authored.location, message);
        }
    }
    if (diagnostics_.diagnostics_truncated || has(Severity::error)) {
        result_.status = 1;
    } else if (has(Severity::unsupported)) {
        result_.status = 2;
    } else if (const auto message = validate_render_scene(scene_); !message.empty()) {
        invalid(entry_, message);
        result_.status = 1;
    } else {
        result_.status = 0;
        std::ostringstream filter_provenance;
        filter_provenance.imbue(std::locale::classic());
        filter_provenance << "Pixel filter: "
                          << (scene_.filter.kind == PixelFilterKind::triangle ? "triangle" : "box")
                          << std::setprecision(std::numeric_limits<float>::max_digits10)
                          << " (x radius " << scene_.filter.x_radius << ", y radius "
                          << scene_.filter.y_radius << ')';
        result_.provenance.push_back(filter_provenance.str());
        if (!scene_.image_textures.empty()) {
            result_.provenance.emplace_back(
                "Image textures: PNG, UV mapping, mipmapped bilinear filtering");
        }
        result_.scene = std::move(scene_);
        result_.provenance.push_back("CIE 1931 sensor exposure uses ISO and shutter duration");
    }
    result_.diagnostics = std::move(diagnostics_.diagnostics);
    return std::move(result_);
}

} // namespace

RenderTranslation translate(const Result &checked, const Location &entry, const Limits &limits) {
    if (const int status = checked.exit_code(); status != 0 || !checked.scene) {
        RenderTranslation result;
        result.status = status != 0 ? status : 1;
        return result;
    }
    return Translator(*checked.scene, entry, limits).run();
}

} // namespace rypath::pbrt
