// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "geometry.hpp"
#include "image_texture.hpp"
#include "sampling.hpp"
#include "texture_footprint.hpp"

#include <functional>
#include <optional>

namespace rypath {

// Bound per-placement patch derivative validation, including reused object meshes.
inline constexpr std::size_t render_patch_corner_limit = 2'000'000;

inline constexpr std::array<float, 8> spectral_knots{360, 420, 470, 520, 570, 620, 700, 830};
inline constexpr std::uint32_t render_tile_capacity = 65536;
inline constexpr std::uint32_t render_emergency_limit = 1024;
inline constexpr std::uint32_t no_area_emission = UINT32_MAX;

struct SpectralKnot {
    float wavelength = 0;
    float value = 0;
};
enum class SpectralMaterialKind { sampled, constant, rgb_sigmoid, image };

struct SpectralMaterial {
    std::array<float, 8> reflectance{};
    std::vector<SpectralKnot> knots{};
    SpectralMaterialKind kind = SpectralMaterialKind::sampled;
    float constant = 0;
    std::array<float, 3> coefficients{};
    std::uint32_t image_texture = 0;
};
enum class RenderModel { demo, pbrt };
enum class PixelFilterKind { box, triangle };

struct PixelFilter {
    PixelFilterKind kind = PixelFilterKind::box;
    float x_radius = 0.5f;
    float y_radius = 0.5f;
};

struct RenderCamera {
    GeometryPoint origin{4, 3, 6};
    GeometryPoint target{0, 1, 0};
    GeometryPoint up{0, 1, 0};
    float vertical_fov_degrees = 45;
};
// World-space pinhole ray coefficients at normalized raster coordinates.
struct RasterCamera {
    GeometryPoint origin;
    GeometryPoint corner;
    GeometryPoint dx;
    GeometryPoint dy;
};
struct AreaEmission {
    std::array<float, 471> radiance{};
    bool two_sided = false;
};
struct RenderScene {
    GeometryData geometry;
    std::vector<SpectralMaterial> materials;
    std::vector<std::uint32_t> instance_materials;
    RenderCamera camera;
    RenderModel model = RenderModel::demo;
    std::optional<RasterCamera> raster_camera{};
    std::array<float, 471> environment{};
    std::uint32_t max_depth = 5;
    double exposure = 1;
    std::vector<std::uint32_t> instance_reverse_orientation{};
    std::vector<AreaEmission> area_emissions{};
    // Empty or one emission index (or no_area_emission) per source instance.
    std::vector<std::uint32_t> instance_emissions{};
    PixelFilter filter{};
    std::vector<ImageTexture> image_textures{};
    TextureCamera texture_camera{};
    // One entry per source instance when image textures are present. Only textured
    // triangles need frames; patch tangents are evaluated at the hit coordinate.
    std::vector<std::vector<TextureTriangleFrame>> instance_texture_frames{};
};
struct RenderSettings {
    std::uint32_t width = 256;
    std::uint32_t height = 192;
    std::uint64_t samples = 128;
    std::uint64_t batch_size = 8;
    std::uint64_t seed = 1;
    RenderSampler sampler{};
};
struct RenderSample {
    std::uint32_t pixel = 0;
    std::uint64_t sample = 0;
    float wavelength = 0;
    float radiance = 0;
    std::uint32_t scatterings = 0;
    // Raw camera/wavelength words permit exact CPU/MSL sampler diagnostics.
    std::array<std::uint32_t, 4> random_words{};
};
struct FilmPixel {
    std::array<double, 3> sum{};
    std::array<double, 3> correction{};
    std::uint64_t count = 0;
    bool operator==(const FilmPixel &) const = default;
};
struct Film {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<FilmPixel> pixels;
    RenderModel model = RenderModel::demo;
    double exposure = 1;
    bool operator==(const Film &) const = default;
};

std::string validate_render_settings(const RenderSettings &settings);
std::string validate_raster_support(const RasterCamera &camera, const PixelFilter &filter,
                                    std::uint32_t width, std::uint32_t height);
// Shared by area geometry preflight and Metal's world-space emitter upload.
std::optional<GeometryPoint> area_world_vertex(GeometryPoint position,
                                               const GeometryTransform &transform);
std::string validate_area_geometry(const GeometryMesh &mesh, const GeometryTransform &transform);
std::string validate_patch_geometry(const GeometryMesh &mesh, const GeometryTransform &transform);
std::string validate_render_scene(const RenderScene &scene);
RenderScene make_demo_scene();
Film make_film(std::uint32_t width, std::uint32_t height, RenderModel model = RenderModel::demo,
               double exposure = 1);
std::array<double, 3> film_mean(const FilmPixel &pixel);
std::array<double, 3> wavelength_xyz(double wavelength);
std::array<double, 3> pbrt_cie_xyz(double wavelength);
double pbrt_y_inner_product(const std::array<float, 471> &spectrum);
double cie_y_integral();
double spectral_reflectance(const SpectralMaterial &material, double wavelength);
std::array<std::uint32_t, 4> sample_words(std::uint64_t seed, std::uint32_t pixel,
                                          std::uint64_t sample, std::uint32_t block);
float sample_uniform(std::uint32_t word);
float triangle_filter_offset(float u, float radius);
std::array<double, 3> linear_rgb(const std::array<double, 3> &xyz);
std::array<double, 3> linear_rgb(const Film &film, const FilmPixel &pixel);
std::array<std::uint8_t, 3> display_rgb(const std::array<double, 3> &xyz);
std::array<std::uint8_t, 3> display_rgb(const Film &film, const FilmPixel &pixel);

// Stages a complete increment from an immutable film. add() requires increasing
// sample indices for each pixel; pixel order may vary. Failure is sticky.
// The caller must not mutate the original film until commit or destruction.
class FilmBatch {
  public:
    FilmBatch(const Film &film, std::uint64_t sample_count);
    std::string add(std::span<const RenderSample> samples);
    std::string commit(Film &film);

  private:
    Film staged_;
    std::uint64_t begin_ = 0;
    std::uint64_t end_ = 0;
    std::string error_;
};

} // namespace rypath
