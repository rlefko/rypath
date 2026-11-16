// SPDX-License-Identifier: GPL-3.0-only

#include "bsdf_table.hpp"
#include "pbrt.hpp"
#include "pbrt_numeric.hpp"
#include "pbrt_spectra.hpp"
#include "ply.hpp"
#include "spectrum_file.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <exception>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <locale>
#include <map>
#include <new>
#include <numbers>
#include <set>
#include <span>
#include <sstream>
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
        return checked_inverse_affine(result);
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

std::optional<float> gamma_encoding(string_view encoding) {
    std::istringstream input{std::string(encoding)};
    input.imbue(std::locale::classic());
    std::string name;
    double gamma = 0;
    if (!(input >> name >> gamma) || name != "gamma" || !representable_float(gamma) ||
        static_cast<float>(gamma) <= 0) {
        return std::nullopt;
    }
    input >> std::ws;
    return input.eof() ? std::optional<float>(static_cast<float>(gamma)) : std::nullopt;
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
    bool allow_texture_reference = false;
    bool allow_spectrum_file = false;
    bool allow_empty_texture = false;
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
    Field{.name = "reflectance",
          .type = "spectral-texture",
          .rule = Rule::albedo,
          .allow_texture_reference = true,
          .allow_spectrum_file = true},
    Field{.name = "displacement", .type = "float-texture", .allow_texture_reference = true},
    Field{"normalmap", "string"},
};

constexpr std::array gltf_material_fields{
    Field{"basecolor", "rgb", 3, false, false, false, Rule::albedo},
    Field{"opacity", "float"},
    Field{"metallic", "float"},
    Field{"roughness", "float"},
    Field{"normalscale", "float"},
    Field{"emissive", "rgb", 3, false, false, false, Rule::albedo},
    Field{"alphamode", "string"},
    Field{"alphacutoff", "float"},
    Field{"doublesided", "bool"},
    Field{.name = "basecolortexture", .type = "spectral-texture", .allow_texture_reference = true},
    Field{.name = "metallicroughnesstexture",
          .type = "spectral-texture",
          .allow_texture_reference = true},
    Field{.name = "normaltexture", .type = "spectral-texture", .allow_texture_reference = true},
    Field{.name = "emissivetexture", .type = "spectral-texture", .allow_texture_reference = true},
};

constexpr std::array conductor_fields{
    Field{.name = "eta",
          .type = "spectral-texture",
          .allow_texture_reference = true,
          .allow_spectrum_file = true},
    Field{.name = "k",
          .type = "spectral-texture",
          .allow_texture_reference = true,
          .allow_spectrum_file = true},
    Field{.name = "roughness", .type = "float-texture", .allow_texture_reference = true},
    Field{.name = "uroughness", .type = "float-texture", .allow_texture_reference = true},
    Field{.name = "vroughness", .type = "float-texture", .allow_texture_reference = true},
    Field{"remaproughness", "bool"},
    material_fields[1],
    material_fields[2],
    material_fields[0],
};

constexpr std::array dielectric_fields{
    Field{.name = "eta", .type = "dielectric-eta", .allow_spectrum_file = true},
    conductor_fields[2],
    conductor_fields[3],
    conductor_fields[4],
    conductor_fields[5],
    material_fields[1],
    material_fields[2],
};

constexpr std::array float_constant_fields{Field{"value", "float"}};
constexpr std::array spectrum_constant_fields{
    Field{.name = "value", .type = "spectral", .rule = Rule::emission, .allow_spectrum_file = true},
};
constexpr std::array mapping_fields{
    Field{"mapping", "string"}, Field{"uscale", "float"}, Field{"vscale", "float"},
    Field{"udelta", "float"},   Field{"vdelta", "float"}, Field{"v1", "vector3", 3},
    Field{"v2", "vector3", 3},
};
constexpr std::array imagemap_fields{
    Field{"filename", "string", 1, false, true},
    Field{"encoding", "string"},
    Field{"filter", "string"},
    Field{"wrap", "string"},
    Field{"scale", "float"},
    Field{"invert", "bool"},
    Field{"maxanisotropy", "float"},
    Field{"rypath.gltf", "bool"},
    Field{"rypath.wraps", "integer"},
    Field{"rypath.wrapt", "integer"},
    Field{"rypath.minfilter", "integer"},
    Field{"rypath.magfilter", "integer"},
    mapping_fields[0],
    mapping_fields[1],
    mapping_fields[2],
    mapping_fields[3],
    mapping_fields[4],
    mapping_fields[5],
    mapping_fields[6],
};

constexpr std::array light_fields{
    Field{.name = "L", .type = "spectral", .rule = Rule::emission, .allow_spectrum_file = true},
    Field{"scale", "float", 1, false, false, false, Rule::nonnegative},
    Field{"illuminance", "float"},
    Field{"portal", "point3", 3, true, false, true},
    Field{"filename", "string"},
};

constexpr std::array point_light_fields{
    Field{.name = "I", .type = "spectral", .rule = Rule::emission, .allow_spectrum_file = true},
    light_fields[1],
    Field{"power", "float"},
    Field{"from", "point3", 3},
};

constexpr std::array spot_light_fields{
    point_light_fields[0],
    point_light_fields[1],
    point_light_fields[2],
    point_light_fields[3],
    Field{"to", "point3", 3},
    Field{"coneangle", "float"},
    Field{"conedeltaangle", "float"},
};

constexpr std::array area_light_fields{
    light_fields[0],
    light_fields[1],
    Field{"twosided", "bool"},
    Field{"power", "float"},
    Field{"filename", "string"},
};

constexpr Field alpha_field{.name = "alpha",
                            .type = "float-texture",
                            .prefer_texture = true,
                            .allow_texture_reference = true,
                            .allow_empty_texture = true};

constexpr std::array mesh_fields{
    Field{"P", "point3", 3, true, true},
    Field{"indices", "integer", 3, true},
    Field{"N", "normal", 3, true},
    Field{"S", "vector3", 3, true},
    Field{"uv", "point2", 2, true},
    Field{"faceIndices", "integer", 1, true, false, true},
    alpha_field,
};

