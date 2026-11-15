// SPDX-License-Identifier: GPL-3.0-only
#include "mesh_bundle.hpp"
#include "pbrt_render.hpp"
#include "stl.hpp"

#include <algorithm>
#include <cmath>
#include <set>

namespace rypath::stl {
namespace {
namespace fs = std::filesystem;
using mesh_bundle::bytes;
using mesh_bundle::Text;
constexpr std::string_view wrapper =
    "Camera \"perspective\"\nFilm \"rgb\" \"integer xresolution\" [1] "
    "\"integer yresolution\" [1] \"string sensor\" \"cie1931\"\n"
    "PixelFilter \"box\"\nSampler \"independent\" \"integer pixelsamples\" [1]\n"
    "Integrator \"path\" \"integer maxdepth\" [1]\nWorldBegin\n"
    "LightSource \"infinite\"\nInclude \"model.pbrt\"\n";
constexpr std::string_view instructions =
    "rypath STL asset bundle\n\n"
    "model.pbrt is a world fragment. Place your enclosing PBRT scene at this bundle\n"
    "root and Include \"model.pbrt\" after WorldBegin. PBRT resolves asset paths\n"
    "from the enclosing scene directory. Move the entire directory together.\n"
    "conversion.json records source hashes, raw facet normals and attributes, solid\n"
    "names, source facet mappings, and explicit color interpretation. Normals are\n"
    "flat geometric normals; stored source normals are provenance only. Colors are\n"
    "linear sRGB diffuse reflectance; alpha is stochastic surface coverage, without\n"
    "premultiplication. Source assets retain their original terms.\n";
void json(std::ostream &out, std::string_view value) {
    mesh_bundle::json_string(out, value, "STL");
}
void location(std::ostream &out, const Location &where, const Asset &asset) {
    out << "{\"file\":";
    json(out, fs::path(*where.file).lexically_relative(asset.entry.parent_path()).generic_string());
    out << ",\"line\":" << where.line << ",\"column\":" << where.column << '}';
}
// Generated PBRT uses no escaped strings. Count raw token bytes, including quotes,
// just as the parser does; numeric spellings are inspected after serialization.
std::size_t longest_token(std::string_view source) {
    std::size_t longest = 0;
    for (std::size_t i = 0; i < source.size();) {
        if (source[i] == '#') {
            while (i < source.size() && source[i] != '\n')
                ++i;
        } else if (source[i] == ' ' || source[i] == '\n' || source[i] == '\r' ||
                   source[i] == '\t') {
            ++i;
        } else {
            const auto start = i;
            if (source[i] == '"') {
                ++i;
                while (i < source.size() && source[i] != '"')
                    ++i;
                if (i < source.size())
                    ++i;
            } else if (source[i] == '[' || source[i] == ']') {
                ++i;
            } else {
                while (i < source.size() && source[i] != ' ' && source[i] != '\n' &&
                       source[i] != '\r' && source[i] != '\t' && source[i] != '[' &&
                       source[i] != ']' && source[i] != '"')
                    ++i;
            }
            longest = std::max(longest, i - start);
        }
    }
    return longest;
}
void fail(const char *message) { throw std::runtime_error(message); }
std::size_t conversion_bytes(const Conversion &conversion) {
    std::size_t total = sizeof(Conversion) + conversion.files.capacity() * sizeof(BundleFile) +
                        conversion.diagnostics.capacity() * sizeof(Diagnostic);
    const auto add = [&](std::size_t n) {
        if (n > std::numeric_limits<std::size_t>::max() - total)
            fail("STL retained conversion size overflow");
        total += n;
    };
    for (const auto &file : conversion.files) {
        add(file.bytes.capacity() + 64);
        add(file.path.native().capacity() * sizeof(fs::path::value_type) + 65);
    }
    for (const auto &d : conversion.diagnostics) {
        add(d.code.capacity() + d.message.capacity() + 130);
        if (d.location.file)
            add(d.location.file->capacity() + sizeof(std::string) + 65);
    }
    return total;
}
std::size_t scratch_budget(const Asset &asset, const Conversion &conversion, const Limits &limits) {
    const auto retained = retained_bytes(asset), converted = conversion_bytes(conversion);
    if (retained > limits.working_bytes || converted > limits.working_bytes - retained)
        fail("STL retained inputs exceed working byte limit");
    const auto scratch = (limits.working_bytes - retained - converted) / 16;
    if (scratch < 4096)
        fail("STL working byte limit has no serialization workspace");
    return scratch;
}
void validate(const Asset &asset, const Limits &limits, std::size_t scratch) {
    if (asset.parts.empty() || asset.colors.empty() || asset.facets.empty() || asset.solids.empty())
        fail("STL bundle requires nonempty geometry, solids and palette");
    const auto available = limits.input.expansions > 2 ? limits.input.expansions - 2 : 0;
    if (asset.parts.size() > available)
        throw std::runtime_error("STL bundle requires " + std::to_string(asset.parts.size()) +
                                 " PLY file expansions; available " + std::to_string(available) +
                                 " after validation wrapper and model");
    if (limits.input.active_files < 2 || limits.input.scopes < 2 || limits.input.list_values < 3)
        fail("STL generated scene exceeds PBRT nesting or list limit");
    // Wrapper: eight directives and twelve values. Fragment: outer scope and ColorSpace,
    // one material definition per palette entry, four directives per part, closing scope.
    if (asset.colors.size() >
            (limits.input.directives - std::min<std::size_t>(11, limits.input.directives)) ||
        asset.parts.size() >
            (limits.input.directives -
             std::min<std::size_t>(11 + asset.colors.size(), limits.input.directives)) /
                4)
        fail("STL generated scene exceeds PBRT directive limit");
    if (asset.facets.size() > limits.input.total_values / 13)
        fail("STL PLY decoded value limit exceeded (13 values per facet)");
    if (asset.facets.size() > limits.input.total_values / 12)
        fail("STL translated geometry value limit exceeded");
    if (asset.facets.size() > scratch / (2 * sizeof(std::size_t)))
        fail("STL facet mapping validation exceeds working byte limit");
    std::vector<std::size_t> seen(asset.facets.size(), 0);
    std::set<std::size_t> materials;
    std::size_t solid_end = 0;
    for (std::size_t i = 0; i < asset.solids.size(); ++i) {
        const auto &solid = asset.solids[i];
        if (solid.first_facet != solid_end || solid.facet_count > asset.facets.size() - solid_end)
            fail("STL solid facet ranges are inconsistent");
        for (std::size_t j = 0; j < solid.facet_count; ++j)
            if (asset.facets[solid_end + j].solid != i)
                fail("STL facet solid binding is inconsistent");
        solid_end += solid.facet_count;
    }
    if (solid_end != asset.facets.size())
        fail("STL solid ranges do not cover all facets");
    for (const auto &color : asset.colors) {
        for (auto v : color.linear_rgb)
            if (!std::isfinite(v) || v < 0 || v > 1)
                fail("STL palette has invalid linear sRGB reflectance");
        if (!std::isfinite(color.coverage) || color.coverage < 0 || color.coverage > 1)
            fail("STL palette has invalid coverage");
    }
    std::size_t parser_values = 13 + asset.colors.size() * 5;
    for (const auto &part : asset.parts) {
        if (part.material >= asset.colors.size() || !materials.insert(part.material).second)
            fail("STL parts require one global group per palette material");
        const auto &mesh = part.geometry;
        if (mesh.triangles.empty() || mesh.positions.size() != mesh.triangles.size() * 3 ||
            part.source_facets.size() != mesh.triangles.size() || !mesh.normals.empty() ||
            !mesh.uvs.empty() || !mesh.tangents.empty() || !mesh.patches.empty())
            fail("STL part requires flat triangle geometry and complete source mappings");
        if (mesh.positions.size() > (4 * scratch) / sizeof(GeometryPoint) ||
            mesh.triangles.size() > (4 * scratch - mesh.positions.size() * sizeof(GeometryPoint)) /
                                        sizeof(std::array<std::uint32_t, 3>))
            fail("STL geometry validation exceeds working byte limit");
        {
            GeometryData checked;
            checked.meshes.push_back(mesh);
            const auto error = validate_geometry(checked);
            if (!error.empty())
                throw std::runtime_error("STL checked geometry: " + error);
        }
        if (const auto error = validate_area_geometry(mesh, {}); !error.empty())
            throw std::runtime_error("STL native triangle rendering range: " + error);
        for (auto p : mesh.positions)
            if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z))
                fail("STL geometry has nonfinite positions");
        for (std::size_t j = 0; j < mesh.triangles.size(); ++j) {
            const auto &triangle = mesh.triangles[j];
            if (triangle != std::array<std::uint32_t, 3>{static_cast<std::uint32_t>(3 * j),
                                                         static_cast<std::uint32_t>(3 * j + 1),
                                                         static_cast<std::uint32_t>(3 * j + 2)})
                fail("STL geometry is not an ordered triangle soup");
            const auto source = part.source_facets[j];
            if (source >= asset.facets.size() || seen[source]++ ||
                asset.facets[source].material != part.material)
                fail("STL facet-to-part mapping is inconsistent");
        }
        parser_values += 3 + (asset.colors[part.material].coverage != 1 ? 1 : 0);
    }
    if (parser_values > limits.input.total_values)
        fail("STL generated PBRT value limit exceeded");
    if (std::find(seen.begin(), seen.end(), 0) != seen.end())
        fail("STL part mapping omits source facets");
    for (const auto &facet : asset.facets)
        for (auto n : facet.authored_normal)
            if (!std::isfinite(n))
                fail("STL provenance has nonfinite authored normals");
}
std::string metadata(const Asset &asset, const Conversion &conversion, std::size_t limit) {
    Text text(limit, "STL");
    auto &out = text.stream;
    out << "{\"schema\":1,\"profile\":\"" << profile << "\",\"materials\":\"" << material_profile
        << "\",\"entry\":\"model.pbrt\",\"pathBase\":\"bundle-root\",\"source\":{\"path\":";
    json(out, asset.entry.filename().generic_string());
    out << ",\"sha256\":";
    json(out, digest_hex(sha256(asset.bytes)));
    out << ",\"bytes\":" << asset.bytes.size() << ",\"encoding\":";
    json(out, asset.encoding == Encoding::binary ? "binary" : "ascii");
    if (asset.encoding == Encoding::binary) {
        if (asset.bytes.size() < 84)
            fail("STL binary provenance requires a complete header");
        out << ",\"headerHex\":\"";
        constexpr char hex[] = "0123456789abcdef";
        for (std::size_t i = 0; i < 80; ++i)
            out << hex[asset.bytes[i] >> 4U] << hex[asset.bytes[i] & 15U];
        out << '"';
    }
    out << "},\"requestedColors\":";
    json(out, color_mode_name(asset.requested_colors));
    out << ",\"effectiveColors\":";
    json(out, color_mode_name(asset.effective_colors));
    out << ",\"headerRGBA\":";
    if (asset.header_color) {
        out << '[';
        for (std::size_t i = 0; i < 4; ++i) {
            if (i)
                out << ',';
            out << unsigned((*asset.header_color)[i]);
        }
        out << ']';
    } else
        out << "null";
    out << ",\"palette\":[";
    for (std::size_t i = 0; i < asset.colors.size(); ++i) {
        if (i)
            out << ',';
        const auto &c = asset.colors[i];
        out << "{\"linearRGB\":[" << c.linear_rgb[0] << ',' << c.linear_rgb[1] << ','
            << c.linear_rgb[2] << "],\"coverage\":" << c.coverage << '}';
    }
    out << "],\"files\":[";
    for (std::size_t i = 0; i < conversion.files.size(); ++i) {
        if (i)
            out << ',';
        const auto &f = conversion.files[i];
        out << "{\"path\":";
        json(out, f.path.generic_string());
        out << ",\"sha256\":";
        json(out, digest_hex(sha256(f.bytes)));
        out << ",\"bytes\":" << f.bytes.size() << '}';
    }
    out << "],\"solids\":[";
    for (std::size_t i = 0; i < asset.solids.size(); ++i) {
        if (i)
            out << ',';
        const auto &s = asset.solids[i];
        out << "{\"name\":";
        json(out, s.name);
        out << ",\"firstFacet\":" << s.first_facet << ",\"facetCount\":" << s.facet_count
            << ",\"location\":";
        location(out, s.location, asset);
        out << '}';
    }
    out << "],\"facets\":[";
    for (std::size_t i = 0; i < asset.facets.size(); ++i) {
        if (i)
            out << ',';
        const auto &f = asset.facets[i];
        out << "{\"authoredNormal\":[" << f.authored_normal[0] << ',' << f.authored_normal[1] << ','
            << f.authored_normal[2] << "],\"attribute\":" << f.attribute << ",\"solid\":" << f.solid
            << ",\"material\":" << f.material
            << ",\"inheritedColor\":" << (f.inherited_color ? "true" : "false")
            << ",\"byteOffset\":" << f.byte_offset << ",\"location\":";
        location(out, f.location, asset);
        out << '}';
    }
    out << "],\"parts\":[";
    for (std::size_t i = 0; i < asset.parts.size(); ++i) {
        if (i)
            out << ',';
        out << "{\"mesh\":\"meshes/part-" << i << ".ply\",\"material\":" << asset.parts[i].material
            << ",\"sourceFacets\":[";
        for (std::size_t j = 0; j < asset.parts[i].source_facets.size(); ++j) {
            if (j)
                out << ',';
            out << asset.parts[i].source_facets[j];
        }
        out << "]}";
    }
    out << "],\"diagnostics\":[";
    for (std::size_t i = 0; i < conversion.diagnostics.size(); ++i) {
        if (i)
            out << ',';
        const auto &d = conversion.diagnostics[i];
        out << "{\"code\":";
        json(out, d.code);
        out << ",\"severity\":";
        json(out, d.severity == Severity::error         ? "error"
                  : d.severity == Severity::unsupported ? "unsupported"
                                                        : "warning");
        out << ",\"location\":";
        location(out, d.location, asset);
        out << ",\"message\":";
        json(out, d.message);
        out << '}';
    }
    out << "]}\n";
    return std::move(text.value);
}
} // namespace

