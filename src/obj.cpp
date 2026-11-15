// SPDX-License-Identifier: GPL-3.0-only
#include "obj.hpp"

#include "pbrt_numeric.hpp"
#include "tiny_obj_c.h"
#include "triangle_attributes.hpp"

#include <mapbox/earcut.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <map>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <tuple>

// The callback decoder receives only records already checked by the provenance
// pass below. Use the same binary64 conversion in both passes, including true
// subnormals; upstream's permissive scanner maps nan/inf and truncates digits.
extern "C" bool rypath_obj_scan_double(const char **cursor, const char *end, double *value) {
    try {
        auto p = *cursor;
        while (p != end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n'))
            ++p;
        const auto begin = p;
        while (p != end && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n')
            ++p;
        const auto parsed = rypath::pbrt::decimal_float<double>(std::string_view(begin, p));
        if (!parsed)
            return false;
        *value = *parsed;
        *cursor = p;
        return true;
    } catch (...) {
        return false;
    }
}

namespace rypath::obj {
namespace {
using V3 = std::array<double, 3>;
using V2 = std::array<double, 2>;
using Tri = std::array<std::uint32_t, 3>;
struct Stop {};

V3 sub(const V3 &a, const V3 &b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
V3 cross(const V3 &a, const V3 &b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
double dot(const V3 &a, const V3 &b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
double length(const V3 &a) { return std::hypot(a[0], a[1], a[2]); }
V3 unit(const V3 &a) {
    const auto n = length(a);
    return {a[0] / n, a[1] / n, a[2] / n};
}
V3 point(const GeometryPoint &p) { return {p.x, p.y, p.z}; }
GeometryPoint narrow(const V3 &p) {
    return {static_cast<float>(p[0]), static_cast<float>(p[1]), static_cast<float>(p[2])};
}

struct Reader {
    const Limits &limits;
    Result result;
    Asset asset;
    std::size_t working = 0, output = 0, values = 0, directives = 0, bytes = 0;
    std::size_t work = 0;
    Location current;
    std::vector<std::string> bindings;
    std::map<std::string, std::uint32_t> material_ids;
    std::size_t expected_v = 0, expected_vt = 0, expected_vn = 0;
    std::string decode_text;
    std::size_t decoded_faces = 0;
    bool callback_failed = false;
    std::vector<std::size_t> arities;

    explicit Reader(const Limits &l) : limits(l) {}
    [[noreturn]] void fail(std::string code, std::string message) {
        add(Severity::error, std::move(code), std::move(message));
        throw Stop{};
    }
    void add(Severity severity, std::string code, std::string message) {
        // Reserve a terminal limit diagnostic rather than silently truncating.
        if (result.diagnostics.size() >= limits.input.diagnostics) {
            if (!result.diagnostics.empty())
                result.diagnostics.back() = {Severity::error, "obj-limit", current,
                                             "OBJ diagnostic limit exceeded"};
            throw Stop{};
        }
        result.diagnostics.push_back({severity, std::move(code), current, std::move(message)});
    }
    void charge(std::size_t n) {
        if (n > limits.working_bytes - std::min(working, limits.working_bytes))
            fail("obj-limit", "OBJ cumulative working allocation budget exceeded");
        working += n;
    }
    void charge_output(std::size_t n) {
        if (n > limits.output_bytes - std::min(output, limits.output_bytes))
            fail("obj-limit", "OBJ retained output budget exceeded");
        output += n;
        charge(n);
    }
    template <class T> void room(std::vector<T> &v, std::size_t extra = 1) {
        if (extra > (std::numeric_limits<std::size_t>::max() / sizeof(T)) - v.size())
            fail("obj-limit", "OBJ allocation size overflow");
        const auto need = v.size() + extra;
        if (need <= v.capacity())
            return;
        const auto cap = std::max(need, v.capacity() ? v.capacity() * 2 : std::size_t{4});
        charge(cap * sizeof(T));
        v.reserve(cap);
    }
    void authored(std::size_t n) {
        if (n > limits.input.total_values - std::min(values, limits.input.total_values))
            fail("obj-limit", "OBJ/MTL authored value limit exceeded");
        values += n;
    }
    std::size_t read_file(const std::filesystem::path &raw, std::string kind) {
        std::error_code ec;
        const auto path = std::filesystem::weakly_canonical(raw, ec);
        if (ec)
            fail("obj-file", "Cannot resolve input: " + raw.string());
        for (std::size_t i = 0; i < asset.sources.size(); ++i)
            if (asset.sources[i].path == path) {
                if (asset.sources[i].kind != kind)
                    fail("obj-file", "One input is referenced as both OBJ and MTL");
                return i;
            }
        if (asset.sources.size() >= limits.input.expansions)
            fail("obj-limit", "OBJ/MTL input file limit exceeded");
        const auto size = std::filesystem::file_size(path, ec);
        if (ec)
            fail("obj-file", "Cannot read input: " + path.string());
        if (size > limits.input.file_bytes ||
            size > limits.input.expanded_bytes - std::min(bytes, limits.input.expanded_bytes))
            fail("obj-limit", "OBJ/MTL file byte limit exceeded");
        charge_output(static_cast<std::size_t>(size) + path.string().size() * 2 + sizeof(Source));
        Source source{path, std::move(kind), {}};
        source.bytes.resize(static_cast<std::size_t>(size));
        std::ifstream input(path, std::ios::binary);
        if (!input ||
            !input.read(reinterpret_cast<char *>(source.bytes.data()),
                        static_cast<std::streamsize>(size)) ||
            input.peek() != EOF)
            fail("obj-file", "Input changed or could not be read completely: " + path.string());
        if (std::ranges::find(source.bytes, std::uint8_t{0}) != source.bytes.end())
            fail("obj-syntax", "NUL byte in OBJ/MTL input");
        bytes += static_cast<std::size_t>(size);
        room(asset.sources);
        asset.sources.push_back(std::move(source));
        return asset.sources.size() - 1;
    }
    std::vector<std::string> tokens(std::string_view line) {
        std::vector<std::string> out;
        std::size_t p = 0;
        while (p < line.size()) {
            while (p < line.size() && (line[p] == ' ' || line[p] == '\t' || line[p] == '\r'))
                ++p;
            if (p == line.size() || line[p] == '#')
                break;
            std::string token;
            bool quoted = false;
            while (p < line.size()) {
                const char c = line[p];
                if (!quoted && (c == ' ' || c == '\t' || c == '\r' || c == '#'))
                    break;
                if (c == '"') {
                    quoted = !quoted;
                    ++p;
                    continue;
                }
                if (c == '\\' && p + 1 < line.size() &&
                    (line[p + 1] == ' ' || line[p + 1] == '\t' || line[p + 1] == '"' ||
                     line[p + 1] == '#' || line[p + 1] == '\\')) {
                    if (token.size() >= limits.input.token_bytes)
                        fail("obj-limit", "OBJ token too long");
                    if (token.size() == token.capacity()) {
                        charge((token.capacity() + 1) * 2);
                        token.reserve((token.capacity() + 1) * 2);
                    }
                    token.push_back(line[p + 1]);
                    p += 2;
                } else {
                    if (static_cast<unsigned char>(c) < 32)
                        fail("obj-syntax", "Control byte in input");
                    if (token.size() >= limits.input.token_bytes)
                        fail("obj-limit", "OBJ token too long");
                    if (token.size() == token.capacity()) {
                        charge((token.capacity() + 1) * 2);
                        token.reserve((token.capacity() + 1) * 2);
                    }
                    token.push_back(c);
                    ++p;
                }
            }
            if (quoted)
                fail("obj-syntax", "Unterminated quoted token");
            if (token.empty())
                fail("obj-syntax", "Empty input token");
            // Includes string growth and its transient/reallocation storage.
            charge(token.size() * 4 + sizeof(std::string));
            if (out.size() >= limits.input.list_values)
                fail("obj-limit", "OBJ record value limit exceeded");
            room(out);
            out.push_back(std::move(token));
            if (p < line.size() && line[p] == '#')
                break;
        }
        return out;
    }
    template <class F> void lines(std::size_t index, F fn) {
        // Copy the shared source handle, not a reference into sources: mtllib can
        // append snapshots and reallocate that vector during an OBJ callback.
        const auto path = asset.sources[index].path;
        const auto file = std::make_shared<const std::string>(path.string());
        const auto *data = asset.sources[index].bytes.data();
        const auto size = asset.sources[index].bytes.size();
        std::size_t p = 0, line_number = 1;
        if (size >= 3 && data[0] == 0xef && data[1] == 0xbb && data[2] == 0xbf)
            p = 3;
        while (p < size) {
            current = {file, line_number, 1};
            std::string logical;
            bool continuation = false;
            do {
                const auto start = p;
                while (p < size && data[p] != '\r' && data[p] != '\n')
                    ++p;
                auto end = p;
                continuation = end > start && data[end - 1] == '\\';
                if (continuation)
                    --end;
                if (end - start > limits.input.file_bytes - logical.size())
                    fail("obj-limit", "OBJ logical record too long");
                charge((end - start + 1) * 2);
                logical.append(reinterpret_cast<const char *>(data + start), end - start);
                logical.push_back(' ');
                if (p < size) {
                    const auto c = data[p++];
                    if (c == '\r' && p < size && data[p] == '\n')
                        ++p;
                    ++line_number;
                }
                if (continuation && p == size)
                    fail("obj-syntax", "Unterminated line continuation");
            } while (continuation);
            auto ts = tokens(logical);
            if (ts.empty())
                continue;
            if (directives >= limits.input.directives)
                fail("obj-limit", "OBJ/MTL directive limit exceeded");
            ++directives;
            authored(ts.size() - 1);
            fn(ts);
        }
    }
    double number(std::string_view token) {
        const auto value = pbrt::decimal_float<double>(token);
        if (!value)
            fail("obj-number", "Expected a finite complete decimal number: " + std::string(token));
        return *value;
    }
    std::int64_t integer(std::string_view token) {
        std::int64_t value = 0;
        if (token.starts_with('+'))
            token.remove_prefix(1);
        const auto [end, ec] = std::from_chars(token.data(), token.data() + token.size(), value);
        if (ec != std::errc{} || end != token.data() + token.size() || token.empty())
            fail("obj-index", "Expected a complete in-range integer");
        return value;
    }
    std::uint32_t index(std::string_view token, std::size_t count) {
        const auto i = integer(token);
        if (i == 0 || i > static_cast<std::int64_t>(count) || i < -static_cast<std::int64_t>(count))
            fail("obj-index", "OBJ index is zero or outside the array at this record");
        return static_cast<std::uint32_t>(i > 0 ? i - 1 : static_cast<std::int64_t>(count) + i);
    }
    Corner corner(std::string_view token) {
        Corner c;
        const auto first = token.find('/');
        c.position = index(token.substr(0, first), expected_v);
        if (first == std::string_view::npos)
            return c;
        const auto last = token.find('/', first + 1);
        if (last == std::string_view::npos) {
            c.uv = index(token.substr(first + 1), expected_vt);
        } else {
            if (token.find('/', last + 1) != std::string_view::npos)
                fail("obj-index", "Too many slash fields in face corner");
            if (last != first + 1)
                c.uv = index(token.substr(first + 1, last - first - 1), expected_vt);
            c.normal = index(token.substr(last + 1), expected_vn);
        }
        return c;
    }
    void mtl(std::size_t file_index) {
        std::optional<std::size_t> active;
        lines(file_index, [&](const auto &ts) {
            if (ts[0] == "newmtl") {
                if (ts.size() != 2)
                    fail("obj-material", "newmtl requires exactly one material name");
                if (material_ids.contains(ts[1]))
                    fail("obj-material", "Duplicate newmtl: " + ts[1]);
                charge_output(sizeof(Material) + ts[1].size() * 2);
                room(asset.materials);
                asset.materials.push_back({ts[1], current, {}});
                active = asset.materials.size() - 1;
                charge(sizeof(decltype(material_ids)::value_type) + 64 + ts[1].size() * 2);
                material_ids.emplace(ts[1], static_cast<std::uint32_t>(*active));
            } else {
                if (!active)
                    fail("obj-material", "MTL property precedes newmtl");
                Property property{ts[0], {}, current};
                charge_output(sizeof(Property) + ts[0].size() * 2);
                for (std::size_t i = 1; i < ts.size(); ++i) {
                    charge_output(sizeof(std::string) + ts[i].size() * 2);
                    room(property.arguments);
                    property.arguments.push_back(ts[i]);
                }
                auto &properties = asset.materials[*active].properties;
                room(properties);
                properties.push_back(std::move(property));
            }
        });
    }
    void preflight(std::size_t entry) {
        std::string object, material;
        std::vector<std::string> groups;
        std::uint32_t smoothing = 0;
        const auto directory = asset.entry.parent_path();
        lines(entry, [&](const auto &ts) {
            const auto &op = ts[0];
            bool decoder_record = false;
            if (op == "v" || op == "vn" || op == "vt") {
                const auto n = ts.size() - 1;
                if ((op == "v" && n != 3 && n != 4) || (op == "vn" && n != 3) ||
                    (op == "vt" && (n < 1 || n > 3)))
                    fail("obj-cardinality", "Invalid vertex/normal/UV component count");
                for (std::size_t i = 1; i < ts.size(); ++i)
                    number(ts[i]);
                if (op == "v" && n == 4 && number(ts[4]) != 1)
                    add(Severity::unsupported, "obj-weight",
                        "Nonunit homogeneous vertex weight is unsupported");
                if (op == "vt" && n == 3 && number(ts[3]) != 0)
                    add(Severity::unsupported, "obj-uv-depth",
                        "Nonzero texture depth is unsupported");
                auto &count = op == "v" ? expected_v : op == "vn" ? expected_vn : expected_vt;
                if (count >= static_cast<std::size_t>(std::numeric_limits<int>::max()))
                    fail("obj-limit", "Too many attributes for loader indices");
                ++count;
                decoder_record = true;
            } else if (op == "f") {
                if (ts.size() < 4)
                    fail("obj-face", "Polygon needs at least three corners");
                if (ts.size() - 1 > limits.face_corners)
                    fail("obj-limit", "Polygon corner limit exceeded");
                charge_output(sizeof(Polygon) + object.size() * 2 +
                              sizeof(Corner) * (ts.size() - 1));
                for (const auto &g : groups)
                    charge_output(sizeof(std::string) + g.size() * 2);
                Polygon polygon;
                polygon.location = current;
                polygon.object = object;
                polygon.groups = groups;
                polygon.smoothing = smoothing;
                for (std::size_t i = 1; i < ts.size(); ++i) {
                    room(polygon.corners);
                    polygon.corners.push_back(corner(ts[i]));
                }
                room(asset.polygons);
                asset.polygons.push_back(std::move(polygon));
                room(bindings);
                charge(material.size() * 2);
                bindings.push_back(material);
                room(arities);
                arities.push_back(ts.size() - 1);
                decoder_record = true;
            } else if (op == "o") {
                if (ts.size() > 2)
                    fail("obj-syntax", "o accepts one name (quote names containing spaces)");
                charge(ts.size() == 2 ? ts[1].size() * 2 : 0);
                object = ts.size() == 2 ? ts[1] : "";
            } else if (op == "g") {
                groups.clear();
                for (std::size_t i = 1; i < ts.size(); ++i) {
                    room(groups);
                    charge(ts[i].size() * 2);
                    groups.push_back(ts[i]);
                }
            } else if (op == "s") {
                if (ts.size() != 2)
                    fail("obj-syntax", "s requires off, on, or one nonnegative integer");
                if (ts[1] == "off" || ts[1] == "0")
                    smoothing = 0;
                else if (ts[1] == "on")
                    smoothing = 1;
                else {
                    const auto s = integer(ts[1]);
                    if (s < 0 || s > std::numeric_limits<std::uint32_t>::max())
                        fail("obj-syntax", "Invalid smoothing group");
                    smoothing = static_cast<std::uint32_t>(s);
                }
            } else if (op == "usemtl") {
                if (ts.size() != 2)
                    fail("obj-material", "usemtl requires one name");
                charge(ts[1].size() * 2);
                material = ts[1];
            } else if (op == "mtllib") {
                if (ts.size() < 2)
                    fail("obj-material", "mtllib requires a library filename");
                for (std::size_t i = 1; i < ts.size(); ++i) {
                    const auto saved = current;
                    const auto count = asset.sources.size();
                    const auto source = read_file(directory / ts[i], "mtl");
                    if (asset.sources.size() != count)
                        mtl(source);
                    current = saved;
                }
            } else {
                constexpr std::array unsupported = {
                    "p",    "l",          "vp",        "cstype", "deg",   "bmat",     "step",
                    "curv", "curv2",      "surf",      "parm",   "trim",  "hole",     "scrv",
                    "sp",   "end",        "con",       "mg",     "bevel", "c_interp", "d_interp",
                    "lod",  "shadow_obj", "trace_obj", "ctech",  "stech", "call",     "csh"};
                if (std::ranges::find(unsupported, op) != unsupported.end())
                    add(Severity::unsupported, "obj-unsupported",
                        op + " is outside the polygonal import profile; no commands or inclusions "
                             "are executed");
                else
                    fail("obj-directive", "Unknown OBJ directive: " + op);
            }
            if (decoder_record) {
                for (const auto &t : ts) {
                    if (t.find_first_of(" \t\r\n") != std::string::npos)
                        fail("obj-syntax", "Whitespace inside a numeric geometry token");
                    charge(t.size() * 2 + 2);
                    decode_text += t;
                    decode_text += ' ';
                }
                decode_text += '\n';
            }
        });
        // Forward material bindings are resolved after all libraries are read.
        for (std::size_t i = 0; i < asset.polygons.size(); ++i) {
            current = asset.polygons[i].location;
            if (bindings[i].empty())
                continue;
            const auto found = material_ids.find(bindings[i]);
            if (found == material_ids.end())
                fail("obj-material", "Undefined usemtl: " + bindings[i]);
            asset.polygons[i].material = found->second;
        }
        if (asset.polygons.empty())
            fail("obj-geometry", "OBJ contains no polygon faces");
    }
    static void *allocate(void *ud, std::size_t size, std::size_t) noexcept {
        auto &r = *static_cast<Reader *>(ud);
        if (size > r.limits.working_bytes - std::min(r.working, r.limits.working_bytes))
            return nullptr;
        r.working += size;
        return std::malloc(size ? size : 1);
    }
    static void release(void *, void *p, std::size_t) noexcept { std::free(p); }
    template <class F> static void callback(void *ud, F fn) noexcept {
        auto &r = *static_cast<Reader *>(ud);
        if (r.callback_failed)
            return;
        try {
            fn(r);
        } catch (...) {
            r.callback_failed = true;
        }
    }
    void decode() {
        tobj_load_config config = tobj_default_config();
        config.triangulate = false;
        config.store_vertex_colors = false;
        config.vertex_color_fallback = false;
        config.num_threads = 1;
        config.max_vertices = std::max({expected_v, expected_vt, expected_vn, std::size_t{1}});
        config.max_indices = std::max(values, std::size_t{1});
        config.max_faces = asset.polygons.size();
        config.max_face_arity = std::max(limits.face_corners, std::size_t{1});
        config.max_line_bytes = std::max(decode_text.size(), std::size_t{1});
        config.max_input_bytes = std::max(decode_text.size(), std::size_t{1});
        config.allocator = {allocate, nullptr, nullptr, release, limits.working_bytes, this};
        tobj_callbacks_d cb{};
        cb.user_data = this;
        // Fixed upstream C callback ABI requires ordered scalar coordinates.
        // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
        cb.vertex_cb = [](void *ud, double x, double y, double z, double) {
            callback(ud, [&](Reader &r) {
                r.charge_output(sizeof(V3));
                r.room(r.asset.positions);
                r.asset.positions.push_back({x, y, z});
            });
        };
        // Fixed upstream C callback ABI requires ordered scalar coordinates.
        // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
        cb.normal_cb = [](void *ud, double x, double y, double z) {
            callback(ud, [&](Reader &r) {
                r.charge_output(sizeof(V3));
                r.room(r.asset.normals);
                r.asset.normals.push_back({x, y, z});
            });
        };
        // Fixed upstream C callback ABI requires ordered scalar coordinates.
        // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
        cb.texcoord_cb = [](void *ud, double u, double v, double) {
            callback(ud, [&](Reader &r) {
                r.charge_output(sizeof(V2));
                r.room(r.asset.uvs);
                r.asset.uvs.push_back({u, v});
            });
        };
        cb.index_cb = [](void *ud, const tobj_index *indices, int n) {
            callback(ud, [&](Reader &r) {
                if (r.decoded_faces >= r.asset.polygons.size() || n < 0 ||
                    static_cast<std::size_t>(n) != r.arities[r.decoded_faces])
                    r.fail("obj-decoder", "Decoder face cardinality disagreement");
                const auto &polygon = r.asset.polygons[r.decoded_faces++];
                for (std::size_t i = 0; i < polygon.corners.size(); ++i) {
                    const auto &c = polygon.corners[i];
                    const auto &d = indices[i];
                    if (d.vertex_index != static_cast<int>(c.position) ||
                        d.texcoord_index != (c.uv ? static_cast<int>(*c.uv) : -1) ||
                        d.normal_index != (c.normal ? static_cast<int>(*c.normal) : -1))
                        r.fail("obj-decoder", "Decoder corner index disagreement");
                }
            });
        };
        tobj_diag diag{};
        diag.on_message = [](void *ud, tobj_severity, std::size_t, const char *, std::size_t) {
            static_cast<Reader *>(ud)->callback_failed = true;
        };
        diag.user_data = this;
        const auto status = tobj_load_obj_with_callbacks_d(
            reinterpret_cast<const std::uint8_t *>(decode_text.data()), decode_text.size(), &cb,
            &config, &diag);
        if (status != TOBJ_OK || callback_failed)
            fail("obj-decoder",
                 "Bounded TinyObjLoader decode failed (allocation, numeric or callback error)");
        if (asset.positions.size() != expected_v || asset.uvs.size() != expected_vt ||
            asset.normals.size() != expected_vn || decoded_faces != asset.polygons.size())
            fail("obj-decoder", "Decoder attribute/face count disagreement");
    }
    void geometry();
    Result run(const std::filesystem::path &path) {
        current = Location(path.string());
        try {
            if (limits.input.diagnostics == 0) {
                result.diagnostics.push_back(
                    {Severity::error, "obj-limit", current, "Diagnostic limit is zero"});
                return std::move(result);
            }
            asset.entry = std::filesystem::absolute(path).lexically_normal();
            const auto source = read_file(asset.entry, "obj");
            asset.entry = asset.sources[source].path;
            preflight(source);
            if (std::ranges::any_of(result.diagnostics,
                                    [](const auto &d) { return d.severity != Severity::warning; }))
                return std::move(result);
            decode();
            geometry();
            result.asset = std::move(asset);
        } catch (const Stop &) {
            return std::move(result); // fail() already retained the source-located diagnostic.
        } catch (const std::exception &e) {
            if (result.diagnostics.size() < limits.input.diagnostics)
                result.diagnostics.push_back({Severity::error, "obj-allocation", current,
                                              std::string("OBJ import failed: ") + e.what()});
        }
        return std::move(result);
    }
};

using L2 = std::array<long double, 2>;
// Exact determinant sign for binary64 coordinates, using error-free
// differences/products and a nonoverlapping expansion (Shewchuk, 1997).
// Input coordinates have already passed binary32 range checks, so products and
// residuals cannot overflow or underflow binary64. This also handles platforms
// where long double has no additional precision.
int exact_orientation(const std::array<L2, 3> &triangle) {
    const auto &a = triangle[0], &b = triangle[1], &c = triangle[2];
    const auto difference = [](double x, double y) {
        const auto hi = x - y, virt = x - hi;
        return std::array{(x - (hi + virt)) + (virt - y), hi};
    };
    const auto ax = difference(static_cast<double>(b[0]), static_cast<double>(a[0]));
    const auto ay = difference(static_cast<double>(b[1]), static_cast<double>(a[1]));
    const auto bx = difference(static_cast<double>(c[0]), static_cast<double>(a[0]));
    const auto by = difference(static_cast<double>(c[1]), static_cast<double>(a[1]));
    std::array<double, 32> expansion{};
    std::size_t count = 0;
    const auto add = [&](double term) {
        std::size_t output = 0;
        for (std::size_t i = 0; i < count; ++i) {
            const auto sum = term + expansion[i], rounded = sum - term;
            const auto residual = (term - (sum - rounded)) + (expansion[i] - rounded);
            if (residual != 0)
                expansion[output++] = residual;
            term = sum;
        }
        if (term != 0)
            expansion[output++] = term;
        count = output;
    };
    for (std::size_t i = 0; i < 2; ++i)
        for (std::size_t j = 0; j < 2; ++j) {
            const auto p = ax[i] * by[j], q = ay[i] * bx[j];
            add(std::fma(ax[i], by[j], -p));
            add(p);
            add(-std::fma(ay[i], bx[j], -q));
            add(-q);
        }
    return count == 0 ? 0 : expansion[count - 1] > 0 ? 1 : -1;
}
long double orient(const L2 &a, const L2 &b, const L2 &c) {
    const auto exact = exact_orientation({a, b, c});
    if (exact == 0)
        return 0;
    const auto estimate = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
    if ((estimate > 0 ? 1 : estimate < 0 ? -1 : 0) != exact)
        return exact * std::numeric_limits<long double>::min();
    return estimate;
}
bool between(const L2 &a, const L2 &b, const L2 &p) {
    return orient(a, b, p) == 0 && p[0] >= std::min(a[0], b[0]) && p[0] <= std::max(a[0], b[0]) &&
           p[1] >= std::min(a[1], b[1]) && p[1] <= std::max(a[1], b[1]);
}
int sign(long double x) { return x > 0 ? 1 : x < 0 ? -1 : 0; }
bool intersects(const L2 &a, const L2 &b, const L2 &c, const L2 &d) {
    const auto ab_c = orient(a, b, c), ab_d = orient(a, b, d), cd_a = orient(c, d, a),
               cd_b = orient(c, d, b);
    return (sign(ab_c) * sign(ab_d) < 0 && sign(cd_a) * sign(cd_b) < 0) ||
           (ab_c == 0 && between(a, b, c)) || (ab_d == 0 && between(a, b, d)) ||
           (cd_a == 0 && between(c, d, a)) || (cd_b == 0 && between(c, d, b));
}

void Reader::geometry() {
    // Long-double predicates certify both the binary64 ring and its binary32
    // realization. Near-cancellation is rejected instead of guessed. Geometry
    // additions do not change transport or primitive intersection estimators.
    for (const auto &p : asset.positions) {
        for (const auto x : p) {
            const auto f = static_cast<float>(x);
            if (!std::isfinite(f) || (x != 0 && f == 0))
                fail("obj-number", "Position cannot be represented as finite nonzero binary32");
        }
    }
    for (auto &n : asset.normals) {
        const auto norm = length(n);
        if (!(norm > 0) || !std::isfinite(norm))
            fail("obj-normal", "Zero or unrepresentable authored normal");
        const auto normalized = unit(n);
        for (std::size_t axis = 0; axis < n.size(); ++axis)
            if (n[axis] != 0 && normalized[axis] == 0)
                fail("obj-normal", "Authored normal component underflows during normalization");
        n = normalized;
    }
    for (const auto &uv : asset.uvs)
        for (const auto x : uv) {
            const auto f = static_cast<float>(x);
            if (!std::isfinite(f) || (x != 0 && f == 0))
                fail("obj-number", "UV cannot be represented as finite nonzero binary32");
        }
    struct Built {
        V3 area{};
        std::vector<Tri> triangles;
    };
    std::vector<Built> built;
    room(built, asset.polygons.size());
    built.resize(asset.polygons.size());
    std::size_t largest = 3;
    for (const auto &p : asset.polygons)
        largest = std::max(largest, p.corners.size());
    // Conservative bound for the pinned single-ring Earcut pool (minimum block
    // 256), index/sort vectors, and geometric-growth reallocations. Reuse the
    // same Earcut object; no refinement, holes or unvalidated fallback input.
    charge(std::max(largest, std::size_t{256}) * std::size_t{1024} + std::size_t{256} * 1024);
    mapbox::detail::Earcut<std::uint32_t> earcut;
    for (std::size_t pi = 0; pi < asset.polygons.size(); ++pi) {
        const auto &polygon = asset.polygons[pi];
        current = polygon.location;
        const auto n = polygon.corners.size();
        if (n > limits.polygon_work / n ||
            n * n > limits.polygon_work - std::min(work, limits.polygon_work))
            fail("obj-limit", "Aggregate quadratic polygon work limit exceeded");
        work += n * n;
        std::vector<V3> points, narrowed;
        room(points, n);
        room(narrowed, n);
        for (const auto &c : polygon.corners) {
            points.push_back(asset.positions[c.position]);
            narrowed.push_back(point(narrow(asset.positions[c.position])));
        }
        V3 low = points[0], high = points[0], area{};
        const auto origin = points[0];
        for (std::size_t i = 0; i < n; ++i) {
            for (std::size_t k = 0; k < 3; ++k) {
                low[k] = std::min(low[k], points[i][k]);
                high[k] = std::max(high[k], points[i][k]);
            }
            const auto a = sub(points[i], origin), b = sub(points[(i + 1) % n], origin),
                       ab = cross(a, b);
            for (std::size_t k = 0; k < 3; ++k)
                area[k] += ab[k];
        }
        const auto diagonal = length(sub(high, low)), area_length = length(area);
        if (!(diagonal > 0) || !(area_length > 0) || !std::isfinite(area_length))
            fail("obj-polygon", "Degenerate polygon plane");
        const auto normal = unit(area);
        const auto tolerance =
            64 * static_cast<double>(std::numeric_limits<float>::epsilon()) * diagonal;
        for (const auto &p : points)
            if (std::abs(dot(sub(p, origin), normal)) > tolerance)
                fail("obj-planarity", "Polygon exceeds the scale-relative planarity tolerance");
        // Quantization must preserve the local geometry, not just leave a
        // topologically valid 2D projection. Large translations can otherwise
        // turn a planar ring into a visibly warped surface after narrowing.
        for (std::size_t i = 0; i < n; ++i)
            if (length(sub(narrowed[i], points[i])) > tolerance)
                fail("obj-polygon", "Position narrowing exceeds the local geometry tolerance");
        V3 narrowed_low = narrowed[0], narrowed_high = narrowed[0], narrowed_area{};
        for (std::size_t i = 0; i < n; ++i) {
            for (std::size_t axis = 0; axis < 3; ++axis) {
                narrowed_low[axis] = std::min(narrowed_low[axis], narrowed[i][axis]);
                narrowed_high[axis] = std::max(narrowed_high[axis], narrowed[i][axis]);
            }
            const auto term =
                cross(sub(narrowed[i], narrowed[0]), sub(narrowed[(i + 1) % n], narrowed[0]));
            for (std::size_t axis = 0; axis < 3; ++axis)
                narrowed_area[axis] += term[axis];
        }
        if (!(length(narrowed_area) > 0) || !std::isfinite(length(narrowed_area)))
            fail("obj-polygon", "Polygon plane collapses after position narrowing");
        const auto narrowed_normal = unit(narrowed_area);
        const auto narrowed_tolerance = 64 *
                                        static_cast<double>(std::numeric_limits<float>::epsilon()) *
                                        length(sub(narrowed_high, narrowed_low));
        for (const auto &p : narrowed)
            if (std::abs(dot(sub(p, narrowed[0]), narrowed_normal)) > narrowed_tolerance)
                fail("obj-planarity",
                     "Narrowed polygon exceeds the scale-relative planarity tolerance");
        std::size_t drop = 0;
        if (std::abs(area[1]) > std::abs(area[drop]))
            drop = 1;
        if (std::abs(area[2]) > std::abs(area[drop]))
            drop = 2;
        const auto x = (drop + 1) % 3, y = (drop + 2) % 3;
        const auto project = [&](const std::vector<V3> &ps) {
            std::vector<L2> out;
            room(out, n);
            for (const auto &p : ps)
                out.push_back({p[x], p[y]});
            return out;
        };
        const auto projected = project(points), flat = project(narrowed);
        const auto safe_orient = [&](const L2 &a, const L2 &b, const L2 &c) {
            const auto p = (b[0] - a[0]) * (c[1] - a[1]), q = (b[1] - a[1]) * (c[0] - a[0]);
            const auto v = orient(a, b, c);
            if (v != 0 && std::abs(v) <= 16 * std::numeric_limits<long double>::epsilon() *
                                             (std::abs(p) + std::abs(q)))
                fail("obj-uncertain", "Numerically uncertain polygon orientation");
            return v;
        };
        const auto check_ring = [&](const auto &ps) {
            for (std::size_t i = 0; i < n; ++i) {
                if (ps[i] == ps[(i + 1) % n])
                    fail("obj-polygon", "Repeated or collapsed polygon edge");
                const auto turn = safe_orient(ps[(i + n - 1) % n], ps[i], ps[(i + 1) % n]);
                if (turn == 0 && !between(ps[(i + n - 1) % n], ps[(i + 1) % n], ps[i]))
                    fail("obj-polygon", "Reversed overlapping collinear boundary");
                for (std::size_t j = i + 1; j < n; ++j) {
                    if (ps[i] == ps[j])
                        fail("obj-polygon", "Repeated polygon position");
                    if (j == i + 1 || (i == 0 && j == n - 1))
                        continue;
                    // Evaluate the error bounds even when the intersection
                    // classifier would short-circuit an obvious crossing.
                    safe_orient(ps[i], ps[(i + 1) % n], ps[j]);
                    safe_orient(ps[i], ps[(i + 1) % n], ps[(j + 1) % n]);
                    safe_orient(ps[j], ps[(j + 1) % n], ps[i]);
                    safe_orient(ps[j], ps[(j + 1) % n], ps[(i + 1) % n]);
                    if (intersects(ps[i], ps[(i + 1) % n], ps[j], ps[(j + 1) % n]))
                        fail("obj-polygon", "Self-intersecting polygon boundary");
                }
            }
        };
        check_ring(projected);
        check_ring(flat);
        // Remove collinear corners for Earcut, then reinsert each original
        // corner by splitting its boundary triangle. Thus UV/normal samples at
        // those corners are retained rather than silently discarded.
        std::vector<std::uint32_t> kept;
        room(kept, n);
        for (std::size_t i = 0; i < n; ++i)
            if (orient(projected[(i + n - 1) % n], projected[i], projected[(i + 1) % n]) != 0)
                kept.push_back(static_cast<std::uint32_t>(i));
        if (kept.size() < 3)
            fail("obj-polygon", "Polygon has fewer than three noncollinear corners");
        std::vector<V2> ring;
        room(ring, kept.size());
        for (const auto i : kept)
            ring.push_back({static_cast<double>(projected[i][0] - projected[0][0]),
                            static_cast<double>(projected[i][1] - projected[0][1])});
        charge(ring.size() * sizeof(V2));
        const std::array rings{ring}; // std::array's owned copy above.
        earcut(rings);
        if (earcut.indices.size() != (kept.size() - 2) * 3)
            fail("obj-triangulation", "Earcut did not cover every polygon corner");
        auto &triangles = built[pi].triangles;
        room(triangles, n - 2);
        const auto winding = sign(static_cast<long double>(area[drop]));
        for (std::size_t i = 0; i < earcut.indices.size(); i += 3) {
            Tri tri{};
            for (std::size_t k = 0; k < 3; ++k) {
                if (earcut.indices[i + k] >= kept.size())
                    fail("obj-triangulation", "Earcut returned an invalid corner");
                tri[k] = kept[earcut.indices[i + k]];
            }
            if (sign(orient(projected[tri[0]], projected[tri[1]], projected[tri[2]])) != winding)
                std::swap(tri[1], tri[2]);
            triangles.push_back(tri);
        }
        for (std::size_t edge = 0; edge < kept.size(); ++edge) {
            const auto a = kept[edge], b = kept[(edge + 1) % kept.size()];
            if ((a + 1) % n == b)
                continue;
            bool found = false;
            const auto count = triangles.size();
            for (std::size_t ti = 0; ti < count && !found; ++ti)
                for (std::size_t k = 0; k < 3 && !found; ++k) {
                    if (triangles[ti][k] != a || triangles[ti][(k + 1) % 3] != b)
                        continue;
                    const auto opposite = triangles[ti][(k + 2) % 3];
                    auto first = a;
                    for (auto next = static_cast<std::uint32_t>((a + 1) % n);;
                         next = static_cast<std::uint32_t>((next + 1) % n)) {
                        const Tri piece{first, next, opposite};
                        if (first == a)
                            triangles[ti] = piece;
                        else {
                            room(triangles);
                            triangles.push_back(piece);
                        }
                        first = next;
                        if (next == b)
                            break;
                    }
                    found = true;
                }
            if (!found)
                fail("obj-triangulation", "Collinear boundary corner could not be retained");
        }
        if (triangles.size() != n - 2)
            fail("obj-triangulation", "Wrong final triangulation cardinality");
        const auto certify = [&](const auto &ps) {
            if (n < 3 || ps.size() != n)
                fail("obj-triangulation", "Polygon certificate requires at least three corners");
            long double polygon_area = 0, triangle_area = 0;
            for (std::size_t i = 0; i < n; ++i)
                polygon_area += orient(ps[0], ps[i], ps[(i + 1) % n]);
            if (sign(polygon_area) != winding)
                fail("obj-triangulation", "Polygon winding changed after narrowing");
            using Edge = std::pair<std::uint32_t, std::uint32_t>;
            struct Use {
                std::size_t count = 0;
                int direction = 0;
            };
            std::map<Edge, Use> edges;
            for (const auto &tri : triangles) {
                const auto a = safe_orient(ps[tri[0]], ps[tri[1]], ps[tri[2]]);
                if (sign(a) != winding)
                    fail("obj-triangulation",
                         "Triangle collapsed or changed orientation after narrowing");
                triangle_area += a;
                const L2 centroid{(ps[tri[0]][0] + ps[tri[1]][0] + ps[tri[2]][0]) / 3,
                                  (ps[tri[0]][1] + ps[tri[1]][1] + ps[tri[2]][1]) / 3};
                bool inside = false;
                for (std::size_t i = 0, j = n - 1; i < n; j = i++) {
                    if ((ps[i][1] > centroid[1]) != (ps[j][1] > centroid[1]) &&
                        centroid[0] < (ps[j][0] - ps[i][0]) * (centroid[1] - ps[i][1]) /
                                              (ps[j][1] - ps[i][1]) +
                                          ps[i][0])
                        inside = !inside;
                }
                if (!inside)
                    fail("obj-triangulation", "Triangle falls outside its polygon");
                for (std::size_t k = 0; k < 3; ++k) {
                    const auto a_index = tri[k], b_index = tri[(k + 1) % 3];
                    const auto key = std::minmax(a_index, b_index);
                    auto it = edges.find(key);
                    if (it == edges.end()) {
                        charge(sizeof(std::pair<const Edge, Use>) + 64);
                        it = edges.emplace(key, Use{}).first;
                    }
                    ++it->second.count;
                    it->second.direction += a_index < b_index ? 1 : -1;
                }
            }
            const auto error = 128 * std::numeric_limits<long double>::epsilon() *
                               static_cast<long double>(n) * std::abs(polygon_area);
            if (std::abs(polygon_area - triangle_area) > error)
                fail("obj-triangulation", "Triangle area does not cover polygon area");
            for (std::size_t i = 0; i < n; ++i) {
                const auto a = static_cast<std::uint32_t>(i),
                           b = static_cast<std::uint32_t>((i + 1) % n);
                const auto it = edges.find(std::minmax(a, b));
                if (it == edges.end() || it->second.count != 1 ||
                    it->second.direction != (a < b ? 1 : -1))
                    fail("obj-triangulation", "Original polygon boundary is not preserved");
            }
            for (auto a = edges.begin(); a != edges.end(); ++a) {
                const auto [i, j] = a->first;
                const bool boundary = (i + 1) % n == j || (j + 1) % n == i;
                if (!boundary && (a->second.count != 2 || a->second.direction != 0))
                    fail("obj-triangulation", "Invalid internal triangle edge incidence");
                for (auto b = std::next(a); b != edges.end(); ++b) {
                    const auto [k, l] = b->first;
                    if (i == k || i == l || j == k || j == l)
                        continue;
                    if (intersects(ps[i], ps[j], ps[k], ps[l]))
                        fail("obj-triangulation", "Crossed or overlapping triangle edges");
                }
            }
        };
        certify(projected);
        certify(flat);
        built[pi].area = area;
    }
    struct Contribution {
        std::uint32_t position, smoothing;
        V3 area;
    };
    std::vector<Contribution> contributions;
    for (std::size_t pi = 0; pi < asset.polygons.size(); ++pi) {
        const auto &p = asset.polygons[pi];
        if (p.smoothing == 0)
            continue;
        for (const auto &c : p.corners) {
            room(contributions);
            contributions.push_back({c.position, p.smoothing, built[pi].area});
        }
    }
    std::ranges::sort(contributions, [](const auto &a, const auto &b) {
        return std::tie(a.position, a.smoothing) < std::tie(b.position, b.smoothing);
    });
    std::size_t write = 0;
    for (std::size_t i = 0; i < contributions.size();) {
        auto sum = contributions[i++];
        while (i < contributions.size() && contributions[i].position == sum.position &&
               contributions[i].smoothing == sum.smoothing) {
            for (std::size_t k = 0; k < 3; ++k)
                sum.area[k] += contributions[i].area[k];
            ++i;
        }
        contributions[write++] = sum;
    }
    contributions.resize(write);
    std::map<std::pair<std::int64_t, bool>, std::size_t> part_indices;
    for (std::size_t pi = 0; pi < asset.polygons.size(); ++pi) {
        const auto &p = asset.polygons[pi];
        current = p.location;
        const bool complete =
            std::ranges::all_of(p.corners, [](const auto &c) { return c.uv.has_value(); });
        const auto key = std::pair{
            p.material ? static_cast<std::int64_t>(*p.material) : std::int64_t{-1}, complete};
        auto it = part_indices.find(key);
        if (it == part_indices.end()) {
            charge(sizeof(decltype(part_indices)::value_type) + 64);
            room(asset.parts);
            const auto id = asset.parts.size();
            asset.parts.push_back({{}, p.material, {}, {}, complete});
            it = part_indices.emplace(key, id).first;
        }
        auto &part = asset.parts[it->second];
        room(part.polygons);
        part.polygons.push_back(pi);
        auto &mesh = part.geometry;
        const auto base = mesh.positions.size();
        charge_output(p.corners.size() * (sizeof(GeometryPoint) * 2 +
                                          (complete ? sizeof(std::array<float, 2>) : 0)) +
                      built[pi].triangles.size() * sizeof(Tri));
        if (base > std::numeric_limits<std::uint32_t>::max() - p.corners.size())
            fail("obj-limit", "Expanded OBJ corner indices overflow");
        for (const auto &c : p.corners) {
            room(mesh.positions);
            mesh.positions.push_back(narrow(asset.positions[c.position]));
            V3 normal{};
            if (c.normal)
                normal = asset.normals[*c.normal];
            else if (p.smoothing == 0)
                normal = unit(built[pi].area);
            else {
                const auto ci = std::lower_bound(contributions.begin(), contributions.end(),
                                                 std::pair{c.position, p.smoothing},
                                                 [](const auto &a, const auto &b) {
                                                     return std::pair{a.position, a.smoothing} < b;
                                                 });
                if (ci == contributions.end() || !(length(ci->area) > 0) ||
                    !std::isfinite(length(ci->area)))
                    fail("obj-normal", "Smoothing normals cancel or overflow");
                normal = unit(ci->area);
            }
            const auto packed_normal = narrow(normal);
            const std::array packed_components{packed_normal.x, packed_normal.y, packed_normal.z};
            for (std::size_t axis = 0; axis < normal.size(); ++axis)
                if (!std::isfinite(packed_components[axis]) ||
                    (normal[axis] != 0 && packed_components[axis] == 0))
                    fail("obj-normal", "Normal component underflows or overflows binary32");
            room(mesh.normals);
            mesh.normals.push_back(packed_normal);
            if (complete) {
                const auto &uv = asset.uvs[*c.uv];
                room(mesh.uvs);
                mesh.uvs.push_back({static_cast<float>(uv[0]), static_cast<float>(uv[1])});
            }
        }
        for (auto tri : built[pi].triangles) {
            for (auto &c : tri)
                c += static_cast<std::uint32_t>(base);
            room(mesh.triangles);
            mesh.triangles.push_back(tri);
            room(part.triangle_polygons);
            part.triangle_polygons.push_back(pi);
        }
    }
    for (auto &part : asset.parts) {
        current = asset.polygons[part.polygons.front()].location;
        // Reuse the native preparation domain, including its common scale and
        // rejection of components vulnerable to GPU flush-to-zero. Do not
        // publish a complete asset that the existing renderer cannot prepare.
        std::vector<GeometryPoint> prepared_normals;
        charge(part.geometry.normals.size() * sizeof(GeometryPoint));
        if (const auto error = prepare_shading_normals(part.geometry, GeometryTransform{}, false,
                                                       prepared_normals);
            !error.empty())
            fail("obj-normal", error + " after OBJ normal conversion");
        GeometryData checked;
        room(checked.meshes);
        checked.meshes.push_back(std::move(part.geometry));
        const auto error = validate_geometry(checked);
        part.geometry = std::move(checked.meshes.front());
        if (!error.empty())
            fail("obj-polygon", error + " after OBJ geometry conversion");
    }
}
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
Result load(const std::filesystem::path &path, const Limits &limits) {
    return Reader(limits).run(path);
}
std::string report(const Result &result) {
    std::ostringstream out;
    out << "OBJ import profile " << profile << '\n';
    for (const auto &d : result.diagnostics) {
        out << *d.location.file << ':' << d.location.line << ':' << d.location.column << ": "
            << (d.severity == Severity::error         ? "error"
                : d.severity == Severity::unsupported ? "unsupported"
                                                      : "warning")
            << " [" << d.code << "] " << d.message << '\n';
    }
    if (result.asset)
        out << result.asset->polygons.size() << " polygons, " << result.asset->parts.size()
            << " parts, " << result.asset->materials.size() << " authored materials\n";
    else
        out << "No complete imported asset\n";
    return out.str();
}
} // namespace rypath::obj
