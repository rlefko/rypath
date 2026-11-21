// SPDX-License-Identifier: GPL-3.0-only
#include "render_driver.hpp"
#include "render_timings.hpp"
#include <cmath>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <unistd.h>

namespace {
using namespace rypath;
std::uint64_t tick = 0, clock_calls = 0;
std::uint64_t clock_value() {
    ++clock_calls;
    return tick;
}
void require(bool value, std::string_view message) {
    if (!value)
        throw std::runtime_error(std::string(message));
}
std::string read(const std::filesystem::path &path) {
    std::ifstream input(path);
    return {std::istreambuf_iterator<char>(input), {}};
}
const auto &phase(const RenderTimings &t, RenderTimingPhase p) {
    return t.phases()[static_cast<std::size_t>(p)];
}
void accounting() {
    tick = clock_calls = 0;
    { RenderTimingScope disabled(nullptr, RenderTimingPhase::command); }
    require(clock_calls == 0, "Disabled scope must not read collector clock");
    RenderTimings t(clock_value);
    {
        RenderTimingScope parent(&t, RenderTimingPhase::command);
        tick = 5;
        {
            RenderTimingScope child(&t, RenderTimingPhase::pbrt_check);
            tick = 12;
        }
        tick = 20;
        t.begin_destruction();
        tick = 25;
    }
    tick = 30;
    t.finish();
    require(t.valid(), "Nested timing accounting valid");
    require(phase(t, RenderTimingPhase::command).inclusive_ns == 20 &&
                phase(t, RenderTimingPhase::command).exclusive_ns == 13 &&
                phase(t, RenderTimingPhase::pbrt_check).exclusive_ns == 7 &&
                phase(t, RenderTimingPhase::destruction).exclusive_ns == 5,
            "Nested durations remain exclusive through teardown transition");
    require(t.json(0, "build").find("\"unscoped_ns\":5") != std::string::npos,
            "Clock time outside scopes remains explicit");
    t.gpu_command(true, 1, 1.25);
    t.gpu_command(true, 0, 0);
    t.gpu_command(false, 1, 2);
    t.gpu_command(true, 2, std::numeric_limits<double>::infinity());
    require(t.gpu_valid_count() == 1 && t.gpu_invalid_count() == 3 &&
                t.gpu_nanoseconds() == 250000000,
            "GPU missing/error timestamps are visible, not zero durations");
    t.failure("path \"quoted\"\n");
    t.provenance("scene", "test");
    const auto json = t.json(2, "compiler");
    require(json.find("\"status\":\"failed\"") != std::string::npos &&
                json.find("\\\"quoted\\\"\\u000a") != std::string::npos,
            "Failure JSON retains status and escapes diagnostic strings");
    RenderTimings invalid(clock_value);
    const auto outer = invalid.begin(RenderTimingPhase::command);
    (void)invalid.begin(RenderTimingPhase::pbrt_check);
    invalid.end(outer);
    invalid.finish();
    require(!invalid.valid(), "Out-of-order scope closure is reported");
}
struct Source {
    unsigned calls = 0, fail_at = 0;
    detail::RenderSampleSource get() {
        return {[] {
                    RenderBuildResult b;
                    b.status = GeometryStatus::success;
                    return b;
                },
                [this](const RenderRequest &r) {
                    ++calls;
                    tick += 7;
                    RenderSamplesResult result;
                    if (calls == fail_at) {
                        result.diagnostic = "injected timing test failure";
                        return result;
                    }
                    result.status = GeometryStatus::success;
                    for (unsigned i = 0; i < r.pixel_count; ++i) {
                        PacketSample p;
                        p.pixel = r.pixel_begin + i;
                        p.sample = r.sample_begin;
                        p.wavelengths.fill(550);
                        p.wavelength_pdfs.fill(1);
                        p.radiance.fill(float(1 + p.pixel + p.sample));
                        result.packet_samples.push_back(p);
                    }
                    return result;
                }};
    }
};
void driver_equivalence() {
    RenderScene scene;
    scene.model = RenderModel::pbrt;
    scene.spectral_transport = SpectralTransport::packet;
    RenderSettings settings;
    settings.width = 2;
    settings.height = 1;
    settings.samples = 4;
    settings.batch_size = 2;
    for (unsigned failure : {0u, 2u}) {
        Source a, b;
        a.fail_at = b.fail_at = failure;
        tick = clock_calls = 0;
        const auto plain = detail::drive_progressive(scene, settings, {}, a.get());
        require(clock_calls == 0, "Disabled driver does no collector bookkeeping");
        RenderTimings t(clock_value);
        RenderControl control;
        control.timings = &t;
        const auto timed = detail::drive_progressive(scene, settings, control, b.get());
        t.finish();
        require(t.valid() && timed.film == plain.film && timed.status == plain.status &&
                    timed.completion == plain.completion && timed.diagnostic == plain.diagnostic &&
                    a.calls == b.calls,
                "Timing must preserve exact Film, failure and scheduling");
        require(phase(t, RenderTimingPhase::driver_sample).count == b.calls &&
                    phase(t, RenderTimingPhase::driver_sample).exclusive_ns ==
                        std::uint64_t{7} * b.calls,
                "Every successful or failing sample call is accounted once");
    }
}
void output_safety() {
    const auto folder = std::filesystem::temp_directory_path() /
                        ("rypath-timings-" + std::to_string(::getpid()) + "-" +
                         std::to_string(RenderTimings::steady_nanoseconds()));
    std::filesystem::create_directory(folder);
    const auto input = folder / "input";
    {
        std::ofstream f(input);
        f << "protected";
    }
    const std::array protected_paths{input};
    RenderTimingsOutput alias;
    require(!alias.reserve(input, protected_paths).empty() && read(input) == "protected",
            "Timing output cannot alias or overwrite existing scene input");
    const auto hard = folder / "hard";
    std::filesystem::create_hard_link(input, hard);
    RenderTimingsOutput hard_alias;
    require(!hard_alias.reserve(hard, protected_paths).empty(),
            "Existing hardlink remains protected");
    std::filesystem::create_directory(folder / "sub");
    const std::array planned_paths{folder / "planned"};
    RenderTimingsOutput normalized_alias;
    require(!normalized_alias.reserve(folder / "sub/../planned", planned_paths).empty(),
            "Normalized nonexistent output aliases are rejected before publication");
    const std::array pending_paths{folder / "image.pending"};
    RenderTimingsOutput pending_alias;
    require(!pending_alias.reserve(folder / "image", pending_paths).empty(),
            "Pending timing file must not alias a planned image output");
    RenderTimingsOutput invalid_path;
    require(
        !invalid_path.reserve(folder / std::string(4096, 'x'), {}).empty(),
        "Filesystem validation failure returns a diagnostic rather than replacing render errors");
    const auto output = folder / "report.json";
    RenderTimingsOutput report;
    require(report.reserve(output, protected_paths).empty() && report.reserved(),
            "Fresh output reserved");
    require(read(output).find("reserved") != std::string::npos,
            "Initial reservation survives interruption");
    RenderTimings t(clock_value);
    t.failure("actual render failure");
    t.finish();
    require(report.publish(t, 2, "test build").empty(),
            "Failed render can still publish diagnostics");
    require(read(output).find("\"exit_code\":2") != std::string::npos &&
                !std::filesystem::exists(output.string() + ".pending") &&
                read(input) == "protected",
            "Publication preserves original error code and protected inputs");
    RenderTimingsOutput repeated;
    require(!repeated.reserve(output, protected_paths).empty(),
            "Report cannot silently overwrite a prior run");
    RenderTimingsOutput replaced;
    const auto target = folder / "replaced.json";
    require(replaced.reserve(target, {}).empty(), "Replacement fixture reserved");
    const auto external = folder / "external";
    {
        std::ofstream f(external);
        f << "replacement";
    }
    std::filesystem::rename(external, target);
    require(!replaced.publish(t, 0, "test").empty() && read(target) == "replacement",
            "Changed destination identity blocks report publication");
    std::filesystem::remove_all(folder);
}
} // namespace
int main() {
    try {
        accounting();
        driver_equivalence();
        output_safety();
        std::cout << "Render timing accounting, equivalence and output safety passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
