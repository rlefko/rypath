// SPDX-License-Identifier: GPL-3.0-only
#include "frame_session.hpp"
#include "mutation_shared.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>
namespace {
using namespace rypath;
namespace mh = rypath_mutation;
namespace rr = rypath_replay;
void need(bool value, const std::string &message) {
    if (!value)
        throw std::runtime_error(message);
}
#if RYPATH_TEST_MUTATION_METAL
constexpr std::uint64_t sample_id = (1ull << 32) + 29;
FrameConfig settings(unsigned steps) {
    FrameConfig c;
    c.width = 8;
    c.height = 4;
    c.seed = 19317;
    c.estimator = FrameEstimator::area_replay;
    c.mutation_steps = steps;
    return c;
}
RenderScene sky(bool packet = false, float radiance = 1) {
    RenderScene s;
    s.model = RenderModel::pbrt;
    s.spectral_transport = packet ? SpectralTransport::packet : SpectralTransport::scalar;
    s.environment.fill(radiance);
    s.raster_camera = RasterCamera{{}, {-.3f, -.3f, 1}, {.6f, 0, 0}, {0, .6f, 0}};
    s.max_depth = 4;
    return s;
}
void plane(RenderScene &s, float z, bool positive, unsigned material = 0,
           unsigned emission = no_area_emission, bool half = false) {
    GeometryMesh m;
    m.positions = {
        {half ? 0.f : -16.f, -16, z}, {16, -16, z}, {16, 16, z}, {half ? 0.f : -16.f, 16, z}};
    m.triangles = positive ? std::vector<std::array<unsigned, 3>>{{0, 1, 2}, {0, 2, 3}}
                           : std::vector<std::array<unsigned, 3>>{{0, 2, 1}, {0, 3, 2}};
    const auto id = unsigned(s.geometry.meshes.size());
    s.geometry.meshes.push_back(std::move(m));
    s.geometry.instances.push_back({id, {}});
    s.instance_materials.push_back(material);
    s.instance_emissions.push_back(emission);
    s.instance_surface_transforms.emplace_back();
}
RenderScene indirect() {
    auto s = sky(true, 0);
    SpectralMaterial a;
    a.kind = SpectralMaterialKind::constant;
    a.constant = .6f;
    s.materials.push_back(a);
    plane(s, 3, false);
    a.constant = .35f;
    s.materials.push_back(a);
    plane(s, -1, true, 1);
    PointLight light;
    light.position = {.75f, .5f, 1};
    light.intensity.fill(8);
    s.point_lights.push_back(light);
    return s;
}
RenderScene edge() {
    auto s = sky(true, 0);
    SpectralMaterial black;
    black.kind = SpectralMaterialKind::constant;
    s.materials.push_back(black);
    AreaEmission e;
    e.radiance.fill(.7f);
    e.two_sided = true;
    s.area_emissions.push_back(e);
    plane(s, 5, true, 0, 0);
    plane(s, 2, false, 0, no_area_emission, true);
    s.perspective_lens = PerspectiveLens{.3f, 4, {1, 0, 0}, {0, 1, 0}};
    s.filter = {PixelFilterKind::gaussian, 1.5f, 1.5f, .5f};
    return s;
}
std::unique_ptr<FrameSession> build(const RenderScene &s, const FrameConfig &c) {
    need(validate_render_scene(s).empty(),
         "Mutation fixture must satisfy complete render validation");
    auto b = build_frame_session(s, c);
    need(b.status == GeometryStatus::success && b.session, b.diagnostic);
    return std::move(b.session);
}
std::unique_ptr<Frame> submit(FrameSession &s, std::uint64_t epoch = 7) {
    auto r = s.submit({.sample_index = sample_id, .epoch = epoch});
    need(r.status == GeometryStatus::success && r.frame && !r.busy, r.diagnostic);
    return std::move(r.frame);
}
struct Output {
    FrameInfo info;
    FrameCapture image;
};
Output finish(Frame &frame, const FrameConfig &c, bool history, bool black = false) {
    const auto i = frame.wait();
    const auto n = std::uint64_t(c.width) * c.height;
    need(i.status == FrameStatus::complete && i.completed_samples == n,
         i.diagnostic.empty() ? "Complete mutation destination accounting" : i.diagnostic);
    need(i.history_used == history && i.temporal_candidates == (history ? 2u : 1u) &&
             i.spatial_candidates == 5 && i.fresh_candidates == n,
         "Mutation retains temporal/spatial/fresh accounting");
    need(i.mutation_attempts == n * c.mutation_steps && i.mutation_accepts <= i.mutation_attempts &&
             i.completed_evaluations == n * ((history ? 7u : 5u) + c.mutation_steps),
         "Every fixed mutation attempt adds exactly one full replay evaluation");
    if (black)
        need(i.mutation_accepts == i.mutation_attempts, "Black unit targets accept every proposal");
    auto image = frame.capture();
    need(image.status == GeometryStatus::success && image.raw_xyz.size() == n,
         "Complete mutation raw capture");
    for (const auto &xyz : image.raw_xyz)
        for (float x : xyz)
            need(std::isfinite(x) && (!black || x == 0), "Finite/black mutation estimate");
    return {i, std::move(image)};
}
void accounting() {
    need(FrameConfig{}.mutation_steps == 1, "One corrected mutation is the live default");
    for (unsigned n : {0u, 1u, 2u, 4u}) {
        auto c = settings(n);
        c.capacity = 1;
        c.quantum = 1;
        auto session = build(sky(false, 0), c);
        std::uint64_t generation = 0, serial = 0;
        for (unsigned frame = 0; frame < 3; ++frame) {
            auto f = submit(*session);
            const auto o = finish(*f, c, frame != 0, true);
            if (frame)
                need(o.info.area_serial == serial + 1 &&
                         o.info.history_generation == generation + 1,
                     "Mutation cold/warm/warm identities");
            generation = o.info.history_generation;
            serial = o.info.area_serial;
        }
        auto cut = submit(*session, 8);
        const auto cold = finish(*cut, c, false, true);
        need(cold.info.area_serial == serial + 1 && cold.info.history_generation == generation + 1,
             "Epoch reset preserves serial and resets imports");
        cut.reset();
        auto orphan = submit(*session, 8);
        session.reset();
        finish(*orphan, c, true, true);
    }
    auto baseline = settings(4);
    baseline.estimator = FrameEstimator::independent;
    auto independent = build(sky(), baseline);
    auto fresh = submit(*independent);
    const auto info = fresh->wait();
    const auto pixels = std::uint64_t(baseline.width) * baseline.height;
    need(info.status == FrameStatus::complete && info.completed_samples == pixels &&
             info.completed_evaluations == pixels && info.fresh_candidates == pixels &&
             info.mutation_attempts == 0 && info.mutation_accepts == 0 && !info.history_used,
         "Independent rendering applies zero mutations even with a legal nonzero configured count");
    need(fresh->capture().status == GeometryStatus::success, "Independent baseline raw capture");
    for (unsigned n : {3u, 5u, UINT32_MAX}) {
        const auto r = build_frame_session(sky(), settings(n));
        need(r.status == GeometryStatus::failure && !r.session && !r.diagnostic.empty(),
             "Illegal mutation count rejects before GPU submission");
    }
}
std::array<float, 3> sensor(const rr::ReplayState &p) {
    const float wavelength = 360.f + 470.f * rr::uniform(p.wavelength);
    const auto cie = pbrt_cie_xyz(wavelength);
    std::array<float, 3> value{};
    for (unsigned c = 0; c < 3; ++c)
        value[c] = float(cie[c]) * 470.f;
    return value;
}
float target(const std::array<float, 3> &xyz, float scale) {
    // Ordinary-range fixture oracle: evaluate the documented target directly,
    // without the production scaled-arithmetic or reservoir/MIS helpers.
    return std::max(std::numeric_limits<float>::min(), 1e-6f * scale) +
           ((std::abs(xyz[0]) * .25f + std::abs(xyz[1]) * .25f) + std::abs(xyz[2]) * .25f);
}
struct MutationOracle {
    bool accepted;
    float normalizer;
};
MutationOracle decide(float normalizer, float old_target, float new_target,
                      const mh::MutationIdentity &id, unsigned step) {
    if (old_target == new_target)
        return {true, normalizer};
    const bool uphill = new_target > old_target;
    int small_exponent = 0, large_exponent = 0, ratio_exponent = 0;
    const float small = std::frexp(std::min(old_target, new_target), &small_exponent);
    const float large = std::frexp(std::max(old_target, new_target), &large_exponent);
    const float ratio = small / large;
    const float fraction = std::frexp(ratio, &ratio_exponent);
    const auto significand = static_cast<unsigned>(std::ldexp(fraction, 24));
    const int bits = 24 - (ratio_exponent + small_exponent - large_exponent);
    need(bits > 0 && bits <= 277, "Independent dyadic MH probability domain");
    bool accepted = uphill, decided = uphill;
    // Compare the random binary expansion directly with the represented dyadic
    // threshold. Equality rejects. No production acceptance/update helper runs.
    for (unsigned ordinal = 0; !decided && ordinal < 9; ++ordinal) {
        const auto word = mh::acceptance_word(id, step, ordinal);
        need(word.status == mh::Status::ok, "Independent oracle entropy");
        for (unsigned bit = 0; bit < 32 && !decided; ++bit) {
            const int position = bits - 1 - int(32 * ordinal + bit);
            const unsigned threshold =
                position >= 0 && position < 24 ? (significand >> unsigned(position)) & 1u : 0u;
            const unsigned random = (word.value >> (31 - bit)) & 1u;
            if (random != threshold) {
                accepted = random < threshold;
                decided = true;
            } else if (position == 0) {
                accepted = false;
                decided = true;
            }
        }
    }
    need(decided, "Independent oracle must finish exact dyadic comparison");
    if (!accepted)
        return {false, normalizer};
    const float represented_product = ratio * large;
    const float correction = uphill ? represented_product / small : small / represented_product;
    return {true, normalizer * correction};
}
void single_pixel_oracle() {
    unsigned wavelength_moves = 0;
    for (std::uint64_t seed : {1ull, 19317ull, 0x913b6a1f7957c251ull, 0xabcdef0135792468ull}) {
        std::optional<Output> selected;
        for (auto mode : {FrameAreaOutput::selected, FrameAreaOutput::conditional_mean}) {
            auto c = settings(4);
            c.width = c.height = 1;
            c.seed = seed;
            c.area_output = mode;
            auto session = build(sky(), c);
            auto frame = submit(*session);
            const auto o = finish(*frame, c, false);
            auto payload = rr::fresh({0, unsigned(sample_id), unsigned(sample_id >> 32),
                                      unsigned(seed), unsigned(seed >> 32)});
            float normalizer = target(sensor(payload), c.target_scale);
            unsigned accepted = 0;
            for (unsigned step = 0; step < c.mutation_steps; ++step) {
                const mh::MutationIdentity id{0, unsigned(o.info.area_serial),
                                              unsigned(o.info.area_serial >> 32), unsigned(seed),
                                              unsigned(seed >> 32)};
                const auto entropy = mh::decision_words(id, step, mh::Entropy::proposal);
                need(entropy.status == mh::Status::ok, "CPU proposal entropy");
                const auto proposed = mh::propose(payload, entropy.value);
                need(proposed.status == mh::Status::ok, "CPU proposal");
                const auto decision =
                    decide(normalizer, target(sensor(payload), c.target_scale),
                           target(sensor(proposed.payload), c.target_scale), id, step);
                if (decision.accepted) {
                    wavelength_moves += payload.wavelength != proposed.payload.wavelength;
                    payload = proposed.payload;
                    ++accepted;
                }
                normalizer = decision.normalizer;
            }
            const auto expected = sensor(payload);
            need(o.info.mutation_accepts == accepted,
                 "GPU acceptance count versus independent dyadic normalizer oracle");
            for (unsigned channel = 0; channel < 3; ++channel) {
                const double value =
                    double(expected[channel]) * normalizer / target(expected, c.target_scale);
                need(std::abs(o.image.raw_xyz[0][channel] - value) <= 3e-5 * std::abs(value) + 3e-6,
                     "One-pixel cold replay wavelength and Z/target contribution");
            }
            if (selected) {
                need(o.info.mutation_accepts == selected->info.mutation_accepts,
                     "Output choice cannot change mutation decisions");
                for (unsigned channel = 0; channel < 3; ++channel)
                    need(std::abs(o.image.raw_xyz[0][channel] -
                                  selected->image.raw_xyz[0][channel]) <=
                             3e-5 * std::abs(selected->image.raw_xyz[0][channel]) + 3e-6,
                         "Five duplicated one-pixel payloads have the same conditional output");
            } else
                selected = o;
        }
    }
    need(wavelength_moves > 0, "One-pixel oracle exercises actual wavelength proposals");
}
std::array<double, 4> mean(const FrameCapture &image) {
    std::array<double, 4> out{};
    for (std::size_t i = 0; i < image.raw_xyz.size(); ++i) {
        for (unsigned c = 0; c < 3; ++c)
            out[c] += image.raw_xyz[i][c] / double(image.raw_xyz.size());
        if (i % 8 == 4)
            out[3] += image.raw_xyz[i][1] / 4.;
    }
    return out;
}
std::array<double, 4> reference(MetalRenderScene &source, const RenderScene &s,
                                std::uint64_t seed) {
    std::array<double, 4> out{};
    for (unsigned sample = 0; sample < 8; ++sample) {
        auto r = source.sample({.width = 8,
                                .height = 4,
                                .seed = seed,
                                .pixel_count = 32,
                                .sample_begin = sample_id + 101 + sample,
                                .sample_count = 1,
                                .total_samples = sample_id + 109});
        need(r.status == GeometryStatus::success, r.diagnostic);
        const bool packet = s.spectral_transport == SpectralTransport::packet;
        need(packet ? r.packet_samples.size() == 32 : r.samples.size() == 32,
             "Raw mutation-reference count");
        for (unsigned pixel = 0; pixel < 32; ++pixel) {
            std::array<double, 3> xyz{};
            if (packet) {
                const auto &p = r.packet_samples[pixel];
                need(p.pixel == pixel && p.sample == sample_id + 101 + sample && p.splat_count == 0,
                     "Packet reference identity");
                xyz = packet_sensor_xyz(p, p.radiance);
            } else {
                const auto &p = r.samples[pixel];
                need(p.pixel == pixel && p.sample == sample_id + 101 + sample,
                     "Scalar reference identity");
                xyz = pbrt_cie_xyz(p.wavelength);
                for (auto &x : xyz)
                    x *= p.radiance * 470;
            }
            for (unsigned c = 0; c < 3; ++c)
                out[c] += xyz[c] / (32 * 8.);
            if (pixel % 8 == 4)
                out[3] += xyz[1] / (4 * 8.);
        }
    }
    return out;
}
void gate(const std::vector<double> &d, double scale, const std::string &label) {
    double sum = 0, square = 0;
    for (double x : d) {
        sum += x;
        square += x * x;
    }
    const auto n = double(d.size()), avg = sum / n,
               var = std::max(0., (square - sum * avg) / (n - 1));
    const auto se = std::sqrt(var / n), tol = 6 * se + .002 + 1e-4 * std::abs(scale);
    std::cout << label << " residual=" << avg << " SE=" << se << " tolerance=" << tol << '\n';
    need(std::isfinite(avg) && std::abs(avg) <= tol,
         label + ": prespecified mutation/raw mean gate");
}
void means(const RenderScene &s, const std::string &label) {
    auto b = build_metal_renderer(s, {}, {.intersection_profile = IntersectionProfile::native});
    need(b.status == GeometryStatus::success && b.scene, b.diagnostic);
    std::array<std::array<std::vector<double>, 4>, 4> residual;
    std::array<double, 4> scale{};
    const std::array<unsigned, 4> counts{0, 1, 2, 4};
    for (unsigned seed = 0; seed < 12; ++seed) {
        const auto raw = reference(*b.scene, s, 0xc7b61a93ULL + 0xd1b54a32d192ed03ULL * seed);
        for (unsigned c = 0; c < 4; ++c)
            scale[c] += raw[c] / 12;
        for (unsigned arm = 0; arm < 4; ++arm) {
            auto cfg = settings(counts[arm]);
            cfg.seed = 0x173af251ULL + 0x9e3779b97f4a7c15ULL * seed;
            auto session = build(s, cfg);
            std::array<double, 4> averaged{};
            for (unsigned frame = 0; frame < 3; ++frame) {
                auto f = submit(*session);
                const auto v = mean(finish(*f, cfg, frame != 0).image);
                if (frame)
                    for (unsigned c = 0; c < 4; ++c)
                        averaged[c] += v[c] / 2;
            }
            for (unsigned c = 0; c < 4; ++c)
                residual[arm][c].push_back(averaged[c] - raw[c]);
        }
    }
    // 12 independent session-seed pairs, fixed before execution. Correlated
    // warm frames/pixels form ONE replicate. No seed retries or adaptive gate.
    for (unsigned arm = 0; arm < 4; ++arm)
        for (unsigned c = 0; c < (label == "DOF edge" ? 4u : 3u); ++c)
            gate(residual[arm][c], scale[c],
                 label + " steps=" + std::to_string(counts[arm]) + " channel=" + std::to_string(c));
}
#endif
} // namespace
int main() {
    try {
#if RYPATH_TEST_MUTATION_METAL
        const auto d = query_metal_device();
#ifdef RYPATH_ALLOW_NO_METAL_DEVICE
        if (d.status == MetalStatus::no_device)
            return 77;
#endif
        need(d.status == MetalStatus::success, d.diagnostic);
#ifdef RYPATH_ALLOW_NO_METAL_RAY_TRACING
        if (!d.device.compute_ray_tracing)
            return 77;
#endif
        need(d.device.compute_ray_tracing, "Mutation frame tests require compute ray tracing");
        accounting();
        single_pixel_oracle();
        means(sky(), "Scalar sky");
        means(edge(), "DOF edge");
        means(indirect(), "Indirect NEE");
#else
        const auto r = build_frame_session(
            {}, {.width = 1, .height = 1, .estimator = FrameEstimator::area_replay});
        need(r.status == GeometryStatus::not_built && !r.session,
             "Portable mutation frame availability");
#endif
        std::cout << "Mutation frame validation passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
