// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "geometry.hpp"
#include "image_texture.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rypath::pbrt {

struct Location {
    static const std::shared_ptr<const std::string> &empty_file() {
        static const auto empty = std::make_shared<const std::string>();
        return empty;
    }
    std::shared_ptr<const std::string> file = empty_file();
    std::size_t line = 1;
    std::size_t column = 1;

    Location() = default;
    Location(std::string path, std::size_t source_line = 1, std::size_t source_column = 1)
        : file(std::make_shared<const std::string>(std::move(path))), line(source_line),
          column(source_column) {}
    Location(std::shared_ptr<const std::string> path, std::size_t source_line,
             std::size_t source_column)
        : file(std::move(path)), line(source_line), column(source_column) {}
};
enum class Severity { error, unsupported, warning };
struct Diagnostic {
    Severity severity = Severity::error;
    std::string code;
    Location location;
    std::string message;
};
struct Limits {
    std::size_t file_bytes = 16 * 1024 * 1024;
    std::size_t expanded_bytes = 64 * 1024 * 1024;
    std::size_t active_files = 32;
    std::size_t expansions = 256;
    std::size_t scopes = 256;
    std::size_t token_bytes = 64 * 1024;
    std::size_t list_values = 1'000'000;
    std::size_t total_values = 2'000'000;
    std::size_t directives = 100'000;
    std::size_t diagnostics = 100;
    ImageLimits image{};
};
enum class ValueKind { number, integer, boolean, string };
struct Value {
    ValueKind kind = ValueKind::number;
    Location location;
    std::string text; // Original numeric lexeme or decoded quoted text.
    double number = 0;
    std::int32_t integer = 0;
    bool boolean = false;
};
struct Parameter {
    Location location;
    std::string type;
    std::string name;
    std::vector<Value> values;
};
struct Directive {
    Location location;
    std::string name;
    std::vector<Value> arguments;
    std::vector<Parameter> parameters;
};
enum class DependencyKind { include, import, mesh, image, spectrum, scene };
struct Dependency {
    Location location;
    DependencyKind kind = DependencyKind::include;
    std::string authored_path;
    std::filesystem::path resolved_path;
    bool exists = false;
    bool visited = false;
};
struct Document {
    std::filesystem::path entry_directory;
    std::vector<Directive> directives;
    std::vector<Dependency> dependencies;
    std::vector<Diagnostic> diagnostics;
    bool syntax_complete = true;
    bool dependencies_complete = true;
    bool diagnostics_truncated = false;
    std::size_t expanded_bytes = 0;
    std::size_t expansion_count = 0;
    std::size_t value_count = 0;
};

// Row-major binary64 matrix; authored values remain in Document separately.
struct Transform {
    std::array<double, 16> values{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
};
struct Descriptor {
    Directive authored;
    Transform transform;
    std::string color_space = "srgb";
    struct InheritedParameter {
        std::shared_ptr<const Parameter> parameter;
        std::string color_space;
    };
    std::vector<InheritedParameter> inherited;
    std::vector<std::size_t> effective_authored;

    struct EffectiveParameter {
        const Parameter *parameter = nullptr;
        std::string_view color_space;
    };
    EffectiveParameter effective_parameter(std::string_view name) const;
};
struct MaterialBinding {
    std::shared_ptr<const std::string> name = Location::empty_file();
    std::shared_ptr<const Descriptor> inline_material;
    Location location;
};
struct Shape {
    Descriptor descriptor;
    MaterialBinding material;
    bool reverse_orientation = false;
    std::uint32_t mesh_index = 0;
    std::shared_ptr<const Descriptor> area_light;
};
struct Object {
    std::string name;
    Location location;
    std::vector<Shape> shapes;
};
struct Placement {
    std::string object;
    Transform transform;
    Location location;
};
struct CheckedImageSource {
    std::filesystem::path resolved_path;
    std::shared_ptr<const DecodedImage> pixels;
};
struct SpectrumImageTexture {
    Descriptor descriptor;
    std::shared_ptr<const CheckedImageSource> source;
    ImageTexture texture;
};
struct Scene {
    std::vector<Descriptor> options;
    std::vector<Descriptor> named_materials;
    std::vector<std::shared_ptr<const Descriptor>> inline_materials;
    std::vector<Descriptor> float_constant_textures;
    std::vector<Descriptor> spectrum_constant_textures;
    std::vector<SpectrumImageTexture> spectrum_image_textures;
    std::vector<Descriptor> lights;
    std::vector<std::shared_ptr<const Descriptor>> area_lights;
    std::vector<Shape> shapes;
    std::vector<Object> objects;
    std::vector<Placement> placements;
    GeometryData geometry;
};
struct Inventory {
    std::size_t named_materials = 0;
    std::size_t meshes = 0;
    std::size_t triangles = 0;
    std::size_t instances = 0;
    std::size_t patches = 0;
};
struct Result {
    Document document;
    std::optional<Scene> scene; // Present only when checked structure is complete.
    Inventory inventory;
    bool structure_complete = false;
    int exit_code() const;
};

Document parse_file(const std::filesystem::path &entry, const Limits &limits = {});
Result interpret(Document document, const Limits &limits = {});
Result check(const std::filesystem::path &entry, const Limits &limits = {});
std::string report(const Result &result);
std::string format_diagnostic(const Diagnostic &diagnostic);
// Central bounded diagnostic insertion, shared by parsing and interpretation.
void diagnose(Document &document, const Limits &limits, Severity severity, std::string code,
              const Location &location, std::string message);

} // namespace rypath::pbrt
