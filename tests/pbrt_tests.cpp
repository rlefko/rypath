// SPDX-License-Identifier: GPL-3.0-only

#include "pbrt.hpp"
#include "pbrt_numeric.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

namespace fs = std::filesystem;
using namespace rypath::pbrt;

constexpr std::string_view triangle = R"(
Shape "trianglemesh" "point3 P" [0 0 0 1 0 0 0 1 0] "integer indices" [0 1 2]
)";
constexpr std::string_view scene_start = "Camera \"perspective\"\nWorldBegin\n";

void require(bool condition, std::string_view reason) {
    if (!condition) {
        throw std::runtime_error(std::string{reason});
    }
}

fs::path write_file(const fs::path &directory, const fs::path &name, std::string_view contents) {
    const auto path = directory / name;
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output << contents;
    output.close();
    require(output.good(), "could not write " + path.string());
    return path;
}

const Diagnostic &find_diagnostic(const Document &document, std::string_view code) {
    const auto found = std::ranges::find(document.diagnostics, code, &Diagnostic::code);
    require(found != document.diagnostics.end(), "expected diagnostic " + std::string{code});
    return *found;
}

void require_clean(const Document &document) {
    if (!document.diagnostics.empty()) {
        const auto &diagnostic = document.diagnostics.front();
        throw std::runtime_error("unexpected " + diagnostic.code + " at " +
                                 *diagnostic.location.file + ": " + diagnostic.message);
    }
}

const Scene &checked_scene(const Result &result) {
    if (result.exit_code() != 0 || !result.structure_complete || !result.scene) {
        throw std::runtime_error(report(result));
    }
    return *result.scene;
}

void syntax_tests(const fs::path &directory) {
    const auto path = write_file(directory, "syntax.pbrt",
                                 "# CRLF and comment\r\n  Camera \"perspective\"\r\n"
                                 "WorldBegin\n"
                                 R"(Material "diffuse" "string data" "a#b\n\t\"\\é"
  "bool enabled" [true false] "float test" [+.5 1. -2e+1 -0 1e-309 5e-324]
  "integer range" [-2147483648 +2147483647] "spectrum data" [410 .25 680 .5]
  "float override" 1 "float override" 2 # both authored values survive
)");
    const auto document = parse_file(path);
    require_clean(document);
    require(document.syntax_complete && document.directives.size() == 3, "syntax inventory");
    const auto &camera = document.directives.at(0);
    require(*camera.location.file == path.string() && camera.location.line == 2 &&
                camera.location.column == 3,
            "directive location after CRLF");
    const auto &parameters = document.directives.at(2).parameters;
    require(parameters.size() == 7, "typed parameter count");
    const auto &escaped = parameters.at(0).values.at(0);
    require(escaped.kind == ValueKind::string && escaped.text == "a#b\n\t\"\\é",
            "quoted escapes, UTF-8 and literal comment marker");
    const auto &booleans = parameters.at(1).values;
    require(booleans.at(0).kind == ValueKind::boolean && booleans.at(0).boolean &&
                !booleans.at(1).boolean,
            "bare bools");
    const auto &numbers = parameters.at(2).values;
    require(numbers.at(0).number == 0.5 && numbers.at(0).text == "+.5" &&
                numbers.at(1).number == 1 && numbers.at(2).number == -20 &&
                std::signbit(numbers.at(3).number) &&
                numbers.at(4).number > std::numeric_limits<double>::denorm_min() &&
                numbers.at(4).number < std::numeric_limits<double>::min() &&
                numbers.at(5).number == std::numeric_limits<double>::denorm_min(),
            "locale-independent decimal notation and preserved lexeme");
    const auto &integers = parameters.at(3).values;
    require(integers.at(0).kind == ValueKind::integer &&
                integers.at(0).integer == std::numeric_limits<std::int32_t>::min() &&
                integers.at(1).integer == std::numeric_limits<std::int32_t>::max(),
            "signed int32 boundaries");
    require(parameters.at(4).values.at(2).number == 680, "arbitrary sampled spectrum retained");
    require(parameters.at(5).values.at(0).number == 1 && parameters.at(6).values.at(0).number == 2,
            "authored duplicate parameter order");

    const auto option_path = write_file(directory, "options.pbrt", R"(Option "Se-ed" -2147483648
Option "displacementEdgeScale" 2147483648
Option "mseReferenceImage" "input.exr"
Option "bool disablepixeljitter" true
)");
    const auto options = parse_file(option_path);
    require_clean(options);
    require(options.syntax_complete && options.directives.size() == 4, "Option syntax inventory");
    const auto option = [&](std::size_t index, std::string_view name,
                            std::string_view type) -> const Value & {
        const auto &directive = options.directives.at(index);
        const auto &parameter = directive.parameters.at(0);
        require(directive.name == "Option" && directive.arguments.empty() &&
                    directive.parameters.size() == 1 && parameter.name == name &&
                    parameter.type == type && parameter.values.size() == 1,
                "Option retains one typed parameter");
        return parameter.values.at(0);
    };
    require(option(0, "Se-ed", "integer").integer == std::numeric_limits<std::int32_t>::min() &&
                option(1, "displacementEdgeScale", "float").number == 2147483648.0 &&
                option(2, "mseReferenceImage", "string").text == "input.exr" &&
                option(3, "disablepixeljitter", "bool").boolean,
            "raw named options and typed option preserve their values");

    struct Invalid {
        std::string_view text;
        std::string_view code;
        Severity severity = Severity::error;
    };
    constexpr std::array bad{
        Invalid{"Mystery", "unknown-directive"},
        Invalid{"WorldEnd", "legacy-directive", Severity::unsupported},
        Invalid{R"(Camera "bad\q")", "invalid-escape"},
        Invalid{"Camera \"bad\nstring\"", "invalid-string"},
        Invalid{"Translate 1e- 0 0", "invalid-number"},
        Invalid{"Translate 1e9999 0 0", "invalid-number"},
        Invalid{"Translate 2e-324 0 0", "invalid-number"},
        Invalid{"Translate 0x1p2 0 0", "unsupported-number", Severity::unsupported},
        Invalid{R"(Film "rgb" "integer xresolution" 2147483648)", "invalid-integer"},
        Invalid{R"(Film "rgb" "integer xresolution" 1.0)", "invalid-integer"},
        Invalid{R"(Material "diffuse" "bool flag" "true")", "invalid-value"},
        Invalid{R"(Material "diffuse" "rgb reflectance" [1 1])", "invalid-list"},
        Invalid{R"(Material "diffuse" "float value" [])", "invalid-list"},
        Invalid{R"(Material "diffuse" "float value" [[1]])", "invalid-list"},
        Invalid{R"(Material "diffuse" "matrix thing" 1)", "invalid-parameter"},
    };
    for (const auto &sample : bad) {
        const auto invalid_path =
            write_file(directory, "invalid.pbrt", "\n  " + std::string{sample.text});
        const auto invalid = parse_file(invalid_path);
        const auto &diagnostic = find_diagnostic(invalid, sample.code);
        require(diagnostic.severity == sample.severity && diagnostic.location.line == 2 &&
                    diagnostic.location.column >= 3 &&
                    *diagnostic.location.file == invalid_path.string(),
                "source-located syntax failure for " + std::string{sample.text});
    }
    const auto bad_encoding =
        parse_file(write_file(directory, "invalid-encoding.pbrt", std::string_view{"#\0", 2}));
    require(find_diagnostic(bad_encoding, "invalid-encoding").location.line == 1,
            "NUL in a comment is diagnosed");
    const auto unknown = parse_file(write_file(directory, "unknown.pbrt", "  UnknownDirective"));
    require(find_diagnostic(unknown, "unknown-directive").location.column == 3,
            "unknown directive starts at exact column");
}

