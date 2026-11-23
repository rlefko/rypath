// SPDX-License-Identifier: GPL-3.0-only
#include "frame_session.hpp"
#include <array>
#include <iostream>
#include <stdexcept>
using namespace rypath;
int main() try {
    constexpr std::array policies{
        AccelerationBuildPolicy::default_build, AccelerationBuildPolicy::fast_blas,
        AccelerationBuildPolicy::fast_tlas, AccelerationBuildPolicy::fast_all};
    for (unsigned i = 0; i < policies.size(); ++i) {
        const auto p = policies[i];
        if (!valid_acceleration_build_policy(p) ||
            parse_acceleration_build_policy(acceleration_build_policy_name(p)) != p ||
            acceleration_fast_blas(p) != (i == 1 || i == 3) ||
            acceleration_fast_tlas(p) != (i == 2 || i == 3))
            throw std::runtime_error("Acceleration build policy mapping");
    }
    if (parse_acceleration_build_policy("fast") ||
        // Intentionally exercise rejection of a value outside the declared enum cases.
        // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
        valid_acceleration_build_policy(static_cast<AccelerationBuildPolicy>(99)) ||
        FrameConfig{}.acceleration_build_policy != policies[0] ||
        RenderBuildOptions{}.acceleration_build_policy != policies[0] ||
        GeometryBuildOptions{}.acceleration_build_policy != policies[0] ||
        GeometryBuildOptions{}.intersection_profile != IntersectionProfile::compensated ||
        RenderBuildOptions{}.intersection_profile != IntersectionProfile::compensated)
        throw std::runtime_error("Acceleration policy defaults/invalid value");
    std::cout << "Acceleration build policy portable checks passed\n";
    return 0;
} catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
}
