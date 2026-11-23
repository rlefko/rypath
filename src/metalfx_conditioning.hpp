// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "geometry.hpp"
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
namespace rypath {
class MetalFrameLease;
struct MetalReconstructionSnapshot;
struct MetalFXConditioningAudit {
    std::array<double, 3> signed_mean_rgb{}, rectified_mean_rgb{}, input_half_mean_rgb{};
    std::uint64_t negative_pixels = 0, high_pixels = 0;
};
struct MetalFXConditioningResult {
    GeometryStatus status = GeometryStatus::failure;
    std::string diagnostic;
    std::vector<std::array<float, 3>> linear_rgb;
    MetalFXConditioningAudit audit;
    bool history_reset = true, gpu_time_valid = false;
    float exposure = 1; // Actual half-float hint; pixels remain unexposed.
    double wall_seconds = 0, gpu_seconds = 0;
};
// Diagnostic-only synchronous comparator. The caller supplies the pre-spatial
// signed temporal mean and fresh MetalFX guides from the SAME completed frame,
// serially and in matching order. Metadata checks cannot establish scene/camera
// identity beyond frame dimensions/sample/epoch. near_plane must match the guide
// stream. No new transport frame or spatial filtering is performed here.
//
// This owns independent MetalFX history in addition to upstream signed temporal
// history: the double temporal filtering can add lag/ghosting. Signed RGB is
// rectified/half-quantized only at this API boundary; this is NOT a signed-fidelity
// repair. CPU upload/readback costs are included in wall_seconds; gpu_seconds
// covers upload+MetalFX+readback commands, excluding upstream temporal work and
// fresh-guide capture. Add those costs for an end-to-end comparison. No production
// default or performance/quality qualification follows from this adapter.
class MetalFXConditioningComparator {
  public:
    static std::unique_ptr<MetalFXConditioningComparator>
    create(MetalFrameLease &, std::string &diagnostic, double near_plane = .001);
    ~MetalFXConditioningComparator();
    MetalFXConditioningResult run(MetalFrameLease &, const MetalReconstructionSnapshot &,
                                  const std::vector<std::array<float, 3>> &signed_mean,
                                  float exposure, bool reset);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    explicit MetalFXConditioningComparator(std::unique_ptr<Impl>);
};
} // namespace rypath
