// SPDX-License-Identifier: GPL-3.0-only

#include "metal_render.hpp"
#include "pbrt_render.hpp"

#include <bit>
#include <cmath>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {

void write_pfm(const std::filesystem::path &output, const rypath::Film &film,
               std::uint64_t samples) {
    static_assert(std::numeric_limits<float>::is_iec559 && sizeof(float) == 4);
    static_assert(std::endian::native == std::endian::little ||
                  std::endian::native == std::endian::big);
    constexpr auto scale = std::endian::native == std::endian::little ? "-1.0" : "1.0";
    std::ofstream file(output, std::ios::binary);
    file.exceptions(std::ios::failbit | std::ios::badbit);
    file << "PF\n" << film.width << ' ' << film.height << '\n' << scale << '\n';
    for (auto row = film.height; row > 0; --row) {
        for (std::uint32_t x = 0; x < film.width; ++x) {
            const auto &pixel = film.pixels[static_cast<std::size_t>(row - 1) * film.width + x];
            if (pixel.count != samples) {
                throw std::runtime_error("Incomplete reference film");
            }
            const auto rgb = rypath::linear_rgb(film, pixel);
            const std::array<float, 3> channels{
                static_cast<float>(rgb[0]), static_cast<float>(rgb[1]), static_cast<float>(rgb[2])};
            if (!std::isfinite(channels[0]) || !std::isfinite(channels[1]) ||
                !std::isfinite(channels[2])) {
                throw std::runtime_error("Nonfinite reference output channel");
            }
            file.write(reinterpret_cast<const char *>(channels.data()), sizeof(channels));
        }
    }
    file.close();
}

} // namespace

int main(int argc, char **argv) {
    try {
        if (argc != 3) {
            std::cerr << "Usage: rypath_pbrt_reference SCENE OUTPUT.pfm\n";
            return 1;
        }
        const std::filesystem::path entry(argv[1]);
        const auto checked = rypath::pbrt::check(entry);
        if (checked.exit_code() != 0) {
            std::cerr << rypath::pbrt::report(checked);
            return checked.exit_code();
        }
        auto translation = rypath::pbrt::translate(checked, rypath::pbrt::Location{entry.string()});
        if (translation.exit_code() != 0 || !translation.scene) {
            for (const auto &diagnostic : translation.diagnostics) {
                std::cerr << *diagnostic.location.file << ':' << diagnostic.location.line << ':'
                          << diagnostic.location.column << ": " << diagnostic.message << '\n';
            }
            return translation.exit_code() != 0 ? translation.exit_code() : 1;
        }
        translation.settings.batch_size = 1024;
        auto rendered = rypath::render_progressive(*translation.scene, translation.settings);
        if (rendered.status != rypath::GeometryStatus::success) {
            std::cerr << rendered.diagnostic << '\n';
            return 1;
        }
        write_pfm(argv[2], rendered.film, translation.settings.samples);
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