constexpr std::array ply_fields{
    Field{"filename", "string", 1, false, true},
    alpha_field,
    Field{.name = "displacement",
          .type = "texture",
          .allow_texture_reference = true,
          .allow_empty_texture = true},
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
    if (allowed == "dielectric-eta") {
        return actual == "float" || accepts_type("spectral-texture", actual);
    }
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

bool builtin_spectrum(string_view name) { return pbrt_named_spectrum(name) != nullptr; }

struct GraphicsState {
    TransformSet transform{};
    std::array<bool, 2> active{true, true};
    MaterialBinding material;
    MediumBinding media{};
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
          asset_remaining_{
              limits.expanded_bytes - std::min(limits.expanded_bytes, document.expanded_bytes),
              limits.expansions - std::min(limits.expansions, document.expansion_count),
              limits.total_values},
          image_limits_(limits.image) {
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
        std::string medium;
    };
    std::vector<PendingOption> options_;
    std::map<string_view, std::size_t> option_indices_;
    std::optional<std::size_t> current_object_;
    std::map<std::string, Location, std::less<>> material_uses_;
    std::map<std::pair<DependencyKind, std::filesystem::path>, std::size_t> inventoried_paths_;
    std::map<std::size_t, PlyResult> ply_cache_;
    std::map<std::size_t, std::shared_ptr<const CheckedSpectrumSource>> decoded_spectra_;
    PlyBudget asset_remaining_;
    std::size_t bsdf_working_used_ = 0;
    std::map<std::pair<std::filesystem::path, BsdfTableKind>, std::shared_ptr<const BsdfTable>>
        bsdf_cache_;
    std::map<std::pair<std::size_t, bool>, std::shared_ptr<const CheckedImageSource>>
        decoded_images_;
    using ImageVariant = std::tuple<std::size_t, ImageEncoding, ImageWrap, float, bool, bool>;
    std::map<ImageVariant, std::shared_ptr<const ImagePyramid>> image_pyramids_;
    std::map<const ImagePyramid *, bool> binary_pyramids_;
    ImageLimits image_limits_;
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
    void texture(const Directive &directive);
    void imagemap(const Directive &directive, bool spectrum, bool unique);
    void procedural_texture(const Directive &directive, bool spectrum, bool unique);
    void texture_mapping(Descriptor &descriptor, bool spatial = false);
    std::shared_ptr<const CheckedImageSource> image_source(const Value &filename,
                                                           bool gltf = false);
    void native_image(Descriptor &descriptor, string_view field, ImageEncoding encoding,
                      ImageWrap wrap, bool square = false);
    bool binary_image(const ImagePyramid &image);
    void order_textures();
    void emitter_masks();
    void shape_controls(const Descriptor &descriptor);
    std::shared_ptr<const DecodedImage> decode_image(const std::filesystem::path &path,
                                                     const Location &source, bool gltf = false);
    std::shared_ptr<const ImagePyramid> image_pyramid(const Location &location,
                                                      const CheckedImageSource &source,
                                                      ImageEncoding encoding, ImageWrap wrap,
                                                      float gamma = 1, bool native = false,
                                                      bool gltf = false);
    void light(const Directive &directive);
    std::shared_ptr<const CheckedSpectrumSource> spectrum_file(const Value &filename);
    void shape(const Directive &directive);
    void ply_shape(const Directive &directive, Descriptor descriptor);
    void register_mesh(const Location &location, Descriptor descriptor, GeometryMesh mesh);
    bool charge_mesh(std::size_t count, const Location &location);
    void medium(const Directive &directive);
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
              kind == DependencyKind::spectrum
                  ? "Missing or inaccessible spectrum file \"" +
                        result_.document.dependencies.back().resolved_path.string() + "\":1:1"
                  : "Missing or inaccessible input file \"" + value.text + "\"");
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
            file("filename", DependencyKind::bsdf, true);
        } else if (type == "fourier") {
            file("bsdffile", DependencyKind::bsdf, true);
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
                   (target == "light" && one_of(name, {"I", "L"})) ||
                   (target == "medium" && one_of(name, {"sigma_a", "sigma_s", "Le"}))) {
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
    const auto scan_parameter = [&](const Field &field) {
        Field lookup = field;
        if (field.type == "dielectric-eta") {
            lookup.type =
                select(directive, {}, Field{field.name, "float"}) ? "float" : "spectral-texture";
        }
        if (const auto *parameter = select(directive, {}, lookup);
            parameter && charge_validation(parameter->values.size(), parameter->location)) {
            parameter_dependencies(*parameter);
        }
    };
    if (name == "Material" || name == "MakeNamedMaterial" || name == "MakeNamedMedium") {
        const string_view target = name == "MakeNamedMedium" ? "medium" : "material";
        const auto *parameter =
            name == "Material" ? nullptr : select(directive, {}, Field{"type", "string"});
        const string_view type = parameter ? string_view(parameter->values.front().text) : argument;
        dependencies(directive, target, type);
        if (target == "material" && type == "diffuse") {
            scan_parameter(material_fields.front());
            return;
        }
        if (target == "material" && one_of(type, {"conductor", "dielectric"})) {
            for (const auto &field : type == "conductor"
                                         ? std::span<const Field>(conductor_fields)
                                         : std::span<const Field>(dielectric_fields)) {
                scan_parameter(field);
            }
            return;
        }
    } else if (name == "Shape") {
        dependencies(directive, "shape", argument);
    } else if (name == "Texture") {
        const string_view channel = directive.arguments[1].text;
        const string_view type = directive.arguments[2].text;
        if (type == "constant" && one_of(channel, {"float", "spectrum"})) {
            if (channel == "spectrum") {
                scan_parameter(spectrum_constant_fields.front());
            }
            return;
        }
        dependencies(directive, "texture", type);
    } else if (name == "LightSource" || name == "AreaLightSource") {
        if (name == "AreaLightSource" && argument.empty()) {
            if (!directive.parameters.empty()) {
                error("unused-parameter", directive.parameters.front().location,
                      "An empty area light declaration takes no parameters");
            }
            return;
        }
        dependencies(directive, "light", argument);
        if (name == "LightSource" && one_of(argument, {"point", "spot"})) {
            scan_parameter(point_light_fields.front());
            return;
        }
        if ((name == "LightSource" && argument == "infinite") ||
            (name == "AreaLightSource" && argument == "diffuse")) {
            scan_parameter(light_fields.front());
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
        if (field.type == "dielectric-eta") {
            // PBRT asks for float eta first, independently of spectral declaration order.
            lookup.type = select(directive, target, Field{field.name, "float"})
                              ? "float"
                              : "spectral-texture";
        }
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
            const bool inert_texture = field.allow_empty_texture && parameter->type == "texture" &&
                                       valid && parameter->values.front().text.empty();
            const bool file_spectrum = valid && parameter->type == "spectrum" &&
                                       parameter->values.front().kind == ValueKind::string &&
                                       !builtin_spectrum(parameter->values.front().text);
            // A malformed cardinality can still name discoverable inputs. The
            // validation charge bounds this scan even for inherited parameters.
            if (file_spectrum && field.allow_spectrum_file) {
                auto file = spectrum_file(parameter->values.front());
                if ((directive.name == "Texture" &&
                     directive.arguments.back().text != "constant") ||
                    target == "material" || target == "medium") {
                    if (target == "material" && field.name == "reflectance") {
                        descriptor.spectrum_file = file;
                        descriptor.spectrum_file_parameter = field.name;
                    }
                    descriptor.spectrum_files.emplace_back(field.name, std::move(file));
                } else {
                    descriptor.spectrum_file = std::move(file);
                    descriptor.spectrum_file_parameter = field.name;
                }
            } else if (!validation_exhausted_ && !inert_texture) {
                parameter_dependencies(*parameter, one_of(field.type, {"float-texture", "texture"})
                                                       ? "float"
                                                       : "spectrum");
            }
            if (!valid) {
                continue;
            }
            effective.emplace(field.name, parameter);
            if (field.deferred ||
                (parameter->type == "texture" && !inert_texture &&
                 !field.allow_texture_reference) ||
                (file_spectrum && !field.allow_spectrum_file)) {
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
            const auto scale = value.kind == ValueKind::integer ? value.integer : value.number;
            valid = (value.kind == ValueKind::number || value.kind == ValueKind::integer) &&
                    representable_float(scale) && static_cast<float>(scale) > 0;
            if (valid) {
                scene_.displacement_edge_scale = static_cast<float>(scale);
                return;
            }
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

    PendingOption pending{&directive, {}, state_.color_space, false, {}};
    if (directive.name == "Camera") {
        pending.medium = state_.media.outside;
        TransformSet world_from_camera{};
        bool invertible = true;
        for (std::size_t i = 0; i < state_.transform.size(); ++i) {
            if (const auto inverse = checked_inverse_affine(state_.transform[i])) {
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
    if (directive.name == "Camera")
        scene_.camera_medium = pending.medium;
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
        supported_type = type == "volpath" ? "volpath" : "path";
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
    if (!one_of(type, {"diffuse", "conductor", "dielectric", "thindielectric", "coateddiffuse",
                       "coatedconductor", "diffusetransmission", "mix", "measured", "fourier",
                       "interface", "none", "", "rypath-gltf-v1"})) {
        deferred_type(directive, type,
                      one_of(type, {"", "none", "interface", "hair", "subsurface"}), "material");
        return;
    }
    Descriptor descriptor = describe(directive, false);
    std::vector<Field> schema;
    const auto add = [&](std::span<const Field> fields) {
        schema.insert(schema.end(), fields.begin(), fields.end());
    };
    if (named)
        schema.push_back(Field{"type", "string", 1, false, true});
    if (type == "rypath-gltf-v1")
        add(gltf_material_fields);
    else if (type == "diffuse" || type == "diffusetransmission")
        add(material_fields);
    else if (type == "conductor")
        add(conductor_fields);
    else if (type == "dielectric" || type == "coateddiffuse")
        add(dielectric_fields);
    else if (type == "thindielectric") {
        schema.push_back(dielectric_fields[0]);
        schema.push_back(material_fields[1]);
        schema.push_back(material_fields[2]);
    } else if (!one_of(type, {"mix", "interface", "none", ""})) {
        schema.push_back(material_fields[1]);
        schema.push_back(material_fields[2]);
    }
    const auto spectral = [&](string_view field) {
        schema.push_back(Field{.name = field,
                               .type = "spectral-texture",
                               .rule = Rule::albedo,
                               .allow_texture_reference = true,
                               .allow_spectrum_file = true});
    };
    const auto scalar = [&](string_view field) {
        schema.push_back(
            Field{.name = field, .type = "float-texture", .allow_texture_reference = true});
    };
    if (type == "coateddiffuse")
        spectral("reflectance");
    if (type == "coatedconductor") {
        for (const auto field : {"conductor.eta", "conductor.k"})
            schema.push_back(Field{.name = field,
                                   .type = "spectral-texture",
                                   .allow_texture_reference = true,
                                   .allow_spectrum_file = true});
        spectral("reflectance");
        schema.push_back(
            Field{.name = "interface.eta", .type = "dielectric-eta", .allow_spectrum_file = true});
        for (const auto field :
             {"interface.roughness", "interface.uroughness", "interface.vroughness",
              "conductor.roughness", "conductor.uroughness", "conductor.vroughness"})
            scalar(field);
        schema.push_back(Field{"remaproughness", "bool"});
    }
    if (type == "coateddiffuse" || type == "coatedconductor") {
        spectral("albedo");
        scalar("g");
        scalar("thickness");
        schema.push_back(Field{"maxdepth", "integer", 1, false, false, false, Rule::nonnegative});
        schema.push_back(Field{"nsamples", "integer", 1, false, false, false, Rule::positive});
    }
    if (type == "diffusetransmission") {
        spectral("transmittance");
        schema.push_back(Field{"scale", "float"});
    }
    if (type == "mix") {
        schema.push_back(Field{"materials", "string", 2, false, true});
        scalar("amount");
    }
    if (type == "measured" || type == "fourier")
        schema.push_back(
            Field{type == "measured" ? "filename" : "bsdffile", "string", 1, false, true});
    const auto fields = validate(descriptor, directive, "material", schema);
    if (type == "mix" && fields.contains("materials"))
        for (const auto &value : fields.at("materials")->values)
            material_uses_.try_emplace(value.text, value.location);
    if (type == "conductor" || type == "coatedconductor") {
        const auto eta = type == "conductor" ? "eta" : "conductor.eta";
        const auto k = type == "conductor" ? "k" : "conductor.k";
        if (fields.contains("reflectance") && (fields.contains(eta) || fields.contains(k)))
            error("invalid-parameter", fields.at("reflectance")->location,
                  "Conductor reflectance cannot be combined with eta or k");
    }
    if (type == "coateddiffuse" || type == "coatedconductor") {
        const auto depth =
            fields.contains("maxdepth") ? fields.at("maxdepth")->values.front().integer : 10;
        const auto samples =
            fields.contains("nsamples") ? fields.at("nsamples")->values.front().integer : 1;
        if (depth > 1024 || samples > 64 || static_cast<long long>(depth) * samples > 65536)
            error("limit-exceeded", directive.location,
                  "Layered material maxdepth/nsamples exceed checked work limits");
    }
    if (type == "measured" || type == "fourier") {
        const auto field = type == "measured" ? "filename" : "bsdffile";
        if (fields.contains(field)) {
            const auto &value = fields.at(field)->values.front();
            const auto index = dependency(DependencyKind::bsdf, value);
            if (index && result_.document.dependencies[*index].exists) {
                auto &input = result_.document.dependencies[*index];
                const auto kind =
                    type == "measured" ? BsdfTableKind::measured : BsdfTableKind::fourier;
                const auto key = std::pair{input.resolved_path, kind};
                if (const auto cached = bsdf_cache_.find(key); cached != bsdf_cache_.end())
                    descriptor.bsdf_table = cached->second;
                else {
                    auto decoded = read_bsdf_table(
                        input.resolved_path, kind,
                        {.remaining_budget = limits_.bsdf_working_bytes - bsdf_working_used_,
                         .per_file_budget = limits_.bsdf_file_bytes});
                    if (!decoded.table)
                        error("invalid-bsdf-table", value.location, decoded.diagnostic);
                    else {
                        bsdf_working_used_ += decoded.table->working_bytes;
                        descriptor.bsdf_table = decoded.table;
                        bsdf_cache_.emplace(key, std::move(decoded.table));
                    }
                }
                input.visited = descriptor.bsdf_table != nullptr;
            }
        }
    }
    if (type == "rypath-gltf-v1") {
        for (const auto name : {"opacity", "metallic", "roughness", "alphacutoff", "normalscale"}) {
            if (fields.contains(name)) {
                const auto &v = fields.at(name)->values.front();
                const bool unit = one_of(name, {"opacity", "metallic", "roughness"});
                if (!representable_float(v.number) ||
                    (name != string_view("normalscale") && v.number < 0) || (unit && v.number > 1))
                    error("invalid-parameter", v.location, "Invalid glTF material factor");
            }
        }
        if (fields.contains("alphamode") &&
            !one_of(fields.at("alphamode")->values.front().text, {"OPAQUE", "MASK", "BLEND"}))
            error("invalid-parameter", fields.at("alphamode")->location, "Unknown glTF alpha mode");
        for (const auto name :
             {"basecolortexture", "metallicroughnesstexture", "normaltexture", "emissivetexture"})
            if (fields.contains(name) && fields.at(name)->type != "texture")
                error("invalid-parameter", fields.at(name)->location,
                      "glTF image slots require named texture references");
    }
    native_image(descriptor, "normalmap", ImageEncoding::linear, ImageWrap::repeat);
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
                native_image(descriptor, "filename", ImageEncoding::srgb, ImageWrap::clamp);
            }
        }
        state_.area_light = std::make_shared<const Descriptor>(std::move(descriptor));
        scene_.area_lights.push_back(state_.area_light);
        return;
    }
    if (!area && one_of(type, {"point", "spot"})) {
        Descriptor descriptor = describe(directive);
        descriptor.outside_medium = state_.media.outside;
        if (type == "spot")
            validate(descriptor, directive, "light", spot_light_fields);
        else
            validate(descriptor, directive, "light", point_light_fields);
        if (type == "spot") {
            const auto *cone_parameter = descriptor.effective_parameter("coneangle").parameter;
            const auto *delta_parameter =
                descriptor.effective_parameter("conedeltaangle").parameter;
            const double cone = cone_parameter && cone_parameter->values.size() == 1
                                    ? cone_parameter->values.front().number
                                    : 30;
            const double delta = delta_parameter && delta_parameter->values.size() == 1
                                     ? delta_parameter->values.front().number
                                     : 5;
            if (!(cone > 0 && cone <= 180))
                error("invalid-parameter",
                      cone_parameter ? cone_parameter->location : directive.location,
                      "Spot cone angle must be in (0, 180]");
            if (!(delta >= 0 && delta <= cone))
                error("invalid-parameter",
                      delta_parameter ? delta_parameter->location : directive.location,
                      "Spot cone delta angle must be in [0, coneangle]");
            Vector from{}, to{0, 0, 1};
            const auto *from_parameter = descriptor.effective_parameter("from").parameter;
            const auto *to_parameter = descriptor.effective_parameter("to").parameter;
            if (from_parameter && from_parameter->values.size() == 3)
                for (std::size_t i = 0; i < 3; ++i)
                    from[i] = from_parameter->values[i].number;
            if (to_parameter && to_parameter->values.size() == 3)
                for (std::size_t i = 0; i < 3; ++i)
                    to[i] = to_parameter->values[i].number;
            if (from == to)
                error("invalid-parameter",
                      to_parameter ? to_parameter->location : directive.location,
                      "Spot from and to must be distinct");
        }
        const auto count = std::ranges::count_if(scene_.lights, [](const Descriptor &light) {
            return one_of(light.authored.arguments.front().text, {"point", "spot"});
        });
        if (count >= 64) {
            error("limit-exceeded", directive.location,
                  "Combined point and spot light count exceeds the limit of 64");
            return;
        }
        scene_.lights.push_back(std::move(descriptor));
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
    } else if (!fields.contains("portal")) {
        native_image(descriptor, "filename", ImageEncoding::srgb, ImageWrap::clamp, true);
    }
    scene_.lights.push_back(std::move(descriptor));
}

std::shared_ptr<const CheckedSpectrumSource> Interpreter::spectrum_file(const Value &filename) {
    if (filename.text.empty()) {
        result_.document.dependencies_complete = false;
        error("missing-parameter", filename.location, "Spectrum file path is empty");
        return {};
    }
    const auto index = dependency(DependencyKind::spectrum, filename);
    if (!index) {
        return {};
    }
    auto &input = result_.document.dependencies[*index];
    if (!input.exists) {
        return {};
    }
    auto [cached, first_attempt] = decoded_spectra_.try_emplace(*index);
    if (first_attempt) {
        auto decoded = decode_spectrum_file(input.resolved_path, limits_, asset_remaining_);
        if (const auto *problem = std::get_if<SpectrumFileIssue>(&decoded)) {
            error(problem->code, filename.location,
                  "Spectrum file \"" + input.resolved_path.string() +
                      "\":" + std::to_string(problem->line) + ":" +
                      std::to_string(problem->column) + ": " + problem->message);
        } else {
            cached->second =
                std::get<std::shared_ptr<const CheckedSpectrumSource>>(std::move(decoded));
            input.visited = true;
        }
    }
    return cached->second;
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
    shape_controls(descriptor);
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
    const auto *tangents = check_count("S", vertex_count * 3);
    const auto *uvs = check_count("uv", vertex_count * 2);
    check_count("faceIndices", triangle_count);
    if ((!normals && fields.contains("N")) || (!uvs && fields.contains("uv"))) {
        return;
    }
    // A large inherited mesh must not bypass the authored-value limit by being used repeatedly.
    const auto mesh_values =
        positions.values.size() + index_count + (normals ? normals->values.size() : 0) +
        (uvs ? uvs->values.size() : 0) + (tangents ? tangents->values.size() : 0);
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
        (normals && !read_points(*normals, mesh.normals, "Vertex normal component")) ||
        (tangents && !read_points(*tangents, mesh.tangents, "Vertex tangent component"))) {
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
    shape_controls(descriptor);
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
        cached = ply_cache_.emplace_hint(
            cached, *index, decode_ply(input.resolved_path, limits_, asset_remaining_));
        if (const auto *problem = std::get_if<PlyIssue>(&cached->second)) {
            issue(problem->severity, problem->code, filename.location,
                  "PLY \"" + input.resolved_path.string() + "\" " + problem->message);
            return;
        }
        input.visited = true;
    }
    if (const auto *mesh = std::get_if<PlyMesh>(&cached->second);
        mesh && charge_mesh(mesh->value_count, directive.location)) {
        if (const auto displacement = fields.find("displacement");
            displacement != fields.end() && !displacement->second->values.front().text.empty()) {
            const auto &source = displacement->second->location;
            if (mesh->geometry.uvs.empty()) {
                error("invalid-geometry", source, "PLY displacement requires authored vertex UVs");
            }
            const auto edge = fields.find("edgelength");
            const auto *length = edge == fields.end() ? nullptr : &edge->second->values.front();
            const double raw_length = length ? length->number : 1;
            const float scaled =
                representable_float(raw_length)
                    ? static_cast<float>(raw_length) * scene_.displacement_edge_scale
                    : 0;
            if (!(scaled > 0) || !std::isfinite(scaled)) {
                error("invalid-parameter", length ? length->location : source,
                      "Scaled displacement edge length must be positive and finite in binary32");
            }
        }
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
                        state_.area_light,     state_.media};
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
            if (!checked_inverse_affine(composed)) {
                error("invalid-transform", directive.location,
                      "Transform is singular or has no finite inverse");
            } else {
                state_.transform[i] = composed;
            }
        }
    }
}

