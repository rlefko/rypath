// SPDX-License-Identifier: GPL-3.0-only

#include "metal_render.hpp"

#include <algorithm>
#include <exception>

namespace rypath {

RenderResult render_progressive(const RenderScene &scene, const RenderSettings &settings,
                                const std::function<void(std::uint64_t)> &progress) {
    RenderResult result;
    try {
        result.diagnostic = validate_render_settings(settings);
        if (!result.diagnostic.empty()) {
            return result;
        }
        if (settings.sampler.kind == SamplerKind::sobol &&
            (scene.model != RenderModel::pbrt || scene.max_depth > sobol_max_depth)) {
            result.diagnostic = "Sobol requires PBRT rendering with maxdepth at most 127";
            return result;
        }
        if (scene.raster_camera) {
            result.diagnostic = validate_raster_support(*scene.raster_camera, scene.filter,
                                                        settings.width, settings.height);
            if (!result.diagnostic.empty()) {
                return result;
            }
        }
        auto built = build_metal_renderer(scene);
        result.device = built.device;
        result.status = built.status;
        result.diagnostic = built.diagnostic;
        if (built.status != GeometryStatus::success) {
            return result;
        }
        result.status = GeometryStatus::failure;
        if (!built.scene) {
            result.diagnostic = "render build returned no scene";
            return result;
        }
        result.film = make_film(settings.width, settings.height, scene.model, scene.exposure);
        const auto pixel_total = static_cast<std::uint32_t>(result.film.pixels.size());
        std::uint64_t completed = 0;
        while (completed < settings.samples) {
            const auto batch_count = std::min(settings.batch_size, settings.samples - completed);
            FilmBatch batch(result.film, batch_count);
            // Pixel tiles may execute in any order. Within a pixel, absolute
            // sample indices are always accumulated in increasing order.
            for (std::uint32_t pixel_begin = 0; pixel_begin < pixel_total;) {
                const auto pixel_count = std::min(std::uint32_t{4096}, pixel_total - pixel_begin);
                const auto samples_per_dispatch = render_tile_capacity / pixel_count;
                for (std::uint64_t sample_offset = 0; sample_offset < batch_count;) {
                    const auto sample_count = static_cast<std::uint32_t>(
                        std::min(static_cast<std::uint64_t>(samples_per_dispatch),
                                 batch_count - sample_offset));
                    RenderRequest request;
                    request.width = settings.width;
                    request.height = settings.height;
                    request.seed = settings.seed;
                    request.sampler = settings.sampler;
                    request.pixel_begin = pixel_begin;
                    request.pixel_count = pixel_count;
                    request.sample_begin = completed + sample_offset;
                    request.sample_count = sample_count;
                    request.total_samples = settings.samples;
                    auto tile = built.scene->sample(request);
                    if (tile.status != GeometryStatus::success) {
                        result.diagnostic = "render batch: " + tile.diagnostic;
                        return result;
                    }
                    result.diagnostic = batch.add(tile.samples);
                    if (!result.diagnostic.empty()) {
                        return result;
                    }
                    sample_offset += sample_count;
                }
                pixel_begin += pixel_count;
            }
            result.diagnostic = batch.commit(result.film);
            if (!result.diagnostic.empty()) {
                return result;
            }
            completed += batch_count;
            if (progress) {
                progress(completed);
            }
        }
        result.status = GeometryStatus::success;
    } catch (const std::exception &error) {
        result.status = GeometryStatus::failure;
        result.diagnostic = std::string{"render: "} + error.what();
    }
    return result;
}

} // namespace rypath
