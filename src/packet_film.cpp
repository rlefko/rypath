// SPDX-License-Identifier: GPL-3.0-only
#include "render.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace rypath {
namespace {
void add_compensated(double value, double &sum, double &correction) {
    const double next = sum + value;
    correction += std::abs(sum) >= std::abs(value) ? (sum - next) + value : (value - next) + sum;
    sum = next;
}
bool finite_pair(double sum, double correction) {
    return std::isfinite(sum) && std::isfinite(correction) && std::isfinite(sum + correction);
}
bool valid_packet(const PacketSample &sample) {
    if (!(sample.filter_weight > 0) || !std::isfinite(sample.filter_weight))
        return false;
    bool secondary = false, terminated = false;
    for (std::size_t i = 0; i < 4; ++i) {
        if (!(sample.wavelengths[i] >= 360 && sample.wavelengths[i] <= 830) ||
            !std::isfinite(sample.wavelength_pdfs[i]) || sample.wavelength_pdfs[i] < 0)
            return false;
        if (i == 0 && sample.wavelength_pdfs[i] == 0)
            return false;
        if (i > 0) {
            secondary |= sample.wavelength_pdfs[i] > 0;
            terminated |= sample.wavelength_pdfs[i] == 0;
        }
    }
    return !(secondary && terminated);
}
bool valid_radiance(const std::array<float, 4> &values) {
    return std::ranges::all_of(values, [](float v) { return std::isfinite(v) && v >= 0; });
}
std::array<double, 3> clamp_sensor(std::array<double, 3> xyz, const Film &film) {
    for (auto &v : xyz)
        v *= film.exposure;
    const auto maximum = *std::max_element(xyz.begin(), xyz.end());
    if (maximum > film.max_component_value)
        for (auto &v : xyz)
            v *= film.max_component_value / maximum;
    return xyz;
}
bool add_xyz(const std::array<double, 3> &xyz, double weight, std::array<double, 3> &sum,
             std::array<double, 3> &correction) {
    for (std::size_t i = 0; i < 3; ++i) {
        if (!std::isfinite(xyz[i] * weight))
            return false;
        add_compensated(xyz[i] * weight, sum[i], correction[i]);
        if (!finite_pair(sum[i], correction[i]))
            return false;
    }
    return true;
}
double gaussian(double x, double sigma) {
    return std::exp(-x * x / (2 * sigma * sigma)) / (std::sqrt(2 * std::numbers::pi) * sigma);
}
} // namespace

double filter_evaluate(const PixelFilter &filter, double x, double y) {
    if (std::abs(x) > filter.x_radius || std::abs(y) > filter.y_radius)
        return 0;
    switch (filter.kind) {
    case PixelFilterKind::box:
        return 1;
    case PixelFilterKind::triangle:
        return std::max(0., filter.x_radius - std::abs(x)) *
               std::max(0., filter.y_radius - std::abs(y));
    case PixelFilterKind::gaussian:
        return std::max(0., gaussian(x, filter.sigma) - gaussian(filter.x_radius, filter.sigma)) *
               std::max(0., gaussian(y, filter.sigma) - gaussian(filter.y_radius, filter.sigma));
    }
    return std::numeric_limits<double>::quiet_NaN();
}

double filter_integral(const PixelFilter &filter) {
    const double x = filter.x_radius, y = filter.y_radius;
    if (!(x > 0 && y > 0) || !std::isfinite(x) || !std::isfinite(y))
        return 0;
    switch (filter.kind) {
    case PixelFilterKind::box:
        return 4 * x * y;
    case PixelFilterKind::triangle:
        return x * x * y * y;
    case PixelFilterKind::gaussian: {
        const double sigma = filter.sigma;
        if (!(sigma > 0) || !std::isfinite(sigma))
            return 0;
        const auto integral = [sigma](double r) {
            return std::erf(r / (std::sqrt(2.) * sigma)) - 2 * r * gaussian(r, sigma);
        };
        return integral(x) * integral(y);
    }
    }
    return 0;
}

std::array<double, 3> packet_sensor_xyz(const PacketSample &sample,
                                        const std::array<float, 4> &radiance) {
    std::array<double, 3> xyz{};
    for (std::size_t lane = 0; lane < 4; ++lane) {
        if (sample.wavelength_pdfs[lane] == 0)
            continue;
        const auto cie = pbrt_cie_xyz(sample.wavelengths[lane]);
        const double weight = double(radiance[lane]) / sample.wavelength_pdfs[lane] / 4;
        for (std::size_t channel = 0; channel < 3; ++channel)
            xyz[channel] += weight * cie[channel];
    }
    return xyz;
}