void state_tests(const fs::path &directory) {
    const auto path = write_file(directory, "state.pbrt",
                                 R"(LookAt 1 2 3 2 2 3 0 1 0
Camera "perspective"
WorldBegin
ObjectInstance "pair"
AttributeBegin
    Translate 5 0 0
    ObjectInstance "pair"
AttributeEnd
AttributeBegin
    ColorSpace "rec2020"
    MakeNamedMaterial "mat" "string type" "diffuse"
        "spectrum reflectance" [400 -.1 515 1.2 700 .3]
    NamedMaterial "mat"
    Translate 2 0 0
    Scale -2 3 1
    ReverseOrientation
)" + std::string{triangle} + R"(
    AttributeBegin
        Identity
        ColorSpace "srgb"
        ReverseOrientation
        Material "diffuse" "rgb reflectance" [1 0 0]
)" + std::string{triangle} + R"(
    AttributeEnd
)" + std::string{triangle} + R"(
AttributeEnd
NamedMaterial "mat"
)" + std::string{triangle} + R"(
Identity
ObjectBegin "pair"
    Translate 0 3 0
    ConcatTransform [1 0 0 0 0 2 0 0 0 0 1 0 0 1 0 1]
)" + std::string{triangle} + R"(
ObjectEnd
)");
    const auto result = check(path);
    const auto &scene = checked_scene(result);
    const auto camera = std::ranges::find_if(
        scene.options, [](const Descriptor &option) { return option.authored.name == "Camera"; });
    require(camera != scene.options.end(), "camera retained");
    require(camera->transform.values ==
                std::array<double, 16>{0, 0, 1, 1, 0, 1, 0, 2, -1, 0, 0, 3, 0, 0, 0, 1},
            "LookAt camera-to-world convention");
    require(scene.shapes.size() == 4 && scene.named_materials.size() == 1,
            "scopes preserve shapes and definitions are visible after scope exit");
    const auto &outer = scene.shapes.at(0);
    const auto &inner = scene.shapes.at(1);
    const auto &restored = scene.shapes.at(2);
    const auto &root = scene.shapes.at(3);
    require(outer.descriptor.transform.values ==
                std::array<double, 16>{-2, 0, 0, 2, 0, 3, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1},
            "translate then scale uses PBRT postmultiplication");
    require(outer.reverse_orientation && !inner.reverse_orientation &&
                restored.reverse_orientation && !root.reverse_orientation,
            "nested reverse orientation and restoration");
    require(*outer.material.name == "mat" && inner.material.inline_material &&
                *restored.material.name == "mat" && *root.material.name == "mat",
            "material binding scopes");
    require(outer.descriptor.color_space == "rec2020" && inner.descriptor.color_space == "srgb" &&
                restored.descriptor.color_space == "rec2020" &&
                root.descriptor.color_space == "srgb" &&
                root.descriptor.transform.values == Transform{}.values,
            "nested color and transform scopes, WorldBegin identity");
    const auto &spectrum = scene.named_materials.at(0).authored.parameters.at(1);
    require(spectrum.type == "spectrum" && spectrum.values.size() == 6 &&
                spectrum.values.at(1).number == -0.1 && spectrum.values.at(2).number == 515 &&
                spectrum.values.at(3).number == 1.2,
            "canonical material retains arbitrary sampled spectrum");
    require(scene.objects.size() == 1 && scene.objects.at(0).shapes.size() == 1 &&
                scene.placements.size() == 2 && scene.geometry.meshes.size() == 5 &&
                scene.geometry.instances.size() == 6,
            "forward object placements share one mesh");
    const auto &first = scene.geometry.instances.at(4);
    const auto &second = scene.geometry.instances.at(5);
    require(
        first.mesh_index == second.mesh_index &&
            first.transform.values == std::array<float, 12>{1, 0, 0, 0, 0, 2, 0, 4, 0, 0, 1, 0} &&
            second.transform.values == std::array<float, 12>{1, 0, 0, 5, 0, 2, 0, 4, 0, 0, 1, 0},
        "instance transform composed with member transform");

    const auto oblique_camera =
        check(write_file(directory, "oblique-camera.pbrt", R"(LookAt 1 2 3 4 6 8 0 1 0
Camera "perspective"
WorldBegin
CoordSysTransform "camera"
)" + std::string{triangle}));
    const auto &oblique_scene = checked_scene(oblique_camera);
    const auto &view = oblique_scene.options.at(0).transform.values;
    require(view == oblique_scene.shapes.at(0).descriptor.transform.values,
            "named camera coordinate system matches the captured camera");
    require(std::abs(view.at(3) - 1) < 1e-12 && std::abs(view.at(7) - 2) < 1e-12 &&
                std::abs(view.at(11) - 3) < 1e-12,
            "oblique camera origin matches LookAt eye");
    const auto projection = [&](std::size_t axis) {
        return 3 * view.at(axis) + 4 * view.at(4 + axis) + 5 * view.at(8 + axis);
    };
    require(std::abs(projection(0)) < 1e-12 && std::abs(projection(1)) < 1e-12 &&
                std::abs(projection(2) - std::sqrt(50.0)) < 1e-12,
            "target lies on camera positive Z");
    require(std::abs(view.at(0) - 5 / std::sqrt(34.0)) < 1e-12 &&
                std::abs(view.at(8) + 3 / std::sqrt(34.0)) < 1e-12 &&
                std::abs(view.at(4)) < 1e-12 && view.at(5) > 0,
            "camera right is horizontal and up agrees with authored world up");

    const auto forward_material =
        check(write_file(directory, "forward-material.pbrt", std::string{scene_start} + R"(
NamedMaterial "later"
)" + std::string{triangle} + R"(
MakeNamedMaterial "later" "string type" "diffuse"
)"));
    require(*checked_scene(forward_material).shapes.at(0).material.name == "later",
            "forward material reference");

    const auto last_parameter =
        check(write_file(directory, "last-parameter.pbrt", std::string{scene_start} + R"(
Shape "trianglemesh" "point3 P" [0 0 0 1 0 0 0 1 0]
    "integer indices" [0 1 99] "integer indices" [2 0 1]
)"));
    require(checked_scene(last_parameter).geometry.meshes.at(0).triangles.at(0) ==
                std::array<std::uint32_t, 3>{2, 0, 1},
            "geometry selects the last authored parameter");

    struct Invalid {
        std::string_view text;
        std::string_view code;
    };
    constexpr std::array bad{
        Invalid{"AttributeEnd", "mismatched-scope"},
        Invalid{"AttributeBegin\nObjectEnd", "mismatched-scope"},
        Invalid{"AttributeBegin", "mismatched-scope"},
        Invalid{R"(NamedMaterial "absent")", "undefined-reference"},
        Invalid{R"(ObjectInstance "absent")", "undefined-reference"},
        Invalid{R"(MakeNamedMaterial "m" "string type" "diffuse"
MakeNamedMaterial "m" "string type" "diffuse")",
                "duplicate-definition"},
        Invalid{R"(ObjectBegin "o"
ObjectEnd
ObjectBegin "o"
ObjectEnd)",
                "duplicate-definition"},
        Invalid{R"(Texture "t" "unknown" "constant")", "unknown-type"},
        Invalid{R"(Texture "t" "spectrum" "unknown-plugin")", "unknown-type"},
        Invalid{R"(Texture "t" "spectrum" "constant"
Texture "t" "spectrum" "constant")",
                "duplicate-definition"},
        Invalid{R"(Material "diffuse" "texture reflectance" "absent")", "undefined-reference"},
        Invalid{R"(MakeNamedMedium "m" "string type" "homogeneous"
MakeNamedMedium "m" "string type" "homogeneous")",
                "duplicate-definition"},
        Invalid{R"(MediumInterface "absent" "")", "undefined-reference"},
    };
    for (const auto &sample : bad) {
        const auto invalid = check(write_file(directory, "invalid-state.pbrt",
                                              std::string{scene_start} + std::string{sample.text}));
        require(invalid.exit_code() == 1 && !invalid.scene, report(invalid));
        require(find_diagnostic(invalid.document, sample.code).severity == Severity::error,
                "state error severity");
    }
    const auto deferred_texture =
        check(write_file(directory, "deferred-texture.pbrt",
                         std::string{scene_start} + R"(Texture "t" "spectrum" "checkerboard"
Material "diffuse" "texture reflectance" "t"
)"));
    require(deferred_texture.exit_code() == 2 && !deferred_texture.scene,
            "recognized texture implementation explicitly outside the profile");
    find_diagnostic(deferred_texture.document, "unsupported-feature");
}

