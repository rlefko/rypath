// SPDX-License-Identifier: GPL-3.0-only
#include "pbrt_render.hpp"
#include "render_identity.hpp"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <unistd.h>

namespace {
using namespace rypath;
namespace fs = std::filesystem;
void require(bool condition, const std::string &message) {
    if (!condition)
        throw std::runtime_error(message);
}
constexpr auto options = "Camera \"perspective\"\nFilm \"rgb\"\nPixelFilter \"box\"\n"
                         "Sampler \"independent\"\nIntegrator \"volpath\"\nWorldBegin\n";
constexpr auto quad = "Shape \"bilinearmesh\" \"point3 P\" [-1 -1 3 1 -1 3 -1 1 3 1 1 3] ";
constexpr auto loop = "Shape \"loopsubdiv\" \"point3 P\" [0 0 3 2 0 3 0 2 3] "
                      "\"integer indices\" [0 1 2] ";
template <typename T> const T &checked_value(const std::optional<T> &value) {
    if (!value)
        throw std::runtime_error("Expected an owned optional result");
    return *value;
}
struct Scratch {
    fs::path dir = fs::temp_directory_path() / ("rypath-pbrt-mesh-" + std::to_string(getpid()));
    Scratch() { fs::create_directories(dir); }
    ~Scratch() { fs::remove_all(dir); }
    fs::path write(const std::string &body) const {
        const auto path = dir / "scene.pbrt";
        std::ofstream(path) << options << body << '\n';
        return path;
    }
};
pbrt::Result checked(const Scratch &tmp, const std::string &body) {
    auto result = pbrt::check(tmp.write(body));
    require(result.exit_code() == 0 && result.scene.has_value(), pbrt::report(result));
    return result;
}
pbrt::RenderTranslation translated(const pbrt::Result &result) {
    auto render = pbrt::translate(result, {});
    std::string message = "Mesh translation failed";
    for (const auto &diagnostic : render.diagnostics)
        message += "\n" + diagnostic.message;
    require(render.exit_code() == 0 && render.scene.has_value(), message);
    require(validate_render_scene(checked_value(render.scene)).empty(), "Translated mesh invalid");
    return render;
}
void rejected(const Scratch &tmp, const std::string &body, int status = 1) {
    auto result = pbrt::check(tmp.write(body));
    require(result.exit_code() == status && !result.scene && !result.document.diagnostics.empty(),
            "Expected explicit rejection: " + body + "\n" + pbrt::report(result));
    require(std::ranges::any_of(
                result.document.diagnostics,
                [](const auto &d) { return d.location.line > 6 && !d.location.file->empty(); }),
            "Mesh rejection lost source location");
}
void bilinear(const Scratch &tmp) {
    const auto result = checked(tmp, quad);
    require(result.inventory.patches == 1 && result.inventory.triangles == 0,
            "Bilinear must retain a patch rather than triangulate");
    const auto &mesh = checked_value(result.scene).geometry.meshes.at(0);
    require(mesh.patches == std::vector<std::array<std::uint32_t, 4>>{{0, 1, 2, 3}},
            "Default tensor-product corner order");
    const auto explicit_result = checked(tmp, std::string(quad) + "\"integer indices\" [0 1 2 3]");
    const auto a = translated(result), b = translated(explicit_result);
    require(render_identity(checked_value(a.scene), a.settings) ==
                render_identity(checked_value(b.scene), b.settings),
            "Implicit and explicit patch indices changed scene identity");
    const auto attributes =
        checked(tmp, std::string(quad) + "\"normal N\" [0 0 -1 0 0 -1 0 0 -1 0 0 -1] "
                                         "\"point2 uv\" [0 0 1 0 0 1 1 1]");
    require(checked_value(attributes.scene).geometry.meshes[0].normals.size() == 4 &&
                checked_value(attributes.scene).geometry.meshes[0].uvs.size() == 4,
            "Patch normals and UVs must survive checking");
    translated(attributes);
    const auto curved =
        checked(tmp, "Shape \"bilinearmesh\" \"point3 P\" [0 0 3 2 0 3 0 2 3 2 2 4]");
    translated(curved);
    for (const auto &body :
         {"Shape \"bilinearmesh\"", "Shape \"bilinearmesh\" \"point3 P\" [0 0 0 1 0 0 0 1 0]",
          "Shape \"bilinearmesh\" \"point3 P\" [0 0 0 1 0 0 0 1 0 1 1 0 2 2 0]",
          "Shape \"bilinearmesh\" \"point3 P\" [0 0 0 1 0 0 0 1 0 1 1 0] \"integer indices\" [0 1 "
          "2]"})
        rejected(tmp, body);
    for (const auto &suffix : {"\"integer indices\" [0 1 2 4]", "\"integer indices\" [0 1 2 -1]",
                               "\"normal N\" [0 0 1]", "\"point2 uv\" [0 0]"})
        rejected(tmp, std::string(quad) + suffix);
    rejected(tmp, std::string(quad) + "\"integer faceIndices\" [0]", 2);
}
void subdivision(const Scratch &tmp) {
    for (int level : {0, 1, 2}) {
        auto result =
            checked(tmp, std::string(loop) + "\"integer levels\" [" + std::to_string(level) + "]");
        const auto &mesh = checked_value(result.scene).geometry.meshes.at(0);
        require(mesh.triangles.size() == (std::size_t{1} << (2 * level)) &&
                    mesh.normals.size() == mesh.positions.size(),
                "Loop refinement count or generated limit normals");
        require(std::ranges::all_of(mesh.positions, [](const auto &p) { return p.z == 3; }) &&
                    std::ranges::all_of(mesh.normals,
                                        [](const auto &n) {
                                            return std::abs(n.x) <= 2e-6F &&
                                                   std::abs(n.y) <= 2e-6F && std::isfinite(n.z) &&
                                                   n.z < 0;
                                        }),
                "Planar Loop limit surface or normal orientation");
        translated(result);
        if (level == 0) {
            // PBRT's boundary limit mask is 3/5 center + 1/5 each neighbor.
            require(std::abs(mesh.positions[0].x - .4F) < 1e-6F &&
                        std::abs(mesh.positions[0].y - .4F) < 1e-6F,
                    "Level zero must project to the limit, not keep control vertices");
        }
    }
    rejected(tmp, std::string(loop) + "\"integer nlevels\" [1]", 1);
    translated(
        checked(tmp, std::string(loop) + "\"string scheme\" [\"ignored\"] \"integer levels\" [0]"));
    for (const auto &suffix : {"\"normal N\" [0 0 1 0 0 1 0 0 1]", "\"point2 uv\" [0 0 1 0 0 1]",
                               "\"vector3 S\" [1 0 0 1 0 0 1 0 0]", "\"float displacement\" [1]"})
        rejected(tmp, std::string(loop) + suffix, 1);
    for (const auto &shape : {std::string(quad), std::string(loop) + "\"integer levels\" [0] "}) {
        const auto alpha_checked = checked(tmp, shape + "\"float alpha\" [.5]");
        const auto alpha_render = translated(alpha_checked);
        require(!checked_value(alpha_render.scene).instance_alpha_roots.empty() &&
                    checked_value(alpha_render.scene).instance_alpha_roots[0] != UINT32_MAX,
                "Mesh alpha must reach the nonopaque intersection path");
    }
    auto defaults = checked(tmp, loop);
    require(defaults.inventory.triangles == 64, "Default Loop level must be three");
    rejected(tmp, "Shape \"loopsubdiv\" \"point3 P\" [0 0 0 1 0 0 0 1 0]");
    for (const auto &indices : {"0 1", "0 1 3", "0 1 -1", "0 0 1"})
        rejected(tmp, "Shape \"loopsubdiv\" \"point3 P\" [0 0 3 2 0 3 0 2 3] "
                      "\"integer levels\" [1] \"integer indices\" [" +
                          std::string(indices) + "]");
    // Three faces sharing an edge cannot form an oriented two-manifold control mesh.
    rejected(tmp, "Shape \"loopsubdiv\" \"point3 P\" [0 0 3 2 0 3 0 2 3 0 -2 3 0 0 5] "
                  "\"integer levels\" [1] \"integer indices\" [0 1 2 1 0 3 0 1 4]");
    rejected(tmp, std::string(loop) + "\"integer levels\" [1000000]");
    for (int budget = 0; budget < 3; ++budget) {
        pbrt::Limits limits;
        if (budget == 0)
            limits.subdivision_scratch_bytes = 1;
        else if (budget == 1)
            limits.subdivision_work = 1;
        else
            limits.total_values = 64;
        const auto result =
            pbrt::check(tmp.write(std::string(loop) + "\"integer levels\" [3]"), limits);
        require(result.exit_code() == 1 && !result.scene && !result.document.diagnostics.empty(),
                "Bounded Loop generation must fail visibly without a partial scene");
    }
}
void state_and_ownership(const Scratch &tmp) {
    std::ofstream(tmp.dir / "part.pbrt") << "ReverseOrientation\n" << quad;
    auto result = checked(tmp, "Material \"diffuse\"\nObjectBegin \"surface\"\n"
                               "Include \"part.pbrt\"\nObjectEnd\nAttributeBegin\n"
                               "Translate 3 4 5\nScale -1 2 1\nObjectInstance \"surface\"\n"
                               "AttributeEnd\nObjectInstance \"surface\"\n");
    require(checked_value(result.scene).objects.size() == 1 &&
                checked_value(result.scene).placements.size() == 2,
            "Mesh object and placement inventory");
    require(checked_value(result.scene).objects[0].shapes[0].reverse_orientation,
            "Included reverse orientation lost");
    auto copy = result;
    result = {};
    fs::remove(tmp.dir / "part.pbrt");
    auto render = translated(copy);
    require(checked_value(render.scene).geometry.instances.size() == 2 &&
                checked_value(render.scene).instance_reverse_orientation ==
                    std::vector<std::uint32_t>{0, 1},
            "Owned mesh lost instance orientation");
    const auto &transform = checked_value(render.scene).geometry.instances[0].transform.values;
    require(transform[0] == -1 && transform[5] == 2 && transform[3] == 3 && transform[7] == 4 &&
                transform[11] == 5,
            "Instance transform lost");
    const auto identity = render_identity(checked_value(render.scene), render.settings);
    auto changed = checked_value(render.scene);
    changed.geometry.meshes[0].positions[0].z += .25F;
    require(render_identity(changed, render.settings) != identity,
            "Mesh position absent from identity");
    changed = checked_value(render.scene);
    changed.instance_reverse_orientation[0] = 1;
    require(render_identity(changed, render.settings) != identity,
            "Orientation absent from identity");
    std::ofstream(tmp.dir / "broken.pbrt") << quad << "\"integer indices\" [0 1 2 9]\n";
    auto bad = pbrt::check(tmp.write("Include \"broken.pbrt\"\n"));
    require(bad.exit_code() == 1 &&
                std::ranges::any_of(bad.document.diagnostics,
                                    [](const auto &d) {
                                        return d.location.file->ends_with("broken.pbrt") &&
                                               d.location.line == 1;
                                    }),
            "Included malformed mesh lost its file/line");
}
void integration_contracts(const Scratch &tmp) {
    for (bool is_loop : {false, true}) {
        const std::string shape = is_loop ? std::string(loop) + "\"integer levels\" [0] " : quad;
        const std::string inherited =
            is_loop ? "Attribute \"shape\" \"point3 P\" [0 0 3 2 0 3 0 2 3] \"integer indices\" [0 "
                      "1 2] \"integer levels\" [0]\nShape \"loopsubdiv\"\n"
                    : "Attribute \"shape\" \"point3 P\" [-1 -1 3 1 -1 3 -1 1 3 1 1 3]\nShape "
                      "\"bilinearmesh\"\n";
        const auto inherited_scene = translated(checked(tmp, inherited));
        const auto explicit_scene = translated(checked(tmp, shape));
        require(render_identity(checked_value(inherited_scene.scene), inherited_scene.settings) ==
                    render_identity(checked_value(explicit_scene.scene), explicit_scene.settings),
                "Inherited mesh parameters differ from explicit parameters");
        auto objects =
            translated(checked(tmp, "ObjectBegin \"mesh\"\n" + shape +
                                        "\nObjectEnd\nObjectInstance \"mesh\"\nTranslate 1 0 "
                                        "0\nObjectInstance \"mesh\"\n"));
        require(checked_value(objects.scene).geometry.meshes.size() == 1 &&
                    checked_value(objects.scene).geometry.instances.size() == 2 &&
                    checked_value(objects.scene).geometry.instances[1].mesh_index == 0,
                "Object placements must reuse generated mesh storage");
        const std::string medium = "MakeNamedMedium \"fog\" \"string type\" "
                                   "\"homogeneous\"\nMediumInterface \"fog\" \"\"\n";
        auto boundary = translated(checked(tmp, medium + shape));
        require(checked_value(boundary.scene).instance_media.at(0).inside == 0 &&
                    checked_value(boundary.scene).instance_media.at(0).outside == no_medium,
                "Mesh medium interface lost");
        const auto partial = checked(tmp, medium + shape + "\"float alpha\" [.5]");
        const auto rejected_alpha = pbrt::translate(partial, pbrt::Location{tmp.dir.string()});
        require(!rejected_alpha.scene && !rejected_alpha.diagnostics.empty(),
                "Nonopaque medium boundary must fail translation");
    }
    for (int level : {0, 1, 2}) {
        auto scene = translated(checked(
            tmp, "AreaLightSource \"diffuse\" \"spectrum L\" [360 1 830 1] \"float power\" [6]\n" +
                     std::string(loop) + "\"integer levels\" [" + std::to_string(level) + "]"));
        std::vector<std::vector<float>> factors;
        std::size_t failed = 0;
        require(
            prepare_area_power(checked_value(scene.scene), factors, failed).empty() &&
                factors.size() == 1 && factors[0].size() == (std::size_t{1} << (2 * level)) &&
                std::ranges::all_of(factors[0], [](float f) { return std::isfinite(f) && f > 0; }),
            "Powered Loop must normalize each generated triangle");
    }
    auto powered_patch = translated(checked(
        tmp, "AreaLightSource \"diffuse\" \"spectrum L\" [360 1 830 1] \"float power\" [6]\n" +
                 std::string(quad)));
    std::vector<std::vector<float>> factors;
    std::size_t failed = 0;
    require(prepare_area_power(checked_value(powered_patch.scene), factors, failed).empty() &&
                factors.size() == 1 && factors[0].size() == 1 && factors[0][0] > 0,
            "Powered bilinear patch must have one normalization factor");
    std::ofstream(tmp.dir / "quad.ply")
        << "ply\nformat ascii 1.0\nelement vertex 4\nproperty float x\nproperty float y\nproperty "
           "float z\n"
           "property float nx\nproperty float ny\nproperty float nz\nproperty float u\nproperty "
           "float v\n"
           "element face 1\nproperty list uchar int vertex_indices\nend_header\n"
           "-1 -1 3 0 0 -1 0 0\n1 -1 3 0 0 -1 1 0\n-1 1 3 0 0 -1 0 1\n1 1 3 0 0 -1 1 1\n4 0 1 3 "
           "2\n";
    const auto inherited_quad =
        checked(tmp, "Attribute \"shape\" \"point3 P\" [0 0 3 2 0 3 0 2 3 2 2 3]\n"
                     "Shape \"bilinearmesh\"\n" +
                         std::string(quad));
    require(inherited_quad.inventory.patches == 2 &&
                checked_value(inherited_quad.scene).geometry.meshes[0].positions[0].x == 0 &&
                checked_value(inherited_quad.scene).geometry.meshes[1].positions[0].x == -1,
            "Explicit bilinear P must override inherited geometry");
    const auto inherited_loop = checked(
        tmp, "Attribute \"shape\" \"point3 P\" [0 0 3 2 0 3 0 2 3] "
             "\"integer indices\" [0 1 2] \"integer levels\" [1]\n"
             "ObjectBegin \"loop\"\nShape \"loopsubdiv\" \"integer levels\" [0]\nObjectEnd\n"
             "ObjectInstance \"loop\"\nTranslate 2 0 0\nObjectInstance \"loop\"\n");
    const auto loop_instances = translated(inherited_loop);
    require(inherited_loop.inventory.triangles == 1 &&
                checked_value(loop_instances.scene).geometry.meshes.size() == 1 &&
                checked_value(loop_instances.scene).geometry.instances.size() == 2 &&
                checked_value(loop_instances.scene).geometry.instances[1].transform.values[3] == 2,
            "Inherited Loop geometry or shared object placements changed");
    auto ply = translated(checked(tmp, "Shape \"plymesh\" \"string filename\" [\"quad.ply\"]"));
    auto inline_mesh = translated(checked(
        tmp, std::string(quad) +
                 "\"normal N\" [0 0 -1 0 0 -1 0 0 -1 0 0 -1] \"point2 uv\" [0 0 1 0 0 1 1 1]"));
    require(render_identity(checked_value(ply.scene), ply.settings) ==
                render_identity(checked_value(inline_mesh.scene), inline_mesh.settings),
            "Cyclic PLY quad and inline tensor patch with N/UV must have equal identity");
}
} // namespace
int main() try {
    Scratch tmp;
    bilinear(tmp);
    subdivision(tmp);
    state_and_ownership(tmp);
    integration_contracts(tmp);
} catch (const std::exception &e) {
    std::cerr << "PBRT mesh test failure: " << e.what() << '\n';
    return 1;
}