void Interpreter::texture(const Directive &directive) {
    const auto &name = directive.arguments.front();
    const auto &channel = directive.arguments[1];
    const string_view type = directive.arguments[2].text;
    if (!one_of(channel.text, {"float", "spectrum"})) {
        error("unknown-type", channel.location,
              "Unknown Texture value type \"" + channel.text + "\"");
        return;
    }
    const bool spectrum = channel.text == "spectrum";
    const bool unique = !name.text.empty() && textures_[spectrum].insert(name.text).second;
    if (!unique) {
        error("duplicate-definition", name.location,
              "Empty or duplicate texture definition \"" + name.text + "\"");
    }
    if (type == "constant") {
        Descriptor descriptor = describe(directive, false);
        validate(descriptor, directive, "texture",
                 spectrum ? spectrum_constant_fields : float_constant_fields);
        auto &constants =
            spectrum ? scene_.spectrum_constant_textures : scene_.float_constant_textures;
        if (unique) {
            constants.push_back(std::move(descriptor));
        }
        return;
    }
    if (type == "imagemap") {
        imagemap(directive, spectrum, unique);
        return;
    }
    if (one_of(type, {"scale", "mix", "directionmix", "bilerp", "checkerboard", "dots"}) ||
        (!spectrum && one_of(type, {"fbm", "wrinkled", "windy"})) ||
        (spectrum && type == "marble")) {
        procedural_texture(directive, spectrum, unique);
        return;
    }
    const bool known = type == "ptex";
    dependencies(directive, "texture", type);
    deferred_type(directive, type, known, "texture");
}

