// SPDX-License-Identifier: GPL-3.0-only
#include "enhanced_reprojection_shared.h"

#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
namespace r = rypath_enhanced_reprojection;
void need(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
r::Projection point(float x, float y, float depth) { return {x, y, depth, 1}; }
r::Donor choose(r::Config c, r::Word owner, r::Projection current, r::Projection previous,
                r::Projection foreground_old, r::Projection foreground_now,
                r::Word ordinary_compatible = 1, r::Word dual_compatible = 1) {
    return r::resolve(c, owner,
                      r::propose(c, owner, current, previous, foreground_old, foreground_now,
                                 ordinary_compatible),
                      dual_compatible);
}
void ordinary_and_support() {
    const r::Config config{16, 4, 1, 0};
    constexpr r::Word owner = 19; // (3,1), center(3.5,1.5).
    const auto current = point(3.5f, 1.5f, .25f);
    const auto previous = point(4.99f, 1.1f, .25f);
    const auto foreground = point(4.5f, 1.5f, .25f);
    const auto lookup = r::ordinary_lookup(config, owner, current, previous);
    need(lookup.status == r::Status::ok && lookup.supported && lookup.pixel == 20,
         "Ordinary lookup must floor the current point's old projection");
    const auto ordinary = choose(config, owner, current, previous, foreground, {}, 1, 0);
    need(ordinary.status == r::Status::ok && ordinary.present && ordinary.supported &&
             ordinary.route == r::Route::ordinary && ordinary.pixel == 20,
         "No-occlusion ordinary donor keeps its stable lookup, ignoring unused forward O");
    need(ordinary.pixel != 21, "Negative control: rounding old x4.99 to5 is wrong");
    const auto mismatch = choose(config, owner, current, previous, foreground, {}, 0);
    need(mismatch.status == r::Status::ok && mismatch.present && !mismatch.supported &&
             mismatch.pixel == owner && mismatch.reason == r::Reason::geometry_mismatch,
         "Unsupported correspondence is a present same-pixel domain, not a null draw");
    auto cold = config;
    cold.history_valid = 0;
    const r::Projection poison{std::numeric_limits<float>::quiet_NaN(), 0, 0, 99};
    const auto absent = choose(cold, owner, poison, poison, poison, poison, 99, 99);
    need(absent.status == r::Status::ok && !absent.present && !absent.supported &&
             absent.pixel == r::invalid_pixel && absent.reason == r::Reason::no_history,
         "No history is structural absence and must not inspect unavailable guides");
}
void parallax_oracle() {
    // Independent pinhole geometry: old C=0, new C=.375, f=16, principal x=16.
    // A background's ordinary motion is f*dC/Zb; the dual displacement is
    // f*dC/Zf. No implementation projection or donor helper supplies this oracle.
    const r::Config config{32, 8, 1, 0};
    constexpr double camera = .375, focal = 16, principal = 16;
    for (unsigned i = 0; i < 5; ++i) {
        const unsigned x = 5 + i;
        const r::Word owner = 2 * config.width + x;
        const double center = x + .5, background_Z = 8 + 2 * i, foreground_Z = 2 + .5 * i;
        const double world_X = camera + (center - principal) * background_Z / focal;
        const double old_x = principal + focal * world_X / background_Z;
        const auto k = static_cast<unsigned>(std::floor(old_x));
        const double foreground_X = (double(k) + .5 - principal) * foreground_Z / focal;
        const double foreground_now = principal + focal * (foreground_X - camera) / foreground_Z;
        const auto previous = point(float(old_x), 2.5f, float(1 / background_Z));
        const auto old_O = point(float(double(k) + .5), 2.5f, float(1 / foreground_Z));
        const auto new_O = point(float(foreground_now), 2.5f, float(1 / foreground_Z));
        const auto selected =
            choose(config, owner, point(float(center), 2.5f, float(1 / background_Z)), previous,
                   old_O, new_O, 0, 1);
        const auto expected_x =
            static_cast<unsigned>(std::floor(center + focal * camera / foreground_Z));
        need(selected.status == r::Status::ok && selected.present && selected.supported &&
                 selected.route == r::Route::dual &&
                 selected.pixel == 2 * config.width + expected_x,
             "Dual motion must follow foreground parallax across nonuniform depths");
        if (i == 0)
            need(selected.pixel != 2 * config.width + k,
                 "Negative control: using background X instead of old foreground O loses dual "
                 "motion");
    }
}
void quantization_occlusion_and_edges() {
    const r::Config config{16, 4, 1, 0};
    constexpr r::Word owner = 24; // (8,1).
    const auto current = point(8.5f, 1.5f, .25f), previous = point(9.1f, 1.5f, .25f);
    const auto old_O = point(9.5f, 1.5f, .5f), new_O = point(7.8f, 1.5f, .5f);
    const auto proposal = r::propose(config, owner, current, previous, old_O, new_O, 0);
    need(proposal.status == r::Status::ok && proposal.route == r::Route::dual &&
             proposal.pixel == 26,
         "Subpixel dual donor must anchor y at the sampled old foreground center");
    const auto wrong_anchor = std::floor(double(previous.x) + 8.5 - double(new_O.x));
    need(wrong_anchor == 9 && proposal.pixel != 16 + unsigned(wrong_anchor),
         "Negative control: substituting y0 for old foreground y changes the donor");
    const auto two_axes =
        choose({16, 16, 1, 0}, 136, point(8.5f, 8.5f, .25f), point(9.1f, 7.9f, .25f),
               point(9.5f, 7.5f, .5f), point(7.8f, 10.1f, .5f), 0);
    need(two_axes.supported && two_axes.route == r::Route::dual && two_axes.pixel == 90,
         "Nonuniform two-axis motion must point sample both extrapolated coordinates");
    const auto rejected = r::resolve(config, owner, proposal, 0);
    need(rejected.present && !rejected.supported && rejected.pixel == owner &&
             rejected.reason == r::Reason::dual_geometry_rejected,
         "Alternative geometry rejection must use explicit same-pixel fallback");
    auto tolerant = config;
    tolerant.depth_tolerance = .25f;
    const auto tied = choose(tolerant, owner, current, previous, old_O, new_O);
    need(tied.route == r::Route::ordinary,
         "Depth separation equal to the declared tolerance is not occlusion");
    auto behind = old_O;
    behind.depth = .125f;
    const auto back = choose(config, owner, current, previous, behind, new_O);
    need(!back.supported && back.reason == r::Reason::depth_mismatch,
         "A surface behind the predicted point is not a foreground blocker");
    const auto wrong_pixel =
        choose(config, owner, current, previous, point(8.5f, 1.5f, .5f), new_O);
    need(wrong_pixel.reason == r::Reason::foreground_anchor_mismatch && !wrong_pixel.supported,
         "Foreground point must belong to the exact point-sampled old pixel");
    // Forward O can be off the NEW raster while the OLD candidate is in bounds.
    const auto offscreen_O = choose(config, 19, point(3.5f, 1.5f, .25f), point(4.7f, 1.5f, .25f),
                                    point(4.5f, 1.5f, .5f), point(-2.f, 1.5f, .5f), 0);
    need(offscreen_O.route == r::Route::dual && offscreen_O.pixel == 26 && offscreen_O.supported,
         "Offscreen forward blocker projection must not discard a usable alternate old donor");
    for (float z : {-20.f, 40.f}) {
        const auto edge = choose(config, owner, current, previous, old_O, point(z, 1.5f, .5f), 0);
        need(edge.status == r::Status::ok && !edge.supported && edge.present &&
                 edge.pixel == owner && edge.reason == r::Reason::offscreen,
             "Offscreen dual donors must fall back rather than clamp to an image edge");
    }
}
void invalid_inputs() {
    const r::Config config{16, 4, 1, 0};
    constexpr r::Word owner = 19;
    const auto current = point(3.5f, 1.5f, .25f), previous = point(4.5f, 1.5f, .25f);
    for (const auto bad :
         {point(-.01f, 1.5f, .25f), point(16.f, 1.5f, .25f), point(4.5f, 4.f, .25f),
          point(4.5f, 1.5f, 0), point(4.5f, 1.5f, -1), point(4.5f, 1.5f, 2),
          point(std::numeric_limits<float>::infinity(), 1.5f, .25f),
          point(4.5f, 1.5f, std::numeric_limits<float>::quiet_NaN()), r::Projection{0, 0, 0, 0}}) {
        const auto value = choose(config, owner, current, bad, previous, {}, 1);
        need(value.status == r::Status::ok && !value.supported && value.present &&
                 value.pixel == owner,
             "Unavailable/nonfinite/sky/near/offscreen geometry must fall back safely");
    }
    const auto mismatch = choose(config, owner, point(4.5f, 1.5f, .25f), previous, previous, {});
    need(mismatch.reason == r::Reason::owner_mismatch && !mismatch.supported,
         "Current projection sanity check must never move the output owner");
    auto malformed = previous;
    malformed.valid = 2;
    need(choose(config, owner, current, malformed, previous, {}).status == r::Status::invalid_input,
         "Malformed projection flags fail explicitly");
    need(choose(config, owner, current, previous, previous, {}, 2).status ==
             r::Status::invalid_input,
         "Malformed compatibility flags fail explicitly");
    for (auto bad :
         {r::Config{0, 4, 1, 0}, r::Config{16, 0, 1, 0}, r::Config{16, 4, 2, 0},
          r::Config{r::maximum_dimension + 1, 1, 1, 0},
          r::Config{r::maximum_dimension, r::maximum_dimension, 1, 0}, r::Config{16, 4, 1, -.1f},
          r::Config{16, 4, 1, std::numeric_limits<float>::quiet_NaN()},
          r::Config{16, 4, 1, std::numeric_limits<float>::infinity()},
          r::Config{16, 4, 1, std::numeric_limits<float>::denorm_min()}})
        need(choose(bad, owner, current, previous, previous, {}).status ==
                 r::Status::invalid_config,
             "Invalid dimensions/history/tolerance must not produce an admitted source");
    need(choose(config, 64, current, previous, previous, {}).status == r::Status::invalid_config,
         "Owner outside pixel domain must fail before flattening");
    const r::Config largest{r::maximum_dimension, 1, 1, 0};
    const auto last = point(float(r::maximum_dimension - 1) + .5f, .5f, .5f);
    const auto last_donor = choose(largest, r::maximum_dimension - 1, last, last, last, {});
    need(last_donor.supported && last_donor.pixel == r::maximum_dimension - 1,
         "Largest exactly represented half-pixel center must remain supported");
    auto foreign = r::propose(config, owner, current, previous, previous, {}, 1);
    foreign.owner = 0;
    need(r::resolve(config, owner, foreign, 1).status == r::Status::invalid_input,
         "A proposal from another output owner cannot be consumed");
}
} // namespace
int main() try {
    ordinary_and_support();
    parallax_oracle();
    quantization_occlusion_and_edges();
    invalid_inputs();
    std::cout << "Enhanced camera-only dual-motion coordinate and fallback checks passed\n";
} catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
}
