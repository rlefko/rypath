// SPDX-License-Identifier: GPL-3.0-only
#include "render_identity.hpp"
#include "render_job.hpp"

#include <array>
#include <bit>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <unistd.h>

namespace {
using namespace rypath;
void require(bool condition, std::string_view message) {
    if (!condition)
        throw std::runtime_error(std::string(message));
}

struct Scratch {
    std::filesystem::path path;
    Scratch() {
        auto name =
            (std::filesystem::temp_directory_path() / "rypath-qmc-checkpoint-XXXXXX").string();
        const auto directory = mkdtemp(name.data());
        require(directory != nullptr, "Cannot create QMC checkpoint scratch directory");
        path = directory;
    }
    ~Scratch() { std::filesystem::remove_all(path); }
};

bool exact_double(double a, double b) {
    return std::bit_cast<std::uint64_t>(a) == std::bit_cast<std::uint64_t>(b);
}
bool exact(const Film &a, const Film &b) {
    if (a.width != b.width || a.height != b.height || a.model != b.model ||
        !exact_double(a.exposure, b.exposure) || a.packet != b.packet ||
        !exact_double(a.max_component_value, b.max_component_value) || a.filter != b.filter ||
        a.compatibility_output != b.compatibility_output || a.pixels.size() != b.pixels.size())
        return false;
    for (std::size_t p = 0; p < a.pixels.size(); ++p) {
        const auto &x = a.pixels[p];
        const auto &y = b.pixels[p];
        if (x.count != y.count || !exact_double(x.weight_sum, y.weight_sum) ||
            !exact_double(x.weight_correction, y.weight_correction))
            return false;
        const std::array planes_a{x.sum,
                                  x.correction,
                                  x.splat_sum,
                                  x.splat_correction,
                                  x.compatibility_sum,
                                  x.compatibility_correction,
                                  x.compatibility_splat_sum,
                                  x.compatibility_splat_correction};
        const std::array planes_b{y.sum,
                                  y.correction,
                                  y.splat_sum,
                                  y.splat_correction,
                                  y.compatibility_sum,
                                  y.compatibility_correction,
                                  y.compatibility_splat_sum,
                                  y.compatibility_splat_correction};
        for (unsigned plane = 0; plane < planes_a.size(); ++plane)
            for (unsigned c = 0; c < 3; ++c)
                if (!exact_double(planes_a[plane][c], planes_b[plane][c]))
                    return false;
    }
    return true;
}

RenderScene sky() {
    RenderScene scene;
    scene.model = RenderModel::pbrt;
    scene.raster_camera = RasterCamera{{}, {-.5f, .5f, 1}, {1, 0, 0}, {0, -1, 0}};
    scene.filter = {PixelFilterKind::gaussian, 1.5f, .75f, .5f};
    scene.environment.fill(.625f);
    scene.max_depth = 0;
    return scene;
}

RenderScene tiled_bdpt() {
    auto scene = sky();
    scene.spectral_transport = SpectralTransport::packet;
    scene.integrator = IntegratorKind::bdpt;
    scene.max_depth = 1;
    scene.environment.fill(0);
    scene.max_component_value = .05;
    scene.raster_camera = RasterCamera{{}, {-.3f, -.3f, 1}, {.6f, 0, 0}, {0, .6f, 0}};
    SpectralMaterial matte;
    matte.kind = SpectralMaterialKind::constant;
    matte.constant = .6f;
    scene.materials.push_back(matte);
    GeometryMesh receiver;
    receiver.positions = {{-8, -8, 3}, {8, -8, 3}, {8, 8, 3}, {-8, 8, 3}};
    receiver.triangles = {{0, 2, 1}, {0, 3, 2}};
    GeometryMesh emitter;
    // Off-axis emission leaves the receiver visible from the camera.
    emitter.positions = {{1.2f, -.7f, 1}, {2.6f, -.7f, 1}, {2.6f, .7f, 1}, {1.2f, .7f, 1}};
    emitter.triangles = {{0, 1, 2}, {0, 2, 3}};
    scene.geometry.meshes = {receiver, emitter};
    scene.geometry.instances = {{0, {}}, {1, {}}};
    scene.instance_materials = {0, 0};
    scene.instance_surface_transforms.resize(2);
    scene.area_emissions.emplace_back().radiance.fill(3);
    scene.instance_emissions = {no_area_emission, 0};
    return scene;
}
void checkpoint_mode(const std::filesystem::path &directory, RenderSampler sampler,
                     bool tiled = false) {
    const auto scene = tiled ? tiled_bdpt() : sky();
    const std::uint64_t target = tiled ? 8 : 64;
    const std::uint64_t prefix = tiled ? 2 : 16;
    RenderSettings settings{.width = tiled ? 129u : 3u,
                            .height = tiled ? 3u : 5u,
                            .samples = target,
                            .batch_size = target,
                            .seed = 19,
                            .sampler = sampler};
    const auto whole = render_progressive(scene, settings);
    require(whole.completion == RenderCompletion::complete, whole.diagnostic);
    if (tiled) {
        bool camera = false, splat = false, clamped_camera = false, clamped_splat = false;
        for (const auto &pixel : whole.film.pixels) {
            camera |= pixel.sum[1] > 0;
            splat |= pixel.splat_sum[1] > 0;
            clamped_camera |= pixel.sum != pixel.compatibility_sum;
            clamped_splat |= pixel.splat_sum != pixel.compatibility_splat_sum;
        }
        require(camera && splat && clamped_camera && clamped_splat,
                "Tiled BDPT checkpoint fixture must exercise raw and clamped camera/splat planes");
    }
    settings.batch_size = prefix;
    std::uint64_t committed = 0;
    RenderControl cancel;
    cancel.committed = [&](const Film &film, const MetalDeviceInfo &) {
        committed = film.pixels.front().count;
        return std::string{};
    };
    cancel.cancelled = [&] { return committed >= prefix; };
    const auto partial = render_progressive(scene, settings, cancel);
    require(partial.completion == RenderCompletion::cancelled && committed == prefix,
            "QMC cancellation did not preserve the first complete batch");
    for (const auto &pixel : partial.film.pixels)
        require(pixel.count == prefix, "QMC cancellation omitted some pixels");

    const auto path = directory / "partial.rycp";
    RenderCheckpoint checkpoint{partial.film,
                                settings,
                                render_identity(scene, settings),
                                {{0, prefix, "qmc-prefix", partial.device.name,
                                  partial.device.architecture, "test-system"}}};
    require(write_checkpoint(checkpoint, path).empty(), "Cannot serialize native QMC prefix");
    const auto restored = read_checkpoint(path);
    if (!restored.value)
        throw std::runtime_error(restored.diagnostic);
    require(exact(restored.value->film, partial.film) &&
                restored.value->identity == checkpoint.identity &&
                restored.value->settings.sampler.kind == sampler.kind &&
                restored.value->settings.sampler.randomization == sampler.randomization,
            "QMC checkpoint lost raw accumulation, identity or sampler mode");

    settings.batch_size = tiled ? 3 : 7;
    CheckpointOptions options{.output = directory / "complete.rycp", .resume = path};
    // The job performs identity and fixed-target checks before supplying the
    // deserialized Film to the progressive renderer; initial_film alone does not.
    const auto resumed =
        run_render_job(scene, settings, options, directory / "image.exr", {}, "qmc-resume");
    require(resumed.render.completion == RenderCompletion::complete &&
                exact(resumed.render.film, whole.film),
            "Serialized QMC resume with a different batch size changed raw Film bits");
    const auto complete = read_checkpoint(options.output);
    if (!complete.value)
        throw std::runtime_error(complete.diagnostic);
    const auto &complete_checkpoint = *complete.value;
    require(exact(complete_checkpoint.film, whole.film) && complete.value->segments.size() == 2 &&
                complete.value->segments[0].begin == 0 &&
                complete.value->segments[0].end == prefix &&
                complete.value->segments[1].begin == prefix &&
                complete.value->segments[1].end == target,
            "Resumed QMC checkpoint lost its final Film or contiguous provenance");
    for (const auto &pixel : resumed.render.film.pixels)
        require(pixel.count == target, "QMC resume changed the fixed sample target");

    if (sampler.kind == SamplerKind::zsobol &&
        sampler.randomization == SobolRandomization::fastowen) {
        for (unsigned mutation = 0; mutation < 3; ++mutation) {
            auto changed_settings = settings;
            auto changed_scene = scene;
            if (mutation == 0)
                changed_settings.samples = 128;
            else if (mutation == 1)
                ++changed_settings.seed;
            else
                changed_scene.filter.sigma = .6f;
            const auto rejected = run_render_job(changed_scene, changed_settings, options,
                                                 directory / "image.exr", {}, "rejected");
            require(rejected.render.status == GeometryStatus::failure &&
                        rejected.render.diagnostic.find("does not match") != std::string::npos &&
                        rejected.render.device.name.empty(),
                    "Changed ZSobol target, seed or Gaussian filter resumed or acquired a GPU");
        }
        const auto unchanged = read_checkpoint(options.output);
        require(unchanged.value && exact(unchanged.value->film, complete_checkpoint.film) &&
                    unchanged.value->identity == complete_checkpoint.identity &&
                    unchanged.value->segments.size() == complete_checkpoint.segments.size(),
                "Rejected ZSobol resume overwrote its completed checkpoint");
    }
}
} // namespace