int Conversion::exit_code() const {
    bool unsupported = false;
    for (const auto &d : diagnostics) {
        if (d.severity == Severity::error)
            return 1;
        unsupported |= d.severity == Severity::unsupported;
    }
    return unsupported ? 2 : 0;
}
Conversion prepare(const Asset &asset, const std::vector<Diagnostic> &diagnostics,
                   const Limits &limits) {
    Conversion result;
    try {
        // Charge borrowed diagnostic storage before copying it into retained conversion state.
        std::size_t diagnostic_bytes = diagnostics.size() * sizeof(Diagnostic);
        for (const auto &d : diagnostics)
            diagnostic_bytes += d.code.capacity() + d.message.capacity() +
                                (d.location.file ? d.location.file->capacity() : 0) + 256;
        if (retained_bytes(asset) > limits.working_bytes ||
            diagnostic_bytes > limits.working_bytes - retained_bytes(asset))
            fail("STL diagnostics exceed working byte limit");
        result.diagnostics = diagnostics;
        if (result.exit_code())
            return result;
        const auto scratch = scratch_budget(asset, result, limits);
        validate(asset, limits, scratch);
        if (limits.input.token_bytes < 20 || limits.input.directives < 10)
            fail("STL generated PBRT or PLY token/header limit exceeded");
        const auto file_limit = std::min<std::size_t>(
            {limits.input.file_bytes, std::size_t{16} * 1024 * 1024, 4 * scratch});
        auto remaining = std::min<std::size_t>(
            {limits.output_bytes, limits.input.expanded_bytes, std::size_t{64} * 1024 * 1024});
        // Validation wrapper participates in expanded-byte limits, but is never published.
        if (wrapper.size() > remaining)
            fail("STL validation wrapper exceeds expanded byte limit");
        remaining -= wrapper.size();
        if (wrapper.size() > file_limit)
            fail("STL validation wrapper exceeds file byte limit");
        const auto add = [&](fs::path path, std::span<const std::uint8_t> data) {
            if (data.size() > file_limit)
                fail("STL generated file byte limit exceeded");
            if (data.size() > remaining)
                fail("STL aggregate output byte limit exceeded");
            remaining -= data.size();
            if (data.size() > scratch_budget(asset, result, limits) * 4)
                fail("STL output allocation exceeds working byte limit");
            result.files.push_back({std::move(path), {data.begin(), data.end()}});
            (void)scratch_budget(asset, result, limits);
        };
        Text identity(scratch, "STL");
        identity.stream << digest_hex(sha256(asset.bytes)) << ':'
                        << color_mode_name(asset.effective_colors);
        for (const auto &c : asset.colors)
            identity.stream << ':' << c.linear_rgb[0] << ':' << c.linear_rgb[1] << ':'
                            << c.linear_rgb[2] << ':' << c.coverage;
        const auto prefix = "rypath-stl-" + digest_hex(sha256(bytes(identity.value))) + "-";
        Text model(std::min(file_limit, remaining), "STL");
        model.stream << "# rypath " << profile << " / " << material_profile
                     << "\n# Conversion assumptions and warnings: "
                        "conversion.json\nAttributeBegin\nColorSpace \"srgb\"\n";
        for (std::size_t i = 0; i < asset.colors.size(); ++i) {
            const auto &c = asset.colors[i];
            model.stream << "MakeNamedMaterial \"" << prefix << i
                         << "\" \"string type\" \"diffuse\" \"rgb reflectance\" ["
                         << c.linear_rgb[0] << ' ' << c.linear_rgb[1] << ' ' << c.linear_rgb[2]
                         << "]\n";
        }
        for (std::size_t i = 0; i < asset.parts.size(); ++i) {
            const auto path = "meshes/part-" + std::to_string(i) + ".ply";
            const auto data = mesh_bundle::ply(asset.parts[i].geometry,
                                               std::min(file_limit, remaining), "STL", profile);
            add(path, data);
            model.stream << "AttributeBegin\nNamedMaterial \"" << prefix << asset.parts[i].material
                         << "\"\nShape \"plymesh\" \"string filename\" \"" << path << '"';
            const auto coverage = asset.colors[asset.parts[i].material].coverage;
            if (coverage != 1)
                model.stream << " \"float alpha\" [" << coverage << ']';
            model.stream << "\nAttributeEnd\n";
        }
        model.stream << "AttributeEnd\n";
        const auto token_bytes = std::max(longest_token(model.value), longest_token(wrapper));
        if (token_bytes > limits.input.token_bytes)
            throw std::runtime_error("STL generated PBRT token requires " +
                                     std::to_string(token_bytes) + " bytes; available " +
                                     std::to_string(limits.input.token_bytes));
        add("model.pbrt", bytes(model.value));
        add("README.txt", bytes(instructions));
        const auto manifest = metadata(asset, result, std::min({file_limit, remaining, scratch}));
        add("conversion.json", bytes(manifest));
    } catch (const std::exception &error) {
        result.files.clear();
        result.diagnostics.push_back({Severity::error, "stl-conversion",
                                      Location{asset.entry.string(), 1, 1}, error.what()});
    }
    return result;
}
int publish(const Asset &asset, Conversion &conversion, const fs::path &directory,
            const Limits &limits) {
    if (conversion.exit_code())
        return conversion.exit_code();
    try {
        const auto scratch = scratch_budget(asset, conversion, limits);
        if (directory.empty() || directory.filename().empty() || directory.filename() == "." ||
            directory.filename() == "..")
            fail("STL output must name a new bundle directory");
        const auto destination = fs::absolute(directory).lexically_normal();
        if (fs::exists(fs::symlink_status(destination)))
            fail("STL output directory already exists");
        if (!fs::is_directory(destination.parent_path()))
            fail("STL output parent directory does not exist");
        mesh_bundle::Stage stage(destination.parent_path(), "STL");
        mesh_bundle::Writer writer(
            stage.path, {.output = limits.output_bytes, .workspace = 4 * scratch}, "STL");
        for (const auto &f : conversion.files)
            writer.write(f.path, f.bytes);
        writer.write(".validate.pbrt", bytes(wrapper));
        // Core PBRT parsing/translation uses its inherited budgets independently of STL-owned
        // workspace.
        const auto checked = pbrt::check(stage.path / ".validate.pbrt", limits.input);
        if (checked.exit_code()) {
            conversion.diagnostics.insert(conversion.diagnostics.end(),
                                          checked.document.diagnostics.begin(),
                                          checked.document.diagnostics.end());
            if (!conversion.exit_code())
                fail("STL staged PBRT validation failed");
            return conversion.exit_code();
        }
        const auto translated =
            pbrt::translate(checked, Location{asset.entry.string(), 1, 1}, limits.input);
        if (translated.exit_code()) {
            conversion.diagnostics.insert(conversion.diagnostics.end(),
                                          translated.diagnostics.begin(),
                                          translated.diagnostics.end());
            if (!conversion.exit_code())
                fail("STL staged PBRT translation failed");
            return conversion.exit_code();
        }
        fs::remove(stage.path / ".validate.pbrt");
        mesh_bundle::publish_directory(stage.path, destination, "STL");
        stage.path.clear();
        return 0;
    } catch (const std::exception &error) {
        conversion.diagnostics.push_back({Severity::error, "stl-publication",
                                          Location{asset.entry.string(), 1, 1}, error.what()});
        return 1;
    }
}
} // namespace rypath::stl