void constant_texture_tests(const fs::path &directory) {
    const auto entry = write_file(directory, "constant-entry.pbrt", R"(WorldBegin
MakeNamedMaterial "m" "string type" "diffuse" "texture reflectance" "shared"
NamedMaterial "m"
AttributeBegin
ColorSpace "rec2020"
Attribute "texture" "rgb value" [.1 .2 .3] "float value" -2
ColorSpace "srgb"
Include "constant-definitions.pbrt"
AttributeEnd
Material "diffuse" "texture reflectance" "nested"
Texture "default-float" "float" "constant"
Texture "default-spectrum" "spectrum" "constant"
Texture "sampled" "spectrum" "constant" "spectrum value" [410 -.1 680 1.2]
Material "diffuse"
)");
    const auto included = write_file(directory, "constant-definitions.pbrt", R"(
Texture "shared" "float" "constant"
Texture "shared" "spectrum" "constant"
Texture "nested" "spectrum" "constant" "rgb value" [.9 .9 .9] "spectrum value" [410 .2 680 .4]
)");
    Scene scene;
    {
        const auto result = check(entry);
        scene = checked_scene(result);
    }
    const auto &floats = scene.float_constant_textures;
    const auto &spectra = scene.spectrum_constant_textures;
    require(floats.size() == 2 && spectra.size() == 4,
            "separate typed namespaces retain every definition including unused ones");
    require(floats.at(0).authored.arguments.at(0).text == "shared" &&
                spectra.at(0).authored.arguments.at(0).text == "shared" &&
                *spectra.at(0).authored.location.file == included.string(),
            "same-name typed descriptors own their Include source");
    const auto scalar = floats.at(0).effective_parameter("value");
    require(scalar.parameter && scalar.parameter->values.at(0).number == -2 &&
                *scalar.parameter->location.file == entry.string() &&
                scalar.parameter->location.line == 6,
            "float value inherits its exact type and source location");
    const auto inherited = spectra.at(0).effective_parameter("value");
    require(inherited.parameter && inherited.parameter->type == "rgb" &&
                inherited.parameter->values.at(1).number == .2 &&
                inherited.color_space == "rec2020" &&
                *inherited.parameter->location.file == entry.string() &&
                spectra.at(0).color_space == "srgb",
            "inherited spectrum retains its original color space and source");
    const auto explicit_value = spectra.at(1).effective_parameter("value");
    require(explicit_value.parameter && explicit_value.parameter->type == "spectrum" &&
                explicit_value.color_space == "srgb" &&
                *explicit_value.parameter->location.file == included.string(),
            "last compatible explicit spectrum wins over earlier types and inherited defaults");
    require(!floats.at(1).effective_parameter("value").parameter &&
                !spectra.at(2).effective_parameter("value").parameter &&
                !scene.inline_materials.at(1)->effective_parameter("reflectance").parameter,
            "omitted texture values and diffuse reflectance remain absent in preflight");
    const auto sampled = spectra.at(3).effective_parameter("value");
    require(sampled.parameter && sampled.parameter->values.at(1).number == -.1 &&
                sampled.parameter->values.at(3).number == 1.2,
            "sampled-spectrum reflectance limits remain the translator's responsibility");
    const auto forward = scene.named_materials.at(0).effective_parameter("reflectance");
    const auto global = scene.inline_materials.at(0)->effective_parameter("reflectance");
    require(forward.parameter && forward.parameter->values.at(0).text == "shared" &&
                global.parameter && global.parameter->values.at(0).text == "nested",
            "named and inline material references survive forward lookup and scope restoration");

    struct Invalid {
        std::string_view text;
        std::string_view code;
    };
    constexpr std::array invalid{
        Invalid{R"(Texture "" "spectrum" "constant")", "duplicate-definition"},
        Invalid{R"(Texture "t" "float" "constant"
Texture "t" "float" "imagemap")",
                "duplicate-definition"},
        Invalid{R"(Texture "t" "float" "constant" "float value" [1 2])", "invalid-parameter"},
        Invalid{R"(Texture "t" "float" "constant" "rgb value" [1 1 1])", "invalid-parameter"},
        Invalid{R"(Texture "t" "spectrum" "constant" "float value" .2)", "invalid-parameter"},
        Invalid{R"(Texture "t" "spectrum" "constant" "texture value" "other")",
                "invalid-parameter"},
        Invalid{R"(Texture "t" "spectrum" "constant" "float unexpected" .2)", "unused-parameter"},
        Invalid{R"(Texture "t" "spectrum" "constant" "rgb value" [0 1.1 1])", "invalid-parameter"},
        Invalid{R"(Texture "t" "spectrum" "constant" "spectrum value" [680 .2 410 .2])",
                "invalid-parameter"},
        Invalid{R"(Texture "t" "spectrum" "constant" "blackbody value" [-10])",
                "invalid-parameter"},
        Invalid{R"(Material "diffuse" "texture reflectance" ["a" "b"])", "invalid-parameter"},
    };
    for (const auto &[text, code] : invalid) {
        const auto result = check(write_file(directory, "invalid-constant.pbrt",
                                             std::string{scene_start} + std::string{text}));
        require(result.exit_code() == 1 && !result.scene, report(result));
        find_diagnostic(result.document, code);
    }

    constexpr std::array typed_failures{
        R"(Material "diffuse" "texture reflectance" ""
)",
        R"(Material "diffuse" "texture reflectance" "t"
Texture "t" "float" "constant"
)",
        R"(Material "diffuse" "texture displacement" "t"
