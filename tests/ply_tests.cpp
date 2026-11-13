// SPDX-License-Identifier: GPL-3.0-only

#include "pbrt.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {

namespace fs = std::filesystem;
using namespace rypath::pbrt;
using namespace std::string_view_literals;

constexpr auto triangle_header = R"(ply
format ascii 1.0
element vertex 3
property float x
property float y
property float z
element face 1
property list uchar int vertex_indices
end_header
)"sv;
constexpr auto triangle_body = "0 0 0\n1 0 0\n0 1 0\n3 0 1 2\n"sv;
constexpr auto shape = "Shape \"plymesh\" \"string filename\" \"mesh.ply\""sv;
constexpr auto direct_scene = "WorldBegin\nShape \"plymesh\" \"string filename\" \"mesh.ply\"\n"sv;

void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

fs::path write(const fs::path &path, std::string_view content) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output << content;
    output.close();
    require(output.good(), "Could not write " + path.string());
    return path;
}

std::string triangle_ply() { return std::string(triangle_header) + std::string(triangle_body); }

std::string replace(std::string source, std::string_view old, std::string_view replacement) {
    const auto at = source.find(old);
    require(at != std::string::npos, "fixture replacement missing");
    source.replace(at, old.size(), replacement);
    return source;
}

const Scene &success(const Result &result) {
    if (!result.scene || result.exit_code() != 0 || !result.structure_complete) {
        throw std::runtime_error(report(result));
    }
    return *result.scene;
}

const Diagnostic &diagnostic(const Result &result, std::string_view code, int exit = 1) {
    const auto found = std::ranges::find(result.document.diagnostics, code, &Diagnostic::code);
    require(result.exit_code() == exit && !result.scene &&
                found != result.document.diagnostics.end(),
            report(result));
    return *found;
}

Result check_mesh(const fs::path &directory, std::string_view data, const Limits &limits = {}) {
    write(directory / "mesh.ply", data);
    return check(write(directory / "scene.pbrt", direct_scene), limits);
}

void ascii(const fs::path &directory) {
    const auto mixed = check_mesh(directory, R"(ply
format ascii 1.0
comment mixed mesh with attributes and property lists
obj_info synthetic fixture
element face 2
property uint32 face_indices
property list uint8 uint16 vertex_indices
property list char int16 metadata
element extra 1
property list int8 float64 values
element vertex 4
property float32 texture_t
property double ny
property float64 z
property char nx
property short x
property float u
property ushort y
property float texture_s
property float s
property float v
property int32 nz
property float t
property list uchar uint extras
end_header
7 3 0 1 2 2 -32768 32767
8 4 0 1 3 2 0
2 1e5 -2.5
10 0 0 0 0 +.25 0 9 8 .5 1 7 1 4294967295
10 0 0 0 1 .75 0 9 8 .5 1 7 0
10 0 0 0 0 .25 1 9 8 1. 1 7 0
10 0 .25 0 1 .75 1 9 8 1e0 1 7 0
)");
    const auto &mesh = success(mixed).geometry.meshes.at(0);
    require(mesh.positions.size() == 4 && mesh.positions[3].x == 1 && mesh.positions[3].y == 1 &&
                mesh.positions[3].z == .25f && mesh.normals.size() == 4 && mesh.normals[3].z == 1 &&
                mesh.uvs.size() == 4 && mesh.uvs[3] == std::array<float, 2>{.75f, 1},
            "Reordered heterogeneous vertex attributes and preferred u/v pair");
    require(mesh.triangles == std::vector<std::array<std::uint32_t, 3>>{{0, 1, 2}} &&
                mesh.patches == std::vector<std::array<std::uint32_t, 4>>{{0, 1, 2, 3}} &&
                mixed.inventory.triangles == 1 && mixed.inventory.patches == 1,
            "Mixed face topology and cyclic-to-tensor corner mapping");
    require(mixed.document.dependencies_complete && mixed.document.dependencies.size() == 1 &&
                mixed.document.dependencies.front().visited &&
                report(mixed).find("1 bilinear patches") != std::string::npos,
            "Checked PLY dependency and patch inventory");

    for (const auto &pair : {std::array{"s", "t"}, std::array{"texture_u", "texture_v"},
                             std::array{"texture_s", "texture_t"}}) {
        const auto header = replace(std::string(triangle_header), "element face 1",
                                    "property float " + std::string(pair[0]) + "\nproperty float " +
                                        pair[1] + "\nelement face 1");
        const auto alternate =
            check_mesh(directory, header + "0 0 0 .2 .8\n1 0 0 1 0\n0 1 0 0 1\n3 0 1 2\n");
        require(success(alternate).geometry.meshes[0].uvs[0] == std::array{.2f, .8f},
                "Alternate UV names");
    }
    for (const auto &variant :
         {std::array<std::string, 2>{"property float u\nproperty float v\nproperty float s\n"
                                     "property list uchar float texture_u\n",
                                     ".2 .8 99 1 4"},
          std::array<std::string, 2>{"property float u\nproperty float s\nproperty float t\n",
                                     "99 .2 .8"}}) {
        const auto header =
            replace(std::string(triangle_header), "element face 1", variant[0] + "element face 1");
        const auto selected =
            check_mesh(directory, header + "0 0 0 " + variant[1] + "\n1 0 0 " + variant[1] +
                                      "\n0 1 0 " + variant[1] + "\n3 0 1 2\n");
        require(success(selected).geometry.meshes[0].uvs[0] == std::array{.2f, .8f},
                "Partial and ignored UV alternatives cannot override the first complete pair");
    }
}

