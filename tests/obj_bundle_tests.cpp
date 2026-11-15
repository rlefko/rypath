// SPDX-License-Identifier: GPL-3.0-only
#include "obj.hpp"
#include "pbrt_render.hpp"
#include "sha256.hpp"

#include <bit>
#include <cmath>
#include <fstream>
#include <iostream>
#include <map>
#include <regex>
#include <stdexcept>
#include <string_view>
#include <unistd.h>

namespace {
namespace fs = std::filesystem;
using namespace rypath;
void require(bool condition, std::string_view why) {
    if (!condition)
        throw std::runtime_error(std::string(why));
}
struct Temporary {
    fs::path path;
    Temporary() {
        auto pattern = (fs::temp_directory_path() / "rypath-obj-bundle-XXXXXX").string();
        require(mkdtemp(pattern.data()) != nullptr, "temporary directory");
        path = pattern;
    }
    ~Temporary() {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }
};
void write(const fs::path &path, std::string_view value) {
    std::ofstream out(path, std::ios::binary);
    out << value;
    require(out.good(), "fixture write");
}
std::vector<std::uint8_t> read(const fs::path &path) {
    std::ifstream in(path, std::ios::binary);
    require(in.good(), "read output");
    return {std::istreambuf_iterator<char>(in), {}};
}
std::string text(const fs::path &path) {
    auto bytes = read(path);
    return {bytes.begin(), bytes.end()};
}
std::map<std::string, std::vector<std::uint8_t>> tree(const fs::path &path) {
    std::map<std::string, std::vector<std::uint8_t>> result;
    for (const auto &entry : fs::recursive_directory_iterator(path))
        if (entry.is_regular_file())
            result.emplace(entry.path().lexically_relative(path).generic_string(),
                           read(entry.path()));
    return result;
}
void no_stage(const fs::path &parent) {
    for (const auto &entry : fs::directory_iterator(parent))
        require(!entry.path().filename().string().starts_with(".rypath-obj-"),
                "publication leaked staging directory");
}
obj::Asset load(const fs::path &entry) {
    auto result = obj::load(entry);
    if (result.exit_code() != 0 || !result.asset)
        throw std::runtime_error(obj::report(result));
    return std::move(result.asset.value());
}
obj::Conversion prepare(const obj::Asset &asset) {
    auto result = obj::prepare_materials(asset);
    if (result.exit_code()) {
        for (const auto &d : result.diagnostics)
            std::cerr << d.code << ": " << d.message << '\n';
        throw std::runtime_error("prepare fixture material");
    }
    return result;
}
void publish_ok(const obj::Asset &asset, obj::Conversion &conversion, const fs::path &path) {
    if (obj::publish(asset, conversion, path)) {
        for (const auto &d : conversion.diagnostics)
            std::cerr << d.code << ": " << d.message << '\n';
        throw std::runtime_error("publish fixture bundle");
    }
}
// Independent decoder for the precise binary PLY schema promised by the exporter.
void verify_ply(const fs::path &path, const GeometryMesh &mesh) {
    const auto bytes = read(path);
    const std::string content(bytes.begin(), bytes.end());
    const auto end = content.find("end_header\n");
    require(end != std::string::npos, "PLY header terminator");
    std::size_t offset = end + 11;
    const auto header = content.substr(0, offset);
    require(header.starts_with("ply\nformat binary_little_endian 1.0\n"), "PLY encoding");
    require(header.find("element vertex " + std::to_string(mesh.positions.size()) + "\n") !=
                std::string::npos,
            "PLY vertex count");
    require(header.find("element face " + std::to_string(mesh.triangles.size()) + "\n") !=
                std::string::npos,
            "PLY face count");
    const auto integer = [&]() {
        require(offset + 4 <= bytes.size(), "truncated PLY scalar");
        std::uint32_t n = 0;
        for (unsigned b = 0; b < 4; ++b)
            n |= std::uint32_t(bytes[offset++]) << (8U * b);
        return n;
    };
    const auto real = [&](float expected) {
        require(std::bit_cast<float>(integer()) == expected, "PLY attribute differs");
    };
    const auto point = [&](GeometryPoint p) {
        real(p.x);
        real(p.y);
        real(p.z);
    };
    for (std::size_t i = 0; i < mesh.positions.size(); ++i) {
        point(mesh.positions[i]);
        if (!mesh.normals.empty())
            point(mesh.normals[i]);
        if (!mesh.uvs.empty()) {
            real(mesh.uvs[i][0]);
            real(mesh.uvs[i][1]);
        }
    }
    for (const auto &triangle : mesh.triangles) {
        require(offset < bytes.size(), "PLY triangle arity byte missing");
        require(bytes[offset] == 3, "PLY triangle arity");
        ++offset;
        for (auto index : triangle)
            require(integer() == index, "PLY corner order/index differs");
    }
    require(offset == bytes.size(), "PLY trailing or missing payload");
}
void verify_manifest(const fs::path &bundle) {
    const auto manifest = text(bundle / "conversion.json");
    // Parse only the unescaped generated safe output paths, independently of publisher internals.
    const std::regex record(
        R"re(\{"path":"([^"\\]+)","sha256":"([0-9a-f]{64})","bytes":([0-9]+)\})re");
    const auto begin = manifest.find("\"files\":[");
    const auto end = manifest.find("],\"authoredMaterials\"", begin);
    require(begin != std::string::npos && end != std::string::npos, "manifest files inventory");
    const auto files = manifest.substr(begin, end - begin);
    std::size_t count = 0;
    for (std::sregex_iterator it(files.begin(), files.end(), record), last; it != last; ++it) {
        const auto payload = read(bundle / (*it)[1].str());
        require(digest_hex(sha256(payload)) == (*it)[2].str(), "manifest SHA-256 mismatch");
        require(payload.size() == std::stoull((*it)[3].str()), "manifest byte size mismatch");
        ++count;
    }
    require(count + 1 == tree(bundle).size(), "manifest must inventory every output except itself");
    require(manifest.find("\"pathBase\":\"bundle-root\"") != std::string::npos,
            "manifest path base");
    require(!fs::exists(bundle / ".validate.pbrt"), "temporary validation wrapper published");
}
constexpr std::string_view geometry = "v -1 -1 3\nv 1 -1 3\nv 1 1 3\nv -1 1 3\n"
                                      "vt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\nvn 0 0 -1\n";
constexpr std::string_view wrapper =
    "Camera \"perspective\"\nFilm \"rgb\" \"integer xresolution\" [4] \"integer yresolution\" [4] "
    "\"string sensor\" \"cie1931\"\n"
    "PixelFilter \"box\"\nSampler \"independent\" \"integer pixelsamples\" [1]\nIntegrator "
    "\"path\"\nWorldBegin\nLightSource \"infinite\"\nInclude \"model.pbrt\"\n";
void round_trip(const fs::path &root) {
    const auto source = root / "source";
    fs::create_directory(source);
    write(source / "model.obj", std::string("mtllib model.mtl\n") + std::string(geometry) +
                                    "o panel\ng front primary\ns 1\nusemtl red\nf 1/1/1 2/2/1 "
                                    "3/3/1\nusemtl blue\nf 1/1/1 3/3/1 4/4/1\n");
    write(source / "model.mtl", "newmtl red\nKd .8 .1 .1\nnewmtl blue\nKd .1 .1 .8\n");
    auto asset = load(source / "model.obj");
    require(asset.parts.size() == 2, "material boundary preserved");
    auto conversion = prepare(asset);
    publish_ok(asset, conversion, root / "one");
    auto second = prepare(asset);
    publish_ok(asset, second, root / "two");
    require(tree(root / "one") == tree(root / "two"),
            "identical imports have identical output bytes");
    for (std::size_t i = 0; i < asset.parts.size(); ++i)
        verify_ply(root / "one" / "meshes" / ("part-" + std::to_string(i) + ".ply"),
                   asset.parts[i].geometry);
    verify_manifest(root / "one");
    const auto metadata = text(root / "one" / "conversion.json");
    require(metadata.find("\"location\":{\"file\":\"model.mtl\"") != std::string::npos &&
                metadata.find("\"severity\":\"warning\"") != std::string::npos,
            "manifest material/diagnostic provenance must identify source file and severity");
    obj::Limits tiny_work;
    tiny_work.working_bytes = 1;
    auto constrained = conversion;
    require(obj::publish(asset, constrained, root / "working-limit", tiny_work) == 1 &&
                !fs::exists(root / "working-limit"),
            "publication must account retained working storage");
    auto metadata_heavy = asset;
    metadata_heavy.polygons[0].groups.assign(4, std::string(std::size_t{256} * 1024, 'g'));
    tiny_work.working_bytes = std::size_t{1024} * 1024;
    require(obj::retained_bytes(metadata_heavy) > tiny_work.working_bytes &&
                obj::prepare_materials(metadata_heavy, tiny_work).exit_code() == 1,
            "material preparation must account retained object/group strings");
    for (const auto &snapshot : asset.sources)
        require(metadata.find(digest_hex(sha256(snapshot.bytes))) != std::string::npos,
                "source snapshot hash missing from manifest");
    for (std::size_t i = 0; i < asset.parts.size(); ++i) {
        const auto &part = asset.parts[i];
        require(part.triangle_polygons.size() == part.geometry.triangles.size(),
                "every triangle needs a source polygon mapping");
        std::string expected =
            "{\"mesh\":\"meshes/part-" + std::to_string(i) + ".ply\",\"trianglePolygons\":[";
        for (std::size_t j = 0; j < part.triangle_polygons.size(); ++j) {
            if (j)
                expected += ',';
            expected += std::to_string(part.triangle_polygons[j]);
        }
        expected += "]}";
        require(metadata.find(expected) != std::string::npos,
                "manifest triangle provenance differs");
    }
    fs::rename(source, root / "relocated-source");
    auto relocated = load(root / "relocated-source" / "model.obj");
    auto moved = prepare(relocated);
    publish_ok(relocated, moved, root / "three");
    require(tree(root / "one") == tree(root / "three"),
            "source location must not change bundle hashes");
    fs::remove_all(root / "relocated-source");
    fs::rename(root / "one", root / "relocated-bundle");
    write(root / "relocated-bundle" / "scene.pbrt", wrapper);
    const auto checked = pbrt::check(root / "relocated-bundle" / "scene.pbrt");
    require(checked.exit_code() == 0, "relocated bundle structural check");
    const auto translated = pbrt::translate(
        checked, pbrt::Location{(root / "relocated-bundle" / "scene.pbrt").string()});
    require(translated.exit_code() == 0 && translated.scene.has_value(),
            "relocated bundle render translation");
    auto existing = prepare(asset);
    write(root / "sentinel", "unchanged");
    require(obj::publish(asset, existing, root / "sentinel") != 0, "existing file must fail");
    require(text(root / "sentinel") == "unchanged", "existing file changed");
    fs::create_directory(root / "existing");
    write(root / "existing" / "keep", "keep");
    existing = prepare(asset);
    require(obj::publish(asset, existing, root / "existing") != 0, "existing directory must fail");
    require(text(root / "existing" / "keep") == "keep", "existing directory changed");
    fs::create_symlink(root / "missing-target", root / "link");
    existing = prepare(asset);
    require(obj::publish(asset, existing, root / "link") != 0, "dangling symlink must fail");
    require(fs::is_symlink(root / "link") &&
                fs::read_symlink(root / "link") == root / "missing-target",
            "symlink replaced");
    auto limited = prepare(asset);
    obj::Limits limits;
    limits.output_bytes = 50;
    require(obj::publish(asset, limited, root / "limited", limits) != 0 &&
                !fs::exists(root / "limited"),
            "bounded output failure must be atomic");
    auto invalid = asset;
    auto invalid_conversion = prepare(invalid);
    invalid.parts[0].geometry.triangles[0][0] = UINT32_MAX;
    require(obj::publish(invalid, invalid_conversion, root / "invalid") != 0 &&
                !fs::exists(root / "invalid"),
            "bad mesh failure must be atomic");
    auto bad_material = prepare(asset);
    bad_material.definitions = "MakeNamedMaterial \"broken\" \"string type\" \"unsupported\"\n";
    require(obj::publish(asset, bad_material, root / "bad-material") != 0 &&
                !fs::exists(root / "bad-material"),
            "staged PBRT failure must be atomic");
    auto unsafe_file = prepare(asset);
    unsafe_file.files.push_back({"../escape", {1, 2, 3}});
    require(obj::publish(asset, unsafe_file, root / "unsafe") != 0 &&
                !fs::exists(root / "unsafe") && !fs::exists(root / "escape"),
            "unsafe output path must fail atomically");
    no_stage(root);
}
void materials_and_snapshots(const fs::path &root) {
    const auto source = root / "material-source";
    fs::create_directory(source);
    // Existing owned JPEG fixture; its provenance remains in tests/fixtures/image/README.md.
    fs::copy_file(fs::path(RYPATH_SOURCE_DIR) / "tests/fixtures/image/baseline-rgb.jpg",
                  source / "map.jpg");
    write(source / "mapped.obj", std::string("mtllib mapped.mtl\n") + std::string(geometry) +
                                     "usemtl mapped\nf 1/1/1 2/2/1 3/3/1\n");
    write(source / "mapped.mtl", "newmtl mapped\nKd .5 .5 .5\nmap_Kd map.jpg\n");
    auto asset = load(source / "mapped.obj");
    auto conversion = prepare(asset);
    const auto original = read(source / "map.jpg");
    write(source / "map.jpg", "changed after preparation");
    write(source / "mapped.obj", "changed after load");
    write(source / "mapped.mtl", "changed after load");
    publish_ok(asset, conversion, root / "snapshot");
    bool found = false;
    for (const auto &[name, bytes] : tree(root / "snapshot")) {
        static_cast<void>(name);
        if (bytes == original)
            found = true;
    }
    require(found, "bundle must retain original image snapshot, not reread source");
    verify_manifest(root / "snapshot");
    auto missing_uvs = asset;
    missing_uvs.parts[0].complete_uvs = false;
    auto reused = conversion;
    require(obj::publish(missing_uvs, reused, root / "missing-uvs") != 0 &&
                !fs::exists(root / "missing-uvs"),
            "mapped material without complete UV must fail atomically");
    for (const auto opacity : {"0", "0.5"}) {
        write(source / "emitter.mtl", std::string("newmtl lamp\nKe 1 1 1\nd ") + opacity + "\n");
        write(source / "emitter.obj", std::string("mtllib emitter.mtl\n") + std::string(geometry) +
                                          "usemtl lamp\nf 1 2 3\n");
        auto emitter = load(source / "emitter.obj");
        require(obj::prepare_materials(emitter).exit_code() != 0,
                "nonopaque emission must be rejected");
    }
    write(source / "default.obj", std::string(geometry) + "f 1 2 3\n");
    auto plain = load(source / "default.obj");
    auto plain_conversion = prepare(plain);
    publish_ok(plain, plain_conversion, root / "default");
    write(source / "bad.mtl", "newmtl invalid\nKd nonsense 0 0\n");
    write(source / "bad.obj",
          std::string("mtllib bad.mtl\n") + std::string(geometry) + "usemtl invalid\nf 1 2 3\n");
    auto malformed = obj::load(source / "bad.obj");
    require(malformed.exit_code() != 0 ||
                (malformed.asset && obj::prepare_materials(*malformed.asset).exit_code() != 0),
            "malformed material must fail");
    // Authored metadata needs JSON escaping without becoming an output pathname.
    auto escaped = plain;
    escaped.sources[0].path = source / "quote\"slash\\line\n.obj";
    escaped.polygons[0].object = "quote\"slash\\line\n";
    auto escaped_conversion = prepare(escaped);
    publish_ok(escaped, escaped_conversion, root / "escaped");
    const auto metadata = text(root / "escaped" / "conversion.json");
    require(metadata.find("quote\\\"slash\\\\line\\u000a") != std::string::npos,
            "authored JSON metadata escaping");
    verify_manifest(root / "escaped");
    escaped.polygons[0].object = std::string(1, static_cast<char>(0xff));
    auto invalid_utf8 = prepare(escaped);
    require(obj::publish(escaped, invalid_utf8, root / "invalid-utf8") == 1 &&
                !fs::exists(root / "invalid-utf8"),
            "invalid UTF-8 must not produce invalid JSON provenance");
    no_stage(root);
}
} // namespace
int main() try {
    Temporary temporary;
    round_trip(temporary.path);
    materials_and_snapshots(temporary.path);
    std::cout << "OBJ bundle round-trip, independent PLY/manifest, snapshot and atomic failure "
                 "checks passed\n";
    return 0;
} catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
}
