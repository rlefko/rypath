// SPDX-License-Identifier: GPL-3.0-only
#include "frame_qualification_identity.hpp"
#include <iostream>
#include <stdexcept>

namespace {
using namespace rypath;
unsigned checks = 0;
void check(bool condition, const char *label) {
    ++checks;
    if (!condition)
        throw std::runtime_error(label);
}
void parameters() {
    FrameConfig c;
    check(c.coating_proposal == FrameCoatingProposal::hero_only, "legacy default");
    check(parse_frame_coating_proposal("hero-only") == c.coating_proposal, "parse default");
    check(parse_frame_coating_proposal("shared-diffuse") == FrameCoatingProposal::shared_diffuse,
          "parse opt-in");
    check(!parse_frame_coating_proposal("shared"), "reject ambiguous policy");
    c.coating_proposal = FrameCoatingProposal::shared_diffuse;
    check(validate_frame_coating_proposal(c).empty(), "independent admitted");
    auto rejected = [&](FrameConfig x) {
        check(!validate_frame_coating_proposal(x).empty(), "unsupported configuration rejected");
        x.coating_proposal = FrameCoatingProposal::hero_only;
        check(validate_frame_coating_proposal(x).empty(), "legacy scope unchanged");
    };
    auto x = c;
    x.execution = FrameExecution::megakernel;
    rejected(x);
    for (auto layout : {FramePacketLayout::hot_cold, FramePacketLayout::split_trace}) {
        x = c;
        x.packet_layout = layout;
        rejected(x);
    }
    for (auto count : {2u, 4u}) {
        x = c;
        x.packet_events = count;
        rejected(x);
    }
    c.estimator = FrameEstimator::area_replay;
    rejected(c);
    c.area_path_domain = FrameAreaPathDomain::contribution;
    rejected(c);
    c.area_path_domain = FrameAreaPathDomain::enhanced;
    for (auto reuse : {FrameAreaReuse::full, FrameAreaReuse::fresh})
        for (auto replay : {FrameContributionReplay::full, FrameContributionReplay::prefix}) {
            c.area_reuse = reuse;
            c.contribution_replay = replay;
            check(validate_frame_coating_proposal(c).empty(), "Enhanced exact evaluator admitted");
        }
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    c.coating_proposal = static_cast<FrameCoatingProposal>(99);
    check(!validate_frame_coating_proposal(c).empty(), "invalid enum rejected");
}
void receipts() {
    FrameConfig c;
    c.width = c.height = 1;
    FrameInfo f;
    f.status = FrameStatus::complete;
    f.width = f.height = 1;
    f.completed_samples = f.completed_evaluations = f.logical_evaluations = f.fresh_candidates = 1;
    check(valid_frame_qualification_identity(c, f, 5), "default receipt admitted");
    c.coating_proposal = FrameCoatingProposal::shared_diffuse;
    check(!valid_frame_qualification_identity(c, f, 5), "silent fallback rejected");
    f.coating_proposal_requested = f.coating_proposal = c.coating_proposal;
    check(!valid_frame_qualification_identity(c, f, 5), "unverified declaration rejected");
    f.coating_proposal_capability_verified = true;
    f.coating_proposal_contract = shared_coating_contract::version;
    check(valid_frame_qualification_identity(c, f, 5), "verified receipt admitted");
    ++f.coating_proposal_contract;
    check(!valid_frame_qualification_identity(c, f, 5), "unknown version rejected");
    f.coating_proposal_contract = shared_coating_contract::version;
    c.coating_proposal = FrameCoatingProposal::hero_only;
    check(!valid_frame_qualification_identity(c, f, 5), "unrequested changed law rejected");
}
void vacuum_scenes() {
    RenderScene scene;
    scene.model = RenderModel::pbrt;
    scene.spectral_transport = SpectralTransport::hero;
    scene.integrator = IntegratorKind::volpath;
    scene.volume_transport = true;
    FrameConfig config;
    config.coating_proposal = FrameCoatingProposal::shared_diffuse;
    const auto admitted = [&](const RenderScene &s) {
        return has_proven_vacuum_media(s) && validate_frame_coating_scene(s, config).empty();
    };
    check(admitted(scene), "vacuum volpath mode admitted without flag edits");
    scene.instance_media = {{no_medium, no_medium}, {no_medium, no_medium}};
    check(admitted(scene), "translated empty instance assignments admitted");
    scene.media.emplace_back();
    scene.instance_media[0] = {0, 0};
    check(admitted(scene), "equal sides retain vacuum with zero coefficients");
    auto bad = scene;
    bad.camera_medium = 0;
    check(!admitted(bad), "camera medium assignment rejected");
    bad = scene;
    bad.instance_media[0] = {0, no_medium};
    check(!admitted(bad), "reachable medium change rejected");
    bad = scene;
    bad.instance_media[0] = {7, 7};
    check(!admitted(bad), "invalid equal medium indices rejected");
    for (unsigned component = 0; component < 3; ++component) {
        bad = scene;
        if (component == 0)
            bad.media[0].sigma_a[0] = .1f;
        else if (component == 1)
            bad.media[0].sigma_s[470] = .1f;
        else
            bad.media[0].emission[42] = .1f;
        check(!admitted(bad), "declared active medium covers unknown light endpoints");
        auto legacy = config;
        legacy.coating_proposal = FrameCoatingProposal::hero_only;
        check(validate_frame_coating_scene(bad, legacy).empty(), "legacy admission unchanged");
        check(!select_spectral_transport(bad, SpectralTransportSelection::hero).empty(),
              "existing explicit hero medium proof unchanged");
    }
    scene.camera_medium = 0;
    scene.instance_media[0] = {0, no_medium};
    check(select_spectral_transport(scene, SpectralTransportSelection::hero).empty(),
          "zero-coefficient assignments retain explicit selection behavior");
    check(scene.volume_transport && scene.camera_medium == no_medium &&
              scene.instance_media.empty() && admitted(scene),
          "selection normalizes vacuum assignments without changing volpath mode");
}
} // namespace
int main() {
    try {
        parameters();
        receipts();
        vacuum_scenes();
        std::cout << checks << " frame coating policy checks passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
