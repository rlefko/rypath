// SPDX-License-Identifier: GPL-3.0-only
#include "packet_wire.h"
#include "pbrt_render.hpp"
#include "render.hpp"
#include "resource_limits.hpp"
#include <filesystem>
#include <fstream>
#include <unistd.h>

#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Bytes = std::vector<std::uint8_t>;
void require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}
void u32(Bytes &b, std::uint32_t n) {
    for (int shift : {24, 16, 8, 0})
        b.push_back(static_cast<std::uint8_t>(n >> shift));
}
void chunk(Bytes &b, const char *name, const Bytes &data) {
    u32(b, static_cast<std::uint32_t>(data.size()));
    const auto begin = b.size();
    b.insert(b.end(), name, name + 4);
    b.insert(b.end(), data.begin(), data.end());
    std::uint32_t crc = 0xffffffffu;
    for (auto i = begin; i < b.size(); ++i) {
        crc ^= b[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0u);
    }
    u32(b, ~crc);
}
Bytes skinny_png(std::uint32_t height) {
    Bytes b{137, 80, 78, 71, 13, 10, 26, 10}, header;
    u32(header, 1);
    u32(header, height);
    header.insert(header.end(), {8, 2, 0, 0, 0});
    chunk(b, "IHDR", header);
    if (height <= 8192) {
        const Bytes pixels(static_cast<std::size_t>(height) * 4, 0);
        const auto n = static_cast<std::uint16_t>(pixels.size());
        Bytes z{0x78,
                1,
                1,
                static_cast<std::uint8_t>(n),
                static_cast<std::uint8_t>(n >> 8),
                static_cast<std::uint8_t>(~n),
                static_cast<std::uint8_t>((~n) >> 8)};
        z.insert(z.end(), pixels.begin(), pixels.end());
        // Adler32 of a zero-filled buffer.
        u32(z, (static_cast<std::uint32_t>(pixels.size() % 65521) << 16) | 1);
        chunk(b, "IDAT", z);
    }
    chunk(b, "IEND", {});
    return b;
}
void checks() {
    using namespace rypath;
    const auto standard = resource_limits(ResourceProfile::standard);
    const auto large = resource_limits(ResourceProfile::large);
    require(validate_resource_limits(standard).empty(), "standard profile valid");
    require(validate_resource_limits(large).empty(), "large profile valid");
    require(standard.pbrt.file_bytes == std::size_t{16} * 1024 * 1024 &&
                standard.pbrt.image.png_allocation_bytes == std::size_t{128} * 1024 * 1024 &&
                standard.pbrt.total_values == 2'000'000 &&
                standard.pbrt.shading_normal_entries == 2'000'000 / 3 &&
                standard.triangle_frame_entries == 2'000'000,
            "standard budgets retained");
    require(large.pbrt.triangle_frame_entries == 128'000'000 &&
                large.pbrt.shading_normal_entries == 128'000'000 &&
                large.pbrt.image.max_dimension == 8192 &&
                large.pbrt.image.png_allocation_bytes == image_scratch_ceiling &&
                large.preparation_bytes == std::size_t{16} * 1024 * 1024 * 1024,
            "large budgets applied");
    require(parse_resource_profile("large") == ResourceProfile::large &&
                parse_resource_profile("standard") == ResourceProfile::standard &&
                !parse_resource_profile("Large") &&
                resource_profile_name(ResourceProfile::large) == "large",
            "profile names exact");
    auto invalid = large;
    invalid.pbrt.total_values = std::numeric_limits<std::size_t>::max();
    require(!validate_resource_limits(invalid).empty(), "oversized profile rejected");
    invalid = large;
    invalid.pbrt.shading_normal_entries = 128'000'001;
    require(!validate_resource_limits(invalid).empty(), "expanded normal ceiling enforced");
    const auto normal_scene_path =
        std::filesystem::temp_directory_path() /
        ("rypath-expanded-normals-" + std::to_string(getpid()) + ".pbrt");
    {
        std::ofstream output(normal_scene_path);
        output << "Camera \"perspective\"\nFilm \"rgb\"\nSampler \"independent\"\nIntegrator "
                  "\"path\"\nWorldBegin\n"
                  "ObjectBegin \"mesh\"\nShape \"trianglemesh\" \"point3 P\" [0 0 3 1 0 3 0 1 3] "
                  "\"normal N\" [0 0 -1 0 0 -1 0 0 -1]\nObjectEnd\n"
                  "ObjectInstance \"mesh\"\nObjectInstance \"mesh\"\nObjectInstance "
                  "\"mesh\"\nObjectInstance \"mesh\"\n";
    }
    const auto normal_checked = pbrt::check(normal_scene_path);
    std::filesystem::remove(normal_scene_path);
    require(normal_checked.exit_code() == 0, "expanded normal fixture checked");
    auto normal_limits = pbrt::Limits{};
    normal_limits.total_values = 32;
    auto normal_translated = pbrt::translate(normal_checked, {}, normal_limits);
    require(normal_translated.exit_code() != 0,
            "legacy normal coordinates honor custom total_values");
    normal_limits.shading_normal_entries = 12;
    normal_translated = pbrt::translate(normal_checked, {}, normal_limits);
    require(normal_translated.exit_code() == 0, "separate expanded normal entry boundary");
    normal_limits.shading_normal_entries = 11;
    normal_translated = pbrt::translate(normal_checked, {}, normal_limits);
    require(normal_translated.exit_code() != 0,
            "one expanded normal entry below required rejected");
    // A full-depth packet retains two paths and every possible camera splat.
    // Enforce the fixed workspace bound independently of image resolution.
    for (std::uint32_t depth : {0u, 5u, static_cast<std::uint32_t>(packet_wire::max_depth)}) {
        const auto stride = packet_wire::lane_bytes(depth);
        const auto capacity = packet_wire::workspace_bytes / stride;
        require(capacity > 0 && std::uint64_t(capacity) * stride <= packet_wire::workspace_bytes &&
                    std::uint64_t(capacity + 1) * stride > packet_wire::workspace_bytes,
                "packet workspace capacity boundary");
        require(packet_wire::vertex_offset(depth) >=
                        sizeof(packet_wire::Result) + depth * sizeof(packet_wire::Splat) &&
                    stride - packet_wire::vertex_offset(depth) >=
                        (2 * depth + 3) * packet_wire::vertex_bytes,
                "packet path and splat storage disjoint");
    }
    auto preparation_scene = make_demo_scene();
    std::size_t preparation_bytes = 0;
    require(validate_render_preparation_budget(preparation_scene, &preparation_bytes).empty() &&
                preparation_bytes > 0,
            "preparation estimate without GPU allocation");
    preparation_scene.preparation_byte_limit = preparation_bytes;
    require(validate_render_preparation_budget(preparation_scene).empty(),
            "exact preparation budget boundary");
    preparation_scene.preparation_byte_limit = preparation_bytes - 1;
    require(!validate_render_preparation_budget(preparation_scene).empty(),
            "one byte below preparation budget rejected");
    preparation_scene.preparation_byte_limit = 1;
    require(validate_render_scene(preparation_scene).find("preparation") != std::string::npos,
            "tiny preparation budget fails before native allocation");
    const auto png = skinny_png(8192);
    require(decode_png(png).limit_exceeded, "standard dimension limit retained");
    const auto decoded = decode_png(png, large.pbrt.image);
    if (!decoded.value)
        throw std::runtime_error("8192 skinny PNG decode failed");
    require(decoded.value->height == 8192 && decoded.value->rgb.size() == std::size_t{8192} * 3,
            "8192 skinny PNG decoded within small budgets");
    require(build_image_pyramid(*decoded.value, ImageEncoding::linear, ImageWrap::repeat,
                                large.pbrt.image)
                .value.has_value(),
            "8192 mip pyramid supported");
    // Exercise structural decode, graph translation, full mip validation and
    // portable shading preparation together, rather than only the image builder.
    const auto mip_dir =
        std::filesystem::temp_directory_path() / ("rypath-large-mip-" + std::to_string(getpid()));
    std::filesystem::create_directories(mip_dir);
    {
        std::ofstream image(mip_dir / "skinny.png", std::ios::binary);
        image.write(reinterpret_cast<const char *>(png.data()),
                    static_cast<std::streamsize>(png.size()));
        std::ofstream scene(mip_dir / "scene.pbrt");
        scene << "Camera \"perspective\"\nFilm \"rgb\" \"integer xresolution\" [1] \"integer "
                 "yresolution\" [1]\n"
                 "Sampler \"independent\" \"integer pixelsamples\" [1]\nIntegrator \"path\" "
                 "\"integer maxdepth\" [0]\nWorldBegin\n"
                 "Texture \"tall\" \"spectrum\" \"imagemap\" \"string filename\" [\"skinny.png\"] "
                 "\"string encoding\" [\"linear\"]\n"
                 "Material \"diffuse\" \"texture reflectance\" [\"tall\"]\n"
                 "Shape \"trianglemesh\" \"point3 P\" [-1 -1 3 1 -1 3 0 1 3]\n";
    }
    const auto mip_checked = pbrt::check(mip_dir / "scene.pbrt", large.pbrt);
    auto mip_translated = pbrt::translate(mip_checked, {}, large.pbrt);
    std::filesystem::remove_all(mip_dir);
    require(mip_checked.exit_code() == 0 && mip_translated.exit_code() == 0 &&
                mip_translated.scene.has_value(),
            "8192 full image scene translation and preparation");
    if (!mip_translated.scene)
        throw std::runtime_error("8192 full image scene translation returned no scene");
    require(mip_translated.scene->image_textures[0].image->levels.size() == 14 &&
                validate_render_scene(*mip_translated.scene).empty(),
            "14-level large-profile render validation");
    auto reduced = large.pbrt.image;
    reduced.file_bytes = png.size() - 1;
    require(decode_png(png, reduced).limit_exceeded, "reduced file budget enforced");
    reduced = large.pbrt.image;
    reduced.retained_bytes = 8192 * 3 - 1;
    require(decode_png(png, reduced).limit_exceeded, "reduced retained budget enforced");
    reduced = large.pbrt.image;
    reduced.scratch_bytes = 1;
    require(decode_png(png, reduced).limit_exceeded, "reduced scratch budget enforced");
    require(decode_png(skinny_png(std::numeric_limits<std::uint32_t>::max()), large.pbrt.image)
                .limit_exceeded,
            "extreme dimensions fail before payload allocation");
    reduced = large.pbrt.image;
    reduced.max_dimension = std::numeric_limits<std::uint32_t>::max();
    require(decode_png(png, reduced).limit_exceeded, "unsupported image limits rejected");
    const std::string header = "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 8192 +X 1\n";
    Bytes hdr(header.begin(), header.end());
    for (unsigned i = 0; i < 8192; ++i)
        hdr.insert(hdr.end(), {128, 64, 32, 129});
    require(decode_hdr(hdr).limit_exceeded, "standard HDR limit retained");
    const auto decoded_hdr = decode_hdr(hdr, large.pbrt.image);
    require(decoded_hdr.value && decoded_hdr.value->floating_rgb.front() == 1,
            "large HDR dimensions supported");
}
} // namespace
int main() {
    try {
        checks();
        std::cout << "Resource profile checks passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
