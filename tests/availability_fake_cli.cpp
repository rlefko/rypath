#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string_view>

namespace {

constexpr std::string_view settings = "Resolution: 8x6\nSamples per pixel: 3\nBatch size: 2\n";

int spoof_marker() {
    std::ifstream marker{"marker.txt"};
    if (!marker) {
        return 3;
    }
    std::cout << marker.rdbuf();
    return 1;
}

struct UnavailableRender {
    std::string_view stdout_text;
    std::string_view reason = "No system-default Metal device";
};

int unavailable(const UnavailableRender &render) {
    std::cout << render.stdout_text;
    std::cerr << "rypath: " << render.reason << '\n';
    return 2;
}

int render_png(const std::filesystem::path &output) {
    std::ofstream png{output, std::ios::binary};
    png << "\x89PNG\r\n\x1a\n";
    png.close();
    if (!png) {
        return 3;
    }
    std::cout << "Device: simulated\n"
                 "Committed samples per pixel: 2/3\nCommitted samples per pixel: 3/3\n"
                 "Total samples: 144\nWrote PNG: simulated.png\n";
    return 0;
}

int demo(std::string_view scenario, const std::filesystem::path &output) {
    if (scenario == "demo-marker") {
        return spoof_marker();
    }
    if (scenario == "demo-success") {
        std::cout << settings << "Seed: 0\n";
        return render_png(output);
    }
    if (scenario == "demo-portable" || scenario == "demo-unexpected-not-built") {
        return unavailable(
            {.stdout_text = "Render status: not_built\n", .reason = "Metal backend was not built"});
    }
    if (scenario == "demo-ray-tracing" || scenario == "demo-ray-tracing-wrong-flag") {
        return unavailable(
            {.stdout_text = "Device: simulated\nRender status: no_ray_tracing\n",
             .reason = "System-default Metal device does not support compute ray tracing"});
    }
    if (scenario == "demo-prefixed-status") {
        return unavailable({.stdout_text = "Diagnostic: Render status: no_device\n"});
    }
    if (scenario == "demo-suffixed-status") {
        return unavailable({.stdout_text = "Render status: no_device (unconfirmed)\n"});
    }
    if (scenario == "demo-incomplete-status") {
        return unavailable({.stdout_text = "Render status: no_device"});
    }
    if (scenario == "demo-duplicate-status") {
        return unavailable({.stdout_text = "Render status: no_device\nRender status: no_device\n"});
    }
    if (scenario == "demo-conflicting-status") {
        return unavailable(
            {.stdout_text = "Render status: no_device\nRender status: no_ray_tracing\n"});
    }
    if (scenario == "demo-extra-status") {
        return unavailable({.stdout_text = "Render status: no_device\nRender status: unknown"});
    }
    if (scenario == "demo-missing-status") {
        return unavailable({.stdout_text = "No system-default Metal device\n"});
    }
    if (scenario == "demo-status-in-stderr") {
        std::cerr << "rypath: Render status: no_device\n";
        return 2;
    }
    if (scenario == "demo-wrong-exit") {
        unavailable({.stdout_text = "Render status: no_device\n"});
        return 1;
    }
    if (scenario == "demo-changed-output") {
        std::ofstream{output} << "overwritten";
    }
    if (scenario == "demo-progress") {
        return unavailable(
            {.stdout_text = "Render status: no_device\nCommitted samples per pixel: 2/3\n"});
    }
    if (scenario == "demo-total-samples") {
        return unavailable({.stdout_text = "Total samples: 144\nRender status: no_device\n"});
    }
    if (scenario == "demo-wrong-diagnostic") {
        return unavailable({.stdout_text = "Render status: no_device\n",
                            .reason = "Library initialization failed"});
    }
    if (scenario == "demo-extra-stderr") {
        std::cerr << "Unexpected diagnostic\n";
    }
    return unavailable({.stdout_text = "Render status: no_device\n"});
}

struct PbrtPaths {
    std::filesystem::path input;
    std::filesystem::path output;
};

int pbrt(std::string_view scenario, const PbrtPaths &paths) {
    const auto filename = paths.input.filename();
    if (filename == "unsupported.pbrt") {
        std::cout << "Rendering was not attempted.\n";
        std::cerr << "unsupported ColorSpace\n";
        return 2;
    }
    if (filename == "unsupported-sampler.pbrt") {
        std::cout << "Sampler type \"halton\"\nUnsupported features: 1\n"
                     "Rendering was not attempted.\n";
        return 2;
    }
    if (filename == "unsafe-filter.pbrt") {
        std::cout << "Rendering was not attempted.\n";
        std::cerr << "xradius must be positive and normal in binary32\n";
        return 1;
    }
    if (filename == "missing.pbrt") {
        std::cout << "missing-file\n";
        return 1;
    }
    if (filename == "sobol-depth.pbrt") {
        if (scenario == "pbrt-marker") {
            return spoof_marker();
        }
        std::cout << "Rendering was not attempted.\n";
        std::cerr << "sobol-depth.pbrt:6:1: unsupported maxdepth above 127\n";
        return 2;
    }
    const bool sobol = filename == "sobol.pbrt";
    if (!sobol && filename != "valid.pbrt") {
        return 3;
    }
    std::cout << "Render profile: pbrt-spectral-v1\n"
                 "Pixel filter: triangle (x radius 1, y radius 1.5)\n"
                 "Light sampling: separate environment and uniform triangle area\n";
    if (sobol) {
        std::cout
            << "Sampler: sobol-v1 / owen (PBRT camera variates; rypath transport dimensions)\n"
            << settings << "Seed: -17\n";
        std::cerr
            << "sobol.pbrt:5:1: warning [sobol-non-power-of-two]: requested 3 samples per pixel\n";
    } else {
        std::cout << "Sampler: philox4x32-10-v1 (sequence differs from PBRT)\n"
                  << settings << "Seed: 0\n";
    }
    std::cout << "Finite path maxdepth: 1\n";
    if (scenario == "pbrt-success" || (scenario == "pbrt-independent-success" && !sobol) ||
        (scenario == "pbrt-sobol-success" && sobol)) {
        return render_png(paths.output);
    }
    if (scenario == "pbrt-portable") {
        return unavailable(
            {.stdout_text = "Render status: not_built\n", .reason = "Metal backend was not built"});
    }
    if (scenario == "pbrt-ray-tracing" || (scenario == "pbrt-conflicting-status" && sobol)) {
        return unavailable(
            {.stdout_text = "Render status: no_ray_tracing\n",
             .reason = "System-default Metal device does not support compute ray tracing"});
    }
    if (scenario == "pbrt-total-samples" && sobol) {
        return unavailable({.stdout_text = "Total samples: 144\nRender status: no_device\n"});
    }
    if (scenario == "pbrt-wrong-diagnostic" && sobol) {
        return unavailable({.stdout_text = "Render status: no_device\n",
                            .reason = "Library initialization failed"});
    }
    return unavailable({.stdout_text = "Render status: no_device\n"});
}

} // namespace

int main(int argc, char **argv) try {
    if (argc < 2) {
        return 3;
    }
    const auto scenario = std::filesystem::current_path().filename().string();
    const std::string_view mode{argv[1]};
    if (mode == "--metal-smoke-test" && argc == 2) {
        if (scenario == "metal-marker") {
            return spoof_marker();
        }
        std::cout << "Metal status: no_device\nReason: No system-default Metal device\n";
        return 2;
    }
    if (mode == "--render-demo" && argc >= 6 && std::string_view{argv[2]} == "--output") {
        if (std::string_view{argv[5]} == "0") {
            std::cerr << "rypath: width must be positive\n";
            return 1;
        }
        return demo(scenario, argv[3]);
    }
    if (mode == "--render-pbrt" && argc >= 5 && std::string_view{argv[3]} == "--output") {
        return pbrt(scenario, {.input = argv[2], .output = argv[4]});
    }
    return 3;
} catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 3;
}
