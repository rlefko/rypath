// SPDX-License-Identifier: GPL-3.0-only
#include "stl.hpp"
#include "pbrt_numeric.hpp"
#include "render.hpp"

#include <algorithm>
#include <bit>
#include <cerrno>
#include <cmath>
#include <fcntl.h>
#include <limits>
#include <map>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace rypath::stl {
namespace {
struct Stop {};
using V = std::array<long double, 3>;
V sub(V a, V b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
V cross(V a, V b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
long double dot(V a, V b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
long double length(V a) { return std::hypot(a[0], a[1], a[2]); }
bool space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}
bool utf8(std::string_view s) {
    for (std::size_t i = 0; i < s.size();) {
        auto c = static_cast<unsigned char>(s[i++]);
        if (c < 128) {
            if (c < 32 && c != '\t')
                return false;
            continue;
        }
        unsigned n = 0, value = 0, minimum = 0;
        if (c >= 0xc2 && c <= 0xdf) {
            n = 1;
            value = c & 31;
            minimum = 128;
        } else if (c >= 0xe0 && c <= 0xef) {
            n = 2;
            value = c & 15;
            minimum = 2048;
        } else if (c >= 0xf0 && c <= 0xf4) {
            n = 3;
            value = c & 7;
            minimum = 65536;
        } else
            return false;
        while (n--) {
            if (i == s.size())
                return false;
            c = static_cast<unsigned char>(s[i++]);
            if ((c & 0xc0) != 0x80)
                return false;
            value = (value << 6) | (c & 63);
        }
        if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff))
            return false;
    }
    return true;
}
double linear(double x) { return x <= .04045 ? x / 12.92 : std::pow((x + .055) / 1.055, 2.4); }
struct Reader {
    const Limits &limits;
    Result result;
    Asset asset;
    Location current;
    std::size_t working = 0, values = 0, pos = 0, line = 1, column = 1;
    std::size_t zero_normals = 0, stale_normals = 0;
    Location zero_location, stale_location;
    std::map<std::array<double, 4>, std::size_t> palette;
    explicit Reader(const Limits &l) : limits(l) {}
    void add(Severity severity, std::string code, std::string message) {
        if (result.diagnostics.size() >= limits.input.diagnostics) {
            if (!result.diagnostics.empty())
                result.diagnostics.back() = {Severity::error, "stl-limit", current,
                                             "STL diagnostic limit exceeded"};
            throw Stop{};
        }
        result.diagnostics.push_back({severity, std::move(code), current, std::move(message)});
    }
    [[noreturn]] void fail(std::string code, std::string message,
                           Severity severity = Severity::error) {
        add(severity, std::move(code), std::move(message));
        throw Stop{};
    }
    void charge(std::size_t n) {
        if (n > limits.working_bytes - std::min(working, limits.working_bytes))
            fail("stl-limit", "STL working allocation budget exceeded");
        working += n;
    }
    template <class T> void room(std::vector<T> &v, std::size_t n = 1) {
        if (n > v.max_size() - v.size())
            fail("stl-limit", "STL allocation size overflow");
        const auto need = v.size() + n;
        if (need <= v.capacity())
            return;
        const auto cap =
            std::max(need, v.capacity() <= v.max_size() / 2 ? v.capacity() * 2 : v.max_size());
        if (cap > std::numeric_limits<std::size_t>::max() / sizeof(T))
            fail("stl-limit", "STL allocation size overflow");
        charge(cap * sizeof(T));
        v.reserve(cap);
    }
    void count(std::size_t n) {
        if (n > limits.input.total_values - std::min(values, limits.input.total_values))
            fail("stl-limit", "STL authored value limit exceeded");
        values += n;
    }
    std::uint32_t u32(std::size_t p) const {
        std::uint32_t x = 0;
        for (unsigned i = 0; i < 4; ++i)
            x |= std::uint32_t(asset.bytes[p + i]) << (8 * i);
        return x;
    }
    double f32(std::size_t p) const { return std::bit_cast<float>(u32(p)); }
    void read(const std::filesystem::path &path) {
        asset.entry = std::filesystem::absolute(path).lexically_normal();
        current = Location(asset.entry.string());
        if (!utf8(asset.entry.string()))
            fail("stl-metadata", "Input path is not valid UTF-8 metadata");
        const int fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK);
        if (fd < 0)
            fail("stl-file", "Cannot open STL input");
        struct Close {
            int fd;
            ~Close() { ::close(fd); }
        } close{fd};
        struct stat before {};
        if (fstat(fd, &before) != 0 || !S_ISREG(before.st_mode) || before.st_size < 0)
            fail("stl-file", "STL input must be a regular file");
        if (limits.input.expansions == 0 || limits.input.active_files == 0)
            fail("stl-limit", "STL input file count limit exceeded");
        const auto size = static_cast<std::uintmax_t>(before.st_size);
        if (size > limits.input.file_bytes || size > limits.input.expanded_bytes ||
            size > std::numeric_limits<std::size_t>::max())
            fail("stl-limit", "STL file byte limit exceeded");
        charge(static_cast<std::size_t>(size));
        asset.bytes.resize(static_cast<std::size_t>(size));
        std::size_t got = 0;
        while (got < asset.bytes.size()) {
            const auto n = ::read(fd, asset.bytes.data() + got, asset.bytes.size() - got);
            if (n < 0 && errno == EINTR)
                continue;
            if (n <= 0)
                fail("stl-file", "STL input changed or could not be read completely");
            got += static_cast<std::size_t>(n);
        }
        std::uint8_t extra = 0;
        struct stat after {};
        ssize_t trailing = 0;
        do {
            trailing = ::read(fd, &extra, 1);
        } while (trailing < 0 && errno == EINTR);
#if defined(__APPLE__)
        const auto modified = [](const struct stat &a, const struct stat &b) {
            return a.st_mtimespec.tv_sec != b.st_mtimespec.tv_sec ||
                   a.st_mtimespec.tv_nsec != b.st_mtimespec.tv_nsec ||
                   a.st_ctimespec.tv_sec != b.st_ctimespec.tv_sec ||
                   a.st_ctimespec.tv_nsec != b.st_ctimespec.tv_nsec;
        };
#else
        const auto modified = [](const struct stat &a, const struct stat &b) {
            return a.st_mtim.tv_sec != b.st_mtim.tv_sec || a.st_mtim.tv_nsec != b.st_mtim.tv_nsec ||
                   a.st_ctim.tv_sec != b.st_ctim.tv_sec || a.st_ctim.tv_nsec != b.st_ctim.tv_nsec;
        };
#endif
        if (trailing != 0 || fstat(fd, &after) != 0 || before.st_size != after.st_size ||
            modified(before, after))
            fail("stl-file", "STL input changed during snapshot");
    }
    void facet(Facet f, const std::array<V, 3> &p, Color color) {
        if (asset.facets.size() >= limits.input.directives ||
            asset.facets.size() >= std::numeric_limits<std::uint32_t>::max() / 3)
            fail("stl-limit", "STL facet count limit exceeded");
        V low = p[0], high = p[0];
        for (auto v : p)
            for (unsigned j = 0; j < 3; ++j) {
                if (!std::isfinite(v[j]) || std::abs(v[j]) > std::numeric_limits<float>::max())
                    fail("stl-geometry", "Position is not finite binary32");
                low[j] = std::min(low[j], v[j]);
                high[j] = std::max(high[j], v[j]);
            }
        const auto normal = cross(sub(p[1], p[0]), sub(p[2], p[0]));
        const auto area = length(normal);
        if (!(area > 0) || !std::isfinite(area))
            fail("stl-geometry", "Facet has degenerate or unrepresentable area");
        const auto tolerance = 64 * std::numeric_limits<float>::epsilon() * length(sub(high, low));
        std::array<V, 3> q{};
        std::array<GeometryPoint, 3> packed{};
        for (unsigned i = 0; i < 3; ++i) {
            packed[i] = {static_cast<float>(p[i][0]), static_cast<float>(p[i][1]),
                         static_cast<float>(p[i][2])};
            q[i] = {packed[i].x, packed[i].y, packed[i].z};
            for (unsigned axis = 0; axis < 3; ++axis)
                if (p[i][axis] != 0 && q[i][axis] == 0)
                    fail("stl-geometry", "Nonzero coordinate underflows binary32");
            if (length(sub(q[i], p[i])) > tolerance)
                fail("stl-geometry", "Position narrowing exceeds local geometry tolerance");
        }
        const auto narrowed = cross(sub(q[1], q[0]), sub(q[2], q[0]));
        if (!(length(narrowed) > 0) || !(dot(normal, narrowed) > 0))
            fail("stl-geometry", "Facet collapses or changes orientation after narrowing");
        // Certify the dominant projected determinant with a conservative bound
        // on subtraction/product rounding (also when long double == double).
        // Uncertain triangles are rejected rather than guessing their winding.
        const auto axis = static_cast<std::size_t>(
            std::max_element(normal.begin(), normal.end(),
                             [](auto a, auto b) { return std::abs(a) < std::abs(b); }) -
            normal.begin());
        const auto certify = [&](const std::array<V, 3> &vertices) {
            const auto a = (axis + 1) % 3, b = (axis + 2) % 3;
            const auto u = sub(vertices[1], vertices[0]), v = sub(vertices[2], vertices[0]);
            const auto determinant = u[a] * v[b] - u[b] * v[a];
            const auto bound = 32 * std::numeric_limits<double>::epsilon() *
                               (std::abs(u[a] * v[b]) + std::abs(u[b] * v[a]));
            if (!(std::abs(determinant) > bound))
                fail("stl-geometry", "Numerically uncertain facet orientation");
            return determinant > 0;
        };
        if (certify(p) != certify(q))
            fail("stl-geometry", "Projected winding changes after narrowing");
        const V authored{f.authored_normal[0], f.authored_normal[1], f.authored_normal[2]};
        if (!std::isfinite(authored[0]) || !std::isfinite(authored[1]) ||
            !std::isfinite(authored[2]))
            fail("stl-normal", "Facet normal must be finite");
        const auto max_normal =
            std::max({std::abs(authored[0]), std::abs(authored[1]), std::abs(authored[2])});
        V unit_authored{};
        if (max_normal > 0) {
            for (unsigned j = 0; j < 3; ++j)
                unit_authored[j] = authored[j] / max_normal;
            const auto n = length(unit_authored);
            for (auto &x : unit_authored)
                x /= n;
        }
        V unit_normal{};
        for (unsigned j = 0; j < 3; ++j)
            unit_normal[j] = normal[j] / area;
        if (max_normal == 0) {
            if (zero_normals++ == 0)
                zero_location = current;
        } else if (dot(unit_authored, unit_normal) <
                   1 - 64 * std::numeric_limits<float>::epsilon()) {
            if (stale_normals++ == 0)
                stale_location = current;
        }
        const std::array<double, 4> key{color.linear_rgb[0], color.linear_rgb[1],
                                        color.linear_rgb[2], color.coverage};
        auto found = palette.find(key);
        if (found == palette.end()) {
            charge(sizeof(std::pair<const std::array<double, 4>, std::size_t>) +
                   4 * sizeof(void *));
            const auto index = asset.colors.size();
            palette.emplace(key, index);
            room(asset.colors);
            asset.colors.push_back(color);
            room(asset.parts);
            Part part;
            part.material = index;
            asset.parts.push_back(std::move(part));
            f.material = index;
        } else
            f.material = found->second;
        auto &part = asset.parts[f.material];
        room(part.geometry.positions, 3);
        room(part.geometry.triangles);
        room(part.source_facets);
        const auto base = static_cast<std::uint32_t>(part.geometry.positions.size());
        for (auto v : packed)
            part.geometry.positions.push_back(v);
        part.geometry.triangles.push_back({base, base + 1, base + 2});
        part.source_facets.push_back(asset.facets.size());
        room(asset.facets);
        asset.facets.push_back(std::move(f));
        ++asset.solids.back().facet_count;
    }
    void advance() {
        const auto c = asset.bytes[pos++];
        if (c == '\r') {
            ++line;
            column = 1;
        } else if (c == '\n') {
            if (pos < 2 || asset.bytes[pos - 2] != '\r')
                ++line;
            column = 1;
        } else {
            ++column;
        }
    }
    std::string_view token() {
        while (pos < asset.bytes.size() && space(static_cast<char>(asset.bytes[pos])))
            advance();
        current = Location(current.file, line, column);
        const auto start = pos;
        while (pos < asset.bytes.size() && !space(static_cast<char>(asset.bytes[pos]))) {
            if (pos - start >= limits.input.token_bytes)
                fail("stl-limit", "STL token byte limit exceeded");
            advance();
        }
        if (start == pos)
            return {};
        return {reinterpret_cast<const char *>(asset.bytes.data() + start), pos - start};
    }
    void expect(std::string_view word) {
        if (token() != word)
            fail("stl-syntax", "Expected " + std::string(word));
    }
    double number() {
        const auto t = token();
        count(1);
        // decimal_float rejects nondecimal spellings, overflow and a nonzero
        // significand rounded to zero on the supported macOS deployment target.
        charge(t.size() + 1);
        const auto value = pbrt::decimal_float<double>(t);
        if (!value)
            fail("stl-number", "Expected representable finite decimal number");
        return *value;
    }
    std::string name() {
        while (pos < asset.bytes.size() && (asset.bytes[pos] == ' ' || asset.bytes[pos] == '\t'))
            advance();
        const auto start = pos;
        while (pos < asset.bytes.size() && asset.bytes[pos] != '\r' && asset.bytes[pos] != '\n') {
            if (pos - start >= limits.input.token_bytes)
                fail("stl-limit", "STL solid name limit exceeded");
            advance();
        }
        auto end = pos;
        while (end > start && (asset.bytes[end - 1] == ' ' || asset.bytes[end - 1] == '\t'))
            --end;
        const std::string_view s(reinterpret_cast<const char *>(asset.bytes.data() + start),
                                 end - start);
        if (!utf8(s))
            fail("stl-metadata", "Solid name is not valid UTF-8 metadata");
        charge(s.size() + 1);
        return std::string(s);
    }
    void ascii() {
        asset.effective_colors = ColorMode::none;
        auto t = token();
        while (!t.empty()) {
            if (t != "solid")
                fail("stl-syntax", "Expected solid");
            const auto location = current;
            auto solid_name = name();
            if (asset.solids.size() >= limits.input.directives)
                fail("stl-limit", "STL solid count limit exceeded");
            room(asset.solids);
            asset.solids.push_back({std::move(solid_name), asset.facets.size(), 0, location});
            t = token();
            while (t == "facet") {
                const auto loc = current;
                const auto offset = pos - t.size();
                Facet f;
                f.location = loc;
                f.byte_offset = offset;
                f.solid = asset.solids.size() - 1;
                expect("normal");
                for (auto &v : f.authored_normal)
                    v = number();
                expect("outer");
                expect("loop");
                std::array<V, 3> points{};
                for (auto &v : points) {
                    expect("vertex");
                    for (auto &x : v)
                        x = number();
                }
                expect("endloop");
                expect("endfacet");
                current = loc;
                facet(std::move(f), points, Color{});
                t = token();
            }
            if (t != "endsolid")
                fail("stl-syntax", "Expected endsolid");
            const auto end_name = name();
            if (!end_name.empty() && end_name != asset.solids.back().name)
                fail("stl-syntax", "endsolid name does not match solid name");
            t = token();
        }
        if (asset.facets.empty())
            fail("stl-syntax", "STL requires at least one facet");
        if (asset.requested_colors != ColorMode::automatic)
            fail("stl-color", "Vendor colors are unsupported for ASCII STL", Severity::unsupported);
    }
    void binary(std::size_t n) {
        asset.encoding = Encoding::binary;
        if (n == 0)
            fail("stl-syntax", "STL requires at least one facet");
        if (n > limits.input.directives || n > limits.input.total_values / 12 ||
            n > std::numeric_limits<std::uint32_t>::max() / 3)
            fail("stl-limit", "STL facet/value count limit exceeded");
        const std::string_view header(reinterpret_cast<const char *>(asset.bytes.data()), 80);
        const auto color = header.find("COLOR=");
        if (color != std::string_view::npos) {
            if (color + 10 > 80 || header.find("COLOR=", color + 6) != std::string_view::npos)
                fail("stl-color", "Duplicate or truncated COLOR= header");
            asset.header_color =
                std::array<std::uint8_t, 4>{asset.bytes[color + 6], asset.bytes[color + 7],
                                            asset.bytes[color + 8], asset.bytes[color + 9]};
            if (asset.requested_colors == ColorMode::viscam)
                fail("stl-color", "VisCAM mode conflicts with COLOR= header");
        }
        if (header.find("MATERIAL=") != std::string_view::npos)
            fail("stl-color", "Richer MATERIAL= records are unsupported", Severity::unsupported);
        asset.effective_colors = asset.requested_colors;
        if (asset.effective_colors == ColorMode::automatic) {
            asset.effective_colors = asset.header_color ? ColorMode::magics : ColorMode::none;
            if (!asset.header_color)
                for (std::size_t i = 0; i < n; ++i)
                    if (asset.bytes[84 + 50 * i + 48] || asset.bytes[84 + 50 * i + 49])
                        fail("stl-color",
                             "Nonzero binary attributes require explicit VisCAM or Magics mode",
                             Severity::unsupported);
        }
        room(asset.solids);
        asset.solids.push_back({"", 0, 0, current});
        // Reserve the known source cardinality only after all count checks.
        room(asset.facets, n);
        for (std::size_t i = 0; i < n; ++i) {
            const auto offset = 84 + 50 * i;
            current = Location(current.file, 1, offset + 1);
            Facet f;
            f.location = current;
            f.byte_offset = offset;
            for (std::size_t j = 0; j < 3; ++j)
                f.authored_normal[j] = f32(offset + 4 * j);
            std::array<V, 3> points{};
            for (std::size_t j = 0; j < 9; ++j)
                points[j / 3][j % 3] = f32(offset + 12 + 4 * j);
            f.attribute = std::uint16_t(asset.bytes[offset + 48]) |
                          std::uint16_t(std::uint16_t(asset.bytes[offset + 49]) << 8);
            Color c;
            if (asset.effective_colors == ColorMode::viscam && (f.attribute & 0x8000)) {
                for (unsigned j = 0; j < 3; ++j)
                    c.linear_rgb[j] = linear(((f.attribute >> (10 - 5 * j)) & 31) / 31.0);
            } else if (asset.effective_colors == ColorMode::magics) {
                if (asset.header_color)
                    c.coverage = (*asset.header_color)[3] / 255.0;
                if (f.attribute & 0x8000) {
                    if (!asset.header_color)
                        fail("stl-color", "Magics inherited facet color requires COLOR= header",
                             Severity::unsupported);
                    f.inherited_color = true;
                    for (unsigned j = 0; j < 3; ++j)
                        c.linear_rgb[j] = linear((*asset.header_color)[j] / 255.0);
                } else
                    for (unsigned j = 0; j < 3; ++j)
                        c.linear_rgb[j] = linear(((f.attribute >> (5 * j)) & 31) / 31.0);
            }
            count(12);
            facet(std::move(f), points, c);
        }
        if (asset.effective_colors != ColorMode::none)
            add(Severity::warning, "stl-color-assumption",
                "stl-srgb-coverage-v1 assumes vendor RGB is sRGB and Magics alpha is stochastic "
                "surface coverage");
    }
    Result run(const std::filesystem::path &path, const Options &options) {
        try {
            asset.requested_colors = options.colors;
            read(path);
            if (options.colors == ColorMode::none)
                fail("stl-color", "none is an internal color mode; choose auto, viscam, or magics");
            bool exact = false;
            std::size_t n = 0;
            if (asset.bytes.size() >= 84) {
                n = u32(80);
                exact = n <= (std::numeric_limits<std::size_t>::max() - 84) / 50 &&
                        84 + 50 * n == asset.bytes.size();
            }
            if (exact)
                binary(n);
            else
                ascii();
            if (zero_normals) {
                current = zero_location;
                add(Severity::warning, "stl-zero-normal",
                    std::to_string(zero_normals) +
                        " zero facet normals retained as metadata; geometry determines shading");
            }
            if (stale_normals) {
                current = stale_location;
                add(Severity::warning, "stl-stale-normal",
                    std::to_string(stale_normals) +
                        " facet normals disagree with winding; retained as metadata");
            }
            for (auto &part : asset.parts) {
                current = asset.facets[part.source_facets.front()].location;
                // The native shader reconstructs edges and geometric normals in
                // binary32 even though STL emits no authored shading normals.
                if (const auto error = validate_area_geometry(part.geometry, {}); !error.empty())
                    fail("stl-geometry",
                         "STL geometry exceeds the native rendering range: " + error);
                GeometryData checked;
                room(checked.meshes);
                checked.meshes.push_back(std::move(part.geometry));
                const auto error = validate_geometry(checked);
                part.geometry = std::move(checked.meshes[0]);
                if (!error.empty())
                    fail("stl-geometry", error);
            }
            result.asset = std::move(asset);
        } catch (const Stop &) {
            result.asset.reset(); // The source-located diagnostic was already retained.
        } catch (const std::exception &e) {
            result.asset.reset();
            result.diagnostics.push_back({Severity::error, "stl-file", current, e.what()});
        }
        return std::move(result);
    }
};
} // namespace
int Result::exit_code() const {
    if (std::ranges::any_of(diagnostics,
                            [](const auto &d) { return d.severity == Severity::error; }))
        return 1;
    if (std::ranges::any_of(diagnostics,
                            [](const auto &d) { return d.severity == Severity::unsupported; }))
        return 2;
    return asset ? 0 : 1;
}
Result load(const std::filesystem::path &path, const Options &options, const Limits &limits) {
    return Reader(limits).run(path, options);
}
std::string_view color_mode_name(ColorMode mode) {
    switch (mode) {
    case ColorMode::automatic:
        return "auto";
    case ColorMode::none:
        return "none";
    case ColorMode::viscam:
        return "viscam";
    case ColorMode::magics:
        return "magics";
    }
    return "unknown";
}
std::string report(const Result &r) {
    std::ostringstream out;
    out << "STL import profile " << profile << '\n';
    for (const auto &d : r.diagnostics)
        out << *d.location.file << ':' << d.location.line << ':' << d.location.column << ": "
            << (d.severity == Severity::error         ? "error"
                : d.severity == Severity::unsupported ? "unsupported"
                                                      : "warning")
            << " [" << d.code << "] " << d.message << '\n';
    if (r.asset)
        out << r.asset->facets.size() << " facets, " << r.asset->solids.size() << " solids, "
            << r.asset->parts.size() << " parts, colors "
            << color_mode_name(r.asset->effective_colors) << '\n';
    else
        out << "No complete imported asset\n";
    return out.str();
}
std::size_t retained_bytes(const Asset &a) {
    std::size_t n = sizeof(a);
    const auto add = [&](std::size_t count, std::size_t size) {
        if (count > (std::numeric_limits<std::size_t>::max() - n) / size) {
            n = std::numeric_limits<std::size_t>::max();
            return;
        }
        n += count * size;
    };
    add(a.bytes.capacity(), 1);
    add(a.entry.native().size() + 1, sizeof(std::filesystem::path::value_type));
    add(a.solids.capacity(), sizeof(Solid));
    add(a.facets.capacity(), sizeof(Facet));
    add(a.colors.capacity(), sizeof(Color));
    add(a.parts.capacity(), sizeof(Part));
    for (const auto &s : a.solids)
        add(s.name.capacity() + 1, 1);
    if (!a.facets.empty())
        add(a.facets.front().location.file->capacity() + 1, 1);
    for (const auto &p : a.parts) {
        add(p.source_facets.capacity(), sizeof(std::size_t));
        add(p.geometry.positions.capacity(), sizeof(GeometryPoint));
        add(p.geometry.triangles.capacity(), sizeof(std::array<std::uint32_t, 3>));
        add(p.geometry.normals.capacity(), sizeof(GeometryPoint));
        add(p.geometry.uvs.capacity(), sizeof(std::array<float, 2>));
        add(p.geometry.patches.capacity(), sizeof(std::array<std::uint32_t, 4>));
        add(p.geometry.tangents.capacity(), sizeof(GeometryPoint));
    }
    return n;
}
} // namespace rypath::stl
