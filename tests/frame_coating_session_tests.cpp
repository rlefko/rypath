// SPDX-License-Identifier: GPL-3.0-only
#include "frame_qualification_identity.hpp"
#include "frame_session_test_access.hpp"
#include "pbrt_render.hpp"
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
using namespace rypath;
void need(bool value, const std::string &why) {
    if (!value)
        throw std::runtime_error(why);
}
void contrast(const RenderScene &scene, FrameConfig c, bool reverse,
              const std::filesystem::path &library) {
    auto other = c;
    c.coating_proposal =
        reverse ? FrameCoatingProposal::shared_diffuse : FrameCoatingProposal::hero_only;
    other.coating_proposal =
        reverse ? FrameCoatingProposal::hero_only : FrameCoatingProposal::shared_diffuse;
    auto pair = FrameSessionTestAccess::build_shared_scene(scene, c, other, library);
    need(pair.status == GeometryStatus::success, pair.diagnostic);
    need(pair.shared_renderer, "One immutable renderer must be shared");
    if (c.estimator == FrameEstimator::area_replay)
        need(pair.independent_history && pair.independent_owned_records,
             "Different sampling laws must own separate history and records");
    bool changed = false;
    for (unsigned sample = 0; sample < 2; ++sample) {
        std::array<FrameCapture, 2> captures;
        for (unsigned arm = 0; arm < 2; ++arm) {
            const auto &config = arm ? other : c;
            auto submitted = pair.sessions[arm]->submit({.sample_index = sample});
            need(submitted.status == GeometryStatus::success && bool(submitted.frame),
                 submitted.diagnostic);
            const auto info = submitted.frame->wait();
            std::string diagnostic;
            need(valid_frame_qualification_identity(config, info, scene.max_depth, &diagnostic),
                 diagnostic);
            captures[arm] = submitted.frame->capture();
            need(captures[arm].status == GeometryStatus::success, captures[arm].diagnostic);
            for (const auto &xyz : captures[arm].raw_xyz)
                for (auto v : xyz)
                    need(std::isfinite(v), "Nonfinite raw transport");
        }
        changed |= captures[0].raw_xyz != captures[1].raw_xyz;
    }
    need(changed, "Coating fixture must exercise distinct admitted proposals");
}
} // namespace
int main(int argc, char **argv) {
    try {
        need(argc == 3, "frame_coating_session_tests SCENE METALLIB");
        const auto device = query_metal_device();
#ifdef RYPATH_ALLOW_NO_METAL_DEVICE
        if (device.status == MetalStatus::no_device)
            return 77;
#endif
        need(device.status == MetalStatus::success, device.diagnostic);
#ifdef RYPATH_ALLOW_NO_METAL_RAY_TRACING
        if (!device.device.compute_ray_tracing)
            return 77;
#endif
        need(device.device.compute_ray_tracing, "Compute ray tracing unavailable");
        auto checked = pbrt::check(argv[1]);
        need(checked.exit_code() == 0, pbrt::report(checked));
        auto translated = pbrt::translate(checked, {argv[1]}, {}, SpectralTransportSelection::hero);
        need(translated.exit_code() == 0 && translated.scene.has_value(), "Fixture translation");
        auto scene = std::move(*translated.scene);
        scene.integrator = IntegratorKind::path;
        scene.max_component_value = std::numeric_limits<double>::infinity();
        FrameConfig c;
        c.width = c.height = 8;
        c.seed = 702211;
        contrast(scene, c, false, argv[2]);
        contrast(scene, c, true, argv[2]);
        c.estimator = FrameEstimator::area_replay;
        c.area_path_domain = FrameAreaPathDomain::enhanced;
        c.contribution_replay = FrameContributionReplay::prefix;
        c.mutation_steps = 0;
        contrast(scene, c, false, argv[2]);
        c.area_reuse = FrameAreaReuse::fresh;
        contrast(scene, c, true, argv[2]);
        std::cout << "4 shared-renderer coating session contrasts passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
