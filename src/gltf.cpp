// SPDX-License-Identifier: GPL-3.0-only
#include "gltf.hpp"
#include "image_texture.hpp"
#include "mesh_bundle.hpp"
#include "pbrt_numeric.hpp"
#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace {
thread_local bool gltf_numeric_error = false;
long long gltf_integer(const char *s) {
    long long v = 0;
    const auto n = std::strlen(s);
    const auto r = std::from_chars(s, s + n, v);
    if (r.ec != std::errc{} || r.ptr != s + n)
        gltf_numeric_error = true;
    return v;
}
int gltf_int(const char *s) {
    const auto v = gltf_integer(s);
    if (v < std::numeric_limits<int>::min() || v > std::numeric_limits<int>::max()) {
        gltf_numeric_error = true;
        return 0;
    }
    return static_cast<int>(v);
}
double gltf_float(const char *s) {
    const auto v = rypath::pbrt::decimal_float<double>(s);
    if (!v || std::abs(*v) > std::numeric_limits<float>::max() ||
        (*v != 0 && static_cast<float>(*v) == 0)) {
        gltf_numeric_error = true;
        return 0;
    }
    return *v;
}
struct GltfAllocation {
    std::size_t remaining = 0;
    bool exhausted = false;
};
thread_local GltfAllocation *gltf_mikk_budget = nullptr;
void *gltf_allocate(void *user, std::size_t n) {
    auto &b = *static_cast<GltfAllocation *>(user);
    if (n > b.remaining) {
        b.exhausted = true;
        return nullptr;
    }
    b.remaining -= n;
    auto *p = std::malloc(n ? n : 1);
    if (!p)
        b.exhausted = true;
    return p;
}
void gltf_deallocate(void *, void *p) { std::free(p); }
void *gltf_mikk_allocate(std::size_t n) { return gltf_allocate(gltf_mikk_budget, n); }
void gltf_mikk_deallocate(void *p) { std::free(p); }
} // namespace
#define CGLTF_ATOI gltf_int
#define CGLTF_ATOLL gltf_integer
#define CGLTF_ATOF gltf_float
#define CGLTF_IMPLEMENTATION
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wconversion"
#pragma clang diagnostic ignored "-Wsign-conversion"
#pragma clang diagnostic ignored "-Wold-style-cast"
#pragma clang diagnostic ignored "-Wunused-but-set-variable"
#pragma clang diagnostic ignored "-Wgnu-anonymous-struct"
#pragma clang diagnostic ignored "-Wnested-anon-types"
#endif
#include "../third_party/cgltf/cgltf.h"
#define malloc gltf_mikk_allocate
#define free gltf_mikk_deallocate
// Include the upstream implementation to scope its allocation adaptation to this TU.
// NOLINTNEXTLINE(bugprone-suspicious-include)
#include "../third_party/mikktspace/mikktspace.c"
#undef malloc
#undef free
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