std::array<double, 3> film_mean(const Film &film, const FilmPixel &pixel) {
    if (!film.packet)
        return film_mean(pixel);
    std::array<double, 3> xyz{};
    if (!pixel.count)
        return xyz;
    const double weight = pixel.weight_sum + pixel.weight_correction;
    const double splat_scale = 1 / (double(pixel.count) * filter_integral(film.filter));
    const bool compatibility = film.compatibility_output && std::isfinite(film.max_component_value);
    for (std::size_t c = 0; c < 3; ++c) {
        const double camera = compatibility
                                  ? pixel.compatibility_sum[c] + pixel.compatibility_correction[c]
                                  : pixel.sum[c] + pixel.correction[c];
        const double splat = compatibility ? pixel.compatibility_splat_sum[c] +
                                                 pixel.compatibility_splat_correction[c]
                                           : pixel.splat_sum[c] + pixel.splat_correction[c];
        xyz[c] = (weight > 0 ? camera / weight : 0) + splat * splat_scale;
    }
    return xyz;
}

std::string validate_film_accumulation(const Film &film) {
    if (!film.width || !film.height ||
        film.pixels.size() != std::uint64_t(film.width) * film.height)
        return "Invalid film dimensions or storage";
    if (!std::isfinite(film.exposure) || film.exposure < 0 ||
        (film.model == RenderModel::demo && (film.exposure != 1 || film.packet)))
        return "Invalid film sensor exposure or model";
    if (film.packet &&
        (!(filter_integral(film.filter) > 0) || !std::isfinite(filter_integral(film.filter)) ||
         !(film.max_component_value > 0)))
        return "Invalid packet film filter or clamp";
    const auto count = film.pixels.front().count;
    for (const auto &p : film.pixels) {
        if (p.count != count || !finite_pair(p.weight_sum, p.weight_correction) ||
            (film.packet && count > 0 && !(p.weight_sum + p.weight_correction > 0)))
            return "Invalid film sample counts or reconstruction weights";
        for (std::size_t c = 0; c < 3; ++c) {
            for (const auto pair :
                 {std::array{p.sum[c], p.correction[c]},
                  std::array{p.splat_sum[c], p.splat_correction[c]},
                  std::array{p.compatibility_sum[c], p.compatibility_correction[c]},
                  std::array{p.compatibility_splat_sum[c], p.compatibility_splat_correction[c]}}) {
                if (!finite_pair(pair[0], pair[1]) ||
                    (count == 0 && (pair[0] != 0 || pair[1] != 0)))
                    return "Invalid film accumulation";
            }
        }
        if (count == 0 && (p.weight_sum != 0 || p.weight_correction != 0))
            return "Invalid empty film reconstruction weights";
    }
    return {};
}

Film make_packet_film(const RenderScene &scene, const RenderSettings &settings) {
    auto film = make_film(settings.width, settings.height, scene.model, scene.exposure);
    film.packet = true;
    film.filter = scene.filter;
    film.max_component_value = scene.max_component_value;
    film.compatibility_output = std::isfinite(scene.max_component_value);
    if (auto error = validate_film_accumulation(film); !error.empty())
        throw std::invalid_argument(error);
    return film;
}

PacketFilmBatch::PacketFilmBatch(const Film &film, std::uint64_t sample_count) {
    error_ = validate_film_accumulation(film);
    if (!error_.empty())
        return;
    if (!film.packet) {
        error_ = "Packet accumulation requires a packet film";
        return;
    }
    begin_ = next_sample_ = film.pixels.front().count;
    if (!sample_count || sample_count > UINT64_MAX - begin_) {
        error_ = "Invalid packet batch sample range";
        return;
    }
    end_ = begin_ + sample_count;
    staged_ = film;
}

