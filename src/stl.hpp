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
#include <string_view>
#include <vector>

namespace rypath::stl {

using Location = pbrt::Location;
using Diagnostic = pbrt::Diagnostic;
using Severity = pbrt::Severity;
inline constexpr auto profile = "stl-triangle-v1";
inline constexpr auto material_profile = "stl-srgb-coverage-v1";

enum class Encoding { ascii, binary };
enum class ColorMode { automatic, none, viscam, magics };
struct Options {
    ColorMode colors = ColorMode::automatic;
};
struct Limits {
    pbrt::Limits input{};
    std::size_t working_bytes = 256 * 1024 * 1024;
    std::size_t output_bytes = 256 * 1024 * 1024;
};
struct Color {
    std::array<double, 3> linear_rgb{.5, .5, .5};
    double coverage = 1;
};
struct Solid {
    std::string name;
    std::size_t first_facet = 0;
    std::size_t facet_count = 0;
    Location location;
};
struct Facet {
    std::array<double, 3> authored_normal{};
    std::uint16_t attribute = 0;
    std::size_t solid = 0;
    std::size_t material = 0;
    bool inherited_color = false;
    std::size_t byte_offset = 0;
    Location location;
};
struct Part {
    GeometryMesh geometry;
    std::size_t material = 0;
    std::vector<std::size_t> source_facets;
};
struct Asset {
    std::filesystem::path entry;
    std::vector<std::uint8_t> bytes;
    Encoding encoding = Encoding::ascii;
    ColorMode requested_colors = ColorMode::automatic;
    ColorMode effective_colors = ColorMode::none;
    std::optional<std::array<std::uint8_t, 4>> header_color;
    std::vector<Solid> solids;
    std::vector<Facet> facets;
    std::vector<Color> colors;
    std::vector<Part> parts;
};
struct Result {
    std::optional<Asset> asset;
    std::vector<Diagnostic> diagnostics;
    int exit_code() const;
};

Result load(const std::filesystem::path &path, const Options &options = {},
            const Limits &limits = {});
std::string report(const Result &result);
std::string_view color_mode_name(ColorMode mode);
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

// Portable preparation enforces generated-bundle budgets without filesystem writes.
Conversion prepare(const Asset &asset, const std::vector<Diagnostic> &diagnostics = {},
                   const Limits &limits = {});
// Validate a staged bundle, then publish atomically to a previously absent directory.
int publish(const Asset &asset, Conversion &conversion, const std::filesystem::path &directory,
            const Limits &limits = {});

} // namespace rypath::stl
