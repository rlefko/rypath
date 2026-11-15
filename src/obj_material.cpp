// SPDX-License-Identifier: GPL-3.0-only
#include "obj.hpp"

#include "exr_image.hpp"
#include "pbrt_numeric.hpp"
#include "render.hpp"
#include "sha256.hpp"
#include "texture_footprint.hpp"
#include "triangle_attributes.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace rypath::obj {
namespace {
using RGB = std::array<double, 3>;
struct Rejected {};
std::string number(double n) {
    std::ostringstream text;
    text.imbue(std::locale::classic());
    text << std::setprecision(17) << n;
    return text.str();
}
std::string rgb(RGB value) {
    return "[" + number(value[0]) + " " + number(value[1]) + " " + number(value[2]) + "]";
}
double luminance(RGB v) { return .2126 * v[0] + .7152 * v[1] + .0722 * v[2]; }
std::string quote(const std::string &s) { return "\"" + s + "\""; }
struct Map {
    const Property *property = nullptr;
    std::string filename;
    std::array<double, 2> offset{0, 0}, scale{1, 1};
    double base = 0, gain = 1, bump = 1;
    bool clamp = false;
    char channel = 'l';
    bool explicit_channel = false;
    bool identity() const {
        return offset == std::array<double, 2>{0, 0} && scale == std::array<double, 2>{1, 1} &&
               !clamp;
    }
};
struct Field {
    std::string parameter;
    double constant = 0;
    std::vector<float> samples;
    std::uint32_t width = 0, height = 0;
    std::optional<Map> mapping;
    bool opaque() const {
        return samples.empty() ? constant == 1
                               : std::ranges::all_of(samples, [](float n) { return n == 1; });
    }
};
class Builder {
  public:
    const Asset &asset;
    const Limits &limits;
    Conversion output;
    std::string prefix;
    std::size_t retained = 0, output_bytes = 0, work = 0, value_work = 0, sequence = 0,
                source_bytes = 0, source_files = 0;
    std::map<std::filesystem::path, DecodedImage> images;
    std::set<std::filesystem::path> bundled;

