// SPDX-License-Identifier: GPL-3.0-only

#include "metal_backend.hpp"

#include <exception>
#include <iostream>
#include <string_view>

namespace {

std::string_view status_name(rypath::MetalStatus status) {
    switch (status) {
    case rypath::MetalStatus::success:
        return "success";
    case rypath::MetalStatus::not_built:
        return "not_built";
    case rypath::MetalStatus::no_device:
        return "no_device";
    case rypath::MetalStatus::failure:
        return "failure";
    }
    return "failure";
}

const char *yes_no(bool value) { return value ? "yes" : "no"; }

int report_metal(bool smoke_test) {
    const auto result = smoke_test ? rypath::run_metal_smoke_test() : rypath::query_metal_device();
    std::cout << "Metal status: " << status_name(result.status) << '\n';
    if (result.status == rypath::MetalStatus::failure) {
        std::cerr << "rypath: " << result.diagnostic << '\n';
        return 1;
    }
    if (result.status != rypath::MetalStatus::success) {
        std::cout << "Reason: " << result.diagnostic << '\n';
        return smoke_test ? 2 : 0;
    }

    const auto &device = result.device;
    std::cout << "Device selection: system default\n"
              << "Device: " << device.name << '\n'
              << "Registry ID: " << device.registry_id << '\n'
              << "Architecture: " << device.architecture << '\n'
              << "Unified memory: " << yes_no(device.unified_memory) << '\n'
              << "Ray tracing API (compute): " << yes_no(device.compute_ray_tracing) << '\n'
              << "Ray tracing API (render): " << yes_no(device.render_ray_tracing) << '\n'
              << "Hardware ray tracing execution: not verified\n";
    if (smoke_test) {
        std::cout << "Dispatches verified: " << result.dispatch_count << '\n'
                  << "Elements verified: " << result.element_count << '\n'
                  << "Guard elements verified: " << result.guard_count << '\n';
    }
    return 0;
}

} // namespace

int main(int argc, char *argv[]) {
    if (argc == 1 || (argc == 2 && std::string_view{argv[1]} == "--help")) {
        std::cout << "Usage: rypath [--help | --version | --metal-info | --metal-smoke-test]\n";
        return 0;
    }
    if (argc == 2) {
        try {
            if (std::string_view{argv[1]} == "--metal-info") {
                return report_metal(false);
            }
            if (std::string_view{argv[1]} == "--metal-smoke-test") {
                return report_metal(true);
            }
        } catch (const std::exception &error) {
            std::cerr << "rypath: " << error.what() << '\n';
            return 1;
        }
    }
    if (argc == 2 && std::string_view{argv[1]} == "--version") {
        std::cout << "rypath " RYPATH_VERSION "\n";
        return 0;
    }

    std::cerr << "rypath: unsupported arguments\n"
                 "Try 'rypath --help' for usage.\n";
    return 1;
}
