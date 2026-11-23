// SPDX-License-Identifier: GPL-3.0-only
#include "packet_dispatch.hpp"
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
    std::uint32_t capacity = 0;
    std::vector<RenderRequest> requests;
    detail::RenderSampleSource source() {
        return {[this] {
                    ++builds;
                    RenderBuildResult b;
                    b.status = GeometryStatus::success;
                    b.packet_lane_capacity = capacity;
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
    scene.max_depth = 5;
    constexpr std::uint32_t default_capacity = 19807;
    require(packet_wire::workspace_bytes / packet_wire::lane_bytes(scene.max_depth) ==
                default_capacity,
            "Update the default workspace boundary fixture for a changed packet layout");
    RenderSettings settings{default_capacity + 1, 1, 3, 2, 17};
    Synthetic all;
    all.budget = settings.samples;
    auto whole = detail::drive_progressive(scene, settings, {}, all.source());
    require(whole.completion == RenderCompletion::complete, whole.diagnostic);
    require(all.calls == 6, "Unexpected packet tile count");
    require(std::ranges::all_of(whole.film.pixels, [](auto &p) { return p.count == 3; }),
            "Default workspace partition omitted or duplicated owners");
    require(std::ranges::any_of(whole.film.pixels, [](auto &p) { return p.splat_sum[1] > 0; }),
            "Scheduling fixture did not exercise splat accumulation");
    for (std::size_t i = 0; i < all.requests.size(); ++i) {
        require(all.requests[i].sample_begin == i / 2, "Noncanonical packet absolute sample order");
        require(all.requests[i].pixel_begin == (i % 2 ? default_capacity : 0),
                "Noncanonical packet source pixel order");
    }
    for (auto batch : {1u, 3u}) {
        auto alternate = settings;
        alternate.batch_size = batch;
        Synthetic source;
        source.budget = settings.samples;
        source.capacity = 257;
        auto rendered = detail::drive_progressive(scene, alternate, {}, source.source());
        require(rendered.completion == RenderCompletion::complete, rendered.diagnostic);
        require(rendered.film == whole.film, "Tile/batch partition changed packet/splat raw bits");
        require(source.requests.front().pixel_count == 257,
                "Driver ignored advertised packet capacity");
        for (const auto &request : source.requests)
            require(request.pixel_count <= source.capacity,
                    "Driver exceeded advertised packet capacity");
    }
    Synthetic prefix;
    prefix.budget = settings.samples;
    prefix.capacity = 1234;
    const auto tiles_per_sample = 1 + (settings.width - 1) / prefix.capacity;
    require(tiles_per_sample > 1 && settings.width % prefix.capacity != 0,
            "Cancellation fixture requires multiple tiles and a partial tail");
    RenderControl control;
    control.cancelled = [&] { return prefix.cancel; };
    auto partial = detail::drive_progressive(scene, settings, control, prefix.source());
    require(partial.completion == RenderCompletion::cancelled, partial.diagnostic);
    require(prefix.calls == settings.batch_size * tiles_per_sample,
            "Cancellation interrupted a packet transaction");
    require(std::ranges::all_of(partial.film.pixels, [](auto &p) { return p.count == 2; }),
            "Cancellation left mixed owner counts");
    Synthetic rest;
    rest.budget = settings.samples;
    rest.capacity = 1777;
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
    const auto capacity = packet_wire::workspace_bytes / packet_wire::lane_bytes(scene.max_depth);
    require(capacity > 0 && capacity < std::numeric_limits<std::uint32_t>::max(),
            "Deep packet boundary fixture width exceeds uint32");
    settings = {static_cast<std::uint32_t>(capacity + 1), 1, 2, 1, 17};
    Synthetic deep;
    deep.budget = settings.samples;
    auto deep_result = detail::drive_progressive(scene, settings, {}, deep.source());
    require(deep_result.completion == RenderCompletion::complete, deep_result.diagnostic);
    require(deep.calls == 4, "Deep packet workspace failed to tile source pixels");
    require(std::ranges::all_of(deep_result.film.pixels, [](auto &p) { return p.count == 2; }),
            "Deep packet partition omitted or duplicated owners");
    for (const auto &r : deep.requests)
        require(r.pixel_count <= capacity, "Deep packet dispatch exceeded workspace");
}
void workspace_policy() {
    using packet_dispatch::lane_capacity;
    using packet_dispatch::payload_bytes;
    constexpr std::size_t baseline = std::size_t{256} * 1024 * 1024;
    constexpr std::size_t maximum = std::size_t{1024} * 1024 * 1024;
    constexpr auto largest = std::numeric_limits<std::size_t>::max();
    require(packet_wire::workspace_bytes == baseline, "Default workspace changed");
    require(payload_bytes({}, {}, largest, largest) == baseline,
            "Unknown estimate authorized growth");
    require(payload_bytes(1024, {}, 0, largest) == baseline,
            "Unknown working set authorized growth");
    require(payload_bytes(1024, {}, 1023, largest) == baseline &&
                payload_bytes(1024, {}, 1024, largest) == baseline,
            "Insufficient working-set headroom wrapped");
    require(payload_bytes(1024, 1023, largest, largest) == baseline &&
                payload_bytes(1024, 1024, largest, largest) == baseline,
            "Insufficient finite-limit headroom wrapped");
    require(payload_bytes(1024, {}, 1026, largest) == baseline &&
                payload_bytes(1024, {}, 1027, largest) == baseline + 1 &&
                payload_bytes(1024, {}, 1032, largest) == baseline + 2,
            "Growth did not charge three accounted bytes per payload byte");
    require(payload_bytes(1024, 1027, largest, largest) == baseline + 1 &&
                payload_bytes(1024, largest, 1027, largest) == baseline + 1,
            "Growth ignored the tighter scene/device budget");
    require(payload_bytes(0, {}, largest, largest) == maximum,
            "Large headroom overflowed or exceeded the 1 GiB ceiling");
    require(payload_bytes(largest, {}, largest, largest) == baseline &&
                payload_bytes(largest - 2, {}, largest, largest) == baseline,
            "Near-SIZE_MAX subtraction or small headroom wrapped");
    for (auto buffer : {std::size_t{0}, std::size_t{63}, std::size_t{64}, baseline, baseline + 64})
        require(payload_bytes(0, {}, largest, buffer) == baseline,
                "Optional growth shrank the existing baseline");
    require(payload_bytes(0, {}, largest, baseline + 65) == baseline + 1,
            "Growth did not reserve the complete buffer guard");
    constexpr auto lane = packet_wire::lane_bytes(5);
    require(lane_capacity(baseline, lane, largest) == 19807 &&
                lane_capacity(maximum, lane, largest) == render_tile_capacity,
            "Default/grown depth-five capacity changed");
    require(lane_capacity(maximum, lane, 2 * lane + 63) == 1 &&
                lane_capacity(maximum, lane, 2 * lane + 64) == 2,
            "Advertised device capacity failed at the guard boundary");
    require(lane_capacity(largest, 1, largest) == render_tile_capacity,
            "Advertised capacity exceeded backend request limit");
    require(lane_capacity(largest, 0, largest) == 0 && lane_capacity(largest, lane, 63) == 0 &&
                lane_capacity(largest, lane, 64) == 0 &&
                lane_capacity(lane - 1, lane, largest) == 0,
            "Invalid/undersized packet capacity wrapped");
}
void advertised_ceiling() {
    auto scene = packet_scene();
    RenderSettings settings{render_tile_capacity + 1, 1, 1, 1, 17};
    Synthetic source;
    source.budget = 1;
    source.capacity = std::numeric_limits<std::uint32_t>::max();
    auto result = detail::drive_progressive(scene, settings, {}, source.source());
    require(result.completion == RenderCompletion::complete, result.diagnostic);
    require(source.calls == 2 && source.requests[0].pixel_count == render_tile_capacity &&
                source.requests[1].pixel_begin == render_tile_capacity &&
                source.requests[1].pixel_count == 1,
            "Driver failed to clamp advertised capacity to backend request limit");
    require(std::ranges::all_of(result.film.pixels, [](auto &p) { return p.count == 1; }),
            "Advertised-capacity partition omitted or duplicated owners");
}
void scalar_scheduling() {
    const RenderScene scene;
    const RenderSettings settings{4097, 1, 2, 2, 17};
    const auto run = [&](std::uint32_t capacity, std::vector<RenderRequest> &requests) {
        detail::RenderSampleSource source{
            [=] {
                RenderBuildResult built;
                built.status = GeometryStatus::success;
                built.packet_lane_capacity = capacity;
                return built;
            },
            [&](const RenderRequest &request) {
                requests.push_back(request);
                RenderSamplesResult result;
                result.status = GeometryStatus::success;
                for (auto pixel = request.pixel_begin;
                     pixel < request.pixel_begin + request.pixel_count; ++pixel)
                    for (std::uint64_t sample = request.sample_begin;
                         sample < request.sample_begin + request.sample_count; ++sample)
                        result.samples.push_back({pixel, sample, 550, 1, 0, {}});
                return result;
            }};
        return detail::drive_progressive(scene, settings, {}, source);
    };
    std::vector<RenderRequest> original, advertised;
    auto a = run(0, original);
    auto b = run(1, advertised);
    require(a.completion == RenderCompletion::complete &&
                b.completion == RenderCompletion::complete && a.film == b.film,
            "Packet capacity altered scalar Film");
    require(original.size() == 2 && advertised.size() == 2 && advertised[0].pixel_count == 4096 &&
                advertised[1].pixel_count == 1 && advertised[0].sample_count == 2 &&
                advertised[1].sample_count == 2,
            "Packet capacity altered scalar dispatch partitioning");
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
        workspace_policy();
        scheduling();
        advertised_ceiling();
        scalar_scheduling();
        film_contract();
        std::cout << "Packet driver and Film contracts passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
