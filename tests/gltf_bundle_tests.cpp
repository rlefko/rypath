// SPDX-License-Identifier: GPL-3.0-only
#include "gltf.hpp"
#include "pbrt_render.hpp"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
using namespace rypath::gltf;
namespace {
namespace fs = std::filesystem;
void require(bool b, std::string_view why) {
    if (!b)
        throw std::runtime_error(std::string(why));
}
std::string text(const Conversion &c, std::string_view name) {
    for (auto &f : c.files)
        if (f.path == name)
            return {f.bytes.begin(), f.bytes.end()};
    throw std::runtime_error("missing bundle file");
}
const Asset &success(const Result &r) {
    if (!r.asset || r.exit_code())
        throw std::runtime_error(report(r));
    return *r.asset;
}
std::string diagnostics(const Conversion &c) {
    std::string s;
    for (auto &d : c.diagnostics)
        s += d.code + ": " + d.message + "\n";
    return s;
}
void roundtrip(const Asset &a, const fs::path &dir) {
    auto c = prepare(a);
    require(c.exit_code() == 0, diagnostics(c));
    require(publish(a, c, dir) == 0, diagnostics(c));
    auto again = prepare(a);
    require(again.exit_code() == 0, diagnostics(again));
    require(publish(a, again, dir) == 1, "publication must preserve existing destination");
    require(fs::exists(dir / "model.pbrt"), "existing bundle remains intact");
    fs::rename(dir, dir.string() + "-moved");
    const auto moved = fs::path(dir.string() + "-moved");
    std::ofstream f(moved / "scene.pbrt");
    f << "Camera \"perspective\"\nFilm \"rgb\" \"integer xresolution\" [1] \"integer yresolution\" "
         "[1]\nPixelFilter \"box\"\nSampler \"independent\" \"integer pixelsamples\" "
         "[1]\nIntegrator \"path\" \"integer maxdepth\" [1]\nWorldBegin\nInclude \"model.pbrt\"\n";
    f.close();
    const auto checked = rypath::pbrt::check(moved / "scene.pbrt");
    require(checked.exit_code() == 0, "relocated PBRT check");
    const auto translated =
        rypath::pbrt::translate(checked, Location{(moved / "scene.pbrt").string()});
    require(translated.exit_code() == 0, "relocated extended scene translation");
}
} // namespace
int main() try {
    const auto fixtures = fs::path(__FILE__).parent_path() / "fixtures/gltf/external";
    const auto tri = load(fixtures / "Triangle/glTF/Triangle.gltf");
    const auto &a = success(tri);
    auto c = prepare(a, tri.diagnostics);
    require(c.exit_code() == 0, diagnostics(c));
    const auto model = text(c, "model.pbrt");
    require(model.find("rypath-gltf-v1") != std::string::npos &&
                model.find("ObjectBegin") != std::string::npos &&
                model.find("ObjectInstance") != std::string::npos,
            "versioned material and object reuse");
    require(model.find("AreaLightSource") == std::string::npos,
            "emission belongs only to material");
    const auto manifest = text(c, "conversion.json");
    require(manifest.find("\"stockPbrtCompatible\":false") != std::string::npos &&
                manifest.find("\"sourceMesh\":0") != std::string::npos &&
                manifest.find("\"sha256\"") != std::string::npos,
            "provenance and compatibility declaration");
    const auto dir = fs::temp_directory_path() /
                     ("rypath-gltf-bundle-" +
                      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directory(dir);
    struct Cleanup {
        fs::path p;
        ~Cleanup() {
            std::error_code e;
            fs::remove_all(p, e);
        }
    } cleanup{dir};
    roundtrip(a, dir / "triangle");
    const auto box = load(fixtures / "Box/glTF-Binary/Box.glb");
    roundtrip(success(box), dir / "box");
    auto multi = a;
    multi.placements.push_back(multi.placements.front());
    multi.placements.back().transform[3] = 2;
    auto shared = prepare(multi);
    require(shared.exit_code() == 0, diagnostics(shared));
    require(shared.files.size() == c.files.size(), "instances reuse one mesh file");
    auto emitting = a;
    emitting.materials[0].emissive = {1, .2, .1};
    auto e = prepare(emitting);
    require(e.exit_code() == 0, diagnostics(e));
    require(text(e, "model.pbrt").find("ObjectBegin") == std::string::npos &&
                text(e, "model.pbrt").find("AreaLightSource") == std::string::npos,
            "direct material emission placement");
    emitting.materials[0].alpha_mode = "BLEND";
    require(prepare(emitting).exit_code() == 0, "opaque emitting blend accepted");
    emitting.materials[0].base_color[3] = .5;
    require(prepare(emitting).exit_code() == 1, "emitting blend rejected before publication");
    Limits low;
    low.output_bytes = 10;
    require(prepare(a, {}, low).exit_code() == 1, "output budget");
    low = Limits{};
    low.working_bytes = retained_bytes(a) + 100;
    require(prepare(a, {}, low).exit_code() == 1, "working budget");
    auto invalid = a;
    invalid.placements[0].primitive = 99;
    require(prepare(invalid).exit_code() == 1, "invalid public asset rejected");
    std::cout << "glTF bundle checks passed\n";
    return 0;
} catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
}