template <typename T> void append(std::string &output, T value, bool big) {
    const auto bytes = std::bit_cast<std::array<char, sizeof(T)>>(value);
    if ((std::endian::native == std::endian::big) == big) {
        output.append(bytes.begin(), bytes.end());
    } else {
        output.append(bytes.rbegin(), bytes.rend());
    }
}

std::string binary_triangle(bool big, float first = 0) {
    std::string data = "ply\r\nformat binary_";
    data += big ? "big_endian" : "little_endian";
    data += " 1.0\r\nelement vertex 3\r\nproperty double y\r\nproperty float x\r\n"
            "property int8 z\r\nelement face 1\r\nproperty list uint16 int32 vertex_indices\r\n"
            "end_header\r\n";
    // The first payload byte can itself be LF. It must survive the CRLF header.
    append(data, std::bit_cast<double>(big ? UINT64_C(0x0a00000000000000) : UINT64_C(0x0a)), big);
    append(data, first, big);
    append(data, std::int8_t{0}, big);
    for (const auto [x, y] : {std::array{1., 0.}, std::array{0., 1.}}) {
        append(data, y, big);
        append(data, static_cast<float>(x), big);
        append(data, std::int8_t{0}, big);
    }
    append(data, std::uint16_t{3}, big);
    for (std::int32_t i = 0; i < 3; ++i) {
        append(data, i, big);
    }
    return data;
}

void binary(const fs::path &directory) {
    for (const bool big : {false, true}) {
        auto data = binary_triangle(big);
        const auto result = check_mesh(directory, data);
        const auto &mesh = success(result).geometry.meshes.at(0);
        require(mesh.positions[0].y == 0 && mesh.positions[1].x == 1 && mesh.positions[2].y == 1 &&
                    mesh.triangles[0] == std::array<std::uint32_t, 3>{0, 1, 2} &&
                    mesh.patches.empty() && mesh.normals.empty() && mesh.uvs.empty(),
                "Binary decoding in either byte order");
        data.pop_back();
        diagnostic(check_mesh(directory, data), "invalid-ply");
        data += "ab";
        diagnostic(check_mesh(directory, data), "invalid-ply");
        diagnostic(
            check_mesh(directory, binary_triangle(big, std::numeric_limits<float>::infinity())),
            "invalid-ply");
    }
}

