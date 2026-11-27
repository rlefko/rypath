// SPDX-License-Identifier: GPL-3.0-only
#include "enhanced_wire.h"
#include "frame_session.hpp"
#include "metal_execution.hpp"
#include "metal_frame_access.hpp"
#include "replay_shared.h"
#include "wavefront_wire.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
namespace {
using namespace rypath;
namespace ew = enhanced_wire;
namespace wf = wavefront_wire;
using Row = std::array<unsigned, 512>;
constexpr unsigned count = 16, guard = 256;
struct Fixture {
    unsigned mode, selected_slot, count, reserved;
};
void need(bool value, const std::string &why) {
    if (!value)
        throw std::runtime_error(why);
}
float f32(unsigned word) { return std::bit_cast<float>(word); }
long double scaled(unsigned fraction, unsigned exponent) {
    const float f = f32(fraction);
    const int e = std::bit_cast<int>(exponent);
    need((fraction == 0 && e == 0) ||
             (std::isfinite(f) && f >= .5f && f < 1 && e >= -4096 && e <= 4096),
         "invalid Number receipt");
    return std::ldexp(static_cast<long double>(f), e);
}
void close(long double actual, long double expected, const std::string &why) {
    // Fixed BEFORE execution, covers the explicit two advances (normal products,
    // four-lane sums, divisions) and the independent geometric/sensor formulas.
    const auto bound = 64.L * std::numeric_limits<float>::epsilon() * std::abs(expected);
    need(std::isfinite(actual) && std::abs(actual - expected) <= bound, why);
}
RenderScene cavity(bool conductor) {
    RenderScene scene;
    scene.model = RenderModel::pbrt;
    scene.spectral_transport = SpectralTransport::hero;
    scene.max_depth = 3;
    scene.filter.kind = PixelFilterKind::box;
    scene.raster_camera = RasterCamera{{}, {-.05f, -.05f, 1}, {.1f, 0, 0}, {0, .1f, 0}};
    SpectralMaterial material;
    material.kind = SpectralMaterialKind::constant;
    material.constant = conductor ? 0.f : .5f;
    if (conductor) {
        material.scattering = ScatteringKind::conductor;
        material.eta.knots = {{360, 1}, {830, 1}};
        material.k.knots = {{360, 2}, {830, 2}};
        material.u_roughness = material.v_roughness = .5f;
        material.remap_roughness = false;
    }
    scene.materials.push_back(material);
    AreaEmission emission;
    emission.radiance.fill(.125f);
    emission.two_sided = true;
    scene.area_emissions.push_back(emission);
    for (unsigned axis = 0; axis < 3; ++axis)
        for (int sign : {-1, 1}) {
            GeometryMesh mesh;
            constexpr int corners[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
            for (const auto &corner : corners) {
                std::array<float, 3> v{};
                v[axis] = float(sign) * 4;
                v[(axis + 1) % 3] = float(corner[0]) * 4;
                v[(axis + 2) % 3] = float(corner[1]) * 4;
                mesh.positions.push_back({v[0], v[1], v[2]});
            }
            mesh.triangles = sign < 0 ? std::vector<std::array<unsigned, 3>>{{0, 1, 2}, {0, 2, 3}}
                                      : std::vector<std::array<unsigned, 3>>{{0, 2, 1}, {0, 3, 2}};
            const auto index = unsigned(scene.geometry.meshes.size());
            scene.geometry.meshes.push_back(std::move(mesh));
            scene.geometry.instances.push_back({index, {}});
            scene.instance_materials.push_back(0);
            scene.instance_emissions.push_back(0);
            scene.instance_surface_transforms.emplace_back();
        }
    PointLight point;
    point.position = {.25f, -.375f, .125f};
    point.intensity.fill(8);
    scene.point_lights.push_back(point);
    // Rough PBRT scattering requires an authored per-instance surface frame
    // table even with constant spectra, matching the ordinary scene pipeline.
    bool needs_frames = false;
    for (std::size_t i = 0; i < scene.geometry.instances.size(); ++i)
        needs_frames = needs_frames || instance_uses_texture_context(scene, i);
    if (needs_frames)
        scene.instance_texture_frames.resize(scene.geometry.instances.size());
    for (std::size_t i = 0; i < scene.geometry.instances.size(); ++i) {
        if (!instance_uses_texture_context(scene, i))
            continue;
        const auto &transform = scene.instance_surface_transforms[i];
        const auto frame_error = prepare_texture_triangle_frames(
            scene.geometry.meshes[scene.geometry.instances[i].mesh_index], transform.shape,
            transform.placement, scene.instance_texture_frames[i]);
        need(frame_error.empty(), "Native cavity surface frames: " + frame_error);
    }
    const auto diagnostic = validate_render_scene(scene);
    need(diagnostic.empty(), diagnostic);
    return scene;
}
void numeric_receipt(const Row &r, const std::string &label) {
    if (!r[4])
        return;
    need(r[43] == 0, label + " helper receipt validation");
    const auto *v = r.data() + 448;
    std::array<long double, 4> P{}, B{}, Qin{}, Nin{}, Qout{}, Nout{};
    long double sum0 = 0, sum1 = 0, sum2 = 0;
    for (unsigned k = 0; k < 4; ++k) {
        P[k] = scaled(v[2 * k], v[2 * k + 1]);
        B[k] = f32(v[8 + k]);
        Nin[k] = f32(v[12 + k]);
        Qin[k] = f32(v[16 + k]);
        Nout[k] = f32(v[20 + k]);
        Qout[k] = f32(v[24 + k]);
        sum0 += P[k];
        sum1 += P[k] * Qin[k];
        sum2 += P[k] * Qin[k] * Qout[k];
    }
    need(sum0 > 0 && sum1 > 0 && sum2 > 0, label + " positive two-advance support");
    for (unsigned k = 0; k < 4; ++k) {
        const auto beta1 = B[k] * Nin[k] * sum0 / sum1;
        const auto beta2 = beta1 * Nout[k] * sum1 / sum2;
        close(f32(v[28 + k]), beta2, label + " independent beta after two advances");
        close(scaled(v[32 + 2 * k], v[33 + 2 * k]), P[k] * Qin[k] * Qout[k],
              label + " independent P after two advances");
        close(scaled(v[40 + 2 * k], v[41 + 2 * k]), P[k] * Qin[k],
              label + " immediate-E MIS retains pre-pivot prefix");
    }
    long double r2 = 0, dot = 0, norm2 = 0;
    for (unsigned k = 0; k < 3; ++k) {
        const long double delta = static_cast<long double>(f32(v[51 + k])) - f32(v[48 + k]);
        const long double normal = f32(v[57 + k]);
        r2 += delta * delta;
        dot += delta * normal;
        norm2 += normal * normal;
    }
    need(r2 > 0 && norm2 > 0, label + " independent owned edge");
    const auto cosine = std::abs(dot) / std::sqrt(r2 * norm2);
    close(scaled(v[55], v[56]), r2, label + " independent distance squared");
    close(f32(v[54]), cosine, label + " destination geometric cosine");
    close(scaled(r[20], r[21]), Qin[0] * (cosine / r2) * Qout[0],
          label + " independent h_eval factorization");
    need(scaled(r[22], r[23]) > 0,
         label + " positive generation density (not assumed equal query density)");
    if (r[6]) {
        need(r[40] == 1 && f32(r[41]) > 0 && f32(r[42]) > 0,
             label + " separate actual generation sampler receipts");
        close(scaled(r[22], r[23]),
              static_cast<long double>(f32(r[41])) * (cosine / r2) * f32(r[42]),
              label + " independent h_gen factorization from same-input sampler receipts");
    }
    need(r[36] == r[10] && r[37] == r[15] && r[38] == r[18],
         label + " exact stored world omega ray direction");
}
void run_variant(const std::filesystem::path &path, bool conductor) {
    const std::string label = conductor ? "rough-.5" : "diffuse";
    FrameConfig settings;
    settings.width = count;
    settings.height = 1;
    settings.capacity = count;
    settings.quantum = 8;
    settings.seed = 51001;
    settings.intersection_profile = IntersectionProfile::native;
    settings.estimator = FrameEstimator::independent;
    auto built = build_frame_session(cavity(conductor), settings);
    need(built.status == GeometryStatus::success && built.session, built.diagnostic);
    auto submitted = built.session->submit({.sample_index = (1ull << 32) + 29});
    need(submitted.status == GeometryStatus::success && submitted.frame, submitted.diagnostic);
    need(submitted.frame->wait().status == FrameStatus::complete,
         "resident fixture frame completion");
    auto acquired = MetalFrameAccess::acquire(*submitted.frame);
    need(acquired.status == FrameStatus::complete && acquired.lease, acquired.diagnostic);
    auto &lease = *acquired.lease;
    auto binding = lease.create_guide_pipeline(@"rypath_reconstruction_guide_surface");
    need(binding.status == GeometryStatus::success && binding.pipeline, binding.diagnostic);
    std::string why;
    auto library = metal::load_library(lease.device(), path, why);
    need(library != nil, why);
    auto constants = [MTLFunctionConstantValues new];
    const bool yes = true, no = false;
    for (unsigned i = 0; i <= 18; ++i)
        [constants setConstantValue:&yes type:MTLDataTypeBool atIndex:i];
    for (unsigned i : {20u, 22u})
        [constants setConstantValue:&no type:MTLDataTypeBool atIndex:i];
    for (unsigned i : {21u, 25u, 27u, 28u})
        [constants setConstantValue:&yes type:MTLDataTypeBool atIndex:i];
    const unsigned zero = 0, one = 1;
    for (unsigned i : {19u, 23u, 29u})
        [constants setConstantValue:&zero type:MTLDataTypeUInt atIndex:i];
    [constants setConstantValue:&one type:MTLDataTypeUInt atIndex:24];
    auto intersection = metal::load_patch_intersection(library, why);
    need(intersection != nil, why);
    auto function =
        metal::load_function(library, @"rypath_enhanced_hybrid_runtime_probe", why, constants);
    need(function != nil, why);
    auto pipeline =
        metal::load_pipeline(lease.device(), library, @"rypath_enhanced_hybrid_runtime_probe", why,
                             intersection, constants);
    need(pipeline != nil, why);
    auto table = metal::make_intersection_table(pipeline, intersection, why);
    need(table != nil, why);
    auto encoder_layout = [function newArgumentEncoderWithBufferIndex:1];
    need(encoder_layout != nil, "runtime probe argument encoder");
    auto allocate = [&](std::size_t n) {
        auto b = [lease.device() newBufferWithLength:n options:MTLResourceStorageModeShared];
        need(b != nil, "runtime probe allocation");
        std::memset(b.contents, 0xa5, n);
        return b;
    };
    auto arguments = allocate(encoder_layout.encodedLength);
    std::array<id<MTLBuffer>, 7> buffers{};
    buffers[0] = allocate(sizeof(wf::Config));
    buffers[1] = allocate(471 * sizeof(render_wire::Point));
    buffers[2] = allocate(ew::bank_count * count * sizeof(ew::Record) + 2 * guard);
    buffers[3] = allocate(count * sizeof(ew::Evaluation) + 2 * guard);
    buffers[4] = allocate(count * sizeof(Row) + 2 * guard);
    buffers[5] = allocate(count * sizeof(rypath_replay::ReplayState));
    buffers[6] = allocate(sizeof(Fixture));
    auto *config = static_cast<wf::Config *>(buffers[0].contents);
    *config = {};
    config->pixel_count = config->capacity = count;
    config->packet = 2;
    config->model = render_wire::pbrt_model;
    config->packet_events = config->group_width = config->replay_enabled = 1;
    config->contribution_mode = 2;
    config->contribution_depth = 3;
    config->contribution_target_scale = 1;
    config->contribution_serial_low = 17;
    auto *cie = static_cast<render_wire::Point *>(buffers[1].contents);
    for (unsigned k = 0; k < 471; ++k)
        cie[k] = {.125f, .25f, .5f};
    auto *replays = static_cast<rypath_replay::ReplayState *>(buffers[5].contents);
    for (unsigned n = 0; n < count; ++n)
        replays[n] = {
            {0x1234567u + n * 7919u, 0xabcdef01u}, {1, 2}, {3, 4}, 5, rypath_replay::contract};
    auto *records =
        reinterpret_cast<ew::Record *>(static_cast<unsigned char *>(buffers[2].contents) + guard);
    auto *evaluations = reinterpret_cast<ew::Evaluation *>(
        static_cast<unsigned char *>(buffers[3].contents) + guard);
    auto *fixture = static_cast<Fixture *>(buffers[6].contents);
    [encoder_layout setArgumentBuffer:arguments offset:0];
    for (unsigned i = 0; i < buffers.size(); ++i)
        [encoder_layout setBuffer:buffers[i] offset:i >= 2 && i <= 4 ? guard : 0 atIndex:i];
    auto parameters = lease.parameters();
    parameters.packet_transport = 2;
    parameters.bidirectional = 0;
    parameters.max_depth = 3;
    parameters.sampler_kind = render_wire::independent_sampler;
    parameters.lens_radius = 0;
    parameters.gaussian_width = 0;
    need(parameters.filter_kind == render_wire::box_filter && parameters.instance_count == 6,
         "manual pinhole/box native fixture parameters");
    const auto dispatch = [&](bool fresh, unsigned slot, bool prefix, bool poison) {
        config->contribution_replay = prefix ? 1u : 0u;
        *fixture = {fresh ? 0u : 1u, slot, count, 0};
        parameters.gaussian_width = poison ? 1u : 0u;
        for (unsigned n = 0; n < count; ++n)
            evaluations[n] = {fresh ? ew::absent() : ew::Reference{ew::history_bank_0, n},
                              ew::Reference{ew::fresh_bank, n},
                              0,
                              0,
                              0,
                              0};
        auto command = [lease.queue() commandBuffer];
        auto encoder = [command computeCommandEncoder];
        need(command && encoder, "runtime probe command");
        need(lease.bind_guides(encoder, *binding.pipeline, arguments).empty(),
             "resident resource binding");
        // Binding carrier is never dispatched. Actual constants25/21 and all
        // transport parameters/table/argument layout belong to this probe.
        [encoder setComputePipelineState:pipeline];
        [encoder setBytes:&parameters length:sizeof(parameters) atIndex:0];
        [encoder setIntersectionFunctionTable:table atBufferIndex:14];
        for (auto buffer : buffers)
            [encoder useResource:buffer usage:MTLResourceUsageRead | MTLResourceUsageWrite];
        [encoder dispatchThreads:MTLSizeMake(count, 1, 1)
            threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
        [encoder endEncoding];
        need(lease.retain_until_completed(command).empty(), "retain resident AS/buffers");
        need(metal::complete(command, "native hybrid runtime probe").empty(),
             "runtime probe GPU completion");
        std::array<Row, count> result{};
        std::memcpy(result.data(), static_cast<unsigned char *>(buffers[4].contents) + guard,
                    sizeof(result));
        for (unsigned id : {2u, 3u, 4u}) {
            const auto *b = static_cast<const unsigned char *>(buffers[id].contents);
            for (unsigned k = 0; k < guard; ++k)
                need(b[k] == 0xa5 && b[buffers[id].length - guard + k] == 0xa5,
                     "runtime probe guard");
        }
        bool invalid_dispatch = false;
        for (unsigned n = 0; n < count; ++n) {
            const auto &r = result[n];
            invalid_dispatch |= r[0] != 0 || r[1] != render_wire::success || r[3] != 1 ||
                                r[48] != 1 || r[55] != 1 || r[58] != 284 ||
                                evaluations[n].ready != 1;
        }
        if (invalid_dispatch) {
            for (unsigned n = 0; n < count; ++n) {
                const auto &r = result[n];
                const auto &e = evaluations[n];
                std::cerr << label << " dispatch n=" << n << " fresh=" << fresh << " slot=" << slot
                          << " prefix=" << prefix << " poison=" << poison << " failed=" << r[0]
                          << " sample_status=" << r[1] << " steps=" << r[2] << " complete=" << r[3]
                          << " finished=" << r[48] << " result_ready=" << r[55]
                          << " begin_failed=" << r[32] << " outside=" << r[59]
                          << " first_next=" << r[60] << " first_selected=" << r[61]
                          << " first_status=" << r[62] << " primary_ready=" << r[44]
                          << " primary_r2_fraction=" << r[45]
                          << " primary_r2_exponent=" << std::bit_cast<int>(r[46])
                          << " primary_cosine_bits=" << r[47] << " source=" << e.source.bank << ':'
                          << e.source.pixel << " result=" << e.result.bank << ':' << e.result.pixel
                          << " owned_ready=" << e.ready
                          << " owned_h=" << std::bit_cast<unsigned>(e.target_h_fraction) << ':'
                          << e.target_h_exponent << " owned_Zexp=" << e.normalizer_exponent
                          << " source_record_tag=" << records[n].tag
                          << " source_record_event=" << records[n].event_ordinal << std::endl;
            }
            std::ofstream file;
            if (const char *artifact_path = std::getenv("RYPATH_HYBRID_PROBE_DIAGNOSTICS"))
                file.open(artifact_path);
            std::ostream &dump = file.is_open() ? static_cast<std::ostream &>(file) : std::cerr;
            dump << "{\"schema\":2,\"variant\":\"" << label << "\",\"fresh\":" << fresh
                 << ",\"slot\":" << slot << ",\"prefix\":" << prefix << ",\"poison\":" << poison
                 << ",\"row_words\":512,\"rows\":[";
            for (unsigned n = 0; n < count; ++n) {
                if (n)
                    dump << ',';
                dump << '[';
                for (unsigned k = 0; k < result[n].size(); ++k) {
                    if (k)
                        dump << ',';
                    dump << result[n][k];
                }
                dump << ']';
            }
            dump << "],\"source_records\":[";
            for (unsigned n = 0; n < count; ++n) {
                if (n)
                    dump << ',';
                std::array<unsigned, 12> words{};
                std::memcpy(words.data(), &records[n], sizeof(ew::Record));
                dump << '[';
                for (unsigned k = 0; k < words.size(); ++k) {
                    if (k)
                        dump << ',';
                    dump << words[k];
                }
                dump << ']';
            }
            dump << "],\"evaluations\":[";
            for (unsigned n = 0; n < count; ++n) {
                if (n)
                    dump << ',';
                std::array<unsigned, 8> words{};
                std::memcpy(words.data(), &evaluations[n], sizeof(ew::Evaluation));
                dump << '[';
                for (unsigned k = 0; k < words.size(); ++k) {
                    if (k)
                        dump << ',';
                    dump << words[k];
                }
                dump << ']';
            }
            dump << "]}\n" << std::flush;
        }
        for (unsigned n = 0; n < count; ++n) {
            const auto &r = result[n];
            need(r[0] == 0 && r[1] == render_wire::success && r[3] == 1 && r[48] == 1 &&
                     r[55] == 1 && r[58] == 284,
                 label + " status/finished fixed catalog n=" + std::to_string(n));
            need(evaluations[n].ready == 1, label + " owned ready publication");
            numeric_receipt(r, label + " numeric n=" + std::to_string(n));
        }
        return result;
    };
    const auto source = dispatch(true, 0, false, false);
    bool unexpected_catalog = false;
    for (unsigned n = 0; n < count; ++n) {
        const auto &r = source[n];
        std::cout << label << " source n=" << n << " active=" << r[4] << " pivot=" << r[5]
                  << " captured_tag=" << r[9] << " arena_tag=" << records[n].tag
                  << " arena_event=" << records[n].event_ordinal
                  << " instance=" << records[n].instance << " primitive=" << records[n].primitive
                  << " status=" << r[1] << " failed=" << r[0] << " steps=" << r[2]
                  << " selected=" << r[49] << " h_fraction_bits=" << r[20]
                  << " h_exponent=" << std::bit_cast<int>(r[21]) << std::endl;
        unexpected_catalog |=
            r[4] && (records[n].tag != 5 || r[5] < 1 || r[5] > 2 || (!conductor && r[5] != 1));
        unexpected_catalog |= std::memcmp(r.data() + 8, r.data() + 400, sizeof(ew::Record)) != 0 ||
                              std::memcmp(r.data() + 400, &records[n], sizeof(ew::Record)) != 0;
    }
    // Emit the ENTIRE predeclared catalog before the existing strict guard.
    // Root may set a private artifact path; failure still remains a test error.
    if (unexpected_catalog) {
        std::ofstream file;
        if (const char *artifact_path = std::getenv("RYPATH_HYBRID_PROBE_DIAGNOSTICS")) {
            file.open(artifact_path);
            if (!file)
                std::cerr << "Cannot open hybrid diagnostic artifact: " << artifact_path
                          << std::endl;
        }
        std::ostream &dump = file.is_open() ? static_cast<std::ostream &>(file) : std::cerr;
        dump << "{\"schema\":1,\"variant\":\"" << label << "\",\"row_words\":512,\"source_rows\":[";
        for (unsigned n = 0; n < count; ++n) {
            if (n != 0)
                dump << ',';
            dump << '[';
            for (unsigned k = 0; k < source[n].size(); ++k) {
                if (k != 0)
                    dump << ',';
                dump << source[n][k];
            }
            dump << ']';
        }
        dump << "],\"arena_records\":[";
        for (unsigned n = 0; n < count; ++n) {
            if (n != 0)
                dump << ',';
            std::array<unsigned, 12> words{};
            std::memcpy(words.data(), &records[n], sizeof(ew::Record));
            dump << '[';
            for (unsigned k = 0; k < words.size(); ++k) {
                if (k != 0)
                    dump << ',';
                dump << words[k];
            }
            dump << ']';
        }
        dump << "]}\n" << std::flush;
    }
    // Declared before the first valid rough-conductor run: GGX may end a
    // canonical path or reject an earlier footprint/certificate. Keep all16
    // identities, permit legal pivot1/2, and require positive E AND existing N.
    // This does not independently prove every earliest rough chart label; the
    // rolling fixed-state/math gates provide separate label evidence.
    unsigned captured = 0;
    for (unsigned n = 0; n < count; ++n) {
        need(source[n][7] == 0, label + " fresh hook disabled");
        need(std::memcmp(source[n].data() + 8, source[n].data() + 400, sizeof(ew::Record)) == 0,
             label + " GPU raw record store/readback mismatch n=" + std::to_string(n));
        need(std::memcmp(source[n].data() + 400, &records[n], sizeof(ew::Record)) == 0,
             label + " CPU arena versus GPU word-readback mismatch n=" + std::to_string(n));
        if (source[n][4]) {
            ++captured;
            need(records[n].tag == 5 && records[n].event_ordinal == source[n][5] &&
                     source[n][5] >= 1 && source[n][5] <= 2 && (conductor || source[n][5] == 1),
                 label + " legal source chart n=" + std::to_string(n) +
                     " captured_pivot=" + std::to_string(source[n][5]) + " captured_tag=" +
                     std::to_string(source[n][9]) + " arena_tag=" + std::to_string(records[n].tag) +
                     " arena_event=" + std::to_string(records[n].event_ordinal) +
                     " steps=" + std::to_string(source[n][2]));
        }
    }
    need(captured > 0, label + " MUST capture at least one real native tag5 (no all-generic pass)");
    if (!conductor)
        need(captured == count,
             "Diffuse predeclared cavity catalog must retain all16 first-pivot captures");
    unsigned positiveE = 0, positiveN = 0;
    for (unsigned slot : {4u, 5u}) {
        const auto full = dispatch(false, slot, false, false);
        const auto prefix = dispatch(false, slot, true, false);
        const auto poison = dispatch(false, slot, true, true);
        for (unsigned n = 0; n < count; ++n) {
            const auto &src = source[n], &a = full[n], &b = prefix[n], &c = poison[n];
            need(std::memcmp(a.data() + 52, b.data() + 52, 6 * sizeof(unsigned)) == 0 &&
                     std::memcmp(b.data() + 52, c.data() + 52, 6 * sizeof(unsigned)) == 0,
                 label + " full/prefix/poison XYZ/query-h exact n=" + std::to_string(n));
            if (!src[4])
                continue;
            need(a[4] == 1 && a[59] == 0 &&
                     std::memcmp(src.data() + 8, a.data() + 8, 12 * sizeof(unsigned)) == 0 &&
                     std::memcmp(a.data() + 8, c.data() + 8, 12 * sizeof(unsigned)) == 0,
                 label + " fixed chart support/record unchanged");
            const unsigned pivot = src[5];
            const unsigned actual_slot = std::min(2u * (pivot + 1u) + (slot & 1u), 6u);
            need(a[33] == actual_slot && b[33] == actual_slot && c[33] == actual_slot,
                 label + " independently grouped legal future E/N request");
            const unsigned expected_mask = (1u << (pivot - 1)) | (1u << pivot);
            need(a[7] == 0 && b[7] == 0 && c[7] == expected_mask,
                 label + " both removed direction words actually poisoned");
            const auto *atom = src.data() + 64 + actual_slot * 48;
            need(atom[0] == 1 && atom[1] == 5, label + " source future atom carries tag5");
            const auto h = scaled(atom[2], atom[3]);
            bool positive = false;
            for (unsigned ch = 0; ch < 3; ++ch) {
                long double expected = 0;
                const long double response = ch == 0 ? .125L : ch == 1 ? .25L : .5L;
                for (unsigned k = 0; k < 4; ++k)
                    expected += static_cast<long double>(f32(atom[6 + k])) * response /
                                f32(atom[10 + k]) / 4 * h;
                close(f32(a[52 + ch]), expected,
                      label + " independently projected source-self atom");
                positive |= expected != 0;
            }
            if (positive) {
                if (actual_slot & 1u)
                    ++positiveN;
                else if (slot == 4)
                    ++positiveE;
            }
        }
    }
    need(positiveE > 0 && positiveN > 0,
         label + " MUST observe positive owned future E and an actually existing N import");
    std::cout << label << " native source catalog16 captured=" << captured
              << " positive_futureE=" << positiveE << " positive_existingN=" << positiveN
              << "; actual forced full/prefix/removed-xyz invariance, independent "
                 "advance/G/projection; discoveryq1 fixture only\n";
}
int run(const std::filesystem::path &path) {
    // Validate BOTH complete fixtures before any Metal setup or first variant.
    // This prevents an expensive diffuse run from hiding an invalid rough setup.
    (void)cavity(false);
    (void)cavity(true);
    auto device = MTLCreateSystemDefaultDevice();
#ifdef RYPATH_ALLOW_NO_METAL_DEVICE
    if (!device)
        return 77;
#endif
    need(device != nil, "No Metal device");
#ifdef RYPATH_ALLOW_NO_METAL_RAY_TRACING
    if (!device.supportsRaytracing)
        return 77;
#endif
    need(device.supportsRaytracing, "Native hybrid runtime probe requires compute ray tracing");
    run_variant(path, false);
    run_variant(path, true);
    return 0;
}
} // namespace
int main(int argc, char **argv) {
    @autoreleasepool {
        try {
            need(argc == 2, "Expected native hybrid probe metallib or --validate-fixtures");
            if (std::string_view(argv[1]) == "--validate-fixtures") {
                const auto diffuse = cavity(false);
                const auto rough = cavity(true);
                need(validate_render_scene(diffuse).empty() && validate_render_scene(rough).empty(),
                     "Both complete scene fixtures must pass CPU validation");
                std::cout << "CPU-only diffuse and rough-.5 cavity fixture validation passed; no "
                             "Metal device or GPU work\n";
                return 0;
            }
            need(!std::string_view(argv[1]).starts_with("--"),
                 "Unknown native hybrid probe option");
            return run(argv[1]);
        } catch (const std::exception &e) {
            std::cerr << e.what() << '\n';
            return 1;
        }
    }
}
