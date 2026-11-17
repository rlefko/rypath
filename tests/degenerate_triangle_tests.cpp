// SPDX-License-Identifier: GPL-3.0-only
#include "analytic.hpp"
#include "metal_geometry.hpp"
#include "pbrt_render.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <unistd.h>
namespace {
using namespace rypath;
void require(bool test, const std::string &message) {
    if (!test)
        throw std::runtime_error(message);
}
GeometryData geometry() {
    GeometryMesh mesh;
    mesh.positions = {{.25f, .25f, 2}, {0, 0, 1}, {1, 1, 1}, {2, 2, 1},
                      {0, 0, 0},       {1, 0, 0}, {0, 1, 0}};
    mesh.triangles = {{0, 0, 0}, {1, 2, 3}, {4, 5, 6}};
    return {{mesh}, {{0, {}}}};
}
struct Scratch {
    std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                ("rypath-zero-triangle-" + std::to_string(getpid()));
    Scratch() { std::filesystem::create_directories(dir); }
    ~Scratch() { std::filesystem::remove_all(dir); }
    pbrt::Result check(const std::string &body) {
        auto path = dir / "scene.pbrt";
        std::ofstream(path) << "Camera \"perspective\"\nFilm \"rgb\"\nSampler "
                               "\"independent\"\nIntegrator \"path\"\nWorldBegin\n"
                            << body;
        return pbrt::check(path);
    }
};
void portable() {
    auto g = geometry();
    require(validate_geometry(g).empty(), "retained zero-measure geometry validates");
    require(!triangle_has_area(g.meshes[0].positions[0], g.meshes[0].positions[0],
                               g.meshes[0].positions[0]),
            "repeated vertex classification");
    require(!triangle_has_area(g.meshes[0].positions[1], g.meshes[0].positions[2],
                               g.meshes[0].positions[3]),
            "collinear classification");
    std::array<double, 16> identity{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    std::vector<TextureTriangleFrame> frames;
    require(prepare_texture_triangle_frames(g.meshes[0], identity, identity, frames).empty(),
            "zero-measure dummy frames");
    require(frames.size() == 3 && frames[0].dpdu.x == 0 && frames[1].dpdv.y == 0 &&
                frames[2].dpdu.x != 0,
            "dummy frames preserve primitive ordinals");
    auto bad = g;
    bad.meshes[0].positions[0].x = std::numeric_limits<float>::infinity();
    require(!validate_geometry(bad).empty(), "finite checks remain active");
    bad = g;
    bad.meshes[0].triangles[0][1] = 999;
    require(!validate_geometry(bad).empty(), "indices remain checked");
    Scratch scratch;
    const std::string shape = "Shape \"trianglemesh\" \"point3 P\" [.25 .25 2 0 0 1 1 1 1 2 2 1 0 "
                              "0 0 1 0 0 0 1 0] \"integer indices\" [0 0 0 1 2 3 4 5 6]\n";
    auto checked = scratch.check("Material \"diffuse\" \"float displacement\" [0]\n" + shape);
    if (checked.exit_code() != 0 || !checked.scene)
        throw std::runtime_error(pbrt::report(checked));
    require(checked.inventory.triangles == 3 &&
                checked.scene->geometry.meshes[0].triangles == g.meshes[0].triangles,
            "source triangles and IDs retained");
    require(checked.document.diagnostics.size() == 1 &&
                checked.document.diagnostics[0].code == "zero-measure-triangles" &&
                checked.document.diagnostics[0].severity == pbrt::Severity::warning,
            "source-located zero-measure warning");
    auto translated = pbrt::translate(checked, {});
    std::string why;
    for (const auto &d : translated.diagnostics)
        why += d.message + "\n";
    if (translated.exit_code() != 0 || !translated.scene)
        throw std::runtime_error(why);
    require(validate_render_scene(*translated.scene).empty(), "zero-measure render validation");
    checked = scratch.check("AreaLightSource \"diffuse\"\n" + shape);
    translated = pbrt::translate(checked, {});
    require(translated.exit_code() == 0 && translated.scene.has_value(),
            "zero-measure radiance emitter keeps null sampling slots");
    checked = scratch.check("AreaLightSource \"diffuse\" \"float power\" [1]\n" + shape);
    translated = pbrt::translate(checked, {});
    require(translated.exit_code() != 0 && !translated.scene,
            "zero-measure positive power rejected");
    // The source double-precision face has nonzero area, but vertex conversion
    // merges its first two points. Classification is on retained binary32 data.
    std::ofstream(scratch.dir / "collapse.ply")
        << "ply\nformat ascii 1.0\nelement vertex 3\nproperty double x\nproperty double "
           "y\nproperty double z\nelement face 1\nproperty list uchar int "
           "vertex_indices\nend_header\n16777216 0 0\n16777217 0 0\n16777216 1 0\n3 0 1 2\n";
    checked = scratch.check("Shape \"plymesh\" \"string filename\" [\"collapse.ply\"]\n");
    require(checked.exit_code() == 0 && checked.scene && checked.inventory.triangles == 1,
            "PLY collapsed face retained");
    require(checked.document.diagnostics.size() == 1 &&
                checked.document.diagnostics[0].message.find("after binary32 conversion") !=
                    std::string::npos,
            "PLY conversion warning");
    std::cout << "Retained zero-measure triangle portable checks passed\n";
}
int native(const std::filesystem::path &library) {
    for (const GeometryTransform xf :
         {GeometryTransform{}, GeometryTransform{{-2, .5f, 0, 4, 0, 3, 0, -2, 0, 0, .5f, 1}}}) {
        auto g = geometry();
        const float huge = std::ldexp(1.f, 24);
        auto &mesh = g.meshes[0];
        mesh.positions.insert(mesh.positions.end(), {{huge, huge, 1}, {1, 1, 1}, {2, 2, 1}});
        mesh.triangles.push_back({7, 8, 9});
        require(!triangle_has_area(mesh.positions[7], mesh.positions[8], mesh.positions[9]),
                "collinear cancellation stress classification");
        g.instances[0].transform = xf;
        auto built = build_metal_geometry(g, library);
        if (built.status == GeometryStatus::no_device) {
#ifdef RYPATH_ALLOW_NO_METAL_DEVICE
            return 77;
#endif
        }
        if (built.status == GeometryStatus::no_ray_tracing) {
#ifdef RYPATH_ALLOW_NO_METAL_RAY_TRACING
            return 77;
#endif
        }
        require(built.status == GeometryStatus::success, built.diagnostic);
        const auto &m = xf.values;
        const auto transform = [&](GeometryPoint p, bool v) {
            return GeometryPoint{m[0] * p.x + m[1] * p.y + m[2] * p.z + (v ? 0 : m[3]),
                                 m[4] * p.x + m[5] * p.y + m[6] * p.z + (v ? 0 : m[7]),
                                 m[8] * p.x + m[9] * p.y + m[10] * p.z + (v ? 0 : m[11])};
        };
        const std::array<GeometryRay, 3> rays{
            {{transform({.25f, .25f, 3}, false), transform({0, 0, -1}, true)},
             {transform({2, 2, 3}, false), transform({0, 0, -1}, true)},
             {transform({1.25f, 1.25f, 3}, false), transform({.25f, .25f, -1}, true)}}};
        const auto hit = built.scene->closest_hits(rays);
        const auto occ = built.scene->occlusion(rays);
        require(hit.status == GeometryStatus::success && occ.status == GeometryStatus::success,
                hit.diagnostic + occ.diagnostic);
        require(hit.hits[0].hit && hit.hits[0].primitive_index == 2 &&
                    std::abs(hit.hits[0].t - 3) < 1e-5,
                "zero-measure faces never mask stable valid primitive ID");
        require(!hit.hits[1].hit && !hit.hits[2].hit &&
                    occ.occluded == std::vector<std::uint8_t>{1, 0, 0},
                "zero-measure closest/occlusion misses");
    }
    auto zero = geometry();
    zero.meshes[0].triangles.resize(2);
    auto built = build_metal_geometry(zero, library);
    require(built.status == GeometryStatus::success, built.diagnostic);
    const std::array<GeometryRay, 1> rays{{{{.25f, .25f, 3}, {0, 0, -1}}}};
    const auto empty_hit = built.scene->closest_hits(rays);
    const auto empty_occ = built.scene->occlusion(rays);
    require(empty_hit.status == GeometryStatus::success && !empty_hit.hits[0].hit &&
                empty_occ.status == GeometryStatus::success && !empty_occ.occluded[0],
            "all-zero-measure mesh builds and remains nonhittable");
    std::cout << "Retained zero-measure native triangle checks passed\n";
    return 0;
}
} // namespace
int main(int argc, char **argv) {
    try {
        if (argc > 1 && std::string_view(argv[1]) == "--native")
            return native(argc > 2 ? argv[2] : "");
        portable();
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