void Interpreter::texture_mapping(Descriptor &descriptor, bool spatial) {
    using namespace texture_graph_wire;
    auto &mapping = descriptor.texture_mapping;
    const auto parameter = [&](string_view name) {
        const auto *selected = descriptor.effective_parameter(name).parameter;
        return selected && !selected->values.empty() ? selected : nullptr;
    };
    const auto *kind = parameter("mapping");
    const string_view name = kind ? string_view(kind->values.front().text) : "uv";
    if (spatial) {
        mapping.kind = texture_graph_wire::spatial;
    } else if (name == "uv") {
        mapping.kind = uv;
    } else if (name == "planar") {
        mapping.kind = planar;
        std::ranges::fill(mapping.values, 0);
        mapping.values[0] = 1;
        mapping.values[4] = 1;
    } else if (name == "spherical") {
        mapping.kind = spherical;
    } else if (name == "cylindrical") {
        mapping.kind = cylindrical;
    } else {
        error("invalid-parameter", kind->values.front().location,
              "Unknown texture mapping \"" + std::string(name) + "\"");
        return;
    }
    for (const auto &field : mapping_fields) {
        const auto *selected = parameter(field.name);
        if (!selected || field.name == "mapping") {
            continue;
        }
        const bool offset = one_of(field.name, {"udelta", "vdelta"});
        const bool vector = one_of(field.name, {"v1", "v2"});
        if (!(mapping.kind == uv && !vector) && !(mapping.kind == planar && (offset || vector))) {
            error("unused-parameter", selected->location,
                  "Texture parameter \"" + selected->name + "\" is unused by its mapping");
            continue;
        }
        const std::size_t index = mapping.kind == uv ? (field.name == "uscale"   ? 0
                                                        : field.name == "vscale" ? 1
                                                        : field.name == "udelta" ? 2
                                                                                 : 3)
                                                     : (field.name == "v1"       ? 0
                                                        : field.name == "v2"     ? 3
                                                        : field.name == "udelta" ? 6
                                                                                 : 7);
        if (selected->values.size() == field.width) {
            for (std::size_t component = 0; component < field.width; ++component) {
                const auto &value = selected->values[component];
                if (representable_float(value.number)) {
                    mapping.values[index + component] = static_cast<float>(value.number);
                } else {
                    error("invalid-parameter", value.location,
                          "Texture mapping parameter overflows binary32");
                }
            }
        }
    }
    if (mapping.kind == uv) {
        return;
    }
    descriptor.transform = capture_transform(descriptor.authored.location);
    const auto inverse = checked_inverse_affine(descriptor.transform);
    if (!inverse || !std::ranges::all_of(inverse->values, representable_float)) {
        error("invalid-parameter", descriptor.authored.location,
              "Texture mapping requires a finite, invertible affine transform");
        return;
    }
    descriptor.texture_from_world = *inverse;
    std::copy_n(inverse->values.begin(), 12, mapping.texture_from_render);
}

