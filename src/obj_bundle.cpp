// SPDX-License-Identifier: GPL-3.0-only
#include "obj.hpp"
#include "pbrt_render.hpp"
#include "sha256.hpp"

#include <algorithm>
#include <bit>
#include <cerrno>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#include <streambuf>
#include <system_error>
#include <unistd.h>
#if defined(__linux__)
#include <fcntl.h>
#include <sys/syscall.h>
#endif

namespace rypath::obj {
namespace {
namespace fs = std::filesystem;

// Bound textual serialization before appending, including metadata with many records.
class Text final : public std::streambuf {
    std::size_t remaining_;

  public:
    explicit Text(std::size_t limit) : remaining_(limit), stream(this) {
        stream.exceptions(std::ios::badbit | std::ios::failbit);
        stream.imbue(std::locale::classic());
        stream << std::setprecision(std::numeric_limits<double>::max_digits10);
    }
    std::string value;
    std::ostream stream;

  private:
    std::streamsize xsputn(const char *s, std::streamsize n) override {
        if (n < 0 || static_cast<std::size_t>(n) > remaining_)
            throw std::runtime_error("OBJ serialized output limit exceeded");
        value.append(s, static_cast<std::size_t>(n));
        remaining_ -= static_cast<std::size_t>(n);
        return n;
    }
    int_type overflow(int_type c) override {
        if (!traits_type::eq_int_type(c, traits_type::eof())) {
            const auto ch = traits_type::to_char_type(c);
            xsputn(&ch, 1);
        }
        return traits_type::not_eof(c);
    }
};

bool valid_utf8(std::string_view value) {
    for (std::size_t i = 0; i < value.size(); ++i) {
        const auto lead = static_cast<unsigned char>(value[i]);
        if (lead < 0x80)
            continue;
        const std::size_t count = lead >= 0xc2 && lead <= 0xdf   ? 2
                                  : lead >= 0xe0 && lead <= 0xef ? 3
                                  : lead >= 0xf0 && lead <= 0xf4 ? 4
                                                                 : 0;
        if (count == 0 || count > value.size() - i)
            return false;
        const auto next = static_cast<unsigned char>(value[i + 1]);
        if ((lead == 0xe0 && next < 0xa0) || (lead == 0xed && next >= 0xa0) ||
            (lead == 0xf0 && next < 0x90) || (lead == 0xf4 && next >= 0x90))
            return false;
        for (std::size_t j = 1; j < count; ++j) {
            const auto c = static_cast<unsigned char>(value[i + j]);
            if (c < 0x80 || c > 0xbf)
                return false;
        }
        i += count - 1;
    }
    return true;
}

void json_string(std::ostream &out, std::string_view value) {
    if (!valid_utf8(value))
        throw std::runtime_error("OBJ bundle metadata requires valid UTF-8 names and paths");
    constexpr char hex[] = "0123456789abcdef";
    out << '"';
    for (const unsigned char c : value) {
        if (c == '"' || c == '\\')
            out << '\\' << static_cast<char>(c);
        else if (c < 32)
            out << "\\u00" << hex[c >> 4U] << hex[c & 15U];
        else
            out << static_cast<char>(c);
    }
    out << '"';
}

std::span<const std::uint8_t> bytes(std::string_view text) {
    return {reinterpret_cast<const std::uint8_t *>(text.data()), text.size()};
}

struct Written {
    std::string path;
    std::string hash;
    std::size_t size;
};

struct Stage {
    fs::path path;
    explicit Stage(const fs::path &parent) {
        auto name = (parent / ".rypath-obj-XXXXXX").string();
        if (!mkdtemp(name.data()))
            throw std::system_error(errno, std::generic_category(), "create OBJ staging directory");
        path = name;
    }
    Stage(const Stage &) = delete;
    Stage &operator=(const Stage &) = delete;
    ~Stage() {
        if (!path.empty()) {
            std::error_code ignored;
            fs::remove_all(path, ignored);
        }
    }
};

class Writer {
  public:
    struct Budget {
        std::size_t output;
        std::size_t workspace;
    };
    Writer(fs::path root, Budget budget)
        : root_(std::move(root)), remaining_(budget.output), workspace_(budget.workspace) {}
    void write(const fs::path &relative, std::span<const std::uint8_t> data) {
        if (relative.empty() || relative.is_absolute() || relative != relative.lexically_normal())
            throw std::runtime_error("Invalid OBJ bundle-relative output path");
        for (const auto &part : relative) {
            if (part == "." || part == "..")
                throw std::runtime_error("Invalid OBJ bundle-relative output component");
        }
        const auto key = relative.generic_string();
        const auto cost = 4 * (key.size() + sizeof(Written)) + 512;
        if (cost > workspace_)
            throw std::runtime_error("OBJ publication metadata exceeds working byte limit");
        workspace_ -= cost;
        if (!names_.insert(key).second)
            throw std::runtime_error("Duplicate OBJ bundle output path: " + key);
        if (data.size() > remaining_)
            throw std::runtime_error("OBJ aggregate output limit exceeded");
        remaining_ -= data.size();
        const auto destination = root_ / relative;
        fs::create_directories(destination.parent_path());
        std::ofstream out(destination, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char *>(data.data()),
                  static_cast<std::streamsize>(data.size()));
        out.close();
        if (!out.good())
            throw std::runtime_error("Could not write OBJ bundle file: " + key);
        files.push_back({key, digest_hex(sha256(data)), data.size()});
    }
    std::size_t remaining() const { return remaining_; }
    std::vector<Written> files;