int main(int argc, char **argv) {
    try {
        require(argc >= 2 && std::string_view(argv[1]) == "--metal", "Expected --metal");
        bool allow_no_device = false, allow_no_ray_tracing = false;
        for (int i = 2; i < argc; ++i) {
            const std::string_view option{argv[i]};
            require(option == "--allow-no-device" || option == "--allow-no-ray-tracing",
                    "Unknown QMC checkpoint availability option");
            allow_no_device |= option == "--allow-no-device";
            allow_no_ray_tracing |= option == "--allow-no-ray-tracing";
        }
        const auto native = build_metal_renderer(sky());
        if ((native.status == GeometryStatus::no_device && allow_no_device) ||
            (native.status == GeometryStatus::no_ray_tracing && allow_no_ray_tracing)) {
            std::cout << "SKIP Metal QMC checkpoint. " << native.diagnostic << '\n';
            return 77;
        }
        require(native.status == GeometryStatus::success && native.scene, native.diagnostic);
        Scratch scratch;
        for (const auto kind : {SamplerKind::halton, SamplerKind::zsobol})
            for (const auto mode : {SobolRandomization::none, SobolRandomization::permutedigits,
                                    SobolRandomization::fastowen, SobolRandomization::owen}) {
                if (kind == SamplerKind::halton && mode == SobolRandomization::fastowen)
                    continue;
                checkpoint_mode(scratch.path, {kind, mode});
            }
        checkpoint_mode(scratch.path, {SamplerKind::halton, SobolRandomization::permutedigits},
                        true);
        std::cout << "Seven native QMC modes and tiled Halton BDPT resume with exact Film bits\n";
    } catch (const std::exception &error) {
        std::cerr << "QMC checkpoint test failure: " << error.what() << '\n';
        return 1;
    }
}
