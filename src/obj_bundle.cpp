// SPDX-License-Identifier: GPL-3.0-only
#include "mesh_bundle.hpp"
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

using mesh_bundle::bytes;
using mesh_bundle::json_string;
using mesh_bundle::ply;
using mesh_bundle::publish_directory;
using mesh_bundle::Stage;
using mesh_bundle::Text;
using mesh_bundle::Writer;
using mesh_bundle::Written;

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
