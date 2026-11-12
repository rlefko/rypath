// SPDX-License-Identifier: GPL-3.0-only

#include "metal_backend.hpp"

#include <exception>
#include <filesystem>
#include <iostream>
#include <string_view>

namespace {

using rypath::MetalResult;
using rypath::MetalStatus;

bool no_work_verified(const MetalResult &result) {
    return result.dispatch_count == 0 && result.element_count == 0 && result.guard_count == 0;
}

int fail(std::string_view message, const MetalResult &result) {
    std::cerr << message << '\n'
              << "Diagnostic: " << result.diagnostic << '\n'
              << "Dispatches: " << result.dispatch_count << ", elements: " << result.element_count
              << ", guard elements: " << result.guard_count << '\n';
    return 1;
}

int check_stub() {
    const auto device = rypath::query_metal_device();
    if (device.status != MetalStatus::not_built || device.diagnostic.empty() ||
        !no_work_verified(device)) {
        return fail("Expected the stub device query to report not_built", device);
    }
    const auto smoke = rypath::run_metal_smoke_test();
    if (smoke.status != MetalStatus::not_built || smoke.diagnostic.empty() ||
        !no_work_verified(smoke)) {
        return fail("Expected the stub smoke test to report not_built", smoke);
    }
    return 0;
}

int check_smoke(const std::filesystem::path &library_path) {
    const auto result = rypath::run_metal_smoke_test(library_path);
    if (result.status == MetalStatus::no_device) {
        if (result.diagnostic.empty() || !no_work_verified(result)) {
            return fail("Unexpected no_device result", result);
        }
        std::cerr << result.diagnostic << '\n';
        // CTest treats this as a failure unless RYPATH_ALLOW_NO_METAL_DEVICE is enabled.
        return 77;
    }
    if (!library_path.empty()) {
        if (result.status != MetalStatus::failure || !no_work_verified(result) ||
            result.diagnostic.find("Metal library load failed") == std::string::npos ||
            result.diagnostic.find(library_path.string()) == std::string::npos) {
            return fail("Expected a Metal library load error with the requested path", result);
        }
        return 0;
    }
    if (result.status != MetalStatus::success || result.dispatch_count != 2 ||
        result.element_count != 258 || result.guard_count == 0) {
        return fail("Expected 2 verified Metal dispatches, 258 elements, and checked guards",
                    result);
    }
    return 0;
}

} // namespace

int main(int argc, char *argv[]) try {
    if (argc == 2 && std::string_view{argv[1]} == "stub") {
        return check_stub();
    }
    if (argc == 2 && std::string_view{argv[1]} == "smoke") {
        return check_smoke({});
    }
    if (argc == 3) {
        const std::string_view mode{argv[1]};
        if (mode == "missing-library" || mode == "invalid-library") {
            const std::filesystem::path path{argv[2]};
            if (std::filesystem::exists(path) != (mode == "invalid-library")) {
                std::cerr << "Unexpected Metal library fixture for " << mode << ": " << path
                          << '\n';
                return 1;
            }
            return check_smoke(path);
        }
    }
    std::cerr << "Expected stub, smoke, missing-library, or invalid-library\n";
    return 1;
} catch (const std::exception &error) {
    std::cerr << "Metal test failed: " << error.what() << '\n';
    return 1;
}
