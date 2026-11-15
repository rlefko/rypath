// SPDX-License-Identifier: GPL-3.0-only
#include "stl.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
using namespace rypath::stl;
namespace fs = std::filesystem;
void require(bool b, std::string_view message) {
    if (!b)
        throw std::runtime_error(std::string(message));
}
void write(const fs::path &p, const std::vector<std::uint8_t> &b) {
    std::ofstream out(p, std::ios::binary);
    for (auto c : b)
        out.put(static_cast<char>(c));
    out.close();
    require(out.good(), "fixture write");
}
void write(const fs::path &p, std::string_view text) {
    write(p, std::vector<std::uint8_t>(text.begin(), text.end()));
}
const Asset &success(const Result &r) {
    if (r.exit_code() != 0 || !r.asset)
        throw std::runtime_error(report(r));
    return *r.asset;
}
void failure(const Result &r, int exit = 1) {
    require(!r.asset && r.exit_code() == exit, report(r));
    require(!r.diagnostics.empty() && !r.diagnostics[0].location.file->empty() &&
                r.diagnostics[0].location.line > 0,
            "located failure");
}
std::string facet(std::string_view normal = "0 0 1",
                  std::string_view points = "0 0 0\nvertex 1 0 0\nvertex 0 1 0") {
    return "facet normal " + std::string(normal) + "\nouter loop\nvertex " + std::string(points) +
           "\nendloop\nendfacet\n";
}
std::vector<std::uint8_t> binary(const std::vector<std::uint16_t> &attrs,
                                 std::string_view header = "") {
    std::vector<std::uint8_t> b(84 + 50 * attrs.size());
    std::copy(header.begin(), header.end(), b.begin());
    const auto put = [&](std::size_t p, std::uint32_t x) {
        for (unsigned i = 0; i < 4; ++i)
            b[p + i] = static_cast<std::uint8_t>((x >> (8 * i)) & 255);
    };
    put(80, static_cast<std::uint32_t>(attrs.size()));
    const std::array<float, 12> values{0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0};
    for (std::size_t i = 0; i < attrs.size(); ++i) {
        for (std::size_t j = 0; j < 12; ++j)
            put(84 + 50 * i + 4 * j, std::bit_cast<std::uint32_t>(values[j]));
        b[84 + 50 * i + 48] = static_cast<std::uint8_t>(attrs[i] & 255);
        b[84 + 50 * i + 49] = static_cast<std::uint8_t>(attrs[i] >> 8);
    }
    return b;
}
double linear(double x) { return x <= .04045 ? x / 12.92 : std::pow((x + .055) / 1.055, 2.4); }
bool close(double a, double b) { return std::abs(a - b) < 1e-12; }
void ascii_cases(const fs::path &p) {
    write(p,
          "solid α name\n" + facet() + "endsolid α name\nsolid\n" + facet("0 0 0") + "endsolid\n");
    const auto r = load(p);
    const auto &a = success(r);
    require(a.encoding == Encoding::ascii && a.solids.size() == 2 && a.solids[0].name == "α name" &&
                a.solids[1].name.empty(),
            "multiple UTF-8/empty solids");
    require(a.parts.size() == 1 && a.parts[0].source_facets == std::vector<std::size_t>{0, 1} &&
                a.parts[0].geometry.positions.size() == 6,
            "unwelded grouping and source map");
    require(a.facets[1].solid == 1 && a.facets[1].authored_normal[2] == 0 &&
                a.facets[0].byte_offset == std::string("solid α name\n").size(),
            "source metadata");
    require(a.parts[0].geometry.normals.empty() && a.parts[0].geometry.uvs.empty(),
            "no synthesized attributes");
    require(a.facets[0].location.line == 2 && a.facets[1].location.line > 2, "ASCII locations");
    write(p, "solid a\n" + facet() + "endsolid\n");
    success(load(p));
    for (const auto &text : std::array<std::string, 7>{
             "solid a\nendsolid a\n", "solid a\n" + facet() + "endsolid b\n",
             "solid a\n" + facet() + "endsolid a\ntrailing", "solid a\nfacet normal nan 0 1\n",
             "solid a\n" + facet("0 0 1", "0 0 0\nvertex 1 0 0\nvertex 2 0 0") + "endsolid a\n",
             "solid a\n" +
                 facet("0 0 1", "100000000 0 0\nvertex 100000001 0 0\nvertex 100000000 1 0") +
                 "endsolid a\n",
             "solid a\n" + facet("0 0 1", "1e100 0 0\nvertex 0 1 0\nvertex 0 0 1") +
                 "endsolid a\n"}) {
        write(p, text);
        failure(load(p));
    }
    write(p, "solid a\n" + facet() + "endsolid a\n");
    failure(load(p, {ColorMode::viscam}), 2);
    failure(load(p, {ColorMode::magics}), 2);
    failure(load(p, {ColorMode::none}));
    for (const auto number : {"+-1", "0x1p0", "1e-9999", "1e9999", "nan", "inf"}) {
        write(p, "solid a\n" + facet(number) + "endsolid a\n");
        failure(load(p));
    }
    write(p,
          "solid a\n" + facet("0 0 1", "1e-100 0 0\nvertex 1 0 0\nvertex 0 1 0") + "endsolid a\n");
    failure(load(p));
    write(p, std::string("solid ") + char(0xff) + "\n" + facet() + "endsolid\n");
    failure(load(p));
    std::string many = "solid many\n";
    for (int i = 0; i < 120; ++i)
        many += facet(i % 2 ? "0 0 -2" : "0 0 0");
    many += "endsolid many\n";
    write(p, many);
    const auto warnings = load(p);
    success(warnings);
    require(warnings.diagnostics.size() == 2, "normal warnings aggregate per category");
    require(warnings.diagnostics[0].message.find("60") != std::string::npos &&
                warnings.diagnostics[1].message.find("60") != std::string::npos,
            "normal warning counts");
}
void ascii_numeric_locations(const fs::path &p) {
    const auto text = "solid lines\n" + facet() + facet() + "endsolid lines\n";
    for (const auto newline : {"\r", "\r\n"}) {
        std::string converted;
        for (const auto c : text) {
            if (c == '\n')
                converted += newline;
            else
                converted += c;
        }
        write(p, converted);
        const auto r = load(p);
        const auto &a = success(r);
        require(a.solids[0].location.line == 1 && a.facets[0].location.line == 2 &&
                    a.facets[1].location.line == 9 && a.facets[1].location.column == 1,
                "CR and CRLF source line locations");
    }
    write(p, "solid huge\n" + facet("0 0 1e308") + facet("1e308 1e308 1e308") + "endsolid huge\n");
    const auto huge = load(p);
    const auto &a = success(huge);
    require(a.facets[0].authored_normal[2] == 1e308 && huge.diagnostics.size() == 1 &&
                huge.diagnostics[0].code == "stl-stale-normal" &&
                huge.diagnostics[0].location.line == 9,
            "enormous finite normals use scaled comparison and retain metadata");
    write(p, "solid uncertain\n" +
                 facet("0 0 1", "0 0 0\nvertex 16777215 16777214 0\nvertex 16777216 16777215 0") +
                 "endsolid uncertain\n");
    const auto uncertain = load(p);
    failure(uncertain);
    require(uncertain.diagnostics.back().message.find("Numerically uncertain") != std::string::npos,
            "near-cancellation projected determinant is rejected explicitly");
}
void binary_cases(const fs::path &p) {
    write(p, binary({0}, "solid binary ambiguity"));
    const auto r = load(p);
    const auto &a = success(r);
    require(a.encoding == Encoding::binary && a.effective_colors == ColorMode::none &&
                a.facets[0].byte_offset == 84,
            "exact binary wins solid prefix");
    require(a.facets[0].authored_normal == std::array<double, 3>{0, 0, 1} &&
                a.colors[0].linear_rgb == std::array<double, 3>{.5, .5, .5},
            "LE binary and linear default");
    write(p, binary({1}));
    failure(load(p), 2);
    const std::vector<std::uint16_t> attrs{
        0x8000, 0xffff, static_cast<std::uint16_t>(0x8000 | (16 << 10) | (7 << 5) | 23), 1, 0x8000};
    write(p, binary(attrs));
    const auto vc = load(p, {ColorMode::viscam});
    const auto &v = success(vc);
    require(v.facets[3].attribute == 1 && !v.facets[3].inherited_color &&
                v.colors[v.facets[3].material].linear_rgb[0] == .5,
            "VisCAM inactive bits retained with fallback");
    require(v.facets[0].material == v.facets[4].material &&
                v.parts[0].source_facets == std::vector<std::size_t>{0, 4},
            "first-use palette and source order");
    const auto mid = v.colors[v.facets[2].material].linear_rgb;
    require(close(mid[0], linear(16 / 31.0)) && close(mid[1], linear(7 / 31.0)) &&
                close(mid[2], linear(23 / 31.0)),
            "VisCAM RGB channel order and midpoint transfer");
    require(v.colors[0].linear_rgb[0] == 0 && v.colors[1].linear_rgb[0] == 1,
            "VisCAM zero/one endpoints");
    auto b =
        binary({0x8000, static_cast<std::uint16_t>(16 | (7 << 5) | (23 << 10)), 0xffff}, "COLOR=");
    b[6] = 128;
    b[7] = 64;
    b[8] = 255;
    b[9] = 127;
    write(p, b);
    const auto mg = load(p);
    const auto &m = success(mg);
    require(m.effective_colors == ColorMode::magics && m.facets[0].inherited_color &&
                m.facets[2].inherited_color,
            "automatic Magics header and inheritance flag");
    require(m.facets[0].material == m.facets[2].material,
            "Magics inherited inactive low bits ignored");
    const auto own = m.colors[m.facets[1].material];
    require(close(own.linear_rgb[0], linear(16 / 31.0)) &&
                close(own.linear_rgb[1], linear(7 / 31.0)) &&
                close(own.linear_rgb[2], linear(23 / 31.0)) && close(own.coverage, 127 / 255.0),
            "Magics own channel order and shared header alpha");
    require(close(m.colors[0].linear_rgb[0], linear(128 / 255.0)) &&
                close(m.colors[0].linear_rgb[1], linear(64 / 255.0)) &&
                m.colors[0].linear_rgb[2] == 1,
            "header RGB transfer");
    failure(load(p, {ColorMode::viscam}));
    write(p, binary({0}));
    const auto opaque = load(p, {ColorMode::magics});
    require(success(opaque).colors[0].coverage == 1, "explicit headerless Magics opaque");
    write(p, binary({0x8000}));
    failure(load(p, {ColorMode::magics}), 2);
    write(p, binary({0}, "MATERIAL="));
    failure(load(p), 2);
    write(p, binary({0}, "COLOR=abcd COLOR=efgh"));
    failure(load(p));
    b = binary({0});
    const std::string marker = "COLOR=";
    std::copy(marker.begin(), marker.end(), b.begin() + 74);
    write(p, b);
    failure(load(p));
    b = binary({0});
    b.pop_back();
    write(p, b);
    failure(load(p));
    failure(load(p, {ColorMode::magics}));
    failure(load(p, {ColorMode::viscam}));
    b = binary({0});
    b.push_back(0);
    write(p, b);
    failure(load(p));
    b = binary({0});
    b[84] = 0;
    b[85] = 0;
    b[86] = 128;
    b[87] = 127;
    write(p, b);
    failure(load(p));
    b = binary({});
    write(p, b);
    failure(load(p));
}
void native_range_cases(const fs::path &p) {
    const auto binary_scale = [](float scale, float tiny_edge = 0) {
        auto bytes = binary({0});
        const auto put = [&](std::size_t offset, float value) {
            const auto bits = std::bit_cast<std::uint32_t>(value);
            for (unsigned i = 0; i < 4; ++i)
                bytes[offset + i] = static_cast<std::uint8_t>((bits >> (8 * i)) & 255);
        };
        put(84 + 24, scale);     // Second vertex x.
        put(84 + 28, tiny_edge); // Second vertex y.
        put(84 + 40, scale);     // Third vertex y.
        return bytes;
    };
    for (const auto scale : {"1e-15", "1e15", "1e-5", "1e5"}) {
        const bool accepted = std::string_view(scale) == "1e-5" || std::string_view(scale) == "1e5";
        write(p, "solid scale\n" +
                     facet("0 0 1", "0 0 0\nvertex " + std::string(scale) + " 0 0\nvertex 0 " +
                                        std::string(scale) + " 0") +
                     "endsolid scale\n");
        const auto ascii_result = load(p);
        if (accepted)
            success(ascii_result);
        else {
            failure(ascii_result);
            require(ascii_result.diagnostics.back().message.find("native rendering range") !=
                        std::string::npos,
                    "ASCII native range diagnostic");
        }
        const auto value = std::string_view(scale) == "1e-15"  ? 1e-15f
                           : std::string_view(scale) == "1e15" ? 1e15f
                           : std::string_view(scale) == "1e-5" ? 1e-5f
                                                               : 1e5f;
        write(p, binary_scale(value));
        const auto binary_result = load(p);
        if (accepted)
            success(binary_result);
        else {
            failure(binary_result);
            require(binary_result.diagnostics.back().message.find("native rendering range") !=
                        std::string::npos,
                    "binary native range diagnostic");
        }
    }
    // A representable subnormal component is allowed when the actual surface
    // normal remains in range: this triangle's cross product is exactly +Z.
    // This differs from source values that narrow all the way to zero above.
    write(p, "solid tiny\n" + facet("0 0 1", "0 0 0\nvertex 1 1e-40 0\nvertex 0 1 0") +
                 "endsolid tiny\n");
    success(load(p));
    write(p, binary_scale(1, 1e-40f));
    const auto subnormal = load(p);
    require(success(subnormal).parts[0].geometry.positions[1].y == 1e-40f,
            "representable subnormal coordinate retained with valid native normal");
}
void limits_cases(const fs::path &p) {
    write(p, binary({0, 0}));
    Limits l;
    l.input.file_bytes = 100;
    failure(load(p, {}, l));
    l = {};
    l.input.total_values = 23;
    failure(load(p, {}, l));
    l = {};
    l.input.directives = 1;
    failure(load(p, {}, l));
    l = {};
    l.working_bytes = 200;
    failure(load(p, {}, l));
    write(p, "solid a\n" + facet() + "endsolid a\n");
    l = {};
    l.input.token_bytes = 3;
    failure(load(p, {}, l));
    l = {};
    l.input.total_values = 11;
    failure(load(p, {}, l));
    failure(load(p.parent_path()));
    failure(load(p.parent_path() / "missing.stl"));
    write(p, binary({0}));
    const auto r = load(p);
    require(retained_bytes(success(r)) >= 84 + 50 + sizeof(Asset), "retained byte accounting");
}
} // namespace
int main() try {
    const auto dir = fs::temp_directory_path() /
                     ("rypath-stl-" +
                      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directory(dir);
    struct Cleanup {
        fs::path path;
        ~Cleanup() {
            std::error_code ignored;
            fs::remove_all(path, ignored);
        }
    } cleanup{dir};
    const auto p = dir / "test.stl";
    ascii_cases(p);
    ascii_numeric_locations(p);
    binary_cases(p);
    limits_cases(p);
    native_range_cases(p);
    std::cout << "STL decoder checks passed\n";
    return 0;
} catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
}