std::string PacketFilmBatch::add(std::span<const PacketSample> samples,
                                 std::span<const PacketSplat> splats) {
    if (!error_.empty())
        return error_;
    const auto fail = [&](const char *message) { return error_ = message; };
    std::size_t next_splat = 0;
    for (const auto &sample : samples) {
        if (sample.sample != next_sample_ || sample.pixel != next_pixel_ || next_sample_ >= end_)
            return fail("Packet owner is duplicated, missing, or out of canonical order");
        if (sample.splat_begin != next_splat || !valid_packet(sample) ||
            !valid_radiance(sample.radiance) || sample.splat_count > 64 ||
            sample.splat_begin > splats.size() ||
            sample.splat_count > splats.size() - sample.splat_begin)
            return fail("Invalid spectral packet or splat range");
        next_splat += sample.splat_count;
        auto &owner = staged_.pixels[sample.pixel];
        const auto xyz = packet_sensor_xyz(sample, sample.radiance);
        if (!add_xyz(xyz, sample.filter_weight, owner.sum, owner.correction))
            return fail("Packet camera accumulation overflow");
        if (std::isfinite(staged_.max_component_value) &&
            !add_xyz(clamp_sensor(xyz, staged_), sample.filter_weight, owner.compatibility_sum,
                     owner.compatibility_correction))
            return fail("Packet compatibility accumulation overflow");
        add_compensated(sample.filter_weight, owner.weight_sum, owner.weight_correction);
        if (!finite_pair(owner.weight_sum, owner.weight_correction))
            return fail("Packet reconstruction weight overflow");
        std::uint32_t previous_strategy = 0;
        for (const auto &splat : splats.subspan(sample.splat_begin, sample.splat_count)) {
            if (splat.strategy <= previous_strategy || splat.strategy > 65 ||
                !std::isfinite(splat.raster[0]) || !std::isfinite(splat.raster[1]) ||
                !valid_radiance(splat.radiance))
                return fail("Invalid or unordered packet splat");
            previous_strategy = splat.strategy;
            const auto value = packet_sensor_xyz(sample, splat.radiance);
            const auto compatibility = clamp_sensor(value, staged_);
            const auto bound = [](double coordinate, std::uint32_t size) {
                return static_cast<std::uint32_t>(std::clamp(coordinate, 0., double(size)));
            };
            const double x = splat.raster[0], y = splat.raster[1];
            const auto x0 = bound(std::ceil(x - .5 - staged_.filter.x_radius), staged_.width);
            const auto x1 = bound(std::floor(x - .5 + staged_.filter.x_radius) + 1, staged_.width);
            const auto y0 = bound(std::ceil(y - .5 - staged_.filter.y_radius), staged_.height);
            const auto y1 = bound(std::floor(y - .5 + staged_.filter.y_radius) + 1, staged_.height);
            for (auto py = y0; py < y1; ++py) {
                for (auto px = x0; px < x1; ++px) {
                    const double weight = filter_evaluate(staged_.filter, x - px - .5, y - py - .5);
                    if (weight == 0)
                        continue;
                    auto &destination = staged_.pixels[std::size_t(py) * staged_.width + px];
                    if (!add_xyz(value, weight, destination.splat_sum,
                                 destination.splat_correction) ||
                        (std::isfinite(staged_.max_component_value) &&
                         !add_xyz(compatibility, weight, destination.compatibility_splat_sum,
                                  destination.compatibility_splat_correction)))
                        return fail("Packet splat accumulation overflow");
                }
            }
        }
        ++owner.count;
        if (++next_pixel_ == staged_.pixels.size()) {
            next_pixel_ = 0;
            ++next_sample_;
        }
    }
    if (next_splat != splats.size())
        return fail("Packet batch contains unowned splat records");
    return {};
}

std::string PacketFilmBatch::commit(Film &film) {
    if (!error_.empty())
        return error_;
    if (next_sample_ != end_ || next_pixel_ != 0)
        return error_ = "Packet batch is incomplete";
    if (film.width != staged_.width || film.height != staged_.height ||
        film.model != staged_.model || film.pixels.size() != staged_.pixels.size() ||
        !film.packet || film.exposure != staged_.exposure ||
        film.max_component_value != staged_.max_component_value || film.filter != staged_.filter ||
        film.compatibility_output != staged_.compatibility_output ||
        !std::ranges::all_of(film.pixels, [&](const FilmPixel &p) { return p.count == begin_; }))
        return error_ = "Film changed before packet batch commit";
    if (auto error = validate_film_accumulation(staged_); !error.empty())
        return error_ = error;
    film = std::move(staged_);
    error_ = "Packet batch has already committed";
    return {};
}
} // namespace rypath
