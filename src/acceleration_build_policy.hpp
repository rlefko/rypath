// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
namespace rypath {
enum class AccelerationBuildPolicy { default_build, fast_blas, fast_tlas, fast_all };
constexpr bool valid_acceleration_build_policy(AccelerationBuildPolicy policy) {
    return policy == AccelerationBuildPolicy::default_build ||
           policy == AccelerationBuildPolicy::fast_blas ||
           policy == AccelerationBuildPolicy::fast_tlas ||
           policy == AccelerationBuildPolicy::fast_all;
}
constexpr bool acceleration_fast_blas(AccelerationBuildPolicy policy) {
    return policy == AccelerationBuildPolicy::fast_blas ||
           policy == AccelerationBuildPolicy::fast_all;
}
constexpr bool acceleration_fast_tlas(AccelerationBuildPolicy policy) {
    return policy == AccelerationBuildPolicy::fast_tlas ||
           policy == AccelerationBuildPolicy::fast_all;
}
constexpr std::string_view acceleration_build_policy_name(AccelerationBuildPolicy policy) {
    switch (policy) {
    case AccelerationBuildPolicy::default_build:
        return "default";
    case AccelerationBuildPolicy::fast_blas:
        return "fast-blas";
    case AccelerationBuildPolicy::fast_tlas:
        return "fast-tlas";
    case AccelerationBuildPolicy::fast_all:
        return "fast-all";
    }
    return "invalid";
}
constexpr std::optional<AccelerationBuildPolicy>
parse_acceleration_build_policy(std::string_view name) {
    for (const auto policy :
         {AccelerationBuildPolicy::default_build, AccelerationBuildPolicy::fast_blas,
          AccelerationBuildPolicy::fast_tlas, AccelerationBuildPolicy::fast_all})
        if (acceleration_build_policy_name(policy) == name)
            return policy;
    return {};
}
struct AccelerationBuildInfo {
    AccelerationBuildPolicy requested = AccelerationBuildPolicy::default_build;
    std::optional<AccelerationBuildPolicy> effective;
    // Absent until capability/profile validation has been reached.
    std::optional<bool> supported;
    std::string diagnostic;
    // Sum of host wall intervals from AS sizing through successful GPU build
    // completion, including allocation and waits; excludes geometry preparation.
    double build_cpu_wall_seconds = 0;
    // Actual retained MTLAccelerationStructure.size values on success, zero on
    // failed creation. Never estimates/RSS. Counts also publish only on success.
    std::uint64_t blas_bytes = 0, tlas_bytes = 0;
    std::uint64_t blas_count = 0, tlas_count = 0, fast_blas_count = 0, fast_tlas_count = 0;
    // Device-wide Metal allocations at successful session creation, not RSS or
    // memory exclusively owned by this session. Absent before that point.
    std::optional<std::uint64_t> device_allocated_bytes;
};
} // namespace rypath