void invalid(const fs::path &directory) {
    const auto triangle = triangle_ply();
    for (const auto &data : {
             std::string("not a ply\n"),
             replace(triangle, "format ascii 1.0", "format ascii 2.0"),
             replace(triangle, "end_header\n", ""),
             replace(triangle, "element face 1", "element face 0"),
             replace(triangle, "element face 1", "element vertex 1"),
             replace(triangle, "float x", "nonsense x"),
             replace(triangle, "float x", "list uchar float x"),
             replace(triangle, "float x", "float y"),
             replace(triangle, "float x", "float nx"),
             replace(triangle, "float x", "float u"),
             replace(triangle, "element face 1", "property float nx\nelement face 1"),
             replace(triangle, "element face 1", "property float texture_v\nelement face 1"),
             replace(triangle, "uchar int vertex_indices", "float int vertex_indices"),
             replace(triangle, "uchar int vertex_indices", "uchar float vertex_indices"),
             replace(triangle, "vertex_indices", "vertex_index"),
             replace(triangle, "0 1 0\n", "0 + 0\n"),
             replace(triangle, "0 1 0\n", "0 1e50 0\n"),
             replace(triangle, "0 1 0\n", "1e-9999 1 0\n"),
             replace(triangle, "0 1 0\n", "0 0x1p0 0\n"),
             replace(triangle, "0 1 0\n", "0 1 0x0\n"),
             replace(triangle, "0 1 0\n", "0 nan 0\n"),
             replace(triangle, "3 0 1 2", "2 0 1"),
             replace(triangle, "3 0 1 2", "256 0 1 2"),
             replace(triangle, "3 0 1 2", "3 0 1 -1"),
             replace(triangle, "3 0 1 2", "3 0 1 3"),
             replace(triangle, "3 0 1 2", "3 0 1 4294967295"),
             replace(triangle, "3 0 1 2", "4 0 1 2"),
             replace(triangle, "3 0 1 2", "3 0 1 2 extra"),
             replace(triangle, "3 0 1 2", "5 0 1 2 0 3"),
         }) {
        const auto result = check_mesh(directory, data);
        const auto &issue = diagnostic(result, "invalid-ply");
        require(issue.location.line == 2 && issue.message.find("mesh.ply") != std::string::npos &&
                    issue.message.find("at byte ") != std::string::npos &&
                    !result.document.dependencies.front().visited &&
                    !result.document.dependencies_complete,
                report(result));
    }
    const auto polygon_header = replace(std::string(triangle_header), "vertex 3", "vertex 5");
    const auto polygon =
        check_mesh(directory, polygon_header + "0 0 0\n1 0 0\n2 1 0\n1 2 0\n0 1 0\n5 0 1 2 3 4\n");
    require(diagnostic(polygon, "unsupported-ply-polygon", 2).message.find("face row 1") !=
                    std::string::npos &&
                !polygon.document.dependencies.front().visited && polygon.inventory.triangles == 0,
            report(polygon));
    diagnostic(check_mesh(directory, replace(triangle, "3 0 1 2", "3 0 1 1")), "invalid-geometry");
    diagnostic(check_mesh(directory,
                          replace(replace(triangle, "float y", "double y"), "0 1 0", "0 1e50 0")),
               "invalid-ply");
    diagnostic(check_mesh(directory, replace(replace(triangle, "float y", "double y"), "0 1 0",
                                             "0 1e-9999 0")),
               "invalid-ply");
    const auto zeros =
        check_mesh(directory, replace(triangle, "0 1 0\n", "-0e-9999 1 +.0E+9999\n"));
    const auto vertex = success(zeros).geometry.meshes.front().positions[2];
    require(vertex.x == 0 && std::signbit(vertex.x) && vertex.z == 0 && !std::signbit(vertex.z),
            "Exact decimal zero preserves sign with extreme exponents");
}

