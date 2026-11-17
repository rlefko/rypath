// SPDX-License-Identifier: GPL-3.0-only
#include "packet_spectrum_shared.h"
#include "packet_wire.h"
#include "render_driver.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>
namespace {
using namespace rypath;
void require(bool value, std::string_view label) {
    if (!value)
        throw std::runtime_error(std::string(label));
}
void close(double actual, double expected, std::string_view label) {
    if (!std::isfinite(actual) ||
        std::abs(actual - expected) > 1e-10 * std::max(1., std::abs(expected)))
        throw std::runtime_error(std::string(label) + ": " + std::to_string(actual) + " vs " +
                                 std::to_string(expected));
}
// Fixture arguments follow the production packet's pixel/absolute-sample identity,
// followed by its uniform spectral radiance.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
PacketSample owner(std::uint32_t pixel, std::uint64_t sample, float radiance = 1) {
    PacketSample p;
    p.pixel = pixel;
    p.sample = sample;
    p.wavelengths.fill(550);
    p.wavelength_pdfs.fill(1);
    p.radiance.fill(radiance);
    return p;
}
RenderScene packet_scene() {
    RenderScene scene;
    scene.model = RenderModel::pbrt;
    scene.spectral_transport = SpectralTransport::packet;
    return scene;
}
struct Synthetic {
    unsigned builds = 0, calls = 0, fail_call = 0, corrupt_call = 0;
    bool cancel = false, scalar = false;
    std::uint64_t budget = 0;
    std::vector<RenderRequest> requests;
    detail::RenderSampleSource source() {
        return {[this] {
                    ++builds;
                    RenderBuildResult b;
                    b.status = GeometryStatus::success;
                    return b;
                },
                [this](const RenderRequest &request) {
                    ++calls;
                    requests.push_back(request);
                    cancel = true;
                    require(request.sample_count == 1, "Packet dispatch combined absolute samples");
                    require(request.total_samples == budget, "Packet total sample budget changed");
                    RenderSamplesResult tile;
                    tile.status = GeometryStatus::success;
                    if (calls == fail_call) {
                        tile.status = GeometryStatus::failure;
                        tile.diagnostic = "injected packet failure";
                        return tile;
                    }
                    if (scalar) {
                        tile.samples.push_back({});
                        return tile;
                    }
                    for (auto pixel = request.pixel_begin;
                         pixel < request.pixel_begin + request.pixel_count; ++pixel) {
                        auto p = owner(pixel, request.sample_begin,
                                       float(1 + (pixel + request.sample_begin * 7) % 13));
                        p.filter_weight = .5f + float(pixel % 3);
                        p.splat_begin = static_cast<std::uint32_t>(tile.packet_splats.size());
                        p.splat_count = 1;
                        PacketSplat splat;
                        splat.raster = {float((pixel + 1) % request.width) + .5f, .5f};
                        splat.radiance.fill(float(1 + (pixel + request.sample_begin) % 7));
                        splat.strategy = 1;
                        tile.packet_splats.push_back(splat);
                        tile.packet_samples.push_back(p);
                    }
                    if (calls == corrupt_call)
                        tile.packet_samples.back().pixel = 0;
                    return tile;
                }};
    }
};
void scheduling() {
    auto scene = packet_scene();
    RenderSettings settings{4097, 1, 3, 2, 17};
    Synthetic all;
    all.budget = settings.samples;
    auto whole = detail::drive_progressive(scene, settings, {}, all.source());
    require(whole.completion == RenderCompletion::complete, whole.diagnostic);
    require(all.calls == 6, "Unexpected packet tile count");
    for (std::size_t i = 0; i < all.requests.size(); ++i) {
        require(all.requests[i].sample_begin == i / 2, "Noncanonical packet absolute sample order");
        require(all.requests[i].pixel_begin == (i % 2 ? 4096 : 0),
                "Noncanonical packet source pixel order");
    }
    for (auto batch : {1u, 3u}) {
        auto alternate = settings;
        alternate.batch_size = batch;
        Synthetic source;
        source.budget = settings.samples;
        auto rendered = detail::drive_progressive(scene, alternate, {}, source.source());
        require(rendered.completion == RenderCompletion::complete, rendered.diagnostic);
        require(rendered.film == whole.film, "Batch partition changed packet/splat raw bits");
    }
    Synthetic prefix;
    prefix.budget = settings.samples;
    RenderControl control;
    control.cancelled = [&] { return prefix.cancel; };
    auto partial = detail::drive_progressive(scene, settings, control, prefix.source());
    require(partial.completion == RenderCompletion::cancelled, partial.diagnostic);
    require(prefix.calls == 4, "Cancellation interrupted a packet transaction");
    require(std::ranges::all_of(partial.film.pixels, [](auto &p) { return p.count == 2; }),
            "Cancellation left mixed owner counts");
    Synthetic rest;
    rest.budget = settings.samples;
    control = {};
    control.initial_film = &partial.film;
    auto resumed = detail::drive_progressive(scene, settings, control, rest.source());
    require(resumed.completion == RenderCompletion::complete && resumed.film == whole.film,
            "Packet resume changed raw Film bits");
    for (unsigned fault = 0; fault < 3; ++fault) {
        Synthetic broken;
        broken.budget = settings.samples;
        broken.fail_call = fault == 0 ? 2 : 0;
        broken.corrupt_call = fault == 1 ? 2 : 0;
        broken.scalar = fault == 2;
        auto failed = detail::drive_progressive(scene, settings, {}, broken.source());
        require(failed.completion == RenderCompletion::failed, "Malformed packet result succeeded");
        require(failed.film == make_packet_film(scene, settings),
                "Failed packet batch leaked camera/splat accumulation");
    }
    for (unsigned fault = 0; fault < 4; ++fault) {
        auto bad = partial.film;
        if (fault == 0)
            bad.packet = false;
        if (fault == 1)
            bad.filter.x_radius += 1;
        if (fault == 2)
            bad.max_component_value = 1;
        if (fault == 3)
            bad.pixels[0].splat_sum[0] = std::numeric_limits<double>::infinity();
        Synthetic source;
        source.budget = settings.samples;
        control = {};
        control.initial_film = &bad;
        auto failed = detail::drive_progressive(scene, settings, control, source.source());
        require(failed.completion == RenderCompletion::failed && source.builds == 0,
                "Invalid packet resume reached source preparation");
    }
    auto reexposed_scene = scene;
    reexposed_scene.exposure = 2;
    Synthetic reexpose;
    reexpose.budget = settings.samples;
    control = {};
    control.initial_film = &partial.film;
    auto reexposed =
        detail::drive_progressive(reexposed_scene, settings, control, reexpose.source());
    auto expected_exposure = whole.film;
    expected_exposure.exposure = 2;
    require(reexposed.completion == RenderCompletion::complete &&
                reexposed.film == expected_exposure,
            "Unclamped packet reexposure altered raw accumulation");
    auto clamped_scene = scene;
    clamped_scene.max_component_value = 1;
    auto clamped = make_packet_film(clamped_scene, settings);
    clamped.exposure = 2;
    Synthetic wrong_exposure;
    wrong_exposure.budget = settings.samples;
    control = {};
    control.initial_film = &clamped;
    auto rejected =
        detail::drive_progressive(clamped_scene, settings, control, wrong_exposure.source());
    require(rejected.completion == RenderCompletion::failed && wrong_exposure.builds == 0,
            "Finite-clamp packet exposure mismatch reached preparation");
    scene.max_depth = 64;
    settings = {1100, 1, 2, 1, 17};
    Synthetic deep;
    deep.budget = settings.samples;
    auto deep_result = detail::drive_progressive(scene, settings, {}, deep.source());
    require(deep_result.completion == RenderCompletion::complete, deep_result.diagnostic);
    const auto capacity = packet_wire::workspace_bytes / packet_wire::lane_bytes(scene.max_depth);
    require(deep.calls == 4, "Deep packet workspace failed to tile source pixels");
    for (const auto &r : deep.requests)
        require(r.pixel_count <= capacity, "Deep packet dispatch exceeded workspace");
}
void film_contract() {
    auto scene = packet_scene();
    RenderSettings settings{1, 1, 2, 2, 1};
    scene.exposure = 2;
    scene.max_component_value = 1;
    auto film = make_packet_film(scene, settings);
    auto bright = owner(0, 0, 100);
    bright.filter_weight = 3;
    auto dark = owner(0, 1, 0);
    dark.filter_weight = 1;
    auto xyz = packet_sensor_xyz(bright, bright.radiance);
    const double maximum = *std::max_element(xyz.begin(), xyz.end());
    PacketFilmBatch batch(film, 2);
    const std::array camera{bright, dark};
    require(batch.add(camera, {}).empty(), "Camera clamp transaction");
    require(batch.commit(film).empty(), "Camera clamp commit");
    auto mean = film_mean(film, film.pixels[0]);
    for (unsigned c = 0; c < 3; ++c) {
        close(mean[c], .75 * xyz[c] / maximum, "Clamp combined camera packet before filter weight");
        close(film.pixels[0].sum[c], 3 * xyz[c],
              "Compatibility clamp altered canonical accumulation");
    }
    // Each t=1 strategy is independently clamped, before splat filtering.
    scene.filter = {PixelFilterKind::box, 1, 1};
    settings.samples = 1;
    auto splat_film = make_packet_film(scene, settings);
    auto p = owner(0, 0, 0);
    p.splat_count = 2;
    PacketSplat s;
    s.raster = {.5f, .5f};
    s.radiance.fill(100);
    s.strategy = 1;
    auto t = s;
    t.strategy = 2;
    const std::array splats{s, t};
    PacketFilmBatch sb(splat_film, 1);
    require(sb.add(std::span{&p, 1}, splats).empty(), "Independent splat clamp add");
    require(sb.commit(splat_film).empty(), "Independent splat clamp commit");
    mean = film_mean(splat_film, splat_film.pixels[0]);
    for (unsigned c = 0; c < 3; ++c)
        close(mean[c], .5 * xyz[c] / maximum,
              "Individual splat clamp and filter integral normalization");
    require(splat_film.pixels[0].count == 1, "Splats advanced destination sample count");
    // One source splats onto overlapping neighboring reconstruction supports.
    scene = packet_scene();
    scene.filter = {PixelFilterKind::triangle, 1.5f, 1};
    settings = {3, 1, 1, 1, 1};
    auto boundary = make_packet_film(scene, settings);
    std::array owners{owner(0, 0, 0), owner(1, 0, 0), owner(2, 0, 0)};
    owners[0].splat_count = 1;
    owners[1].splat_begin = owners[2].splat_begin = 1;
    s.raster = {0, .5f};
    s.radiance.fill(1);
    s.strategy = 1;
    PacketFilmBatch edge(boundary, 1);
    require(edge.add(owners, std::span{&s, 1}).empty(), "Boundary splat add");
    require(edge.commit(boundary).empty(), "Boundary splat commit");
    const auto unit = packet_sensor_xyz(owners[0], s.radiance);
    for (unsigned c = 0; c < 3; ++c) {
        close(boundary.pixels[0].splat_sum[c], unit[c], "Splat cropped at image boundary");
        close(boundary.pixels[1].splat_sum[c], 0, "Splat zero at triangle support boundary");
        close(boundary.pixels[2].splat_sum[c], 0, "Splat outside support");
    }
    const auto before = boundary;
    PacketFilmBatch rollback(boundary, 1);
    auto invalid = owner(0, 1);
    invalid.splat_count = 1;
    s.strategy = 0;
    require(!rollback.add(std::span{&invalid, 1}, std::span{&s, 1}).empty(),
            "Invalid splat strategy accepted");
    require(!rollback.commit(boundary).empty() && boundary == before,
            "Invalid transaction leaked raw state");
    PacketFilmBatch missing(boundary, 1);
    auto first = owner(0, 1);
    require(missing.add(std::span{&first, 1}, {}).empty(), "Partial owner add");
    require(!missing.commit(boundary).empty() && boundary == before,
            "Incomplete transaction committed");
    for (auto kind : {PixelFilterKind::box, PixelFilterKind::triangle, PixelFilterKind::gaussian}) {
        PixelFilter filter{kind, 1.2f, .8f};
        filter.sigma = .6f;
        constexpr unsigned n = 512;
        double quadrature = 0;
        for (unsigned y = 0; y < n; ++y)
            for (unsigned x = 0; x < n; ++x)
                quadrature += filter_evaluate(
                    filter, (double(x) + .5) / n * 2 * filter.x_radius - filter.x_radius,
                    (double(y) + .5) / n * 2 * filter.y_radius - filter.y_radius);
        quadrature *= 4.0 * filter.x_radius * filter.y_radius / (double(n) * n);
        require(std::abs(quadrature - filter_integral(filter)) < 2e-6,
                "Filter analytic integral disagrees with independent midpoint quadrature");
    }
    // Terminated wavelengths retain the factor-four probability compensation.
    auto full = owner(0, 0);
    full.wavelength_pdfs.fill(.01f);
    auto terminated = full;
    terminated.wavelength_pdfs = {.0025f, 0, 0, 0};
    const auto four = packet_sensor_xyz(full, full.radiance),
               one = packet_sensor_xyz(terminated, terminated.radiance);
    for (unsigned c = 0; c < 3; ++c)
        close(four[c], one[c], "Hero wavelength termination compensation");
}
} // namespace
int main() {
    try {
        scheduling();
        film_contract();
        std::cout << "Packet driver and Film contracts passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
