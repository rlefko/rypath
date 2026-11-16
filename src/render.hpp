// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "geometry.hpp"
#include "image_texture.hpp"
#include "sampling.hpp"
#include "spectral_knot.hpp"
#include "texture_footprint.hpp"
#include "texture_graph.hpp"

#include <functional>
#include <optional>
#include <span>

namespace rypath {

// Bound per-placement patch derivative validation, including reused object meshes.
inline constexpr std::size_t render_patch_corner_limit = 2'000'000;

inline constexpr std::array<float, 8> spectral_knots{360, 420, 470, 520, 570, 620, 700, 830};
inline constexpr std::uint32_t render_tile_capacity = 65536;
inline constexpr std::uint32_t render_emergency_limit = 1024;
inline constexpr std::uint32_t no_area_emission = UINT32_MAX;
inline constexpr std::uint32_t no_medium = UINT32_MAX;
inline constexpr std::uint32_t medium_boundary_limit = 4096;

// PBRT homogeneous spectra are sampled at integer nanometers and looked up at
// the nearest wavelength, with halfway values rounded away from zero.
struct HomogeneousMedium {
    std::array<float, 471> sigma_a{};
    std::array<float, 471> sigma_s{};
    std::array<float, 471> emission{};
    float g = 0;
};
struct MediumInterface {
    std::uint32_t inside = no_medium;
    std::uint32_t outside = no_medium;
};

enum class SpectralMaterialKind { sampled, constant, rgb_sigmoid, image, blackbody, texture_graph };
enum class ScatteringKind {
    diffuse,
    conductor,
    dielectric,
    thin_dielectric,
    diffuse_transmission,
    coated_diffuse,
    coated_conductor,
    measured,
    fourier,
    mix,
    null_interface,
    gltf
};

// Versioned spectral adaptation of glTF 2.0 metallic-roughness materials.
// Texture indices refer to raw linear RGBA glTF samplers, not spectral graphs.
struct GltfMaterialParameters {
    std::array<float, 4> base_color{1, 1, 1, 1};
    float metallic = 1;
    float roughness = 1;
    float normal_scale = 1;
    float alpha_cutoff = .5f;
    std::uint32_t alpha_mode = 0; // OPAQUE, MASK, BLEND
    bool double_sided = false;
    std::uint32_t base_color_texture = UINT32_MAX;
    std::uint32_t metallic_roughness_texture = UINT32_MAX;
    std::uint32_t normal_texture = UINT32_MAX;
    std::uint32_t emissive_texture = UINT32_MAX;
    std::array<float, 3> emissive{0, 0, 0};
};

// Optical constants are unbounded nonnegative spectra, never diffuse albedo.
// PBRT named curves extend endpoints; inline/file curves are zero outside support.
struct OpticalSpectrum {
    std::vector<SpectralKnot> knots;
    bool extend_endpoints = false;
};

struct BsdfTable;
// Additional surface parameters are stored separately on the GPU so ordinary
// diffuse, conductor and dielectric records retain a compact common layout.
struct SurfaceMaterialParameters {
    std::uint32_t eta_root = UINT32_MAX;
    std::uint32_t k_root = UINT32_MAX;
    std::uint32_t conductor_reflectance_root = UINT32_MAX;
    float transmittance = 0.25f;
    std::uint32_t transmittance_root = UINT32_MAX;
    float transmission_scale = 1;
    std::uint32_t transmission_scale_root = UINT32_MAX;
    OpticalSpectrum interface_eta{};
    std::uint32_t interface_eta_root = UINT32_MAX;
    float interface_u_roughness = 0;
    float interface_v_roughness = 0;
    std::uint32_t interface_u_roughness_root = UINT32_MAX;
    std::uint32_t interface_v_roughness_root = UINT32_MAX;
    float thickness = 0.01f;
    std::uint32_t thickness_root = UINT32_MAX;
    float albedo = 0;
    std::uint32_t albedo_root = UINT32_MAX;
    float g = 0;
    std::uint32_t g_root = UINT32_MAX;
    std::uint32_t max_depth = 10;
    std::uint32_t samples = 1;
    std::uint32_t first_material = UINT32_MAX;
    std::uint32_t second_material = UINT32_MAX;
    float mix_amount = 0.5f;
    std::uint32_t mix_amount_root = UINT32_MAX;
};

struct SpectralMaterial {
    std::array<float, 8> reflectance{};
    std::vector<SpectralKnot> knots{};
    SpectralMaterialKind kind = SpectralMaterialKind::sampled;
    float constant = 0;
    std::array<float, 3> coefficients{};
    std::uint32_t image_texture = 0;
    float temperature_kelvin = 0;
    std::uint32_t texture_root = UINT32_MAX;
    std::uint32_t bump_root = UINT32_MAX;
    std::uint32_t normal_map = UINT32_MAX;
    ScatteringKind scattering = ScatteringKind::diffuse;
    OpticalSpectrum eta{};
    OpticalSpectrum k{};
    float u_roughness = 0;
    float v_roughness = 0;
    std::uint32_t u_roughness_root = UINT32_MAX;
    std::uint32_t v_roughness_root = UINT32_MAX;
    bool remap_roughness = true;
    SurfaceMaterialParameters surface{};
    std::shared_ptr<const BsdfTable> table{};
    GltfMaterialParameters gltf{};
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
    std::uint32_t image_texture = UINT32_MAX;
    float image_scale = 1;
    std::uint32_t gltf_material = UINT32_MAX;
};
inline constexpr std::uint32_t max_point_lights = 64;
struct PointLight {
    GeometryPoint position{};
    std::array<float, 471> intensity{};
};
struct SurfaceTransforms {
    std::array<double, 16> shape{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    std::array<double, 16> placement{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    bool authored_reverse = false;
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
    std::vector<PointLight> point_lights{};
    // Empty or one emission index (or no_area_emission) per source instance.
    std::vector<std::uint32_t> instance_emissions{};
    PixelFilter filter{};
    std::vector<ImageTexture> image_textures{};
    TextureCamera texture_camera{};
    // One entry per source instance when image textures are present. Only textured
    // triangles need frames; patch tangents are evaluated at the hit coordinate.
    std::vector<std::vector<TextureTriangleFrame>> instance_texture_frames{};
    TextureGraph texture_graph{};
    std::vector<std::uint32_t> instance_alpha_roots{};
    std::vector<std::uint32_t> instance_invisible_emitters{};
    std::uint32_t environment_image = UINT32_MAX;
    float environment_image_scale = 1;
    GeometryTransform environment_texture_from_world{};
    std::vector<SurfaceTransforms> instance_surface_transforms{};
    bool volume_transport = false;
    std::vector<HomogeneousMedium> media{};
    std::uint32_t camera_medium = no_medium;
    // Empty means no authored transitions; equal sides retain the ray's medium.
    std::vector<MediumInterface> instance_media{};
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
std::string validate_image_light(const ImagePyramid &image);
bool instance_uses_texture_context(const RenderScene &scene, std::size_t index);
RenderScene make_demo_scene();
Film make_film(std::uint32_t width, std::uint32_t height, RenderModel model = RenderModel::demo,
               double exposure = 1);
std::array<double, 3> film_mean(const FilmPixel &pixel);
std::array<double, 3> wavelength_xyz(double wavelength);
std::array<double, 3> pbrt_cie_xyz(double wavelength);
double pbrt_y_inner_product(const std::array<float, 471> &spectrum);
double cie_y_integral();
double sampled_spectrum(std::span<const SpectralKnot> knots, double wavelength);
double blackbody_reflectance(float temperature_kelvin, double wavelength);
// Pack a validated positive finite temperature for binary32 wavelength evaluation.
float blackbody_exponent_factor(float temperature_kelvin);
double spectral_reflectance(const SpectralMaterial &material, double wavelength);
double optical_spectrum(const OpticalSpectrum &spectrum, double wavelength);
std::string validate_conductor_material(const SpectralMaterial &material);
std::string validate_dielectric_material(const SpectralMaterial &material);
bool scene_has_dielectrics(const RenderScene &scene);
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
