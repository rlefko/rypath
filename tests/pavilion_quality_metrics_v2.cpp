// SPDX-License-Identifier: GPL-3.0-only
#include "image_texture.hpp"
#include "pavilion_quality_metrics.hpp"
#include <atomic>
#include <bit>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <mutex>
#include <random>
#include <set>
#include <sstream>
#include <thread>
namespace pq = pavilion_quality;
struct Image {
    unsigned width, height;
    std::vector<float> rgb;
};
Image read_image(const std::string &path, double scale) {
    if (!std::isfinite(scale) || scale <= 0)
        throw std::runtime_error("Invalid image scale");
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw std::runtime_error("Missing image: " + path);
    Image out{};
    if (path.ends_with(".pfm")) {
        std::string magic;
        float endian;
        in >> magic >> out.width >> out.height >> endian;
        if (magic != "PF" || endian != -1 || !out.width || !out.height || out.width > 8192 ||
            out.height > 8192)
            throw std::runtime_error("Invalid PFM: " + path);
        if (in.get() != '\n')
            throw std::runtime_error("Invalid PFM header");
        out.rgb.resize(std::size_t(out.width) * out.height * 3);
        // Width is bounded to 8192 above; this row byte count fits streamsize.
        const auto row_bytes =
            static_cast<std::streamsize>(std::size_t(out.width) * 3 * sizeof(float));
        for (unsigned y = 0; y < out.height; ++y)
            in.read(reinterpret_cast<char *>(out.rgb.data() +
                                             std::size_t(out.height - 1 - y) * out.width * 3),
                    row_bytes);
        if (!in || in.peek() != std::char_traits<char>::eof())
            throw std::runtime_error("Invalid PFM payload");
        if constexpr (std::endian::native != std::endian::little)
            throw std::runtime_error("PFM requires little endian host");
    } else {
        std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(in), {}};
        rypath::ImageLimits limits;
        limits.max_dimension = 8192;
        limits.file_bytes = rypath::image_file_ceiling;
        auto decoded = rypath::decode_exr(bytes, limits);
        if (!decoded.value)
            throw std::runtime_error(decoded.diagnostic);
        if (decoded.value->sample_format != rypath::ImageSampleFormat::float32)
            throw std::runtime_error("Quality protocol requires FLOAT EXR");
        out.width = decoded.value->width;
        out.height = decoded.value->height;
        out.rgb = std::move(decoded.value->floating_rgb);
    }
    for (auto &v : out.rgb) {
        v = float(v * scale);
        if (!std::isfinite(v))
            throw std::runtime_error("Nonfinite RGB image");
    }
    return out;
}
// The CLI supplies distinct input and output paths in this documented order.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void extract(const char *source, const char *target, double scale) {
    auto image = read_image(source, scale);
    std::ofstream out(target, std::ios::binary);
    out << "PF\n" << image.width << ' ' << image.height << "\n-1.0\n";
    // Both supported input readers bound width to 8192.
    const auto row_bytes =
        static_cast<std::streamsize>(std::size_t(image.width) * 3 * sizeof(float));
    for (unsigned y = 0; y < image.height; ++y)
        out.write(reinterpret_cast<const char *>(
                      image.rgb.data() + std::size_t(image.height - 1 - y) * image.width * 3),
                  row_bytes);
    if (!out)
        throw std::runtime_error("PFM write failed");
}
struct Dataset {
    std::string name;
    pq::Ensemble luminance[2];
    std::vector<std::array<std::vector<double>, 3>> means[2];
    std::vector<pq::Estimate> point[2];
};
// Same per-draw seed-seq/rejection algorithm as v1; only two real arms.
std::array<std::vector<unsigned>, 2> bootstrap_counts(unsigned n, std::uint64_t seed,
                                                      unsigned draw) {
    std::array<std::vector<unsigned>, 2> counts{std::vector<unsigned>(n), std::vector<unsigned>(n)};
    if (n != 64)
        throw std::runtime_error("v2 requires exactly 64 replicates per arm");
    for (unsigned backend = 0; backend < 2; ++backend) {
        std::seed_seq sequence{unsigned(seed), unsigned(seed >> 32), draw, backend};
        std::mt19937 rng(sequence);
        const std::uint32_t threshold = std::uint32_t(0u - n) % n;
        for (unsigned i = 0; i < n; ++i) {
            std::uint32_t word;
            do {
                word = rng();
            } while (word < threshold);
            ++counts[backend][word % n];
        }
    }
    return counts;
}
// Manifest input and JSON output have distinct roles in the CLI contract.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
int analyze(const char *manifest, const char *destination) {
    std::ifstream in(manifest);
    std::string magic;
    unsigned width, height, n, draws, groups, nregions, threads;
    std::uint64_t seed;
    in >> magic >> width >> height >> n >> draws >> seed >> groups >> nregions >> threads;
    if (!in || magic != "PAVILION_METRICS_2" || n != 64 || draws != 20000 || !groups ||
        groups > 4 || !nregions || nregions > 6 || !threads || threads > 64)
        throw std::runtime_error("Invalid metrics manifest or bootstrap budget");
    std::vector<pq::Region> regions(nregions);
    std::vector<std::string> names(nregions);
    std::set<std::string> region_names;
    const std::set<std::string> allowed_regions{"full", "wall", "soffit", "floor", "pool", "glass"};
    for (unsigned r = 0; r < nregions; ++r) {
        in >> names[r] >> regions[r].x0 >> regions[r].y0 >> regions[r].x1 >> regions[r].y1;
        if (!allowed_regions.contains(names[r]) || !region_names.insert(names[r]).second)
            throw std::runtime_error("Invalid or duplicate region");
    }
    std::vector<Dataset> data(groups);
    std::set<std::string> dataset_names;
    std::vector<unsigned> ones(n, 1);
    for (auto &group : data) {
        in >> group.name;
        if (!dataset_names.insert(group.name).second ||
            (group.name != "day/raw" && group.name != "day/clamped" && group.name != "night/raw" &&
             group.name != "night/clamped"))
            throw std::runtime_error("Invalid or duplicate dataset");
        for (unsigned backend = 0; backend < 2; ++backend) {
            group.means[backend].resize(nregions);
            for (unsigned rep = 0; rep < n; ++rep) {
                std::string path;
                double scale;
                in >> std::quoted(path) >> scale;
                if (!in)
                    throw std::runtime_error("Missing replica in manifest");
                auto image = read_image(path, scale);
                if (image.width != width || image.height != height)
                    throw std::runtime_error("Image resolution mismatch");
                std::vector<double> lum(std::size_t(width) * height);
                for (std::size_t p = 0; p < lum.size(); ++p)
                    lum[p] = .2126 * image.rgb[3 * p] + .7152 * image.rgb[3 * p + 1] +
                             .0722 * image.rgb[3 * p + 2];
                group.luminance[backend].push_back(std::move(lum));
                for (unsigned r = 0; r < nregions; ++r) {
                    const auto &roi = regions[r];
                    if (roi.x0 >= roi.x1 || roi.y0 >= roi.y1 || roi.x1 > width || roi.y1 > height)
                        throw std::runtime_error("Invalid ROI");
                    const double count = double(roi.x1 - roi.x0) * (roi.y1 - roi.y0);
                    double mean[3] = {};
                    for (unsigned y = roi.y0; y < roi.y1; ++y)
                        for (unsigned x = roi.x0; x < roi.x1; ++x)
                            for (unsigned c = 0; c < 3; ++c)
                                mean[c] += image.rgb[(std::size_t(y) * width + x) * 3 + c] / count;
                    for (unsigned c = 0; c < 3; ++c)
                        group.means[backend][r][c].push_back(mean[c]);
                }
            }
            group.point[backend] =
                pq::estimate(group.luminance[backend], ones, width, height, regions);
        }
    }
    in >> std::ws;
    if (!in.eof())
        throw std::runtime_error("Trailing metrics manifest fields");
    std::vector<double> maxima(draws);
    std::atomic<unsigned> next{0};
    std::exception_ptr failure;
    std::mutex mutex;
    auto run = [&] {
        try {
            for (unsigned draw; (draw = next.fetch_add(1)) < draws;) {
                // One whole-image resample per arm/draw, shared across every plane/ROI.
                auto counts = bootstrap_counts(n, seed, draw);
                double maximum = -std::numeric_limits<double>::infinity();
                for (const auto &group : data) {
                    auto candidate =
                        pq::estimate(group.luminance[0], counts[0], width, height, regions);
                    auto reference =
                        pq::estimate(group.luminance[1], counts[1], width, height, regions);
                    for (unsigned r = 0; r < nregions; ++r)
                        for (unsigned metric = 0; metric < 2; ++metric) {
                            auto value = [metric](const pq::Estimate &v) {
                                return metric ? v.tail : v.variance;
                            };
                            const double point =
                                pq::score(value(group.point[0][r]), value(group.point[1][r]),
                                          group.point[1][r].epsilon);
                            const double boot = pq::score(value(candidate[r]), value(reference[r]),
                                                          reference[r].epsilon);
                            maximum = std::max(maximum, boot - point);
                        }
                }
                maxima[draw] = maximum;
            }
        } catch (...) {
            std::lock_guard lock(mutex);
            if (!failure)
                failure = std::current_exception();
            next = draws;
        }
    };
    std::vector<std::thread> pool;
    for (unsigned t = 0; t < threads; ++t)
        pool.emplace_back(run);
    for (auto &t : pool)
        t.join();
    if (failure)
        std::rethrow_exception(failure);
    const double critical = pq::quantile95(std::move(maxima));
    std::ofstream out(destination);
    out << std::setprecision(17)
        << "{\"engine\":\"pavilion-quality-v2-two-arm\",\"replicates\":" << n
        << ",\"backend_count\":2,\"resolution\":[" << width << "," << height << "]"
        << ",\"bootstrap_draws\":" << draws << ",\"bootstrap_seed\":" << seed
        << ",\"bootstrap_rng\":\"mt19937-per-draw-seed-seq-rejection\""
        << ",\"simultaneous_log_margin\":" << critical << ",\"gates\":[";
    bool first = true, all = true;
    unsigned gate_count = 0, mean_count = 0;
    for (const auto &group : data)
        for (unsigned r = 0; r < nregions; ++r)
            for (unsigned metric = 0; metric < 2; ++metric) {
                const double candidate =
                    metric ? group.point[0][r].tail : group.point[0][r].variance;
                const double reference =
                    metric ? group.point[1][r].tail : group.point[1][r].variance;
                const double epsilon = group.point[1][r].epsilon;
                const bool absolute = reference <= epsilon;
                const double log_upper = pq::score(candidate, reference, epsilon) + critical;
                const double log_bound = log_upper + (absolute ? std::log(epsilon) : 0.);
                const double upper = candidate == 0 ? 0 : std::exp(log_bound);
                const bool pass = std::isfinite(upper) && upper <= 1.25 * (absolute ? epsilon : 1.);
                all = all && pass;
                ++gate_count;
                if (!first)
                    out << ',';
                first = false;
                out << "{\"dataset\":\"" << group.name << "\",\"region\":\"" << names[r]
                    << "\",\"metric\":\"" << (metric ? "tail" : "variance")
                    << "\",\"candidate\":" << candidate << ",\"reference\":" << reference
                    << ",\"epsilon\":" << epsilon
                    << ",\"absolute\":" << (absolute ? "true" : "false")
                    << ",\"log_upper\":" << log_upper << ",\"upper\":";
                if (!std::isfinite(upper))
                    out << "null";
                else
                    out << upper;
                out << ",\"pass\":" << (pass ? "true" : "false") << '}';
            }
    out << "],\"mean_gates\":[";
    first = true;
    for (const auto &group : data)
        if (group.name.ends_with("/raw"))
            for (unsigned r = 0; r < nregions; ++r)
                for (unsigned c = 0; c < 3; ++c) {
                    const auto &a = group.means[0][r][c], &b = group.means[1][r][c];
                    const double ma = std::accumulate(a.begin(), a.end(), 0.) / n;
                    const double mb = std::accumulate(b.begin(), b.end(), 0.) / n;
                    double variance = 0;
                    for (double x : a)
                        variance += (x - ma) * (x - ma) / (n * (n - 1));
                    for (double x : b)
                        variance += (x - mb) * (x - mb) / (n * (n - 1));
                    const double se = std::sqrt(variance),
                                 threshold = 5 * se + 2e-5 * std::max(1., std::abs(mb));
                    const bool pass = pq::mean_gate(a, b);
                    all = all && pass;
                    ++mean_count;
                    if (!first)
                        out << ',';
                    first = false;
                    out << "{\"dataset\":\"" << group.name << "\",\"region\":\"" << names[r]
                        << "\",\"channel\":" << c << ",\"candidate\":" << ma
                        << ",\"reference\":" << mb << ",\"difference\":" << ma - mb
                        << ",\"combined_se\":" << se << ",\"threshold\":" << threshold
                        << ",\"pass\":" << (pass ? "true" : "false") << '}';
                }
    const bool complete = gate_count == 48 && mean_count == 36 && n == 64 && width == 320 &&
                          height == 170 && dataset_names.size() == 4;
    out << "],\"gate_count\":" << gate_count << ",\"complete\":" << (complete ? "true" : "false")
        << ",\"accepted\":" << (all && complete ? "true" : "false") << "}\n";
    if (!out)
        throw std::runtime_error("Metrics write failed");
    return 0;
}
#ifndef PAVILION_QUALITY_V2_TESTING
int main(int argc, char **argv) {
    try {
        if (argc == 5 && std::string(argv[1]) == "extract") {
            extract(argv[2], argv[3], std::stod(argv[4]));
            return 0;
        }
        if (argc == 4 && std::string(argv[1]) == "analyze")
            return analyze(argv[2], argv[3]);
        throw std::runtime_error(
            "Usage: metrics extract input output.pfm scale | analyze manifest output.json");
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 2;
    }
}

#endif