namespace rypath::gltf {
namespace {
struct Stop {};
using Matrix = std::array<double, 16>;
using Vec = std::array<double, 3>;
constexpr Matrix identity{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
Vec sub(Vec a, Vec b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
Vec cross(Vec a, Vec b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
double length(Vec v) { return std::hypot(v[0], v[1], v[2]); }
Matrix multiply(const Matrix &a, const Matrix &b) {
    Matrix c{};
    for (std::size_t i = 0; i < 4; ++i)
        for (std::size_t j = 0; j < 4; ++j)
            for (std::size_t k = 0; k < 4; ++k)
                c[4 * i + j] += a[4 * i + k] * b[4 * k + j];
    return c;
}
struct Json {
    enum Kind { object, array, string, number, boolean, null } kind = null;
    std::string key, text;
    std::vector<Json> children;
    std::size_t offset = 0;
    const Json *find(std::string_view name) const {
        for (const auto &v : children)
            if (v.key == name)
                return &v;
        return nullptr;
    }
};
struct Reader {
    const Limits &limits;
    const Options &options;
    Result result;
    Asset asset;
    GltfAllocation allocation;
    Location current;
    std::string context = "$";
    std::size_t files = 0, expanded = 0, values = 0, json_pos = 0, work_used = 0;
    std::string_view json;
    std::string canonical_json;
    std::span<const std::uint8_t> glb_bin;
    std::vector<std::size_t> line_starts;
    Json root;
    cgltf_data *data = nullptr;
    std::vector<std::vector<std::uint8_t>> buffers;
    std::vector<bool> selected;
    std::map<const cgltf_material *, std::size_t> materials;
    std::map<const cgltf_texture *, std::size_t> textures;
    std::map<const cgltf_image *, std::size_t> images;
    std::map<const cgltf_primitive *, std::size_t> primitives;
    explicit Reader(const Limits &l, const Options &o)
        : limits(l), options(o), allocation{l.working_bytes} {}
    ~Reader() {
        if (data)
            cgltf_free(data);
    }
    void where(std::size_t pos) {
        const auto offset = std::min(pos, json.size());
        const auto it = std::upper_bound(line_starts.begin(), line_starts.end(), offset);
        const auto i =
            it == line_starts.begin() ? 0 : static_cast<std::size_t>(it - line_starts.begin() - 1);
        current.line = i + 1;
        current.column = offset - (line_starts.empty() ? 0 : line_starts[i]) + 1;
    }
    void canonicalize() {
        // Replay the already decoded DOM. cgltf matches raw key/enum spellings,
        // so escaped known keys must not be silently treated as unknown fields.
        charge(product(json.size(), 2) + 128);
        mesh_bundle::Text text(json.size(), "glTF canonical JSON");
        const auto quote = [&](std::string_view value) {
            auto &o = text.stream;
            constexpr char hex[] = "0123456789abcdef";
            o << '"';
            for (const unsigned char c : value) {
                switch (c) {
                case '"':
                    o << "\\\"";
                    break;
                case '\\':
                    o << "\\\\";
                    break;
                case '\b':
                    o << "\\b";
                    break;
                case '\f':
                    o << "\\f";
                    break;
                case '\n':
                    o << "\\n";
                    break;
                case '\r':
                    o << "\\r";
                    break;
                case '\t':
                    o << "\\t";
                    break;
                default:
                    if (c < 32)
                        o << "\\u00" << hex[c >> 4] << hex[c & 15];
                    else
                        o << static_cast<char>(c);
                }
            }
            o << '"';
        };
        const auto emit = [&](auto &&self, const Json &v) -> void {
            auto &o = text.stream;
            if (v.kind == Json::object || v.kind == Json::array) {
                o << (v.kind == Json::object ? '{' : '[');
                for (std::size_t i = 0; i < v.children.size(); ++i) {
                    if (i)
                        o << ',';
                    if (v.kind == Json::object) {
                        quote(v.children[i].key);
                        o << ':';
                    }
                    self(self, v.children[i]);
                }
                o << (v.kind == Json::object ? '}' : ']');
            } else if (v.kind == Json::string)
                quote(v.text);
            else
                o << (v.kind == Json::null ? "null" : v.text);
        };
        emit(emit, root);
        canonical_json = std::move(text.value);
    }
    void diagnose(Severity s, std::string code, std::string msg) {
        if (result.diagnostics.size() >= limits.input.diagnostics) {
            if (!result.diagnostics.empty())
                result.diagnostics.back() = {Severity::error, "gltf-limit", current,
                                             "glTF diagnostic limit exceeded"};
            throw Stop{};
        }
        charge(sizeof(Diagnostic) + code.size() + msg.size() + context.size() + 256);
        result.diagnostics.push_back(
            {s, std::move(code), current, context + ": " + std::move(msg)});
    }
    [[noreturn]] void fail(std::string msg, std::string code = "gltf-invalid",
                           Severity s = Severity::error) {
        diagnose(s, std::move(code), std::move(msg));
        throw Stop{};
    }
    void charge(std::size_t n) {
        if (n > allocation.remaining) {
            // Avoid recursive allocation diagnostics when the budget itself is exhausted.
            if (result.diagnostics.size() < limits.input.diagnostics)
                result.diagnostics.push_back({Severity::error, "gltf-limit", current,
                                              "glTF working allocation limit exceeded"});
            throw Stop{};
        }
        allocation.remaining -= n;
    }
    std::size_t product(std::size_t a, std::size_t b) {
        if (b && a > std::numeric_limits<std::size_t>::max() / b)
            fail("integer size overflow");
        return a * b;
    }
    void work(std::size_t n) {
        if (n > limits.work_items - std::min(work_used, limits.work_items))
            fail("cumulative glTF decode/placement work limit exceeded", "gltf-limit");
        work_used += n;
    }
    void count(std::size_t n) {
        if (n > limits.input.total_values - std::min(values, limits.input.total_values))
            fail("glTF value limit exceeded", "gltf-limit");
        values += n;
    }
    std::vector<std::uint8_t> read_file(const std::filesystem::path &p) {
        if (++files > limits.input.expansions || limits.input.active_files == 0)
            fail("input resource count limit exceeded", "gltf-limit");
        const int fd = ::open(p.c_str(), O_RDONLY | O_NONBLOCK);
        if (fd < 0)
            fail("cannot open resource " + p.filename().string(), "gltf-file");
        struct Close {
            int fd;
            ~Close() { ::close(fd); }
        } close{fd};
        struct stat before {
        }, after{};
        if (fstat(fd, &before) != 0 || !S_ISREG(before.st_mode) || before.st_size < 0)
            fail("resource must be a regular file", "gltf-file");
        const auto n = static_cast<std::uintmax_t>(before.st_size);
        if (n > limits.input.file_bytes ||
            n > limits.input.expanded_bytes - std::min(expanded, limits.input.expanded_bytes))
            fail("input resource byte limit exceeded", "gltf-limit");
        charge(static_cast<std::size_t>(n));
        expanded += static_cast<std::size_t>(n);
        std::vector<std::uint8_t> b(static_cast<std::size_t>(n));
        std::size_t got = 0;
        while (got < b.size()) {
            const auto r = ::read(fd, b.data() + got, b.size() - got);
            if (r < 0 && errno == EINTR)
                continue;
            if (r <= 0)
                fail("resource changed or could not be read completely", "gltf-file");
            got += static_cast<std::size_t>(r);
        }
        std::uint8_t extra = 0;
        ssize_t trailing = 0;
        do {
            trailing = ::read(fd, &extra, 1);
        } while (trailing < 0 && errno == EINTR);
#if defined(__APPLE__)
        const auto changed = [](const struct stat &a, const struct stat &b) {
            return a.st_mtimespec.tv_sec != b.st_mtimespec.tv_sec ||
                   a.st_mtimespec.tv_nsec != b.st_mtimespec.tv_nsec ||
                   a.st_ctimespec.tv_sec != b.st_ctimespec.tv_sec ||
                   a.st_ctimespec.tv_nsec != b.st_ctimespec.tv_nsec;
        };
#else
        const auto changed = [](const struct stat &a, const struct stat &b) {
            return a.st_mtim.tv_sec != b.st_mtim.tv_sec || a.st_mtim.tv_nsec != b.st_mtim.tv_nsec ||
                   a.st_ctim.tv_sec != b.st_ctim.tv_sec || a.st_ctim.tv_nsec != b.st_ctim.tv_nsec;
        };
#endif
        if (trailing != 0 || fstat(fd, &after) != 0 || before.st_size != after.st_size ||
            changed(before, after))
            fail("resource changed during snapshot", "gltf-file");
        return b;
    }
    static std::uint32_t u32(const std::uint8_t *p) {
        return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) | (std::uint32_t(p[2]) << 16) |
               (std::uint32_t(p[3]) << 24);
    }
    void whitespace() {
        while (json_pos < json.size() && (json[json_pos] == ' ' || json[json_pos] == '\r' ||
                                          json[json_pos] == '\n' || json[json_pos] == '\t'))
            ++json_pos;
    }
    [[noreturn]] void json_fail(std::string msg) {
        where(json_pos);
        fail(std::move(msg), "gltf-json");
    }
    static int hex(char c) {
        return c >= '0' && c <= '9'   ? c - '0'
               : c >= 'a' && c <= 'f' ? c - 'a' + 10
               : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                      : -1;
    }
    unsigned unicode() {
        if (json.size() - json_pos < 4)
            json_fail("truncated Unicode escape");
        unsigned u = 0;
        for (unsigned i = 0; i < 4; ++i) {
            const auto h = hex(json[json_pos++]);
            if (h < 0)
                json_fail("invalid Unicode escape");
            u = (u << 4) | static_cast<unsigned>(h);
        }
        return u;
    }
    std::string string() {
        if (json_pos == json.size() || json[json_pos] != '"')
            json_fail("expected string");
        ++json_pos;
        // Reserve and charge before materializing any decoded string. Unicode escapes
        // never expand beyond their encoded byte length.
        auto end = json_pos;
        while (end < json.size() && json[end] != '"') {
            if (json[end] == '\\' && end + 1 < json.size())
                ++end;
            ++end;
        }
        if (end == json.size())
            json_fail("unterminated string");
        const auto raw_size = end - json_pos;
        if (raw_size > limits.input.token_bytes)
            json_fail("string exceeds token limit");
        charge(raw_size + 32);
        std::string s;
        s.reserve(raw_size);
        while (json_pos < json.size()) {
            const auto c = static_cast<unsigned char>(json[json_pos++]);
            if (c == '"') {
                if (!mesh_bundle::valid_utf8(s))
                    json_fail("invalid UTF-8 string");
                return s;
            }
            if (c < 32)
                json_fail("control character in string");
            if (c != '\\')
                s.push_back(static_cast<char>(c));
            else {
                if (json_pos == json.size())
                    json_fail("truncated string escape");
                const auto e = json[json_pos++];
                switch (e) {
                case '"':
                case '\\':
                case '/':
                    s.push_back(e);
                    break;
                case 'b':
                    s.push_back('\b');
                    break;
                case 'f':
                    s.push_back('\f');
                    break;
                case 'n':
                    s.push_back('\n');
                    break;
                case 'r':
                    s.push_back('\r');
                    break;
                case 't':
                    s.push_back('\t');
                    break;
                case 'u': {
                    auto u = unicode();
                    if (u >= 0xd800 && u <= 0xdbff) {
                        if (json.size() - json_pos < 2 || json[json_pos] != '\\' ||
                            json[json_pos + 1] != 'u')
                            json_fail("missing low surrogate");
                        json_pos += 2;
                        const auto lo = unicode();
                        if (lo < 0xdc00 || lo > 0xdfff)
                            json_fail("invalid low surrogate");
                        u = 0x10000 + ((u - 0xd800) << 10) + (lo - 0xdc00);
                    } else if (u >= 0xdc00 && u <= 0xdfff)
                        json_fail("unpaired low surrogate");
                    if (u == 0)
                        json_fail("NUL strings are outside the import profile");
                    if (u < 128)
                        s.push_back(static_cast<char>(u));
                    else if (u < 2048) {
                        s.push_back(static_cast<char>(0xc0 | (u >> 6)));
                        s.push_back(static_cast<char>(0x80 | (u & 63)));
                    } else if (u < 65536) {
                        s.push_back(static_cast<char>(0xe0 | (u >> 12)));
                        s.push_back(static_cast<char>(0x80 | ((u >> 6) & 63)));
                        s.push_back(static_cast<char>(0x80 | (u & 63)));
                    } else {
                        s.push_back(static_cast<char>(0xf0 | (u >> 18)));
                        s.push_back(static_cast<char>(0x80 | ((u >> 12) & 63)));
                        s.push_back(static_cast<char>(0x80 | ((u >> 6) & 63)));
                        s.push_back(static_cast<char>(0x80 | (u & 63)));
                    }
                    break;
                }
                default:
                    json_fail("invalid string escape");
                }
            }
            if (s.size() > limits.input.token_bytes)
                json_fail("string exceeds token limit");
        }
        json_fail("unterminated string");
    }
    Json parse(std::size_t depth = 0) {
        whitespace();
        if (depth > std::min<std::size_t>(limits.input.scopes, 256))
            json_fail("JSON nesting limit exceeded");
        work(1);
        charge(2 * sizeof(Json) + 64);
        count(1);
        Json v;
        v.offset = json_pos;
        if (json_pos == json.size())
            json_fail("missing value");
        const auto c = json[json_pos];
        if (c == '{' || c == '[') {
            v.kind = c == '{' ? Json::object : Json::array;
            ++json_pos;
            whitespace();
            const auto end = c == '{' ? '}' : ']';
            if (json_pos < json.size() && json[json_pos] == end) {
                ++json_pos;
                return v;
            }
            std::set<std::string> keys;
            for (;;) {
                std::string key;
                if (c == '{') {
                    key = string();
                    charge(2 * key.size() + 96);
                    if (!keys.insert(key).second)
                        json_fail("duplicate object key");
                    whitespace();
                    if (json_pos == json.size() || json[json_pos] != ':')
                        json_fail("missing colon");
                    ++json_pos;
                }
                auto child = parse(depth + 1);
                child.key = std::move(key);
                v.children.push_back(std::move(child));
                if (v.children.size() > limits.input.list_values)
                    json_fail("JSON list limit exceeded");
                whitespace();
                if (json_pos == json.size())
                    json_fail("unterminated container");
                const auto sep = json[json_pos++];
                if (sep == end)
                    break;
                if (sep != ',')
                    json_fail("missing comma");
                whitespace();
            }
        } else if (c == '"') {
            v.kind = Json::string;
            v.text = string();
        } else if (c == 't' || c == 'f' || c == 'n') {
            const std::string_view token = c == 't' ? "true" : c == 'f' ? "false" : "null";
            if (json.substr(json_pos, token.size()) != token)
                json_fail("invalid literal");
            v.kind = c == 'n' ? Json::null : Json::boolean;
            v.text = token;
            json_pos += token.size();
        } else {
            v.kind = Json::number;
            const auto start = json_pos;
            if (json[json_pos] == '-')
                ++json_pos;
            if (json_pos == json.size())
                json_fail("truncated number");
            if (json[json_pos] == '0')
                ++json_pos;
            else {
                if (json[json_pos] < '1' || json[json_pos] > '9')
                    json_fail("invalid number");
                while (json_pos < json.size() && json[json_pos] >= '0' && json[json_pos] <= '9')
                    ++json_pos;
            }
            if (json_pos < json.size() && json[json_pos] == '.') {
                ++json_pos;
                const auto p = json_pos;
                while (json_pos < json.size() && json[json_pos] >= '0' && json[json_pos] <= '9')
                    ++json_pos;
                if (p == json_pos)
                    json_fail("missing fractional digits");
            }
            if (json_pos < json.size() && (json[json_pos] == 'e' || json[json_pos] == 'E')) {
                ++json_pos;
                if (json_pos < json.size() && (json[json_pos] == '+' || json[json_pos] == '-'))
                    ++json_pos;
                const auto p = json_pos;
                while (json_pos < json.size() && json[json_pos] >= '0' && json[json_pos] <= '9')
                    ++json_pos;
                if (p == json_pos)
                    json_fail("missing exponent digits");
            }
            if (json_pos - start > std::min<std::size_t>(limits.input.token_bytes, 127))
                json_fail("numeric token exceeds checked decoder limit");
            charge(json_pos - start + 32);
            v.text = std::string(json.substr(start, json_pos - start));
            if (!pbrt::decimal_float<double>(v.text))
                json_fail("nonfinite or unrepresentable number");
        }
        return v;
    }
    const Json &field(const Json &v, std::string_view name, Json::Kind kind, bool required = true) {
        const auto *f = v.find(name);
        if (!f) {
            if (required)
                fail("missing field " + std::string(name));
            static const Json empty;
            return empty;
        }
        if (f->kind != kind) {
            where(f->offset);
            fail("wrong type for " + std::string(name));
        }
        return *f;
    }
    std::size_t integer(const Json &v) {
        if (v.kind != Json::number || v.text.empty() || v.text[0] == '-' ||
            v.text.find_first_of(".eE") != std::string::npos)
            fail("expected nonnegative integer");
        std::size_t n = 0;
        auto r = std::from_chars(v.text.data(), v.text.data() + v.text.size(), n);
        if (r.ec != std::errc{} || r.ptr != v.text.data() + v.text.size() ||
            n > static_cast<std::size_t>(std::numeric_limits<long long>::max()))
            fail("integer out of range");
        return n;
    }
    void index(const Json &v, std::string_view name, std::size_t bound, bool required = false) {
        if (auto *f = v.find(name)) {
            if (integer(*f) >= bound)
                fail("out-of-range " + std::string(name));
        } else if (required)
            fail("missing index " + std::string(name));
    }
    std::size_t array_size(std::string_view name) {
        auto *v = root.find(name);
        if (!v)
            return 0;
        if (v->kind != Json::array)
            fail("expected array " + std::string(name));
        for (const auto &e : v->children)
            if (e.kind != Json::object)
                fail("expected object in " + std::string(name));
        return v->children.size();
    }
    template <class F> void each(std::string_view name, F f) {
        if (auto *v = root.find(name)) {
            for (std::size_t i = 0; i < v->children.size(); ++i) {
                context = "$." + std::string(name) + "[" + std::to_string(i) + "]";
                where(v->children[i].offset);
                f(v->children[i]);
            }
        }
    }
    void numeric(const Json &v, std::string_view name, bool positive = false) {
        if (auto *f = v.find(name)) {
            const auto n = integer(*f);
            if (positive && n == 0)
                fail("zero " + std::string(name));
        }
    }
    void vector(const Json &v, std::string_view name, std::size_t count_) {
        if (auto *a = v.find(name)) {
            if (a->kind != Json::array || a->children.size() != count_)
                fail("invalid " + std::string(name) + " vector size");
            for (auto &e : a->children)
                if (e.kind != Json::number)
                    fail("nonnumeric " + std::string(name));
        }
    }
    double real(const Json &v) {
        if (v.kind != Json::number)
            fail("expected numeric value");
        const auto parsed = pbrt::decimal_float<double>(v.text);
        if (!parsed)
            fail("numeric value is not representable as binary64");
        return *parsed;
    }
    void unit_field(const Json &v, std::string_view key) {
        if (auto *f = v.find(key)) {
            const auto n = real(*f);
            if (n < 0 || n > 1)
                fail("factor outside [0,1]: " + std::string(key));
        }
    }
    void unit_vector(const Json &v, std::string_view key, std::size_t n) {
        vector(v, key, n);
        if (auto *f = v.find(key))
            for (auto &x : f->children)
                if (real(x) < 0 || real(x) > 1)
                    fail("factor outside [0,1]: " + std::string(key));
    }
    void extension_required() {
        for (const auto key : {"extensionsRequired", "extensionsUsed"})
            if (auto *e = root.find(key)) {
                if (e->kind != Json::array)
                    fail("extension names must be an array");
                std::set<std::string> seen;
                for (auto &s : e->children) {
                    if (s.kind != Json::string || !seen.insert(s.text).second)
                        fail("invalid or duplicate extension name");
                }
            }
        if (auto *req = root.find("extensionsRequired"))
            for (auto &s : req->children) {
                const auto *used = root.find("extensionsUsed");
                if (!used || std::none_of(used->children.begin(), used->children.end(),
                                          [&](const Json &x) { return x.text == s.text; }))
                    fail("required extension is not listed in extensionsUsed");
                fail("unsupported required extension " + s.text, "gltf-extension",
                     Severity::unsupported);
            }
    }
    void schema() {
        if (root.kind != Json::object)
            fail("root must be an object");
        const auto &meta = field(root, "asset", Json::object);
        const auto &version = field(meta, "version", Json::string);
        if (version.text != "2.0")
            fail("only glTF 2.0 is supported", "gltf-version", Severity::unsupported);
        if (auto *min = meta.find("minVersion"))
            if (min->kind != Json::string || min->text != "2.0")
                fail("unsupported minVersion", "gltf-version", Severity::unsupported);
        const auto nb = array_size("buffers"), nv = array_size("bufferViews"),
                   na = array_size("accessors"), ni = array_size("images"),
                   ns = array_size("samplers"), nt = array_size("textures"),
                   nm = array_size("materials"), nmesh = array_size("meshes"),
                   nn = array_size("nodes"), nscene = array_size("scenes"),
                   nskin = array_size("skins"), ncamera = array_size("cameras");
        array_size("animations");
        index(root, "scene", nscene);
        each("buffers", [&](const Json &v) {
            integer(field(v, "byteLength", Json::number));
            if (v.find("uri"))
                field(v, "uri", Json::string);
        });
        each("bufferViews", [&](const Json &v) {
            index(v, "buffer", nb, true);
            numeric(v, "byteOffset");
            integer(field(v, "byteLength", Json::number));
            numeric(v, "byteStride", true);
            if (auto *t = v.find("target")) {
                const auto x = integer(*t);
                if (x != 34962 && x != 34963)
                    fail("invalid bufferView target enum");
            }
        });
        each("accessors", [&](const Json &v) {
            index(v, "bufferView", nv);
            numeric(v, "byteOffset");
            integer(field(v, "count", Json::number));
            const auto component_type = integer(field(v, "componentType", Json::number));
            if (component_type != 5120 && component_type != 5121 && component_type != 5122 &&
                component_type != 5123 && component_type != 5125 && component_type != 5126)
                fail("invalid accessor componentType");
            const auto &type = field(v, "type", Json::string).text;
            const auto dimensions = type == "SCALAR"                   ? 1u
                                    : type == "VEC2"                   ? 2u
                                    : type == "VEC3"                   ? 3u
                                    : type == "VEC4" || type == "MAT2" ? 4u
                                    : type == "MAT3"                   ? 9u
                                    : type == "MAT4"                   ? 16u
                                                                       : 0u;
            if (!dimensions)
                fail("invalid accessor type");
            vector(v, "min", dimensions);
            vector(v, "max", dimensions);
            if (auto *lo = v.find("min"))
                if (auto *hi = v.find("max"))
                    for (std::size_t i = 0; i < dimensions; ++i)
                        if (real(lo->children[i]) > real(hi->children[i]))
                            fail("accessor min exceeds max");
            if (v.find("normalized"))
                field(v, "normalized", Json::boolean);
            if (auto *s = v.find("sparse")) {
                if (s->kind != Json::object)
                    fail("invalid sparse object");
                integer(field(*s, "count", Json::number));
                auto &i = field(*s, "indices", Json::object);
                index(i, "bufferView", nv, true);
                integer(field(i, "componentType", Json::number));
                numeric(i, "byteOffset");
                auto &val = field(*s, "values", Json::object);
                index(val, "bufferView", nv, true);
                numeric(val, "byteOffset");
            }
        });
        each("images", [&](const Json &v) {
            const bool uri = v.find("uri"), view = v.find("bufferView");
            if (uri == view)
                fail("image requires exactly one URI or bufferView");
            if (uri)
                field(v, "uri", Json::string);
            if (view) {
                index(v, "bufferView", nv, true);
                field(v, "mimeType", Json::string);
            }
            if (v.find("mimeType"))
                field(v, "mimeType", Json::string);
        });
        each("samplers", [&](const Json &v) {
            for (auto k : {"minFilter", "magFilter", "wrapS", "wrapT"}) {
                if (auto *f = v.find(k)) {
                    const auto x = integer(*f);
                    const auto key = std::string_view(k);
                    if (key == "minFilter"   ? (x != 9728 && x != 9729 && (x < 9984 || x > 9987))
                        : key == "magFilter" ? (x != 9728 && x != 9729)
                                             : (x != 10497 && x != 33071 && x != 33648))
                        fail("invalid sampler enum");
                }
            }
        });
        each("textures", [&](const Json &v) {
            index(v, "source", ni);
            index(v, "sampler", ns);
        });
        const auto texinfo = [&](const Json &v, std::string_view key) {
            if (auto *t = v.find(key)) {
                if (t->kind != Json::object)
                    fail("texture info must be an object");
                index(*t, "index", nt, true);
                numeric(*t, "texCoord");
                if (t->find("scale"))
                    field(*t, "scale", Json::number);
                if (t->find("strength"))
                    field(*t, "strength", Json::number);
            }
        };
        each("materials", [&](const Json &v) {
            if (auto *p = v.find("pbrMetallicRoughness")) {
                if (p->kind != Json::object)
                    fail("invalid PBR object");
                unit_vector(*p, "baseColorFactor", 4);
                unit_field(*p, "metallicFactor");
                unit_field(*p, "roughnessFactor");
                if (p->find("metallicFactor"))
                    field(*p, "metallicFactor", Json::number);
                if (p->find("roughnessFactor"))
                    field(*p, "roughnessFactor", Json::number);
                texinfo(*p, "baseColorTexture");
                texinfo(*p, "metallicRoughnessTexture");
            }
            unit_vector(v, "emissiveFactor", 3);
            texinfo(v, "normalTexture");
            texinfo(v, "emissiveTexture");
            texinfo(v, "occlusionTexture");
            if (v.find("doubleSided"))
                field(v, "doubleSided", Json::boolean);
            if (v.find("alphaMode")) {
                auto &a = field(v, "alphaMode", Json::string);
                if (a.text != "OPAQUE" && a.text != "MASK" && a.text != "BLEND")
                    fail("invalid alphaMode");
            }
            if (auto *f = v.find("alphaCutoff"))
                if (real(*f) < 0)
                    fail("negative alphaCutoff");
            if (auto *f = v.find("occlusionTexture"))
                unit_field(*f, "strength");
        });
        each("meshes", [&](const Json &v) {
            auto &p = field(v, "primitives", Json::array);
            if (p.children.empty())
                fail("empty mesh primitive list");
            for (auto &pr : p.children) {
                if (pr.kind != Json::object)
                    fail("primitive must be an object");
                auto &at = field(pr, "attributes", Json::object);
                for (auto &a : at.children)
                    if (integer(a) >= na)
                        fail("attribute accessor out of range");
                index(pr, "indices", na);
                index(pr, "material", nm);
                if (auto *mode = pr.find("mode"))
                    if (integer(*mode) > 6)
                        fail("invalid primitive mode enum");
                if (auto *targets = pr.find("targets")) {
                    if (targets->kind != Json::array)
                        fail("invalid targets");
                    for (auto &t : targets->children) {
                        if (t.kind != Json::object)
                            fail("invalid target");
                        for (auto &a : t.children)
                            if (integer(a) >= na)
                                fail("target accessor out of range");
                    }
                }
            }
        });
        each("nodes", [&](const Json &v) {
            index(v, "mesh", nmesh);
            index(v, "skin", nskin);
            index(v, "camera", ncamera);
            vector(v, "matrix", 16);
            vector(v, "translation", 3);
            vector(v, "rotation", 4);
            vector(v, "scale", 3);
            if (v.find("matrix") &&
                (v.find("translation") || v.find("rotation") || v.find("scale")))
                fail("matrix and TRS are mutually exclusive");
            if (auto *c = v.find("children")) {
                if (c->kind != Json::array)
                    fail("children must be array");
                for (auto &i : c->children)
                    if (integer(i) >= nn)
                        fail("child node out of range");
            }
        });
        each("scenes", [&](const Json &v) {
            if (auto *n = v.find("nodes")) {
                if (n->kind != Json::array)
                    fail("scene nodes must be array");
                for (auto &i : n->children)
                    if (integer(i) >= nn)
                        fail("scene node out of range");
            }
        });
        each("cameras", [&](const Json &v) {
            const auto &type = field(v, "type", Json::string).text;
            if (type == "perspective") {
                const auto &p = field(v, "perspective", Json::object);
                const auto near = real(field(p, "znear", Json::number));
                const auto yfov = real(field(p, "yfov", Json::number));
                if (!(near > 0) || !(yfov > 0 && yfov < 3.14159265358979323846))
                    fail("invalid perspective camera range");
                if (auto *f = p.find("zfar"))
                    if (!(real(*f) > near))
                        fail("invalid perspective zfar");
                if (auto *f = p.find("aspectRatio"))
                    if (!(real(*f) > 0))
                        fail("invalid camera aspectRatio");
            } else if (type == "orthographic") {
                const auto &p = field(v, "orthographic", Json::object);
                const auto near = real(field(p, "znear", Json::number));
                const auto far = real(field(p, "zfar", Json::number));
                if (real(field(p, "xmag", Json::number)) == 0 ||
                    real(field(p, "ymag", Json::number)) == 0 || near < 0 || !(far > near))
                    fail("invalid orthographic camera range");
            } else
                fail("invalid camera type");
        });
        each("skins", [&](const Json &v) {
            index(v, "inverseBindMatrices", na);
            index(v, "skeleton", nn);
            for (auto &i : field(v, "joints", Json::array).children)
                if (integer(i) >= nn)
                    fail("skin joint out of range");
        });
        each("animations", [&](const Json &v) {
            auto &sam = field(v, "samplers", Json::array);
            for (auto &s : sam.children) {
                index(s, "input", na, true);
                index(s, "output", na, true);
            }
            for (auto &c : field(v, "channels", Json::array).children) {
                index(c, "sampler", sam.children.size(), true);
                auto &t = field(c, "target", Json::object);
                index(t, "node", nn);
                field(t, "path", Json::string);
            }
        });
        context = "$";
        extension_required();
    }
    std::string decoded(char *s) {
        if (!s)
            return {};
        std::string out = s;
        cgltf_decode_string(out.data());
        out.resize(std::strlen(out.c_str()));
        return out;
    }
    std::vector<std::uint8_t> resource(std::string uri, std::string_view media = {}) {
        if (uri.starts_with("data:")) {
            const auto comma = uri.find(',');
            if (comma == std::string::npos)
                fail("malformed data URI");
            const auto head = uri.substr(5, comma - 5);
            if (head != "application/octet-stream;base64" &&
                head != "application/gltf-buffer;base64" && head != "image/png;base64" &&
                head != "image/jpeg;base64")
                fail("unsupported data URI media/encoding", "gltf-uri", Severity::unsupported);
            if (!media.empty() && head != std::string(media) + ";base64")
                fail("data URI media type mismatch");
            const auto body = std::string_view(uri).substr(comma + 1);
            if (body.size() % 4)
                fail("invalid base64 length");
            std::size_t size = product(body.size() / 4, 3);
            if (!body.empty() && body.back() == '=')
                --size;
            if (body.size() > 1 && body[body.size() - 2] == '=')
                --size;
            if (size > limits.input.file_bytes ||
                size >
                    limits.input.expanded_bytes - std::min(expanded, limits.input.expanded_bytes))
                fail("data URI byte limit exceeded", "gltf-limit");
            charge(size);
            expanded += size;
            std::vector<std::uint8_t> out;
            out.reserve(size);
            const auto digit = [](char c) -> int {
                return c >= 'A' && c <= 'Z'   ? c - 'A'
                       : c >= 'a' && c <= 'z' ? c - 'a' + 26
                       : c >= '0' && c <= '9' ? c - '0' + 52
                       : c == '+'             ? 62
                       : c == '/'             ? 63
                                              : -1;
            };
            for (std::size_t i = 0; i < body.size(); i += 4) {
                unsigned x = 0;
                unsigned pad = 0;
                for (unsigned j = 0; j < 4; ++j) {
                    const char c = body[i + j];
                    if (c == '=') {
                        if (i + 4 != body.size() || j < 2)
                            fail("invalid base64 padding");
                        ++pad;
                        x <<= 6;
                    } else {
                        const auto d = digit(c);
                        if (d < 0 || pad)
                            fail("invalid base64 character");
                        x = (x << 6) | static_cast<unsigned>(d);
                    }
                }
                if (pad > 2 || (pad == 1 && (x & 255)) || (pad == 2 && (x & 65535)))
                    fail("noncanonical base64 padding bits");
                out.push_back(static_cast<std::uint8_t>(x >> 16));
                if (pad < 2)
                    out.push_back(static_cast<std::uint8_t>(x >> 8));
                if (!pad)
                    out.push_back(static_cast<std::uint8_t>(x));
            }
            return out;
        }
        std::string path;
        for (std::size_t i = 0; i < uri.size(); ++i) {
            unsigned char c = static_cast<unsigned char>(uri[i]);
            if (c == '%') {
                if (uri.size() - i < 3 || hex(uri[i + 1]) < 0 || hex(uri[i + 2]) < 0)
                    fail("invalid URI escape", "gltf-uri");
                c = static_cast<unsigned char>(16 * hex(uri[i + 1]) + hex(uri[i + 2]));
                i += 2;
            }
            if (c < 32 || c == 127 || c == '\\' || c == ':' || c == '?' || c == '#')
                fail("unsupported local URI character", "gltf-uri");
            path.push_back(static_cast<char>(c));
        }
        const std::filesystem::path relative(path);
        if (relative.empty() || relative.is_absolute() || !mesh_bundle::valid_utf8(path))
            fail("resource URI must be a UTF-8 relative local path", "gltf-uri");
        // Deliberate local-resource restriction, independent of glTF syntax support.
        for (const auto &part : relative)
            if (part == "..")
                fail("parent traversal is outside the local resource profile", "gltf-uri",
                     Severity::unsupported);
        const auto full = (asset.entry.parent_path() / relative).lexically_normal();
        for (const auto &r : asset.resources)
            if (r.path == full) {
                charge(r.bytes.size());
                return r.bytes;
            }
        auto b = read_file(full);
        charge(b.size() + sizeof(Resource) + uri.size() + full.native().size() + 128);
        asset.resources.push_back({std::move(uri), full, b});
        return b;
    }
    void range(const cgltf_buffer_view *v, std::size_t off, std::size_t count_, std::size_t stride,
               std::size_t elem) {
        if (!v || !v->buffer)
            fail("missing buffer view storage");
        if (off > v->size ||
            (count_ && (elem > v->size - off || (count_ - 1) > (v->size - off - elem) / stride)))
            fail("accessor byte range exceeds bufferView");
    }
    const std::uint8_t *view(const cgltf_buffer_view *v) {
        if (!v || !v->buffer || !v->buffer->data)
            fail("missing buffer bytes");
        return static_cast<const std::uint8_t *>(v->buffer->data) + v->offset;
    }
    std::uint32_t unsigned_component(const std::uint8_t *p, cgltf_component_type t) {
        if (t == cgltf_component_type_r_8u)
            return p[0];
        if (t == cgltf_component_type_r_16u)
            return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8);
        if (t == cgltf_component_type_r_32u)
            return u32(p);
        fail("invalid unsigned component type");
    }
    double component(const std::uint8_t *p, cgltf_component_type t, bool normalized) {
        switch (t) {
        case cgltf_component_type_r_32f: {
            const auto f = std::bit_cast<float>(u32(p));
            if (!std::isfinite(f))
                fail("nonfinite accessor component");
            return f;
        }
        case cgltf_component_type_r_8: {
            const auto n = std::bit_cast<std::int8_t>(p[0]);
            return normalized ? std::max(-1., n / 127.) : n;
        }
        case cgltf_component_type_r_16: {
            const auto u =
                static_cast<std::uint16_t>(std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8));
            const auto n = std::bit_cast<std::int16_t>(u);
            return normalized ? std::max(-1., n / 32767.) : n;
        }
        case cgltf_component_type_r_8u:
        case cgltf_component_type_r_16u:
        case cgltf_component_type_r_32u: {
            const auto n = unsigned_component(p, t);
            return normalized ? n / (t == cgltf_component_type_r_8u    ? 255.
                                     : t == cgltf_component_type_r_16u ? 65535.
                                                                       : 4294967295.)
                              : n;
        }
        default:
            fail("invalid accessor component type");
        }
    }
    void storage() {
        charge(product(data->buffers_count, sizeof(std::vector<std::uint8_t>)));
        buffers.resize(data->buffers_count);
        for (std::size_t i = 0; i < data->buffers_count; ++i) {
            context = "$.buffers[" + std::to_string(i) + "]";
            auto &b = data->buffers[i];
            if (b.size == 0)
                fail("empty buffer");
            if (b.uri) {
                const auto uri = decoded(b.uri);
                if (uri.starts_with("data:") &&
                    !uri.starts_with("data:application/octet-stream;base64,") &&
                    !uri.starts_with("data:application/gltf-buffer;base64,"))
                    fail("buffer data URI requires an application buffer media type");
                buffers[i] = resource(uri);
            } else {
                if (!asset.binary || i != 0 || !data->bin)
                    fail("buffer has no URI or GLB BIN");
                if (b.size > data->bin_size || data->bin_size - b.size > 3)
                    fail("GLB BIN length disagrees with buffer");
                charge(data->bin_size);
                buffers[i].assign(static_cast<const std::uint8_t *>(data->bin),
                                  static_cast<const std::uint8_t *>(data->bin) + data->bin_size);
            }
            if (buffers[i].size() < b.size)
                fail("buffer is shorter than byteLength");
            b.data = buffers[i].data();
            b.data_free_method = cgltf_data_free_method_none;
        }
        for (std::size_t i = 0; i < data->buffer_views_count; ++i) {
            context = "$.bufferViews[" + std::to_string(i) + "]";
            const auto &v = data->buffer_views[i];
            if (!v.buffer || v.size == 0 || v.offset > v.buffer->size ||
                v.size > v.buffer->size - v.offset)
                fail("bufferView range exceeds buffer");
            if (v.stride && (v.stride < 4 || v.stride > 252 || v.stride % 4))
                fail("invalid vertex byteStride");
        }
        for (std::size_t i = 0; i < data->accessors_count; ++i) {
            context = "$.accessors[" + std::to_string(i) + "]";
            auto &a = data->accessors[i];
            const auto cs = cgltf_component_size(a.component_type),
                       elem = cgltf_calc_size(a.type, a.component_type);
            if (!cs || !elem || !a.count || a.count > limits.input.total_values)
                fail("invalid accessor type or count");
            if (a.offset % cs)
                fail("misaligned accessor byteOffset");
            if (a.normalized && (a.component_type == cgltf_component_type_r_32f ||
                                 a.component_type == cgltf_component_type_r_32u))
                fail("invalid normalized component type");
            if (a.buffer_view) {
                if (a.stride < elem || a.stride % cs || (a.buffer_view->offset + a.offset) % cs)
                    fail("invalid accessor stride/alignment");
                range(a.buffer_view, a.offset, a.count, a.stride, elem);
            } else if (a.offset)
                fail("byteOffset without bufferView");
            if (a.is_sparse) {
                auto &s = a.sparse;
                const auto sc = cgltf_component_size(s.indices_component_type);
                if (s.indices_component_type != cgltf_component_type_r_8u &&
                    s.indices_component_type != cgltf_component_type_r_16u &&
                    s.indices_component_type != cgltf_component_type_r_32u)
                    fail("invalid sparse index type");
                if (!s.count || s.count > a.count || s.indices_byte_offset % sc ||
                    s.values_byte_offset % cs ||
                    (s.indices_buffer_view->offset + s.indices_byte_offset) % sc ||
                    (s.values_buffer_view->offset + s.values_byte_offset) % cs)
                    fail("invalid sparse count/alignment");
                if (s.indices_buffer_view->stride || s.values_buffer_view->stride)
                    fail("sparse bufferViews cannot have byteStride");
                range(s.indices_buffer_view, s.indices_byte_offset, s.count, sc, sc);
                range(s.values_buffer_view, s.values_byte_offset, s.count, elem, elem);
                work(s.count);
                std::uint32_t previous = 0;
                for (std::size_t j = 0; j < s.count; ++j) {
                    const auto idx = unsigned_component(view(s.indices_buffer_view) +
                                                            s.indices_byte_offset + j * sc,
                                                        s.indices_component_type);
                    if (idx >= a.count || (j && idx <= previous))
                        fail("sparse indices must be increasing, unique and in range");
                    previous = idx;
                }
            }
        }
    }
    void selected_view(const cgltf_buffer_view *v) {
        if (!v || !v->buffer)
            fail("selected storage has no buffer");
        if (v->extensions_count || v->has_meshopt_compression || v->buffer->extensions_count)
            fail("selected buffer/bufferView extensions unsupported", "gltf-extension",
                 Severity::unsupported);
    }
    std::vector<double> accessor(const cgltf_accessor &a, cgltf_type type) {
        if (a.type != type)
            fail("accessor shape disagrees with attribute semantic");
        if (a.extensions_count)
            fail("selected accessor extensions unsupported", "gltf-extension",
                 Severity::unsupported);
        const auto n = cgltf_num_components(a.type), cs = cgltf_component_size(a.component_type),
                   elem = cgltf_calc_size(a.type, a.component_type);
        work(product(a.count, n));
        if (a.is_sparse)
            work(product(a.sparse.count, n + 1));
        count(product(a.count, n));
        charge(product(product(a.count, n), sizeof(double)));
        std::vector<double> out(a.count * n, 0);
        const auto copy = [&](const std::uint8_t *src, std::size_t dst) {
            for (std::size_t k = 0; k < n; ++k)
                out[dst * n + k] = component(src + k * cs, a.component_type, a.normalized != 0);
        };
        if (a.buffer_view) {
            selected_view(a.buffer_view);
            for (std::size_t i = 0; i < a.count; ++i)
                copy(view(a.buffer_view) + a.offset + i * a.stride, i);
        }
        if (a.is_sparse) {
            const auto &s = a.sparse;
            selected_view(s.indices_buffer_view);
            selected_view(s.values_buffer_view);
            const auto sc = cgltf_component_size(s.indices_component_type);
            for (std::size_t j = 0; j < s.count; ++j) {
                const auto idx =
                    unsigned_component(view(s.indices_buffer_view) + s.indices_byte_offset + j * sc,
                                       s.indices_component_type);
                copy(view(s.values_buffer_view) + s.values_byte_offset + j * elem, idx);
            }
        }
        return out;
    }
    const Json &node_json(std::size_t i) { return root.find("nodes")->children[i]; }
    Matrix local(std::size_t i) {
        const auto &j = node_json(i);
        Matrix m = identity;
        const auto vals = [&](std::string_view key, auto &out) {
            if (auto *a = j.find(key))
                for (std::size_t k = 0; k < a->children.size(); ++k)
                    out[k] = real(a->children[k]);
        };
        if (j.find("matrix")) {
            std::array<double, 16> col{};
            vals("matrix", col);
            for (std::size_t r = 0; r < 4; ++r)
                for (std::size_t c = 0; c < 4; ++c)
                    m[4 * r + c] = col[4 * c + r];
            if (m[12] != 0 || m[13] != 0 || m[14] != 0 || m[15] != 1)
                fail("node matrix must be affine");
        } else {
            std::array<double, 3> t{}, s{1, 1, 1};
            std::array<double, 4> q{0, 0, 0, 1};
            vals("translation", t);
            vals("scale", s);
            vals("rotation", q);
            const auto norm = std::hypot(std::hypot(q[0], q[1]), std::hypot(q[2], q[3]));
            if (std::abs(norm - 1) > 1e-5)
                fail("rotation quaternion must have unit length");
            for (auto &x : q)
                x /= norm;
            const auto x = q[0], y = q[1], z = q[2], w = q[3];
            m = {(1 - 2 * y * y - 2 * z * z) * s[0],
                 (2 * x * y - 2 * z * w) * s[1],
                 (2 * x * z + 2 * y * w) * s[2],
                 t[0],
                 (2 * x * y + 2 * z * w) * s[0],
                 (1 - 2 * x * x - 2 * z * z) * s[1],
                 (2 * y * z - 2 * x * w) * s[2],
                 t[1],
                 (2 * x * z - 2 * y * w) * s[0],
                 (2 * y * z + 2 * x * w) * s[1],
                 (1 - 2 * x * x - 2 * y * y) * s[2],
                 t[2],
                 0,
                 0,
                 0,
                 1};
        }
        return m;
    }
    void validate_matrix(const Matrix &m) {
        pbrt::Transform t;
        for (std::size_t i = 0; i < 16; ++i) {
            if (!std::isfinite(m[i]))
                fail("nonfinite composed transform");
            t.values[i] = m[i];
        }
        if (!pbrt::exact_linear_determinant_sign(t))
            fail("singular node placement");
    }
    void hierarchy() {
        charge(product(data->nodes_count, 3 * sizeof(std::size_t) + 2));
        std::vector<std::size_t> parents(data->nodes_count, 0);
        std::vector<std::size_t> heights(data->nodes_count, 0);
        std::vector<unsigned char> state(data->nodes_count, 0);
        for (std::size_t i = 0; i < data->nodes_count; ++i)
            for (std::size_t c = 0; c < data->nodes[i].children_count; ++c) {
                const auto n = static_cast<std::size_t>(data->nodes[i].children[c] - data->nodes);
                if (++parents[n] > 1)
                    fail("node graph must have one parent per node");
            }
        struct Visit {
            std::size_t node;
            std::size_t depth;
        };
        const auto visit = [&](auto &&self, Visit step) -> void {
            const auto i = step.node;
            if (step.depth > std::min<std::size_t>(limits.input.scopes, 256))
                fail("node hierarchy depth limit exceeded", "gltf-limit");
            if (state[i] == 1)
                fail("node hierarchy cycle");
            if (state[i] == 2)
                return;
            state[i] = 1;
            for (std::size_t c = 0; c < data->nodes[i].children_count; ++c) {
                const auto child =
                    static_cast<std::size_t>(data->nodes[i].children[c] - data->nodes);
                self(self, Visit{child, step.depth + 1});
                heights[i] = std::max(heights[i], heights[child] + 1);
            }
            if (heights[i] > std::min<std::size_t>(limits.input.scopes, 256))
                fail("node hierarchy depth limit exceeded", "gltf-limit");
            state[i] = 2;
        };
        for (std::size_t i = 0; i < data->nodes_count; ++i) {
            context = "$.nodes[" + std::to_string(i) + "]";
            visit(visit, Visit{i, 0});
            (void)local(i); // Renderability of a placement is enforced in the selected scene.
        }
        if (!data->scenes_count)
            fail("asset has no selectable scene");
        for (std::size_t i = 0; i < data->scenes_count; ++i) {
            asset.scene_names.push_back(decoded(data->scenes[i].name));
            std::set<const cgltf_node *> seen;
            for (std::size_t n = 0; n < data->scenes[i].nodes_count; ++n) {
                auto *node = data->scenes[i].nodes[n];
                if (parents[static_cast<std::size_t>(node - data->nodes)] ||
                    !seen.insert(node).second)
                    fail("scene must contain unique root nodes");
            }
        }
        if (options.scene) {
            if (*options.scene >= data->scenes_count)
                fail("selected scene index is out of range", "gltf-scene");
            asset.selected_scene = *options.scene;
        } else if (data->scene)
            asset.selected_scene = static_cast<std::size_t>(data->scene - data->scenes);
        else if (data->scenes_count == 1)
            asset.selected_scene = 0;
        else
            fail("multiple scenes without default require explicit --gltf-scene", "gltf-scene");
        selected.resize(data->nodes_count, false);
        const auto mark = [&](auto &&self, cgltf_node *n, std::size_t depth) -> void {
            if (depth > std::min<std::size_t>(limits.input.scopes, 256))
                fail("selected node hierarchy depth limit exceeded", "gltf-limit");
            const auto i = static_cast<std::size_t>(n - data->nodes);
            selected[i] = true;
            for (std::size_t c = 0; c < n->children_count; ++c)
                self(self, n->children[c], depth + 1);
        };
        auto &scene = data->scenes[asset.selected_scene];
        for (std::size_t n = 0; n < scene.nodes_count; ++n)
            mark(mark, scene.nodes[n], 0);
        for (std::size_t i = 0; i < data->animations_count; ++i)
            for (std::size_t c = 0; c < data->animations[i].channels_count; ++c) {
                auto *n = data->animations[i].channels[c].target_node;
                if (n && selected[static_cast<std::size_t>(n - data->nodes)])
                    fail("animation targets selected scene", "gltf-animation",
                         Severity::unsupported);
            }
    }
    void no_extensions(const Json &j) {
        if (j.kind == Json::object) {
            for (const auto &v : j.children) {
                if (v.key == "extras")
                    continue;
                if (v.key == "extensions" && (v.kind != Json::object || !v.children.empty()))
                    fail("selected rendering extensions are unsupported", "gltf-extension",
                         Severity::unsupported);
                no_extensions(v);
            }
        } else if (j.kind == Json::array)
            for (const auto &v : j.children)
                no_extensions(v);
    }
    std::size_t image(cgltf_image *source) {
        if (auto it = images.find(source); it != images.end())
            return it->second;
        no_extensions(
            root.find("images")->children[static_cast<std::size_t>(source - data->images)]);
        Image im;
        im.source = static_cast<std::size_t>(source - data->images);
        im.name = decoded(source->name);
        im.mime_type = decoded(source->mime_type);
        if (source->uri) {
            const auto uri = decoded(source->uri);
            if (uri.starts_with("data:")) {
                const auto end = uri.find(';');
                const auto mime = uri.substr(5, end == std::string::npos ? 0 : end - 5);
                if (mime != "image/png" && mime != "image/jpeg")
                    fail("image data URI requires PNG/JPEG media type");
                if (!im.mime_type.empty() && im.mime_type != mime)
                    fail("image MIME type disagrees with data URI");
                im.mime_type = mime;
            }
            im.bytes = resource(uri, im.mime_type);
        } else {
            auto *v = source->buffer_view;
            selected_view(v);
            charge(v->size);
            im.bytes.assign(view(v), view(v) + v->size);
        }
        const bool png =
            im.bytes.size() >= 8 && std::memcmp(im.bytes.data(), "\x89PNG\r\n\x1a\n", 8) == 0;
        const bool jpeg =
            im.bytes.size() >= 3 && im.bytes[0] == 255 && im.bytes[1] == 216 && im.bytes[2] == 255;
        if (!png && !jpeg)
            fail("image must be PNG or JPEG", "gltf-image", Severity::unsupported);
        const auto actual = png ? "image/png" : "image/jpeg";
        if (!im.mime_type.empty() && im.mime_type != actual)
            fail("image MIME type disagrees with payload");
        im.mime_type = actual;
        auto lim = limits.input.image;
        lim.retained_bytes = std::min(lim.retained_bytes, allocation.remaining / 2);
        lim.scratch_bytes = std::min(lim.scratch_bytes, allocation.remaining / 2);
        auto decoded_image = png ? decode_png(im.bytes, lim, true) : decode_jpeg(im.bytes, lim);
        if (!decoded_image.value)
            fail("image decode: " + decoded_image.diagnostic, "gltf-image",
                 decoded_image.unsupported ? Severity::unsupported : Severity::error);
        im.opaque =
            decoded_image.value->alpha.empty() ||
            std::all_of(decoded_image.value->alpha.begin(), decoded_image.value->alpha.end(),
                        [](std::uint8_t a) { return a == 255; });
        const auto index_ = asset.images.size();
        charge(sizeof(Image) + im.name.size() + im.mime_type.size() + 128);
        asset.images.push_back(std::move(im));
        images[source] = index_;
        return index_;
    }
    std::size_t texture(cgltf_texture_view &v, Material &m) {
        if (v.texcoord < 0)
            fail("negative texture coordinate set");
        const auto uv = static_cast<std::size_t>(v.texcoord);
        if (m.uv_set && *m.uv_set != uv)
            fail("active material textures require one shared UV set", "gltf-uv",
                 Severity::unsupported);
        m.uv_set = uv;
        if (v.has_transform)
            fail("texture transform unsupported", "gltf-extension", Severity::unsupported);
        auto *source = v.texture;
        if (auto it = textures.find(source); it != textures.end())
            return it->second;
        no_extensions(
            root.find("textures")->children[static_cast<std::size_t>(source - data->textures)]);
        if (!source->image)
            fail("texture has no core image source", "gltf-image", Severity::unsupported);
        Texture t;
        t.image = image(source->image);
        if (auto *s = source->sampler) {
            no_extensions(
                root.find("samplers")->children[static_cast<std::size_t>(s - data->samplers)]);
            t.wrap_s = s->wrap_s;
            t.wrap_t = s->wrap_t;
            t.min_filter = s->min_filter ? s->min_filter : 9987;
            t.mag_filter = s->mag_filter ? s->mag_filter : 9729;
        }
        const auto wrap = [](int x) { return x == 10497 || x == 33071 || x == 33648; };
        if (!wrap(t.wrap_s) || !wrap(t.wrap_t) || (t.mag_filter != 9728 && t.mag_filter != 9729) ||
            (t.min_filter != 9728 && t.min_filter != 9729 &&
             (t.min_filter < 9984 || t.min_filter > 9987)))
            fail("invalid texture sampler enum");
        const auto index_ = asset.textures.size();
        charge(2 * sizeof(Texture) + 64);
        asset.textures.push_back(t);
        textures[source] = index_;
        return index_;
    }
    std::size_t material(cgltf_material *source) {
        if (auto it = materials.find(source); it != materials.end())
            return it->second;
        Material m;
        if (source) {
            const auto si = static_cast<std::size_t>(source - data->materials);
            const auto &j = root.find("materials")->children[si];
            no_extensions(j);
            m.name = decoded(source->name);
            const auto real = [&](const Json &v, std::string_view key, double fallback) {
                auto *f = v.find(key);
                return f ? this->real(*f) : fallback;
            };
            if (auto *p = j.find("pbrMetallicRoughness")) {
                if (auto *c = p->find("baseColorFactor"))
                    for (std::size_t i = 0; i < 4; ++i)
                        m.base_color[i] = this->real(c->children[i]);
                m.metallic = real(*p, "metallicFactor", 1);
                m.roughness = real(*p, "roughnessFactor", 1);
            }
            if (auto *e = j.find("emissiveFactor"))
                for (std::size_t i = 0; i < 3; ++i)
                    m.emissive[i] = this->real(e->children[i]);
            m.alpha_mode = source->alpha_mode == cgltf_alpha_mode_mask    ? "MASK"
                           : source->alpha_mode == cgltf_alpha_mode_blend ? "BLEND"
                                                                          : "OPAQUE";
            m.alpha_cutoff = real(j, "alphaCutoff", .5);
            m.double_sided = source->double_sided != 0;
            const auto unit = [&](double n) {
                if (!std::isfinite(n) || n < 0 || n > 1)
                    fail("material factor outside [0,1]");
            };
            for (auto x : m.base_color)
                unit(x);
            for (auto x : m.emissive)
                unit(x);
            unit(m.metallic);
            unit(m.roughness);
            if (!std::isfinite(m.alpha_cutoff) || m.alpha_cutoff < 0 ||
                m.alpha_cutoff > std::numeric_limits<float>::max())
                fail("invalid alphaCutoff");
            auto &p = source->pbr_metallic_roughness;
            if (p.base_color_texture.texture)
                m.base_texture = texture(p.base_color_texture, m);
            if (p.metallic_roughness_texture.texture)
                m.metallic_roughness_texture = texture(p.metallic_roughness_texture, m);
            if (source->normal_texture.texture) {
                m.normal_texture = texture(source->normal_texture, m);
                if (auto *n = j.find("normalTexture"))
                    m.normal_scale = real(*n, "scale", 1);
            }
            if (source->emissive_texture.texture)
                m.emissive_texture = texture(source->emissive_texture, m);
            if (source->occlusion_texture.texture) {
                if (!source->occlusion_texture.texture->image)
                    fail("occlusion texture has no core image", "gltf-image",
                         Severity::unsupported);
                (void)image(source->occlusion_texture.texture->image);
                diagnose(Severity::warning, "gltf-ao",
                         "occlusion texture retained in source snapshot; ambient occlusion is "
                         "ignored by path transport");
            }
        }
        if (m.emits() && m.alpha_mode == "BLEND" &&
            (m.base_color[3] != 1 ||
             (m.base_texture && !asset.images[asset.textures[*m.base_texture].image].opaque)))
            fail("fractional BLEND emission is outside the material profile", "gltf-emission",
                 Severity::unsupported);
        const auto i = asset.materials.size();
        charge(2 * sizeof(Material) + m.name.size() + 128);
        asset.materials.push_back(std::move(m));
        materials[source] = i;
        return i;
    }
    static Vec vec(GeometryPoint p) { return {p.x, p.y, p.z}; }
    GeometryPoint point(Vec p) {
        GeometryPoint v{static_cast<float>(p[0]), static_cast<float>(p[1]),
                        static_cast<float>(p[2])};
        if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z))
            fail("position or frame cannot be represented as binary32");
        return v;
    }
    static GeometryPoint normal(Vec n) {
        const auto l = length(n);
        return {static_cast<float>(n[0] / l), static_cast<float>(n[1] / l),
                static_cast<float>(n[2] / l)};
    }
    void mikk(Primitive &p) {
        struct Context {
            GeometryMesh *mesh;
        };
        Context ctx{&p.geometry};
        SMikkTSpaceInterface api{};
        api.m_getNumFaces = [](const SMikkTSpaceContext *c) {
            return static_cast<int>(static_cast<Context *>(c->m_pUserData)->mesh->triangles.size());
        };
        api.m_getNumVerticesOfFace = [](const SMikkTSpaceContext *, int) { return 3; };
        // MikkTSpace fixes the ABI order of face/corner and sign/face/corner parameters.
        // NOLINTBEGIN(bugprone-easily-swappable-parameters)
        api.m_getPosition = [](const SMikkTSpaceContext *c, float out[], int f, int v) {
            auto &m = *static_cast<Context *>(c->m_pUserData)->mesh;
            auto q = m.positions[3 * static_cast<std::size_t>(f) + static_cast<std::size_t>(v)];
            out[0] = q.x;
            out[1] = q.y;
            out[2] = q.z;
        };
        api.m_getNormal = [](const SMikkTSpaceContext *c, float out[], int f, int v) {
            auto &m = *static_cast<Context *>(c->m_pUserData)->mesh;
            auto q = m.normals[3 * static_cast<std::size_t>(f) + static_cast<std::size_t>(v)];
            out[0] = q.x;
            out[1] = q.y;
            out[2] = q.z;
        };
        api.m_getTexCoord = [](const SMikkTSpaceContext *c, float out[], int f, int v) {
            auto &m = *static_cast<Context *>(c->m_pUserData)->mesh;
            auto q = m.uvs[3 * static_cast<std::size_t>(f) + static_cast<std::size_t>(v)];
            out[0] = q[0];
            out[1] = q[1];
        };
        api.m_setTSpaceBasic = [](const SMikkTSpaceContext *c, const float t[], float sign, int f,
                                  int v) {
            auto &m = *static_cast<Context *>(c->m_pUserData)->mesh;
            const auto i = 3 * static_cast<std::size_t>(f) + static_cast<std::size_t>(v);
            m.tangents[i] = {t[0], t[1], t[2]};
            m.tangent_signs[i] = sign;
        };
        // NOLINTEND(bugprone-easily-swappable-parameters)
        work(p.geometry.positions.size());
        const auto n = p.geometry.positions.size();
        charge(product(n, sizeof(GeometryPoint) + sizeof(float)));
        p.geometry.tangents.resize(n);
        p.geometry.tangent_signs.resize(n);
        if (p.geometry.triangles.size() >
            static_cast<std::size_t>(std::numeric_limits<int>::max() / 4))
            fail("MikkTSpace triangle range exceeded", "gltf-limit");
        // Conservatively reserve topology work before upstream welding/grouping.
        // This also bounds adverse coincident-vertex and seam-heavy inputs.
        work(product(p.geometry.triangles.size(), p.geometry.triangles.size()));
        SMikkTSpaceContext c{&api, &ctx};
        gltf_mikk_budget = &allocation;
        const auto ok = genTangSpaceDefault(&c);
        gltf_mikk_budget = nullptr;
        if (!ok || allocation.exhausted)
            fail("bounded MikkTSpace generation failed", "gltf-limit");
        for (std::size_t i = 0; i < n; ++i) {
            const auto t = vec(p.geometry.tangents[i]);
            if (!std::isfinite(length(t)) || std::abs(length(t) - 1) > 1e-4 ||
                (p.geometry.tangent_signs[i] != 1 && p.geometry.tangent_signs[i] != -1))
                fail("MikkTSpace produced invalid tangent");
        }
        p.generated_tangents = true;
    }
    std::size_t primitive(cgltf_mesh *mesh, cgltf_primitive *source) {
        if (auto it = primitives.find(source); it != primitives.end())
            return it->second;
        const auto meshindex = static_cast<std::size_t>(mesh - data->meshes),
                   pi = static_cast<std::size_t>(source - mesh->primitives);
        context =
            "$.meshes[" + std::to_string(meshindex) + "].primitives[" + std::to_string(pi) + "]";
        const auto &mj = root.find("meshes")->children[meshindex];
        if (auto *e = mj.find("extensions"))
            if (!e->children.empty())
                fail("selected mesh extensions unsupported", "gltf-extension",
                     Severity::unsupported);
        no_extensions(mj.find("primitives")->children[pi]);
        if (source->type != cgltf_primitive_type_triangles)
            fail("only TRIANGLES primitives are supported", "gltf-topology", Severity::unsupported);
        if (source->targets_count || mesh->weights_count)
            fail("selected morph geometry unsupported", "gltf-morph", Severity::unsupported);
        Primitive p;
        p.mesh = meshindex;
        p.primitive = pi;
        p.name = decoded(mesh->name);
        p.material = material(source->material);
        const auto &mat = asset.materials[p.material];
        const cgltf_accessor *pos = nullptr, *norm = nullptr, *tangent = nullptr, *uv = nullptr;
        for (std::size_t i = 0; i < source->attributes_count; ++i) {
            const auto &a = source->attributes[i];
            switch (a.type) {
            case cgltf_attribute_type_position:
                if (a.index || pos)
                    fail("duplicate/invalid POSITION");
                pos = a.data;
                break;
            case cgltf_attribute_type_normal:
                if (a.index || norm)
                    fail("duplicate/invalid NORMAL");
                norm = a.data;
                break;
            case cgltf_attribute_type_tangent:
                if (a.index || tangent)
                    fail("duplicate/invalid TANGENT");
                tangent = a.data;
                break;
            case cgltf_attribute_type_texcoord:
                if (mat.uv_set && a.index == static_cast<int>(*mat.uv_set)) {
                    if (uv)
                        fail("duplicate active TEXCOORD");
                    uv = a.data;
                }
                break;
            case cgltf_attribute_type_color:
                fail("selected vertex COLOR attributes unsupported", "gltf-color",
                     Severity::unsupported);
            case cgltf_attribute_type_joints:
            case cgltf_attribute_type_weights:
                fail("selected skeletal attributes unsupported", "gltf-skin",
                     Severity::unsupported);
            default:
                fail("selected custom/unknown attributes unsupported", "gltf-attribute",
                     Severity::unsupported);
            }
        }
        if (!pos || pos->component_type != cgltf_component_type_r_32f || pos->normalized ||
            !pos->has_min || !pos->has_max)
            fail("POSITION requires FLOAT VEC3 and min/max");
        if (pos->buffer_view && ((pos->buffer_view->offset + pos->offset) % 4 || pos->stride % 4))
            fail("vertex attributes require four-byte alignment");
        for (std::size_t i = 0; i < source->attributes_count; ++i)
            if (source->attributes[i].data->count != pos->count)
                fail("all primitive vertex attributes require matching counts");
        auto positions = accessor(*pos, cgltf_type_vec3);
        std::vector<double> normals, tangents, uvs;
        for (std::size_t i = 0; i < pos->count; ++i)
            for (std::size_t k = 0; k < 3; ++k)
                if (positions[3 * i + k] < pos->min[k] || positions[3 * i + k] > pos->max[k])
                    fail("POSITION min/max does not enclose actual values");
        if (norm) {
            if (norm->component_type != cgltf_component_type_r_32f || norm->normalized ||
                norm->count != pos->count)
                fail("NORMAL requires matching FLOAT VEC3");
            normals = accessor(*norm, cgltf_type_vec3);
            for (std::size_t i = 0; i < norm->count; ++i)
                if (std::abs(length({normals[3 * i], normals[3 * i + 1], normals[3 * i + 2]}) - 1) >
                    1e-4)
                    fail("NORMAL must be unit length");
        } else
            p.generated_normals = true;
        if (mat.uv_set && !uv)
            fail("selected texture UV set is absent", "gltf-uv");
        if (uv) {
            if (uv->count != pos->count ||
                !((uv->component_type == cgltf_component_type_r_32f && !uv->normalized) ||
                  ((uv->component_type == cgltf_component_type_r_8u ||
                    uv->component_type == cgltf_component_type_r_16u) &&
                   uv->normalized)))
                fail("TEXCOORD requires matching FLOAT or normalized unsigned data");
            uvs = accessor(*uv, cgltf_type_vec2);
        }
        if (tangent && norm && mat.normal_texture) {
            if (tangent->count != pos->count ||
                tangent->component_type != cgltf_component_type_r_32f || tangent->normalized)
                fail("TANGENT requires matching FLOAT VEC4");
            tangents = accessor(*tangent, cgltf_type_vec4);
            for (std::size_t i = 0; i < tangent->count; ++i)
                if (std::abs(length({tangents[4 * i], tangents[4 * i + 1], tangents[4 * i + 2]}) -
                             1) > 1e-4 ||
                    (tangents[4 * i + 3] != 1 && tangents[4 * i + 3] != -1))
                    fail("TANGENT requires unit XYZ and sign ±1");
        }
        std::vector<double> indices;
        std::size_t n = pos->count;
        if (source->indices) {
            const auto &a = *source->indices;
            if (a.normalized ||
                (a.component_type != cgltf_component_type_r_8u &&
                 a.component_type != cgltf_component_type_r_16u &&
                 a.component_type != cgltf_component_type_r_32u) ||
                (a.buffer_view && a.buffer_view->stride))
                fail("indices require unsigned SCALAR data without stride");
            indices = accessor(a, cgltf_type_scalar);
            n = a.count;
        }
        if (n == 0 || n % 3)
            fail("TRIANGLES count must be a nonzero multiple of three");
        if (n > std::numeric_limits<std::uint32_t>::max() || n / 3 > limits.input.directives)
            fail("triangle count limit exceeded", "gltf-limit");
        const auto per = 2 * sizeof(GeometryPoint) + (uv ? sizeof(std::array<float, 2>) : 0) +
                         (tangents.empty() ? 0 : sizeof(GeometryPoint) + sizeof(float));
        charge(product(n, per) + product(n / 3, sizeof(std::array<std::uint32_t, 3>)));
        work(n);
        p.geometry.positions.reserve(n);
        p.geometry.normals.reserve(n);
        p.geometry.triangles.reserve(n / 3);
        if (uv)
            p.geometry.uvs.reserve(n);
        if (!tangents.empty()) {
            p.geometry.tangents.reserve(n);
            p.geometry.tangent_signs.reserve(n);
        }
        for (std::size_t f = 0; f < n; f += 3) {
            std::array<std::size_t, 3> ids{};
            std::array<Vec, 3> points{};
            for (std::size_t c = 0; c < 3; ++c) {
                const auto index_ = indices.empty() ? static_cast<double>(f + c) : indices[f + c];
                if (index_ < 0 || index_ >= static_cast<double>(pos->count))
                    fail("triangle index out of range");
                ids[c] = static_cast<std::size_t>(index_);
                for (std::size_t k = 0; k < 3; ++k)
                    points[c][k] = positions[3 * ids[c] + k];
            }
            const auto face = cross(sub(points[1], points[0]), sub(points[2], points[0]));
            if (!(length(face) > 0))
                fail("zero-area source triangle");
            for (std::size_t c = 0; c < 3; ++c) {
                const auto i = ids[c];
                p.geometry.positions.push_back(point(points[c]));
                p.geometry.normals.push_back(
                    norm ? normal({normals[3 * i], normals[3 * i + 1], normals[3 * i + 2]})
                         : normal(face));
                if (uv) {
                    const auto u = static_cast<float>(uvs[2 * i]),
                               v = static_cast<float>(uvs[2 * i + 1]);
                    if (!std::isfinite(u) || !std::isfinite(v))
                        fail("UV exceeds binary32 range");
                    p.geometry.uvs.push_back({u, v});
                }
                if (!tangents.empty()) {
                    p.geometry.tangents.push_back(
                        point({tangents[4 * i], tangents[4 * i + 1], tangents[4 * i + 2]}));
                    p.geometry.tangent_signs.push_back(static_cast<float>(tangents[4 * i + 3]));
                }
            }
            if (!tangents.empty() &&
                (p.geometry.tangent_signs[f] != p.geometry.tangent_signs[f + 1] ||
                 p.geometry.tangent_signs[f] != p.geometry.tangent_signs[f + 2]))
                fail("mixed tangent.w signs make triangle frame undefined");
            p.geometry.triangles.push_back({static_cast<std::uint32_t>(f),
                                            static_cast<std::uint32_t>(f + 1),
                                            static_cast<std::uint32_t>(f + 2)});
        }
        if (mat.normal_texture && tangents.empty())
            mikk(p);
        // glTF samplers consume native top-left UVs, including the generated frame.
        const auto index_ = asset.primitives.size();
        charge(2 * sizeof(Primitive) + p.name.size() + 128);
        asset.primitives.push_back(std::move(p));
        primitives[source] = index_;
        return index_;
    }
    void placements(cgltf_node *node, const Matrix &parent, std::size_t depth = 0) {
        if (depth > std::min<std::size_t>(limits.input.scopes, 256))
            fail("selected hierarchy too deep", "gltf-limit");
        const auto ni = static_cast<std::size_t>(node - data->nodes);
        context = "$.nodes[" + std::to_string(ni) + "]";
        const auto &j = node_json(ni);
        if (auto *e = j.find("extensions"))
            if (!e->children.empty())
                fail("selected node extension unsupported", "gltf-extension",
                     Severity::unsupported);
        if (node->skin || node->weights_count)
            fail("selected skin/morph node unsupported", "gltf-static", Severity::unsupported);
        if (node->camera)
            diagnose(Severity::warning, "gltf-camera",
                     "selected camera retained in source snapshot; bundle is a world fragment with "
                     "caller-owned camera");
        const auto world = multiply(parent, local(ni));
        validate_matrix(world);
        if (node->mesh)
            for (std::size_t i = 0; i < node->mesh->primitives_count; ++i) {
                const auto index_ = primitive(node->mesh, &node->mesh->primitives[i]);
                const auto &mesh = asset.primitives[index_].geometry;
                work(product(mesh.triangles.size(), 9));
                for (const auto &tri : mesh.triangles) {
                    std::array<Vec, 3> pts{};
                    for (std::size_t c = 0; c < 3; ++c) {
                        const auto p = vec(mesh.positions[tri[c]]);
                        Vec q{};
                        for (std::size_t r = 0; r < 3; ++r)
                            q[r] = world[4 * r] * p[0] + world[4 * r + 1] * p[1] +
                                   world[4 * r + 2] * p[2] + world[4 * r + 3];
                        pts[c] = vec(point(q));
                    }
                    if (!(length(cross(sub(pts[1], pts[0]), sub(pts[2], pts[0]))) > 0))
                        fail("placement collapses triangle after binary32 narrowing");
                }
                if (asset.placements.size() >= limits.input.directives)
                    fail("placed primitive limit exceeded", "gltf-limit");
                charge(2 * sizeof(Placement) + decoded(node->name).size() + 128);
                asset.placements.push_back({index_, ni, world, decoded(node->name)});
            }
        for (std::size_t c = 0; c < node->children_count; ++c)
            placements(node->children[c], world, depth + 1);
    }
    void run(const std::filesystem::path &path) {
        asset.entry = std::filesystem::absolute(path).lexically_normal();
        current = Location(asset.entry.string());
        if (!mesh_bundle::valid_utf8(asset.entry.string()))
            fail("input path must be UTF-8");
        asset.bytes = read_file(asset.entry);
        json = std::string_view(reinterpret_cast<const char *>(asset.bytes.data()),
                                asset.bytes.size());
        if (asset.bytes.size() >= 4 && u32(asset.bytes.data()) == 0x46546c67) {
            asset.binary = true;
            if (asset.bytes.size() < 20 || u32(asset.bytes.data() + 4) != 2 ||
                u32(asset.bytes.data() + 8) != asset.bytes.size())
                fail("invalid GLB v2 header/length", "gltf-container");
            std::size_t off = 12;
            bool seen_json = false, seen_bin = false;
            while (off < asset.bytes.size()) {
                if (asset.bytes.size() - off < 8)
                    fail("truncated GLB chunk");
                const auto n = u32(asset.bytes.data() + off),
                           type = u32(asset.bytes.data() + off + 4);
                off += 8;
                if (n % 4 || n > asset.bytes.size() - off)
                    fail("invalid GLB chunk length");
                if (type == 0x4e4f534a) {
                    if (seen_json || off != 20)
                        fail("GLB JSON must be the first unique chunk");
                    seen_json = true;
                    json = std::string_view(
                        reinterpret_cast<const char *>(asset.bytes.data() + off), n);
                } else if (type == 0x004e4942) {
                    if (!seen_json || seen_bin)
                        fail("invalid GLB BIN chunk ordering");
                    seen_bin = true;
                    glb_bin = {asset.bytes.data() + off, n};
                } else
                    fail("unknown GLB chunk outside import profile", "gltf-container",
                         Severity::unsupported);
                off += n;
            }
            if (!seen_json)
                fail("missing GLB JSON");
        }
        if (json.size() > static_cast<std::size_t>(std::numeric_limits<int>::max() / 2))
            fail("JSON exceeds decoder integer range", "gltf-limit");
        const auto lines = static_cast<std::size_t>(std::count(json.begin(), json.end(), '\n')) + 1;
        charge(product(lines, sizeof(std::size_t)));
        line_starts.reserve(lines);
        line_starts.push_back(0);
        for (std::size_t i = 0; i < json.size(); ++i)
            if (json[i] == '\n')
                line_starts.push_back(i + 1);
        root = parse();
        whitespace();
        if (json_pos != json.size())
            json_fail("trailing JSON data");
        schema();
        canonicalize();
        cgltf_options opt{};
        opt.memory = {gltf_allocate, gltf_deallocate, &allocation};
        gltf_numeric_error = false;
        const auto r = cgltf_parse(&opt, canonical_json.data(), canonical_json.size(), &data);
        if (r != cgltf_result_success || gltf_numeric_error)
            fail(allocation.exhausted ? "cgltf allocation budget exceeded"
                                      : "checked cgltf structural/numeric decoding failed",
                 "gltf-parser");
        data->bin = glb_bin.data();
        data->bin_size = glb_bin.size();
        storage();
        hierarchy();
        context = "$.scenes[" + std::to_string(asset.selected_scene) + "]";
        no_extensions(root.find("scenes")->children[asset.selected_scene]);
        auto &scene = data->scenes[asset.selected_scene];
        for (std::size_t n = 0; n < scene.nodes_count; ++n)
            placements(scene.nodes[n], identity);
        if (asset.placements.empty())
            fail("selected scene contains no supported placed triangles", "gltf-scene");
        result.asset = std::move(asset);
    }
};
int exit(const std::vector<Diagnostic> &d) {
    bool unsupported = false;
    for (auto &e : d) {
        if (e.severity == Severity::error)
            return 1;
        unsupported |= e.severity == Severity::unsupported;
    }
    return unsupported ? 2 : 0;
}
} // namespace
int Result::exit_code() const {
    const auto code = exit(diagnostics);
    return code == 0 && !asset ? 1 : code;
}
Result load(const std::filesystem::path &path, const Options &options, const Limits &limits) {
    Reader r(limits, options);
    try {
        r.run(path);
    } catch (const Stop &) {
        // Reader has already retained the source-located diagnostic.
        r.result.asset.reset();
    } catch (const std::exception &e) {
        r.result.asset.reset();
        r.result.diagnostics.push_back(
            {Severity::error, "gltf-exception", Location{path.string()}, e.what()});
    }
    return std::move(r.result);
}
std::string report(const Result &r) {
    std::ostringstream out;
    for (const auto &d : r.diagnostics)
        out << *d.location.file << ':' << d.location.line << ':' << d.location.column << ": "
            << (d.severity == Severity::error         ? "error"
                : d.severity == Severity::unsupported ? "unsupported"
                                                      : "warning")
            << " [" << d.code << "] " << d.message << '\n';
    if (r.asset) {
        const auto &a = *r.asset;
        out << "glTF " << profile << ": scene " << a.selected_scene << ", " << a.primitives.size()
            << " unique primitives, " << a.placements.size() << " placements, "
            << a.materials.size() << " materials, " << a.images.size() << " images\n";
    }
    return out.str();
}
std::size_t retained_bytes(const Asset &a) {
    std::size_t n = sizeof(a) + a.bytes.capacity();
    const auto add = [&](std::size_t v) {
        if (v > std::numeric_limits<std::size_t>::max() - n)
            throw std::runtime_error("glTF retained size overflow");
        n += v;
    };
    add(a.entry.native().capacity() * sizeof(std::filesystem::path::value_type));
    add(a.resources.capacity() * sizeof(Resource) + a.images.capacity() * sizeof(Image) +
        a.textures.capacity() * sizeof(Texture) + a.materials.capacity() * sizeof(Material) +
        a.primitives.capacity() * sizeof(Primitive) + a.placements.capacity() * sizeof(Placement) +
        a.scene_names.capacity() * sizeof(std::string));
    for (auto &s : a.scene_names)
        add(s.capacity() + 64);
    for (auto &r : a.resources)
        add(r.bytes.capacity() + r.uri.capacity() + r.path.native().capacity() + 128);
    for (auto &i : a.images)
        add(i.bytes.capacity() + i.name.capacity() + i.mime_type.capacity() + 128);
    for (auto &m : a.materials)
        add(m.name.capacity() + m.alpha_mode.capacity() + 128);
    for (auto &p : a.primitives) {
        add(p.name.capacity() + 128);
        auto &g = p.geometry;
        add(g.positions.capacity() * sizeof(GeometryPoint) +
            g.normals.capacity() * sizeof(GeometryPoint) +
            g.tangents.capacity() * sizeof(GeometryPoint) +
            g.tangent_signs.capacity() * sizeof(float) +
            g.uvs.capacity() * sizeof(std::array<float, 2>) +
            g.triangles.capacity() * sizeof(std::array<std::uint32_t, 3>));
    }
    for (auto &p : a.placements)
        add(p.name.capacity() + 64);
    return n;
}
} // namespace rypath::gltf
