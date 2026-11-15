// SPDX-License-Identifier: GPL-3.0-only
#include "obj.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {
using namespace rypath::obj;
namespace fs = std::filesystem;
constexpr std::string_view vertices = "v 0 0 0\nv 1 0 0\nv 0 1 0\n";
void require(bool b, std::string_view message) {
    if (!b)
        throw std::runtime_error(std::string(message));
}
void write(const fs::path &path, std::string_view text) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << text;
    out.close();
    require(out.good(), "Fixture write failed");
}
Result check(const fs::path &dir, std::string_view text, const Limits &limits = {}) {
    write(dir / "mesh.obj", text);
    return load(dir / "mesh.obj", limits);
}
const Asset &success(const Result &r) {
    if (r.exit_code() != 0 || !r.asset)
        throw std::runtime_error(report(r));
    return r.asset.value();
}
void failure(const Result &r, std::string_view code, int exit = 1) {
    require(!r.asset && r.exit_code() == exit, report(r));
    require(std::ranges::any_of(r.diagnostics,
                                [&](const auto &d) {
                                    return d.code == code && d.location.line > 0 &&
                                           !d.location.file->empty();
                                }),
            report(r));
}
void indices(const fs::path &dir) {
    const auto r =
        check(dir, std::string(vertices) + "vt .25 .75\nvt .5 .5\nvt 0 0\nvn 0 0 4\no part\ng "
                                           "front body\ns 7\nf -3/-1/-1 -2/-3/-1 -1/-2/-1\n");
    const auto &a = success(r);
    const auto &p = a.polygons[0];
    require(p.corners[0].position == 0 && p.corners[0].uv == 2 && p.corners[1].uv == 0 &&
                p.corners[2].uv == 1,
            "Independent relative attributes");
    require(p.object == "part" && p.groups == std::vector<std::string>{"front", "body"} &&
                p.smoothing == 7,
            "Authored metadata");
    require(a.parts[0].complete_uvs && a.parts[0].geometry.uvs[0][0] == 0 &&
                a.parts[0].geometry.normals[0].z == 1,
            "UV remapping and normal normalization");
    require(a.parts[0].triangle_polygons == std::vector<std::size_t>{0}, "Triangle provenance");
    const auto after = check(dir, std::string(vertices) + "f -3 -2 -1\nv 4 5 6\n");
    require(success(after).polygons[0].corners[0].position == 0,
            "Relative indices resolve at record");
    const auto missing = check(dir, std::string(vertices) + "vn 0 0 1\nf 1//1 2 3\n");
    require(!success(missing).polygons[0].corners[1].normal &&
                !success(missing).parts[0].complete_uvs,
            "Mixed omitted attributes remain authored");
}
void materials(const fs::path &dir) {
    write(dir / "sub/library.mtl", "# raw values preserved\nnewmtl paint\nKd .2 .3 .4\nmap_Kd -s 2 "
                                   "3 -o 1 0 \"tex name.png\"\nPr .25\n");
    const auto r =
        check(dir, std::string(vertices) +
                       "usemtl paint\nf 1 2 3\nmtllib sub/library.mtl\nmtllib sub/library.mtl\n");
    const auto &a = success(r);
    require(a.sources.size() == 2 && a.materials.size() == 1 && a.polygons[0].material == 0,
            "Forward material resolution and unique snapshot");
    require(a.materials[0].properties[1].arguments.back() == "tex name.png" &&
                a.materials[0].properties[1].location.line == 4,
            "MTL raw map option provenance");
    failure(check(dir, std::string(vertices) + "usemtl absent\nf 1 2 3\n"), "obj-material");
    write(dir / "duplicate.mtl", "newmtl paint\nKd 1 1 1\nnewmtl paint\n");
    failure(check(dir, std::string(vertices) + "mtllib duplicate.mtl\nf 1 2 3\n"), "obj-material");
    failure(check(dir, std::string(vertices) + "mtllib missing.mtl\nf 1 2 3\n"), "obj-file");
    write(dir / "orphan.mtl", "Kd .5\n");
    failure(check(dir, std::string(vertices) + "mtllib orphan.mtl\nf 1 2 3\n"), "obj-material");
}
void polygons(const fs::path &dir) {
    const std::string concave = "v 0 0 0\nv 2 0 0\nv 2 2 0\nv 1 1 0\nv 0 2 0\nf 1 2 3 4 5\n";
    const auto r = check(dir, concave);
    const auto &m = success(r).parts[0].geometry;
    require(m.triangles.size() == 3 && m.positions.size() == 5, "Concave triangulation");
    double area = 0;
    for (const auto &t : m.triangles) {
        const auto &a = m.positions[t[0]], &b = m.positions[t[1]], &c = m.positions[t[2]];
        area += ((b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x)) / 2;
    }
    require(area == 3, "Independent concave area oracle");
    auto reverse = concave;
    reverse.replace(reverse.find("f 1"), std::string::npos, "f 5 4 3 2 1\n");
    const auto reversed = check(dir, reverse);
    require(success(reversed).parts[0].geometry.normals[0].z == -1, "Clockwise winding retained");
    const auto collinear = check(dir, "v 0 0 0\nv 1 0 0\nv 2 0 0\nv 2 1 0\nv 0 1 0\nvt 0 0\nvt .2 "
                                      "0\nvt .4 0\nvt 1 1\nvt 0 1\nf 1/1 2/2 3/3 4/4 5/5\n");
    const auto &c = success(collinear).parts[0].geometry;
    require(c.triangles.size() == 3 && c.uvs[1][0] == .2F,
            "Collinear corner and authored UV retained");
    require(std::ranges::any_of(c.triangles,
                                [](const auto &t) { return std::ranges::find(t, 1U) != t.end(); }),
            "Collinear vertex participates in triangles");
    failure(check(dir, "v 0 0 0\nv 1 1 0\nv 0 1 0\nv 1 0 0\nf 1 2 3 4\n"), "obj-polygon");
    failure(check(dir, std::string(vertices) + "f 1 2 2 3\n"), "obj-polygon");
    failure(check(dir, "v 0 0 0\nv 1 0 0\nv 1 1 .1\nv 0 1 0\nf 1 2 3 4\n"), "obj-planarity");
    failure(check(dir, "v 16777216 0 0\nv 16777217 0 0\nv 16777216 1 0\nf 1 2 3\n"), "obj-polygon");
}
void smoothing(const fs::path &dir) {
    const std::string base = "v 0 0 0\nv 1 0 0\nv 0 1 0\nv 0 0 2\nvn 1 0 0\ns 3\no a\ng one\nf "
                             "1//1 2 3\no b\ng two\nf 2 1 4\n";
    const auto r = check(dir, base);
    const auto &a = success(r);
    const auto &m = a.parts[0].geometry;
    require(a.parts[0].polygons.size() == 2 &&
                a.parts[0].triangle_polygons == std::vector<std::size_t>{0, 1},
            "Group/material coalescing preserves polygon provenance");
    require(m.normals[0].x == 1 && m.normals[0].y == 0, "Explicit normal overrides smoothing");
    const auto expected_y = 2 / std::sqrt(5.0), expected_z = 1 / std::sqrt(5.0);
    require(std::abs(m.normals[1].y - expected_y) < 1e-6 &&
                std::abs(m.normals[1].z - expected_z) < 1e-6,
            "Area-weighted smoothing crosses object/group changes");
    require(std::abs(m.normals[4].y - expected_y) < 1e-6,
            "Explicit-normal face contributes to missing-normal synthesis");
    auto flat = base;
    flat.replace(flat.find("s 3"), 3, "s off");
    const auto f = check(dir, flat);
    require(success(f).parts[0].geometry.normals[1].z == 1 &&
                success(f).parts[0].geometry.normals[4].y == 1,
            "s off creates flat original-face normals");
    const auto partition = check(dir, std::string(vertices) + "vt 0 0\nf 1/1 2/1 3/1\nf 1 2 3\n");
    require(success(partition).parts.size() == 2,
            "UV completeness partitions geometry without invented coordinates");
}
void invalid(const fs::path &dir) {
    for (const auto suffix : {"f 0 2 3\n", "f 1 2 4\n", "f -4 -2 -1\n", "f 1/ 2 3\n", "f 1// 2 3\n",
                              "f 1/0 2 3\n", "f 1//0 2 3\n", "f 1/1 2 3\n", "f 1//1 2 3\n",
                              "f 1/2/3/4 2 3\n", "f 1x 2 3\n", "f 9223372036854775808 2 3\n"})
        failure(check(dir, std::string(vertices) + suffix), "obj-index");
    for (const auto token : {"nan", "inf", "1e", "1e999", "1e-999", "0x1p0", "1junk"})
        failure(check(dir, "v " + std::string(token) + " 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n"),
                "obj-number");
    failure(check(dir, "v 0 0\nf 1 2 3\n"), "obj-cardinality");
    failure(check(dir, std::string(vertices) + "vn 0 0 0\nf 1//1 2//1 3//1\n"), "obj-normal");
    failure(check(dir, std::string(vertices) + "unknown 1\nf 1 2 3\n"), "obj-directive");
    for (const auto op :
         {"p 1", "l 1 2", "csh touch SHOULD_NOT_EXIST", "call missing.obj", "curv 0 1 1 2"})
        failure(check(dir, std::string(vertices) + op + "\nf 1 2 3\n"), "obj-unsupported", 2);
    require(!fs::exists(dir / "SHOULD_NOT_EXIST"), "Commands never execute");
    const auto bom =
        check(dir, "\xef\xbb\xbfv 0 0 0\r\nv 1 0 0\r\nv 0 1 0\r\nf 1 2 \\\r\n3 # comment\r\n");
    success(bom);
    const auto subnormal =
        check(dir, "v 0 0 0\nv 1 0 0\nv 0 1 0\nvt 1.401298464324817e-45 0\nf 1/1 2/1 3/1\n");
    require(success(subnormal).parts[0].geometry.uvs[0][0] > 0,
            "Scientific binary32 subnormal survives both decoding passes");
    failure(check(dir, "v 1e-320 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n"), "obj-number");
}
void native_domain(const fs::path &dir) {
    for (const auto normal : {"1 1e-40 0", "1 1e-50 0", "1 1.401298464324817e-45 0",
                              "1 1.1754943508222875e-38 0", "1e308 1e-308 0"}) {
        const auto result =
            check(dir, std::string(vertices) + "vn " + normal + "\nf 1//1 2//1 3//1\n");
        failure(result, "obj-normal");
    }
    // Native common scaling puts this component exactly at FLT_MIN.
    const auto minimum =
        check(dir, std::string(vertices) + "vn 1 2.350988701644575e-38 0\nf 1//1 2//1 3//1\n");
    success(minimum);
    const auto diagonal_minimum =
        check(dir, std::string(vertices) +
                       "vn .7071067811865476 .7071067811865476 1.1754943508222875e-38\n"
                       "f 1//1 2//1 3//1\n");
    success(diagonal_minimum);
    // Synthesized geometric and smoothing normals must obey the same domain.
    for (const auto smoothing : {"off", "1"}) {
        const auto result = check(dir, std::string("v 0 0 0\nv 1 0 0\nv 0 1 1e-40\ns ") +
                                           smoothing + "\nf 1 2 3\n");
        failure(result, "obj-normal");
    }
    failure(check(dir, "v 0 0 0\nv 1 0 0\nv 0 10000000000 1.401298464324817e-45\n"
                       "f 1 2 3\n"),
            "obj-normal");
    failure(check(dir, "v 0 0 10000000000\nv 1000 0 10000000600\n"
                       "v 1000 1000 10000001200\nv 0 1000 10000000600\nf 1 2 3 4\n"),
            "obj-polygon");
    failure(check(dir, "v 0 0 10000000000\nv 1000 0 10000000600\n"
                       "v 0 1000 10000000600\nf 1 2 3\n"),
            "obj-polygon");
}
void bounds(const fs::path &dir) {
    const auto data = std::string(vertices) + "f 1 2 3\n";
    Limits l;
    l.input.file_bytes = 10;
    failure(check(dir, data, l), "obj-limit");
    l = {};
    l.input.expanded_bytes = 10;
    failure(check(dir, data, l), "obj-limit");
    l = {};
    l.input.expansions = 0;
    failure(check(dir, data, l), "obj-limit");
    l = {};
    l.input.total_values = 2;
    failure(check(dir, data, l), "obj-limit");
    l = {};
    l.input.directives = 2;
    failure(check(dir, data, l), "obj-limit");
    l = {};
    l.input.token_bytes = 1;
    failure(check(dir, "v 00 0 0\n" + data, l), "obj-limit");
    l = {};
    l.face_corners = 2;
    failure(check(dir, data, l), "obj-limit");
    l = {};
    l.polygon_work = 8;
    failure(check(dir, data, l), "obj-limit");
    l = {};
    l.working_bytes = 128;
    failure(check(dir, data, l), "obj-limit");
    l = {};
    l.output_bytes = 1;
    failure(check(dir, data, l), "obj-limit");
    l = {};
    l.input.diagnostics = 0;
    failure(check(dir, data, l), "obj-limit");
}
void external() {
    const auto r = load(fs::path(RYPATH_SOURCE_DIR) / "tests/fixtures/obj/external/cube.obj");
    const auto &a = success(r);
    std::size_t triangles = 0;
    for (const auto &p : a.parts)
        triangles += p.geometry.triangles.size();
    require(a.polygons.size() == 6 && a.materials.size() == 5 && a.sources.size() == 2 &&
                triangles == 12,
            "Pinned external cube coverage");
    require(a.polygons[1].groups == std::vector<std::string>{"back", "cube"} &&
                a.polygons[1].material == a.polygons[0].material,
            "External multi-groups and material persistence");
}
} // namespace
int main() {
    fs::path dir;
    try {
        dir = fs::temp_directory_path() /
              ("rypath-obj-" +
               std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        indices(dir / "indices");
        materials(dir / "materials");
        polygons(dir / "polygons");
        smoothing(dir / "smoothing");
        invalid(dir / "invalid");
        native_domain(dir / "native-domain");
        bounds(dir / "bounds");
        external();
        std::error_code cleanup_error;
        fs::remove_all(dir, cleanup_error);
        require(!cleanup_error, "Could not clean OBJ test scratch directory");
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        std::error_code cleanup_error;
        fs::remove_all(dir, cleanup_error);
        return 1;
    }
}