void Interpreter::procedural_texture(const Directive &directive, bool spectrum, bool unique) {
    const string_view type = directive.arguments[2].text;
    const auto operand = [spectrum](string_view name, bool reference = true, bool scalar = false) {
        const bool spectral = spectrum && !scalar;
        return Field{.name = name,
                     .type = spectral ? (reference ? "spectral-texture" : "spectral")
                                      : (reference ? "float-texture" : "float"),
                     .rule = spectral ? Rule::albedo : Rule::none,
                     .allow_texture_reference = reference,
                     .allow_spectrum_file = spectral};
    };
    std::vector<Field> schema;
    if (type == "scale") {
        schema = {operand("tex"), operand("scale", true, true)};
    } else if (type == "mix" || type == "directionmix" || type == "checkerboard") {
        schema = {operand("tex1"), operand("tex2")};
        if (type == "mix") {
            schema.push_back(operand("amount", true, true));
        } else if (type == "directionmix") {
            schema.push_back(Field{"dir", "vector3", 3});
        } else {
            schema.push_back(Field{"dimension", "integer"});
        }
    } else if (type == "bilerp") {
        schema = {operand("v00", false), operand("v01", false), operand("v10", false),
                  operand("v11", false)};
    } else if (type == "dots") {
        schema = {operand("inside"), operand("outside")};
    } else if (type != "windy") {
        schema = {Field{"octaves", "integer", 1, false, false, false, Rule::nonnegative},
                  Field{"roughness", "float"}};
        if (type == "marble") {
            schema.insert(schema.end(), {Field{"scale", "float"}, Field{"variation", "float"}});
        }
    }
    bool spatial = one_of(type, {"fbm", "wrinkled", "windy", "marble"});
    const bool mapped = one_of(type, {"bilerp", "checkerboard", "dots"});
    if (type == "checkerboard") {
        if (const auto *dim = select(directive, "texture", Field{"dimension", "integer"});
            dim && dim->values.size() == 1) {
            const auto value = dim->values.front().integer;
            spatial = value == 3;
            if (value != 2 && value != 3) {
                error("invalid-parameter", dim->location, "Checkerboard dimension must be 2 or 3");
            }
        }
    }
    if (mapped && !spatial) {
        schema.insert(schema.end(), mapping_fields.begin(), mapping_fields.end());
    }
    Descriptor descriptor = describe(directive, false);
    const auto fields = validate(descriptor, directive, "texture", schema);
    if (mapped || spatial) {
        texture_mapping(descriptor, spatial);
    }
    for (const auto &[name, selected] : fields) {
        if (selected->type == "float" && !representable_float(selected->values.front().number)) {
            error("invalid-parameter", selected->values.front().location,
                  "Texture parameter \"" + std::string(name) + "\" overflows binary32");
        }
    }
    if (const auto octaves = fields.find("octaves");
        octaves != fields.end() && octaves->second->values.front().integer >
                                       static_cast<std::int32_t>(texture_graph_wire::max_octaves)) {
        unsupported(octaves->second->location, "Procedural texture supports at most 32 octaves");
    }
    if (type == "directionmix") {
        Vector direction{0, 1, 0};
        if (const auto dir = fields.find("dir"); dir != fields.end()) {
            for (std::size_t i = 0; i < direction.size(); ++i) {
                direction[i] = dir->second->values[i].number;
            }
        }
        descriptor.transform = capture_transform(directive.location);
        Vector transformed{};
        for (std::size_t row = 0; row < transformed.size(); ++row) {
            for (std::size_t column = 0; column < direction.size(); ++column) {
                transformed[row] +=
                    descriptor.transform.values[row * 4 + column] * direction[column];
            }
        }
        if (!normalize(transformed)) {
            const auto dir = fields.find("dir");
            error("invalid-parameter",
                  dir == fields.end() ? directive.location : dir->second->location,
                  "Directionmix requires a finite nonzero transformed direction");
        } else {
            std::ranges::copy(transformed, descriptor.texture_direction.begin());
        }
    }
    if (unique) {
        scene_.procedural_textures.push_back(std::move(descriptor));
    }
}