Texture "t" "spectrum" "constant"
)",
    };
    for (std::size_t i = 0; i < typed_failures.size(); ++i) {
        const auto result = check(write_file(directory, "invalid-texture-binding.pbrt",
                                             std::string{scene_start} + typed_failures.at(i)));
        require(result.exit_code() == 1 && !result.scene, report(result));
        const auto &diagnostic = find_diagnostic(result.document, "undefined-reference");
        require(diagnostic.location.line == 3 &&
                    diagnostic.message.starts_with(i == 2 ? "Undefined float texture"
                                                          : "Undefined spectrum texture"),
                "typed reference errors identify the use and the required channel");
    }

    constexpr std::array deferred_consumers{
        R"(Material "diffuse" "texture displacement" "t")",
        R"(Shape "trianglemesh" "point3 P" [0 0 0 1 0 0 0 1 0] "texture alpha" "t")",
    };
    for (const auto *consumer : deferred_consumers) {
        const auto result = check(write_file(
            directory, "deferred-texture-consumer.pbrt",
            std::string{scene_start} + "Texture \"t\" \"float\" \"constant\"\n" + consumer));
        require(result.exit_code() == 2 && !result.scene, report(result));
        require(find_diagnostic(result.document, "unsupported-parameter").location.line == 4,
                "valid float references preserve deferred consumer diagnostics");
    }
}

