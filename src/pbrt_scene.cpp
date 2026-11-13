// SPDX-License-Identifier: GPL-3.0-only

#include "pbrt.hpp"
#include "pbrt_numeric.hpp"
#include "ply.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <map>
#include <numbers>
#include <set>
#include <span>
#include <string_view>
#include <tuple>

namespace rypath::pbrt {
namespace {

using std::string_view;
using Vector = std::array<double, 3>;
using TransformSet = std::array<Transform, 2>;
using Inherited = Descriptor::InheritedParameter;
using SchemaName = std::pair<string_view, string_view>;
struct DependencyContext {
    string_view target;
    string_view type;
};

bool one_of(string_view value, std::initializer_list<string_view> options) {
    return std::ranges::find(options, value) != options.end();
}

Transform multiply(const Transform &a, const Transform &b) {
    Transform result;
    for (std::size_t row = 0; row < 4; ++row) {
        for (std::size_t column = 0; column < 4; ++column) {
            double value = 0;
            for (std::size_t i = 0; i < 4; ++i) {
                value += a.values[row * 4 + i] * b.values[i * 4 + column];
            }
            result.values[row * 4 + column] = value;
        }
    }
    return result;
}

bool affine(const Transform &transform) {
    const auto &m = transform.values;
    return m[12] == 0 && m[13] == 0 && m[14] == 0 && m[15] == 1;
}

std::optional<Transform> inverse_affine(const Transform &transform) {
    if (!affine(transform) ||
        !std::ranges::all_of(transform.values, [](double value) { return std::isfinite(value); }) ||
        exact_singular_linear(transform)) {
        return std::nullopt;
    }
    // Eliminate the linear part with pivoting. Keep the affine last row exact.
    std::array<std::array<double, 6>, 3> a{};
    for (std::size_t row = 0; row < 3; ++row) {
        for (std::size_t col = 0; col < 3; ++col) {
            a[row][col] = transform.values[row * 4 + col];
        }
        a[row][3 + row] = 1;
    }
    for (std::size_t col = 0; col < 3; ++col) {
        std::size_t pivot = col;
        for (std::size_t row = col + 1; row < 3; ++row) {
            if (std::abs(a[row][col]) > std::abs(a[pivot][col])) {
                pivot = row;
            }
        }
        std::swap(a[col], a[pivot]);
        const double scale = a[col][col];
        if (scale == 0 || !std::isfinite(scale)) {
            return std::nullopt;
        }
        for (auto &value : a[col]) {
            value /= scale;
        }
        for (std::size_t row = 0; row < 3; ++row) {
            if (row == col) {
                continue;
            }
            const double factor = a[row][col];
            for (std::size_t i = 0; i < a[row].size(); ++i) {
                a[row][i] -= factor * a[col][i];
            }
        }
    }

    Transform inverse;
    for (std::size_t row = 0; row < 3; ++row) {
        double translation = 0;
        for (std::size_t col = 0; col < 3; ++col) {
            inverse.values[row * 4 + col] = a[row][3 + col];
            translation -= a[row][3 + col] * transform.values[col * 4 + 3];
        }
        inverse.values[row * 4 + 3] = translation;
    }
    if (!std::ranges::all_of(inverse.values, [](double value) { return std::isfinite(value); })) {
        return std::nullopt;
    }
    return inverse;
}

Vector cross(const Vector &a, const Vector &b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}

bool normalize(Vector &vector) {
    const double scale = std::max({std::abs(vector[0]), std::abs(vector[1]), std::abs(vector[2])});
    if (scale == 0 || !std::isfinite(scale)) {
        return false;
    }
    for (auto &value : vector) {
        value /= scale;
    }
    const double length = std::hypot(vector[0], vector[1], vector[2]);
    for (auto &value : vector) {
        value /= length;
    }
    return true;
}

bool parallel(const std::array<Vector, 2> &vectors) {
    const auto &[a, b] = vectors;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const std::size_t j = (i + 1) % a.size();
        Transform minor;
        minor.values[0] = a[i];
        minor.values[1] = a[j];
        minor.values[4] = b[i];
        minor.values[5] = b[j];
        if (!exact_singular_linear(minor)) {
            return false;
        }
    }
    return true;
}

std::optional<Transform> transform_argument(const Directive &directive) {
    const auto &args = directive.arguments;
    const auto number = [&](std::size_t index) { return args[index].number; };
    const auto vector = [&](std::size_t index) -> Vector {
        return {number(index), number(index + 1), number(index + 2)};
    };
    Transform result;
    auto &m = result.values;
    if (directive.name == "Translate") {
        m[3] = number(0);
        m[7] = number(1);
        m[11] = number(2);
    } else if (directive.name == "Scale") {
        m[0] = number(0);
        m[5] = number(1);
        m[10] = number(2);
    } else if (directive.name == "Rotate") {
        Vector axis = vector(1);
        if (!normalize(axis)) {
            return std::nullopt;
        }
        const auto [x, y, z] = axis;
        const double radians = std::remainder(number(0), 360.) * (std::numbers::pi / 180.);
        const double c = std::cos(radians);
        const double s = std::sin(radians);
        const double t = 1 - c;
        m = {x * x * t + c,
             x * y * t - z * s,
             x * z * t + y * s,
             0,
             y * x * t + z * s,
             y * y * t + c,
             y * z * t - x * s,
             0,
             z * x * t - y * s,
             z * y * t + x * s,
             z * z * t + c,
             0,
             0,
             0,
             0,
             1};
    } else if (directive.name == "LookAt") {
        const Vector eye = vector(0);
        Vector forward = vector(3);
        Vector up = vector(6);
        for (std::size_t i = 0; i < 3; ++i) {
            forward[i] -= eye[i];
        }
        if (!std::ranges::all_of(forward, [](double value) { return std::isfinite(value); }) ||
            parallel({forward, up}) || !normalize(forward) || !normalize(up)) {
            return std::nullopt;
        }
        Vector right = cross(up, forward);
        if (!normalize(right)) {
            return std::nullopt;
        }
        up = cross(forward, right);
        for (std::size_t i = 0; i < 3; ++i) {
            m[i * 4] = right[i];
            m[i * 4 + 1] = up[i];
            m[i * 4 + 2] = forward[i];
            m[i * 4 + 3] = eye[i];
        }
        return inverse_affine(result);
    } else if (directive.name != "Identity") {
        // PBRT writes the 16 input values in column-major order.
        for (std::size_t row = 0; row < 4; ++row) {
            for (std::size_t col = 0; col < 4; ++col) {
                m[row * 4 + col] = number(col * 4 + row);
            }
        }
    }
    return result;
}

bool representable_float(double value) {
    return std::isfinite(value) && std::abs(value) <= std::numeric_limits<float>::max();
}

enum class Rule {
    none,
    positive,
    nonnegative,
    fov,
    albedo,
    emission,
    screen,
    bvh_split,
    light_sampler,
    sobol_randomization,
    sensor,
};

struct Field {
    string_view name;
    string_view type;
    std::size_t width = 1;
    bool array = false;
    bool required = false;
    bool deferred = false;
    Rule rule = Rule::none;
    bool prefer_texture = false;
};

constexpr std::array camera_fields{
    Field{"fov", "float", 1, false, false, false, Rule::fov},
    Field{"lensradius", "float", 1, false, false, false, Rule::nonnegative},
    Field{"focaldistance", "float", 1, false, false, false, Rule::positive},
    Field{"frameaspectratio", "float", 1, false, false, false, Rule::positive},
    Field{"screenwindow", "float", 4, false, false, false, Rule::screen},
    Field{"shutteropen", "float"},
    Field{"shutterclose", "float"},
};

constexpr std::array film_fields{
    Field{"filename", "string"},
    Field{"xresolution", "integer", 1, false, false, false, Rule::positive},
    Field{"yresolution", "integer", 1, false, false, false, Rule::positive},
    Field{"pixelbounds", "integer", 4},
    Field{"cropwindow", "float", 4},
    Field{"diagonal", "float", 1, false, false, false, Rule::positive},
    Field{"iso", "float", 1, false, false, false, Rule::positive},
    Field{"whitebalance", "float", 1, false, false, false, Rule::nonnegative},
    Field{"sensor", "string", 1, false, false, false, Rule::sensor},
    Field{"savefp16", "bool"},
    Field{"maxcomponentvalue", "float", 1, false, false, false, Rule::positive},
};

constexpr std::array filter_fields{
    Field{"xradius", "float", 1, false, false, false, Rule::positive},
    Field{"yradius", "float", 1, false, false, false, Rule::positive},
};

constexpr std::array sampler_fields{
    Field{"pixelsamples", "integer", 1, false, false, false, Rule::positive},
    Field{"seed", "integer"},
};

constexpr std::array sobol_sampler_fields{
    sampler_fields[0],
    sampler_fields[1],
    Field{"randomization", "string", 1, false, false, false, Rule::sobol_randomization},
};

constexpr std::array integrator_fields{
    Field{"maxdepth", "integer", 1, false, false, false, Rule::nonnegative},
    Field{"regularize", "bool"},
    Field{"lightsampler", "string", 1, false, false, false, Rule::light_sampler},
};

constexpr std::array accelerator_fields{
    Field{"maxnodeprims", "integer", 1, false, false, false, Rule::positive},
    Field{"splitmethod", "string", 1, false, false, false, Rule::bvh_split},
};

constexpr std::array material_fields{
    Field{"reflectance", "spectral-texture", 1, false, false, false, Rule::albedo},
    Field{"displacement", "float-texture", 1, false, false, true},
    Field{"normalmap", "string", 1, false, false, true},
};

constexpr std::array named_material_fields{
    Field{"type", "string", 1, false, true},
    material_fields[0],
    material_fields[1],
    material_fields[2],
};

constexpr std::array light_fields{
    Field{"L", "spectral", 1, false, false, false, Rule::emission},
    Field{"scale", "float", 1, false, false, false, Rule::nonnegative},
    Field{"illuminance", "float"},
    Field{"portal", "point3", 3, true, false, true},
    Field{"filename", "string", 1, false, false, true},
};

constexpr std::array area_light_fields{
    light_fields[0],
    light_fields[1],
    Field{"twosided", "bool"},
    Field{"power", "float"},
    Field{"filename", "string"},
};

constexpr std::array mesh_fields{
    Field{"P", "point3", 3, true, true},
    Field{"indices", "integer", 3, true},
    Field{"N", "normal", 3, true},
    Field{"S", "vector3", 3, true, false, true},
    Field{"uv", "point2", 2, true},
    Field{"faceIndices", "integer", 1, true, false, true},
    Field{"alpha", "float-texture", 1, false, false, true},
};

constexpr std::array ply_fields{
    Field{"filename", "string", 1, false, true},
    Field{"alpha", "float-texture", 1, false, false, true, Rule::none, true},
    Field{"displacement", "texture"},
    Field{"edgelength", "float"},
};

string_view canonical_type(string_view actual) {
    if (actual == "point") {
        return "point3";
    }
    if (actual == "vector") {
        return "vector3";
    }
    if (actual == "color") {
        return "rgb";
    }
    if (actual == "normal3") {
        return "normal";
    }
    return actual;
}

bool accepts_type(string_view allowed, string_view actual) {
    actual = canonical_type(actual);
    if (allowed == "spectral" || allowed == "spectral-texture") {
        return one_of(actual, {"rgb", "blackbody", "spectrum"}) ||
               (allowed == "spectral-texture" && actual == "texture");
    }
    return actual == allowed ||
           (allowed == "float-texture" && one_of(actual, {"float", "texture"}));
}

std::size_t field_width(const Field &field, const Parameter &parameter) {
    if (canonical_type(parameter.type) == "rgb") {
        return 3;
    }
    if (parameter.type == "spectrum" && parameter.values.front().kind != ValueKind::string) {
        return 2;
    }
    return field.width;
}

bool builtin_sensor(string_view name) {
    return one_of(name,
                  {"canon_eos_100d", "canon_eos_1dx_mkii", "canon_eos_200d", "canon_eos_200d_mkii",
                   "canon_eos_5d", "canon_eos_5d_mkii", "canon_eos_5d_mkiii", "canon_eos_5d_mkiv",
                   "canon_eos_5ds", "canon_eos_m", "hasselblad_l1d_20c", "nikon_d810", "nikon_d850",
                   "sony_ilce_6400", "sony_ilce_7m3", "sony_ilce_7rm3", "sony_ilce_9"});
}

bool builtin_spectrum(string_view name) {
    // Names from Spectra::Init in the pinned PBRT util/spectrum.cpp.
    if (one_of(name,
               {"glass-BK7", "glass-BAF10", "glass-FK51A", "glass-LASF9", "glass-F5", "glass-F10",
                "glass-F11", "stdillum-A", "stdillum-D50", "stdillum-D65", "illum-acesD60"})) {
        return true;
    }
    if (name.starts_with("metal-")) {
        name.remove_prefix(6);
        const auto hyphen = name.find('-');
        return hyphen != string_view::npos &&
               one_of(name.substr(0, hyphen), {"Ag", "Al", "Au", "Cu", "CuZn", "MgO", "TiO2"}) &&
               one_of(name.substr(hyphen + 1), {"eta", "k"});
    }
    if (name.starts_with("stdillum-F")) {
        return one_of(name.substr(10),
                      {"1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11", "12"});
    }
    return name.size() > 2 && name[name.size() - 2] == '_' &&
           one_of(name.substr(name.size() - 1), {"r", "g", "b"}) &&
           builtin_sensor(name.substr(0, name.size() - 2));
}

struct GraphicsState {
    TransformSet transform{};
    std::array<bool, 2> active{true, true};
    MaterialBinding material;
    std::shared_ptr<const Descriptor> area_light;
    std::string color_space = "srgb";
    bool reverse = false;
};

struct AttributeKey {
    string_view target;
    string_view name;
    string_view type;
    auto operator<=>(const AttributeKey &) const = default;
};
struct AttributeDefault {
    Inherited parameter;
    std::size_t order = 0;
};
struct AttributeChange {
    AttributeKey key;
    std::optional<AttributeDefault> previous;
};
struct Scope {
    Location location;
    bool object = false;
    GraphicsState state;
    std::size_t attribute_mark = 0;
};

class Interpreter {
  public:
    Interpreter(Document document, const Limits &limits)
        : limits_(limits),
          ply_remaining_{limits.expanded_bytes -
                             std::min(limits.expanded_bytes, document.expanded_bytes),
                         limits.expansions - std::min(limits.expansions, document.expansion_count),
                         limits.total_values} {
        result_.document = std::move(document);
    }

