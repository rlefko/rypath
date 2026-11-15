// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "geometry.hpp"
#include "sha256.hpp"
#include <bit>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <set>
#include <span>
#include <stdexcept>
#include <streambuf>
#include <system_error>
#include <unistd.h>
namespace rypath::mesh_bundle {
namespace fs = std::filesystem;
// Bound textual serialization before appending, including metadata with many records.
class Text final : public std::streambuf {
    std::size_t remaining_;
    std::string label_;

  public:
    explicit Text(std::size_t limit, std::string label = "OBJ")
        : remaining_(limit), label_(std::move(label)), stream(this) {
        stream.exceptions(std::ios::badbit | std::ios::failbit);
        stream.imbue(std::locale::classic());
        stream << std::setprecision(std::numeric_limits<double>::max_digits10);
    }
    std::string value;
    std::ostream stream;

  private:
    std::streamsize xsputn(const char *s, std::streamsize n) override {
        if (n < 0 || static_cast<std::size_t>(n) > remaining_)
            throw std::runtime_error(label_ + " serialized output limit exceeded");
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

inline bool valid_utf8(std::string_view value) {
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

inline void json_string(std::ostream &out, std::string_view value, std::string_view label = "OBJ") {
    if (!valid_utf8(value))
        throw std::runtime_error(std::string(label) +
                                 " bundle metadata requires valid UTF-8 names and paths");
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

inline std::span<const std::uint8_t> bytes(std::string_view text) {
    return {reinterpret_cast<const std::uint8_t *>(text.data()), text.size()};
}

struct Written {
    std::string path;
    std::string hash;
    std::size_t size;
};

struct Stage {
    fs::path path;
    explicit Stage(const fs::path &parent, std::string label = "OBJ") {
        auto name =
            (parent / (label == "OBJ" ? ".rypath-obj-XXXXXX" : ".rypath-stl-XXXXXX")).string();
        if (!mkdtemp(name.data()))
            throw std::system_error(errno, std::generic_category(),
                                    "create " + label + " staging directory");
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
    Writer(fs::path root, Budget budget, std::string label = "OBJ")
        : root_(std::move(root)), remaining_(budget.output), workspace_(budget.workspace),
          label_(std::move(label)) {}
    void write(const fs::path &relative, std::span<const std::uint8_t> data) {
        if (relative.empty() || relative.is_absolute() || relative != relative.lexically_normal())
            throw std::runtime_error("Invalid " + label_ + " bundle-relative output path");
        for (const auto &part : relative) {
            if (part == "." || part == "..")
                throw std::runtime_error("Invalid " + label_ + " bundle-relative output component");
        }
        const auto key = relative.generic_string();
        const auto cost = 4 * (key.size() + sizeof(Written)) + 512;
        if (cost > workspace_)
            throw std::runtime_error("" + label_ +
                                     " publication metadata exceeds working byte limit");
        workspace_ -= cost;
        if (!names_.insert(key).second)
            throw std::runtime_error("Duplicate " + label_ + " bundle output path: " + key);
        if (data.size() > remaining_)
            throw std::runtime_error("" + label_ + " aggregate output limit exceeded");
        remaining_ -= data.size();
        const auto destination = root_ / relative;
        fs::create_directories(destination.parent_path());
        std::ofstream out(destination, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char *>(data.data()),
                  static_cast<std::streamsize>(data.size()));
        out.close();
        if (!out.good())
            throw std::runtime_error("Could not write " + label_ + " bundle file: " + key);
        files.push_back({key, digest_hex(sha256(data)), data.size()});
    }
    std::size_t remaining() const { return remaining_; }
    std::vector<Written> files;

  private:
    fs::path root_;
    std::size_t remaining_;
    std::size_t workspace_;
    std::set<std::string> names_;
    std::string label_;
};

inline std::vector<std::uint8_t> ply(const GeometryMesh &mesh, std::size_t limit,
                                     std::string_view label = "OBJ",
                                     std::string_view profile = "obj-polygon-v1") {
    if (!mesh.patches.empty() || !mesh.tangents.empty() || mesh.positions.empty() ||
        mesh.triangles.empty() || mesh.positions.size() > std::numeric_limits<std::int32_t>::max())
        throw std::runtime_error(std::string(label) +
                                 " export requires nonempty checked triangle geometry");
    const bool normals = !mesh.normals.empty(), uvs = !mesh.uvs.empty();
    if ((normals && mesh.normals.size() != mesh.positions.size()) ||
        (uvs && mesh.uvs.size() != mesh.positions.size()))
        throw std::runtime_error(std::string(label) +
                                 " export requires complete vertex attributes");
    Text header(limit, std::string(label));
    header.stream << "ply\nformat binary_little_endian 1.0\ncomment rypath " << profile << "\n"
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
        throw std::runtime_error(std::string(label) + " mesh output limit exceeded");
    const auto vertex_end = header_bytes + mesh.positions.size() * stride;
    if (mesh.triangles.size() > (limit - vertex_end) / 13)
        throw std::runtime_error(std::string(label) + " triangle output limit exceeded");
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
                throw std::runtime_error(std::string(label) +
                                         " triangle index is outside its vertex array");
            integer(index);
        }
    }
    return result;
}

void publish_directory(const fs::path &from, const fs::path &to, std::string_view label = "OBJ");
} // namespace rypath::mesh_bundle
