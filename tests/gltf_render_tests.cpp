// SPDX-License-Identifier: GPL-3.0-only
#include "mesh_bundle.hpp"
#include "metal_render.hpp"
#include "pbrt_render.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <unistd.h>

namespace {
using namespace rypath;
namespace fs = std::filesystem;
void require(bool ok, const std::string &why) {
    if (!ok)
        throw std::runtime_error(why);
}
struct Temporary {
    fs::path path;
    Temporary() {
        auto name = (fs::temp_directory_path() / "rypath-gltf-render-XXXXXX").string();
        require(mkdtemp(name.data()), "scratch directory");
        path = name;
    }
    ~Temporary() {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }
};
void write(const fs::path &path, const std::string &source) {
    std::ofstream out(path, std::ios::binary);
    out.write(source.data(), static_cast<std::streamsize>(source.size()));
    require(out.good(), "write fixture");
}
void be32(std::string &s, std::uint32_t v) {
    for (int i = 3; i >= 0; --i)
        s.push_back(static_cast<char>(v >> (i * 8)));
}
void chunk(std::string &png, const std::string &name, const std::string &data) {
    be32(png, static_cast<std::uint32_t>(data.size()));
    const auto begin = png.size();
    png += name;
    png += data;
    std::uint32_t crc = 0xffffffff;
    for (std::size_t i = begin; i < png.size(); ++i) {
        crc ^= static_cast<unsigned char>(png[i]);
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0);
    }
    be32(png, ~crc);
}
// Owned 1x1 RGBA PNG using a stored DEFLATE block. No production image encoder.
void png_pixels(const fs::path &path, std::uint32_t width, const std::vector<unsigned char> &rgba) {
    std::string bytes("\x89PNG\r\n\x1a\n", 8), ihdr;
    be32(ihdr, width);
    be32(ihdr, 1);
    ihdr += std::string("\x08\x06\0\0\0", 5);
    chunk(bytes, "IHDR", ihdr);
    std::string raw(1, '\0');
    for (auto c : rgba)
        raw.push_back(static_cast<char>(c));
    require(raw.size() < 65536, "stored PNG block bound");
    const auto size = static_cast<std::uint16_t>(raw.size());
    std::string deflate("\x78\x01\x01", 3);
    for (auto word : {size, static_cast<std::uint16_t>(~size)}) {
        deflate.push_back(static_cast<char>(word));
        deflate.push_back(static_cast<char>(word >> 8));
    }
    deflate += raw;
    std::uint32_t a = 1, b = 0;
    for (unsigned char c : raw) {
        a = (a + c) % 65521;
        b = (b + a) % 65521;
    }
    be32(deflate, (b << 16) | a);
    chunk(bytes, "IDAT", deflate);
    chunk(bytes, "IEND", "");
    write(path, bytes);
}
void png(const fs::path &path, std::array<unsigned char, 4> rgba) {
    png_pixels(path, 1, {rgba.begin(), rgba.end()});
}
std::string header(bool back = false, bool environment = true, int depth = 0) {
    return std::string(back ? "LookAt 0 0 6 0 0 3 0 1 0\n" : "") +
           R"(Camera "perspective" "float fov" 4
Film "rgb" "integer xresolution" 1 "integer yresolution" 1 "string sensor" "cie1931"
PixelFilter "box"
Sampler "independent"
Integrator "path" "integer maxdepth" )" +
           std::to_string(depth) + "\nWorldBegin\n" +
           (environment ? "LightSource \"infinite\" \"rgb L\" [1 1 1]\n" : "");
}
std::string quad(bool front = true, bool opposite_normal = false, float z = 3) {
    std::ostringstream out;
    out << "Shape \"trianglemesh\" \"point3 P\" [-10 -10 " << z << " 10 -10 " << z << " 10 10 " << z
        << " -10 10 " << z << "] \"integer indices\" [" << (front ? "0 2 1 0 3 2" : "0 1 2 0 2 3")
        << "] \"point2 uv\" [0 0 1 0 1 1 0 1]";
    if (opposite_normal)
        out << " \"normal N\" [0 0 1 0 0 1 0 0 1 0 0 1]";
    return out.str() + "\n";
}
std::string black(const std::string &parameters = "") {
    return "Material \"rypath-gltf-v1\" \"rgb basecolor\" [0 0 0] \"float metallic\" [1] " +
           parameters + "\n";
}
RenderScene scene(const fs::path &path, const std::string &source) {
    write(path, source);
    const auto checked = pbrt::check(path);
    require(checked.exit_code() == 0, pbrt::report(checked));
    auto translated = pbrt::translate(checked, pbrt::Location{path.string()});
    if (!translated.scene) {
        for (const auto &d : translated.diagnostics)
            std::cerr << pbrt::format_diagnostic(d);
        throw std::runtime_error("translate render fixture");
    }
    return std::move(*translated.scene);
}
constexpr std::uint32_t spp = 2048;
using Means = std::array<double, 8>;
Means means(const RenderScene &input) {
    auto renderer = build_metal_renderer(input);
    require(renderer.scene && renderer.status == GeometryStatus::success, renderer.diagnostic);
    Means out{};
    for (std::size_t i = 0; i < out.size(); ++i) {
        auto result = renderer.scene->sample({.width = 1,
                                              .height = 1,
                                              .seed = 0x47231 + i,
                                              .pixel_count = 1,
                                              .sample_count = spp,
                                              .total_samples = spp});
        require(result.status == GeometryStatus::success && result.samples.size() == spp &&
                    result.guard_count > 0,
                result.diagnostic);
        for (const auto &sample : result.samples) {
            require(std::isfinite(sample.radiance) && sample.radiance >= 0,
                    "finite nonnegative raw radiance");
            out[i] += 470 * pbrt_cie_xyz(sample.wavelength)[1] * sample.radiance / spp;
        }
    }
    return out;
}
// Tolerances are fixed before execution: six standard errors across eight
// independent seed randomizations, plus 0.2% scale and a 2e-6 absolute floor.
void compare(const Means &a, const Means &b, double multiplier, const std::string &name,
             bool different = false) {
    Means delta{};
    for (std::size_t i = 0; i < 8; ++i)
        delta[i] = a[i] - multiplier * b[i];
    const double average = std::accumulate(delta.begin(), delta.end(), 0.0) / 8;
    double variance = 0;
    for (double d : delta)
        variance += (d - average) * (d - average) / 56;
    const double scale = std::accumulate(b.begin(), b.end(), 0.0) / 8;
    const double tolerance = 6 * std::sqrt(variance) + .002 * std::abs(scale) + 2e-6;
    std::cout << name << " delta=" << average << " tolerance=" << tolerance << '\n';
    require(different ? std::abs(average) > tolerance : std::abs(average) <= tolerance, name);
}
void zero(const Means &a, const std::string &name) {
    require(std::ranges::all_of(a, [](double x) { return x == 0; }), name);
}
void alpha_and_culling(const fs::path &dir) {
    const auto path = dir / "case.pbrt";
    // Invisible, valid off-camera geometry retains a supported scene while the
    // camera rays measure the environment directly.
    const auto background = means(scene(path, header() + "Translate 100 0 0\n" + black() + quad()));
    require(background[0] > 0, "environment control positive");
    zero(means(scene(path, header() + black() + quad())), "front face blocks");
    compare(means(scene(path, header() + black() + quad(false))), background, 1,
            "back face culled");
    zero(means(scene(path, header() + black("\"bool doublesided\" [true]") + quad(false))),
         "double-sided back face blocks");
    zero(means(scene(path, header() + black() + quad(true, true))),
         "culling ignores authored opposite normals");
    compare(means(scene(path, header() + black() + quad(false, true))), background, 1,
            "back culling ignores authored normals");
    zero(means(scene(path, header() + "Scale -1 1 1\n" + black() + quad())),
         "reflection preserves glTF front winding");
    zero(means(scene(path, header(true) + black() + quad(false))),
         "back camera sees opposite front");
    for (const auto &[parameters, passes] : std::array<std::pair<std::string, bool>, 6>{
             {{"\"string alphamode\" [\"MASK\"] \"float opacity\" [0] \"float alphacutoff\" [0]",
               false},
              {"\"string alphamode\" [\"MASK\"] \"float opacity\" [.5] \"float alphacutoff\" [.5]",
               false},
              {"\"string alphamode\" [\"MASK\"] \"float opacity\" [0] \"float alphacutoff\" [.5]",
               true},
              {"\"string alphamode\" [\"MASK\"] \"float opacity\" [1] \"float alphacutoff\" [1.1]",
               true},
              {"\"string alphamode\" [\"OPAQUE\"] \"float opacity\" [0]", false},
              {"\"string alphamode\" [\"BLEND\"] \"float opacity\" [0]", true}}}) {
        const auto actual = means(scene(path, header() + black(parameters) + quad()));
        if (passes)
            compare(actual, background, 1, parameters);
        else
            zero(actual, parameters);
    }
    // Vertex UV interpolation can round around .5. Bracket the filtered
    // value here; exact cutoff equality is covered by the factor cases above.
    png_pixels(dir / "alpha.png", 2, {255, 255, 255, 0, 255, 255, 255, 255});
    auto fixed_uv_quad = quad();
    const auto uv = fixed_uv_quad.find("[0 0 1 0 1 1 0 1]");
    require(uv != std::string::npos, "fixed UV fixture");
    fixed_uv_quad.replace(uv, 17, "[.5 .5 .5 .5 .5 .5 .5 .5]");
    const std::string texture =
        "Texture \"alpha\" \"spectrum\" \"imagemap\" \"string filename\" [\"alpha.png\"] \"bool "
        "rypath.gltf\" [true] \"integer rypath.magfilter\" [9729]\n";
    zero(means(scene(path, header() + texture +
                               black("\"texture basecolortexture\" [\"alpha\"] \"string "
                                     "alphamode\" [\"MASK\"] \"float alphacutoff\" [.4999]") +
                               fixed_uv_quad)),
         "filtered alpha above cutoff is opaque");
    compare(means(scene(path, header() + texture +
                                  black("\"texture basecolortexture\" [\"alpha\"] \"string "
                                        "alphamode\" [\"MASK\"] \"float alphacutoff\" [.5001]") +
                                  fixed_uv_quad)),
            background, 1, "cutoff follows alpha filtering");
    zero(means(scene(path,
                     header() + texture +
                         black("\"texture basecolortexture\" [\"alpha\"] \"float opacity\" [.5] "
                               "\"string alphamode\" [\"MASK\"] \"float alphacutoff\" [.2499]") +
                         fixed_uv_quad)),
         "cutoff follows factor multiplication");
    const auto blend = black("\"string alphamode\" [\"BLEND\"] \"float opacity\" [.25]");
    compare(means(scene(path, header() + blend + quad())), background, .75,
            "one BLEND layer expected transmittance");
    compare(means(scene(path, header() + blend + quad(true, false, 2) + quad(true, false, 3))),
            background, .5625, "two BLEND layers expected transmittance");
    const auto emitter =
        black("\"rgb emissive\" [1 1 1] \"string alphamode\" [\"MASK\"] \"float opacity\" [0]");
    zero(means(scene(path, header(false, false) + emitter + quad())),
         "zero masked emitter is dark");
    const auto active =
        means(scene(path, header(false, false) + black("\"rgb emissive\" [1 1 1]") + quad()));
    require(active[0] > 0, "opaque emitter positive");
    compare(means(scene(path, header(false, false) + black("\"rgb emissive\" [1 1 1]") +
                                  quad(true, true))),
            active, 1, "emission uses geometric front despite opposite authored normals");
    zero(means(scene(path,
                     header(true, false) + black("\"rgb emissive\" [1 1 1]") + quad(true, true))),
         "emissive geometric back remains dark with opposite authored normals");

    compare(means(scene(path, header(false, false) +
                                  black("\"rgb emissive\" [1 1 1] \"string alphamode\" [\"MASK\"] "
                                        "\"float opacity\" [0] \"float alphacutoff\" [0]") +
                                  quad())),
            active, 1, "cutoff equality activates emitter");
}
// Keep geometry, spectra, receiver and sampler fixed while replacing only the
// emitter representation. Depth 1 covers receiver NEE and continuation-hit
// emission; depth 3 also permits further scattering. Both exercise MIS.
std::string receiver_and_emitter(int depth, bool gltf, bool mask, bool cutout, bool reversed,
                                 bool two_sided, bool textured = false) {
    std::string out = header(false, false, depth);
    if (textured)
        out += "Texture \"emitter-color\" \"spectrum\" \"imagemap\" "
               "\"string filename\" [\"emitter-alpha.png\"] \"bool rypath.gltf\" [true]\n";

    if (mask)
        out += "Texture \"emitter-alpha\" \"spectrum\" \"imagemap\" "
               "\"string filename\" [\"emitter-alpha.png\"] \"bool rypath.gltf\" [true]\n";
    out += "Material \"diffuse\" \"rgb reflectance\" [.6 .6 .6]\n" + quad();
    out += "AttributeBegin\n";
    if (gltf) {
        out += black(std::string("\"rgb emissive\" [1 1 1] \"bool doublesided\" [") +
                     (two_sided ? "true" : "false") + "]" +
                     (textured ? " \"texture emissivetexture\" [\"emitter-color\"]" : "") +
                     (mask ? std::string(" \"string alphamode\" [\"MASK\"] "
                                         "\"texture basecolortexture\" [\"emitter-alpha\"] "
                                         "\"float opacity\" [.5] \"float alphacutoff\" [") +
                                 (cutout ? ".5001" : ".5") + "]"
                           : ""));
    } else {
        out += "Material \"diffuse\" \"rgb reflectance\" [0 0 0]\n"
               "AreaLightSource \"diffuse\" \"rgb L\" [1 1 1] \"bool twosided\" [" +
               std::string(two_sided ? "true" : "false") + "]\n";
    }
    out += "Shape \"trianglemesh\" \"point3 P\" [-1 2 1 1 2 1 1 2 2 -1 2 2] "
           "\"point2 uv\" [0 0 1 0 1 1 0 1] \"integer indices\" [" +
           std::string(reversed ? "0 2 1 0 3 2" : "0 1 2 0 2 3") + "]\nAttributeEnd\n";
    return out;
}
void emitter_nee_and_mis(const fs::path &dir) {
    png(dir / "emitter-alpha.png", {255, 255, 255, 255});
    const auto path = dir / "emitter-nee.pbrt";
    for (const int depth : {1, 3}) {
        const auto label = "glTF emitter depth " + std::to_string(depth);
        const auto expected =
            means(scene(path, receiver_and_emitter(depth, false, false, false, false, false)));
        require(std::ranges::all_of(expected, [](double v) { return v > 0; }),
                label + " independent PBRT emitter control is positive");
        compare(means(scene(path, receiver_and_emitter(depth, true, false, false, false, false))),
                expected, 1, label + " opaque NEE/MIS parity");
        compare(means(scene(path, receiver_and_emitter(depth, true, true, false, false, false))),
                expected, 1, label + " MASK exact-cutoff NEE/MIS parity");
        compare(
            means(scene(path, receiver_and_emitter(depth, true, false, false, false, false, true))),
            expected, 1, label + " textured emission NEE/MIS parity");
        compare(
            means(scene(path, receiver_and_emitter(depth, true, true, false, false, false, true))),
            expected, 1, label + " textured MASK emission NEE/MIS parity");
        zero(means(scene(path, receiver_and_emitter(depth, true, true, true, false, false))),
             label + " MASK cutout contributes no direct or continuation emission");
        zero(means(scene(path, receiver_and_emitter(depth, true, false, false, true, false))),
             label + " geometric back of single-sided emitter contributes nothing");
        const auto two_sided =
            means(scene(path, receiver_and_emitter(depth, false, false, false, true, true)));
        compare(means(scene(path, receiver_and_emitter(depth, true, false, false, true, true))),
                two_sided, 1, label + " two-sided emitter backside NEE/MIS parity");
    }
    const auto lit_backdrop = black("\"rgb emissive\" [1 1 1]") + quad();
    const auto backdrop = means(scene(path, header(false, false, 3) + lit_backdrop));
    compare(means(scene(path, header(false, false, 3) +
                                  black("\"string alphamode\" [\"BLEND\"] \"float opacity\" [.5]") +
                                  quad(true, false, 2) + lit_backdrop)),
            backdrop, .5, "full-depth continuation through BLEND to emitter has no extra weight");
}
void frame_mesh(const fs::path &path, float sign, bool back = false) {
    GeometryMesh mesh;
    mesh.positions = {{-10, -10, 3}, {10, -10, 3}, {10, 10, 3}, {-10, 10, 3}};
    mesh.triangles = back ? std::vector<std::array<std::uint32_t, 3>>{{0, 1, 2}, {0, 2, 3}}
                          : std::vector<std::array<std::uint32_t, 3>>{{0, 2, 1}, {0, 3, 2}};
    mesh.normals.assign(4, {0, 0, back ? 1.f : -1.f});
    mesh.tangents.assign(4, {1, 0, 0});
    mesh.tangent_signs.assign(4, sign);
    mesh.uvs = {{{0, 0}, {1, 0}, {1, 1}, {0, 1}}};
    const auto bytes = mesh_bundle::ply(mesh, 10000, "glTF", "gltf-static-v1");
    write(path, std::string(bytes.begin(), bytes.end()));
}
std::string mapped(const std::string &mesh, double scale, bool light_negative = false,
                   bool back_camera = false) {
    std::ostringstream out;
    out << header(back_camera, false, 1);
    out << "Texture \"normal\" \"spectrum\" \"imagemap\" \"string filename\" [\"normal.png\"] "
           "\"string encoding\" [\"linear\"] \"bool rypath.gltf\" [true]\n";
    // A small side emitter makes the bitangent sign observable. The opposite
    // scene is an independently mirrored geometry/light arrangement.
    out << "AttributeBegin\nMaterial \"diffuse\" \"rgb reflectance\" [0 0 0]\nAreaLightSource "
           "\"diffuse\" \"rgb L\" [20 20 20] \"bool twosided\" [true]\nShape \"trianglemesh\" "
           "\"point3 P\" [-1 "
        << (light_negative ? -2 : 2) << " 1 1 " << (light_negative ? -2 : 2) << " 1 1 "
        << (light_negative ? -2 : 2) << " 2 -1 " << (light_negative ? -2 : 2)
        << " 2] \"integer indices\" [0 1 2 0 2 3]\nAttributeEnd\n";
    out << "Material \"rypath-gltf-v1\" \"rgb basecolor\" [.6 .6 .6] \"float metallic\" [0] "
           "\"float roughness\" [.8] \"bool doublesided\" [true] \"texture normaltexture\" "
           "[\"normal\"] \"float normalscale\" ["
        << scale << "]\nShape \"plymesh\" \"string filename\" [\"" << mesh << "\"]\n";
    return out.str();
}
void normals_and_progressive(const fs::path &dir) {
    // A small X component and substantial Y tilt: the +/- signed tangent frame must
    // flip the strong bitangent lobe. This map is asymmetric by construction.
    png(dir / "normal.png", {128, 180, 245, 255});
    frame_mesh(dir / "plus.ply", 1);
    frame_mesh(dir / "minus.ply", -1);
    frame_mesh(dir / "back.ply", 1, true);
    const auto path = dir / "case.pbrt";
    const auto shadow_clear = means(scene(path, mapped("plus.ply", 0)));
    const std::string blocker_points =
        "Shape \"trianglemesh\" \"point3 P\" [-5 1 1 5 1 1 5 1 2.9 -5 1 2.9] \"integer indices\" ";
    zero(means(scene(path, mapped("plus.ply", 0) + black() + blocker_points + "[0 1 2 0 2 3]\n")),
         "front-facing glTF shadow occlusion");
    compare(
        means(scene(path, mapped("plus.ply", 0) + black() + blocker_points + "[0 2 1 0 3 2]\n")),
        shadow_clear, 1, "back-facing glTF shadow culling");
    zero(means(scene(path, mapped("plus.ply", 0) + "AttributeBegin\nScale -1 1 1\n" + black() +
                               blocker_points + "[0 1 2 0 2 3]\nAttributeEnd\n")),
         "reflected front-facing glTF shadow occlusion");
    zero(means(scene(path, mapped("plus.ply", 0) + black() + blocker_points +
                               "[0 1 2 0 2 3] \"normal N\" [0 1 0 0 1 0 0 1 0 0 1 0]\n")),
         "shadow culling ignores authored normals");

    zero(means(scene(path, mapped("plus.ply", 0) + black("\"bool doublesided\" [true]") +
                               blocker_points + "[0 2 1 0 3 2]\n")),
         "double-sided glTF shadow occlusion");
    const auto positive = means(scene(path, mapped("plus.ply", 1)));
    const auto negative = means(scene(path, mapped("minus.ply", 1)));
    compare(positive, negative, 1, "tangent handedness negative control", true);
    auto reflected = mapped("plus.ply", 1);
    reflected.insert(reflected.find("Shape \"plymesh\""), "Scale -1 1 1\n");
    compare(positive, means(scene(path, reflected)), 1,
            "reflected instance preserves signed tangent frame");

    compare(positive, means(scene(path, mapped("minus.ply", 1, true))), 1,
            "bitangent sign with mirrored side light");
    compare(means(scene(path, mapped("plus.ply", 0))), means(scene(path, mapped("minus.ply", 0))),
            1, "zero normal scale removes handedness");
    compare(positive, means(scene(path, mapped("plus.ply", -1, true))), 1,
            "signed normal scale mirrors side response");
    // Reversing the geometric front while retaining a front camera exercises
    // the double-sided mapped backface rather than only a swapped camera.
    const auto back = means(scene(path, mapped("back.ply", 1)));
    require(std::accumulate(back.begin(), back.end(), 0.0) > 0,
            "mapped double-sided backface receives light");
    compare(back, positive, 1, "double-sided mapped backface reverses shading frame");
    const auto input = scene(path, mapped("plus.ply", 1));
    RenderSettings settings{
        .width = 1, .height = 1, .samples = 64, .batch_size = 64, .seed = 0x49172};
    auto whole = render_progressive(input, settings);
    require(whole.status == GeometryStatus::success, whole.diagnostic);
    settings.batch_size = 7;
    auto split = render_progressive(input, settings);
    require(split.status == GeometryStatus::success, split.diagnostic);
    require(split.film == whole.film, "glTF progressive partition identity");
    bool stop = false;
    RenderControl control;
    control.cancelled = [&] { return stop; };
    control.committed = [&](const Film &film, const MetalDeviceInfo &) {
        stop = film.pixels[0].count >= 14;
        return std::string{};
    };
    auto partial = render_progressive(input, settings, control);
    require(partial.completion == RenderCompletion::cancelled, "glTF cancellation boundary");
    RenderControl resume;
    resume.initial_film = &partial.film;
    auto resumed = render_progressive(input, settings, resume);
    require(resumed.status == GeometryStatus::success, resumed.diagnostic);
    require(resumed.film == whole.film, "glTF resumed raw identity");
}
} // namespace
int main() try {
    const auto device = query_metal_device();
#if defined(RYPATH_ALLOW_NO_METAL_DEVICE)
    if (device.status == MetalStatus::no_device) {
        std::cout << "SKIP: " << device.diagnostic << '\n';
        return 77;
    }
#endif
    require(device.status == MetalStatus::success, device.diagnostic);
#if defined(RYPATH_ALLOW_NO_METAL_RAY_TRACING)
    if (!device.device.compute_ray_tracing) {
        std::cout << "SKIP: no compute ray tracing\n";
        return 77;
    }
#endif
    require(device.device.compute_ray_tracing, "Metal ray tracing required");
    std::cout << "Device: " << device.device.name << '\n';
    Temporary temporary;
    // A factor-only chromatic material still needs the RGB spectral table.
    const auto colored = means(scene(temporary.path / "color.pbrt",
                                     header(false, true, 1) +
                                         "Material \"rypath-gltf-v1\" \"rgb basecolor\" [.8 .2 "
                                         ".05] \"float metallic\" [.3] \"float roughness\" [.4]\n" +
                                         quad()));
    require(colored[0] > 0, "factor-only chromatic glTF material");
    alpha_and_culling(temporary.path);
    emitter_nee_and_mis(temporary.path);
    normals_and_progressive(temporary.path);
    return 0;
} catch (const std::exception &e) {
    std::cerr << "glTF render: " << e.what() << '\n';
    return 1;
}