    Result run();

  private:
    const Limits &limits_;
    Result result_;
    Scene scene_;
    GraphicsState state_;
    std::vector<Scope> scopes_;
    std::vector<AttributeChange> attribute_changes_;
    std::map<AttributeKey, AttributeDefault> attributes_;
    std::map<string_view, TransformSet> coordinates_;
    std::set<string_view> named_materials_;
    std::array<std::set<string_view>, 2> textures_;
    std::set<string_view> media_;
    using SourceKey = std::tuple<std::shared_ptr<const std::string>, std::size_t, std::size_t>;
    struct TextureUse {
        Location location;
        std::string name;
        string_view channel;
    };
    std::vector<TextureUse> texture_uses_;
    std::set<std::pair<SourceKey, string_view>> seen_texture_uses_;
    std::vector<const Value *> medium_uses_;
    std::map<string_view, std::size_t> objects_;
    struct PendingOption {
        const Directive *directive = nullptr;
        Transform transform;
        std::string color_space;
        bool animated = false;
    };
    std::vector<PendingOption> options_;
    std::map<string_view, std::size_t> option_indices_;
    std::optional<std::size_t> current_object_;
    std::map<std::string, Location, std::less<>> material_uses_;
    std::map<std::pair<DependencyKind, std::filesystem::path>, std::size_t> inventoried_paths_;
    std::map<std::size_t, PlyResult> ply_cache_;
    PlyBudget ply_remaining_;
    std::size_t asset_path_bytes_ = 0;
    bool asset_path_budget_exceeded_ = false;
    bool skipped_include_ = false;
    bool world_ = false;
    bool failed_ = false;
    bool budget_exceeded_ = false;
    std::size_t attribute_order_ = 0;
    std::size_t mesh_values_ = 0;
    std::size_t validation_values_ = 0;
    bool validation_exhausted_ = false;
    std::size_t scope_overflow_ = 0;