    Builder(const Asset &a, const Limits &l) : asset(a), limits(l) {
        Sha256 hash;
        for (const auto &s : asset.sources)
            hash.update(sha256(s.bytes));
        for (const auto &m : asset.materials) {
            hash_text(hash, m.name);
            for (const auto &p : m.properties) {
                hash_text(hash, p.name);
                for (const auto &arg : p.arguments)
                    hash_text(hash, arg);
            }
        }
        prefix = "obj_" + digest_hex(hash.finish()) + "_";
    }
    static bool image_property(std::string_view name) {
        constexpr std::array<std::string_view, 12> names{"map_Ka", "map_Kd", "map_Ks",   "map_Ns",
                                                         "map_Pr", "map_Pm", "map_Ke",   "map_d",
                                                         "map_Tr", "bump",   "map_bump", "norm"};
        return std::ranges::find(names, name) != names.end();
    }
    static void hash_text(Sha256 &hash, const std::string &text) {
        hash.update(std::span(reinterpret_cast<const std::uint8_t *>(text.data()), text.size()));
        const std::array<std::uint8_t, 1> separator{0};
        hash.update(separator);
    }
    void diagnostic(Severity severity, const Location &where, std::string code,
                    std::string message) {
        if (output.diagnostics.size() < limits.input.diagnostics)
            output.diagnostics.push_back({severity, std::move(code), where, std::move(message)});
        else {
            const Diagnostic exhausted{Severity::error, "obj.limit", where,
                                       "Diagnostic budget exhausted"};
            if (output.diagnostics.empty())
                output.diagnostics.push_back(exhausted);
            else
                output.diagnostics.back() = exhausted;
        }
    }
    [[noreturn]] void fail(const Location &where, std::string message, bool unsupported = false) {
        diagnostic(unsupported ? Severity::unsupported : Severity::error, where,
                   unsupported ? "obj.material.unsupported" : "obj.material.invalid",
                   std::move(message));
        throw Rejected{};
    }
    void warn(const Location &where, std::string message) {
        diagnostic(Severity::warning, where, "obj.material.approximation", std::move(message));
        if (!output.diagnostics.empty() && output.diagnostics.back().code == "obj.limit")
            throw Rejected{};
    }
    void charge(std::size_t amount, const Location &where) {
        if (amount > limits.working_bytes - std::min(retained, limits.working_bytes))
            fail(where, "Material processing exceeds the aggregate working byte limit");
        retained += amount;
    }
    void charge_output(std::size_t amount, const Location &where) {
        if (amount > limits.output_bytes - std::min(output_bytes, limits.output_bytes))
            fail(where, "Material bundle exceeds the aggregate output byte limit");
        output_bytes += amount;
    }
    double numeric(const std::string &s, const Location &where, double low, double high) {
        const auto parsed = pbrt::decimal_float<double>(s);
        if (!parsed || *parsed < low || *parsed > high ||
            !std::isfinite(static_cast<float>(*parsed)))
            fail(where,
                 "Expected a finite number in [" + number(low) + ", " + number(high) + "]: " + s);
        const double n = *parsed;
        return n;
    }
    double scalar(const Property &p, double low = 0, double high = 1) {
        if (p.arguments.size() != 1)
            fail(p.location, p.name + " requires one numeric value");
        return numeric(p.arguments[0], p.location, low, high);
    }
    RGB color(const Property &p, bool emission = false) {
        if (!p.arguments.empty() && (p.arguments[0] == "spectral" || p.arguments[0] == "xyz"))
            fail(p.location, p.name + " spectral and XYZ colors are unsupported", true);
        if (p.arguments.size() != 1 && p.arguments.size() != 3)
            fail(p.location, p.name + " requires one gray or three RGB values");
        RGB v{};
        for (std::size_t i = 0; i < 3; ++i)
            v[i] = numeric(p.arguments[p.arguments.size() == 1 ? 0 : i], p.location, 0,
                           emission ? 1e20 : 1);
        return v;
    }
    Map parse_map(const Property &p, bool bump) {
        Map m;
        m.property = &p;
        std::size_t i = 0;
        const auto &a = p.arguments;
        auto take = [&]() -> const std::string & {
            if (i == a.size())
                fail(p.location, "Missing map option argument in " + p.name);
            return a[i++];
        };
        while (i < a.size() && a[i].starts_with('-')) {
            const auto option = a[i++];
            if (option == "-o" || option == "-s") {
                std::array<double, 3> v = option == "-o" ? std::array<double, 3>{0, 0, 0}
                                                         : std::array<double, 3>{1, 1, 1};
                std::size_t count = 0;
                while (i < a.size() && count < 3) {
                    if (!pbrt::is_decimal(a[i]))
                        break;
                    v[count++] = numeric(a[i++], p.location, -1e6, 1e6);
                }
                if (count == 0)
                    fail(p.location, option + " requires one to three finite values");
                if (v[2] != (option == "-o" ? 0 : 1))
                    fail(p.location, "Nontrivial third map coordinate is unsupported", true);
                if (option == "-o")
                    m.offset = {v[0], v[1]};
                else
                    m.scale = {v[0], v[1]};
            } else if (option == "-clamp") {
                const auto &s = take();
                if (s != "on" && s != "off")
                    fail(p.location, "-clamp requires on or off");
                m.clamp = s == "on";
            } else if (option == "-mm") {
                m.base = numeric(take(), p.location, -1e6, 1e6);
                m.gain = numeric(take(), p.location, -1e6, 1e6);
            } else if (option == "-bm" && bump) {
                m.bump = numeric(take(), p.location, -1e6, 1e6);
            } else if (option == "-imfchan") {
                const auto &s = take();
                if (s.size() != 1 || std::string_view("rgbml").find(s[0]) == std::string_view::npos)
                    fail(p.location, "Unsupported -imfchan (expected r/g/b/m/l)", true);
                m.channel = s[0];
                m.explicit_channel = true;
            } else
                fail(p.location, "Unsupported MTL map option: " + option, true);
        }
        if (i == a.size())
            fail(p.location, "Map filename is missing");
        m.filename = take();
        // Loader tokenizes quoted filenames as one argument; unquoted spaces are retained here.
        while (i < a.size())
            m.filename += " " + take();
        if (m.filename.empty() || m.filename.find('\0') != std::string::npos)
            fail(p.location, "Invalid empty or NUL-containing image path");
        return m;
    }
    void definition(std::string s, const Location &where) {
        if (++work > limits.input.directives)
            fail(where, "Material definition work exceeds directive limit");
        charge(s.size(), where);
        charge_output(s.size(), where);
        output.definitions += s + "\n";
    }
    std::string file(std::vector<std::uint8_t> bytes, const std::string &extension,
                     const Location &where) {
        const auto path = std::filesystem::path("assets") / (digest_hex(sha256(bytes)) + extension);
        if (bundled.insert(path).second) {
            if (output.files.size() >= limits.input.expansions)
                fail(where, "Material bundle file count exceeds limit");
            charge(bytes.size(), where);
            charge_output(bytes.size(), where);
            output.files.push_back({path, std::move(bytes)});
        }
        return path.generic_string();
    }
    const DecodedImage &image(const Map &m) {
        const auto &where = m.property->location;
        auto base = std::filesystem::path(*where.file).parent_path();
        if (base.empty())
            base = asset.entry.parent_path();
        const auto path = (base / m.filename).lexically_normal();
        if (const auto found = images.find(path); found != images.end())
            return found->second;
        if (source_files >= limits.input.expansions)
            fail(where, "Aggregate source file count exceeds limit");
        std::error_code file_error;
        if (!std::filesystem::is_regular_file(path, file_error) || file_error)
            fail(where, "MTL image must be a readable regular file: " + path.string());
        const auto size = std::filesystem::file_size(path, file_error);
        if (file_error || size > limits.input.file_bytes ||
            size > static_cast<std::uintmax_t>(std::numeric_limits<std::streamsize>::max()))
            fail(where,
                 "MTL image exceeds input file byte limit or cannot be sized: " + path.string());
        std::ifstream input(path, std::ios::binary);
        if (!input)
            fail(where, "Cannot read MTL image: " + path.string());
        if (static_cast<std::size_t>(size) >
            limits.input.expanded_bytes - std::min(source_bytes, limits.input.expanded_bytes))
            fail(where, "Aggregate source bytes exceed expanded input limit");
        source_bytes += static_cast<std::size_t>(size);
        ++source_files;
        charge(static_cast<std::size_t>(size), where);
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
        if (!input.read(reinterpret_cast<char *>(bytes.data()),
                        static_cast<std::streamsize>(bytes.size())))
            fail(where, "Cannot read complete MTL image: " + path.string());
        const auto trailing = input.peek();
        if (trailing != std::char_traits<char>::eof() || input.bad() ||
            std::filesystem::file_size(path, file_error) != size || file_error)
            fail(where, "MTL image changed size while reading: " + path.string());
        auto extension = path.extension().string();
        std::ranges::transform(extension, extension.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        auto image_limits = limits.input.image;
        const auto remaining = limits.working_bytes - std::min(retained, limits.working_bytes);
        image_limits.retained_bytes = std::min(image_limits.retained_bytes, remaining / 2);
        image_limits.scratch_bytes = std::min(image_limits.scratch_bytes, remaining / 2);
        ImageResult<DecodedImage> decoded;
        if (extension == ".png")
            decoded = decode_png(bytes, image_limits);
        else if (extension == ".jpg" || extension == ".jpeg")
            decoded = decode_jpeg(bytes, image_limits);
        else if (extension == ".exr")
            decoded = decode_exr(bytes, image_limits);
        else if (extension == ".hdr")
            decoded = decode_hdr(bytes, image_limits);
        else
            fail(where, "Unsupported MTL image format: " + extension, true);
        if (!decoded.value)
            fail(where, decoded.diagnostic, decoded.unsupported);
        charge(decoded_image_retained_bytes(*decoded.value), where);
        // Preserve a hash-addressed original and a source snapshot for provenance.
        file(bytes, extension, where);
        output.sources.push_back({path, "image", std::move(bytes)});
        return images.emplace(path, std::move(*decoded.value)).first->second;
    }
    double sample(const DecodedImage &im, std::size_t pixel, std::size_t channel,
                  bool alpha = false) {
        if (im.sample_format == ImageSampleFormat::half ||
            im.sample_format == ImageSampleFormat::float32)
            return alpha ? im.floating_alpha[pixel] : im.floating_rgb[3 * pixel + channel];
        const auto &bytes = alpha ? im.alpha : im.rgb;
        const auto index = alpha ? pixel : 3 * pixel + channel;
        if (im.sample_format == ImageSampleFormat::unorm16)
            return (256u * bytes[2 * index] + bytes[2 * index + 1]) / 65535.;
        return bytes[index] / 255.;
    }
    static double linearize(double n) {
        return n <= .04045 ? n / 12.92 : std::pow((n + .055) / 1.055, 2.4);
    }
    std::string exr(const DecodedImage &im, const std::vector<float> &pixels,
                    const Location &where) {
        std::vector<std::uint8_t> bytes;
        auto il = limits.input.image;
        const auto remaining = limits.working_bytes - std::min(retained, limits.working_bytes);
        il.retained_bytes = std::min(il.retained_bytes, remaining / 2);
        il.scratch_bytes = std::min(il.scratch_bytes, remaining / 2);
        const auto error = encode_exr(
            {im.width, im.height, pixels},
            [&](std::uint64_t offset, std::span<const std::uint8_t> data) {
                const auto maximum =
                    std::min({limits.input.file_bytes, remaining / 2,
                              limits.output_bytes - std::min(output_bytes, limits.output_bytes)});
                if (offset > maximum || data.size() > maximum - offset)
                    return false;
                const auto end = static_cast<std::size_t>(offset) + data.size();
                if (bytes.size() < end)
                    bytes.resize(end);
                std::copy(data.begin(), data.end(),
                          bytes.begin() + static_cast<std::ptrdiff_t>(offset));
                return true;
            },
            il);
        if (!error.empty())
            fail(where, "Derived MTL texture: " + error);
        return file(std::move(bytes), ".exr", where);
    }
    std::string mapping(const Map &m) {
        return " \"string wrap\" " + quote(m.clamp ? "clamp" : "repeat") + " \"float uscale\" " +
               number(m.scale[0]) + " \"float vscale\" " + number(m.scale[1]) +
               " \"float udelta\" " + number(m.offset[0]) + " \"float vdelta\" " +
               number(m.offset[1]);
    }
    struct Prepared {
        std::string path;
        std::vector<float> values;
        std::uint32_t width, height;
    };
    Prepared prepare(const Map &m, RGB multiplier, bool scalar, bool color_decode, bool ns = false,
                     bool square = false, bool invert = false) {
        const auto &im = image(m);
        const auto &where = m.property->location;
        if (!scalar && m.explicit_channel)
            fail(where, "-imfchan on an RGB map is unsupported", true);
        if (m.channel == 'm' && im.source_channels != 4)
            fail(where, "-imfchan m requires retained image alpha", true);
        const auto pixels = static_cast<std::size_t>(im.width) * im.height;
        if (pixels > limits.input.total_values / 3 ||
            value_work > limits.input.total_values - 3 * pixels)
            fail(where, "Derived texture sample work exceeds total value limit");
        value_work += 3 * pixels;
        charge(pixels * 3 * sizeof(float), where);
        std::vector<float> values(pixels * 3);
        const bool byte = im.sample_format == ImageSampleFormat::unorm8 ||
                          im.sample_format == ImageSampleFormat::unorm16;
        bool ns_clamped = false;
        for (std::size_t i = 0; i < pixels; ++i) {
            RGB c{};
            for (std::size_t k = 0; k < 3; ++k) {
                const auto n = sample(im, i, k);
                c[k] = color_decode && byte ? linearize(n) : n;
            }
            if (scalar) {
                double n = m.channel == 'm'   ? sample(im, i, 0, true)
                           : m.channel == 'r' ? c[0]
                           : m.channel == 'g' ? c[1]
                           : m.channel == 'b' ? c[2]
                                              : luminance(c);
                n = (m.base + m.gain * n) * multiplier[0];
                if (ns) {
                    ns_clamped = ns_clamped || n < 0 || n > 1000;
                    n = 1 - std::sqrt(std::clamp(n, 0., 1000.) / 1000.);
                } else if (square && (n < 0 || n > 1))
                    fail(where, "Mapped perceptual roughness must be in [0,1]");
                if (square)
                    n *= n;
                if (invert)
                    n = 1 - n;
                c.fill(n);
            } else
                for (std::size_t k = 0; k < 3; ++k)
                    c[k] = (m.base + m.gain * c[k]) * multiplier[k];
            for (std::size_t k = 0; k < 3; ++k) {
                if (!std::isfinite(c[k]) || !std::isfinite(static_cast<float>(c[k])))
                    fail(where, "Derived MTL texel is not finite binary32");
                values[3 * i + k] = static_cast<float>(c[k]);
            }
        }
        if (ns_clamped)
            warn(where, "map_Ns effective samples outside [0,1000] are clamped before "
                        "perceptual-roughness conversion");
        auto path = exr(im, values, where);
        return {std::move(path), std::move(values), im.width, im.height};
    }
    std::string texture(const Map &m, const std::string &path, bool scalar) {
        const auto name = prefix + "t" + std::to_string(sequence++);
        definition("Texture " + quote(name) + " " + quote(scalar ? "float" : "spectrum") +
                       " \"imagemap\" \"string filename\" " + quote(path) + mapping(m),
                   m.property->location);
        return name;
    }
    Field field(const Property *p, const std::optional<Map> &map, double value,
                const std::string &parameter, bool ns = false, bool square = false,
                bool invert = false) {
        Field result;
        result.constant = value;
        if (!map) {
            if (ns)
                value = 1 - std::sqrt(std::clamp(value, 0., 1000.) / 1000.);
            if (square)
                value *= value;
            if (invert)
                value = 1 - value;
            result.constant = value;
            result.parameter = " \"float " + parameter + "\" " + number(value);
        } else {
            auto prepared = prepare(*map, {p ? value : 1, 1, 1}, true, false, ns, square, invert);
            if ((parameter == "amount" || parameter == "alpha") &&
                std::ranges::any_of(prepared.values, [](float n) { return n < 0 || n > 1; }))
                fail(map->property->location, "Mapped " + parameter + " must be in [0,1]");
            result.parameter =
                " \"texture " + parameter + "\" " + quote(texture(*map, prepared.path, true));
            result.samples = std::move(prepared.values);
            result.width = prepared.width;
            result.height = prepared.height;
            result.mapping = map;
        }
        return result;
    }
    std::string spectrum(const Property *p, const std::optional<Map> &map, RGB value,
                         const std::string &parameter, bool emission = false) {
        if (!map)
            return " \"rgb " + parameter + "\" " + rgb(value);
        auto prepared = prepare(*map, p ? value : RGB{1, 1, 1}, false, true);
        if (std::ranges::any_of(prepared.values,
                                [emission](float n) { return n < 0 || (!emission && n > 1); }))
            fail(map->property->location,
                 "Mapped " + parameter + " is outside its nonnegative material range");
        return " \"texture " + parameter + "\" " + quote(texture(*map, prepared.path, false));
    }
    void named(const std::string &name, const std::string &type, const std::string &parameters,
               const Location &where) {
        definition("MakeNamedMaterial " + quote(name) + " \"string type\" " + quote(type) +
                       parameters,
                   where);
    }
    std::string mix(const std::string &a, const std::string &b, const std::string &amount,
                    const Location &where) {
        const auto name = prefix + "mix" + std::to_string(sequence++);
        named(name, "mix", " \"string materials\" [" + quote(a) + " " + quote(b) + "]" + amount,
              where);
        return name;
    }
    MaterialBinding material(const Material &material, std::size_t index) {
        const auto &where = material.location;
        std::map<std::string, const Property *> properties;
        std::map<std::string, Map> maps;
        const std::set<std::string> color_names{"Kd", "Ks", "Ka", "Ke", "Tf"};
        const std::set<std::string> scalar_names{"Ns", "Ni", "d",   "Tr",   "Pr",
                                                 "Pm", "Pc", "Pcr", "illum"};
        for (const auto &p : material.properties) {
            if (++work > limits.input.directives)
                fail(p.location, "Material property work exceeds directive limit");
            charge(sizeof(Property) + p.name.size(), p.location);
            for (const auto &arg : p.arguments)
                charge(arg.size(), p.location);
            if (p.name == "d" && !p.arguments.empty() && p.arguments[0] == "-halo")
                fail(p.location, "d -halo dissolve semantics are unsupported", true);
            if (color_names.contains(p.name)) {
                color(p, p.name == "Ke" || p.name == "Ka");
            } else if (scalar_names.contains(p.name)) {
                const auto v = scalar(p, p.name == "Ni" ? std::numeric_limits<float>::min() : 0,
                                      p.name == "Ns"      ? 1e20
                                      : p.name == "Ni"    ? 1e6
                                      : p.name == "illum" ? 10
                                                          : 1);
                if (p.name == "illum" && std::floor(v) != v)
                    fail(p.location, "illum must be an integer");
            } else if (image_property(p.name)) {
                auto m = parse_map(p, p.name == "bump" || p.name == "map_bump");
                const auto &decoded = image(m); // Validate even overridden maps and options.
                if (m.channel == 'm' && decoded.source_channels != 4)
                    fail(p.location, "-imfchan m requires retained image alpha", true);
                const bool rgb_map = p.name == "map_Kd" || p.name == "map_Ks" ||
                                     p.name == "map_Ke" || p.name == "map_Ka" || p.name == "norm";
                if (rgb_map && m.explicit_channel)
                    fail(p.location, "-imfchan on an RGB map is unsupported", true);
                if ((p.name == "norm" || p.name == "map_Ke") && !m.identity())
                    fail(p.location, p.name + " requires identity repeat UV mapping", true);
                maps[p.name] = std::move(m);
            } else {
                fail(p.location, "Unsupported MTL property: " + p.name, true);
            }
            if (properties.contains(p.name))
                warn(p.location, "Repeated " + p.name + "; last definition is effective");
            properties[p.name] = &p;
        }
        auto prop = [&](const std::string &name) -> const Property * {
            const auto found = properties.find(name);
            return found == properties.end() ? nullptr : found->second;
        };
        auto value = [&](const std::string &name, double fallback) {
            const auto *p = prop(name);
            return p ? scalar(*p, name == "Ni" ? std::numeric_limits<float>::min() : 0,
                              name == "Ns"      ? 1e20
                              : name == "Ni"    ? 1e6
                              : name == "illum" ? 10
                                                : 1)
                     : fallback;
        };
        auto cv = [&](const std::string &name, RGB fallback) {
            const auto *p = prop(name);
            return p ? color(*p, name == "Ke") : fallback;
        };
        auto map = [&](const std::string &name) -> std::optional<Map> {
            const auto found = maps.find(name);
            return found == maps.end() ? std::nullopt : std::optional<Map>(found->second);
        };
        const auto illum = static_cast<int>(value("illum", 2));
        if (illum == 10)
            fail(prop("illum")->location, "illum 10 shadow-catcher semantics are unsupported",
                 true);
        warn(where, "MTL converted using mtl-pbr-approx-v1: linear RGB constants, sRGB byte color "
                    "maps, linear data maps, +Y tangent normals; legacy highlights approximate "
                    "physical scattering");
        if (illum == 0)
            warn(where,
                 "illum 0 unlit color is approximated by lit diffuse; unlit appearance is lost");
        if (map("map_Ka"))
            warn(map("map_Ka")->property->location,
                 "map_Ka retained as an inactive ambient image; no ambient illumination or "
                 "emission is synthesized");
        if (prop("Ka"))
            warn(prop("Ka")->location, "Ka ambient color retained as provenance; no ambient "
                                       "illumination or emission is synthesized");
        const bool transmission = illum == 4 || illum == 9 || illum == 6 || illum == 7;
        if (transmission && cv("Tf", {1, 1, 1}) != RGB{1, 1, 1})
            fail(prop("Tf")->location, "Colored Tf transmission/absorption is unsupported", true);
        if (!transmission && prop("Tf"))
            warn(prop("Tf")->location, "Tf is inactive for the selected opaque illum model");
        MaterialBinding binding;
        binding.name = prefix + "m" + std::to_string(index);
        binding.requires_uvs =
            std::ranges::any_of(maps, [](const auto &entry) { return entry.first != "map_Ka"; });
        auto opacity = field(prop("d"), map("map_d"), value("d", 1), "alpha");
        if (prop("Tr") || map("map_Tr")) {
            auto tr = field(prop("Tr"), map("map_Tr"), value("Tr", 0), "alpha", false, false, true);
            if (prop("d") || map("map_d")) {
                const auto close = [](double a, double b) {
                    return std::abs(a - b) <= 4 * std::numeric_limits<float>::epsilon();
                };
                bool equal = true;
                if (opacity.samples.empty() && tr.samples.empty()) {
                    equal = std::abs(opacity.constant - tr.constant) <=
                            8 * std::numeric_limits<double>::epsilon();
                } else if (opacity.samples.empty()) {
                    equal = std::ranges::all_of(
                        tr.samples, [&](float n) { return close(n, opacity.constant); });
                } else if (tr.samples.empty()) {
                    equal = std::ranges::all_of(opacity.samples,
                                                [&](float n) { return close(n, tr.constant); });
                } else {
                    const bool same_mapping = opacity.width == tr.width &&
                                              opacity.height == tr.height &&
                                              opacity.mapping->offset == tr.mapping->offset &&
                                              opacity.mapping->scale == tr.mapping->scale &&
                                              opacity.mapping->clamp == tr.mapping->clamp;
                    const bool opacity_uniform = std::ranges::all_of(
                        opacity.samples, [&](float n) { return close(n, opacity.samples[0]); });
                    const bool tr_uniform = std::ranges::all_of(
                        tr.samples, [&](float n) { return close(n, tr.samples[0]); });
                    if (opacity_uniform && tr_uniform)
                        equal = close(opacity.samples[0], tr.samples[0]);
                    else if (!same_mapping)
                        fail(where,
                             "Cannot prove d/map_d and Tr/map_Tr opacity equality across different "
                             "image dimensions or UV mappings",
                             true);
                    else
                        for (std::size_t k = 0; equal && k < opacity.samples.size(); ++k)
                            equal = close(opacity.samples[k], tr.samples[k]);
                }
                if (!equal)
                    fail(where, "Contradictory d/map_d and Tr/map_Tr opacity definitions");
            } else
                opacity = std::move(tr);
        }
        if ((!opacity.samples.empty() &&
             std::ranges::any_of(opacity.samples, [](float x) { return x < 0 || x > 1; })) ||
            (opacity.samples.empty() && (opacity.constant < 0 || opacity.constant > 1)))
            fail(where, "Coverage opacity must remain in [0,1]");
        if (!opacity.opaque())
            binding.alpha = opacity.parameter;
        const RGB emission = cv("Ke", {0, 0, 0});
        if (emission != RGB{0, 0, 0} || map("map_Ke")) {
            if (!opacity.opaque())
                fail(where,
                     "MTL emission requires fully opaque coverage, including no zero-alpha emitter",
                     true);
            if (const auto em = map("map_Ke")) {
                if (!em->identity())
                    fail(em->property->location,
                         "Image emission requires identity repeat UV mapping", true);
                auto prepared = prepare(*em, prop("Ke") ? emission : RGB{1, 1, 1}, false, true);
                if (std::ranges::any_of(prepared.values, [](float x) { return x < 0; }))
                    fail(em->property->location, "Image emission must be nonnegative");
                binding.emission =
                    "AreaLightSource \"diffuse\" \"string filename\" " + quote(prepared.path);
            } else
                binding.emission = "AreaLightSource \"diffuse\" \"rgb L\" " + rgb(emission);
        }
        std::string detail;
        auto bump = map("bump");
        if (map("map_bump")) {
            if (bump)
                warn(map("map_bump")->property->location, "map_bump overrides bump");
            bump = map("map_bump");
        }
        if (const auto normal = map("norm")) {
            if (!normal->identity())
                fail(normal->property->location, "Normal maps require identity repeat UV mapping",
                     true);
            if (bump)
                warn(normal->property->location,
                     "norm takes precedence over bump; bump mapping is inactive");
            auto prepared = prepare(*normal, {1, 1, 1}, false, false);
            if (std::ranges::any_of(prepared.values, [](float x) { return x < 0 || x > 1; }))
                fail(normal->property->location, "Tangent-space normal channels must be in [0,1]");
            for (std::size_t i = 0; i < prepared.values.size(); i += 3)
                if (prepared.values[i] == .5f && prepared.values[i + 1] == .5f &&
                    prepared.values[i + 2] == .5f)
                    fail(normal->property->location,
                         "Tangent-space normal map contains a zero vector");
            detail = " \"string normalmap\" " + quote(prepared.path);
        } else if (bump) {
            auto adjusted = *bump;
            adjusted.base *= bump->bump;
            adjusted.gain *= bump->bump;
            detail = field(nullptr, adjusted, 1, "displacement").parameter;
        }
        const bool pr = prop("Pr") || map("map_Pr");
        if (pr && (prop("Ns") || map("map_Ns")))
            warn(where, "Pr/map_Pr overrides legacy Ns/map_Ns roughness");
        auto rough = field(prop(pr ? "Pr" : "Ns"), map(pr ? "map_Pr" : "map_Ns"),
                           value(pr ? "Pr" : "Ns", pr ? .5 : 250), "roughness", !pr, true);
        if (prop("Ns") && value("Ns", 250) > 1000)
            warn(prop("Ns")->location,
                 "Ns is clamped to 1000 before perceptual-roughness conversion");
        const std::string remap = " \"bool remaproughness\" false";
        const auto diffuse =
            spectrum(prop("Kd"), map("map_Kd"), cv("Kd", {.5, .5, .5}), "reflectance");
        const auto eta = " \"float eta\" " + number(value("Ni", 1.5));
        const bool pbr_metal = prop("Pm") || map("map_Pm");
        const bool coat = prop("Pc") && value("Pc", 0) > 0;
        if (prop("Pcr") && !coat)
            warn(prop("Pcr")->location, "Pcr is inactive without a nonzero Pc coat");
        if (transmission) {
            if (coat || pbr_metal)
                fail(where,
                     "Pc/Pm coatings or metallic mixing on transmissive illum models are "
                     "unsupported",
                     true);
            if (prop("Kd") || map("map_Kd") || prop("Ks") || map("map_Ks"))
                warn(where, "Kd/Ks tint and legacy highlights are inactive in the dielectric "
                            "approximation");
            if (illum == 4 || illum == 9) {
                warn(where,
                     "illum 4/9 approximated as a thin dielectric without volume absorption");
                named(binding.name, "thindielectric", eta + detail, where);
            } else {
                warn(where, "illum 6/7 approximated as a dielectric without volume absorption");
                named(binding.name, "dielectric", eta + rough.parameter + remap + detail, where);
            }
            return binding;
        }
        const bool conductor = illum == 3 || illum == 5 || illum == 8;
        const auto ks = cv("Ks", {0, 0, 0});
        if (ks[0] != ks[1] || ks[1] != ks[2] || map("map_Ks"))
            warn(where, "Colored Ks/map_Ks highlight reduced to luminance for dielectric mixing; "
                        "colored highlight detail is lost");
        if (coat)
            warn(where, "Pc/Pcr selects one effective clearcoat; separate legacy Ks/Ns highlights "
                        "are lost");
        const auto plain_diffuse = binding.name + "_diffuse";
        named(plain_diffuse, "diffuse", diffuse + detail, where);
        std::string dielectric_base = plain_diffuse;
        auto rough_parameter = [&](const std::string &name) {
            auto result = rough.parameter;
            const auto begin = result.find("roughness");
            result.replace(begin, std::string("roughness").size(), name);
            return result;
        };
        auto coated_diffuse = [&]() {
            const auto name = binding.name + "_coat";
            auto cr =
                coat ? field(prop("Pcr"), {}, value("Pcr", .5), "roughness", false, true) : rough;
            named(name, "coateddiffuse", diffuse + eta + cr.parameter + remap + detail, where);
            return name;
        };
        if (coat) {
            dielectric_base = mix(plain_diffuse, coated_diffuse(),
                                  field(prop("Pc"), {}, value("Pc", 0), "amount").parameter, where);
        } else if ((pr || pbr_metal) && !prop("Ks") && !map("map_Ks")) {
            dielectric_base = coated_diffuse();
        } else if (illum == 2) {
            auto amount = field(prop("Ks"), {}, luminance(ks), "amount");
            if (const auto spec = map("map_Ks")) {
                // Multiply linear RGB before reducing to luminance, never multiply spectra.
                auto prepared = prepare(*spec, prop("Ks") ? ks : RGB{1, 1, 1}, false, true);
                for (std::size_t i = 0; i < prepared.values.size(); i += 3) {
                    const auto v = static_cast<float>(luminance(
                        {prepared.values[i], prepared.values[i + 1], prepared.values[i + 2]}));
                    if (v < 0 || v > 1)
                        fail(spec->property->location, "Mapped Ks mix amount must be in [0,1]");
                    prepared.values[i] = prepared.values[i + 1] = prepared.values[i + 2] = v;
                }
                const auto &im = image(*spec);
                const auto path = exr(im, prepared.values, spec->property->location);
                amount.parameter = " \"texture amount\" " + quote(texture(*spec, path, true));
            }
            dielectric_base = mix(plain_diffuse, coated_diffuse(), amount.parameter, where);
        } else if (prop("Ks") || map("map_Ks")) {
            if (!conductor)
                warn(where, "Ks/map_Ks highlights are inactive for illum 0/1 diffuse");
        }
        if (conductor || pbr_metal) {
            warn(where, "Metal uses an RGB reflectance conductor approximation, not measured "
                        "complex refractive indices");
            const auto metal_name = binding.name + "_metal";
            const auto metal_reflectance =
                pbr_metal ? diffuse : spectrum(prop("Ks"), map("map_Ks"), ks, "reflectance");
            if (coat) {
                const auto bare = metal_name + "_bare";
                named(bare, "conductor", metal_reflectance + rough.parameter + remap + detail,
                      where);
                const auto cr =
                    field(prop("Pcr"), {}, value("Pcr", .5), "interface.roughness", false, true);
                named(metal_name, "coatedconductor",
                      metal_reflectance + " \"float interface.eta\" " + number(value("Ni", 1.5)) +
                          cr.parameter + rough_parameter("conductor.roughness") + remap + detail,
                      where);
                const auto coated =
                    mix(bare, metal_name, field(prop("Pc"), {}, value("Pc", 0), "amount").parameter,
                        where);
                if (!pbr_metal) {
                    named(binding.name, "mix",
                          " \"string materials\" [" + quote(coated) + " " + quote(coated) +
                              "] \"float amount\" 0",
                          where);
                    return binding;
                }
                const auto amount = field(prop("Pm"), map("map_Pm"), value("Pm", 0), "amount");
                named(binding.name, "mix",
                      " \"string materials\" [" + quote(dielectric_base) + " " + quote(coated) +
                          "]" + amount.parameter,
                      where);
            } else {
                named(metal_name, "conductor", metal_reflectance + rough.parameter + remap + detail,
                      where);
                const auto amount =
                    pbr_metal ? field(prop("Pm"), map("map_Pm"), value("Pm", 0), "amount").parameter
                              : " \"float amount\" 1";
                named(binding.name, "mix",
                      " \"string materials\" [" + quote(dielectric_base) + " " + quote(metal_name) +
                          "]" + amount,
                      where);
            }
        } else {
            named(binding.name, "mix",
                  " \"string materials\" [" + quote(dielectric_base) + " " +
                      quote(dielectric_base) + "] \"float amount\" 0",
                  where);
        }
        return binding;
    }
    void run() {
        const auto where = Location(asset.entry.string());
        charge(retained_bytes(asset), where);
        // Input source ceilings are separate from the full retained Asset storage charge.
        for (const auto &s : asset.sources) {
            if (source_files >= limits.input.expansions ||
                s.bytes.size() > limits.input.expanded_bytes -
                                     std::min(source_bytes, limits.input.expanded_bytes))
                fail(Location(s.path.string()), "Aggregate source snapshots exceed input limits");
            ++source_files;
            source_bytes += s.bytes.size();
            charge_output(s.bytes.size(), Location(s.path.string()));
        }
        if (asset.materials.size() > limits.input.directives)
            fail(where, "Material count exceeds directive limit");
        // Resolve and snapshot the full image closure before choosing any global PBRT names.
        // image() caches the decoded image and original bytes, so material conversion neither
        // reopens dependencies nor charges their retained storage twice.
        std::vector<bool> preload_failed(asset.materials.size(), false);
        for (std::size_t i = 0; i < asset.materials.size(); ++i) {
            for (const auto &property : asset.materials[i].properties) {
                try {
                    if (++work > limits.input.directives)
                        fail(property.location, "Material dependency work exceeds directive limit");
                    if (image_property(property.name)) {
                        const auto map = parse_map(property, property.name == "bump" ||
                                                                 property.name == "map_bump");
                        image(map);
                    }
                } catch (const Rejected &) {
                    if (!output.diagnostics.empty() &&
                        output.diagnostics.back().code == "obj.limit")
                        throw;
                    preload_failed[i] = true;
                }
            }
        }
        Sha256 closure;
        hash_text(closure, prefix);
        for (const auto &source : output.sources)
            closure.update(sha256(source.bytes));
        prefix = "obj_" + digest_hex(closure.finish()) + "_";
        output.default_material.name = prefix + "default";
        named(output.default_material.name, "diffuse", " \"rgb reflectance\" [.5 .5 .5]",
              Location(asset.entry.string()));
        output.materials.resize(asset.materials.size());
        for (std::size_t i = 0; i < asset.materials.size(); ++i) {
            if (preload_failed[i])
                continue;
            try {
                output.materials[i] = material(asset.materials[i], i);
            } catch (const Rejected &) {
                if (!output.diagnostics.empty() && output.diagnostics.back().code == "obj.limit")
                    throw;
            }
        }
        for (const auto &part : asset.parts) {
            auto where = Location(asset.entry.string());
            if (!part.polygons.empty() && part.polygons[0] < asset.polygons.size())
                where = asset.polygons[part.polygons[0]].location;
            if (part.material && *part.material >= output.materials.size()) {
                diagnostic(Severity::error, where, "obj.material.index",
                           "Part material index is outside the source material table");
                continue;
            }
            const auto &binding =
                part.material ? output.materials[*part.material] : output.default_material;
            if (binding.requires_uvs && !part.complete_uvs)
                diagnostic(Severity::error, where, "obj.material.uv",
                           "Mapped material requires authored UV coordinates on every face corner");
            const auto &mesh = part.geometry;
            if (part.material) {
                constexpr std::size_t coordinates_per_frame = 6;
                if (mesh.triangles.size() >
                    (limits.input.total_values - std::min(value_work, limits.input.total_values)) /
                        coordinates_per_frame)
                    fail(where, "Expanded triangle texture frame value limit exceeded");
                value_work += mesh.triangles.size() * coordinates_per_frame;
                charge(mesh.triangles.size() * sizeof(TextureTriangleFrame), where);
                constexpr std::array<double, 16> identity{1, 0, 0, 0, 0, 1, 0, 0,
                                                          0, 0, 1, 0, 0, 0, 0, 1};
                std::vector<TextureTriangleFrame> frames;
                if (const auto message = prepare_texture_triangle_frames(mesh, identity, identity,
                                                                         frames, {0, 0, 0});
                    !message.empty())
                    diagnostic(Severity::error, where, "obj.material.frame", message);
            }
            if (!binding.emission.empty()) {
                const auto area_error = validate_area_geometry(mesh, {});
                if (!area_error.empty())
                    diagnostic(Severity::error, where, "obj.material.emission", area_error);
                if (mesh.normals.size() >
                    (limits.input.total_values - std::min(value_work, limits.input.total_values)) /
                        3)
                    fail(where, "Expanded shading normal value limit exceeded");
                value_work += mesh.normals.size() * 3;
                charge(mesh.normals.size() * sizeof(GeometryPoint), where);
                std::vector<GeometryPoint> normals;
                const auto normal_error = prepare_shading_normals(mesh, {}, false, normals);
                if (!normal_error.empty())
                    diagnostic(Severity::error, where, "obj.material.normal", normal_error);
                else if (area_error.empty()) {
                    if (const auto message =
                            validate_normal_emission(mesh, {}, false, false, normals);
                        !message.empty())
                        diagnostic(Severity::unsupported, where,
                                   "obj.material.emission.orientation", message);
                }
            }
        }
    }
};
} // namespace

int Conversion::exit_code() const {
    bool unsupported = false;
    for (const auto &d : diagnostics) {
        if (d.severity == Severity::error)
            return 1;
        unsupported = unsupported || d.severity == Severity::unsupported;
    }
    return unsupported ? 2 : 0;
}

Conversion prepare_materials(const Asset &asset, const Limits &limits) {
    Builder builder(asset, limits);
    try {
        builder.run();
    } catch (const Rejected &) {
        return std::move(builder.output);
    } catch (const std::exception &e) {
        builder.diagnostic(Severity::error, Location(asset.entry.string()), "obj.material.resource",
                           e.what());
    }
    return std::move(builder.output);
}
} // namespace rypath::obj
