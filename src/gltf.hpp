// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "geometry.hpp"
#include "pbrt.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>
namespace rypath::gltf {
using Location = pbrt::Location;
using Diagnostic = pbrt::Diagnostic;
using Severity = pbrt::Severity;
inline constexpr auto profile = "gltf-static-v1";
inline constexpr auto material_profile = "rypath-gltf-v1";
struct Options {
    std::optional<std::size_t> scene;
};
struct Limits {
    pbrt::Limits input{};
    std::size_t working_bytes = 256 * 1024 * 1024;
    std::size_t output_bytes = 256 * 1024 * 1024;
    std::size_t work_items = std::size_t{1} << 26;
};
struct Resource {
    std::string uri;
    std::filesystem::path path;
    std::vector<std::uint8_t> bytes;
};
struct Image {
    std::size_t source = 0;
    std::string name;
    std::string mime_type;
    std::vector<std::uint8_t> bytes;
    bool opaque = true;
};
struct Texture {
    std::size_t image = 0;
    int wrap_s = 10497, wrap_t = 10497, min_filter = 9987, mag_filter = 9729;
};
struct Material {
    std::string name;
    std::array<double, 4> base_color{1, 1, 1, 1};
    double metallic = 1, roughness = 1, normal_scale = 1, alpha_cutoff = .5;
    std::array<double, 3> emissive{};
    std::string alpha_mode = "OPAQUE";
    bool double_sided = false;
    std::optional<std::size_t> base_texture, metallic_roughness_texture, normal_texture,
        emissive_texture;
    std::optional<std::size_t> uv_set;
    bool emits() const { return emissive[0] > 0 || emissive[1] > 0 || emissive[2] > 0; }
};
struct Primitive {
    GeometryMesh geometry;
    std::size_t mesh = 0, primitive = 0, material = 0;
    std::string name;
    bool generated_normals = false, generated_tangents = false;
};
struct Placement {
    std::size_t primitive = 0, node = 0;
    // Row-major object-to-world matrix, including the affine last row.
    std::array<double, 16> transform{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    std::string name;
};
struct Asset {
    std::filesystem::path entry;
    std::vector<std::uint8_t> bytes;
    bool binary = false;
    std::size_t selected_scene = 0;
    std::vector<std::string> scene_names;
    std::vector<Resource> resources;
    std::vector<Image> images;
    std::vector<Texture> textures;
    std::vector<Material> materials;
    std::vector<Primitive> primitives;
    std::vector<Placement> placements;
};
struct Result {
    std::optional<Asset> asset;
    std::vector<Diagnostic> diagnostics;
    int exit_code() const;
};
Result load(const std::filesystem::path &path, const Options &options = {},
            const Limits &limits = {});
std::string report(const Result &result);
std::size_t retained_bytes(const Asset &asset);
struct BundleFile {
    std::filesystem::path path;
    std::vector<std::uint8_t> bytes;
};
struct Conversion {
    std::vector<BundleFile> files;
    std::vector<Diagnostic> diagnostics;
    int exit_code() const;
};
Conversion prepare(const Asset &asset, const std::vector<Diagnostic> &diagnostics = {},
                   const Limits &limits = {});
int publish(const Asset &asset, Conversion &conversion, const std::filesystem::path &directory,
            const Limits &limits = {});
} // namespace rypath::gltf