    void issue(Severity severity, string_view code, const Location &location, std::string message);
    void error(string_view code, const Location &location, std::string message) {
        issue(Severity::error, code, location, std::move(message));
    }
    void unsupported(const Location &location, std::string message) {
        issue(Severity::unsupported, "unsupported-feature", location, std::move(message));
    }
    void visit(const Directive &directive);
    void state_directive(const Directive &directive);
    void option(const Directive &directive);
    void finalize_options();
    void validate_option(const PendingOption &pending);
    void validate_film_bounds(const std::map<string_view, const Parameter *> &fields);
    void material(const Directive &directive);
    void light(const Directive &directive);
    void shape(const Directive &directive);
    void ply_shape(const Directive &directive, Descriptor descriptor);
    void register_mesh(const Location &location, Descriptor descriptor, GeometryMesh mesh);
    bool charge_mesh(std::size_t count, const Location &location);
    void deferred(const Directive &directive);
    void attribute(const Directive &directive);
    void begin_scope(const Directive &directive, bool object);
    void end_scope(const Directive &directive, bool object);
    std::optional<std::size_t> dependency(DependencyKind kind, const Value &value);
    void dependencies(const Directive &directive, string_view target, string_view type);
    void parameter_dependencies(const Parameter &parameter, string_view texture_channel = {});
    void authored_dependencies(const Directive &directive, DependencyContext context = {});
    bool charge_validation(std::size_t count, const Location &location);
    void scan_after_include(const Directive &directive);
    static SourceKey source_key(const Location &location) {
        // File handles identify individual textual expansions and survive inherited value copies.
        return {location.file, location.line, location.column};
    }
    void deferred_type(const Directive &directive, string_view type, bool known,
                       string_view target = {});
    const Parameter *select(const Directive &directive, string_view target, const Field &field,
                            Descriptor *descriptor = nullptr);
    std::map<string_view, const Parameter *> validate(Descriptor &descriptor,
                                                      const Directive &directive,
                                                      string_view target,
                                                      std::span<const Field> schema);
    bool field_valid(const Field &field, const Parameter &parameter);
    void apply_rule(const Field &field, const Parameter &parameter);
    Descriptor describe(const Directive &directive, bool capture = true);
    Transform capture_transform(const Location &location);
    void add_instance(std::uint32_t mesh, const Transform &transform, const Location &location);
    void resolve_references();
};

void Interpreter::issue(Severity severity, string_view code, const Location &location,
                        std::string message) {
    failed_ |= severity != Severity::warning;
    diagnose(result_.document, limits_, severity, std::string(code), location, std::move(message));
    if (result_.document.diagnostics_truncated) {
        result_.document.dependencies_complete = false;
    }
}

std::optional<std::size_t> Interpreter::dependency(DependencyKind kind, const Value &value) {
    if (value.text.empty() || result_.document.diagnostics_truncated ||
        asset_path_budget_exceeded_) {
        return std::nullopt;
    }
    auto path = result_.document.entry_directory / std::filesystem::path(value.text);
    const auto key = std::pair{kind, path};
    if (const auto found = inventoried_paths_.find(key); found != inventoried_paths_.end()) {
        return found->second;
    }
    // Match the resolver's path identity without collapsing symlink-sensitive '..' components.
    const std::size_t bytes = path.native().size() * sizeof(std::filesystem::path::value_type);
    if (bytes > limits_.expanded_bytes - asset_path_bytes_) {
        asset_path_budget_exceeded_ = true;
        result_.document.dependencies_complete = false;
        error("limit-exceeded", value.location, "Resolved asset path byte limit exceeded");
        return std::nullopt;
    }
    asset_path_bytes_ += bytes;
    const auto index = result_.document.dependencies.size();
    inventoried_paths_.emplace(key, index);
    std::error_code filesystem_error;
    const bool exists = std::filesystem::is_regular_file(path, filesystem_error);
    result_.document.dependencies.push_back(
        {value.location, kind, value.text, std::move(path), exists, false});
    if (!exists) {
        error("missing-file", value.location,
              "Missing or inaccessible input file \"" + value.text + "\"");
    }
    return index;
}

void Interpreter::dependencies(const Directive &directive, string_view target, string_view type) {
    const auto file = [&](string_view field, DependencyKind kind, bool required = false,
                          std::initializer_list<string_view> builtin = {}) {
        const auto *parameter = select(directive, target, Field{field, "string"});
        if (!parameter) {
            if (!required) {
                return;
            }
            result_.document.dependencies_complete = false;
            const auto wrong_type =
                std::ranges::find(directive.parameters, field, &Parameter::name);
            if (wrong_type != directive.parameters.end()) {
                error("invalid-parameter", wrong_type->location,
                      "Expected string parameter \"" + std::string(field) + "\"");
            } else if (!skipped_include_) {
                error("missing-parameter", directive.location,
                      "Missing required string parameter \"" + std::string(field) + "\"");
            }
            return;
        }
        if (parameter->values.size() != 1) {
            result_.document.dependencies_complete = false;
            error("invalid-parameter", parameter->location,
                  "Expected one file path for \"" + std::string(field) + "\"");
            return;
        }
        const auto &value = parameter->values.front();
        if (required && value.text.empty()) {
            result_.document.dependencies_complete = false;
            error("missing-parameter", value.location,
                  "Required file path \"" + std::string(field) + "\" is empty");
        } else if (!one_of(value.text, builtin)) {
            dependency(kind, value);
        }
    };
    if (target == "material") {
        file("normalmap", DependencyKind::image);
        if (type == "measured") {
            file("filename", DependencyKind::scene, true);
        }
    } else if (target == "shape" && type == "plymesh") {
        file("filename", DependencyKind::mesh, true);
    } else if (target == "shape" && type == "bilinearmesh") {
        file("emissionfilename", DependencyKind::image);
    } else if ((target == "texture" && one_of(type, {"imagemap", "ptex"})) ||
               (target == "light" && type == "projection")) {
        file("filename", DependencyKind::image, true);
    } else if ((target == "light" && one_of(type, {"infinite", "goniometric"})) ||
               directive.name == "AreaLightSource") {
        file("filename", DependencyKind::image);
    } else if (target == "medium" && type == "nanovdb") {
        file("filename", DependencyKind::scene, true);
    } else if (directive.name == "Camera" && type == "realistic") {
        file("lensfile", DependencyKind::scene, true);
        file("aperture", DependencyKind::image, false, {"gaussian", "square", "pentagon", "star"});
    }
}

bool Interpreter::charge_validation(std::size_t count, const Location &location) {
    if (validation_exhausted_) {
        return false;
    }
    if (count > limits_.total_values - validation_values_) {
        validation_exhausted_ = true;
        result_.document.dependencies_complete = false;
        error("limit-exceeded", location, "Effective parameter value limit exceeded");
        return false;
    }
    validation_values_ += count;
    return true;
}

void Interpreter::parameter_dependencies(const Parameter &parameter, string_view texture_channel) {
    if (!one_of(parameter.type, {"spectrum", "texture"}) || parameter.values.empty() ||
        parameter.values.front().kind != ValueKind::string) {
        return;
    }
    for (const auto &value : parameter.values) {
        if (result_.document.diagnostics_truncated || asset_path_budget_exceeded_) {
            return;
        }
        if (parameter.type == "spectrum" && !builtin_spectrum(value.text)) {
            if (value.text.empty()) {
                result_.document.dependencies_complete = false;
                error("missing-parameter", value.location, "Spectrum file path is empty");
            } else {
                dependency(DependencyKind::spectrum, value);
            }
        } else if (parameter.type == "texture" && !skipped_include_ &&
                   seen_texture_uses_.emplace(source_key(value.location), texture_channel).second) {
            texture_uses_.push_back({value.location, value.text, texture_channel});
        }
    }
}

void Interpreter::deferred_type(const Directive &directive, string_view type, bool known,
                                string_view target) {
    if (known) {
        unsupported(directive.location, directive.name + " type \"" + std::string(type) +
                                            "\" is outside the structural profile");
    } else {
        error("unknown-type", directive.arguments.back().location,
              "Unknown " + directive.name + " type \"" + std::string(type) + "\"");
    }
    authored_dependencies(directive, {target, type});
}

void Interpreter::authored_dependencies(const Directive &directive, DependencyContext context) {
    const auto [target, type] = context;
    // Known spectral consumers use the last compatible type, while unknown
    // fields can only be deduplicated by exact name and type.
    const auto key = [&](const Parameter &parameter) {
        const string_view name = parameter.name;
        const string_view declared_type = canonical_type(parameter.type);
        string_view family;
        if (target == "material" &&
            one_of(name, {"reflectance", "transmittance", "albedo", "sigma_a", "sigma_s", "mfp",
                          "conductor.eta", "conductor.k", "k"})) {
            family = "spectral-texture";
        } else if ((target == "material" && one_of(name, {"eta", "interface.eta"})) ||
                   (target == "light" && one_of(name, {"I", "L"}))) {
            family = type == "conductor" ? "spectral-texture" : "spectral";
        }
        return SchemaName{name, accepts_type(family, declared_type) ? family : declared_type};
    };

    std::set<SchemaName> seen;
    for (auto it = directive.parameters.rbegin(); it != directive.parameters.rend(); ++it) {
        if (result_.document.diagnostics_truncated) {
            return;
        }
        if (seen.emplace(key(*it)).second && charge_validation(it->values.size(), it->location)) {
            parameter_dependencies(*it);
        }
    }
    if (target.empty()) {
        return;
    }
    std::map<SchemaName, const AttributeDefault *> inherited;
    for (auto it = attributes_.lower_bound({target, {}, {}});
         it != attributes_.end() && it->first.target == target; ++it) {
        const auto &parameter = *it->second.parameter.parameter;
        // Charge even shadowed entries: finding them is work repeated for each entity.
        if (!charge_validation(1, parameter.location)) {
            return;
        }
        const auto identity = key(parameter);
        if (seen.contains(identity)) {
            continue;
        }
        const auto [found, inserted] = inherited.try_emplace(identity, &it->second);
        if (!inserted && it->second.order > found->second->order) {
            found->second = &it->second;
        }
    }
    for (const auto &[identity, entry] : inherited) {
        if (result_.document.diagnostics_truncated) {
            return;
        }
        const auto &parameter = *entry->parameter.parameter;
        if (parameter.type == "string" &&
            !one_of(parameter.name, {"type", "filename", "normalmap", "emissionfilename"})) {
            result_.document.dependencies_complete = false;
        }
        if (one_of(parameter.type, {"spectrum", "texture"})) {
            if (!charge_validation(parameter.values.size() - 1, parameter.location)) {
                return;
            }
            parameter_dependencies(parameter);
        }
    }
}

void Interpreter::scan_after_include(const Directive &directive) {
    // An unvisited textual Include may change scopes, transforms, definitions,
    // or inherited parameters. Only explicitly authored asset paths can be
    // inspected in the remaining stream.
    const string_view name = directive.name;
    const string_view argument =
        directive.arguments.empty() ? string_view{} : directive.arguments.front().text;
    if (name == "Material" || name == "MakeNamedMaterial" || name == "MakeNamedMedium") {
        const string_view target = name == "MakeNamedMedium" ? "medium" : "material";
        const auto *parameter =
            name == "Material" ? nullptr : select(directive, {}, Field{"type", "string"});
        const string_view type = parameter ? string_view(parameter->values.front().text) : argument;
        dependencies(directive, target, type);
        if (target == "material" && type == "diffuse") {
            if (const auto *reflectance = select(directive, {}, material_fields.front())) {
                if (charge_validation(reflectance->values.size(), reflectance->location)) {
                    parameter_dependencies(*reflectance);
                }
            }
            return;
        }
    } else if (name == "Shape") {
        dependencies(directive, "shape", argument);
    } else if (name == "Texture") {
        dependencies(directive, "texture", directive.arguments[2].text);
    } else if (name == "LightSource" || name == "AreaLightSource") {
        if (name == "AreaLightSource" && argument.empty()) {
            if (!directive.parameters.empty()) {
                error("unused-parameter", directive.parameters.front().location,
                      "An empty area light declaration takes no parameters");
            }
            return;
        }
        dependencies(directive, "light", argument);
        if ((name == "LightSource" && argument == "infinite") ||
            (name == "AreaLightSource" && argument == "diffuse")) {
            if (const auto *radiance = select(directive, {}, light_fields.front())) {
                if (charge_validation(radiance->values.size(), radiance->location)) {
                    parameter_dependencies(*radiance);
                }
            }
            return;
        }
    } else if (name == "Camera") {
        dependencies(directive, {}, argument);
    } else if (name == "Attribute" || name == "Include" || name == "Import") {
        return;
    }
    authored_dependencies(directive);
}

const Parameter *Interpreter::select(const Directive &directive, string_view target,
                                     const Field &field, Descriptor *descriptor) {
    for (auto it = directive.parameters.rbegin(); it != directive.parameters.rend(); ++it) {
        if (it->name == field.name && accepts_type(field.type, it->type)) {
            if (descriptor) {
                descriptor->effective_authored.push_back(
                    static_cast<std::size_t>(it.base() - directive.parameters.begin() - 1));
            }
            return &*it;
        }
    }
    const AttributeDefault *selected = nullptr;
    if (skipped_include_) {
        return nullptr;
    }
    for (auto it = attributes_.lower_bound({target, field.name, {}});
         it != attributes_.end() && it->first.target == target && it->first.name == field.name;
         ++it) {
        if (accepts_type(field.type, it->first.type) &&
            (!selected || it->second.order > selected->order)) {
            selected = &it->second;
        }
    }
    if (!selected) {
        return nullptr;
    }
    if (descriptor) {
        descriptor->inherited.push_back(selected->parameter);
    }
    return selected->parameter.parameter.get();
}

std::map<string_view, const Parameter *> Interpreter::validate(Descriptor &descriptor,
                                                               const Directive &directive,
                                                               string_view target,
                                                               std::span<const Field> schema) {
    for (const auto &parameter : directive.parameters) {
        const auto field = std::ranges::find(schema, parameter.name, &Field::name);
        if (field == schema.end()) {
            error("unused-parameter", parameter.location,
                  "Unknown parameter \"" + parameter.name + "\" for " + directive.name);
        } else if (!accepts_type(field->type, parameter.type)) {
            error("invalid-parameter", parameter.location,
                  "Unexpected type \"" + parameter.type + "\" for \"" + parameter.name + "\"");
        }
    }

    std::map<string_view, const Parameter *> effective;
    for (const auto &field : schema) {
        Field lookup = field;
        if (field.prefer_texture) {
            // PBRT queries alpha as a texture first and only falls back to float
            // when the effective texture is absent or names an empty string.
            lookup.type = "texture";
            const auto *texture = select(directive, target, lookup);
            if (!texture || (texture->values.size() == 1 && texture->values.front().text.empty())) {
                lookup.type = "float";
                if (texture && !select(directive, target, lookup)) {
                    lookup.type = "texture";
                }
            }
        }
        const auto *parameter = select(directive, target, lookup, &descriptor);
        if (!parameter) {
            if (field.required) {
                error("missing-parameter", directive.location,
                      "Missing required parameter \"" + std::string(field.name) + "\"");
            }
        } else {
            const bool valid = field_valid(field, *parameter);
            const bool inert_texture =
                lookup.type == "texture" && valid && parameter->values.front().text.empty();
            // A malformed cardinality can still name discoverable inputs. The
            // validation charge bounds this scan even for inherited parameters.
            if (!validation_exhausted_ && !inert_texture) {
                parameter_dependencies(*parameter, one_of(field.type, {"float-texture", "texture"})
                                                       ? "float"
                                                       : "spectrum");
            }
            if (!valid) {
                continue;
            }
            effective.emplace(field.name, parameter);
            if (field.deferred || (parameter->type == "texture" && !inert_texture) ||
                (parameter->type == "spectrum" &&
                 parameter->values.front().kind == ValueKind::string &&
                 !builtin_spectrum(parameter->values.front().text))) {
                issue(Severity::unsupported, "unsupported-parameter", parameter->location,
                      "Parameter \"" + parameter->name +
                          "\" uses a feature outside the structural profile");
            }
        }
    }
    return effective;
}

bool Interpreter::field_valid(const Field &field, const Parameter &parameter) {
    const auto count = parameter.values.size();
    if (!charge_validation(count, parameter.location)) {
        return false;
    }
    const auto width = count == 0 ? field.width : field_width(field, parameter);
    const bool sampled_spectrum = parameter.type == "spectrum" && count > 0 &&
                                  parameter.values.front().kind != ValueKind::string;
    if (count == 0 || ((field.array || sampled_spectrum) ? count % width != 0 : count != width)) {
        const auto code = one_of(field.name, {"P", "indices", "N", "S", "uv", "faceIndices"})
                              ? "invalid-geometry"
                              : "invalid-parameter";
        error(code, parameter.location, "Wrong value count for \"" + parameter.name + "\"");
        return false;
    }
    if (sampled_spectrum) {
        for (std::size_t i = 0; i < count; i += 2) {
            const auto &value = parameter.values[i];
            if (i > 0 && value.number <= parameter.values[i - 2].number) {
                error("invalid-parameter", value.location,
                      "Spectrum wavelengths must be strictly increasing");
                return false;
            }
        }
    }
    apply_rule(field, parameter);
    return true;
}

void Interpreter::apply_rule(const Field &field, const Parameter &parameter) {
    const auto &values = parameter.values;
    const auto number = [&](std::size_t index) {
        return values[index].kind == ValueKind::integer ? values[index].integer
                                                        : values[index].number;
    };
    bool valid = true;
    switch (field.rule) {
    case Rule::none:
        break;
    case Rule::positive:
        valid = number(0) > 0;
        break;
    case Rule::nonnegative:
        valid = number(0) >= 0;
        break;
    case Rule::fov:
        valid = number(0) > 0 && number(0) < 180;
        break;
    case Rule::albedo:
    case Rule::emission:
        if (canonical_type(parameter.type) == "rgb") {
            valid = std::ranges::all_of(values, [&](const Value &value) {
                return value.number >= 0 && (field.rule != Rule::albedo || value.number <= 1);
            });
        } else if (parameter.type == "blackbody") {
            valid = number(0) > 0;
        }
        break;
    case Rule::screen:
        valid = valid && number(0) < number(1) && number(2) < number(3);
        break;
    case Rule::bvh_split:
        valid = one_of(values.front().text, {"sah", "hlbvh", "middle", "equal"});
        break;
    case Rule::light_sampler:
        valid = one_of(values.front().text, {"uniform", "power", "bvh", "exhaustive"});
        break;
    case Rule::sobol_randomization:
        if (!one_of(values.front().text, {"none", "permutedigits", "fastowen", "owen"})) {
            error("invalid-parameter", parameter.location,
                  "Sobol randomization must be none, permutedigits, fastowen, or owen");
        }
        break;
    case Rule::sensor:
        valid = values.front().text == "cie1931" || builtin_sensor(values.front().text);
        break;
    }
    if (!valid) {
        error("invalid-parameter", parameter.location,
              "Invalid value for \"" + parameter.name + "\"");
    }
}

Transform Interpreter::capture_transform(const Location &location) {
    if (state_.transform[0].values != state_.transform[1].values) {
        unsupported(location, "Animated transforms are outside the structural profile");
    }
    return state_.transform[0];
}

Descriptor Interpreter::describe(const Directive &directive, bool capture) {
    return {directive,
            capture ? capture_transform(directive.location) : Transform{},
            state_.color_space,
            {},
            {}};
}

void Interpreter::option(const Directive &directive) {
    if (directive.name == "Option") {
        const auto &parameter = directive.parameters.front();
        std::string key;
        for (unsigned char character : parameter.name) {
            if (character != '-' && character != '_') {
                key += static_cast<char>(std::tolower(character));
            }
        }
        const auto &value = parameter.values.front();
        bool valid;
        if (key == "seed") {
            valid = value.kind == ValueKind::integer;
        } else if (key == "displacementedgescale") {
            valid = value.kind == ValueKind::number || value.kind == ValueKind::integer;
        } else if (one_of(key, {"msereferenceimage", "msereferenceout", "rendercoordsys"})) {
            valid = value.kind == ValueKind::string;
            if (valid && key == "msereferenceimage") {
                dependency(DependencyKind::image, value);
            } else if (valid && key == "rendercoordsys") {
                valid = one_of(value.text, {"camera", "cameraworld", "world"});
            }
        } else if (one_of(key,
                          {"disablepixeljitter", "disabletexturefiltering",
                           "disablewavelengthjitter", "forcediffuse", "pixelstats", "wavefront"})) {
            valid = value.kind == ValueKind::boolean;
        } else {
            error("unknown-option", parameter.location,
                  "Unknown Option \"" + parameter.name + "\"");
            return;
        }
        if (!valid) {
            error("invalid-parameter", value.location,
                  "Invalid value for Option \"" + parameter.name + "\"");
        } else {
            unsupported(directive.location, "Option settings are outside the structural profile");
        }
        return;
    }

    PendingOption pending{&directive, {}, state_.color_space, false};
    if (directive.name == "Camera") {
        TransformSet world_from_camera{};
        bool invertible = true;
        for (std::size_t i = 0; i < state_.transform.size(); ++i) {
            if (const auto inverse = inverse_affine(state_.transform[i])) {
                world_from_camera[i] = *inverse;
            } else {
                invertible = false;
            }
        }
        if (!invertible) {
            error("invalid-transform", directive.location,
                  "Camera transform has no finite affine inverse");
        } else {
            pending.transform = world_from_camera[0];
            pending.animated = state_.transform[0].values != state_.transform[1].values;
            coordinates_["camera"] = world_from_camera;
        }
    }
    const auto [found, inserted] = option_indices_.try_emplace(directive.name, options_.size());
    if (inserted) {
        options_.push_back(std::move(pending));
    } else {
        options_[found->second] = std::move(pending);
    }
}

void Interpreter::finalize_options() {
    for (const auto &pending : options_) {
        validate_option(pending);
    }
}

void Interpreter::validate_option(const PendingOption &pending) {
    const auto &directive = *pending.directive;
    const string_view type = directive.arguments.front().text;
    Descriptor descriptor{directive, pending.transform, pending.color_space, {}, {}};
    std::span<const Field> schema;
    string_view supported_type;
    bool known = false;
    if (directive.name == "Camera") {
        supported_type = "perspective";
        known = one_of(type, {"orthographic", "realistic", "spherical"});
        schema = camera_fields;
        if (pending.animated) {
            unsupported(directive.location,
                        "Animated camera transform is outside the structural profile");
        }
    } else if (directive.name == "Film") {
        supported_type = "rgb";
        known = one_of(type, {"gbuffer", "spectral"});
        schema = film_fields;
    } else if (directive.name == "PixelFilter") {
        supported_type = type == "triangle" ? "triangle" : "box";
        known = one_of(type, {"gaussian", "mitchell", "sinc"});
        schema = filter_fields;
    } else if (directive.name == "Sampler") {
        supported_type = type == "sobol" ? "sobol" : "independent";
        known = one_of(type, {"zsobol", "paddedsobol", "halton", "pmj02bn", "stratified"});
        schema = type == "sobol" ? std::span<const Field>{sobol_sampler_fields}
                                 : std::span<const Field>{sampler_fields};
    } else if (directive.name == "Integrator") {
        supported_type = "path";
        known = one_of(type, {"function", "simplepath", "lightpath", "simplevolpath", "volpath",
                              "bdpt", "mlt", "ambientocclusion", "randomwalk", "sppm"});
        schema = integrator_fields;
    } else {
        supported_type = "bvh";
        known = type == "kdtree";
        schema = accelerator_fields;
    }
    dependencies(directive, {}, type);
    if (type != supported_type) {
        deferred_type(directive, type, known);
        return;
    }
    const auto fields = validate(descriptor, directive, {}, schema);
    if (directive.name == "Film") {
        validate_film_bounds(fields);
    } else if (directive.name == "Camera") {
        const auto open = fields.find("shutteropen");
        const auto close = fields.find("shutterclose");
        const double open_time = open == fields.end() ? 0. : open->second->values.front().number;
        const double close_time = close == fields.end() ? 1. : close->second->values.front().number;
        if (close_time <= open_time) {
            error("invalid-parameter", directive.location,
                  "Camera shutterclose must be later than shutteropen");
        }
    }
    scene_.options.push_back(std::move(descriptor));
}

void Interpreter::validate_film_bounds(const std::map<string_view, const Parameter *> &fields) {
    const auto resolution = [&](string_view name, std::int32_t fallback) {
        const auto found = fields.find(name);
        return found == fields.end() ? fallback : found->second->values.front().integer;
    };
    const std::array extent{resolution("xresolution", 1280), resolution("yresolution", 720)};
    if (extent[0] <= 0 || extent[1] <= 0) {
        return; // Reported by the field schema.
    }
    const auto crop = fields.find("cropwindow");
    const auto pixels = fields.find("pixelbounds");
    if (crop != fields.end()) {
        const auto &values = crop->second->values;
        for (std::size_t axis = 0; axis < extent.size(); ++axis) {
            const auto [a, b] = std::minmax(values[axis * 2].number, values[axis * 2 + 1].number);
            const auto begin = std::ceil(extent[axis] * std::clamp(a, 0., 1.));
            const auto end = std::ceil(extent[axis] * std::clamp(b, 0., 1.));
            if (begin >= end) {
                error("invalid-parameter", crop->second->location, "Film crop selects no pixels");
                return;
            }
        }
    } else if (pixels != fields.end()) {
        const auto &values = pixels->second->values;
        for (std::size_t axis = 0; axis < extent.size(); ++axis) {
            const auto [a, b] = std::minmax(values[axis * 2].integer, values[axis * 2 + 1].integer);
            const auto begin = std::max(0, a);
            const auto end = std::min(extent[axis], b);
            if (begin >= end) {
                error("invalid-parameter", pixels->second->location,
                      "Film pixel bounds select no pixels within the image");
                return;
            }
        }
    }
}

void Interpreter::material(const Directive &directive) {
    const bool named = directive.name == "MakeNamedMaterial";
    const string_view name = directive.arguments.front().text;
    string_view type = name;
    if (named) {
        ++result_.inventory.named_materials;
        if (name.empty() || !named_materials_.insert(name).second) {
            error("duplicate-definition", directive.location,
                  "Empty or duplicate named material \"" + std::string(name) + "\"");
        }
        const Field type_field{"type", "string", 1, false, true};
        if (const auto *parameter = select(directive, "material", type_field)) {
            if (parameter->values.size() != 1) {
                error("invalid-parameter", parameter->location,
                      "Expected exactly one named material type");
                return;
            }
            type = parameter->values.front().text;
            if (type.empty()) {
                error("invalid-parameter", parameter->location,
                      "Named material type cannot be empty");
                return;
            }
        } else {
            error("missing-parameter", directive.location,
                  "Named material requires \"string type\"");
            return;
        }
    }
    dependencies(directive, "material", type);
    if (type != "diffuse") {
        if (type == "mix") {
            if (const auto *materials =
                    select(directive, "material", Field{"materials", "string", 2});
                materials && field_valid(Field{"materials", "string", 2}, *materials)) {
                for (const auto &value : materials->values) {
                    material_uses_.try_emplace(value.text, value.location);
                }
            }
        }
        deferred_type(directive, type,
                      one_of(type, {"", "none", "interface", "coateddiffuse", "coatedconductor",
                                    "diffusetransmission", "dielectric", "thindielectric", "hair",
                                    "conductor", "measured", "subsurface", "mix"}),
                      "material");
        return;
    }

    Descriptor descriptor = describe(directive, false);
    validate(descriptor, directive, "material",
             named ? std::span<const Field>(named_material_fields)
                   : std::span<const Field>(material_fields));
    if (named) {
        scene_.named_materials.push_back(std::move(descriptor));
    } else {
        state_.material = {Location::empty_file(),
                           std::make_shared<const Descriptor>(std::move(descriptor)),
                           directive.location};
        scene_.inline_materials.push_back(state_.material.inline_material);
    }
}

void Interpreter::light(const Directive &directive) {
    const string_view type = directive.arguments.front().text;
    const bool area = directive.name == "AreaLightSource";
    if (area && type.empty()) {
        state_.area_light.reset();
        if (!directive.parameters.empty()) {
            error("unused-parameter", directive.parameters.front().location,
                  "An empty area light declaration takes no parameters");
        }
        return;
    }
    dependencies(directive, "light", type);
    if (area && type == "diffuse") {
        // The declaration captures the parameter dictionary. The following shape owns its pose.
        Descriptor descriptor = describe(directive, false);
        const auto fields = validate(descriptor, directive, "light", area_light_fields);
        if (const auto filename = fields.find("filename");
            filename != fields.end() && !filename->second->values.front().text.empty()) {
            if (fields.contains("L")) {
                error("invalid-parameter", filename->second->location,
                      "A diffuse area light cannot specify both L and a filename");
            } else {
                unsupported(
                    filename->second->location,
                    "Diffuse area light emission images are outside the structural profile");
            }
        }
        state_.area_light = std::make_shared<const Descriptor>(std::move(descriptor));
        scene_.area_lights.push_back(state_.area_light);
        return;
    }
    if (area || type != "infinite") {
        const bool known =
            !area && one_of(type, {"point", "spot", "goniometric", "projection", "distant"});
        deferred_type(directive, type, known, "light");
        return;
    }

    Descriptor descriptor = describe(directive);
    const auto fields = validate(descriptor, directive, "light", light_fields);
    if (fields.contains("L") && fields.contains("filename") &&
        !fields.at("filename")->values.front().text.empty() && !fields.contains("portal")) {
        error("invalid-parameter", fields.at("filename")->location,
              "An infinite light cannot specify both L and a filename without a portal");
    }
    scene_.lights.push_back(std::move(descriptor));
}

void Interpreter::shape(const Directive &directive) {
    const string_view type = directive.arguments.front().text;
    if (current_object_ && state_.area_light) {
        unsupported(directive.location,
                    "Diffuse area lights on shapes inside object definitions are unsupported");
    }
    if (!one_of(type, {"trianglemesh", "plymesh"})) {
        dependencies(directive, "shape", type);
        deferred_type(
            directive, type,
            one_of(type, {"sphere", "cylinder", "disk", "bilinearmesh", "curve", "loopsubdiv"}));
        return;
    }
    ++result_.inventory.meshes;
    Descriptor descriptor = describe(directive);
    if (type == "plymesh") {
        ply_shape(directive, std::move(descriptor));
        return;
    }
    const auto fields = validate(descriptor, directive, "shape", mesh_fields);
    if (!fields.contains("P")) {
        return;
    }
    const auto &positions = *fields.at("P");
    const auto vertex_count = positions.values.size() / 3;
    const auto *indices = fields.contains("indices") ? fields.at("indices") : nullptr;
    if (!indices && vertex_count != 3) {
        error("missing-parameter", directive.location,
              "Triangle meshes with more or fewer than three vertices require indices");
        return;
    }
    const std::size_t index_count = indices ? indices->values.size() : 3;
    const std::size_t triangle_count = index_count / 3;
    const auto check_count = [&](string_view name, std::size_t expected) -> const Parameter * {
        const auto found = fields.find(name);
        if (found == fields.end()) {
            return nullptr;
        }
        if (found->second->values.size() != expected) {
            error("invalid-geometry", found->second->location,
                  "Wrong value count for \"" + std::string(name) + "\" relative to mesh size");
            return nullptr;
        }
        return found->second;
    };
    const auto *normals = check_count("N", vertex_count * 3);
    check_count("S", vertex_count * 3);
    const auto *uvs = check_count("uv", vertex_count * 2);
    check_count("faceIndices", triangle_count);
    if ((!normals && fields.contains("N")) || (!uvs && fields.contains("uv"))) {
        return;
    }
    // A large inherited mesh must not bypass the authored-value limit by being used repeatedly.
    const auto mesh_values = positions.values.size() + index_count +
                             (normals ? normals->values.size() : 0) +
                             (uvs ? uvs->values.size() : 0);
    if (!charge_mesh(mesh_values, directive.location)) {
        return;
    }

    GeometryMesh mesh;
    const auto narrow = [&](const Value &value, float &component, string_view name) {
        if (!representable_float(value.number)) {
            error("invalid-geometry", value.location, std::string(name) + " overflows binary32");
            return false;
        }
        component = static_cast<float>(value.number);
        return true;
    };
    const auto read_points = [&](const Parameter &parameter, std::vector<GeometryPoint> &points,
                                 string_view name) {
        points.reserve(vertex_count);
        for (std::size_t i = 0; i < parameter.values.size(); i += 3) {
            GeometryPoint point;
            if (!narrow(parameter.values[i], point.x, name) ||
                !narrow(parameter.values[i + 1], point.y, name) ||
                !narrow(parameter.values[i + 2], point.z, name)) {
                return false;
            }
            points.push_back(point);
        }
        return true;
    };
    if (!read_points(positions, mesh.positions, "Vertex coordinate") ||
        (normals && !read_points(*normals, mesh.normals, "Vertex normal component"))) {
        return;
    }
    if (uvs) {
        mesh.uvs.reserve(vertex_count);
        for (std::size_t i = 0; i < uvs->values.size(); i += 2) {
            std::array<float, 2> uv{};
            if (!narrow(uvs->values[i], uv[0], "UV coordinate") ||
                !narrow(uvs->values[i + 1], uv[1], "UV coordinate")) {
                return;
            }
            mesh.uvs.push_back(uv);
        }
    }
    mesh.triangles.reserve(triangle_count);
    if (!indices) {
        mesh.triangles.push_back({0, 1, 2});
    } else {
        for (std::size_t i = 0; i < index_count; i += 3) {
            std::array<std::uint32_t, 3> triangle{};
            for (std::size_t corner = 0; corner < triangle.size(); ++corner) {
                const auto &value = indices->values[i + corner];
                if (value.integer < 0 || static_cast<std::size_t>(value.integer) >= vertex_count) {
                    error("invalid-geometry", value.location, "Triangle index is out of bounds");
                    return;
                }
                triangle[corner] = static_cast<std::uint32_t>(value.integer);
            }
            mesh.triangles.push_back(triangle);
        }
    }

    GeometryData checked;
    checked.meshes.push_back(std::move(mesh));
    if (const auto message = validate_geometry(checked); !message.empty()) {
        error("invalid-geometry", indices ? indices->location : positions.location,
              message + " after conversion to binary32");
        return;
    }
    register_mesh(directive.location, std::move(descriptor), std::move(checked.meshes.front()));
}

void Interpreter::ply_shape(const Directive &directive, Descriptor descriptor) {
    const auto fields = validate(descriptor, directive, "shape", ply_fields);
    const auto found = fields.find("filename");
    if (found == fields.end()) {
        result_.document.dependencies_complete = false;
        return;
    }
    const auto &filename = found->second->values.front();
    if (filename.text.empty()) {
        result_.document.dependencies_complete = false;
        error("missing-parameter", filename.location, "Required PLY file path is empty");
        return;
    }
    const auto index = dependency(DependencyKind::mesh, filename);
    if (!index) {
        return;
    }
    auto &input = result_.document.dependencies[*index];
    if (!input.exists) {
        return;
    }
    auto cached = ply_cache_.lower_bound(*index);
    if (cached == ply_cache_.end() || cached->first != *index) {
        cached = ply_cache_.emplace_hint(cached, *index,
                                         decode_ply(input.resolved_path, limits_, ply_remaining_));
        if (const auto *problem = std::get_if<PlyIssue>(&cached->second)) {
            issue(problem->severity, problem->code, filename.location,
                  "PLY \"" + input.resolved_path.string() + "\" " + problem->message);
            return;
        }
        input.visited = true;
    }
    if (const auto *mesh = std::get_if<PlyMesh>(&cached->second);
        mesh && charge_mesh(mesh->value_count, directive.location)) {
        register_mesh(directive.location, std::move(descriptor), mesh->geometry);
    }
}

bool Interpreter::charge_mesh(std::size_t count, const Location &location) {
    if (count > limits_.total_values - mesh_values_) {
        if (!budget_exceeded_) {
            error("limit-exceeded", location, "Translated mesh value limit exceeded");
            budget_exceeded_ = true;
        }
        return false;
    }
    mesh_values_ += count;
    return true;
}

void Interpreter::register_mesh(const Location &location, Descriptor descriptor,
                                GeometryMesh mesh) {
    result_.inventory.triangles += mesh.triangles.size();
    result_.inventory.patches += mesh.patches.size();
    const auto mesh_index = static_cast<std::uint32_t>(scene_.geometry.meshes.size());
    scene_.geometry.meshes.push_back(std::move(mesh));
    Shape checked_shape{std::move(descriptor), state_.material, state_.reverse, mesh_index,
                        state_.area_light};
    if (current_object_) {
        scene_.objects[*current_object_].shapes.push_back(std::move(checked_shape));
    } else {
        add_instance(mesh_index, checked_shape.descriptor.transform, location);
        scene_.shapes.push_back(std::move(checked_shape));
    }
}

void Interpreter::attribute(const Directive &directive) {
    const string_view target = directive.arguments.front().text;
    if (!one_of(target, {"shape", "light", "material", "medium", "texture"})) {
        error("invalid-parameter", directive.location,
              "Unknown Attribute target \"" + std::string(target) + "\"");
        return;
    }
    for (const auto &parameter : directive.parameters) {
        const AttributeKey key{target, parameter.name, parameter.type};
        const auto found = attributes_.find(key);
        if (!scopes_.empty()) {
            attribute_changes_.push_back(
                {key, found != attributes_.end() ? std::optional(found->second) : std::nullopt});
        }
        attributes_.insert_or_assign(
            key,
            AttributeDefault{{std::make_shared<const Parameter>(parameter), state_.color_space},
                             ++attribute_order_});
    }
}

void Interpreter::begin_scope(const Directive &directive, bool object) {
    if (scopes_.size() >= limits_.scopes) {
        error("limit-exceeded", directive.location, "Scene scope limit exceeded");
        result_.document.dependencies_complete = false;
        scope_overflow_ = 1;
        return;
    }
    scopes_.push_back({directive.location, object, state_, attribute_changes_.size()});
    if (!object) {
        return;
    }
    const string_view name = directive.arguments.front().text;
    if (current_object_) {
        error("mismatched-scope", directive.location, "Nested object definitions are invalid");
    } else if (name.empty() || !objects_.try_emplace(name, scene_.objects.size()).second) {
        error("duplicate-definition", directive.location,
              "Empty or duplicate object definition \"" + std::string(name) + "\"");
    } else {
        current_object_ = scene_.objects.size();
        scene_.objects.push_back({std::string(name), directive.location, {}});
        return;
    }
    scope_overflow_ = 1;
    scopes_.pop_back();
    result_.document.dependencies_complete = false;
}

void Interpreter::end_scope(const Directive &directive, bool object) {
    if (scopes_.empty() || scopes_.back().object != object) {
        error("mismatched-scope", directive.location, "Unexpected " + directive.name);
        return;
    }
    const auto mark = scopes_.back().attribute_mark;
    while (attribute_changes_.size() > mark) {
        const auto &change = attribute_changes_.back();
        if (change.previous) {
            attributes_.insert_or_assign(change.key, *change.previous);
        } else {
            attributes_.erase(change.key);
        }
        attribute_changes_.pop_back();
    }
    state_ = std::move(scopes_.back().state);
    scopes_.pop_back();
    if (object) {
        current_object_.reset();
    }
}

void Interpreter::state_directive(const Directive &directive) {
    const string_view name = directive.name;
    const auto argument = [&]() -> const Value & { return directive.arguments.front(); };
    if (name == "WorldBegin") {
        if (world_) {
            error("wrong-block", directive.location, "WorldBegin appears more than once");
            return;
        }
        finalize_options();
        world_ = true;
        state_.transform = {};
        state_.active = {true, true};
        coordinates_["world"] = state_.transform;
    } else if (name == "ActiveTransform") {
        state_.active = {argument().text != "EndTime", argument().text != "StartTime"};
    } else if (name == "TransformTimes") {
        if (argument().number > directive.arguments[1].number) {
            error("invalid-transform", directive.location, "TransformTimes start is after its end");
        }
    } else if (name == "ColorSpace") {
        auto color_space = argument().text;
        std::ranges::transform(color_space, color_space.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (!one_of(color_space, {"srgb", "dci-p3", "rec2020", "aces2065-1"})) {
            error("invalid-parameter", argument().location,
                  "Unknown color space \"" + argument().text + "\"");
            return;
        }
        state_.color_space = std::move(color_space);
    } else if (name == "CoordinateSystem") {
        coordinates_.insert_or_assign(argument().text, state_.transform);
    } else if (name == "CoordSysTransform") {
        if (const auto found = coordinates_.find(argument().text); found != coordinates_.end()) {
            state_.transform = found->second;
        } else {
            error("undefined-reference", argument().location,
                  "Undefined coordinate system \"" + argument().text + "\"");
        }
    } else {
        const auto next = transform_argument(directive);
        if (!next) {
            error("invalid-transform", directive.location,
                  "Degenerate " + directive.name + " transform");
            return;
        }
        if (!affine(*next)) {
            unsupported(directive.location,
                        "Projective transforms are outside the structural profile");
            return;
        }
        if (exact_singular_linear(*next)) {
            error("invalid-transform", directive.location, "Transform operand is singular");
            return;
        }
        const bool replace = name == "Transform" || name == "Identity";
        for (std::size_t i = 0; i < state_.transform.size(); ++i) {
            if (!state_.active[i]) {
                continue;
            }
            const Transform composed = replace ? *next : multiply(state_.transform[i], *next);
            if (!inverse_affine(composed)) {
                error("invalid-transform", directive.location,
                      "Transform is singular or has no finite inverse");
            } else {
                state_.transform[i] = composed;
            }
        }
    }
}

void Interpreter::deferred(const Directive &directive) {
    string_view target;
    string_view type;
    if (directive.name == "MediumInterface") {
        for (const auto &medium : directive.arguments) {
            if (!medium.text.empty()) {
                medium_uses_.push_back(&medium);
            }
        }
        if (!directive.arguments.front().text.empty() || !directive.arguments.back().text.empty()) {
            unsupported(directive.location, "MediumInterface is outside the structural profile");
        }
        return;
    }

    const auto &name = directive.arguments.front();
    bool known;
    if (directive.name == "Texture") {
        target = "texture";
        type = directive.arguments[2].text;
        const auto &channel = directive.arguments[1];
        if (!one_of(channel.text, {"float", "spectrum"})) {
            error("unknown-type", channel.location,
                  "Unknown Texture value type \"" + channel.text + "\"");
            return;
        }
        if (name.text.empty() || !textures_[channel.text == "spectrum"].insert(name.text).second) {
            error("duplicate-definition", name.location,
                  "Empty or duplicate texture definition \"" + name.text + "\"");
        }
        known = one_of(type, {"constant", "scale", "mix", "directionmix", "bilerp", "imagemap",
                              "checkerboard", "dots", "ptex"}) ||
                (channel.text == "float" && one_of(type, {"fbm", "wrinkled", "windy"})) ||
                (channel.text == "spectrum" && type == "marble");
    } else {
        target = "medium";
        if (name.text.empty() || !media_.insert(name.text).second) {
            error("duplicate-definition", name.location,
                  "Empty or duplicate medium definition \"" + name.text + "\"");
        }
        const Field field{"type", "string", 1, false, true};
        const auto *parameter = select(directive, target, field);
        if (!parameter) {
            error("missing-parameter", directive.location, "Named medium requires \"string type\"");
            return;
        }
        if (!field_valid(field, *parameter)) {
            return;
        }
        type = parameter->values.front().text;
        known = one_of(type, {"homogeneous", "uniformgrid", "rgbgrid", "cloud", "nanovdb"});
    }
    dependencies(directive, target, type);
    deferred_type(directive, type, known, target);
}

void Interpreter::visit(const Directive &directive) {
    string_view name = directive.name;
    if (name == "TransformBegin") {
        name = "AttributeBegin";
    } else if (name == "TransformEnd") {
        name = "AttributeEnd";
    }
    if (scope_overflow_ > 0) {
        if (one_of(name, {"ObjectBegin", "AttributeBegin"})) {
            ++scope_overflow_;
        } else if (one_of(name, {"ObjectEnd", "AttributeEnd"})) {
            --scope_overflow_;
        }
        return;
    }
    if (one_of(name, {"Include", "WorldEnd"})) {
        return; // The parser handles file expansion and reports legacy directives.
    }
    const bool option_name = one_of(
        name, {"Camera", "Film", "PixelFilter", "Sampler", "Integrator", "Accelerator", "Option"});
    const bool world_name = one_of(
        name, {"AttributeBegin", "AttributeEnd", "ObjectBegin", "ObjectEnd", "ObjectInstance",
               "Material", "MakeNamedMaterial", "NamedMaterial", "LightSource", "AreaLightSource",
               "Texture", "Shape", "ReverseOrientation", "Import"});
    if ((world_ && (option_name || name == "TransformTimes")) || (!world_ && world_name)) {
        error("wrong-block", directive.location,
              directive.name + (world_ ? " must precede WorldBegin" : " must follow WorldBegin"));
        // Do not apply graphics-state changes in the wrong block.
        return;
    }

    if (option_name) {
        option(directive);
    } else if (name == "Attribute") {
        attribute(directive);
    } else if (one_of(name, {"AttributeBegin", "ObjectBegin"})) {
        begin_scope(directive, name == "ObjectBegin");
    } else if (one_of(name, {"AttributeEnd", "ObjectEnd"})) {
        end_scope(directive, name == "ObjectEnd");
    } else if (name == "ObjectInstance") {
        ++result_.inventory.instances;
        if (current_object_) {
            error("mismatched-scope", directive.location,
                  "ObjectInstance inside an object definition");
        } else {
            scene_.placements.push_back({directive.arguments.front().text,
                                         capture_transform(directive.location),
                                         directive.location});
        }
    } else if (one_of(name, {"Material", "MakeNamedMaterial"})) {
        material(directive);
    } else if (name == "NamedMaterial") {
        const auto &argument = directive.arguments.front();
        state_.material = {std::make_shared<const std::string>(argument.text), nullptr,
                           argument.location};
        material_uses_.try_emplace(argument.text, argument.location);
    } else if (one_of(name, {"LightSource", "AreaLightSource"})) {
        light(directive);
    } else if (name == "Shape") {
        shape(directive);
    } else if (name == "ReverseOrientation") {
        state_.reverse = !state_.reverse;
    } else if (one_of(name, {"Texture", "MakeNamedMedium", "MediumInterface"})) {
        deferred(directive);
    } else if (name != "Import") {
        state_directive(directive);
    }
}

void Interpreter::add_instance(std::uint32_t mesh, const Transform &transform,
                               const Location &location) {
    if (scene_.geometry.instances.size() >= limits_.total_values) {
        if (!budget_exceeded_) {
            error("limit-exceeded", location, "Expanded geometry instance limit exceeded");
            budget_exceeded_ = true;
        }
        return;
    }
    GeometryTransform narrow;
    for (std::size_t i = 0; i < narrow.values.size(); ++i) {
        if (!representable_float(transform.values[i])) {
            error("invalid-geometry", location, "Instance transform overflows binary32");
            return;
        }
        narrow.values[i] = static_cast<float>(transform.values[i]);
    }

    // The same geometry validator tests binary32 singularity for direct and
    // composed instances. A dummy empty mesh avoids rereading each instance's mesh.
    GeometryData checked;
    checked.meshes.emplace_back();
    checked.instances.push_back({0, narrow});
    if (const auto message = validate_geometry(checked); !message.empty()) {
        error("invalid-geometry", location, message + " after conversion to binary32");
        return;
    }
    scene_.geometry.instances.push_back({mesh, narrow});
}

void Interpreter::resolve_references() {
    const bool unknown_definitions =
        !result_.document.syntax_complete || skipped_include_ ||
        std::ranges::any_of(result_.document.dependencies, [](const Dependency &dependency) {
            return dependency.kind == DependencyKind::import && !dependency.visited;
        });
    if (!unknown_definitions) {
        for (const auto &[name, location] : material_uses_) {
            if (result_.document.diagnostics_truncated) {
                return;
            }
            if (!named_materials_.contains(name)) {
                error("undefined-reference", location,
                      "Undefined named material \"" + std::string(name) + "\"");
            }
        }
        for (const auto &[location, name, channel] : texture_uses_) {
            if (result_.document.diagnostics_truncated) {
                return;
            }
            if ((channel == "spectrum" || !textures_[0].contains(name)) &&
                (channel == "float" || !textures_[1].contains(name))) {
                error("undefined-reference", location,
                      "Undefined " + std::string(channel) + (channel.empty() ? "" : " ") +
                          "texture \"" + name + "\"");
            }
        }
        for (const auto *value : medium_uses_) {
            if (result_.document.diagnostics_truncated) {
                return;
            }
            if (!media_.contains(value->text)) {
                error("undefined-reference", value->location,
                      "Undefined medium \"" + value->text + "\"");
            }
        }
    }
    std::size_t expanded = scene_.geometry.instances.size();
    for (const auto &placement : scene_.placements) {
        if (result_.document.diagnostics_truncated) {
            return;
        }
        const auto found = objects_.find(placement.object);
        if (found == objects_.end()) {
            if (!unknown_definitions) {
                error("undefined-reference", placement.location,
                      "Undefined object \"" + placement.object + "\"");
            }
            continue;
        }
        const auto &shapes = scene_.objects[found->second].shapes;
        if (shapes.size() > limits_.total_values - expanded) {
            error("limit-exceeded", placement.location,
                  "Expanded geometry instance limit exceeded");
            break;
        }
        expanded += shapes.size();
        for (const auto &member : shapes) {
            if (result_.document.diagnostics_truncated) {
                return;
            }
            add_instance(member.mesh_index,
                         multiply(placement.transform, member.descriptor.transform),
                         placement.location);
        }
    }
}

Result Interpreter::run() {
    std::set<SourceKey> unvisited_includes;
    for (const auto &dependency : result_.document.dependencies) {
        if (dependency.kind == DependencyKind::include && !dependency.visited) {
            unvisited_includes.insert(source_key(dependency.location));
        }
    }
    for (const auto &directive : result_.document.directives) {
        if (result_.document.diagnostics_truncated) {
            break;
        }
        if (directive.name == "Include" &&
            unvisited_includes.contains(source_key(directive.arguments.front().location))) {
            skipped_include_ = true;
        }
        if (skipped_include_) {
            scan_after_include(directive);
        } else {
            visit(directive);
        }
    }
    const bool parent_stream_complete = result_.document.syntax_complete && !skipped_include_ &&
                                        !result_.document.diagnostics_truncated;
    if (parent_stream_complete && !world_) {
        const auto &directives = result_.document.directives;
        const Location source = directives.empty()
                                    ? Location(result_.document.entry_directory.string())
                                    : directives.back().location;
        error("wrong-block", source, "Scene is missing WorldBegin");
    }
    if (!scopes_.empty() && parent_stream_complete) {
        const auto &scope = scopes_.back();
        error("mismatched-scope", scope.location,
              scope.object ? "Unclosed ObjectBegin" : "Unclosed AttributeBegin");
    }
    if (!result_.document.diagnostics_truncated) {
        resolve_references();
    }
    result_.document.dependencies_complete &=
        std::ranges::all_of(result_.document.dependencies, &Dependency::visited);
    const auto &document = result_.document;
    result_.structure_complete =
        !failed_ && document.syntax_complete && document.dependencies_complete &&
        !document.diagnostics_truncated &&
        std::ranges::none_of(document.diagnostics, [](const Diagnostic &diagnostic) {
            return diagnostic.severity != Severity::warning;
        });
    if (result_.structure_complete) {
        result_.scene = std::move(scene_);
    }
    return std::move(result_);
}

} // namespace

Descriptor::EffectiveParameter Descriptor::effective_parameter(std::string_view name) const {
    for (const auto index : effective_authored) {
        if (const auto &parameter = authored.parameters[index]; parameter.name == name) {
            return {&parameter, color_space};
        }
    }
    for (const auto &entry : inherited) {
        if (entry.parameter->name == name) {
            return {entry.parameter.get(), entry.color_space};
        }
    }
    return {};
}

Result interpret(Document document, const Limits &limits) {
    return Interpreter(std::move(document), limits).run();
}

} // namespace rypath::pbrt
