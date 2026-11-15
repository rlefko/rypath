// SPDX-License-Identifier: GPL-3.0-only
#include "image_output.hpp"

#include <cmath>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <unistd.h>

namespace {
using namespace rypath;
void require(bool condition, const char *reason) {
    if (!condition)
        throw std::runtime_error(reason);
}
std::vector<std::uint8_t> read(const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>{input}, {}};
}
void check(const std::filesystem::path &directory) {
    const auto path = directory / "result.exr";
    {
        std::ofstream output(path);
        output << "existing";
    }
    const auto sentinel = read(path);
    auto film = make_film(2, 1, RenderModel::pbrt, 2);
    film.pixels[0] = {{2, 0, 0}, {0.5, 0, 0}, 2};
    film.pixels[1] = {{0, 2, 0}, {}, 2};
    const auto original = film;
    auto reject = [&](const Film &invalid, ExrPrecision precision, const char *message) {
        require(!write_exr(invalid, path, precision).empty(), message);
        require(read(path) == sentinel, "failure replaced existing destination");
    };
    auto invalid = film;
    invalid.pixels[1].count = 1;
    reject(invalid, ExrPrecision::float32, "accepted unequal counts");
    invalid = film;
    invalid.pixels[0].count = 0;
    reject(invalid, ExrPrecision::float32, "accepted zero counts");
    invalid = film;
    invalid.pixels[0].sum[0] = std::numeric_limits<double>::infinity();
    reject(invalid, ExrPrecision::float32, "accepted nonfinite sum");
    invalid = film;
    invalid.pixels[0].correction[0] = std::numeric_limits<double>::quiet_NaN();
    reject(invalid, ExrPrecision::float32, "accepted nonfinite compensation");
    invalid = film;
    invalid.exposure = std::numeric_limits<double>::infinity();
    reject(invalid, ExrPrecision::float32, "accepted nonfinite exposure");
    invalid = film;
    invalid.pixels[0].sum[0] = 1e100;
    reject(invalid, ExrPrecision::float32, "accepted FLOAT overflow");
    invalid = film;
    invalid.pixels[0].sum[0] = 1e6;
    reject(invalid, ExrPrecision::half, "accepted HALF overflow");
    invalid = film;
    invalid.exposure = 1e308;
    reject(invalid, ExrPrecision::float32, "accepted projection overflow");
    invalid = film;
    invalid.width = 0;
    reject(invalid, ExrPrecision::float32, "accepted invalid dimensions");
    require(!write_exr(film, directory / "missing" / "image.exr").empty(),
            "accepted missing parent");
    const auto target_directory = directory / "directory.exr";
    std::filesystem::create_directory(target_directory);
    require(!write_exr(film, target_directory).empty(), "accepted directory destination");
    require(std::filesystem::is_directory(target_directory), "failure removed directory");
    // Reserved attributes must fail after temporary creation without publication.
    const std::pair<std::string, std::string> reserved_metadata{"channels", "invalid"};
    require(!write_exr(film, path, ExrPrecision::float32, std::span{&reserved_metadata, 1}).empty(),
            "accepted reserved metadata");
    require(read(path) == sentinel, "metadata failure replaced existing destination");
    for (const auto precision : {ExrPrecision::float32, ExrPrecision::half}) {
        const std::pair<std::string, std::string> metadata{"rypath.test", "film-output"};
        require(write_exr(film, path, precision, std::span{&metadata, 1}).empty(),
                "valid EXR publication failed");
        const auto decoded = decode_exr(read(path));
        if (!decoded.value)
            throw std::runtime_error("published EXR did not decode");
        const auto &image = *decoded.value;
        require(image.width == 2 && image.height == 1, "wrong output extent");
        require(image.sample_format == (precision == ExrPrecision::half
                                            ? ImageSampleFormat::half
                                            : ImageSampleFormat::float32),
                "wrong precision");
        // Independent constants: compensated XYZ mean (1.25,0,0), exposure 2;
        // second pixel is XYZ (0,1,0), exposure 2. No display clipping/transfer.
        const std::array<double, 6> expected{8.1011107285475, -2.4231668814, 0.13910879918,
                                             -3.07426766109,  3.75202448596, -0.408052477596};
        for (std::size_t i = 0; i < expected.size(); ++i)
            require(std::abs(image.floating_rgb[i] - expected[i]) <
                        (precision == ExrPrecision::half ? 0.004 : 0.000001),
                    "linear output projection changed");
    }
    require(film == original, "writer mutated raw film");
    // Output width exceeds the decoder input default; output must not inherit it.
    auto wide = make_film(4097, 1);
    for (auto &pixel : wide.pixels)
        pixel.count = 1;
    require(write_exr(wide, path).empty(), "output inherited texture dimension limit");
    for (const auto &entry : std::filesystem::directory_iterator(directory))
        require(!entry.path().filename().native().starts_with(".rypath-exr"),
                "temporary EXR leaked");
}
} // namespace
int main() try {
    auto pattern = (std::filesystem::temp_directory_path() / "rypath-exr-output.XXXXXX").native();
    const auto *created = mkdtemp(pattern.data());
    if (!created)
        return 1;
    const std::filesystem::path directory{created};
    try {
        check(directory);
        std::filesystem::remove_all(directory);
        std::cout << "EXR film publication checks passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::error_code cleanup_error;
        std::filesystem::remove_all(directory, cleanup_error);
        std::cerr << error.what() << '\n';
        return 1;
    }
} catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
}
