// SPDX-License-Identifier: GPL-3.0-only
#include "mesh_bundle.hpp"
#include "pbrt_render.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <unistd.h>

namespace {
using namespace rypath;
namespace fs = std::filesystem;
void require(bool value, const std::string &message) {
    if (!value)
        throw std::runtime_error(message);
}
constexpr auto options = "Camera \"perspective\"\nFilm \"rgb\"\nPixelFilter \"box\"\nSampler "
                         "\"independent\"\nIntegrator \"path\"\nWorldBegin\n";
constexpr auto triangle = "Shape \"trianglemesh\" \"point3 P\" [0 0 2 1 0 2 0 1 2] \"integer "
                          "indices\" [0 1 2] \"point2 uv\" [0 0 1 0 0 1]\n";
fs::path write(const fs::path &dir, const std::string &source) {
    auto path = dir / "scene.pbrt";
    std::ofstream(path) << options << source;
    return path;
}
} // namespace
int main() try {
    const auto dir = fs::temp_directory_path() / ("rypath-gltf-scene-" + std::to_string(getpid()));
    try {
        fs::create_directories(dir);
        auto path = write(dir, std::string("Material \"rypath-gltf-v1\"\n") + triangle);
        auto checked = pbrt::check(path);
        require(checked.exit_code() == 0, pbrt::report(checked));
        auto translated = pbrt::translate(checked, pbrt::Location{path.string()});
        if (!translated.scene)
            throw std::runtime_error("glTF default material translation");
        const auto &material =
            translated.scene->materials.at(translated.scene->instance_materials.at(0));
        require(material.scattering == ScatteringKind::gltf && material.gltf.metallic == 1 &&
                    material.gltf.roughness == 1 && material.gltf.base_color[3] == 1,
                "glTF defaults");
        path = write(
            dir, std::string(
                     "Material \"rypath-gltf-v1\" \"rgb basecolor\" [.1 .2 .3] \"float opacity\" "
                     "[.4] \"float metallic\" [.2] \"float roughness\" [.7] \"string alphamode\" "
                     "[\"MASK\"] \"bool doublesided\" [true] \"rgb emissive\" [.3 .2 .1]\n") +
                     triangle);
        checked = pbrt::check(path);
        require(checked.exit_code() == 0, pbrt::report(checked));
        translated = pbrt::translate(checked, pbrt::Location{path.string()});
        if (!translated.scene)
            throw std::runtime_error("glTF emissive MASK translation");
        const auto id = translated.scene->instance_materials.at(0);
        const auto &g = translated.scene->materials.at(id).gltf;
        require(g.alpha_mode == 1 && g.double_sided && g.base_color[3] == .4f && g.metallic == .2f,
                "glTF factor preservation");
        require(translated.scene->area_emissions.at(translated.scene->instance_emissions.at(0))
                        .gltf_material == id,
                "emission binds glTF material");
        path = write(
            dir, std::string("MakeNamedMaterial \"emitter\" \"string type\" [\"rypath-gltf-v1\"] "
                             "\"rgb emissive\" [1 1 1]\nNamedMaterial \"emitter\"\n") +
                     triangle + "Translate 3 0 0\n" + triangle);
        checked = pbrt::check(path);
        require(checked.exit_code() == 0, pbrt::report(checked));
        translated = pbrt::translate(checked, pbrt::Location{path.string()});
        require(translated.scene && translated.scene->area_emissions.size() == 1 &&
                    translated.scene->instance_emissions.at(0) ==
                        translated.scene->instance_emissions.at(1),
                "emissive meshes share material emission record");
        pbrt::Limits small;
        small.total_values = 256;
        const auto bounded = pbrt::translate(checked, pbrt::Location{path.string()}, small);
        require(!bounded.scene && bounded.exit_code() == 1,
                "glTF emission table charges translated value budget");
        path = write(dir, std::string("Material \"rypath-gltf-v1\" \"rgb emissive\" [1 0 0] "
                                      "\"string alphamode\" [\"BLEND\"] \"float opacity\" [.5]\n") +
                              triangle);
        checked = pbrt::check(path);
        require(checked.exit_code() == 0, pbrt::report(checked));
        translated = pbrt::translate(checked, pbrt::Location{path.string()});
        require(!translated.scene && translated.exit_code() == 2,
                "fractional BLEND emission diagnosed");
        for (const auto &invalid : {"\"float metallic\" [2]", "\"float roughness\" [-1]",
                                    "\"string alphamode\" [\"bad\"]"}) {
            path =
                write(dir, std::string("Material \"rypath-gltf-v1\" ") + invalid + "\n" + triangle);
            require(pbrt::check(path).exit_code() == 1,
                    "invalid glTF factors rejected structurally");
        }
        std::ofstream(dir / "patch.ply")
            << "ply\nformat ascii 1.0\nelement vertex 4\nproperty float x\nproperty float "
               "y\nproperty float z\nelement face 1\nproperty list uchar int "
               "vertex_indices\nend_header\n0 0 2\n1 0 2\n1 1 2\n0 1 2\n4 0 1 2 3\n";
        path = write(
            dir,
            "Material \"rypath-gltf-v1\"\nShape \"plymesh\" \"string filename\" [\"patch.ply\"]\n");
        checked = pbrt::check(path);
        require(checked.exit_code() == 0, pbrt::report(checked));
        translated = pbrt::translate(checked, pbrt::Location{path.string()});
        require(!translated.scene && translated.exit_code() == 2,
                "glTF patch binding diagnosed before GPU upload");
        const auto image_fixture = fs::path(__FILE__).parent_path() /
                                   "fixtures/pbrt-render/core-textures/assets/white.png";
        fs::copy_file(image_fixture, dir / "white.png", fs::copy_options::overwrite_existing);
        const std::string image_definition =
            "Texture \"color\" \"spectrum\" \"imagemap\" \"string filename\" [\"white.png\"] "
            "\"bool rypath.gltf\" [true] \"integer rypath.wraps\" [33648] \"integer rypath.wrapt\" "
            "[33071] \"integer rypath.minfilter\" [9986] \"integer rypath.magfilter\" [9728]\n";
        path = write(dir,
                     image_definition +
                         "Material \"rypath-gltf-v1\" \"texture basecolortexture\" [\"color\"]\n" +
                         triangle);
        checked = pbrt::check(path);
        require(checked.exit_code() == 0, pbrt::report(checked));
        translated = pbrt::translate(checked, pbrt::Location{path.string()});
        if (!translated.scene)
            throw std::runtime_error("glTF color image translation");
        const auto &image_material =
            translated.scene->materials.at(translated.scene->instance_materials.at(0));
        const auto &texture =
            translated.scene->image_textures.at(image_material.gltf.base_color_texture);
        require(texture.image->gltf && texture.gltf_sampler &&
                    texture.gltf_sampler->wrap_s == 33648 &&
                    texture.gltf_sampler->min_filter == 9986,
                "glTF sampler retained");
        path = write(
            dir,
            image_definition +
                "Material \"rypath-gltf-v1\" \"texture metallicroughnesstexture\" [\"color\"]\n" +
                triangle);
        checked = pbrt::check(path);
        require(checked.exit_code() == 0, pbrt::report(checked));
        translated = pbrt::translate(checked, pbrt::Location{path.string()});
        require(!translated.scene && translated.exit_code() == 1,
                "color/data encoding mismatch rejected");
        path =
            write(dir, image_definition +
                           "Material \"diffuse\" \"texture reflectance\" [\"color\"]\n" + triangle);
        checked = pbrt::check(path);
        require(checked.exit_code() == 0, pbrt::report(checked));
        translated = pbrt::translate(checked, pbrt::Location{path.string()});
        require(!translated.scene && translated.exit_code() == 1,
                "glTF image cannot leak into standard material graph");
        GeometryMesh mesh;
        mesh.positions = {{0, 0, 2}, {1, 0, 2}, {0, 1, 2}};
        mesh.triangles = {{{0, 1, 2}}};
        mesh.normals = {{0, 0, 1}, {0, 0, 1}, {0, 0, 1}};
        mesh.tangents = {{1, 0, 0}, {1, 0, 0}, {1, 0, 0}};
        mesh.tangent_signs = {-1, -1, -1};
        const auto bytes = mesh_bundle::ply(mesh, 10000, "glTF", "gltf-static-v1");
        {
            std::ofstream file(dir / "mesh.ply", std::ios::binary);
            file.write(reinterpret_cast<const char *>(bytes.data()),
                       static_cast<std::streamsize>(bytes.size()));
        }
        path = write(
            dir,
            "Material \"rypath-gltf-v1\"\nShape \"plymesh\" \"string filename\" [\"mesh.ply\"]\n");
        checked = pbrt::check(path);
        require(checked.exit_code() == 0, pbrt::report(checked));
        if (!checked.scene)
            throw std::runtime_error("PLY signed tangent scene missing");
        require(checked.scene->geometry.meshes.at(0).tangent_signs == mesh.tangent_signs,
                "PLY signed tangent roundtrip");
        GeometryData geometry;
        geometry.meshes.push_back(mesh);
        geometry.meshes[0].tangent_signs[0] = 1;
        require(!validate_geometry(geometry).empty(), "mixed triangle handedness rejected");
        geometry.meshes[0] = mesh;
        geometry.meshes[0].tangents[0] = {0, 0, 1};
        require(!validate_geometry(geometry).empty(), "degenerate tangent frame rejected");
        fs::remove_all(dir);
        std::cout << "glTF scene tests passed\n";
        return 0;
    } catch (const std::exception &error) {
        fs::remove_all(dir);
        std::cerr << error.what() << '\n';
        return 1;
    }
} catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
}