  private:
    fs::path root_;
    std::size_t remaining_;
    std::size_t workspace_;
    std::set<std::string> names_;
};

std::vector<std::uint8_t> ply(const GeometryMesh &mesh, std::size_t limit) {
    if (!mesh.patches.empty() || !mesh.tangents.empty() || mesh.positions.empty() ||
        mesh.triangles.empty() || mesh.positions.size() > std::numeric_limits<std::int32_t>::max())
        throw std::runtime_error("OBJ export requires nonempty checked triangle geometry");
    const bool normals = !mesh.normals.empty(), uvs = !mesh.uvs.empty();
    if ((normals && mesh.normals.size() != mesh.positions.size()) ||
        (uvs && mesh.uvs.size() != mesh.positions.size()))
        throw std::runtime_error("OBJ export requires complete vertex attributes");
    Text header(limit);
    header.stream << "ply\nformat binary_little_endian 1.0\ncomment rypath obj-polygon-v1\n"
                  << "element vertex " << mesh.positions.size()
                  << "\nproperty float x\nproperty float y\nproperty float z\n";
    if (normals)
        header.stream << "property float nx\nproperty float ny\nproperty float nz\n";
    if (uvs)
        header.stream << "property float u\nproperty float v\n";
    header.stream << "element face " << mesh.triangles.size()
                  << "\nproperty list uchar int vertex_indices\nend_header\n";
    const std::size_t stride = 12 + (normals ? 12 : 0) + (uvs ? 8 : 0);
    const auto header_bytes = header.value.size();
    if (header_bytes > limit || mesh.positions.size() > (limit - header_bytes) / stride)
        throw std::runtime_error("OBJ mesh output limit exceeded");
    const auto vertex_end = header_bytes + mesh.positions.size() * stride;
    if (mesh.triangles.size() > (limit - vertex_end) / 13)
        throw std::runtime_error("OBJ triangle output limit exceeded");
    std::vector<std::uint8_t> result(header.value.begin(), header.value.end());
    result.reserve(vertex_end + mesh.triangles.size() * 13);
    const auto integer = [&](std::uint32_t v) {
        for (unsigned i = 0; i < 4; ++i)
            result.push_back(static_cast<std::uint8_t>(v >> (8U * i)));
    };
    const auto real = [&](float v) { integer(std::bit_cast<std::uint32_t>(v)); };
    const auto point = [&](GeometryPoint p) {
        real(p.x);
        real(p.y);
        real(p.z);
    };
    for (std::size_t i = 0; i < mesh.positions.size(); ++i) {
        point(mesh.positions[i]);
        if (normals)
            point(mesh.normals[i]);
        if (uvs) {
            real(mesh.uvs[i][0]);
            real(mesh.uvs[i][1]);
        }
    }
    for (const auto &triangle : mesh.triangles) {
        result.push_back(3);
        for (const auto index : triangle) {
            if (index >= mesh.positions.size())
                throw std::runtime_error("OBJ triangle index is outside its vertex array");
            integer(index);
        }
    }
    return result;
}

void source_json(std::ostream &out, const Source &source, const fs::path &base) {
    out << "{\"path\":";
    json_string(out, source.path.lexically_relative(base).generic_string());
    out << ",\"kind\":";
    json_string(out, source.kind);
    out << ",\"sha256\":";
    json_string(out, digest_hex(sha256(source.bytes)));
    out << ",\"bytes\":" << source.bytes.size() << '}';
}

void location_json(std::ostream &out, const Location &location, const fs::path &base) {
    out << "{\"file\":";
    json_string(out, fs::path(*location.file).lexically_relative(base).generic_string());
    out << ",\"line\":" << location.line << ",\"column\":" << location.column << '}';
}

std::string manifest(const Asset &asset, const Conversion &conversion,
                     const std::vector<Written> &files, std::size_t limit) {
    Text text(limit);
    auto &out = text.stream;
    out << "{\"schema\":1,\"profile\":\"" << profile << "\",\"materials\":\"" << material_profile
        << "\",\"entry\":\"model.pbrt\",\"pathBase\":\"bundle-root\","
        << "\"sources\":[";
    bool first = true;
    for (const auto *sources : {&asset.sources, &conversion.sources}) {
        for (const auto &source : *sources) {
            if (!first)
                out << ',';
            first = false;
            source_json(out, source, asset.entry.parent_path());
        }
    }
    out << "],\"files\":[";
    first = true;
    for (const auto &file : files) {
        if (!first)
            out << ',';
        first = false;
        out << "{\"path\":";
        json_string(out, file.path);
        out << ",\"sha256\":";
        json_string(out, file.hash);
        out << ",\"bytes\":" << file.size << '}';
    }
    out << "],\"authoredMaterials\":[";
    first = true;
    for (const auto &material : asset.materials) {
        if (!first)
            out << ',';
        first = false;
        out << "{\"name\":";
        json_string(out, material.name);
        out << ",\"location\":";
        location_json(out, material.location, asset.entry.parent_path());
        out << ",\"properties\":[";
        bool first_property = true;
        for (const auto &property : material.properties) {
            if (!first_property)
                out << ',';
            first_property = false;
            out << "{\"name\":";
            json_string(out, property.name);
            out << ",\"location\":";
            location_json(out, property.location, asset.entry.parent_path());
            out << ",\"arguments\":[";
            for (std::size_t i = 0; i < property.arguments.size(); ++i) {
                if (i)
                    out << ',';
                json_string(out, property.arguments[i]);
            }
            out << "]}";
        }
        out << "]}";
    }
    out << "],\"polygons\":[";
    first = true;
    for (const auto &polygon : asset.polygons) {
        if (!first)
            out << ',';
        first = false;
        out << "{\"line\":" << polygon.location.line << ",\"object\":";
        json_string(out, polygon.object);
        out << ",\"smoothing\":" << polygon.smoothing << ",\"material\":";
        if (polygon.material)
            out << *polygon.material;
        else
            out << "null";
        out << ",\"groups\":[";
        for (std::size_t i = 0; i < polygon.groups.size(); ++i) {
            if (i)
                out << ',';
            json_string(out, polygon.groups[i]);
        }
        out << "]}";
    }
    out << "],\"parts\":[";
    for (std::size_t i = 0; i < asset.parts.size(); ++i) {
        if (i)
            out << ',';
        out << "{\"mesh\":\"meshes/part-" << i << ".ply\",\"trianglePolygons\":[";
        const auto &part = asset.parts[i];
        for (std::size_t j = 0; j < part.triangle_polygons.size(); ++j) {
            if (j)
                out << ',';
            out << part.triangle_polygons[j];
        }
        out << "]}";
    }
    out << "],\"diagnostics\":[";
    first = true;
    for (const auto &diagnostic : conversion.diagnostics) {
        if (!first)
            out << ',';
        first = false;
        out << "{\"code\":";
        json_string(out, diagnostic.code);
        out << ",\"severity\":";
        json_string(out, diagnostic.severity == Severity::error         ? "error"
                         : diagnostic.severity == Severity::unsupported ? "unsupported"
                                                                        : "warning");
        out << ",\"location\":";
        location_json(out, diagnostic.location, asset.entry.parent_path());
        out << ",\"message\":";
        json_string(out, diagnostic.message);
        out << '}';
    }
    out << "]}\n";
    return std::move(text.value);
}

// The no-replacement rename is atomic even if another process creates the destination.
void publish_directory(const fs::path &from, const fs::path &to) {
    int result;
    do {
#if defined(__APPLE__)
        result = renamex_np(from.c_str(), to.c_str(), RENAME_EXCL);
#elif defined(__linux__) && defined(SYS_renameat2)
        constexpr unsigned no_replace = 1;
        result = static_cast<int>(
            syscall(SYS_renameat2, AT_FDCWD, from.c_str(), AT_FDCWD, to.c_str(), no_replace));
#else
        errno = ENOTSUP;
        result = -1;
#endif
    } while (result < 0 && errno == EINTR);
    if (result != 0)
        throw std::system_error(errno, std::generic_category(), "publish new OBJ bundle directory");
}

constexpr std::string_view validation_header =
    "Camera \"perspective\"\nFilm \"rgb\" \"integer xresolution\" [1] "
    "\"integer yresolution\" [1] \"string sensor\" \"cie1931\"\n"
    "PixelFilter \"box\"\nSampler \"independent\" \"integer pixelsamples\" [1]\n"
    "Integrator \"path\" \"integer maxdepth\" [1]\nWorldBegin\n"
    "LightSource \"infinite\"\nInclude \"model.pbrt\"\n";
constexpr std::string_view instructions =
    "rypath OBJ/MTL asset bundle\n\n"
    "model.pbrt is a world fragment, without a camera, film, sampler or lighting setup.\n"
    "Place your enclosing PBRT scene at this bundle root and Include \"model.pbrt\"\n"
    "after WorldBegin. PBRT resolves all asset paths from the enclosing scene directory.\n"
    "Move the entire directory together. conversion.json records source hashes, material\n"
    "approximations and authored metadata. Source assets retain their original terms.\n";
} // namespace

int publish(const Asset &asset, Conversion &conversion, const fs::path &directory,
            const Limits &limits) {
    if (conversion.exit_code() != 0)
        return conversion.exit_code();
    try {
        const auto asset_bytes = retained_bytes(asset),
                   conversion_bytes = retained_bytes(conversion);
        if (asset_bytes > limits.working_bytes ||
            conversion_bytes > limits.working_bytes - asset_bytes)
            throw std::runtime_error("OBJ retained inputs exceed publication working byte limit");
        // Divide remaining OBJ-owned workspace among file metadata, growable text,
        // and one serialized mesh. Core PBRT validation has its own existing limits.
        const auto scratch = (limits.working_bytes - asset_bytes - conversion_bytes) / 16;
        if (scratch < 4096)
            throw std::runtime_error(
                "OBJ publication working byte limit has no serialization workspace");
        if (directory.empty() || directory.filename().empty() || directory.filename() == "." ||
            directory.filename() == "..")
            throw std::runtime_error("OBJ output must name a new bundle directory");
        const auto destination = fs::absolute(directory).lexically_normal();
        if (fs::exists(fs::symlink_status(destination)))
            throw std::runtime_error("OBJ output directory already exists");
        if (!fs::is_directory(destination.parent_path()))
            throw std::runtime_error("OBJ output parent directory does not exist");
        if (asset.parts.empty() || conversion.materials.size() != asset.materials.size())
            throw std::runtime_error("OBJ bundle requires complete geometry and material bindings");
        Stage stage(destination.parent_path());
        Writer writer(stage.path, {.output = limits.output_bytes, .workspace = 4 * scratch});
        for (const auto &file : conversion.files)
            writer.write(file.path, file.bytes);
        Text fragment(std::min(writer.remaining(), scratch));
        fragment.stream << "# rypath " << profile << " / " << material_profile
                        << "\n# Conversion assumptions and warnings: conversion.json\n"
                        << conversion.definitions;
        for (std::size_t i = 0; i < asset.parts.size(); ++i) {
            const auto &part = asset.parts[i];
            if (part.material && *part.material >= conversion.materials.size())
                throw std::runtime_error("Invalid OBJ part material binding");
            const auto &material =
                part.material ? conversion.materials[*part.material] : conversion.default_material;
            if (material.requires_uvs && !part.complete_uvs)
                throw std::runtime_error("Mapped OBJ material requires complete UV coordinates");
            const auto mesh_file = "meshes/part-" + std::to_string(i) + ".ply";
            const auto mesh =
                ply(part.geometry,
                    std::min({writer.remaining(), limits.input.file_bytes, 4 * scratch}));
            writer.write(mesh_file, mesh);
            fragment.stream << "AttributeBegin\nNamedMaterial \"" << material.name << "\"\n";
            if (!material.emission.empty())
                fragment.stream << material.emission << '\n';
            fragment.stream << "Shape \"plymesh\" \"string filename\" \"" << mesh_file << "\" "
                            << material.alpha << "\nAttributeEnd\n";
        }
        writer.write("model.pbrt", bytes(fragment.value));
        // This temporary wrapper validates the same relative paths consumers will use.
        writer.write(".validate.pbrt", bytes(validation_header));
        const auto checked = pbrt::check(stage.path / ".validate.pbrt", limits.input);
        if (checked.exit_code() != 0) {
            conversion.diagnostics.insert(conversion.diagnostics.end(),
                                          checked.document.diagnostics.begin(),
                                          checked.document.diagnostics.end());
            return conversion.exit_code();
        }
        const auto translated =
            pbrt::translate(checked, Location{asset.entry.string(), 1, 1}, limits.input);
        if (translated.exit_code() != 0) {
            conversion.diagnostics.insert(conversion.diagnostics.end(),
                                          translated.diagnostics.begin(),
                                          translated.diagnostics.end());
            if (conversion.exit_code() == 0)
                throw std::runtime_error("OBJ bundle render translation did not produce a scene");
            return conversion.exit_code();
        }
        fs::remove(stage.path / ".validate.pbrt");
        writer.files.pop_back();
        writer.write("README.txt", bytes(instructions));
        const auto metadata =
            manifest(asset, conversion, writer.files, std::min(writer.remaining(), scratch));
        writer.write("conversion.json", bytes(metadata));
        publish_directory(stage.path, destination);
        stage.path.clear();
        return 0;
    } catch (const std::exception &error) {
        conversion.diagnostics.push_back({Severity::error, "obj-publication",
                                          Location{asset.entry.string(), 1, 1}, error.what()});
        return 1;
    }
}
} // namespace rypath::obj
