// SPDX-License-Identifier: GPL-3.0-only
#include "metal_frame_access.hpp"
#include "metal_reconstruction_access.hpp"
#include "metalfx_conditioning.hpp"
#include "metalfx_denoiser.hpp"
#import <Foundation/Foundation.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string_view>
namespace {
using namespace rypath;
void need(bool ok, const std::string &why) {
    if (!ok)
        throw std::runtime_error(why);
}
int run(bool strict) {
    auto device = MTLCreateSystemDefaultDevice();
    auto probe = MetalFXDenoiser::create(device, {128, 128, 128, 128});
    if (!probe.denoiser) {
        std::cout << probe.diagnostic << '\n';
        return strict ? 1 : 77;
    }
    probe.denoiser.reset();
    RenderScene scene;
    scene.model = RenderModel::pbrt;
    scene.environment.fill(.5f);
    scene.max_depth = 1;
    scene.materials = {{.kind = SpectralMaterialKind::constant, .constant = .5f}};
    auto owner = build_frame_session(scene, {.width = 128, .height = 128, .seed = 17});
    need(owner.session != nullptr, owner.diagnostic);
    auto raw = owner.session->submit({});
    need(raw.frame != nullptr, raw.diagnostic);
    need(raw.frame->wait().status == FrameStatus::complete, "Conditioning source completion");
    const auto before = raw.frame->capture();
    need(before.status == GeometryStatus::success, before.diagnostic);
    auto fresh = build_reconstruction_stream(
        *raw.frame, {.mode = ReconstructionMode::metalfx, .capture_diagnostics = true});
    need(fresh.stream != nullptr, fresh.diagnostic);
    auto output = fresh.stream->submit(*raw.frame);
    need(output.frame != nullptr, output.diagnostic);
    need(output.frame->wait().status == ReconstructionStatus::complete, "Fresh guide completion");
    auto snapshot = MetalReconstructionStreamProbe::capture_diagnostics(*fresh.stream);
    need(snapshot.diagnostic.empty(), snapshot.diagnostic);
    auto lease = MetalFrameAccess::acquire(*raw.frame);
    need(lease.lease != nullptr, lease.diagnostic);
    std::string diagnostic;
    auto comparator = MetalFXConditioningComparator::create(*lease.lease, diagnostic);
    need(comparator != nullptr, diagnostic);
    std::vector<std::array<float, 3>> mean(128 * 128, {-.25f, .5f, 2.f});
    auto rejected = comparator->run(*lease.lease, snapshot, mean, 0, false);
    need(rejected.status == GeometryStatus::failure &&
             rejected.diagnostic.find("exposure") != std::string::npos,
         "Invalid conditioning exposure accepted");
    const auto result = comparator->run(*lease.lease, snapshot, mean, .25f, false);
    need(result.status == GeometryStatus::success, result.diagnostic);
    need(result.history_reset && result.exposure == .25f && result.linear_rgb.size() == mean.size(),
         "Conditioning reset/exposure/output contract");
    need(result.audit.negative_pixels == mean.size() && result.audit.high_pixels == 0,
         "Independent per-pixel clipping counts");
    const std::array<double, 3> signed_expected{-.25, .5, 2}, rectified_expected{0, .5, 2};
    for (unsigned c = 0; c < 3; ++c) {
        need(result.audit.signed_mean_rgb[c] == signed_expected[c] &&
                 result.audit.rectified_mean_rgb[c] == rectified_expected[c] &&
                 result.audit.input_half_mean_rgb[c] == rectified_expected[c],
             "Independent signed/rectified/half mean oracle");
    }
    auto invalid = snapshot;
    const auto index = std::find_if(invalid.textures.begin(), invalid.textures.end(),
                                    [](const auto &t) { return t.name == "input.depth"; });
    need(index != invalid.textures.end(), "Missing depth fixture");
    invalid.textures.push_back(*index);
    rejected = comparator->run(*lease.lease, invalid, mean, 1, false);
    need(rejected.status == GeometryStatus::failure &&
             rejected.diagnostic.find("Duplicate") != std::string::npos,
         "Duplicate conditioning guide accepted");
    invalid = snapshot;
    invalid.info.source.sample_index += 1;
    rejected = comparator->run(*lease.lease, invalid, mean, 1, false);
    need(rejected.status == GeometryStatus::failure, "Mismatched conditioning source accepted");
    invalid = snapshot;
    for (auto &t : invalid.textures)
        if (t.name == "input.roughness") {
            const std::uint16_t infinity = 0x7c00;
            std::memcpy(t.bytes.data(), &infinity, 2);
        }
    rejected = comparator->run(*lease.lease, invalid, mean, 1, false);
    need(rejected.status == GeometryStatus::failure &&
             rejected.diagnostic.find("Nonfinite") != std::string::npos,
         "Nonfinite conditioning guide accepted");
    need(raw.frame->capture().raw_xyz == before.raw_xyz, "Conditioning modified raw accumulation");
    std::cout << "Conditioning API/audit contracts passed; quality and performance unqualified\n";
    return 0;
}
} // namespace
int main(int argc, char **argv) {
    if (argc > 2 || (argc == 2 && std::string_view(argv[1]) != "--require-supported"))
        return 1;
    @autoreleasepool {
        try {
            return run(argc == 2);
        } catch (const std::exception &e) {
            std::cerr << e.what() << '\n';
            return 1;
        }
    }
}