void dependency_tests(const fs::path &directory) {
    fs::create_directories(directory / "other working directory");
    fs::current_path(directory / "other working directory");
    const auto entry = write_file(directory, "entry.pbrt",
                                  std::string{scene_start} + R"(Include "nested/first.pbrt"
Include "nested/first.pbrt"
)");
    const auto included = write_file(directory, "nested/first.pbrt", R"(Include "shared.pbrt"
)");
    const auto shared = write_file(directory, "shared.pbrt", triangle);
    write_file(directory, "nested/shared.pbrt", "WrongDirectory");
    const auto result = check(fs::relative(entry));
    require(checked_scene(result).shapes.size() == 2 && result.document.expansion_count == 5,
            "root-relative nested includes from other CWD and repeated expansion");
    const auto &dependencies = result.document.dependencies;
    require(dependencies.size() == 4 &&
                fs::canonical(dependencies.at(0).resolved_path) == included &&
                fs::canonical(dependencies.at(1).resolved_path) == shared &&
                fs::canonical(dependencies.at(2).resolved_path) == included &&
                fs::canonical(dependencies.at(3).resolved_path) == shared,
            "dependencies follow encounter order");
    for (const auto &dependency : dependencies) {
        require(dependency.kind == DependencyKind::include && dependency.exists &&
                    dependency.visited,
                "include edge state");
    }
    require(fs::canonical(*dependencies.at(1).location.file) == included &&
                dependencies.at(1).location.line == 1 && dependencies.at(1).location.column == 9,
            "include dependency records reference origin");

    write_file(directory, "nested/first.pbrt", R"(Include "nested/../entry.pbrt"
)");
    const auto cycle = parse_file(entry);
    require(!cycle.dependencies_complete &&
                *find_diagnostic(cycle, "include-cycle").location.file == included.string(),
            "canonical cycle detection and included-file diagnostic location");

    const auto split_scope =
        write_file(directory, "scope-entry.pbrt", std::string{scene_start} + R"(AttributeBegin
Translate 7 0 0
Include "close-scope.pbrt"
)" + std::string{triangle});
    write_file(directory, "close-scope.pbrt", std::string{triangle} + "AttributeEnd\n");
    const auto split = check(split_scope);
    const auto &split_scene = checked_scene(split);
    require(split_scene.shapes.at(0).descriptor.transform.values.at(3) == 7 &&
                split_scene.shapes.at(1).descriptor.transform.values.at(3) == 0,
            "include shares graphics state and can close a parent scope");

    const auto pending = write_file(directory, "pending.pbrt", std::string{scene_start} + R"(
Include "missing.pbrt"
Import "module.pbrt"
Include "compressed.pbrt.gz"
Shape "plymesh" "string filename" "mesh.ply"
Texture "t" "spectrum" "imagemap" "string filename" "map.exr"
Material "diffuse" "spectrum reflectance" "curve.spd"
)");
    write_file(fs::current_path(), "missing.pbrt", "Identity");
    write_file(directory, "module.pbrt", "UnknownDirective");
    write_file(directory, "compressed.pbrt.gz", "\x1f\x8b");
    write_file(directory, "mesh.ply", "unchecked");
    write_file(directory, "map.exr", "unchecked");
    write_file(directory, "curve.spd", "unchecked");
    const auto incomplete = check(pending);
    require(incomplete.exit_code() == 1 && !incomplete.document.dependencies_complete,
            "missing dependency takes priority over unsupported directives");
    find_diagnostic(incomplete.document, "missing-file");
    find_diagnostic(incomplete.document, "unsupported-import");
    find_diagnostic(incomplete.document, "unsupported-gzip");
    constexpr std::array expected{DependencyKind::include, DependencyKind::import,
                                  DependencyKind::include, DependencyKind::mesh,
                                  DependencyKind::image,   DependencyKind::spectrum};
    require(incomplete.document.dependencies.size() == expected.size(), report(incomplete));
    for (std::size_t i = 0; i < expected.size(); ++i) {
        const auto &dependency = incomplete.document.dependencies.at(i);
        require(dependency.kind == expected.at(i) && dependency.exists == (i != 0) &&
                    !dependency.visited,
                "dependency kind, availability and unchecked-content status");
    }

    const auto gzip = check(write_file(directory, "compressed.pbrt", "\x1f\x8b"));
    require(gzip.exit_code() == 2 && !gzip.document.syntax_complete &&
                !gzip.document.dependencies_complete,
            "compressed entry explicitly unsupported");
    find_diagnostic(gzip.document, "unsupported-gzip");
    require(check(directory / "absent-entry.pbrt").exit_code() == 1, "missing entry is an error");

    const auto replaced =
        check(write_file(directory, "replaced-spectrum.pbrt", std::string{scene_start} + R"(
Material "diffuse" "spectrum reflectance" "unused.spd" "rgb reflectance" [.5 .5 .5]
)"));
    checked_scene(replaced);
    require(replaced.document.dependencies.empty(), "overridden spectrum is not an input");

    const auto inherited = check(write_file(directory, "inherited-spectrum.pbrt",
                                            std::string{scene_start} + R"(Attribute "material"
    "spectrum reflectance" "inherited-missing.spd"
Material "diffuse"
)"));
    const auto &missing = find_diagnostic(inherited.document, "missing-file");
    require(inherited.exit_code() == 1 && missing.location.line == 4 &&
                inherited.document.dependencies.size() == 1 &&
                inherited.document.dependencies.at(0).kind == DependencyKind::spectrum &&
                !inherited.document.dependencies.at(0).exists,
            "effective inherited spectrum is an input with definition provenance");

    write_file(directory, "constant.spd", "400 .1 700 .2\n");
    const auto file_constant = check(write_file(directory, "file-constant.pbrt", R"(WorldBegin
Attribute "texture" "spectrum value" "constant.spd"
Texture "t" "spectrum" "constant"
)"));
    require(file_constant.exit_code() == 2 && !file_constant.scene &&
                file_constant.document.dependencies.size() == 1,
            "inherited constant spectrum files are inventoried and deferred");
    const auto &constant_input = file_constant.document.dependencies.front();
    require(constant_input.kind == DependencyKind::spectrum && constant_input.exists &&
                !constant_input.visited && constant_input.location.line == 2,
            "constant spectrum file retains its inherited source");
    find_diagnostic(file_constant.document, "unsupported-parameter");

    const auto partial_constants =
        check(write_file(directory, "partial-constants.pbrt", R"(WorldBegin
Attribute "texture" "spectrum value" "inherited.spd"
Include "compressed.pbrt.gz"
Texture "t" "spectrum" "constant" "spectrum value" "constant.spd"
Texture "shadowed" "spectrum" "constant" "spectrum value" "unused.spd" "rgb value" [1 1 1]
Texture "default" "spectrum" "constant"
Material "diffuse" "texture reflectance" "unverified"
)"));
    require(partial_constants.exit_code() == 2 && !partial_constants.scene &&
                partial_constants.document.dependencies.size() == 2,
            "after an unread Include only effective explicit spectrum paths are inventoried");
    const auto &partial_input = partial_constants.document.dependencies.at(1);
    require(partial_input.kind == DependencyKind::spectrum &&
                partial_input.authored_path == "constant.spd" && partial_input.location.line == 4,
            "explicit live constant path is found while shadowed and inherited paths are skipped");

    const auto image = check(write_file(directory, "texture-image.pbrt", R"(WorldBegin
Material "diffuse" "texture reflectance" "image"
Texture "image" "spectrum" "imagemap" "string filename" "map.exr"
)"));
    require(image.exit_code() == 2 && !image.scene && image.document.dependencies.size() == 1,
            "a resolved image texture reference remains outside the structural profile");
    const auto &image_input = image.document.dependencies.front();
    require(image_input.kind == DependencyKind::image && image_input.exists &&
                !image_input.visited && image_input.location.line == 3,
            "referenced image textures retain the image asset in the dependency inventory");
    find_diagnostic(image.document, "unsupported-feature");
}

void review_regression_tests(const fs::path &directory) {
    const auto stopped = check(write_file(directory, "stopped-prefix.pbrt", R"(WorldBegin
NamedMaterial "later"
Translate 0x1p2 0 0
MakeNamedMaterial "later" "string type" "diffuse"
)"));
    require(stopped.exit_code() == 2 && !stopped.scene && !stopped.document.syntax_complete,
            "an incomplete unsupported prefix cannot prove a later reference undefined");

    write_file(directory, "scope.pbrt.gz", "\x1f\x8b");
    const auto unknown_scope = check(write_file(directory, "unknown-scope.pbrt", R"(WorldBegin
Include "scope.pbrt.gz"
AttributeEnd
)"));
    require(unknown_scope.exit_code() == 2 && !unknown_scope.scene,
            "an unvisited textual include may contain scope transitions");

    Limits diagnostic_limit;
    diagnostic_limit.diagnostics = 2;
    const auto truncated =
        check(write_file(directory, "semantic-diagnostic-limit.pbrt", R"(WorldBegin
AreaLightSource "diffuse" "bool twosided" [true false]
AreaLightSource "diffuse" "float scale" -1
AreaLightSource "diffuse" "float twosided" 1
)"),
              diagnostic_limit);
    require(truncated.exit_code() == 1 && truncated.document.syntax_complete &&
                truncated.document.diagnostics_truncated && !truncated.structure_complete,
            "semantic diagnostic limits do not erase a completed syntax pass");

    const auto inherited_texture =
        check(write_file(directory, "owned-texture-reference.pbrt", R"(WorldBegin
AttributeBegin
Attribute "material" "texture reflectance" "later-texture"
Material "diffuse"
AttributeEnd
Texture "later-texture" "spectrum" "constant"
)"));
    const auto &textures = checked_scene(inherited_texture);
    const auto reference = textures.inline_materials.at(0)->effective_parameter("reflectance");
    require(reference.parameter && reference.parameter->location.line == 3 &&
                reference.parameter->values.at(0).text == "later-texture",
            "inherited texture reference owns its source after scope destruction");

    std::string paths = "WorldBegin\n";
    constexpr std::size_t path_count = 12;
    for (std::size_t i = 0; i < path_count; ++i) {
        const auto name = "spectrum-" + std::to_string(i) + ".spd";
        write_file(directory, name, "400 .1 700 .2\n");
        paths += "AttributeBegin\nAttribute \"material\" \"spectrum reflectance\" \"" + name +
                 "\"\nMaterial \"diffuse\"\nAttributeEnd\n";
    }
    const auto owned_paths = check(write_file(directory, "owned-dependencies.pbrt", paths));
    require(owned_paths.exit_code() == 2 && owned_paths.document.dependencies.size() == path_count,
            "dependency source identity survives destroyed and reused inherited storage");

    const auto inherited_shape = check(write_file(directory, "preworld-attribute.pbrt", R"(
Attribute "shape" "point3 P" [0 0 0 1 0 0 0 1 0]
WorldBegin
Shape "trianglemesh"
)"));
    require(checked_scene(inherited_shape).geometry.meshes.size() == 1,
            "Attribute defaults may precede WorldBegin and survive its transform reset");
}

void bounded_and_numeric_tests(const fs::path &directory) {
    const std::string name(60'000, 'm');
    std::string shared = "WorldBegin\nMakeNamedMaterial \"" + name +
                         "\" \"string type\" \"diffuse\"\nNamedMaterial \"" + name + "\"\n";
    for (std::size_t i = 0; i < 1000; ++i) {
        shared += triangle;
    }
    const auto sharing = check(write_file(directory, "shared-name.pbrt", shared));
    const auto &shapes = checked_scene(sharing).shapes;
    require(shapes.size() == 1000 && shapes.front().material.name == shapes.back().material.name,
            "large inherited material names have shared owned storage across shapes");

    std::string repeated =
        "WorldBegin\nAttribute \"material\" \"spectrum reflectance\" [1 .1 2 .2]\n";
    for (std::size_t i = 0; i < 20; ++i) {
        repeated += "Material \"diffuse\"\n";
    }
    Limits work_limit;
    work_limit.total_values = 40;
    const auto work = check(write_file(directory, "inherited-work.pbrt", repeated), work_limit);
    require(work.exit_code() == 1 && work.document.syntax_complete,
            "inherited validation work is bounded separately from authored tokens");
    find_diagnostic(work.document, "limit-exceeded");

    std::string repeated_constants =
        "WorldBegin\nAttribute \"texture\" \"spectrum value\" [1 .1 2 .2 3 .3 4 .4]\n";
    for (std::size_t i = 0; i < 20; ++i) {
        repeated_constants += "Texture \"" + std::to_string(i) + "\" \"spectrum\" \"constant\"\n";
    }
    work_limit.total_values = 100;
    const auto constant_work = check(
        write_file(directory, "inherited-constant-work.pbrt", repeated_constants), work_limit);
    require(constant_work.exit_code() == 1 && constant_work.document.syntax_complete,
            "repeated inherited texture values charge effective validation work");
    find_diagnostic(constant_work.document, "limit-exceeded");

    const auto mix = check(write_file(directory, "owned-mix.pbrt", R"(WorldBegin
AttributeBegin
Attribute "material" "string materials" ["long_material_name_aaaaaaaaaaaaaaaa" "long_material_name_bbbbbbbbbbbbbbbb"]
Material "mix"
AttributeEnd
MakeNamedMaterial "long_material_name_aaaaaaaaaaaaaaaa" "string type" "diffuse"
MakeNamedMaterial "long_material_name_bbbbbbbbbbbbbbbb" "string type" "diffuse"
)"));
    require(mix.exit_code() == 2,
            "deferred mix references survive inherited parameter destruction");

    constexpr std::array malformed_hex{"0x", "0x1pg", "0xgarbage", "-0x.1p+"};
    for (const auto *literal : malformed_hex) {
        const auto invalid =
            check(write_file(directory, "malformed-hex.pbrt",
                             "WorldBegin\nTranslate " + std::string{literal} + " 0 0\n"));
        require(invalid.exit_code() == 1, "malformed hexadecimal spelling is an error");
    }
    write_file(directory, "partial.pbrt.gz", "\x1f\x8b");
    for (const auto *suffix : {"\"rgb reflectance\" [.5 .5 .5]", "0 0"}) {
        const auto partial =
            check(write_file(directory, "partial-parent.pbrt",
                             "WorldBegin\nInclude \"partial.pbrt.gz\"\n" + std::string{suffix}));
        require(partial.exit_code() == 2 && !partial.document.syntax_complete,
                "unvisited textual input may leave a partial directive at parent continuation");
    }

    constexpr std::array invalid_scenes{
        R"(Transform [-0.86 -0.08 0.71 0 -0.77 -0.57 -0.22 0 -0.79 0.88 -0.36 0 0 0 0 1] ConcatTransform [5 -3 2 0 6 -6 0 0 3 6 9 0 0 0 0 1] Camera "perspective" WorldBegin)",
        R"(Transform [5 -3 2 0 6 -6 0 0 3 6 9 0 0 0 0 1] Camera "perspective" WorldBegin)",
        R"(LookAt 0 0 0 3 7 2 9 21 6 Camera "perspective" WorldBegin)",
        R"(Camera "perspective" "float shutteropen" 2 WorldBegin)",
        R"(Film "rgb" "string sensor" "absent" WorldBegin)",
        R"(Film "rgb" "integer xresolution" 10 "integer yresolution" 10 "integer pixelbounds" [20 30 0 10] WorldBegin)",
        R"(WorldBegin Shape "plymesh")",
        R"(WorldBegin Texture "t" "spectrum" "imagemap" "string filename" "")",
        R"(WorldBegin Material "measured")",
        R"(WorldBegin Material "diffuse" "spectrum reflectance" "")",
    };
    for (const auto *scene : invalid_scenes) {
        const auto invalid = check(write_file(directory, "invalid-checked-scene.pbrt", scene));
        require(invalid.exit_code() == 1 && !invalid.scene, report(invalid));
    }
    const auto replaced_camera =
        check(write_file(directory, "replaced-camera.pbrt",
                         R"(Camera "realistic" "string lensfile" "unused-missing.dat"
Camera "perspective"
WorldBegin
)"));
    require(checked_scene(replaced_camera).options.size() == 1 &&
                replaced_camera.document.dependencies.empty(),
            "only final option assets are required");

    std::string asset_list = "WorldBegin\nMaterial \"coateddiffuse\" \"spectrum reflectance\" [";
    for (std::size_t i = 0; i < 1000; ++i) {
        asset_list += "\"missing.spd\" ";
    }
    asset_list += "]\n";
    const auto dedup = check(write_file(directory, "repeated-assets.pbrt", asset_list));
    require(dedup.exit_code() == 1 && dedup.document.dependencies.size() <= 1,
            "repeated external assets cannot amplify the dependency report");

    // Exact integer reference, independently scaled by powers of two per row.
    std::uint32_t random = 0xa318e74b;
    for (std::size_t trial = 0; trial < 512; ++trial) {
        std::array<std::array<std::int64_t, 3>, 3> a{};
        for (auto &row : a) {
            for (auto &value : row) {
                random ^= random << 13;
                random ^= random >> 17;
                random ^= random << 5;
                value = static_cast<std::int64_t>(random % 2001) - 1000;
            }
        }
        if (trial % 2 == 0) {
            for (std::size_t column = 0; column < 3; ++column) {
                a[2][column] = a[0][column] + a[1][column];
            }
        }
        const auto determinant = a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) -
                                 a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
                                 a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
        Transform transform;
        constexpr std::array exponents{900, -900, 0};
        for (std::size_t row = 0; row < 3; ++row) {
            for (std::size_t column = 0; column < 3; ++column) {
                transform.values[row * 4 + column] =
                    std::ldexp(static_cast<double>(a[row][column]), exponents[row]);
            }
        }
        require(exact_singular_linear(transform) == (determinant == 0),
                "exact binary64 singularity agrees with independent integer determinant");
    }
    Transform extremes;
    extremes.values[0] = std::numeric_limits<double>::denorm_min();
    extremes.values[5] = std::numeric_limits<double>::max();
    require(!exact_singular_linear(extremes), "determinant preserves subnormal nonzero products");
}

void limits_tests(const fs::path &directory) {
    constexpr std::string_view input = R"(WorldBegin
Include "nested.pbrt"
)";
    constexpr std::string_view nested = R"(Material "diffuse" "rgb reflectance" [1 1 1]
)";
    const auto entry = write_file(directory, "entry.pbrt", input);
    write_file(directory, "nested.pbrt", nested);
    struct Boundary {
        std::size_t Limits::*member;
        std::size_t exact;
        std::string_view name;
    };
    constexpr std::array boundaries{
        Boundary{&Limits::file_bytes, nested.size(), "individual file bytes"},
        Boundary{&Limits::expanded_bytes, input.size() + nested.size(), "expanded bytes"},
        Boundary{&Limits::active_files, 2, "active file frames"},
        Boundary{&Limits::expansions, 2, "total file expansions"},
        Boundary{&Limits::directives, 3, "directives including Include"},
        Boundary{&Limits::list_values, 3, "values per array"},
        Boundary{&Limits::total_values, 5, "all typed values"},
    };
    for (const auto &boundary : boundaries) {
        Limits limits;
        limits.*boundary.member = boundary.exact;
        const auto accepted = parse_file(entry, limits);
        require_clean(accepted);
        require(accepted.syntax_complete && accepted.dependencies_complete, boundary.name);
        --(limits.*boundary.member);
        const auto rejected = parse_file(entry, limits);
        require(find_diagnostic(rejected, "limit-exceeded").severity == Severity::error,
                boundary.name);
        require(!rejected.syntax_complete || !rejected.dependencies_complete,
                "rejected boundary marks the scan incomplete");
    }

    Limits token_limit;
    token_limit.token_bytes = std::string_view{"WorldBegin"}.size();
    const auto word = write_file(directory, "word.pbrt", "WorldBegin\n");
    require_clean(parse_file(word, token_limit));
    --token_limit.token_bytes;
    find_diagnostic(parse_file(word, token_limit), "limit-exceeded");

    Limits scope_limit;
    scope_limit.scopes = 1;
    const auto one_scope = write_file(directory, "scopes.pbrt",
                                      std::string{scene_start} + "AttributeBegin\nAttributeEnd\n");
    checked_scene(check(one_scope, scope_limit));
    const auto two_scopes = write_file(
        directory, "scopes.pbrt",
        std::string{scene_start} + "AttributeBegin\nAttributeBegin\nAttributeEnd\nAttributeEnd\n");
    const auto scope_rejected = check(two_scopes, scope_limit);
    require(scope_rejected.exit_code() == 1 && !scope_rejected.scene, "scope depth boundary");
    find_diagnostic(scope_rejected.document, "limit-exceeded");

    Limits diagnostic_limit;
    diagnostic_limit.diagnostics = 1;
    const auto one_error = write_file(directory, "diagnostics.pbrt", R"(Include "missing-1.pbrt"
)");
    const auto one_diagnostic = parse_file(one_error, diagnostic_limit);
    require(one_diagnostic.diagnostics.size() == 1 && !one_diagnostic.diagnostics_truncated,
            "exact diagnostic capacity accepted");
    const auto two_errors = write_file(directory, "diagnostics.pbrt", R"(Include "missing-1.pbrt"
Include "missing-2.pbrt"
)");
    const auto truncated = check(two_errors, diagnostic_limit);
    require(truncated.document.diagnostics.size() == 1 &&
                truncated.document.diagnostics_truncated && truncated.exit_code() == 1,
            "overflow reserves bounded space for truncation diagnostic");
    find_diagnostic(truncated.document, "diagnostic-limit");
    diagnostic_limit.diagnostics = 0;
    const auto suppressed = check(one_error, diagnostic_limit);
    require(suppressed.document.diagnostics.empty() && suppressed.document.diagnostics_truncated &&
                suppressed.exit_code() == 1,
            "zero diagnostic capacity still reports failure");
}

void geometry_tests(const fs::path &directory) {
    struct Invalid {
        std::string_view body;
        std::string_view code;
    };
    constexpr std::array bad{
        Invalid{R"(Shape "trianglemesh" "point3 P" [0 0 0 1e39 0 0 0 1 0]
"integer indices" [0 1 2])",
                "invalid-geometry"},
        Invalid{R"(Shape "trianglemesh" "point3 P" [16777216 0 0 16777217 0 0 16777216 1 0]
"integer indices" [0 1 2])",
                "invalid-geometry"},
        Invalid{R"(Shape "trianglemesh" "point3 P" [0 0 0 1 0 0 0 1 0]
"integer indices" [-1 1 2])",
                "invalid-geometry"},
        Invalid{R"(Shape "trianglemesh" "point3 P" [0 0 0 1 0 0 0 1 0]
"integer indices" [0 1 3])",
                "invalid-geometry"},
        Invalid{R"(Shape "trianglemesh" "point3 P" [0 0 0 1 0 0 0 1 0]
"integer indices" [0 1])",
                "invalid-geometry"},
        Invalid{R"(Shape "trianglemesh" "point3 P" [0 0 0 1 0 0 0 1 0]
"integer indices" [0 1 2] "normal N" [0 0 1])",
                "invalid-geometry"},
        Invalid{R"(Shape "trianglemesh" "point3 P" [0 0 0 1 0 0 0 1 0]
"point2 uv" [0 0 1 1])",
                "invalid-geometry"},
        Invalid{R"(Shape "trianglemesh" "point3 P" [0 0 0 1 0 0 0 1 0]
"point2 uv" [0 0 1 1 0])",
                "invalid-list"},
        Invalid{R"(Shape "trianglemesh" "point3 P" [0 0 0 1 0 0 0 1 0]
"float uv" [0 0 1 1 0 1])",
                "invalid-parameter"},
        Invalid{"Scale 0 1 1", "invalid-transform"},
        Invalid{"LookAt 0 0 0 0 0 1 0 0 1", "invalid-transform"},
    };
    for (const auto &sample : bad) {
        const auto invalid = check(write_file(directory, "invalid-geometry.pbrt",
                                              std::string{scene_start} + std::string{sample.body}));
        require(invalid.exit_code() == 1 && !invalid.scene, report(invalid));
        find_diagnostic(invalid.document, sample.code);
    }
    const auto narrowed =
        check(write_file(directory, "narrowed.pbrt",
                         std::string{scene_start} + "Scale 1e-50 1 1\n" + std::string{triangle}));
    require(narrowed.exit_code() == 1 && !narrowed.scene,
            "transform becoming singular after float narrowing fails");
}

