// SPDX-License-Identifier: GPL-3.0-only
#include "bsdf_table.hpp"
#include "bsdf_table_fixture.hpp"
#import <Metal/Metal.h>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <stdexcept>
static void require(bool b, const char *s) {
    if (!b)
        throw std::runtime_error(s);
}
static void close(float a, double b, double tolerance, const char *s) {
    if (std::abs(a - b) > tolerance * std::max(1., std::abs(b)))
        throw std::runtime_error(std::string(s) + ": got " + std::to_string(a) + " expected " +
                                 std::to_string(b));
}
int main(int argc, char **argv) {
    @autoreleasepool {
        try {
            if (argc == 3 && std::string(argv[1]) == "--write-fourier-fixture") {
                // Owned RGB expansion with curved zenith and angular densities,
                // both hemispheres and eta=1.5 radiance transmission scaling.
                table_fixture::write(argv[2], table_fixture::fourier(3, true, 1, true));
                return 0;
            }
            require(argc == 2 || argc == 4 || argc == 5,
                    "usage: table_bsdf_probe metallib [table oracle [fourier]]");
            id<MTLDevice> device = MTLCreateSystemDefaultDevice();
            if (!device) {
                std::cerr << "Metal device unavailable\n";
                return 77;
            }
            NSError *error = nil;
            id<MTLLibrary> lib = [device newLibraryWithURL:[NSURL fileURLWithPath:@(argv[1])]
                                                     error:&error];
            require(lib != nil, [[error description] UTF8String]);
            auto fn = [lib newFunctionWithName:@"rypath_table_bsdf_probe"];
            auto pipeline = [device newComputePipelineStateWithFunction:fn error:&error];
            require(pipeline != nil, [[error description] UTF8String]);
            auto queue = [device newCommandQueue];
            const bool dump_fourier = argc == 5 && std::string(argv[4]) == "fourier-dump";
            std::ofstream samples;
            if (dump_fourier) {
                samples.open(argv[3]);
                require(bool(samples), "cannot create Fourier sample queries");
                samples << std::setprecision(10);
            }
            constexpr unsigned count = 31 * 31;
            unsigned checked = 0, accepted = 0;
            auto path = std::filesystem::temp_directory_path() / "rypath-table-probe.binary";
            const auto dispatch = [&](const rypath::BsdfTable &t) {
                auto input = [device newBufferWithBytes:t.words.data()
                                                 length:t.words.size() * 4
                                                options:MTLResourceStorageModeShared];
                auto output = [device newBufferWithLength:count * 16 * sizeof(float)
                                                  options:MTLResourceStorageModeShared];
                auto command = [queue commandBuffer];
                auto encoder = [command computeCommandEncoder];
                [encoder setComputePipelineState:pipeline];
                [encoder setBuffer:input offset:0 atIndex:0];
                [encoder setBuffer:output offset:0 atIndex:1];
                [encoder dispatchThreads:MTLSizeMake(count, 1, 1)
                    threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
                [encoder endEncoding];
                [command commit];
                [command waitUntilCompleted];
                require(command.status == MTLCommandBufferStatusCompleted, "GPU command failed");
                const auto *out = static_cast<const float *>(output.contents);
                return std::vector<float>(out, out + count * 16);
            };
            for (unsigned mode = argc >= 4 ? (argc == 5 ? 8 : 7) : 0;
                 mode < (argc >= 4 ? (argc == 5 ? 9u : 8u) : 7u); ++mode) {
                bool fourier = mode < 4 || mode == 8;
                bool harmonic = mode == 2 || mode == 3;
                bool rgb = mode % 2 == 1;
                auto bytes = fourier ? table_fixture::fourier(rgb ? 3 : 1, harmonic)
                                     : table_fixture::measured(mode == 4   ? 1
                                                               : mode == 5 ? 2
                                                                           : 5);
                if (mode < 7)
                    table_fixture::write(path, bytes);
                auto result = rypath::read_bsdf_table(
                    mode >= 7 ? std::filesystem::path(argv[2]) : path,
                    fourier ? rypath::BsdfTableKind::fourier : rypath::BsdfTableKind::measured);
                require(bool(result.table), result.diagnostic.c_str());
                std::filesystem::remove(path);
                const auto output = dispatch(*result.table);
                const auto *out = output.data();
                std::ifstream oracle;
                if (mode >= 7 && !dump_fourier)
                    oracle.open(argv[3]);
                for (unsigned id = 0; id < count; ++id) {
                    auto q = out + 16 * id;
                    if (dump_fourier)
                        samples << id << ' ' << int(q[7]) << ' ' << q[0] << ' ' << q[1] << ' '
                                << q[2] << '\n';
                    if (mode >= 7 && !dump_fourier) {
                        int ok;
                        float x, y, z, f, pdf;
                        require(bool(oracle >> ok >> x >> y >> z >> f >> pdf), "oracle input");
                        if (mode == 8) {
                            double g, b, zenith_cdf, angular_cdf;
                            require(bool(oracle >> g >> b >> zenith_cdf >> angular_cdf),
                                    "Fourier RGB/quantile oracle");
                            if (ok) {
                                close(q[4], g, 2e-3, "PBRT angular G");
                                close(q[5], b, 2e-3, "PBRT angular B");
                                require(std::isfinite(zenith_cdf) && std::isfinite(angular_cdf),
                                        "nonfinite independent Fourier quantile");
                                // A binary32 direction and interpolant are checked
                                // against independent double CDF integrals, not
                                // upstream v3's amplitude-dependent inversion.
                                require(std::abs(zenith_cdf - (double(id / 31) + .5) / 31) < 5e-5,
                                        "independent Fourier zenith quantile");
                                require(std::abs(angular_cdf - (double(id % 31) + .5) / 31) < 5e-5,
                                        "independent Fourier angular quantile");
                            }
                        }
                        require(q[7] == ok, "PBRT acceptance mismatch");
                        if (ok) {
                            close(q[0], x, mode == 8 ? 0 : 2e-4, "PBRT query/sample x");
                            close(q[1], y, mode == 8 ? 0 : 2e-4, "PBRT query/sample y");
                            close(q[2], z, mode == 8 ? 0 : 2e-4, "PBRT query/sample z");
                            close(q[3], f, 2e-3, "PBRT sample f");
                            close(q[6], pdf, 2e-3, "PBRT sample pdf");
                        }
                    }
                    require(q[8] == 1, "invalid sample");
                    ++checked;
                    if (!q[7])
                        continue;
                    ++accepted;
                    close(q[0] * q[0] + q[1] * q[1] + q[2] * q[2], 1, 2e-6, "unit direction");
                    require(q[6] > 0 && q[12] == 1, "positive pdf");
                    close(q[3], q[10], 2e-5, "sample/evaluate f");
                    close(q[6], q[11], 2e-5, "sample/evaluate pdf");
                    if (mode == 8)
                        continue;
                    if (fourier) {
                        double mi = -q[2], mo = (id % 2 == 0 ? 1. : -1.) / std::sqrt(1.13),
                               scale = 1 / std::abs(mi);
                        if (mi * mo > 0)
                            scale *= mi > 0 ? 1 / 2.25 : 2.25;
                        double cosphi = -(q[0] * .2 + q[1] * .3) /
                                        (std::sqrt(q[0] * q[0] + q[1] * q[1]) * std::sqrt(.13));
                        double y = 1 + (harmonic ? .2 * cosphi : 0),
                               r = .8 + (harmonic ? .32 * cosphi : 0),
                               b = .6 - (harmonic ? .18 * cosphi : 0);
                        close(q[6], y / (4 * M_PI), 3e-6, "Fourier analytic pdf");
                        close(q[3], scale * (rgb ? r : y), 3e-5, "Fourier analytic f");
                        close(mi, 2 * (double(id / 31) + .5) / 31 - 1, 2e-6,
                              "Fourier analytic mu sample");
                        if (rgb) {
                            close(q[4], scale * (1.39829 * y - .100913 * b - .297375 * r), 3e-5,
                                  "Fourier angular RGB G");
                            close(q[5], scale * b, 3e-5, "Fourier angular RGB B");
                        }
                    } else if (mode != 7) {
                        close(q[3], (.5 + 1.5 * double(id % 5) / 4) / (4 * std::abs(q[2])), 3e-5,
                              "measured analytic f");
                        close(q[13], (double(id % 31) + .5) / 31, 2e-5, "warp inverse x");
                        close(q[14], (double(id / 31) + .5) / 31, 2e-5, "warp inverse y");
                        close(q[15], 1, 2e-6, "warp pdf");
                        double sign = id % 2 == 0 ? 1 : -1, ox = sign * .2 / std::sqrt(1.13),
                               oy = sign * .3 / std::sqrt(1.13), oz = 1 / std::sqrt(1.13);
                        double hx = ox + sign * q[0], hy = oy + sign * q[1], hz = oz + sign * q[2],
                               norm = std::sqrt(hx * hx + hy * hy + hz * hz);
                        hx /= norm;
                        hy /= norm;
                        hz /= norm;
                        double theta = std::atan2(std::sqrt(hx * hx + hy * hy), hz),
                               px = std::sqrt(theta / (M_PI / 2));
                        double jac =
                            4 * (ox * hx + oy * hy + oz * hz) *
                            std::max(2 * M_PI * M_PI * px * std::sqrt(hx * hx + hy * hy), 1e-6);
                        if (std::abs(q[6] - 1 / jac) > 5e-4 * std::max(1., 1 / jac)) {
                            std::cerr << "mode " << mode << " id " << id << " wi " << q[0] << " "
                                      << q[1] << " " << q[2] << "\n";
                        }
                        close(q[6], 1 / jac, 5e-4, "measured analytic directional pdf");
                    }
                }
            }
            if (argc == 2) {
                // Overall Fourier amplitude must affect f alone: the directional
                // proposal and PDF remain the same, including curved zenith CDFs.
                for (bool varying_zenith : {false, true})
                    for (bool harmonic : {false, true}) {
                        std::vector<float> reference;
                        for (float amplitude : {1.f, 1e-8f, 1e8f}) {
                            table_fixture::write(path, table_fixture::fourier(
                                                           3, harmonic, amplitude, varying_zenith));
                            const auto table =
                                rypath::read_bsdf_table(path, rypath::BsdfTableKind::fourier);
                            require(bool(table.table), table.diagnostic.c_str());
                            const auto output = dispatch(*table.table);
                            if (reference.empty())
                                reference = output;
                            for (unsigned id = 0; id < count; ++id) {
                                const auto *q = output.data() + id * 16;
                                const auto *r = reference.data() + id * 16;
                                require(q[8] == 1 && q[7] == r[7], "scaled Fourier acceptance");
                                for (unsigned c : {0u, 1u, 2u, 6u})
                                    close(q[c], r[c], 2e-5, "scaled Fourier direction/PDF");
                                double phi = std::atan2(-q[1], -q[0]) - std::atan2(.3, .2);
                                if (phi < 0)
                                    phi += 2 * M_PI;
                                const double angular_cdf =
                                    (phi + (harmonic ? .2 * std::sin(phi) : 0)) / (2 * M_PI);
                                close(angular_cdf, (double(id % 31) + .5) / 31, 3e-6,
                                      "Fourier analytic angular quantile");
                                const double mu = -q[2];
                                double zenith_cdf = (mu + 1) / 2;
                                if (varying_zenith) {
                                    const double z = mu <= 0 ? mu + 1 : mu;
                                    zenith_cdf =
                                        (mu <= 0
                                             ? z + 1.5 * z * z + z * z * z - .75 * z * z * z * z
                                             : 2.75 + 4 * z - 2 * z * z * z + .75 * z * z * z * z) /
                                        5.5;
                                }
                                close(zenith_cdf, (double(id / 31) + .5) / 31, 3e-6,
                                      "Fourier analytic zenith quantile");
                                // At exactly tangent incidence f has a singular
                                // 1/|cos(theta)| limit; compare away from that node.
                                if (std::abs(q[2]) > 1e-4f)
                                    for (unsigned c : {3u, 4u, 5u})
                                        close(q[c] / amplitude, r[c], 2e-4, "scaled Fourier value");
                                ++checked;
                            }
                        }
                    }
                std::filesystem::remove(path);
            }
            std::cout << "Table BSDF GPU checks: " << checked << " samples, " << accepted
                      << " accepted\n";
            return 0;
        } catch (const std::exception &e) {
            std::cerr << e.what() << '\n';
            return 1;
        }
    }
}