void Interpreter::imagemap(const Directive &directive, bool spectrum, bool unique) {
    auto descriptor = describe(directive, false);
    const auto previous_issues = result_.document.diagnostics.size();
    const auto fields = validate(descriptor, directive, "texture", imagemap_fields);
    const auto file = fields.find("filename");
    if (file == fields.end()) {
        result_.document.dependencies_complete = false;
        return;
    }
    const auto &filename = file->second->values.front();
    if (filename.text.empty()) {
        result_.document.dependencies_complete = false;
        error("missing-parameter", filename.location, "Required imagemap file path is empty");
        return;
    }
    const auto index = dependency(DependencyKind::image, filename);
    texture_mapping(descriptor);
    const auto text = [&](string_view name, string_view fallback) {
        const auto found = fields.find(name);
        return found == fields.end() ? fallback : string_view(found->second->values.front().text);
    };
    const auto source_location = [&](string_view name) -> const Location & {
        return fields.at(name)->values.front().location;
    };
    const auto encoding_name = text("encoding", "sRGB");
    ImageEncoding encoding = ImageEncoding::srgb;
    float gamma = 1;
    if (encoding_name == "linear") {
        encoding = ImageEncoding::linear;
    } else if (encoding_name != "sRGB") {
        if (const auto exponent = gamma_encoding(encoding_name)) {
            encoding = ImageEncoding::gamma;
            gamma = *exponent;
        } else {
            error("invalid-parameter", source_location("encoding"),
                  "Unknown imagemap encoding \"" + std::string(encoding_name) + "\"");
        }
    }
    const auto wrap_name = text("wrap", "repeat");
    ImageWrap wrap = ImageWrap::repeat;
    if (wrap_name == "black") {
        wrap = ImageWrap::black;
    } else if (wrap_name == "clamp") {
        wrap = ImageWrap::clamp;
    } else if (wrap_name == "octahedralsphere") {
        unsupported(source_location("wrap"), "Imagemap octahedralsphere wrap is unsupported");
    } else if (wrap_name != "repeat") {
        error("invalid-parameter", source_location("wrap"),
              "Unknown imagemap wrap \"" + std::string(wrap_name) + "\"");
    }
    ImageTexture texture;
    const auto filter = text("filter", "bilinear");
    if (filter == "point") {
        texture.filter = ImageFilter::point;
    } else if (filter == "trilinear") {
        texture.filter = ImageFilter::trilinear;
    } else if (one_of(filter, {"ewa", "EWA"})) {
        texture.filter = ImageFilter::ewa;
    } else if (filter != "bilinear") {
        error("invalid-parameter", source_location("filter"),
              "Unknown imagemap filter \"" + std::string(filter) + "\"");
    }
    if (descriptor.texture_mapping.kind == texture_graph_wire::uv) {
        const auto &values = descriptor.texture_mapping.values;
        texture.uscale = values[0];
        texture.vscale = values[1];
        texture.udelta = values[2];
        texture.vdelta = values[3];
    }
    if (const auto found = fields.find("scale"); found != fields.end()) {
        const auto &value = found->second->values.front();
        if (representable_float(value.number)) {
            texture.scale = static_cast<float>(value.number);
        } else {
            error("invalid-parameter", value.location, "Imagemap scale overflows binary32");
        }
    }
    if (const auto found = fields.find("invert"); found != fields.end()) {
        texture.invert = found->second->values.front().boolean;
    }
    if (const auto found = fields.find("maxanisotropy"); found != fields.end()) {
        const auto &value = found->second->values.front();
        if (!std::isfinite(value.number)) {
            error("invalid-parameter", value.location, "Imagemap maxanisotropy must be finite");
        } else if (texture.filter == ImageFilter::ewa) {
            if (value.number < 1 || value.number > 16) {
                unsupported(value.location, "Imagemap EWA maxanisotropy must be in [1, 16]");
            } else {
                texture.max_anisotropy = static_cast<float>(value.number);
            }
        }
    }
    if (!unique || !index || result_.document.diagnostics_truncated ||
        previous_issues != result_.document.diagnostics.size()) {
        return;
    }
    const bool gltf =
        fields.contains("rypath.gltf") && fields.at("rypath.gltf")->values.front().boolean;
    if (gltf) {
        GltfSampler sampler;
        const auto setting = [&](string_view field, std::uint32_t fallback, auto allowed) {
            if (!fields.contains(field))
                return fallback;
            const auto value = fields.at(field)->values.front().integer;
            if (std::ranges::find(allowed, value) == allowed.end()) {
                error("invalid-parameter", source_location(field), "Invalid glTF sampler mode");
                return fallback;
            }
            return static_cast<std::uint32_t>(value);
        };
        sampler.wrap_s = setting("rypath.wraps", 10497, std::array{10497, 33071, 33648});
        sampler.wrap_t = setting("rypath.wrapt", 10497, std::array{10497, 33071, 33648});
        sampler.min_filter =
            setting("rypath.minfilter", 9987, std::array{9728, 9729, 9984, 9985, 9986, 9987});
        sampler.mag_filter = setting("rypath.magfilter", 9729, std::array{9728, 9729});
        texture.gltf_sampler = sampler;
        if (!spectrum || (encoding != ImageEncoding::srgb && encoding != ImageEncoding::linear))
            error("invalid-parameter", directive.location,
                  "glTF images require spectrum type and sRGB or linear encoding");
        for (const auto field : {"filter", "wrap", "scale", "invert", "maxanisotropy", "mapping",
                                 "uscale", "vscale", "udelta", "vdelta", "v1", "v2"})
            if (fields.contains(field))
                error("invalid-parameter", source_location(field),
                      "Standard imagemap controls cannot be mixed with glTF samplers");
    } else {
        for (const auto field :
             {"rypath.wraps", "rypath.wrapt", "rypath.minfilter", "rypath.magfilter"})
            if (fields.contains(field))
                error("invalid-parameter", source_location(field),
                      "glTF sampler parameters require rypath.gltf=true");
    }
    if (previous_issues != result_.document.diagnostics.size())
        return;
    if (auto source = image_source(filename, gltf)) {
        if (gltf && source->pixels->file_format != ImageFileFormat::png &&
            source->pixels->file_format != ImageFileFormat::jpeg) {
            error("invalid-image", filename.location, "glTF images require PNG or JPEG");
            return;
        }
        texture.image =
            image_pyramid(filename.location, *source, encoding, wrap, gamma, false, gltf);
        if (texture.image) {
            auto &images = spectrum ? scene_.spectrum_image_textures : scene_.float_image_textures;
            images.push_back({std::move(descriptor), std::move(source), std::move(texture)});
        }
    }
}

std::shared_ptr<const CheckedImageSource> Interpreter::image_source(const Value &filename,
                                                                    bool gltf) {
    const auto index = dependency(DependencyKind::image, filename);
    if (!index) {
        return {};
    }
    auto &input = result_.document.dependencies[*index];
    if (!input.exists) {
        return {};
    }
    auto [cached, first_attempt] = decoded_images_.try_emplace(std::pair{*index, gltf});
    if (first_attempt) {
        if (auto pixels = decode_image(input.resolved_path, filename.location, gltf)) {
            cached->second = std::make_shared<const CheckedImageSource>(
                CheckedImageSource{input.resolved_path, std::move(pixels)});
            input.visited = true;
        }
    }
    return cached->second;
}

void Interpreter::native_image(Descriptor &descriptor, string_view field, ImageEncoding encoding,
                               ImageWrap wrap, bool square) {
    const auto *selected = descriptor.effective_parameter(field).parameter;
    if (!selected || selected->values.size() != 1 || selected->values.front().text.empty()) {
        return;
    }
    const auto &filename = selected->values.front();
    if (const auto source = image_source(filename)) {
        if (source->pixels->source_channels < 3) {
            error("invalid-image", filename.location,
                  "Normal and light maps require an image with R, G, and B channels");
        } else if (square && source->pixels->width != source->pixels->height) {
            error("invalid-image", filename.location,
                  "An equal-area infinite light image must be square");
        } else if (field != "normalmap" &&
                   std::ranges::any_of(source->pixels->floating_rgb,
                                       [](float value) { return value < 0; })) {
            error("invalid-image", filename.location,
                  "Image lights require nonnegative RGB samples");
        } else {
            descriptor.native_image =
                image_pyramid(filename.location, *source, encoding, wrap, 1, true);
            descriptor.native_image_source = source;
        }
    }
}

void Interpreter::shape_controls(const Descriptor &descriptor) {
    const auto *parameter = descriptor.effective_parameter("alpha").parameter;
    if (parameter && parameter->type == "float" && parameter->values.size() == 1) {
        const auto &value = parameter->values.front();
        if (!representable_float(value.number)) {
            error("invalid-parameter", value.location, "Shape alpha overflows binary32");
        }
    }
}

std::shared_ptr<const DecodedImage> Interpreter::decode_image(const std::filesystem::path &path,
                                                              const Location &source, bool gltf) {
    auto extension = path.extension().string();
    std::ranges::transform(extension, extension.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    const bool jpeg = one_of(extension, {".jpg", ".jpeg"});
    if (!jpeg && !one_of(extension, {".png", ".exr", ".hdr"})) {
        unsupported(source, "Only PNG, JPEG, EXR, and Radiance HDR image files are supported");
        return {};
    }
    if (asset_remaining_.files == 0) {
        error("limit-exceeded", source, "Image input file count limit exceeded");
        return {};
    }
    --asset_remaining_.files;
    std::error_code filesystem_error;
    const auto size = std::filesystem::file_size(path, filesystem_error);
    if (filesystem_error) {
        error("unreadable-file", source, "Cannot size image input: " + filesystem_error.message());
        return {};
    }
    if (size > limits_.file_bytes || size > std::vector<std::uint8_t>{}.max_size() ||
        size > static_cast<std::uintmax_t>(std::numeric_limits<std::streamsize>::max())) {
        error("limit-exceeded", source, "Image file byte limit exceeded");
        return {};
    }
    if (size > asset_remaining_.bytes) {
        error("limit-exceeded", source, "Expanded input byte limit exceeded");
        return {};
    }
    asset_remaining_.bytes -= static_cast<std::size_t>(size);
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        error("unreadable-file", source, "Cannot open image input");
        return {};
    }
    try {
        std::vector<std::uint8_t> data(static_cast<std::size_t>(size));
        if (!stream.read(reinterpret_cast<char *>(data.data()),
                         static_cast<std::streamsize>(size)) ||
            stream.peek() != std::char_traits<char>::eof() || stream.bad()) {
            error("unreadable-file", source, "image input changed or could not be read");
            return {};
        }
        auto decoded = jpeg                  ? decode_jpeg(data, image_limits_)
                       : extension == ".exr" ? decode_exr(data, image_limits_)
                       : extension == ".hdr" ? decode_hdr(data, image_limits_)
                                             : decode_png(data, image_limits_, gltf);
        if (!decoded.value) {
            issue(decoded.unsupported ? Severity::unsupported : Severity::error,
                  decoded.unsupported      ? "unsupported-feature"
                  : decoded.limit_exceeded ? "limit-exceeded"
                                           : "invalid-image",
                  source, "Image \"" + path.string() + "\": " + decoded.diagnostic);
            return {};
        }
        image_limits_.retained_bytes -= decoded_image_retained_bytes(*decoded.value);
        return std::make_shared<const DecodedImage>(std::move(*decoded.value));
    } catch (const std::bad_alloc &) {
        error("limit-exceeded", source, "Cannot allocate image input or decoded pixels");
        return {};
    }
}

