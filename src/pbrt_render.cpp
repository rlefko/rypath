// SPDX-License-Identifier: GPL-3.0-only
#include "pbrt_render.hpp"

#include "mesh_displacement.hpp"
#include "pbrt_numeric.hpp"
#include "pbrt_spectra.hpp"
#include "reflectance_curve.hpp"
#include "rgb_spectrum.hpp"
#include "texture_graph.hpp"
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
#include <set>
#include <span>
#include <sstream>
#include <string_view>
#include <utility>

namespace rypath::pbrt {
namespace {

using std::string_view;
using Vector = std::array<double, 3>;
namespace graph = texture_graph_wire;
enum class SpectrumRole { reflectance, emission, graph_operand };
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

double resample_emission(std::span<const SpectralKnot> knots, std::array<float, 471> &dense) {
    for (std::size_t i = 0; i < dense.size(); ++i) {
        dense[i] = static_cast<float>(sampled_spectrum(knots, 360. + static_cast<double>(i)));
    }
    return pbrt_y_inner_product(dense);
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
    std::size_t displacement_work_remaining_ = DisplacementLimits{}.evaluation_work;
    std::map<string_view, std::uint32_t> spectrum_textures_;
    std::map<string_view, std::uint32_t> image_indices_;
    std::map<string_view, std::uint32_t> float_image_indices_;
    std::array<std::map<string_view, std::uint32_t>, 2> texture_roots_;
    std::set<string_view> nested_spectra_;
    std::set<string_view> direct_spectra_;
    std::map<const ImagePyramid *, std::uint32_t> native_images_;
    std::map<const ImagePyramid *, ImageFileFormat> image_formats_;
    std::vector<const Descriptor *> graph_mapping_sources_;

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
    void spectral_limit(const Location &source);
    bool charge_spectral_values(const Parameter &parameter);
    bool charge_file_spectrum(const Location &source, std::size_t knots, std::size_t samples);
    std::optional<std::vector<SpectralKnot>> reflectance_curve(const Descriptor &descriptor,
                                                               const Parameter &selected);
    std::optional<std::vector<SpectralKnot>> spectrum(const Descriptor &descriptor,
                                                      string_view name, SpectrumRole role);
    bool supports_rgb_space(string_view color_space, const Location &source);
    std::optional<float> blackbody_temperature(const Parameter &parameter);
    bool blackbody_emission(const Parameter &parameter, std::optional<float> scale,
                            double light_multiplier, std::array<float, 471> &dense);
    bool emission(const Descriptor &descriptor, double light_multiplier,
                  std::array<float, 471> &dense);
    void light(const Descriptor &descriptor);
    std::optional<float> image_emission(const Descriptor &descriptor, float illuminance = -1);
    std::uint32_t native_image(const Descriptor &descriptor);
    void constant_textures();
    void image_textures();
    void compile_textures();
    std::uint32_t append_node(const graph::Node &node, const Descriptor *mapping = nullptr);
    std::uint32_t append_material(SpectralMaterial material);
    std::uint32_t spectrum_leaf(const SpectralMaterial &material);
    std::uint32_t spectrum_leaf(std::span<const SpectralKnot> knots);
    std::uint32_t raw_spectrum_operand(const Descriptor &descriptor, const Parameter &selected);
    std::uint32_t operand(const Descriptor &descriptor, string_view name, bool spectrum,
                          float fallback);
    std::uint32_t float_binding(const Descriptor &descriptor, string_view name);
    std::uint32_t compile_texture(const Descriptor &descriptor);
    void displace_geometry(std::size_t remaining);
    void rebase_texture_mappings();
    void texture_frames();
    std::optional<SpectralMaterial> reflectance(const Descriptor &descriptor,
                                                ReflectanceSource source, string_view name,
                                                float fallback);
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
    result_.save_fp16 = boolean(descriptor, "savefp16", true);
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
    const std::array linear{m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10]};
    if (const auto message = prepare_texture_camera(linear, scene_.texture_camera);
        !message.empty()) {
        invalid(descriptor.authored.location, message);
        return;
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

void Translator::spectral_limit(const Location &source) {
    if (!spectral_limit_exceeded_) {
        issue(Severity::error, "limit-exceeded", source,
              "Translated spectrum or texture value limit exceeded");
        spectral_limit_exceeded_ = true;
    }
}

bool Translator::charge_spectral_values(const Parameter &parameter) {
    const auto count = parameter.values.size();
    if (count > limits_.list_values || count > limits_.total_values - spectral_values_) {
        spectral_limit(parameter.location);
        return false;
    }
    spectral_values_ += count;
    return true;
}

bool Translator::charge_file_spectrum(const Location &source, std::size_t knots,
                                      std::size_t samples) {
    const auto remaining = limits_.total_values - spectral_values_;
    // Each knot consumes two decoded scalars. Charge the dense pass even for unplaced lights.
    if (knots > limits_.list_values / 2 || samples > remaining ||
        knots > (remaining - samples) / 2) {
        spectral_limit(source);
        return false;
    }
    spectral_values_ += 2 * knots + samples;
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
        if (role != SpectrumRole::graph_operand && !outside_range &&
            (value.number < 0 || value.number > maximum)) {
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

std::optional<float> Translator::blackbody_temperature(const Parameter &parameter) {
    if (!charge_spectral_values(parameter)) {
        return std::nullopt;
    }
    const auto &temperature = parameter.values.front();
    if (!float_representable(temperature.number)) {
        invalid(temperature.location, "Blackbody temperature overflows binary32");
        return std::nullopt;
    }
    const auto kelvin = static_cast<float>(temperature.number);
    if (!(kelvin > 0)) {
        invalid(temperature.location, "Blackbody temperature must be positive in binary32");
        return std::nullopt;
    }
    return kelvin;
}

bool Translator::blackbody_emission(const Parameter &parameter, std::optional<float> scale,
                                    double light_multiplier, std::array<float, 471> &dense) {
    const auto temperature = blackbody_temperature(parameter);
    if (!temperature || !scale) {
        return false;
    }
    const double kelvin = *temperature;

    // Pinned PBRT v4 Blackbody constants. Express h*c/k in nm K.
    constexpr double c2 = 6.62606957e-34 * 299792458. / 1.3806488e-23 * 1e9;
    constexpr double reference = 830.;
    const double reference_factor = -std::expm1(-(c2 / reference) / kelvin);
    std::array<double, 471> shape{};
    double normalization = 0;
    for (std::size_t i = 0; i < shape.size(); ++i) {
        const double wavelength = 360. + static_cast<double>(i);
        // Divide Planck radiance by its 830 nm value. Factoring the exponentials
        // keeps the exponent nonpositive without subtracting large exponents.
        const double delta = c2 * (reference - wavelength) / (reference * wavelength);
        shape[i] = std::pow(reference / wavelength, 5) * std::exp(-delta / kelvin) *
                   reference_factor / -std::expm1(-(c2 / wavelength) / kelvin);
        normalization += shape[i] * pbrt_cie_xyz(wavelength)[1];
    }
    if (!(normalization > 0 && std::isfinite(normalization))) {
        invalid(parameter.location,
                "Emission spectrum requires a positive finite CIE Y inner product");
        return false;
    }
    const double amplitude = *scale * light_multiplier / normalization;
    for (std::size_t i = 0; i < dense.size(); ++i) {
        const double normalized = shape[i] * amplitude;
        if (!float_representable(normalized)) {
            invalid(parameter.location, "Normalized emission spectrum overflows binary32");
            return false;
        }
        dense[i] = static_cast<float>(normalized);
    }
    return true;
}

bool Translator::emission(const Descriptor &descriptor, double light_multiplier,
                          std::array<float, 471> &dense) {
    const auto scale = narrow(descriptor, "scale", 1);
    const auto [selected, color_space] = descriptor.effective_parameter("L");
    if (selected && selected->type == "blackbody") {
        return blackbody_emission(*selected, scale, light_multiplier, dense);
    }
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
    } else if (selected->type == "spectrum" && selected->values.front().kind == ValueKind::string) {
        const auto *named = pbrt_named_spectrum(selected->values.front().text);
        if (named) {
            if (!charge_spectral_values(*selected)) {
                return false;
            }
            dense = *named;
            normalization = pbrt_y_inner_product(dense);
        } else {
            const auto *file = descriptor.checked_spectrum("L");
            if (!file) {
                invalid(source, "Emission spectrum filename requires a checked spectrum file");
                return false;
            }
            const auto &knots = file->knots;
            if (!charge_file_spectrum(source, knots.size(), dense.size())) {
                return false;
            }
            if (std::ranges::any_of(knots,
                                    [](const SpectralKnot &knot) { return knot.value < 0; })) {
                unsupported(source, "Emission spectrum must be nonnegative");
                return false;
            }
            normalization = resample_emission(knots, dense);
        }
    } else {
        auto knots = spectrum(descriptor, "L", SpectrumRole::emission);
        if (!knots) {
            return false;
        }
        normalization = resample_emission(*knots, dense);
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

std::uint32_t Translator::native_image(const Descriptor &descriptor) {
    if (!descriptor.native_image) {
        return graph::no_texture;
    }
    auto [entry, inserted] = native_images_.try_emplace(
        descriptor.native_image.get(), static_cast<std::uint32_t>(scene_.image_textures.size()));
    if (inserted) {
        scene_.image_textures.push_back({descriptor.native_image});
    }
    return entry->second;
}

std::optional<float> Translator::image_emission(const Descriptor &descriptor, float illuminance) {
    if (const auto error = validate_image_light(*descriptor.native_image); !error.empty()) {
        invalid(location(descriptor, "filename"), error);
        return std::nullopt;
    }
    const auto scale = narrow(descriptor, "scale", 1);
    if (!scale) {
        return std::nullopt;
    }
    double amplitude = *scale / pbrt_y_inner_product(pbrt_d65());
    if (illuminance > 0) {
        // PBRT integrates the upper hemisphere of its equal-area image in light space.
        // Only the mapping's z component is needed for this cosine-weighted sum.
        const auto &image = *descriptor.native_image;
        const auto &base = image.levels.front();
        double sum = 0;
        // The Y row of the inverse of the PBRT film's D65-based sRGB matrix.
        constexpr std::array<double, 3> luminance{0.212673499227, 0.715151622536, 0.072174878238};
        for (std::uint32_t y = 0; y < base.height; ++y) {
            const float t = (static_cast<float>(y) + 0.5f) / static_cast<float>(base.height);
            for (std::uint32_t x = 0; x < base.width; ++x) {
                const float s = (static_cast<float>(x) + 0.5f) / static_cast<float>(base.width);
                const double radius = std::abs(2. * s - 1) + std::abs(2. * t - 1);
                if (radius >= 1) {
                    continue;
                }
                const auto rgb = image_base_linear_rgb(image, s, t);
                if (!rgb) {
                    invalid(location(descriptor, "filename"), "Infinite light image lookup failed");
                    return std::nullopt;
                }
                const auto &[r, g, b] = *rgb;
                const double y_value = r * luminance[0] + g * luminance[1] + b * luminance[2];
                const double z = 1 - radius * radius;
                sum += y_value * z;
            }
        }
        const double integral =
            sum * 2 * std::numbers::pi / (static_cast<double>(base.width) * base.height);
        if (!(integral > 0)) {
            invalid(location(descriptor, "illuminance"),
                    "Image light illuminance requires positive upper-hemisphere image luminance");
            return std::nullopt;
        }
        amplitude *= illuminance / integral;
    }
    // RGBIlluminant reconstruction multiplies the final amplitude by up to 2.
    if (!float_representable(amplitude * 2)) {
        invalid(location(descriptor, "scale"), "Image emission scale overflows binary32");
        return std::nullopt;
    }
    return static_cast<float>(amplitude);
}

void Translator::light(const Descriptor &descriptor) {
    const auto illuminance = narrow(descriptor, "illuminance", -1);
    if (descriptor.native_image) {
        const auto &m = descriptor.transform.values;
        const Vector x{m[0], m[4], m[8]}, y{m[1], m[5], m[9]}, z{m[2], m[6], m[10]};
        constexpr double tolerance = 64 * std::numeric_limits<float>::epsilon();
        const std::array gram_errors{dot(x, x) - 1, dot(y, y) - 1, dot(z, z) - 1,
                                     dot(x, y),     dot(x, z),     dot(y, z)};
        if (!std::ranges::all_of(gram_errors,
                                 [](double error) { return std::abs(error) <= tolerance; })) {
            unsupported(descriptor.authored.location,
                        "Infinite image light transform must have an orthonormal linear part");
            return;
        }
        if (illuminance) {
            if (const auto scale = image_emission(descriptor, *illuminance)) {
                scene_.environment_image = native_image(descriptor);
                scene_.environment_image_scale = *scale;
                scene_.environment_texture_from_world.values = {
                    static_cast<float>(m[0]),  static_cast<float>(m[4]),
                    static_cast<float>(m[8]),  0,
                    static_cast<float>(m[1]),  static_cast<float>(m[5]),
                    static_cast<float>(m[9]),  0,
                    static_cast<float>(m[2]),  static_cast<float>(m[6]),
                    static_cast<float>(m[10]), 0};
            }
        }
    } else {
        const double multiplier =
            illuminance && *illuminance > 0 ? *illuminance / std::numbers::pi : 1.;
        emission(descriptor, multiplier, scene_.environment);
    }
}

std::optional<std::vector<SpectralKnot>> Translator::reflectance_curve(const Descriptor &descriptor,
                                                                       const Parameter &selected) {
    if (!charge_spectral_values(selected)) {
        return std::nullopt;
    }
    auto raw = pbrt_named_spectrum_knots(selected.values.front().text);
    if (raw.empty()) {
        const auto *file = descriptor.checked_spectrum(selected.name);
        if (!file) {
            invalid(selected.location, "Reflectance filename requires a checked spectrum file");
            return std::nullopt;
        }
        raw = file->knots;
    }
    // Account for source processing and generated storage separately, including
    // unused definitions. Constant-texture references share the resulting payload.
    if (!charge_file_spectrum(selected.location, raw.size(), 0)) {
        return std::nullopt;
    }
    const auto capacity =
        std::min(limits_.list_values / 2, (limits_.total_values - spectral_values_) / 2);
    auto compiled = compile_reflectance_curve(raw, capacity);
    if (compiled.error == ReflectanceCurveError::limit_exceeded) {
        spectral_limit(selected.location);
        return std::nullopt;
    }
    if (compiled.error != ReflectanceCurveError::none) {
        invalid(selected.location,
                "Reflectance requires at least two finite, strictly increasing spectrum knots");
        return std::nullopt;
    }
    spectral_values_ += 2 * compiled.knots.size();
    return std::move(compiled.knots);
}

std::optional<SpectralMaterial> Translator::reflectance(const Descriptor &descriptor,
                                                        ReflectanceSource source, string_view name,
                                                        float fallback) {
    const auto [selected, value_space] = descriptor.effective_parameter(name);
    // Named textures use captured value provenance; inline RGB uses its material dictionary.
    const string_view color_space =
        source == ReflectanceSource::texture ? value_space : descriptor.color_space;
    SpectralMaterial material;
    if (!selected) {
        material.kind = SpectralMaterialKind::constant;
        material.constant = fallback;
    } else if (selected->type == "rgb" || selected->type == "color") {
        if (!charge_spectral_values(*selected) ||
            !supports_rgb_space(color_space, selected->location)) {
            return std::nullopt;
        }
        std::array<float, 3> rgb{};
        std::ranges::transform(selected->values, rgb.begin(),
                               [](const Value &value) { return static_cast<float>(value.number); });
        const auto reflectance = reconstruct_srgb(rgb);
        if (!reflectance) {
            invalid(selected->location, "Diffuse RGB reflectance reconstruction failed");
            return std::nullopt;
        }
        material.kind = reflectance->is_constant ? SpectralMaterialKind::constant
                                                 : SpectralMaterialKind::rgb_sigmoid;
        material.constant = reflectance->constant;
        material.coefficients = reflectance->coefficients;
    } else if (selected->type == "blackbody") {
        const auto temperature = blackbody_temperature(*selected);
        if (!temperature) {
            return std::nullopt;
        }
        material.kind = SpectralMaterialKind::blackbody;
        material.temperature_kelvin = *temperature;
    } else if (selected->type == "spectrum" && selected->values.front().kind == ValueKind::string) {
        auto knots = reflectance_curve(descriptor, *selected);
        if (!knots) {
            return std::nullopt;
        }
        material.knots = std::move(*knots);
    } else {
        auto knots = spectrum(descriptor, name, SpectrumRole::reflectance);
        if (!knots) {
            return std::nullopt;
        }
        material.knots = std::move(*knots);
    }
    return material;
}

std::uint32_t Translator::append_material(SpectralMaterial material) {
    const auto index = static_cast<std::uint32_t>(scene_.materials.size());
    scene_.materials.push_back(std::move(material));
    return index;
}

std::uint32_t Translator::append_node(const graph::Node &node, const Descriptor *mapping) {
    const auto index = static_cast<std::uint32_t>(scene_.texture_graph.nodes.size());
    scene_.texture_graph.nodes.push_back(node);
    graph_mapping_sources_.push_back(node.mapping.kind == graph::uv ? nullptr : mapping);
    return index;
}

std::uint32_t Translator::spectrum_leaf(const SpectralMaterial &material) {
    graph::Node node;
    node.type = graph::spectrum;
    switch (material.kind) {
    case SpectralMaterialKind::sampled:
        return spectrum_leaf(material.knots);
    case SpectralMaterialKind::constant:
        node.values[0] = material.constant;
        break;
    case SpectralMaterialKind::rgb_sigmoid:
        node.kind = graph::rgb_sigmoid;
        std::ranges::copy(material.coefficients, node.values);
        break;
    case SpectralMaterialKind::blackbody:
        node.kind = graph::blackbody;
        node.values[0] = material.temperature_kelvin;
        break;
    default:
        return graph::no_texture;
    }
    return append_node(node);
}

std::uint32_t Translator::spectrum_leaf(std::span<const SpectralKnot> knots) {
    graph::Node node;
    node.type = graph::spectrum;
    node.kind = graph::knots;
    node.payload[0] = static_cast<std::uint32_t>(scene_.texture_graph.knots.size());
    node.payload[1] = static_cast<std::uint32_t>(knots.size());
    for (const auto &knot : knots) {
        scene_.texture_graph.knots.push_back({knot.wavelength, knot.value});
    }
    return append_node(node);
}

std::uint32_t Translator::raw_spectrum_operand(const Descriptor &descriptor,
                                               const Parameter &selected) {
    if (selected.values.front().kind != ValueKind::string) {
        const auto knots = spectrum(descriptor, selected.name, SpectrumRole::graph_operand);
        return knots ? spectrum_leaf(*knots) : graph::no_texture;
    }
    if (!charge_spectral_values(selected)) {
        return graph::no_texture;
    }
    auto knots = pbrt_named_spectrum_knots(selected.values.front().text);
    if (knots.empty()) {
        const auto *file = descriptor.checked_spectrum(selected.name);
        if (!file) {
            invalid(selected.location, "Reflectance filename requires a checked spectrum file");
            return graph::no_texture;
        }
        knots = file->knots;
    }
    if (!charge_file_spectrum(selected.location, knots.size(), 0)) {
        return graph::no_texture;
    }
    float previous = -std::numeric_limits<float>::infinity();
    for (const auto &knot : knots) {
        if (!(knot.wavelength > previous) || !std::isfinite(knot.wavelength) ||
            !std::isfinite(knot.value)) {
            invalid(selected.location,
                    "Spectrum operand requires finite, strictly increasing knots");
            return graph::no_texture;
        }
        previous = knot.wavelength;
    }
    if (knots.size() < 2) {
        invalid(selected.location, "Spectrum operand requires at least two knots");
        return graph::no_texture;
    }
    return spectrum_leaf(knots);
}

void Translator::constant_textures() {
    for (const auto &descriptor : input_.spectrum_constant_textures) {
        if (diagnostics_.diagnostics_truncated) {
            return;
        }
        const auto &name = descriptor.authored.arguments.front().text;
        const auto *selected = parameter(descriptor, "value");
        if (selected && selected->type == "spectrum" && nested_spectra_.contains(name) &&
            !direct_spectra_.contains(name)) {
            continue; // Only the graph consumes this unclipped curve.
        }
        auto value = reflectance(descriptor, ReflectanceSource::texture, "value", 1);
        spectrum_textures_.emplace(name, value ? append_material(std::move(*value)) : 0);
    }
}

void Translator::image_textures() {
    const auto add_image = [&](const auto &definition, bool spectrum) {
        const auto &name = definition.descriptor.authored.arguments.front().text;
        const auto index = static_cast<std::uint32_t>(scene_.image_textures.size());
        scene_.image_textures.push_back(definition.texture);
        image_formats_.try_emplace(definition.texture.image.get(),
                                   definition.source->pixels->file_format);
        if (spectrum) {
            image_indices_.emplace(name, index);
            if (definition.descriptor.texture_mapping.kind == graph::uv) {
                SpectralMaterial material;
                material.kind = SpectralMaterialKind::image;
                material.image_texture = index;
                spectrum_textures_.emplace(name, append_material(std::move(material)));
            }
        } else {
            float_image_indices_.emplace(name, index);
        }
    };
    for (const auto &definition : input_.spectrum_image_textures) {
        add_image(definition, true);
    }
    for (const auto &definition : input_.float_image_textures) {
        add_image(definition, false);
    }
}

std::uint32_t Translator::operand(const Descriptor &descriptor, string_view name, bool spectrum,
                                  float fallback) {
    const auto *selected = parameter(descriptor, name);
    if (selected && selected->type == "texture") {
        const auto found = texture_roots_[spectrum].find(selected->values.front().text);
        return found == texture_roots_[spectrum].end() ? graph::no_texture : found->second;
    }
    if (spectrum) {
        if (selected && selected->type == "spectrum") {
            return raw_spectrum_operand(descriptor, *selected);
        }
        // Bilerp reads standalone spectra. Texture-valued operands use the
        // declaration's color space for inline RGB, as diffuse reflectance does.
        const auto source = name == "v00" || name == "v01" || name == "v10" || name == "v11"
                                ? ReflectanceSource::texture
                                : ReflectanceSource::material;
        const auto value = reflectance(descriptor, source, name, fallback);
        return value ? spectrum_leaf(*value) : graph::no_texture;
    }
    if (selected && !charge_spectral_values(*selected)) {
        return graph::no_texture;
    }
    const auto value = narrow(descriptor, name, fallback);
    if (!value) {
        return graph::no_texture;
    }
    graph::Node node;
    node.values[0] = *value;
    return append_node(node);
}

std::uint32_t Translator::float_binding(const Descriptor &descriptor, string_view name) {
    const auto *selected = parameter(descriptor, name);
    if (!selected || (selected->type == "texture" && selected->values.front().text.empty())) {
        return graph::no_texture;
    }
    return operand(descriptor, name, false, 0);
}

std::uint32_t Translator::compile_texture(const Descriptor &descriptor) {
    const auto &arguments = descriptor.authored.arguments;
    const auto &name = arguments.front().text;
    const auto &type = arguments[2].text;
    const bool spectrum = arguments[1].text == "spectrum";
    if (type == "constant") {
        if (!spectrum) {
            return operand(descriptor, "value", false, 1);
        }
        if (nested_spectra_.contains(name)) {
            if (const auto *selected = parameter(descriptor, "value");
                selected && selected->type == "spectrum") {
                return raw_spectrum_operand(descriptor, *selected);
            }
        }
        const auto found = spectrum_textures_.find(name);
        return found == spectrum_textures_.end() || found->second == 0
                   ? graph::no_texture
                   : spectrum_leaf(scene_.materials[found->second]);
    }
    graph::Node node;
    node.type = spectrum ? graph::spectrum : graph::scalar;
    node.mapping = descriptor.texture_mapping;
    bool valid = true;
    const auto child = [&](std::size_t index, string_view field, float fallback,
                           bool scalar = false) {
        node.children[index] = operand(descriptor, field, spectrum && !scalar, fallback);
        valid &= node.children[index] != graph::no_texture;
    };
    if (type == "imagemap") {
        node.kind = graph::image;
        node.payload[0] = spectrum ? image_indices_.at(name) : float_image_indices_.at(name);
    } else if (type == "scale") {
        node.kind = graph::scale;
        child(0, "tex", 1);
        child(1, "scale", 1, true);
        if (valid) {
            // PBRT folds an identity scalar and merges a constant factor with an
            // image's scale before inversion and spectrum reconstruction.
            const auto fold_image =
                [&](std::uint32_t image_index,
                    std::uint32_t factor_index) -> std::optional<std::uint32_t> {
                const auto &factor = scene_.texture_graph.nodes[factor_index];
                if (factor.kind != graph::constant_node || factor.type != graph::scalar) {
                    return std::nullopt;
                }
                if (factor.values[0] == 1) {
                    return image_index;
                }
                auto leaf = scene_.texture_graph.nodes[image_index];
                if (leaf.kind != graph::image) {
                    return std::nullopt;
                }
                auto image = scene_.image_textures[leaf.payload[0]];
                image.scale *= factor.values[0];
                if (!std::isfinite(image.scale)) {
                    invalid(location(descriptor, "scale"),
                            "Scaled imagemap scale overflows binary32");
                    return graph::no_texture;
                }
                leaf.payload[0] = static_cast<std::uint32_t>(scene_.image_textures.size());
                scene_.image_textures.push_back(std::move(image));
                return append_node(leaf, graph_mapping_sources_[image_index]);
            };
            if (auto folded = fold_image(node.children[0], node.children[1])) {
                return *folded;
            }
            if (!spectrum) {
                if (auto folded = fold_image(node.children[1], node.children[0])) {
                    return *folded;
                }
            }
        }
    } else if (type == "mix" || type == "directionmix" || type == "checkerboard") {
        const bool checker = type == "checkerboard";
        node.kind = checker         ? graph::checkerboard
                    : type == "mix" ? graph::mix
                                    : graph::directionmix;
        child(0, "tex1", checker ? 1 : 0);
        child(1, "tex2", checker ? 0 : 1);
        if (type == "mix") {
            child(2, "amount", 0.5f, true);
        } else if (!checker) {
            std::ranges::copy(descriptor.texture_direction, node.values);
        }
    } else if (type == "bilerp") {
        node.kind = graph::bilerp;
        child(0, "v00", 0);
        child(1, "v01", 1);
        child(2, "v10", 0);
        child(3, "v11", 1);
    } else if (type == "dots") {
        node.kind = graph::dots;
        // The pinned constructors' positional arguments reverse the named operands.
        child(0, "inside", 1);
        child(1, "outside", 0);
    } else if (type == "windy") {
        node.kind = graph::windy;
    } else {
        node.kind = type == "fbm"        ? graph::fbm
                    : type == "wrinkled" ? graph::wrinkled
                                         : graph::marble;
        node.payload[0] = static_cast<std::uint32_t>(integer(descriptor, "octaves", 8));
        node.values[0] = static_cast<float>(number(descriptor, "roughness", 0.5));
        if (type == "marble") {
            node.values[1] = static_cast<float>(number(descriptor, "scale", 1));
            node.values[2] = static_cast<float>(number(descriptor, "variation", 0.2));
        }
    }
    return valid ? append_node(node, &descriptor) : graph::no_texture;
}

void Translator::compile_textures() {
    const auto mark_direct_spectrum = [&](const Descriptor &material) {
        if (const auto *value = parameter(material, "reflectance");
            value && value->type == "texture") {
            direct_spectra_.insert(value->values.front().text);
        }
    };
    std::ranges::for_each(input_.named_materials, mark_direct_spectrum);
    for (const auto &descriptor : input_.inline_materials) {
        mark_direct_spectrum(*descriptor);
    }
    for (const auto &descriptor : input_.procedural_textures) {
        if (descriptor.authored.arguments[1].text != "spectrum") {
            continue;
        }
        for (const auto field : {"tex", "tex1", "tex2", "inside", "outside"}) {
            if (const auto *operand = parameter(descriptor, field);
                operand && operand->type == "texture") {
                nested_spectra_.insert(operand->values.front().text);
            }
        }
    }
    constant_textures();
    image_textures();
    for (const auto ref : input_.texture_order) {
        if (diagnostics_.diagnostics_truncated) {
            return;
        }
        const auto &descriptor = input_.texture_descriptor(ref);
        const auto &arguments = descriptor.authored.arguments;
        const auto root = compile_texture(descriptor);
        if (root != graph::no_texture) {
            texture_roots_[arguments[1].text == "spectrum"].emplace(arguments.front().text, root);
        }
    }
}

void Translator::displace_geometry(std::size_t remaining) {
    std::vector<graph::Word> work;
    const auto prepare = [&](const Shape &shape) {
        const auto &descriptor = shape.descriptor;
        const auto displacement = string(descriptor, "displacement");
        if (!displacement || displacement->empty() || diagnostics_.diagnostics_truncated) {
            return;
        }
        const auto root = float_binding(descriptor, "displacement");
        if (root == graph::no_texture) {
            return;
        }
        if (work.empty()) {
            const auto message =
                validate_texture_graph(scene_.texture_graph, scene_.image_textures.size(), &work);
            if (!message.empty()) {
                invalid(location(descriptor, "displacement"), message);
                return;
            }
        }
        const auto edge_length = narrow(descriptor, "edgelength", 1);
        if (!edge_length) {
            return;
        }
        const float scaled_length = *edge_length * input_.displacement_edge_scale;
        const TextureImageSampler images{
            &scene_.image_textures,
            [](const void *user, graph::Word image, bool spectrum, graph::Mapped2D coordinates,
               float) -> std::optional<float> {
                const auto &textures = *static_cast<const std::vector<ImageTexture> *>(user);
                if (spectrum || image >= textures.size()) {
                    return std::nullopt;
                }
                const auto &texture = textures[image];
                const auto value =
                    image_linear_scalar(*texture.image, coordinates.s, 1 - coordinates.t,
                                        coordinates.dsdx, coordinates.dtdx, coordinates.dsdy,
                                        coordinates.dtdy, texture.filter, texture.max_anisotropy);
                if (!value || !std::isfinite(*value)) {
                    return std::nullopt;
                }
                const float scaled = *value * texture.scale;
                if (!std::isfinite(scaled)) {
                    return std::nullopt;
                }
                return texture.invert ? std::max(0.f, 1 - scaled) : scaled;
            }};
        auto &mesh = scene_.geometry.meshes[shape.mesh_index];
        const auto original_values = 3 * (mesh.positions.size() + mesh.normals.size() +
                                          mesh.triangles.size() + mesh.tangents.size()) +
                                     2 * mesh.uvs.size() + 4 * mesh.patches.size();
        const auto evaluate = [&](GeometryPoint p, std::array<float, 2> uv) {
            graph::Context context{};
            context.p = {p.x, p.y, p.z};
            context.u = uv[0];
            context.v = uv[1];
            return evaluate_texture_graph(scene_.texture_graph, root, context, 550, images);
        };
        DisplacementLimits limits;
        limits.scalar_values = remaining + original_values;
        limits.evaluation_work = displacement_work_remaining_;
        limits.work_per_vertex = work[root];
        auto result =
            displace_mesh(mesh, descriptor.transform.values, scaled_length, evaluate, limits);
        displacement_work_remaining_ -= result.evaluation_work;
        if (!result.mesh) {
            issue(Severity::error,
                  result.limit_exceeded ? "limit-exceeded" : "invalid-render-value",
                  location(descriptor, "displacement"), std::move(result.diagnostic));
            return;
        }
        remaining = limits.scalar_values - result.scalar_values;
        mesh = std::move(*result.mesh);
    };
    std::ranges::for_each(input_.shapes, prepare);
    for (const auto &object : input_.objects) {
        std::ranges::for_each(object.shapes, prepare);
    }
}

void Translator::rebase_texture_mappings() {
    if (!scene_.raster_camera) {
        return;
    }
    const auto origin = to_vector(scene_.raster_camera->origin);
    for (std::size_t i = 0; i < graph_mapping_sources_.size(); ++i) {
        const auto *descriptor = graph_mapping_sources_[i];
        if (!descriptor) {
            continue;
        }
        const auto &inverse = descriptor->texture_from_world.values;
        auto &matrix = scene_.texture_graph.nodes[i].mapping.texture_from_render;
        for (std::size_t row = 0; row < origin.size(); ++row) {
            const auto offset = 4 * row;
            const auto translation =
                inverse[offset + 3] +
                dot({inverse[offset], inverse[offset + 1], inverse[offset + 2]}, origin);
            if (!float_representable(translation)) {
                invalid(descriptor->authored.location,
                        "Camera-relative texture transform overflows binary32");
                break;
            }
            matrix[offset + 3] = static_cast<float>(translation);
        }
    }
}

void Translator::texture_frames() {
    if (!scene_.raster_camera ||
        (scene_.image_textures.empty() && scene_.texture_graph.nodes.empty())) {
        return;
    }
    try {
        scene_.instance_texture_frames.resize(scene_.geometry.instances.size());
        scene_.instance_surface_transforms.resize(scene_.geometry.instances.size());
    } catch (const std::bad_alloc &) {
        issue(Severity::error, "limit-exceeded", entry_, "Cannot allocate instance texture frames");
        return;
    }
    std::size_t remaining = limits_.total_values;
    std::size_t instance = 0;
    const auto prepare = [&](const Shape &shape, const Transform &placement,
                             const Location &source) {
        const auto index = instance++;
        if (!instance_uses_texture_context(scene_, index)) {
            return true;
        }
        auto &frames = scene_.instance_texture_frames[index];
        scene_.instance_surface_transforms[index] = {shape.descriptor.transform.values,
                                                     placement.values, shape.reverse_orientation};
        const auto &mesh = scene_.geometry.meshes[shape.mesh_index];
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
    const auto authored_bump = float_binding(descriptor, "displacement");
    const auto normal = native_image(descriptor);
    const auto bump = normal == graph::no_texture ? authored_bump : graph::no_texture;
    std::uint32_t existing = 0;
    std::optional<SpectralMaterial> value;
    if (selected && selected->type == "texture") {
        const auto &name = selected->values.front().text;
        if (const auto found = spectrum_textures_.find(name); found != spectrum_textures_.end()) {
            existing = found->second;
        } else if (const auto root = texture_roots_[true].find(name);
                   root != texture_roots_[true].end()) {
            value.emplace();
            value->kind = SpectralMaterialKind::texture_graph;
            value->texture_root = root->second;
        } else {
            invalid(selected->location, "Diffuse reflectance requires a checked spectrum texture");
            return 0;
        }
    } else if (selected) {
        value = reflectance(descriptor, ReflectanceSource::material, "reflectance", 0.5f);
        if (!value) {
            return 0;
        }
    }
    if (!value && bump == graph::no_texture && normal == graph::no_texture) {
        return existing;
    }
    if (!value) {
        value = scene_.materials[existing];
    }
    value->bump_root = bump;
    value->normal_map = normal;
    return append_material(std::move(*value));
}

void Translator::geometry() {
    const auto &geometry = input_.geometry;
    if (geometry.instances.size() > limits_.total_values ||
        geometry.meshes.size() > limits_.total_values ||
        input_.inline_materials.size() > limits_.directives ||
        input_.named_materials.size() > limits_.directives ||
        input_.float_constant_textures.size() > limits_.directives ||
        input_.spectrum_constant_textures.size() > limits_.directives ||
        input_.texture_order.size() > limits_.directives ||
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
              std::pair{mesh.patches.size(), 4U}, std::pair{mesh.tangents.size(), 3U}}) {
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
    compile_textures();
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
        std::uint32_t alpha = graph::no_texture;
        bool invisible_emitter = false;
    };
    std::vector<MeshBinding> bindings(geometry.meshes.size());
    std::map<const Descriptor *, std::uint32_t> area_lights;
    const auto bind = [&](const Shape &shape) {
        const auto &binding = shape.material;
        auto &mesh = bindings[shape.mesh_index];
        mesh.shape = &shape;
        mesh.alpha = float_binding(shape.descriptor, "alpha");
        if (mesh.alpha != graph::no_texture) {
            const auto &alpha = scene_.texture_graph.nodes[mesh.alpha];
            if (alpha.kind == graph::constant_node) {
                if (alpha.values[0] == 1) {
                    mesh.alpha = graph::no_texture;
                } else if (alpha.values[0] == 0 && shape.area_light) {
                    mesh.invisible_emitter = true;
                }
            }
        }
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
        bool emitted = false;
        if (descriptor->native_image) {
            if (supports_rgb_space(descriptor->color_space, descriptor->authored.location)) {
                if (const auto scale = image_emission(*descriptor)) {
                    area.image_scale = *scale;
                    if (placed != area_lights.end() && !positive_power) {
                        area.image_texture = native_image(*descriptor);
                    }
                    emitted = true;
                }
            }
        } else {
            emitted = emission(*descriptor, 1., area.radiance);
        }
        if (!emitted || positive_power || placed == area_lights.end()) {
            continue;
        }
        const auto uncharged_samples = descriptor->checked_spectrum("L") || descriptor->native_image
                                           ? 0
                                           : area.radiance.size();
        if (uncharged_samples > limits_.total_values - spectral_values_ ||
            scene_.area_emissions.size() >= no_area_emission) {
            issue(Severity::error, "limit-exceeded", descriptor->authored.location,
                  "Translated area light table limit exceeded");
            return;
        }
        spectral_values_ += uncharged_samples;
        placed->second = static_cast<std::uint32_t>(scene_.area_emissions.size());
        scene_.area_emissions.push_back(area);
    }

    scene_.geometry = geometry;
    displace_geometry(remaining);
    rebase_texture_mappings();
    const bool has_alpha = std::ranges::any_of(
        bindings, [](const MeshBinding &binding) { return binding.alpha != graph::no_texture; });
    const bool has_invisible = std::ranges::any_of(bindings, &MeshBinding::invisible_emitter);
    if (has_alpha) {
        scene_.instance_alpha_roots.reserve(geometry.instances.size());
    }
    if (has_invisible) {
        scene_.instance_invisible_emitters.reserve(geometry.instances.size());
    }
    scene_.instance_materials.reserve(geometry.instances.size());
    scene_.instance_reverse_orientation.reserve(geometry.instances.size());
    if (!area_lights.empty()) {
        scene_.instance_emissions.reserve(geometry.instances.size());
    }
    // Every placement prepares normals here and during portable scene validation.
    std::size_t normal_coordinates_remaining = limits_.total_values;
    std::size_t patch_corners_remaining = std::min(limits_.total_values, render_patch_corner_limit);
    for (const auto &instance : geometry.instances) {
        const auto &mesh = scene_.geometry.meshes[instance.mesh_index];
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
        const auto &mesh = scene_.geometry.meshes[instance.mesh_index];
        const auto &shape = *binding.shape;
        if (const auto message = validate_patch_geometry(mesh, instance.transform);
            !message.empty()) {
            invalid(shape.descriptor.authored.location, message);
        }
        scene_.instance_materials.push_back(binding.material);
        if (has_alpha) {
            scene_.instance_alpha_roots.push_back(binding.alpha);
        }
        if (has_invisible) {
            scene_.instance_invisible_emitters.push_back(binding.invisible_emitter);
        }
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

std::string
image_filter_provenance(const RenderScene &scene,
                        const std::map<const ImagePyramid *, ImageFileFormat> &formats) {
    if (scene.image_textures.empty()) {
        return {};
    }
    struct FilterMode {
        ImageFilter filter;
        string_view name;
        bool used = false;
    };
    std::array modes{
        FilterMode{ImageFilter::point, "point"}, FilterMode{ImageFilter::bilinear, "bilinear"},
        FilterMode{ImageFilter::trilinear, "trilinear"}, FilterMode{ImageFilter::ewa, "ewa"}};
    std::set<ImageFileFormat> used_formats;
    for (std::size_t i = 0; i < scene.geometry.instances.size(); ++i) {
        const auto &material = scene.materials[scene.instance_materials[i]];
        const auto &mesh = scene.geometry.meshes[scene.geometry.instances[i].mesh_index];
        if (material.kind == SpectralMaterialKind::image &&
            (!mesh.triangles.empty() || !mesh.patches.empty())) {
            const auto &texture = scene.image_textures[material.image_texture];
            const auto format = formats.find(texture.image.get());
            used_formats.insert(format == formats.end() ? ImageFileFormat::png : format->second);
            for (auto &mode : modes) {
                mode.used |= mode.filter == texture.filter;
            }
        }
    }
    auto remaining = std::ranges::count(modes, true, &FilterMode::used);
    if (remaining == 0) {
        return {};
    }
    std::string provenance = "Image textures: ";
    auto formats_remaining = used_formats.size();
    for (const auto format : used_formats) {
        switch (format) {
        case ImageFileFormat::png:
            provenance += "PNG";
            break;
        case ImageFileFormat::jpeg:
            provenance += "JPEG";
            break;
        case ImageFileFormat::exr:
            provenance += "EXR";
            break;
        case ImageFileFormat::hdr:
            provenance += "Radiance HDR";
            break;
        }
        if (--formats_remaining > 0)
            provenance += formats_remaining == 1 ? " and " : ", ";
    }
    provenance += ", UV mapping, mipmapped ";
    for (const auto &mode : modes) {
        if (mode.used) {
            provenance += mode.name;
            if (--remaining > 0) {
                provenance += remaining == 1 ? " and " : ", ";
            }
        }
    }
    return provenance + " filtering";
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
        if (auto images = image_filter_provenance(scene_, image_formats_); !images.empty()) {
            result_.provenance.push_back(std::move(images));
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
