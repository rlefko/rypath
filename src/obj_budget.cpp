// SPDX-License-Identifier: GPL-3.0-only
#include "obj.hpp"

#include <limits>

namespace rypath::obj {
namespace {
class Size {
  public:
    std::size_t bytes = 0;
    void add(std::size_t count, std::size_t width = 1) {
        const auto remaining = std::numeric_limits<std::size_t>::max() - bytes;
        if (count > remaining / width)
            bytes = std::numeric_limits<std::size_t>::max();
        else
            bytes += count * width;
    }
    template <typename T> void vector(const std::vector<T> &v) {
        if (v.capacity()) {
            add(v.capacity(), sizeof(T));
            add(64); // Conservative allocator bookkeeping.
        }
    }
    void string(const std::string &s) {
        add(s.capacity());
        add(65); // Includes terminator and bookkeeping; also charges SSO strings.
    }
    void path(const std::filesystem::path &p) {
        add(p.native().capacity(), sizeof(std::filesystem::path::value_type));
        add(65);
    }
    void location(const Location &l) {
        if (l.file) {
            add(sizeof(std::string));
            string(*l.file);
        }
    }
    void sources(const std::vector<Source> &sources) {
        vector(sources);
        for (const auto &s : sources) {
            path(s.path);
            string(s.kind);
            vector(s.bytes);
        }
    }
    void binding(const MaterialBinding &binding) {
        string(binding.name);
        string(binding.alpha);
        string(binding.emission);
    }
};
} // namespace

std::size_t retained_bytes(const Asset &asset) {
    Size size;
    size.add(sizeof(Asset));
    size.path(asset.entry);
    size.sources(asset.sources);
    size.vector(asset.positions);
    size.vector(asset.uvs);
    size.vector(asset.normals);
    size.vector(asset.polygons);
    for (const auto &p : asset.polygons) {
        size.vector(p.corners);
        size.string(p.object);
        size.vector(p.groups);
        for (const auto &group : p.groups)
            size.string(group);
        size.location(p.location);
    }
    size.vector(asset.materials);
    for (const auto &m : asset.materials) {
        size.string(m.name);
        size.location(m.location);
        size.vector(m.properties);
        for (const auto &p : m.properties) {
            size.string(p.name);
            size.location(p.location);
            size.vector(p.arguments);
            for (const auto &arg : p.arguments)
                size.string(arg);
        }
    }
    size.vector(asset.parts);
    for (const auto &p : asset.parts) {
        size.vector(p.polygons);
        size.vector(p.triangle_polygons);
        const auto &g = p.geometry;
        size.vector(g.positions);
        size.vector(g.normals);
        size.vector(g.uvs);
        size.vector(g.tangents);
        size.vector(g.triangles);
        size.vector(g.patches);
    }
    return size.bytes;
}

std::size_t retained_bytes(const Conversion &conversion) {
    Size size;
    size.add(sizeof(Conversion));
    size.string(conversion.definitions);
    size.vector(conversion.materials);
    for (const auto &binding : conversion.materials)
        size.binding(binding);
    size.binding(conversion.default_material);
    size.vector(conversion.files);
    for (const auto &file : conversion.files) {
        size.path(file.path);
        size.vector(file.bytes);
    }
    size.sources(conversion.sources);
    size.vector(conversion.diagnostics);
    for (const auto &d : conversion.diagnostics) {
        size.string(d.code);
        size.string(d.message);
        size.location(d.location);
    }
    return size.bytes;
}
} // namespace rypath::obj
