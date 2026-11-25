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
#include <string_view>

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
enum class PixelFilterKind { box, triangle, gaussian };

struct PixelFilter {
    PixelFilterKind kind = PixelFilterKind::box;
    float x_radius = 0.5f;
    float y_radius = 0.5f;
    float sigma = 0.5f;
    bool operator==(const PixelFilter &) const = default;
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
struct PerspectiveLens {
    float radius = 0;
    float focal_distance = 1e6f;
    GeometryPoint camera_x{1, 0, 0};
    GeometryPoint camera_y{0, 1, 0};
};
struct AreaEmission {
    double power = 0; // Canonical nonnegative authored power; zero preserves legacy radiance.
    std::array<float, 471> radiance{};
    bool two_sided = false;
    std::uint32_t image_texture = UINT32_MAX;
    float image_scale = 1;
    std::uint32_t gltf_material = UINT32_MAX;
};
inline constexpr std::uint32_t max_positional_lights = 64;
inline constexpr std::uint32_t max_point_lights = max_positional_lights;
inline constexpr std::uint32_t max_distant_lights = 64;
struct DistantLight {
    // Unit world direction from the scattering vertex toward the source.
    GeometryPoint direction{};
    std::array<float, 471> radiance{};
};
struct PointLight {
    GeometryPoint position{};
    std::array<float, 471> intensity{};
};
struct SpotLight {
    GeometryPoint position{};
    std::array<float, 471> intensity{};
    // Row-major inverse direction map; a common positive scale is immaterial.
    std::array<float, 9> light_from_world{1, 0, 0, 0, 1, 0, 0, 0, 1};
    float cos_falloff_start = 1;
    float cos_falloff_end = 0;
};
struct GoniometricLight {
    GeometryPoint position{};
    std::array<float, 471> intensity{};
    // Actual inverse declaration transform followed by PBRT's Y/Z swap.
    // Unlike spot cones, this unit-direction map must not be rescaled.
    std::array<float, 9> light_from_world{1, 0, 0, 0, 0, 1, 0, 1, 0};
    std::uint32_t image_texture = UINT32_MAX;
};
struct ProjectionLight {
    GeometryPoint position{};
    // Actual inverse declaration transform followed by PBRT's local Y flip.
    // Scale and shear are significant; do not normalize or rescale this map.
    std::array<float, 9> light_from_world{1, 0, 0, 0, -1, 0, 0, 0, 1};
    std::uint32_t image_texture = UINT32_MAX;
    float image_scale = 1;
    float inv_tan_half_fov = 1;
    float screen_half_width = 1;
    float screen_half_height = 1;
};
struct SurfaceTransforms {
    std::array<double, 16> shape{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    std::array<double, 16> placement{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    bool authored_reverse = false;
};
enum class IntegratorKind : std::uint32_t { path, volpath, bdpt };
enum class LightSamplerKind : std::uint32_t { bvh, power, uniform };
enum class SpectralTransport : std::uint32_t { scalar = 0, packet = 1, hero = 2 };
enum class SpectralTransportSelection { automatic, scalar, packet, hero };
enum class HeroWavelengthSampling : std::uint32_t { uniform = 0, visible = 1 };
std::string_view hero_wavelength_sampling_name(HeroWavelengthSampling mode);
std::optional<HeroWavelengthSampling> parse_hero_wavelength_sampling(std::string_view name);
constexpr bool uses_packet_transport(SpectralTransport mode) {
    return mode == SpectralTransport::packet || mode == SpectralTransport::hero;
}
std::string_view spectral_transport_name(SpectralTransport mode);
std::optional<SpectralTransportSelection> parse_spectral_transport(std::string_view name);
struct RenderScene {
    GeometryData geometry;
    std::vector<SpectralMaterial> materials;
    std::vector<std::uint32_t> instance_materials;
    RenderCamera camera;
    RenderModel model = RenderModel::demo;
    std::optional<RasterCamera> raster_camera{};
    std::optional<PerspectiveLens> perspective_lens{};
    std::array<float, 471> environment{};
    std::uint32_t max_depth = 5;
    double exposure = 1;
    std::vector<std::uint32_t> instance_reverse_orientation{};
    std::vector<AreaEmission> area_emissions{};
    std::vector<PointLight> point_lights{};
    std::vector<SpotLight> spot_lights{};
    std::vector<GoniometricLight> goniometric_lights{};
    std::vector<ProjectionLight> projection_lights{};
    std::vector<DistantLight> distant_lights{};
    float distant_scene_radius = 0;
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
    IntegratorKind integrator = IntegratorKind::path;
    LightSamplerKind light_sampler = LightSamplerKind::bvh;
    SpectralTransport spectral_transport = SpectralTransport::scalar;
    HeroWavelengthSampling hero_wavelengths = HeroWavelengthSampling::visible;
    double max_component_value = std::numeric_limits<double>::infinity();
    // Acceptance/resource policy does not change the radiometric scene identity.
    ImageLimits image_limits{};
    std::size_t triangle_frame_limit = render_patch_corner_limit;
    std::size_t preparation_byte_limit = std::numeric_limits<std::size_t>::max();
};
// Select after entry-point integrator/film overrides. Does not change authored depth.
// Explicit unsupported requests return an error without selecting a fallback.
std::string select_spectral_transport(RenderScene &scene, SpectralTransportSelection requested,
                                      std::string *provenance = nullptr);
struct RenderSettings {
    std::uint32_t width = 256;
    std::uint32_t height = 192;
    std::uint64_t samples = 128;
    std::uint64_t batch_size = 8;
    std::uint64_t seed = 1;
    RenderSampler sampler{};
    // PBRT's global stochastic-material seed is independent of its sampler seed.
    std::int32_t material_seed = 0;
};
struct RenderSample {
    std::uint32_t pixel = 0;
    std::uint64_t sample = 0;
    float wavelength = 0;
    float radiance = 0;
    std::uint32_t scatterings = 0;
    // Camera/wavelength diagnostics: Halton stores binary32 IEEE bits;
    // the other samplers retain their raw integer uniform words.
    std::array<std::uint32_t, 4> random_words{};
};
struct PacketSplat {
    std::array<float, 2> raster{};
    std::array<float, 4> radiance{};
    std::uint32_t strategy = 0;
};
struct PacketSample {
    std::uint32_t pixel = 0;
    std::uint64_t sample = 0;
    std::array<float, 4> wavelengths{};
    std::array<float, 4> wavelength_pdfs{};
    std::array<float, 4> radiance{};
    float filter_weight = 1;
    std::uint32_t splat_begin = 0;
    std::uint32_t splat_count = 0;
};
struct FilmPixel {
    std::array<double, 3> sum{};
    std::array<double, 3> correction{};
    std::uint64_t count = 0;
    std::array<double, 3> splat_sum{}, splat_correction{};
    std::array<double, 3> compatibility_sum{}, compatibility_correction{};
    std::array<double, 3> compatibility_splat_sum{}, compatibility_splat_correction{};
    double weight_sum = 0, weight_correction = 0;
    bool operator==(const FilmPixel &) const = default;
};
struct Film {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<FilmPixel> pixels;
    RenderModel model = RenderModel::demo;
    double exposure = 1;
    bool packet = false;
    double max_component_value = std::numeric_limits<double>::infinity();
    PixelFilter filter{};
    // False selects canonical unclamped output; accumulation always retains both.
    bool compatibility_output = false;
    bool operator==(const Film &) const = default;
};

double filter_integral(const PixelFilter &filter);
double filter_evaluate(const PixelFilter &filter, double x, double y);
std::array<double, 3> packet_sensor_xyz(const PacketSample &sample,
                                        const std::array<float, 4> &radiance);
std::array<double, 3> film_mean(const Film &film, const FilmPixel &pixel);
std::string validate_film_accumulation(const Film &film);
Film make_packet_film(const RenderScene &scene, const RenderSettings &settings);

// Packet owners are reduced in absolute-sample, pixel order. Splats retain their
// strategy order and never advance destination sample counts.
class PacketFilmBatch {
  public:
    PacketFilmBatch(const Film &film, std::uint64_t sample_count);
    std::string add(std::span<const PacketSample> samples, std::span<const PacketSplat> splats);
    std::string commit(Film &film);

  private:
    Film staged_;
    std::uint64_t begin_ = 0, end_ = 0, next_sample_ = 0;
    std::uint32_t next_pixel_ = 0;
    std::string error_;
};

std::string validate_render_settings(const RenderSettings &settings);
std::string validate_raster_support(const RasterCamera &camera, const PixelFilter &filter,
                                    std::uint32_t width, std::uint32_t height);
std::string validate_lens_support(const RasterCamera &camera, const PerspectiveLens &lens,
                                  const PixelFilter &filter, std::uint32_t width,
                                  std::uint32_t height);
// Factors indexed by source instance, then triangles followed by patches.
// Empty entries preserve legacy emission. Image means are evaluated once per table.
std::string prepare_area_power(const RenderScene &scene, std::vector<std::vector<float>> &factors,
                               std::size_t &failed_instance,
                               std::size_t max_factors = 128 * 1024 * 1024);
// Shared by area geometry preflight and Metal's world-space emitter upload.
std::optional<double> area_image_mean(const ImagePyramid &image, const ImageLimits &limits = {});

std::optional<GeometryPoint> area_world_vertex(GeometryPoint position,
                                               const GeometryTransform &transform);
std::string validate_area_geometry(const GeometryMesh &mesh, const GeometryTransform &transform);
std::string validate_patch_geometry(const GeometryMesh &mesh, const GeometryTransform &transform);
std::string validate_render_scene(const RenderScene &scene);
// Three times logical shading/staging storage plus fixed tables and packet
// workspace. Excludes native acceleration structures, driver scratch and pipelines.
std::string validate_render_preparation_budget(const RenderScene &scene,
                                               std::size_t *estimated_bytes = nullptr);
std::string validate_image_light(const ImagePyramid &image, const ImageLimits &limits = {});
std::string validate_goniometric_image(const ImagePyramid &image, const ImageLimits &limits = {});
std::string validate_projection_image(const ImagePyramid &image, const ImageLimits &limits = {});
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