void triangle_attributes(const fs::path &directory) {
    const auto path = write_file(directory, "triangle-attributes.pbrt", R"(WorldBegin
Attribute "shape" "normal N" [0 0 2 0 0 3 0 0 4] "point2 uv" [-1 2 0 0 1 1]
AttributeBegin
    Attribute "shape" "point2 uv" [0 0 2 0 0 2]
    ObjectBegin "member"
        Scale 2 3 4
        Shape "trianglemesh" "point3 P" [0 0 0 1 0 0 0 1 0]
            "normal N" [1e39 0 0]
            "normal3 N" [0 0 0 1 0 0 0 2 0]
    ObjectEnd
AttributeEnd
Shape "trianglemesh" "point3 P" [0 0 0 1 0 0 0 1 0]
ObjectInstance "member"
Translate 3 0 0
ObjectInstance "member"
)");
    const auto checked = check(path);
    const auto &geometry = checked_scene(checked).geometry;
    const auto &member = geometry.meshes.at(0);
    require(member.normals.size() == 3 && member.normals.at(0).z == 0 &&
                member.normals.at(1).x == 1 && member.normals.at(2).y == 2 &&
                member.uvs.at(1) == std::array{2.0f, 0.0f},
            "Last typed normal and scoped UV defaults retain the authored object-space values");
    const auto &direct = geometry.meshes.at(1);
    require(direct.normals.at(0).z == 2 && direct.normals.at(2).z == 4 &&
                direct.uvs.at(0) == std::array{-1.0f, 2.0f},
            "Defaults are restored after the nested scope, with UVs outside the unit square");
    require(geometry.instances.size() == 3 && geometry.instances.at(1).mesh_index == 0 &&
                geometry.instances.at(2).mesh_index == 0 &&
                geometry.instances.at(2).transform.values[3] == 3,
            "Both placements reference one mesh's attributes");

    for (const auto &sample :
         {R"("normal N" [0 0 1 0 0 1e39 0 0 1])", R"("point2 uv" [0 0 1e39 1 0 1])"}) {
        const auto invalid_path =
            write_file(directory, "triangle-attribute-overflow.pbrt",
                       std::string{scene_start} + std::string{triangle} + sample);
        const auto rejected = check(invalid_path);
        const auto &diagnostic = find_diagnostic(rejected.document, "invalid-geometry");
        require(rejected.exit_code() == 1 && !rejected.scene && diagnostic.location.line == 5 &&
                    *diagnostic.location.file == invalid_path.string() &&
                    diagnostic.message.find("overflows binary32") != std::string::npos,
                "Attribute narrowing failures identify the authored component");
    }

    const auto inherited = write_file(directory, "triangle-attribute-storage.pbrt", R"(WorldBegin
Attribute "shape" "point3 P" [0 0 0 1 0 0 0 1 0] "normal N" [0 0 1 0 0 1 0 0 1]
Shape "trianglemesh"
Shape "trianglemesh"
)");
    Limits limits;
    limits.total_values = 42;
    const auto at_limit = check(inherited, limits);
    require(checked_scene(at_limit).geometry.meshes.size() == 2,
            "Two expanded attributed meshes fit the exact storage budget");
    --limits.total_values;
    const auto over_limit = check(inherited, limits);
    require(over_limit.exit_code() == 1 && !over_limit.scene &&
                find_diagnostic(over_limit.document, "limit-exceeded").message ==
                    "Translated mesh value limit exceeded",
            "Inherited vertex normals count again when a second mesh owns their values");
}

