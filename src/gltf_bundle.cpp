// SPDX-License-Identifier: GPL-3.0-only
#include "gltf.hpp"
#include "mesh_bundle.hpp"
#include "pbrt_numeric.hpp"
#include "pbrt_render.hpp"
#include <algorithm>
#include <cmath>
#include <set>
namespace rypath::gltf {
namespace {
namespace fs = std::filesystem;
using mesh_bundle::bytes;
using mesh_bundle::Text;
constexpr std::string_view wrapper =
    "Camera \"perspective\"\nFilm \"rgb\" \"integer xresolution\" [1] \"integer yresolution\" [1] "
    "\"string sensor\" \"cie1931\"\n"
    "PixelFilter \"box\"\nSampler \"independent\" \"integer pixelsamples\" [1]\nIntegrator "
    "\"path\" \"integer maxdepth\" [1]\nWorldBegin\nLightSource \"infinite\"\nInclude "
    "\"model.pbrt\"\n";
constexpr std::string_view instructions =
    "rypath static glTF/GLB asset bundle\n\n"
    "model.pbrt is a world fragment using the versioned rypath-gltf-v1 material\n"
    "and glTF sampler extensions. Stock PBRT compatibility is not claimed. Place\n"
    "your enclosing scene at this bundle root and Include \"model.pbrt\" after\n"
    "WorldBegin. Move the entire directory together. Camera and lighting are caller-owned.\n"
    "Core metallic-roughness is an explicit RGB-to-spectral rendering adaptation.\n"
    "AO is ignored by path transport. Visibility and emitter alpha use LOD zero.\n"
    "conversion.json retains original source/resource hashes, selected scene, names,\n"
    "primitive/placement mappings, material/sampler state, output hashes and diagnostics.\n"
    "Original source assets retain their terms.\n";
[[noreturn]] void fail(std::string s) { throw std::runtime_error(std::move(s)); }
void json(std::ostream &o, std::string_view s) { mesh_bundle::json_string(o, s, "glTF"); }
std::size_t size(const Conversion &c) {
    std::size_t n = sizeof(c) + c.files.capacity() * sizeof(BundleFile) +
                    c.diagnostics.capacity() * sizeof(Diagnostic);
    for (auto &f : c.files)
        n += f.bytes.capacity() + f.path.native().capacity() + 128;
    for (auto &d : c.diagnostics)
        n += d.message.capacity() + d.code.capacity() +
             (d.location.file ? d.location.file->capacity() : 0) + 256;
    return n;
}
std::size_t available(const Asset &a, const Conversion &c, const Limits &l) {
    const auto n = retained_bytes(a), m = size(c);
    if (n > l.working_bytes || m > l.working_bytes - n)
        fail("glTF retained conversion exceeds working limit");
    return l.working_bytes - n - m;
}
void validate(const Asset &a, const Limits &l) {
    if (a.primitives.empty() || a.materials.empty() || a.placements.empty() ||
        a.selected_scene >= a.scene_names.size())
        fail("glTF bundle requires checked nonempty scene geometry");
    if (a.primitives.size() > l.input.expansions || a.placements.size() > l.input.directives ||
        l.input.active_files < 2 || l.input.scopes < 3)
        fail("glTF generated scene exceeds parser limits");
    for (auto &m : a.materials) {
        for (auto x : m.base_color)
            if (!std::isfinite(x) || x < 0 || x > 1)
                fail("invalid base color factor");
        for (auto x : m.emissive)
            if (!std::isfinite(x) || x < 0 || x > 1)
                fail("invalid emission factor");
        if (!std::isfinite(m.metallic) || m.metallic < 0 || m.metallic > 1 ||
            !std::isfinite(m.roughness) || m.roughness < 0 || m.roughness > 1 ||
            !std::isfinite(m.normal_scale) || !std::isfinite(m.alpha_cutoff) ||
            m.alpha_cutoff < 0 ||
            (m.alpha_mode != "OPAQUE" && m.alpha_mode != "MASK" && m.alpha_mode != "BLEND"))
            fail("invalid material factors");
        if (m.emits() && m.alpha_mode == "BLEND" &&
            (m.base_color[3] != 1 ||
             (m.base_texture && (*m.base_texture >= a.textures.size() ||
                                 a.textures[*m.base_texture].image >= a.images.size() ||
                                 !a.images[a.textures[*m.base_texture].image].opaque))))
            fail("fractional BLEND emission unsupported");
        for (auto t :
             {m.base_texture, m.metallic_roughness_texture, m.normal_texture, m.emissive_texture})
            if (t && *t >= a.textures.size())
                fail("material texture index out of range");
    }
    for (auto &t : a.textures) {
        const auto wrap = [](int x) { return x == 10497 || x == 33648 || x == 33071; };
        if (t.image >= a.images.size() || !wrap(t.wrap_s) || !wrap(t.wrap_t) ||
            (t.mag_filter != 9728 && t.mag_filter != 9729) ||
            (t.min_filter != 9728 && t.min_filter != 9729 &&
             (t.min_filter < 9984 || t.min_filter > 9987)))
            fail("invalid texture/sampler binding");
    }
    for (auto &i : a.images)
        if (i.bytes.empty() || (i.mime_type != "image/png" && i.mime_type != "image/jpeg"))
            fail("invalid image snapshot");
    std::size_t values = 0;
    for (auto &p : a.primitives) {
        const auto &g = p.geometry;
        if (p.material >= a.materials.size() || g.positions.empty() || g.triangles.empty() ||
            g.positions.size() != g.normals.size() ||
            (!g.uvs.empty() && g.uvs.size() != g.positions.size()) ||
            (!g.tangents.empty() && (g.tangents.size() != g.positions.size() ||
                                     g.tangent_signs.size() != g.positions.size())) ||
            !g.patches.empty())
            fail("incomplete primitive geometry");
        if (g.positions.size() > l.input.total_values / 16 ||
            g.triangles.size() > l.input.total_values / 3 ||
            values > l.input.total_values - g.positions.size() * 16)
            fail("glTF generated geometry value limit exceeded");
        values += g.positions.size() * 16;
        for (auto q : g.positions)
            if (!std::isfinite(q.x) || !std::isfinite(q.y) || !std::isfinite(q.z))
                fail("nonfinite generated position");
        for (auto tri : g.triangles) {
            for (auto i : tri)
                if (i >= g.positions.size())
                    fail("generated triangle index out of range");
            const auto x = g.positions[tri[0]], y = g.positions[tri[1]], z = g.positions[tri[2]];
            const double ax = double(y.x) - x.x, ay = double(y.y) - x.y, az = double(y.z) - x.z,
                         bx = double(z.x) - x.x, by = double(z.y) - x.y, bz = double(z.z) - x.z;
            if (std::hypot(ay * bz - az * by, az * bx - ax * bz, ax * by - ay * bx) == 0)
                fail("zero-area generated triangle");
        }
    }
    for (auto &p : a.placements) {
        if (p.primitive >= a.primitives.size())
            fail("placement primitive index out of range");
        pbrt::Transform m;
        for (std::size_t i = 0; i < 16; ++i) {
            if (!std::isfinite(p.transform[i]))
                fail("nonfinite placement matrix");
            m.values[i] = p.transform[i];
        }
        if (p.transform[12] != 0 || p.transform[13] != 0 || p.transform[14] != 0 ||
            p.transform[15] != 1 || !pbrt::exact_linear_determinant_sign(m))
            fail("nonaffine/singular placement matrix");
    }
}
std::string image_path(const Asset &a, std::size_t i) {
    return "images/image-" + std::to_string(i) +
           (a.images[i].mime_type == "image/png" ? ".png" : ".jpg");
}
std::string metadata(const Asset &a, const Conversion &c, std::size_t limit) {
    Text t(limit, "glTF");
    auto &o = t.stream;
    o << "{\"schema\":1,\"profile\":\"" << profile << "\",\"materialProfile\":\""
      << material_profile
      << "\",\"stockPbrtCompatible\":false,\"entry\":\"model.pbrt\",\"pathBase\":\"bundle-root\","
         "\"source\":{\"path\":";
    json(o, a.entry.filename().generic_string());
    o << ",\"bytes\":" << a.bytes.size() << ",\"sha256\":";
    json(o, digest_hex(sha256(a.bytes)));
    o << ",\"container\":";
    json(o, a.binary ? "GLB" : "glTF");
    o << "},\"selectedScene\":" << a.selected_scene << ",\"scenes\":[";
    for (std::size_t i = 0; i < a.scene_names.size(); ++i) {
        if (i)
            o << ',';
        json(o, a.scene_names[i]);
    }
    o << "],\"resources\":[";
    for (std::size_t i = 0; i < a.resources.size(); ++i) {
        if (i)
            o << ',';
        auto &r = a.resources[i];
        o << "{\"uri\":";
        json(o, r.uri);
        o << ",\"bytes\":" << r.bytes.size() << ",\"sha256\":";
        json(o, digest_hex(sha256(r.bytes)));
        o << '}';
    }
    o << "],\"images\":[";
    for (std::size_t i = 0; i < a.images.size(); ++i) {
        if (i)
            o << ',';
        auto &im = a.images[i];
        o << "{\"source\":" << im.source << ",\"name\":";
        json(o, im.name);
        o << ",\"path\":";
        json(o, image_path(a, i));
        o << ",\"mimeType\":";
        json(o, im.mime_type);
        o << ",\"sha256\":";
        json(o, digest_hex(sha256(im.bytes)));
        o << '}';
    }
    o << "],\"samplers\":[";
    for (std::size_t i = 0; i < a.textures.size(); ++i) {
        if (i)
            o << ',';
        auto &s = a.textures[i];
        o << "{\"image\":" << s.image << ",\"wrapS\":" << s.wrap_s << ",\"wrapT\":" << s.wrap_t
          << ",\"minFilter\":" << s.min_filter << ",\"magFilter\":" << s.mag_filter << '}';
    }
    o << "],\"materials\":[";
    for (std::size_t i = 0; i < a.materials.size(); ++i) {
        if (i)
            o << ',';
        auto &m = a.materials[i];
        o << "{\"name\":";
        json(o, m.name);
        o << ",\"baseColor\":[" << m.base_color[0] << ',' << m.base_color[1] << ','
          << m.base_color[2] << ',' << m.base_color[3] << "],\"metallic\":" << m.metallic
          << ",\"roughness\":" << m.roughness << ",\"normalScale\":" << m.normal_scale
          << ",\"emissive\":[" << m.emissive[0] << ',' << m.emissive[1] << ',' << m.emissive[2]
          << "],\"alphaMode\":";
        json(o, m.alpha_mode);
        o << ",\"alphaCutoff\":" << m.alpha_cutoff
          << ",\"doubleSided\":" << (m.double_sided ? "true" : "false");
        const auto optional = [&](std::string_view name, std::optional<std::size_t> value) {
            o << ",\"" << name << "\":";
            if (value)
                o << *value;
            else
                o << "null";
        };
        optional("baseTexture", m.base_texture);
        optional("metallicRoughnessTexture", m.metallic_roughness_texture);
        optional("normalTexture", m.normal_texture);
        optional("emissiveTexture", m.emissive_texture);
        optional("uvSet", m.uv_set);
        o << '}';
    }
    o << "],\"primitives\":[";
    for (std::size_t i = 0; i < a.primitives.size(); ++i) {
        if (i)
            o << ',';
        auto &p = a.primitives[i];
        o << "{\"sourceMesh\":" << p.mesh << ",\"sourcePrimitive\":" << p.primitive
          << ",\"material\":" << p.material << ",\"name\":";
        json(o, p.name);
        o << ",\"mesh\":\"meshes/primitive-" << i
          << ".ply\",\"triangles\":" << p.geometry.triangles.size()
          << ",\"generatedNormals\":" << (p.generated_normals ? "true" : "false")
          << ",\"generatedTangents\":" << (p.generated_tangents ? "true" : "false") << '}';
    }
    o << "],\"placements\":[";
    for (std::size_t i = 0; i < a.placements.size(); ++i) {
        if (i)
            o << ',';
        auto &p = a.placements[i];
        o << "{\"node\":" << p.node << ",\"primitive\":" << p.primitive << ",\"name\":";
        json(o, p.name);
        o << ",\"rowMajorTransform\":[";
        for (std::size_t k = 0; k < 16; ++k) {
            if (k)
                o << ',';
            o << p.transform[k];
        }
        o << "]}";
    }
    o << "],\"files\":[";
    for (std::size_t i = 0; i < c.files.size(); ++i) {
        if (i)
            o << ',';
        auto &f = c.files[i];
        o << "{\"path\":";
        json(o, f.path.generic_string());
        o << ",\"bytes\":" << f.bytes.size() << ",\"sha256\":";
        json(o, digest_hex(sha256(f.bytes)));
        o << '}';
    }
    o << "],\"diagnostics\":[";
    for (std::size_t i = 0; i < c.diagnostics.size(); ++i) {
        if (i)
            o << ',';
        auto &d = c.diagnostics[i];
        o << "{\"code\":";
        json(o, d.code);
        o << ",\"severity\":";
        json(o, d.severity == Severity::error         ? "error"
                : d.severity == Severity::unsupported ? "unsupported"
                                                      : "warning");
        o << ",\"message\":";
        json(o, d.message);
        o << ",\"line\":" << d.location.line << ",\"column\":" << d.location.column << '}';
    }
    o << "]}\n";
    return std::move(t.value);
}
} // namespace
int Conversion::exit_code() const {
    bool unsupported = false;
    for (auto &d : diagnostics) {
        if (d.severity == Severity::error)
            return 1;
        unsupported |= d.severity == Severity::unsupported;
    }
    return unsupported ? 2 : 0;
}
Conversion prepare(const Asset &a, const std::vector<Diagnostic> &diagnostics, const Limits &l) {
    Conversion c;
    try {
        c.diagnostics = diagnostics;
        if (c.exit_code())
            return c;
        validate(a, l);
        if (available(a, c, l) < 4096)
            fail("glTF serialization workspace exhausted");
        auto remaining = std::min(l.output_bytes, l.input.expanded_bytes);
        if (wrapper.size() > remaining)
            fail("validation wrapper exceeds output limit");
        remaining -= wrapper.size();
        const auto add = [&](fs::path path, std::span<const std::uint8_t> data) {
            if (data.size() > remaining || data.size() > l.input.file_bytes)
                fail("glTF output byte limit exceeded");
            if (data.size() > available(a, c, l) / 2)
                fail("glTF output allocation exceeds working limit");
            remaining -= data.size();
            c.files.push_back({std::move(path), {data.begin(), data.end()}});
        };
        const auto prefix = "rypath-gltf-" + digest_hex(sha256(a.bytes)) + "-s" +
                            std::to_string(a.selected_scene) + "-";
        Text model(std::min({remaining, l.input.file_bytes, available(a, c, l) / 3}), "glTF");
        auto &o = model.stream;
        o << "# rypath " << profile
          << "; requires rypath-gltf-v1 extensions\n# Stock PBRT compatibility is not claimed; "
             "provenance: conversion.json\nAttributeBegin\nColorSpace \"srgb\"\n";
        std::set<std::pair<std::size_t, bool>> used;
        const auto texture = [&](std::optional<std::size_t> index, bool color) {
            if (!index)
                return;
            used.insert({*index, color});
        };
        for (auto &m : a.materials) {
            texture(m.base_texture, true);
            texture(m.metallic_roughness_texture, false);
            texture(m.normal_texture, false);
            texture(m.emissive_texture, true);
        }
        const auto texname = [&](std::size_t i, bool color) {
            return prefix + "texture-" + std::to_string(i) + (color ? "-color" : "-data");
        };
        for (auto [i, color] : used) {
            const auto &t = a.textures[i];
            o << "Texture \"" << texname(i, color)
              << "\" \"spectrum\" \"imagemap\" \"string filename\" \"" << image_path(a, t.image)
              << "\" \"string encoding\" \"" << (color ? "sRGB" : "linear")
              << "\" \"bool rypath.gltf\" [true] \"integer rypath.wraps\" [" << t.wrap_s
              << "] \"integer rypath.wrapt\" [" << t.wrap_t << "] \"integer rypath.minfilter\" ["
              << t.min_filter << "] \"integer rypath.magfilter\" [" << t.mag_filter << "]\n";
        }
        for (std::size_t i = 0; i < a.materials.size(); ++i) {
            const auto &m = a.materials[i];
            o << "MakeNamedMaterial \"" << prefix << "material-" << i
              << "\" \"string type\" \"rypath-gltf-v1\" \"rgb basecolor\" [" << m.base_color[0]
              << ' ' << m.base_color[1] << ' ' << m.base_color[2] << "] \"float opacity\" ["
              << m.base_color[3] << "] \"float metallic\" [" << m.metallic
              << "] \"float roughness\" [" << m.roughness << "] \"float normalscale\" ["
              << m.normal_scale << "] \"rgb emissive\" [" << m.emissive[0] << ' ' << m.emissive[1]
              << ' ' << m.emissive[2] << "] \"string alphamode\" \"" << m.alpha_mode
              << "\" \"float alphacutoff\" [" << m.alpha_cutoff << "] \"bool doublesided\" ["
              << (m.double_sided ? "true" : "false") << ']';
            const auto bind = [&](std::string_view field, std::optional<std::size_t> t,
                                  bool color) {
                if (t)
                    o << " \"texture " << field << "\" \"" << texname(*t, color) << '"';
            };
            bind("basecolortexture", m.base_texture, true);
            bind("metallicroughnesstexture", m.metallic_roughness_texture, false);
            bind("normaltexture", m.normal_texture, false);
            bind("emissivetexture", m.emissive_texture, true);
            o << '\n';
        }
        const auto shape = [&](std::size_t i) {
            o << "NamedMaterial \"" << prefix << "material-" << a.primitives[i].material
              << "\"\nShape \"plymesh\" \"string filename\" \"meshes/primitive-" << i << ".ply\"\n";
        };
        for (std::size_t i = 0; i < a.primitives.size(); ++i) {
            auto b = mesh_bundle::ply(
                a.primitives[i].geometry,
                std::min({remaining, l.input.file_bytes, available(a, c, l) / 3}), "glTF", profile);
            add("meshes/primitive-" + std::to_string(i) + ".ply", b);
            if (!a.materials[a.primitives[i].material].emits()) {
                o << "ObjectBegin \"" << prefix << "object-" << i << "\"\n";
                shape(i);
                o << "ObjectEnd\n";
            }
        }
        for (auto &p : a.placements) {
            o << "AttributeBegin\nTransform [";
            for (std::size_t col = 0; col < 4; ++col)
                for (std::size_t row = 0; row < 4; ++row)
                    o << p.transform[4 * row + col] << ' ';
            o << "]\n";
            if (a.materials[a.primitives[p.primitive].material].emits())
                shape(p.primitive);
            else
                o << "ObjectInstance \"" << prefix << "object-" << p.primitive << "\"\n";
            o << "AttributeEnd\n";
        }
        o << "AttributeEnd\n";
        add("model.pbrt", bytes(model.value));
        for (std::size_t i = 0; i < a.images.size(); ++i)
            add(image_path(a, i), a.images[i].bytes);
        add("README.txt", bytes(instructions));
        const auto manifest =
            metadata(a, c, std::min({remaining, l.input.file_bytes, available(a, c, l) / 3}));
        add("conversion.json", bytes(manifest));
    } catch (const std::exception &e) {
        c.files.clear();
        c.diagnostics.push_back(
            {Severity::error, "gltf-conversion", Location{a.entry.string()}, e.what()});
    }
    return c;
}
int publish(const Asset &a, Conversion &c, const fs::path &directory, const Limits &l) {
    if (c.exit_code())
        return c.exit_code();
    try {
        validate(a, l);
        const auto workspace = available(a, c, l);
        if (directory.empty() || directory.filename().empty() || directory.filename() == "." ||
            directory.filename() == "..")
            fail("output must name a new bundle directory");
        const auto dest = fs::absolute(directory).lexically_normal();
        if (fs::exists(fs::symlink_status(dest)))
            fail("output directory already exists");
        if (!fs::is_directory(dest.parent_path()))
            fail("output parent directory does not exist");
        mesh_bundle::Stage stage(dest.parent_path(), "glTF");
        mesh_bundle::Writer writer(stage.path, {l.output_bytes, workspace}, "glTF");
        for (auto &f : c.files)
            writer.write(f.path, f.bytes);
        writer.write(".validate.pbrt", bytes(wrapper));
        const auto checked = pbrt::check(stage.path / ".validate.pbrt", l.input);
        if (checked.exit_code()) {
            c.diagnostics.insert(c.diagnostics.end(), checked.document.diagnostics.begin(),
                                 checked.document.diagnostics.end());
            if (!c.exit_code())
                fail("staged validation failed");
            return c.exit_code();
        }
        const auto translated = pbrt::translate(checked, Location{a.entry.string()}, l.input);
        if (translated.exit_code()) {
            c.diagnostics.insert(c.diagnostics.end(), translated.diagnostics.begin(),
                                 translated.diagnostics.end());
            if (!c.exit_code())
                fail("staged translation failed");
            return c.exit_code();
        }
        fs::remove(stage.path / ".validate.pbrt");
        mesh_bundle::publish_directory(stage.path, dest, "glTF");
        stage.path.clear();
        return 0;
    } catch (const std::exception &e) {
        c.diagnostics.push_back(
            {Severity::error, "gltf-publication", Location{a.entry.string()}, e.what()});
        return 1;
    }
}
} // namespace rypath::gltf