std::shared_ptr<const ImagePyramid> Interpreter::image_pyramid(const Location &location,
                                                               const CheckedImageSource &source,
                                                               ImageEncoding encoding,
                                                               ImageWrap wrap, float gamma,
                                                               bool native, bool gltf) {
    if (!gltf)
        encoding = image_effective_encoding(*source.pixels, encoding);
    if (encoding != ImageEncoding::gamma) {
        gamma = 1;
    }
    const auto dependency_index =
        inventoried_paths_.at({DependencyKind::image, source.resolved_path});
    auto [cached, first_attempt] = image_pyramids_.try_emplace(
        ImageVariant{dependency_index, encoding, wrap, gamma, native, gltf});
    if (!first_attempt) {
        return cached->second;
    }
    try {
        auto pyramid =
            gltf     ? build_gltf_image_pyramid(*source.pixels, encoding, image_limits_)
            : native ? build_image_base(*source.pixels, encoding, wrap, image_limits_, gamma)
                     : build_image_pyramid(*source.pixels, encoding, wrap, image_limits_, gamma);
        if (!pyramid.value) {
            issue(pyramid.unsupported ? Severity::unsupported : Severity::error,
                  pyramid.unsupported      ? "unsupported-feature"
                  : pyramid.limit_exceeded ? "limit-exceeded"
                                           : "invalid-image",
                  location,
                  "Image \"" + source.resolved_path.string() + "\": " + pyramid.diagnostic);
            return {};
        }
        image_limits_.retained_bytes -=
            pyramid.value->texels.size() * sizeof(std::uint32_t) +
            pyramid.value->levels.size() * sizeof(ImageMipLevel) +
            (pyramid.value->custom_transfer ? sizeof(*pyramid.value->custom_transfer) : 0);
        cached->second = std::make_shared<const ImagePyramid>(std::move(*pyramid.value));
        return cached->second;
    } catch (const std::bad_alloc &) {
        error("limit-exceeded", location, "Cannot allocate image data");
        return {};
    }
}

namespace {
struct PresetDefinition {
    std::string_view name;
    std::array<float, 3> scattering, absorption;
};
constexpr PresetDefinition presets[]{
#include "../third_party/pbrt/medium_presets.inc"
};
} // namespace