void area_light_state(const fs::path &directory) {
    std::string source(scene_start);
    source += R"(Attribute "light" "rgb L" [2 1 0.5] "float scale" 3
AreaLightSource "diffuse"
Attribute "light" "float scale" 7
Translate 1 2 3
)";
    source += triangle;
    source += "AttributeBegin\nAreaLightSource \"diffuse\" \"bool twosided\" true\n";
    source += triangle;
    source += "AreaLightSource \"\"\n";
    source += triangle;
    source += "AttributeEnd\n";
    source += triangle;
    source += "ObjectBegin \"object\"\nAreaLightSource \"\"\n";
    source += triangle;
    source += "ObjectEnd\nObjectInstance \"object\"\n";
    source += triangle;
    const auto checked = check(write_file(directory, "scoped-area-light.pbrt", source));
    const auto &scene = checked_scene(checked);
    require(scene.area_lights.size() == 2 && scene.lights.empty() && scene.shapes.size() == 5 &&
                scene.objects.size() == 1 && scene.geometry.instances.size() == 6,
            "Area declarations, checked shapes, and placements stay distinct");
    const auto &outer = scene.area_lights[0];
    const auto &inner = scene.area_lights[1];
    require(scene.shapes[0].area_light == outer && scene.shapes[1].area_light == inner &&
                !scene.shapes[2].area_light && scene.shapes[3].area_light == outer &&
                scene.shapes[4].area_light == outer && !scene.objects[0].shapes[0].area_light,
            "Area binding replacement, reset, and scope restoration");
    require(outer->effective_parameter("scale").parameter->values.front().number == 3 &&
                inner->effective_parameter("scale").parameter->values.front().number == 7 &&
                outer->effective_parameter("L").color_space == "srgb" &&
                outer->transform.values == Transform{}.values &&
                scene.shapes[0].descriptor.transform.values[3] == 1,
            "Area declarations capture defaults and shapes capture transforms");

    const auto object = check(write_file(
        directory, "emissive-object.pbrt",
        std::string(scene_start) + "AreaLightSource \"diffuse\"\nObjectBegin \"object\"\n" +
            std::string(triangle) + "ObjectEnd\n"));
    const auto &object_diagnostic = find_diagnostic(object.document, "unsupported-feature");
    require(object.exit_code() == 2 && !object.scene &&
                object_diagnostic.message.find("inside object definitions") != std::string::npos &&
                object_diagnostic.location.line == 6,
            "Inherited emitters on object member shapes have a shape diagnostic");

    write_file(directory, "emission.exr", "asset placeholder");
    const auto image = check(write_file(directory, "area-image.pbrt", R"(WorldBegin
Attribute "light" "string filename" "emission.exr"
AreaLightSource "diffuse"
)"));
    require(image.exit_code() == 2 && image.document.dependencies.size() == 1 &&
                image.document.dependencies.front().kind == DependencyKind::image,
            "An inherited area light image is inventoried and unsupported");
    const auto conflict = check(write_file(directory, "area-image-conflict.pbrt", R"(WorldBegin
Attribute "light" "string filename" "emission.exr"
AreaLightSource "diffuse" "rgb L" [1 1 1]
)"));
    require(conflict.exit_code() == 1 && conflict.document.dependencies.size() == 1 &&
                find_diagnostic(conflict.document, "invalid-parameter").location.line == 2,
            "Effective L and filename conflict at the inherited filename");
    const auto reset = check(write_file(directory, "area-image-reset.pbrt", R"(WorldBegin
Attribute "light" "string filename" "missing.exr"
AreaLightSource "diffuse" "string filename" ""
AreaLightSource ""
)"));
    require(checked_scene(reset).area_lights.size() == 1 && reset.document.dependencies.empty(),
            "Explicit empty filename and reset do not inherit an image dependency");
    const auto malformed = check(write_file(directory, "area-malformed-spectrum.pbrt", R"(WorldBegin
AreaLightSource "diffuse" "spectrum L" ["missing-a.spd" "missing-b.spd"]
)"));
    require(malformed.exit_code() == 1 && !malformed.document.dependencies_complete &&
                malformed.document.dependencies.size() == 2 &&
                find_diagnostic(malformed.document, "invalid-parameter").location.line == 2,
            "Malformed effective emission retains discoverable spectrum dependencies");
}

void triangle_filter_schema(const fs::path &directory) {
    struct RejectedFilter {
        std::string_view source;
        std::string_view code;
    };
    constexpr std::array rejected{
        RejectedFilter{R"(PixelFilter "triangle" "float xradius" 0)", "invalid-parameter"},
        RejectedFilter{R"(PixelFilter "triangle" "float yradius" -1)", "invalid-parameter"},
        RejectedFilter{R"(PixelFilter "triangle" "float xradius" nan)", "invalid-number"},
        RejectedFilter{R"(PixelFilter "triangle" "float xradius" [1 2])", "invalid-parameter"},
        RejectedFilter{R"(PixelFilter "triangle" "integer yradius" 1)", "invalid-parameter"},
        RejectedFilter{R"(PixelFilter "triangle" "float radius" 1)", "unused-parameter"},
    };
    for (const auto &[source, code] : rejected) {
        const auto path =
            write_file(directory, "invalid-filter.pbrt", std::string(source) + "\nWorldBegin\n");
        const auto result = check(path);
        require(result.exit_code() == 1 && !result.scene, report(result));
        const auto &diagnostic = find_diagnostic(result.document, code);
        require(diagnostic.location.line == 1 && *diagnostic.location.file == path.string(),
                "Triangle filter schema errors retain source location");
    }
}

void sobol_sampler_schema(const fs::path &directory) {
    const auto invalid = write_file(directory, "invalid-sobol.pbrt", R"(Sampler "sobol"
    "string randomization" "bogus"
WorldBegin
)");
    const auto rejected = check(invalid);
    const auto &diagnostic = find_diagnostic(rejected.document, "invalid-parameter");
    require(rejected.exit_code() == 1 && diagnostic.location.line == 2 &&
                *diagnostic.location.file == invalid.string() &&
                diagnostic.message.find("Sobol randomization") != std::string::npos,
            "Unknown Sobol randomization has a source-located diagnostic");

    const auto independent = check(write_file(directory, "independent-randomization.pbrt",
                                              R"(Sampler "independent" "string randomization" "owen"
WorldBegin
)"));
    require(independent.exit_code() == 1 &&
                find_diagnostic(independent.document, "unused-parameter").location.line == 1,
            "Independent sampler does not accept Sobol randomization");

    const auto option_seed = check(write_file(directory, "sobol-option-seed.pbrt",
                                              R"(Option "integer seed" 17
Sampler "sobol"
WorldBegin
)"));
    require(option_seed.exit_code() == 2 &&
                find_diagnostic(option_seed.document, "unsupported-feature").location.line == 1,
            "Scene-wide Option seed remains unsupported with Sobol");
}

void corpus_tests(const fs::path &fixture_directory) {
    const auto path = fixture_directory / "cornell-box.pbrt";
    const auto result = check(path);
    require(result.exit_code() == 0 && result.scene && result.document.syntax_complete &&
                result.document.dependencies_complete && result.document.dependencies.empty() &&
                result.document.diagnostics.empty(),
            report(result));
    require(result.inventory.named_materials == 8 && result.inventory.meshes == 8 &&
                result.inventory.triangles == 36 && result.inventory.instances == 0,
            "pinned Cornell Box inventory");
}

} // namespace

int main(int argc, char *argv[]) {
    try {
        require(argc == 4, "expected a group, scratch directory and fixture directory");
        const std::string_view group = argv[1];
        const auto directory = fs::weakly_canonical(argv[2]);
        if (group == "syntax") {
            syntax_tests(directory);
        } else if (group == "state") {
            state_tests(directory);
            constant_texture_tests(directory);
            triangle_filter_schema(directory);
            sobol_sampler_schema(directory);
            area_light_state(directory);
            review_regression_tests(directory);
            bounded_and_numeric_tests(directory);
        } else if (group == "dependencies") {
            dependency_tests(directory);
        } else if (group == "limits") {
            limits_tests(directory);
        } else if (group == "geometry") {
            geometry_tests(directory);
            triangle_attributes(directory);
        } else if (group == "corpus") {
            corpus_tests(fs::canonical(argv[3]));
        } else {
            throw std::runtime_error("unknown PBRT test group " + std::string{group});
        }
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
