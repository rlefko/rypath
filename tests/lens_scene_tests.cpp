// SPDX-License-Identifier: GPL-3.0-only
#include "pbrt_render.hpp"
#include "render_identity.hpp"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <unistd.h>

namespace {
using namespace rypath;
void require(bool ok, const std::string &why) {
    if (!ok)
        throw std::runtime_error(why);
}
struct Fixture {
    std::filesystem::path directory =
        std::filesystem::temp_directory_path() / ("rypath-lens-scene-" + std::to_string(getpid()));
    Fixture() { std::filesystem::create_directories(directory); }
    ~Fixture() { std::filesystem::remove_all(directory); }
    pbrt::RenderTranslation translate(const std::string &camera,
                                      const std::string &transform = {}) const {
        const auto path = directory / "lens.pbrt";
        std::ofstream(path) << transform << "\nCamera \"perspective\" " << camera
                            << "\nFilm \"rgb\" \"integer xresolution\" [8] "
                               "\"integer yresolution\" [6]\nPixelFilter \"box\"\n"
                               "Sampler \"independent\"\nIntegrator \"path\"\nWorldBegin\n"
                               "Material \"diffuse\"\nShape \"trianglemesh\" "
                               "\"point3 P\" [-1 -1 3 1 -1 3 0 1 3]\n";
        const auto checked = pbrt::check(path);
        if (checked.exit_code() != 0) {
            pbrt::RenderTranslation failed;
            failed.status = checked.exit_code();
            failed.diagnostics = checked.document.diagnostics;
            return failed;
        }
        return pbrt::translate(checked, pbrt::Location{path.string()});
    }
};
const RenderScene &scene(const pbrt::RenderTranslation &translation) {
    if (!translation.scene)
        throw std::runtime_error("expected successful lens scene translation");
    return translation.scene.value();
}
void translation() {
    Fixture f;
    const auto pinhole = f.translate("");
    require(!scene(pinhole).perspective_lens, "default remains pinhole");
    const auto inert = f.translate("\"float lensradius\" [0] \"float focaldistance\" [1e300]");
    require(!scene(inert).perspective_lens, "zero aperture does not narrow focus");
    require(render_identity(scene(pinhole), pinhole.settings) ==
                render_identity(scene(inert), inert.settings),
            "inert focal distance must preserve effective identity");
    const auto defaults = f.translate("\"float lensradius\" [.25]");
    const auto &default_scene = scene(defaults);
    if (!default_scene.perspective_lens)
        throw std::runtime_error("positive aperture supported");
    const auto &lens = default_scene.perspective_lens.value();
    require(lens.radius == .25f && lens.focal_distance == 1e6f, "PBRT focal-distance default");
    require(lens.camera_x.x == 1 && lens.camera_x.y == 0 && lens.camera_x.z == 0 &&
                lens.camera_y.x == 0 && lens.camera_y.y == 1 && lens.camera_y.z == 0,
            "camera basis retains authored camera axes");
    const auto reflected = f.translate("\"float lensradius\" [.5] \"float focaldistance\" [4]",
                                       "Transform [-1 0 0 0 0 1 0 0 0 0 1 0 3 4 5 1]");
    const auto &reflected_scene = scene(reflected);
    if (!reflected_scene.perspective_lens || !reflected_scene.raster_camera)
        throw std::runtime_error("reflected lens pose supported");
    const auto &r = reflected_scene.perspective_lens.value();
    require(r.camera_x.x == -1 && r.camera_y.y == 1 &&
                reflected_scene.raster_camera.value().origin.x == 3 &&
                reflected_scene.raster_camera.value().origin.y == -4 &&
                reflected_scene.raster_camera.value().origin.z == -5,
            "reflection and translation preserve declared camera frame");
    for (const auto parameters : {"\"float lensradius\" [-1]", "\"float lensradius\" [1e300]",
                                  "\"float lensradius\" [1e-300]", "\"float lensradius\" [1e-40]",
                                  "\"float lensradius\" [.1] \"float focaldistance\" [0]",
                                  "\"float lensradius\" [.1] \"float focaldistance\" [-1]",
                                  "\"float lensradius\" [.1] \"float focaldistance\" [1e300]",
                                  "\"float lensradius\" [.1] \"float focaldistance\" [1e-300]",
                                  "\"float lensradius\" [1e-30] \"float focaldistance\" [1e30]"}) {
        const auto failed = f.translate(parameters);
        require(!failed.scene && !failed.diagnostics.empty(),
                "invalid or unrepresentable lens rejected: " + std::string(parameters));
        require(failed.diagnostics.front().location.line > 0, "lens diagnostic has source line");
    }
    for (const auto pose : {"Scale 2 1 1", "Transform [1 0 0 0 .1 1 0 0 0 0 1 0 0 0 0 1]"})
        require(!f.translate("\"float lensradius\" [.1]", pose).scene,
                "scaled/sheared lens camera remains unsupported");
}
void validation() {
    RasterCamera camera{{}, {-1, 1, 1}, {2, 0, 0}, {0, -2, 0}};
    PerspectiveLens lens{.25f, 4, {1, 0, 0}, {0, -1, 0}};
    require(validate_lens_support(camera, lens, {}, 8, 6).empty(), "valid lens support");
    for (const auto bad :
         {0.f, -1.f, std::numeric_limits<float>::denorm_min(),
          std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
        auto changed = lens;
        changed.radius = bad;
        require(!validate_lens_support(camera, changed, {}, 8, 6).empty(), "invalid radius");
        changed = lens;
        changed.focal_distance = bad;
        require(!validate_lens_support(camera, changed, {}, 8, 6).empty(), "invalid focus");
    }
    for (const auto axis : {GeometryPoint{0, 0, 0}, GeometryPoint{2, 0, 0}, GeometryPoint{0, -1, 0},
                            GeometryPoint{0, 0, 1}}) {
        auto changed = lens;
        changed.camera_x = axis;
        require(!validate_lens_support(camera, changed, {}, 8, 6).empty(),
                "nonrigid or raster-incompatible aperture frame");
    }
    auto huge = camera;
    huge.origin = {1e30f, 1e30f, 0};
    require(!validate_lens_support(huge, lens, {}, 8, 6).empty(),
            "aperture cannot collapse at a huge world origin");
    // Both rotated axes move along X after rounding, but their Y motion
    // disappears at a huge origin: a disk would silently become a line.
    const float h = std::sqrt(.5f);
    RasterCamera collapsed{{0, 1e30f, 0}, {-2 * h, 0, 1}, {2 * h, 2 * h, 0}, {2 * h, -2 * h, 0}};
    auto rotated = lens;
    rotated.camera_x = {h, h, 0};
    rotated.camera_y = {-h, h, 0};
    require(!validate_lens_support(collapsed, rotated, {}, 8, 6).empty(),
            "rounded independent aperture axes cannot become collinear");
    auto ratio = lens;
    ratio.radius = 1e30f;
    ratio.focal_distance = 1e-30f;
    require(!validate_lens_support(camera, ratio, {}, 8, 6).empty(), "overflowing radius/focus");
    ratio.radius = 1e-30f;
    ratio.focal_distance = 1e30f;
    require(!validate_lens_support(camera, ratio, {}, 8, 6).empty(), "underflowing radius/focus");
    auto extreme = camera;
    extreme.corner = {0, 0, 1};
    extreme.dx = {1e38f, 0, 0};
    PixelFilter broad{PixelFilterKind::triangle, 100, 100};
    require(!validate_lens_support(extreme, lens, broad, 1, 1).empty(),
            "lens validation includes full triangle-filter support");
    auto scene = make_demo_scene();
    scene.perspective_lens = lens;
    require(!validate_render_scene(scene).empty(), "lens requires PBRT raster camera");
}
} // namespace
int main() try {
    translation();
    validation();
    std::cout
        << "PASS perspective lens translation, inert defaults, reflected frame and validation\n";
} catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
}
