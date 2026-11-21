// SPDX-License-Identifier: GPL-3.0-only

#include "render_driver.hpp"
#include "packet_wire.h"
#include "render_timings.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <exception>

namespace rypath {

RenderResult detail::drive_progressive(const RenderScene &scene, const RenderSettings &settings,
                                       const RenderControl &control,
                                       const RenderSampleSource &source) {
    RenderResult result;
    RenderTimingScope driver(control.timings, RenderTimingPhase::driver_overhead);
    try {
        RenderTimingScope validation(control.timings, RenderTimingPhase::driver_validation);
        result.diagnostic = validate_render_settings(settings);
        if (!result.diagnostic.empty()) {
            return result;
        }
        if (scene.spectral_transport == SpectralTransport::packet &&
            scene.integrator == IntegratorKind::bdpt && scene.max_depth > 0 &&
            settings.sampler.kind == SamplerKind::halton) {
            result.diagnostic =
                validate_halton_light_range(settings.width, settings.height, 0, settings.samples);
            if (!result.diagnostic.empty())
                return result;
        }
        const auto depth_limit = sampler_max_depth(settings.sampler, scene_has_dielectrics(scene));
        if (settings.sampler.kind != SamplerKind::independent &&
            (scene.model != RenderModel::pbrt || scene.max_depth > depth_limit)) {
            result.diagnostic =
                "QMC requires PBRT rendering with maxdepth at most " + std::to_string(depth_limit);
            return result;
        }
        if (scene.raster_camera) {
            result.diagnostic = validate_raster_support(*scene.raster_camera, scene.filter,
                                                        settings.width, settings.height);
            if (!result.diagnostic.empty()) {
                return result;
            }
        }
        if (scene.perspective_lens) {
            if (!scene.raster_camera || scene.model != RenderModel::pbrt) {
                result.diagnostic = "Perspective lens requires a PBRT raster camera";
                return result;
            }
            result.diagnostic =
                validate_lens_support(*scene.raster_camera, *scene.perspective_lens, scene.filter,
                                      settings.width, settings.height);
            if (!result.diagnostic.empty()) {
                return result;
            }
        }
        if (!((scene.model == RenderModel::demo && scene.exposure == 1) ||
              (scene.model == RenderModel::pbrt && std::isfinite(scene.exposure) &&
               scene.exposure >= 0))) {
            result.diagnostic = "Invalid film model or exposure";
            return result;
        }
        RenderTimingScope film_initialization(control.timings, RenderTimingPhase::film_initialize);
        const bool packet = scene.spectral_transport == SpectralTransport::packet;
        if (control.initial_film) {
            const auto &film = *control.initial_film;
            if (film.width != settings.width || film.height != settings.height ||
                film.model != scene.model ||
                film.pixels.size() != std::size_t{settings.width} * settings.height) {
                result.diagnostic = "Resume Film dimensions, model, or storage mismatch";
                return result;
            }
            if (film.packet != packet) {
                result.diagnostic = "Resume Film spectral transport mismatch";
                return result;
            }
            if (packet) {
                if (film.filter != scene.filter ||
                    film.max_component_value != scene.max_component_value ||
                    (std::isfinite(film.max_component_value) && film.exposure != scene.exposure)) {
                    result.diagnostic = "Resume packet Film filter, clamp, or exposure mismatch";
                    return result;
                }
                result.diagnostic = validate_film_accumulation(film);
                if (!result.diagnostic.empty())
                    return result;
            }
            const auto count = film.pixels.front().count;
            for (const auto &pixel : film.pixels) {
                if (pixel.count != count || count > settings.samples) {
                    result.diagnostic = "Resume Film has inconsistent or excessive sample counts";
                    return result;
                }
                for (std::size_t i = 0; i < 3; ++i) {
                    if (!std::isfinite(pixel.sum[i]) || !std::isfinite(pixel.correction[i]) ||
                        !std::isfinite(pixel.sum[i] + pixel.correction[i]) ||
                        (count == 0 && (pixel.sum[i] != 0 || pixel.correction[i] != 0))) {
                        result.diagnostic = "Resume Film has invalid raw accumulation";
                        return result;
                    }
                }
            }
            result.film = film;
            result.film.exposure = scene.exposure;
        } else {
            result.film =
                packet ? make_packet_film(scene, settings)
                       : make_film(settings.width, settings.height, scene.model, scene.exposure);
        }
        const auto pixel_total = static_cast<std::uint32_t>(result.film.pixels.size());
        auto completed = result.film.pixels.front().count;
        film_initialization.close();
        validation.close();
        bool in_loop = false;
        const auto notify = [&] {
            if (control.committed) {
                RenderTimingScope notification(control.timings,
                                               in_loop ? RenderTimingPhase::checkpoint_callback
                                                       : RenderTimingPhase::initial_checkpoint);
                result.diagnostic = control.committed(result.film, result.device);
                if (!result.diagnostic.empty())
                    return false;
            }
            return true;
        };
        const auto finish = [&](RenderCompletion completion) {
            result.status = GeometryStatus::success;
            result.completion = completion;
        };
        if (completed == settings.samples) {
            if (notify())
                finish(RenderCompletion::complete);
            return result;
        }
        if (control.cancelled && control.cancelled()) {
            if (notify())
                finish(RenderCompletion::cancelled);
            return result;
        }
        const auto preparation_start = std::chrono::steady_clock::now();
        auto built = render_timed(control.timings, RenderTimingPhase::preparation,
                                  [&] { return source.prepare(); });
        result.pipeline_seconds = built.pipeline_seconds;
        result.preparation_seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - preparation_start)
                .count() -
            result.pipeline_seconds;
        result.device = built.device;
        result.status = built.status;
        result.diagnostic = built.diagnostic;
        if (built.status != GeometryStatus::success) {
            // Preserve the fresh-render unavailable/error contract. A restored
            // Film remains useful to callers even when preparation fails.
            if (!control.initial_film)
                result.film = {};
            return result;
        }
        result.status = GeometryStatus::failure;
        if (!notify())
            return result;
        const auto render_start = std::chrono::steady_clock::now();
        RenderTimingScope loop(control.timings, RenderTimingPhase::render_loop);
        in_loop = true;
        while (completed < settings.samples) {
            if (control.cancelled && control.cancelled()) {
                finish(RenderCompletion::cancelled);
                return result;
            }
            const auto batch_count = std::min(settings.batch_size, settings.samples - completed);
            if (packet) {
                auto batch = render_timed(control.timings, RenderTimingPhase::film_stage, [&] {
                    return PacketFilmBatch(result.film, batch_count);
                });
                const auto packet_pixels = std::min(
                    render_tile_capacity,
                    built.packet_lane_capacity != 0
                        ? built.packet_lane_capacity
                        : static_cast<std::uint32_t>(packet_wire::workspace_bytes /
                                                     packet_wire::lane_bytes(scene.max_depth)));
                // Splat destinations overlap pixel tiles. Canonical owner order
                // is absolute sample, then source pixel, independent of batches.
                for (std::uint64_t sample_offset = 0; sample_offset < batch_count;
                     ++sample_offset) {
                    for (std::uint32_t pixel_begin = 0; pixel_begin < pixel_total;) {
                        const auto pixel_count = std::min(packet_pixels, pixel_total - pixel_begin);
                        RenderRequest request;
                        request.width = settings.width;
                        request.height = settings.height;
                        request.seed = settings.seed;
                        request.material_seed = settings.material_seed;
                        request.sampler = settings.sampler;
                        request.pixel_begin = pixel_begin;
                        request.pixel_count = pixel_count;
                        request.sample_begin = completed + sample_offset;
                        request.sample_count = 1;
                        request.total_samples = settings.samples;
                        auto tile = render_timed(control.timings, RenderTimingPhase::driver_sample,
                                                 [&] { return source.sample(request); });
                        if (tile.status != GeometryStatus::success) {
                            result.diagnostic = "render packet batch: " + tile.diagnostic;
                            return result;
                        }
                        if (!tile.samples.empty()) {
                            result.diagnostic = "Packet renderer returned scalar samples";
                            return result;
                        }
                        result.diagnostic =
                            render_timed(control.timings, RenderTimingPhase::film_add, [&] {
                                return batch.add(tile.packet_samples, tile.packet_splats);
                            });
                        if (!result.diagnostic.empty())
                            return result;
                        pixel_begin += pixel_count;
                    }
                }
                result.diagnostic = render_timed(control.timings, RenderTimingPhase::film_commit,
                                                 [&] { return batch.commit(result.film); });
                if (!result.diagnostic.empty())
                    return result;
            } else {
                auto batch = render_timed(control.timings, RenderTimingPhase::film_stage,
                                          [&] { return FilmBatch(result.film, batch_count); });
                // Pixel tiles may execute in any order. Within a pixel, absolute
                // sample indices are always accumulated in increasing order.
                for (std::uint32_t pixel_begin = 0; pixel_begin < pixel_total;) {
                    const auto pixel_count =
                        std::min(std::uint32_t{4096}, pixel_total - pixel_begin);
                    const auto samples_per_dispatch = render_tile_capacity / pixel_count;
                    for (std::uint64_t sample_offset = 0; sample_offset < batch_count;) {
                        const auto sample_count = static_cast<std::uint32_t>(
                            std::min(static_cast<std::uint64_t>(samples_per_dispatch),
                                     batch_count - sample_offset));
                        RenderRequest request;
                        request.width = settings.width;
                        request.height = settings.height;
                        request.seed = settings.seed;
                        request.material_seed = settings.material_seed;
                        request.sampler = settings.sampler;
                        request.pixel_begin = pixel_begin;
                        request.pixel_count = pixel_count;
                        request.sample_begin = completed + sample_offset;
                        request.sample_count = sample_count;
                        request.total_samples = settings.samples;
                        auto tile = render_timed(control.timings, RenderTimingPhase::driver_sample,
                                                 [&] { return source.sample(request); });
                        if (tile.status != GeometryStatus::success) {
                            result.diagnostic = "render batch: " + tile.diagnostic;
                            return result;
                        }
                        result.diagnostic =
                            render_timed(control.timings, RenderTimingPhase::film_add,
                                         [&] { return batch.add(tile.samples); });
                        if (!result.diagnostic.empty()) {
                            return result;
                        }
                        sample_offset += sample_count;
                    }
                    pixel_begin += pixel_count;
                }
                result.diagnostic = render_timed(control.timings, RenderTimingPhase::film_commit,
                                                 [&] { return batch.commit(result.film); });
                if (!result.diagnostic.empty()) {
                    return result;
                }
            }
            completed += batch_count;
            if (!notify())
                return result;
            if (control.progress) {
                render_timed(control.timings, RenderTimingPhase::progress,
                             [&] { control.progress(completed); });
            }
        }
        loop.close();
        result.render_seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - render_start).count();
        finish(RenderCompletion::complete);
    } catch (const std::exception &error) {
        result.status = GeometryStatus::failure;
        result.diagnostic = std::string{"render: "} + error.what();
    }
    return result;
}

RenderResult render_progressive(const RenderScene &scene, const RenderSettings &settings,
                                const RenderControl &control) {
    std::unique_ptr<MetalRenderScene> native;
    detail::RenderSampleSource source;
    source.prepare = [&] {
        auto built = build_metal_renderer(scene, {}, control.timings);
        if (built.status == GeometryStatus::success && !built.scene) {
            built.status = GeometryStatus::failure;
            built.diagnostic = "render build returned no scene";
        }
        native = std::move(built.scene);
        return built;
    };
    source.sample = [&](const RenderRequest &request) {
        return control.timings ? native->sample(request, *control.timings)
                               : native->sample(request);
    };
    if (!control.timings)
        return detail::drive_progressive(scene, settings, control, source);
    auto result = detail::drive_progressive(scene, settings, control, source);
    render_timed(control.timings, RenderTimingPhase::destruction, [&] { native.reset(); });
    return result;
}

RenderResult render_progressive(const RenderScene &scene, const RenderSettings &settings,
                                const std::function<void(std::uint64_t)> &progress) {
    RenderControl control;
    control.progress = progress;
    return render_progressive(scene, settings, control);
}

} // namespace rypath