void bindings(const fs::path &directory) {
    const auto asset = write(directory / "mesh.ply", triangle_ply());
    write(directory / "nested" / "mesh.ply", "Not the asset");
    const auto child = write(directory / "nested" / "shape.pbrt", R"(ObjectBegin "fixture"
Translate 1 0 0
ReverseOrientation
Shape "plymesh" "string filename" "mesh.ply"
ObjectEnd
)");
    const auto entry = write(directory / "scene.pbrt", R"(WorldBegin
Include "nested/shape.pbrt"
Material "diffuse" "rgb reflectance" [.25 .5 .75]
Shape "plymesh" "string filename" "missing-override.ply" "string filename" "mesh.ply"
  "float edgelength" -1 "texture displacement" ""
Translate 0 2 0
ObjectInstance "fixture"
Translate 0 2 0
ObjectInstance "fixture"
)");
    auto result = check(entry);
    const auto &scene = success(result);
    require(result.document.dependencies.size() == 2 &&
                result.document.dependencies[1].resolved_path == asset &&
                *result.document.dependencies[1].location.file == child.string(),
            "Entry-root resolution and dependency source in included file");
    require(scene.objects[0].shapes[0].mesh_index == 0 &&
                scene.objects[0].shapes[0].reverse_orientation && scene.shapes[0].mesh_index == 1 &&
                scene.shapes[0].material.inline_material && !scene.shapes[0].reverse_orientation &&
                scene.geometry.instances[0].mesh_index == 1 &&
                scene.geometry.instances[1].mesh_index == 0 &&
                scene.geometry.instances[1].transform.values[3] == 1 &&
                scene.geometry.instances[1].transform.values[7] == 2 &&
                scene.geometry.instances[2].transform.values[7] == 4,
            "Shared object mesh, distinct direct mesh and graphics state");

    const auto inherited = check(write(directory / "inherited.pbrt", R"(WorldBegin
Attribute "shape" "string filename" "mesh.ply"
Shape "plymesh"
)"));
    success(inherited);
    require(inherited.document.dependencies.front().location.line == 2,
            "Inherited filename retains its authored location");

    const auto alias = check(write(directory / "path-alias.pbrt",
                                   std::string(direct_scene) +
                                       "Shape \"plymesh\" \"string filename\" \"./mesh.ply\"\n"));
    require(success(alias).geometry.meshes.size() == 2 && alias.document.dependencies.size() == 2,
            "Decode cache uses the exact resolved path without normalization");

    const auto with_parameter = [&](std::string_view parameter) {
        return check(write(directory / "parameter.pbrt",
                           std::string(direct_scene) + std::string(parameter) + "\n"));
    };
    for (const auto invalid_parameter : {
             R"("string filename" ["mesh.ply" "extra.ply"])"sv,
             R"("float filename" 1)"sv,
             R"("float displacement" 1)"sv,
             R"("float edgelength" [1 2])"sv,
         }) {
        diagnostic(with_parameter(invalid_parameter), "invalid-parameter");
    }
    const auto empty = with_parameter(R"("string filename" "")");
    diagnostic(empty, "missing-parameter");
    require(!empty.document.dependencies_complete, report(empty));
    const auto missing = with_parameter(R"("string filename" "missing.ply")");
    diagnostic(missing, "missing-file");
    require(!missing.document.dependencies.front().exists &&
                !missing.document.dependencies_complete,
            report(missing));
    diagnostic(with_parameter(R"("float alpha" 1)"), "unsupported-parameter", 2);
    diagnostic(with_parameter(R"("texture alpha" "absent" "float alpha" 1)"),
               "undefined-reference");
    diagnostic(with_parameter(R"("float alpha" 1 "texture alpha" "absent")"),
               "undefined-reference");
    diagnostic(with_parameter(R"("texture alpha" "")"), "unsupported-parameter", 2);
    const auto empty_alpha =
        with_parameter(R"("texture alpha" "shadowed" "texture alpha" "" "float alpha" 1)");
    diagnostic(empty_alpha, "unsupported-parameter", 2);
    const auto inherited_alpha = check(write(directory / "inherited-alpha.pbrt", R"(WorldBegin
Attribute "shape" "texture alpha" "absent"
Shape "plymesh" "string filename" "mesh.ply" "float alpha" 1
)"));
    const auto &alpha_issue = diagnostic(inherited_alpha, "undefined-reference");
    require(alpha_issue.location.line == 2 &&
                alpha_issue.message.find("float texture \"absent\"") != std::string::npos,
            report(inherited_alpha));
    diagnostic(with_parameter(R"("texture displacement" "absent")"), "undefined-reference");
    diagnostic(check(write(directory / "spectral-displacement.pbrt", R"(WorldBegin
Texture "d" "spectrum" "constant" "rgb value" [.25 .5 .75]
Shape "plymesh" "string filename" "mesh.ply" "texture displacement" "d"
)")),
               "undefined-reference");
    diagnostic(check(write(directory / "displaced.pbrt", R"(WorldBegin
Texture "d" "float" "constant" "float value" .25
Shape "plymesh" "string filename" "mesh.ply" "texture displacement" "d"
)")),
               "unsupported-parameter", 2);

    const auto lit = check(write(directory / "emitting-object.pbrt", R"(WorldBegin
ObjectBegin "fixture"
AreaLightSource "diffuse" "rgb L" [1 1 1]
Shape "plymesh" "string filename" "mesh.ply"
ObjectEnd
ObjectInstance "fixture"
)"));
    require(diagnostic(lit, "unsupported-feature", 2).message.find("object definitions") !=
                std::string::npos,
            report(lit));

    write(asset, "broken\n");
    result = check(entry);
    const auto &problem = diagnostic(result, "invalid-ply");
    require(*problem.location.file == child.string() && problem.location.line == 4 &&
                result.document.diagnostics.size() == 1 && !result.document.dependencies[1].visited,
            report(result));

    const auto skipped = check(write(directory / "skipped.pbrt", R"(WorldBegin
Include "missing.pbrt"
Shape "plymesh" "string filename" "mesh.ply"
)"));
    diagnostic(skipped, "missing-file");
    require(skipped.document.diagnostics.size() == 1 && skipped.document.dependencies.size() == 2 &&
                skipped.document.dependencies[1].exists &&
                !skipped.document.dependencies[1].visited,
            report(skipped));

    write(asset, triangle_ply());
    write(directory / "map.exr", "unchecked");
    const auto incomplete = check(write(directory / "incomplete.pbrt", R"(WorldBegin
Texture "t" "spectrum" "imagemap" "string filename" "map.exr"
Shape "plymesh" "string filename" "mesh.ply"
)"));
    require(incomplete.exit_code() == 2 && !incomplete.document.dependencies_complete &&
                !incomplete.document.dependencies[0].visited &&
                incomplete.document.dependencies[1].visited,
            report(incomplete));
}

void bounds(const fs::path &directory) {
    const auto triangle = triangle_ply();
    Limits limits;
    limits.file_bytes = triangle.size() - 1;
    diagnostic(check_mesh(directory, triangle, limits), "limit-exceeded");
    limits = {};
    limits.expanded_bytes = triangle.size() + direct_scene.size() - 1;
    diagnostic(check_mesh(directory, triangle, limits), "limit-exceeded");
    limits.expanded_bytes++;
    success(check_mesh(directory, triangle, limits));
    limits = {};
    limits.expansions = 1;
    diagnostic(check_mesh(directory, triangle, limits), "limit-exceeded");
    limits = {};
    limits.directives = 8;
    diagnostic(check_mesh(directory, triangle, limits), "limit-exceeded");
    limits = {};
    limits.token_bytes = 20;
    diagnostic(
        check_mesh(directory, replace(triangle, "0 0 0", std::string(40, '0') + " 0 0"), limits),
        "limit-exceeded");
    diagnostic(check_mesh(directory, replace(triangle, "float x", "float " + std::string(40, 'x')),
                          limits),
               "limit-exceeded");
    diagnostic(
        check_mesh(directory, replace(triangle, "element vertex",
                                      "comment " + std::string(std::size_t{256} * 1024, 'x') +
                                          "\nelement vertex")),
        "limit-exceeded");
    diagnostic(check_mesh(directory, replace(triangle, "vertex 3", "vertex 4000000000")),
               "limit-exceeded");
    diagnostic(check_mesh(directory, replace(triangle, "vertex 3", "vertex 100000")),
               "invalid-ply");
    diagnostic(check_mesh(directory, replace(triangle, "element vertex 3",
                                             "element junk 1000001\n"
                                             "property char a\nproperty char b\n"
                                             "element vertex 3")),
               "limit-exceeded");
    diagnostic(
        check_mesh(directory, replace(replace(triangle, "element vertex 3",
                                              "element extra 1\nproperty list uint float items\n"
                                              "element vertex 3"),
                                      "end_header\n", "end_header\n4294967295\n")),
        "limit-exceeded");
    diagnostic(
        check_mesh(directory, replace(replace(triangle, "element vertex 3",
                                              "element extra 1\nproperty list char float items\n"
                                              "element vertex 3"),
                                      "end_header\n", "end_header\n-1\n")),
        "invalid-ply");
    limits = {};
    limits.list_values = 2;
    diagnostic(check_mesh(directory, triangle, limits), "limit-exceeded");
    limits = {};
    limits.total_values = 12;
    diagnostic(check_mesh(directory, triangle, limits), "limit-exceeded");
    limits.total_values = 13; // 9 coordinates, 1 count and 3 indices.
    success(check_mesh(directory, triangle, limits));

    write(directory / "mesh.ply", triangle);
    const auto repeated =
        write(directory / "repeated.pbrt", std::string(direct_scene) + std::string(shape) + "\n");
    limits.expanded_bytes = fs::file_size(repeated) + triangle.size();
    limits.expansions = 2;
    limits.total_values = 24; // Two mesh copies, only one PLY decode.
    require(success(check(repeated, limits)).geometry.meshes.size() == 2, "Bounded decode cache");
    limits.total_values--;
    require(diagnostic(check(repeated, limits), "limit-exceeded")
                    .message.find("Translated mesh value limit") != std::string::npos,
            "Repeated geometry storage is charged separately");

    write(directory / "sub" / "mesh.ply", triangle);
    const auto separators = write(directory / "separators.pbrt", R"(WorldBegin
Shape "plymesh" "string filename" "sub/mesh.ply"
Shape "plymesh" "string filename" "sub//mesh.ply"
)");
    limits.expanded_bytes = fs::file_size(separators) + triangle.size();
    limits.total_values = 24;
    const auto deduplicated = check(separators, limits);
    require(success(deduplicated).geometry.meshes.size() == 2 &&
                deduplicated.document.dependencies.size() == 1,
            "Redundant path separators reuse one bounded decode");

    write(directory / "other.ply", triangle);
    const auto two_paths =
        write(directory / "two-paths.pbrt",
              std::string(direct_scene) + "Shape \"plymesh\" \"string filename\" \"other.ply\"\n");
    limits.expanded_bytes = fs::file_size(two_paths) + triangle.size() * 2;
    limits.expansions = 3;
    limits.total_values = 25;
    diagnostic(check(two_paths, limits), "limit-exceeded");
    limits.total_values = 26;
    success(check(two_paths, limits));
}

} // namespace

int main(int argc, char *argv[]) {
    try {
        require(argc == 2, "Expected a temporary test directory argument");
        const auto directory = fs::absolute(argv[1]);
        ascii(directory / "ascii");
        binary(directory / "binary");
        invalid(directory / "invalid");
        bindings(directory / "bindings");
        bounds(directory / "bounds");
        return 0;
    } catch (const std::exception &exception) {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
