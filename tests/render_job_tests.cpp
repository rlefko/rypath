// SPDX-License-Identifier: GPL-3.0-only
#include "render_identity.hpp"
#include "render_job.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <unistd.h>

namespace {
void require(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
struct Scratch {
    Scratch() {
        auto name = (std::filesystem::temp_directory_path() / "rypath-job-XXXXXX").string();
        const auto result = mkdtemp(name.data());
        if (!result)
            throw std::runtime_error("temporary directory failed");
        path = result;
    }
    ~Scratch() { std::filesystem::remove_all(path); }
    std::filesystem::path path;
};
} // namespace

int main() {
    try {
        Scratch scratch;
        const auto &dir = scratch.path;
        rypath::CheckpointOptions options;
        options.output = dir / "state.rycp";
        auto image = dir / "image.exr";
        require(rypath::validate_checkpoint_paths(options, image, {}).empty(), "valid paths");
        require(!rypath::validate_checkpoint_paths(options, options.output, {}).empty(),
                "direct collision");
        std::ofstream{options.output} << "sentinel";
        std::filesystem::create_symlink(options.output, dir / "sym");
        std::filesystem::create_hard_link(options.output, dir / "hard");
        for (const auto &alias : {dir / "sym", dir / "hard"}) {
            require(!rypath::validate_checkpoint_paths(options, alias, {}).empty(),
                    "filesystem alias");
            const std::array input{alias};
            require(!rypath::validate_checkpoint_paths(options, image, input).empty(),
                    "input alias");
        }
        std::filesystem::create_directory(dir / "parent");
        std::filesystem::create_directory_symlink(dir / "parent", dir / "parent-link");
        options.output = dir / "parent" / "new";
        require(
            !rypath::validate_checkpoint_paths(options, dir / "parent-link" / "new", {}).empty(),
            "nonexistent leaf below symlink");
        options = {};
        options.interval_supplied = true;
        require(!rypath::validate_checkpoint_paths(options, image, {}).empty(), "orphan interval");

        // A completed checkpoint must export on portable builds without GPU acquisition.
        auto scene = rypath::make_demo_scene();
        scene.model = rypath::RenderModel::pbrt;
        rypath::RenderSettings settings;
        settings.width = settings.height = 1;
        settings.samples = 3;
        rypath::RenderCheckpoint checkpoint;
        checkpoint.settings = settings;
        checkpoint.identity = rypath::render_identity(scene, settings);
        checkpoint.film = rypath::make_film(1, 1, scene.model, scene.exposure);
        checkpoint.film.pixels.front() = {{1, 2, 3}, {1e-18, -1e-18, 0}, 3};
        checkpoint.segments.push_back({0, 3, "original", "GPU", "architecture", "OS"});
        options = {};
        options.resume = dir / "complete.rycp";
        options.output = dir / "copy.rycp";
        require(rypath::write_checkpoint(checkpoint, options.resume).empty(), "write checkpoint");
        scene.exposure = 2;
        settings.batch_size = 1;
        const auto job = rypath::run_render_job(scene, settings, options, image, {}, "unused");
        require(job.render.status == rypath::GeometryStatus::success &&
                    job.render.completion == rypath::RenderCompletion::complete,
                "completed resume failed");
        require(job.render.device.name.empty(), "completed resume acquired GPU");
        require(job.render.film.exposure == 2 && job.render.film.pixels == checkpoint.film.pixels,
                "exposure changed raw samples");
        require(job.segments.size() == 1 && job.segments.front().build == "original",
                "export fabricated provenance");
        const auto copied = rypath::read_checkpoint(options.output);
        require(copied.value && copied.value->film == job.render.film,
                "completed checkpoint publication lost state");
        settings.samples = 4;
        const auto mismatch = rypath::run_render_job(scene, settings, options, image, {}, "unused");
        require(mismatch.render.status == rypath::GeometryStatus::failure &&
                    mismatch.render.diagnostic.find("does not match") != std::string::npos &&
                    mismatch.render.device.name.empty(),
                "target mismatch did not reject before GPU");
        const auto unchanged = rypath::read_checkpoint(options.output);
        require(unchanged.value && unchanged.value->film == job.render.film,
                "mismatch overwrote checkpoint");
        settings.samples = 3;
        scene.spectral_transport = rypath::SpectralTransport::packet;
        scene.max_component_value = 50;
        checkpoint.settings = settings;
        checkpoint.identity = rypath::render_identity(scene, settings);
        checkpoint.film = rypath::make_packet_film(scene, settings);
        auto &pixel = checkpoint.film.pixels.front();
        pixel.count = 3;
        pixel.sum = {1, 2, 3};
        pixel.correction = {1e-20, -1e-20, 0};
        pixel.splat_sum = {4, 5, 6};
        pixel.compatibility_sum = {7, 8, 9};
        pixel.compatibility_splat_sum = {10, 11, 12};
        pixel.weight_sum = 3;
        pixel.weight_correction = 1e-20;
        require(rypath::write_checkpoint(checkpoint, options.resume).empty(),
                "write packet checkpoint");
        const auto packet_job =
            rypath::run_render_job(scene, settings, options, image, {}, "unused");
        require(packet_job.render.status == rypath::GeometryStatus::success &&
                    packet_job.render.device.name.empty() &&
                    packet_job.render.film == checkpoint.film,
                "completed packet resume preserves every accumulation plane without GPU");
        scene.exposure = 3;
        const auto exposed = rypath::run_render_job(scene, settings, options, image, {}, "unused");
        require(exposed.render.status == rypath::GeometryStatus::failure &&
                    exposed.render.diagnostic.find("does not match") != std::string::npos &&
                    exposed.render.device.name.empty(),
                "clamped packet exposure change rejected before GPU");
        const auto protected_output = rypath::read_checkpoint(options.output);
        require(protected_output.value && protected_output.value->film == checkpoint.film,
                "clamped exposure mismatch preserves published checkpoint");
        std::cout << "Checkpoint job and path checks passed\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
