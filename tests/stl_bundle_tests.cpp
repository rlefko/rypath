// SPDX-License-Identifier: GPL-3.0-only
#include "pbrt_render.hpp"
#include "rgb_spectrum.hpp"
#include "sha256.hpp"
#include "stl.hpp"
#include <bit>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <unistd.h>

namespace {
using namespace rypath;
namespace fs = std::filesystem;
void require(bool test, const char *message) {
    if (!test)
        throw std::runtime_error(message);
}
struct Temporary {
    fs::path path;
    Temporary() {
        auto name = (fs::temp_directory_path() / "rypath-stl-bundle-XXXXXX").string();
        require(mkdtemp(name.data()), "create test directory");
        path = name;
    }
    ~Temporary() {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }
};
stl::Asset fixture(const fs::path &entry, std::size_t count = 1) {
    stl::Asset a;
    a.entry = entry;
    a.bytes = {'t', 'e', 's', 't'};
    a.requested_colors = stl::ColorMode::magics;
    a.effective_colors = stl::ColorMode::magics;
    a.solids.push_back({"empty", 0, 0, {entry.string(), 1, 1}});
    a.solids.push_back({"body", 0, count, {entry.string(), 2, 1}});
    for (std::size_t i = 0; i < count; ++i) {
        a.colors.push_back({{double(i % 32) / 31, .25, .75}, i == 0 ? 0 : .5});
        stl::Facet f;
        f.authored_normal = {0, 0, 7};
        f.attribute = 123;
        f.solid = 1;
        f.material = i;
        f.byte_offset = 84 + 50 * i;
        f.inherited_color = i == 0;
        f.location = {entry.string(), 3 + i, 1};
        a.facets.push_back(f);
        stl::Part p;
        p.material = i;
        p.source_facets = {i};
        p.geometry.positions = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
        p.geometry.triangles = {{0, 1, 2}};
        a.parts.push_back(std::move(p));
    }
    return a;
}
const stl::BundleFile &file(const stl::Conversion &c, std::string_view path) {
    for (const auto &f : c.files)
        if (f.path == path)
            return f;
    throw std::runtime_error("missing output file");
}
std::string text(const stl::Conversion &c, std::string_view path) {
    const auto &v = file(c, path).bytes;
    return {v.begin(), v.end()};
}
std::uint32_t u32(const std::vector<std::uint8_t> &v, std::size_t offset) {
    require(offset + 4 <= v.size(), "PLY truncated");
    return std::uint32_t(v[offset]) | std::uint32_t(v[offset + 1]) << 8U |
           std::uint32_t(v[offset + 2]) << 16U | std::uint32_t(v[offset + 3]) << 24U;
}
void no_stage(const fs::path &path) {
    for (const auto &e : fs::directory_iterator(path))
        require(!e.path().filename().string().starts_with(".rypath-stl-"), "stage leaked");
}
void inspect() {
    Temporary t;
    auto a = fixture(t.path / "source.stl", 2);
    std::vector<stl::Diagnostic> warnings{
        {stl::Severity::warning, "owned-warning", {a.entry.string(), 3, 1}, "retained"}};
    auto c = stl::prepare(a, warnings);
    if (c.exit_code())
        for (auto &d : c.diagnostics)
            std::cerr << d.message << '\n';
    require(c.exit_code() == 0, "prepare valid asset");
    require(!fs::exists(a.entry), "prepare wrote source");
    const auto model = text(c, "model.pbrt"), manifest = text(c, "conversion.json");
    require(model.find("ColorSpace \"srgb\"") != std::string::npos, "color space scope");
    require(model.find("[0 0.25 0.75]") != std::string::npos,
            "linear reflectance remains unpremultiplied");
    require(model.find("\"float alpha\" [0]") != std::string::npos, "explicit zero alpha");
    require(model.find("\"float alpha\" [0.5]") != std::string::npos, "explicit partial alpha");
    require(manifest.find("\"name\":\"empty\"") != std::string::npos, "empty solid retained");
    require(manifest.find("\"authoredNormal\":[0,0,7]") != std::string::npos,
            "source normal retained");
    require(manifest.find("\"attribute\":123") != std::string::npos, "raw attribute retained");
    require(manifest.find("\"inheritedColor\":true") != std::string::npos,
            "inherited flag retained");
    require(manifest.find("\"sourceFacets\":[1]") != std::string::npos, "facet mapping retained");
    require(manifest.find("\"severity\":\"warning\"") != std::string::npos, "warning retained");
    require(manifest.find(digest_hex(sha256(a.bytes))) != std::string::npos, "source hash");
    for (const auto &f : c.files)
        if (f.path != "conversion.json")
            require(manifest.find(digest_hex(sha256(f.bytes))) != std::string::npos, "output hash");
    const auto &ply = file(c, "meshes/part-0.ply").bytes;
    std::string body(ply.begin(), ply.end());
    const auto end = body.find("end_header\n") + 11;
    require(end >= 11 && end + 49 == ply.size(), "PLY size");
    require(body.find("property float nx") == std::string::npos, "no authored shading normals");
    require(std::bit_cast<float>(u32(ply, end + 12)) == 1, "independently decoded vertex");
    require(ply[end + 36] == 3 && u32(ply, end + 37) == 0 && u32(ply, end + 41) == 1 &&
                u32(ply, end + 45) == 2,
            "independently decoded winding");
    auto again = stl::prepare(a, warnings);
    require(again.files.size() == c.files.size(), "deterministic count");
    for (std::size_t i = 0; i < c.files.size(); ++i)
        require(c.files[i].bytes == again.files[i].bytes, "deterministic bytes");
    auto relocated = a;
    relocated.entry = t.path / "elsewhere" / "source.stl";
    for (auto &s : relocated.solids)
        s.location.file = std::make_shared<const std::string>(relocated.entry.string());
    for (auto &f : relocated.facets)
        f.location.file = std::make_shared<const std::string>(relocated.entry.string());
    require(text(stl::prepare(relocated), "model.pbrt") == model, "relocatable model and names");
    auto changed = a;
    changed.colors[0].coverage = 1;
    require(text(stl::prepare(changed), "model.pbrt") != model, "effective options affect names");
    require(stl::publish(a, c, t.path / "bundle") == 0, "publish validates translation");
    require(!fs::exists(t.path / "bundle" / ".validate.pbrt"), "wrapper removed");
    no_stage(t.path);
    // A new enclosing entry point after relocation exercises PBRT root-relative paths.
    fs::rename(t.path / "bundle", t.path / "moved");
    const auto enclosing = t.path / "moved" / "scene.pbrt";
    {
        std::ofstream out(enclosing);
        out << "Camera \"perspective\"\nFilm \"rgb\" \"integer xresolution\" [1] \"integer "
               "yresolution\" [1] \"string sensor\" \"cie1931\"\n"
               "PixelFilter \"box\"\nSampler \"independent\" \"integer pixelsamples\" [1]\n"
               "Integrator \"path\" \"integer maxdepth\" [1]\nWorldBegin\nLightSource "
               "\"infinite\"\nInclude \"model.pbrt\"\n";
    }
    const auto checked = pbrt::check(enclosing);
    require(checked.exit_code() == 0, "relocated PBRT structural check");
    const auto translated = pbrt::translate(checked, {enclosing.string(), 1, 1});
    require(translated.exit_code() == 0 && translated.scene.has_value(),
            "relocated render translation");
    const auto &scene = *translated.scene;
    require(scene.geometry.meshes.size() == 2 && scene.geometry.instances.size() == 2,
            "translation retains both zero and partial coverage parts");
    require(scene.geometry.meshes[0].positions[1].x == 1 &&
                scene.geometry.meshes[0].normals.empty(),
            "translation preserves position and flat normal policy");
    require(scene.instance_alpha_roots.size() == 2, "coverage roots translated");
    for (std::size_t i = 0; i < 2; ++i) {
        const auto coverage =
            evaluate_texture_graph(scene.texture_graph, scene.instance_alpha_roots[i], {}, 550);
        require(coverage && *coverage == (i == 0 ? 0.f : .5f), "translated coverage value");
        const auto &material = scene.materials[scene.instance_materials[i]];
        require(material.scattering == ScatteringKind::diffuse, "translated diffuse binding");
        const auto expected = reconstruct_srgb({float(a.colors[i].linear_rgb[0]), .25f, .75f});
        require(expected.has_value() && material.coefficients == expected->coefficients,
                "translated linear sRGB reflectance independent expectation");
    }
    for (const auto kind : {0, 1, 2}) {
        const auto dest = t.path / ("occupied" + std::to_string(kind));
        if (kind == 0) {
            std::ofstream out(dest);
            out << "sentinel";
        }
        if (kind == 1)
            fs::create_directory(dest);
        if (kind == 2)
            fs::create_symlink(t.path / "absent", dest);
        auto copy = again;
        require(stl::publish(a, copy, dest) == 1, "existing destination refusal");
        require(fs::symlink_status(dest).type() != fs::file_type::not_found,
                "destination preserved");
        no_stage(t.path);
    }
    auto broken = again;
    for (auto &f : broken.files)
        if (f.path == "model.pbrt")
            f.bytes = {'!'};
    require(stl::publish(a, broken, t.path / "bad") == 1, "bad staged PBRT rejected");
    require(!fs::exists(t.path / "bad"), "failed publish absent");
    no_stage(t.path);
    auto unsupported =
        stl::prepare(a, {{stl::Severity::unsupported, "unsupported", {}, "retained"}});
    require(unsupported.exit_code() == 2 && unsupported.files.empty(), "unsupported preserved");
}
void budgets() {
    Temporary t;
    auto a = fixture(t.path / "source.stl", 254);
    require(stl::prepare(a).exit_code() == 0, "254 PLY default expansion boundary");
    stl::Limits token_limits;
    token_limits.input.token_bytes = 80;
    const auto token_failure = stl::prepare(a, {}, token_limits);
    require(token_failure.exit_code() == 1, "quoted material token includes both quotes");
    require(token_failure.diagnostics.back().message.find("81") != std::string::npos,
            "longest generated token count reported");
    token_limits.input.token_bytes = 81;
    auto token_success = stl::prepare(a, {}, token_limits);
    require(token_success.exit_code() == 0, "exact generated quoted token boundary");
    require(stl::publish(a, token_success, t.path / "token-boundary", token_limits) == 0,
            "prepare and publish agree at token boundary");
    auto too_many = fixture(a.entry, 255);
    auto c = stl::prepare(too_many);
    require(c.exit_code() == 1 && c.diagnostics.back().message.find("255") != std::string::npos &&
                c.diagnostics.back().message.find("254") != std::string::npos,
            "255 PLY count diagnostic");
    a = fixture(a.entry);
    stl::Limits limits;
    limits.input.total_values = 20;
    require(stl::prepare(a, {}, limits).exit_code() == 1, "generated parser values bounded");
    limits = {};
    limits.input.file_bytes = 100;
    require(stl::prepare(a, {}, limits).exit_code() == 1, "file bounded in prepare");
    limits = {};
    limits.output_bytes = 100;
    require(stl::prepare(a, {}, limits).exit_code() == 1, "aggregate output bounded");
    limits = {};
    limits.working_bytes = stl::retained_bytes(a);
    require(stl::prepare(a, {}, limits).exit_code() == 1, "retained memory counted");
    limits = {};
    limits.input.directives = 12;
    require(stl::prepare(a, {}, limits).exit_code() == 1, "directive budget");
    auto malformed = a;
    malformed.parts[0].source_facets[0] = 10;
    require(stl::prepare(malformed).exit_code() == 1, "invalid source mapping");
    malformed = a;
    malformed.parts[0].geometry.triangles[0][2] = 7;
    require(stl::prepare(malformed).exit_code() == 1, "invalid triangle index");
    malformed = a;
    malformed.parts[0].geometry.positions[2] = malformed.parts[0].geometry.positions[1];
    require(stl::prepare(malformed).exit_code() == 1, "degenerate geometry rejected in prepare");
    for (const auto scale : {1e-15f, 1e15f}) {
        malformed = a;
        malformed.parts[0].geometry.positions[1].x = scale;
        malformed.parts[0].geometry.positions[2].y = scale;
        require(stl::prepare(malformed).exit_code() == 1,
                "unrepresentable native surface normal rejected in prepare");
    }
    malformed = a;
    malformed.colors[0].coverage = 2;
    require(stl::prepare(malformed).exit_code() == 1, "invalid coverage");
    malformed = a;
    malformed.solids[1].facet_count = 0;
    require(stl::prepare(malformed).exit_code() == 1, "incomplete solid coverage");
    malformed = a;
    malformed.solids[0].name = "\xff";
    require(stl::prepare(malformed).exit_code() == 1, "invalid UTF8 metadata");
}
} // namespace
int main() {
    try {
        inspect();
        budgets();
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
    return 0;
}