void Interpreter::medium(const Directive &directive) {
    if (directive.name == "MediumInterface") {
        for (const auto &medium : directive.arguments) {
            if (!medium.text.empty()) {
                medium_uses_.push_back(&medium);
            }
        }
        state_.media = {directive.arguments.front().text, directive.arguments.back().text,
                        directive.location};
        return;
    }

    const auto &name = directive.arguments.front();
    if (name.text.empty() || !media_.insert(name.text).second) {
        error("duplicate-definition", name.location,
              "Empty or duplicate medium definition \"" + name.text + "\"");
    }
    const Field field{"type", "string", 1, false, true};
    const auto *parameter = select(directive, "medium", field);
    if (!parameter) {
        error("missing-parameter", directive.location, "Named medium requires \"string type\"");
        return;
    }
    if (!field_valid(field, *parameter)) {
        return;
    }
    const string_view type = parameter->values.front().text;
    const bool known = one_of(type, {"homogeneous", "uniformgrid", "rgbgrid", "cloud", "nanovdb"});
    if (type != "homogeneous") {
        dependencies(directive, "medium", type);
        deferred_type(directive, type, known, "medium");
        return;
    }
    auto descriptor = describe(directive);
    std::vector schema{Field{"type", "string", 1, false, true},
                       Field{"preset", "string"},
                       Field{.name = "sigma_a", .type = "spectral", .allow_spectrum_file = true},
                       Field{.name = "sigma_s", .type = "spectral", .allow_spectrum_file = true},
                       Field{.name = "Le", .type = "spectral", .allow_spectrum_file = true},
                       Field{"scale", "float"},
                       Field{"Lescale", "float"},
                       Field{"g", "float"}};
    const auto *preset = select(directive, "medium", Field{"preset", "string"});
    if (preset && field_valid(Field{"preset", "string"}, *preset) &&
        std::ranges::find(presets, preset->values.front().text, &PresetDefinition::name) !=
            std::end(presets)) {
        if (std::ranges::any_of(directive.parameters, [](const Parameter &p) {
                return p.name == "sigma_a" || p.name == "sigma_s";
            })) {
            error("unused-parameter", preset->location,
                  "A recognized medium preset conflicts with explicit coefficients");
            return;
        }
        // Inherited defaults are allowed to be unused by pinned PBRT. Do not
        // decode coefficient files shadowed by the measured preset.
        std::erase_if(schema,
                      [](const Field &f) { return f.name == "sigma_a" || f.name == "sigma_s"; });
    }
    validate(descriptor, directive, "medium", schema);
    scene_.media.push_back(std::move(descriptor));
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
    } else if (name == "Texture") {
        texture(directive);
    } else if (one_of(name, {"MakeNamedMedium", "MediumInterface"})) {
        medium(directive);
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

void Interpreter::order_textures() {
    using Kind = TextureDefinitionRef::Kind;
    using Key = std::pair<bool, string_view>;
    struct Definition {
        TextureDefinitionRef ref;
        enum class State { unvisited, visiting, done, failed };
        State state = State::unvisited;
        std::size_t depth = 1;
        std::size_t work = 1;
    };
    std::map<Key, Definition> definitions;
    const auto add = [&](Kind kind, std::size_t count, bool spectrum) {
        for (std::size_t i = 0; i < count; ++i) {
            const TextureDefinitionRef ref{kind, i};
            const auto &arguments = scene_.texture_descriptor(ref).authored.arguments;
            definitions.emplace(Key{spectrum, arguments.front().text}, Definition{ref});
        }
    };
    add(Kind::float_constant, scene_.float_constant_textures.size(), false);
    add(Kind::spectrum_constant, scene_.spectrum_constant_textures.size(), true);
    add(Kind::float_image, scene_.float_image_textures.size(), false);
    add(Kind::spectrum_image, scene_.spectrum_image_textures.size(), true);
    for (std::size_t i = 0; i < scene_.procedural_textures.size(); ++i) {
        const auto &arguments = scene_.procedural_textures[i].authored.arguments;
        definitions.emplace(Key{arguments[1].text == "spectrum", arguments.front().text},
                            Definition{{Kind::procedural, i}});
    }

    using State = Definition::State;
    const auto visit = [&](auto &&self, const Key &key, Definition &definition,
                           const Location &source, std::size_t active_depth) -> bool {
        if (definition.state == State::visiting) {
            error("cyclic-texture", source,
                  "Cyclic texture reference to \"" + std::string(key.second) + "\"");
            return false;
        }
        if (definition.state == State::done || definition.state == State::failed) {
            return definition.state == State::done;
        }
        if (active_depth > texture_graph_wire::max_depth) {
            error("limit-exceeded", source, "Texture graph exceeds maximum depth");
            definition.state = State::failed;
            return false;
        }
        definition.state = State::visiting;
        const auto &descriptor = scene_.texture_descriptor(definition.ref);
        if (definition.ref.kind == Kind::procedural) {
            const auto &type = descriptor.authored.arguments.back().text;
            const auto child = [&](string_view name, bool spectrum) {
                const auto *operand = descriptor.effective_parameter(name).parameter;
                if (!operand || operand->type != "texture" || operand->values.size() != 1) {
                    ++definition.work; // Inline or default constant leaf.
                    definition.depth = std::max<std::size_t>(definition.depth, 2);
                    return true;
                }
                const auto &reference = operand->values.front();
                const Key target{spectrum, reference.text};
                const auto found = definitions.find(target);
                if (found == definitions.end() ||
                    !self(self, target, found->second, reference.location, active_depth + 1)) {
                    return false;
                }
                definition.work += found->second.work;
                definition.depth = std::max(definition.depth, found->second.depth + 1);
                return true;
            };
            bool valid = true;
            if (type == "scale") {
                valid = child("tex", key.first) && child("scale", false);
            } else if (one_of(type, {"mix", "directionmix", "checkerboard"})) {
                valid = child("tex1", key.first) && child("tex2", key.first);
                if (type == "mix") {
                    valid = child("amount", false) && valid;
                }
            } else if (type == "dots") {
                // Dots chooses exactly one branch. Both references still
                // participate in depth and cycle checks.
                valid = child("inside", key.first);
                const auto inside_work = definition.work;
                definition.work = 1;
                valid = child("outside", key.first) && valid;
                definition.work = std::max(definition.work, inside_work);
            } else if (type == "bilerp") {
                valid = child("v00", key.first) && child("v01", key.first) &&
                        child("v10", key.first) && child("v11", key.first);
            }
            if (!valid) {
                definition.state = State::failed;
                return false;
            }
        }
        if (definition.depth > texture_graph_wire::max_depth ||
            definition.work > texture_graph_wire::max_visits) {
            error("limit-exceeded", descriptor.authored.location,
                  "Texture graph exceeds maximum depth or evaluation work");
            definition.state = State::failed;
            return false;
        }
        definition.state = State::done;
        scene_.texture_order.push_back(definition.ref);
        return true;
    };
    for (auto &[key, definition] : definitions) {
        if (result_.document.diagnostics_truncated) {
            return;
        }
        visit(visit, key, definition, scene_.texture_descriptor(definition.ref).authored.location,
              1);
    }
}

bool Interpreter::binary_image(const ImagePyramid &image) {
    auto [cached, first] = binary_pyramids_.try_emplace(&image, false);
    if (!first) {
        return cached->second;
    }
    const auto &base = image.levels.front();
    for (std::uint32_t y = 0; y < base.height; ++y) {
        for (std::uint32_t x = 0; x < base.width; ++x) {
            const auto value = image_linear_scalar(
                image, (static_cast<float>(x) + 0.5f) / static_cast<float>(base.width),
                (static_cast<float>(y) + 0.5f) / static_cast<float>(base.height), 0, 0, 0, 0,
                ImageFilter::point);
            if (!value || (*value != 0 && *value != 1)) {
                return false;
            }
        }
    }
    cached->second = true;
    return true; // Black wrapping adds only zero-valued border samples.
}

void Interpreter::emitter_masks() {
    if (std::ranges::none_of(scene_.shapes,
                             [](const Shape &shape) { return !!shape.area_light; })) {
        return;
    }
    struct Mask {
        std::optional<float> literal;
        SpectrumImageTexture *image = nullptr;
    };
    std::map<string_view, Mask> masks;
    const auto resolve = [&](const Descriptor &descriptor, string_view name, float fallback) {
        const auto *parameter = descriptor.effective_parameter(name).parameter;
        if (!parameter) {
            return Mask{fallback};
        }
        if (parameter->values.size() != 1) {
            return Mask{};
        }
        const auto &value = parameter->values.front();
        if (parameter->type == "texture") {
            const auto found = masks.find(value.text);
            return found == masks.end() ? Mask{} : found->second;
        }
        return representable_float(value.number) ? Mask{static_cast<float>(value.number)} : Mask{};
    };
    using Kind = TextureDefinitionRef::Kind;
    for (const auto ref : scene_.texture_order) {
        const auto &descriptor = scene_.texture_descriptor(ref);
        const auto &arguments = descriptor.authored.arguments;
        if (arguments[1].text != "float") {
            continue;
        }
        Mask mask;
        if (ref.kind == Kind::float_constant) {
            mask = resolve(descriptor, "value", 1);
        } else if (ref.kind == Kind::float_image) {
            mask.image = &scene_.float_image_textures[ref.index];
        } else if (arguments.back().text == "scale") {
            const auto texture = resolve(descriptor, "tex", 1);
            const auto scale = resolve(descriptor, "scale", 1);
            // Match PBRT's identity scale folding without certifying arbitrary arithmetic.
            if (scale.literal == 1) {
                mask = texture;
            } else if (texture.literal == 1) {
                mask = scale;
            }
        }
        masks.emplace(arguments.front().text, mask);
    }
    for (const auto &shape : scene_.shapes) {
        if (!shape.area_light || result_.document.diagnostics_truncated) {
            continue;
        }
        const auto *alpha = shape.descriptor.effective_parameter("alpha").parameter;
        if (!alpha || alpha->values.size() != 1) {
            continue;
        }
        const auto &value = alpha->values.front();
        if (alpha->type == "texture" &&
            (value.text.empty() || !textures_[0].contains(value.text))) {
            continue; // An empty value is inert. The typed reference check reports a missing name.
        }
        const auto mask = resolve(shape.descriptor, "alpha", 1);
        if (mask.literal == 0 || mask.literal == 1) {
            continue;
        }
        if (auto *image = mask.image;
            image && image->descriptor.texture_mapping.kind == texture_graph_wire::uv &&
            image->texture.filter == ImageFilter::point && image->texture.scale == 1 &&
            binary_image(*image->texture.image)) {
            image->binary_base = true;
            continue;
        }
        unsupported(alpha->location,
                    "Area emitter alpha requires a binary constant or direct UV point imagemap");
    }
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
    std::map<string_view, const Descriptor *> material_definitions;
    for (const auto &material : scene_.named_materials)
        material_definitions.emplace(material.authored.arguments.front().text, &material);
    std::map<string_view, unsigned> material_state;
    const auto visit_material = [&](auto &&self, string_view name, std::size_t depth) -> void {
        if (material_state[name] == 2 || !material_definitions.contains(name))
            return;
        const auto &descriptor = *material_definitions.at(name);
        if (material_state[name] == 1) {
            error("cyclic-reference", descriptor.authored.location,
                  "Cyclic mix material reference");
            return;
        }
        if (depth > limits_.scopes) {
            error("limit-exceeded", descriptor.authored.location,
                  "Material reference depth limit exceeded");
            return;
        }
        material_state[name] = 1;
        if (const auto *materials = descriptor.effective_parameter("materials").parameter)
            for (const auto &value : materials->values)
                self(self, value.text, depth + 1);
        material_state[name] = 2;
    };
    for (const auto &[name, descriptor] : material_definitions)
        visit_material(visit_material, name, 0);
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
        if (parent_stream_complete) {
            order_textures();
            emitter_masks();
        }
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

const CheckedSpectrumSource *Descriptor::checked_spectrum(std::string_view name) const {
    const auto found =
        std::ranges::find(spectrum_files, name, &decltype(spectrum_files)::value_type::first);
    if (found != spectrum_files.end()) {
        return found->second.get();
    }
    return name == spectrum_file_parameter ? spectrum_file.get() : nullptr;
}

const Descriptor &Scene::texture_descriptor(TextureDefinitionRef ref) const {
    using Kind = TextureDefinitionRef::Kind;
    switch (ref.kind) {
    case Kind::float_constant:
        return float_constant_textures.at(ref.index);
    case Kind::spectrum_constant:
        return spectrum_constant_textures.at(ref.index);
    case Kind::float_image:
        return float_image_textures.at(ref.index).descriptor;
    case Kind::spectrum_image:
        return spectrum_image_textures.at(ref.index).descriptor;
    case Kind::procedural:
        return procedural_textures.at(ref.index);
    }
    std::terminate();
}

Result interpret(Document document, const Limits &limits) {
    return Interpreter(std::move(document), limits).run();
}

} // namespace rypath::pbrt
