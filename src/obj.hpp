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

namespace rypath::obj {

using Location = pbrt::Location;
using Diagnostic = pbrt::Diagnostic;
using Severity = pbrt::Severity;
inline constexpr auto profile = "obj-polygon-v1";
inline constexpr auto material_profile = "mtl-pbr-approx-v1";

struct Limits {
    pbrt::Limits input{};
    std::size_t working_bytes = 256 * 1024 * 1024;
    std::size_t output_bytes = 256 * 1024 * 1024;
    std::size_t face_corners = 4096;
    std::size_t polygon_work = 1U << 26U;
};

struct Source {
    std::filesystem::path path;
    std::string kind;
    std::vector<std::uint8_t> bytes;
};

struct Property {
    std::string name;
    std::vector<std::string> arguments;
    Location location;
};

struct Material {
    std::string name;
    Location location;
    std::vector<Property> properties;
};

struct Corner {
    std::uint32_t position = 0;
    std::optional<std::uint32_t> uv;
    std::optional<std::uint32_t> normal;
};

struct Polygon {
    std::vector<Corner> corners;
    std::optional<std::uint32_t> material;
    std::string object;
    std::vector<std::string> groups;
    std::uint32_t smoothing = 0;
    Location location;
};

struct Part {
    GeometryMesh geometry;
    std::optional<std::uint32_t> material;
    std::vector<std::size_t> polygons;
    std::vector<std::size_t> triangle_polygons; // Source polygon for each emitted triangle.
    bool complete_uvs = false;
};

struct Asset {
    std::filesystem::path entry;
    std::vector<Source> sources;
    std::vector<std::array<double, 3>> positions;
    std::vector<std::array<double, 2>> uvs;
    std::vector<std::array<double, 3>> normals;
    std::vector<Polygon> polygons;
    std::vector<Material> materials;
    std::vector<Part> parts;
};

struct Result {
    std::optional<Asset> asset;
    std::vector<Diagnostic> diagnostics;
    int exit_code() const;
};

// Owns bounded source snapshots. No GPU acquisition or output publication.
Result load(const std::filesystem::path &path, const Limits &limits = {});
std::string report(const Result &result);

struct BundleFile {
    std::filesystem::path path; // Safe bundle-relative path, never an authored filename.
    std::vector<std::uint8_t> bytes;
};

struct MaterialBinding {
    std::string name;
    std::string alpha;    // Complete optional PBRT shape parameter.
    std::string emission; // Complete optional AreaLightSource directive.
    bool requires_uvs = false;
};

struct Conversion {
    std::string definitions;
    std::vector<MaterialBinding> materials; // Same ordering as Asset::materials.
    MaterialBinding default_material;
    std::vector<BundleFile> files;
    std::vector<Source> sources; // Validated image source snapshots.
    std::vector<Diagnostic> diagnostics;
    int exit_code() const;
};

// Conservative capacity-based accounting of owned payloads, including metadata.
// Shared source-location strings are deliberately counted at each reference.
std::size_t retained_bytes(const Asset &asset);
std::size_t retained_bytes(const Conversion &conversion);

// Material processing is portable and retains every approximation in diagnostics.
Conversion prepare_materials(const Asset &asset, const Limits &limits = {});

// Validates a staged PBRT bundle and publishes only to a previously absent directory.
// Appends publication/round-trip diagnostics; leaves the destination absent on failure.
int publish(const Asset &asset, Conversion &conversion, const std::filesystem::path &directory,
            const Limits &limits = {});

} // namespace rypath::obj
