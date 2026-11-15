// SPDX-License-Identifier: GPL-3.0-only
#include "render_identity.hpp"
#include "bsdf_table.hpp"
#include "checkpoint_contract.h"
#include <bit>
#include <string_view>
#include <type_traits>

namespace rypath {
namespace {
// No native object representation enters this stream. All sizes are u64 LE,
// enums u32 LE, booleans/presence u8, and IEEE float/double words LE. Each
// named field is prefixed by its UTF-8 name with a u64 byte length; sequences
// have a u64 element count. Fixed arrays also carry their count.
class Canonical {
    Sha256 hash_;
    void byte(std::uint8_t x) { hash_.update(std::span<const std::uint8_t>(&x, 1)); }

  public:
    template <typename T>
        requires(std::is_integral_v<T> && !std::is_same_v<T, bool>)
    void value(T x) {
        using U = std::make_unsigned_t<T>;
        U bits = static_cast<U>(x);
        std::array<std::uint8_t, sizeof(T)> bytes{};
        for (std::size_t i = 0; i < sizeof(T); ++i) {
            bytes[i] = static_cast<std::uint8_t>(bits & 255);
            bits >>= 8;
        }
        hash_.update(bytes);
    }
    void value(bool x) { byte(x ? 1 : 0); }
    void value(float x) { value(std::bit_cast<std::uint32_t>(x)); }
    void value(double x) { value(std::bit_cast<std::uint64_t>(x)); }
    template <typename T>
        requires std::is_enum_v<T>
    void value(T x) {
        value(static_cast<std::uint32_t>(x));
    }
    void tag(std::string_view name) {
        value(static_cast<std::uint64_t>(name.size()));
        hash_.update({reinterpret_cast<const std::uint8_t *>(name.data()), name.size()});
    }
    template <typename T> void field(std::string_view name, const T &x) {
        tag(name);
        value(x);
    }
    template <typename T, std::size_t N> void value(const std::array<T, N> &x) { sequence(x); }
    template <typename T, std::size_t N> void value(const T (&x)[N]) { sequence(x); }
    template <typename T> void value(const std::vector<T> &x) { sequence(x); }
    template <typename T> void sequence(const T &x) {
        value(static_cast<std::uint64_t>(std::size(x)));
        for (const auto &element : x)
            value(element);
    }
    template <typename T> void value(const std::optional<T> &x) {
        value(x.has_value());
        if (x)
            value(*x);
    }
    template <typename T> void value(const std::shared_ptr<T> &x) {
        value(bool(x));
        if (x)
            value(*x);
    }
#define FIELD(name) field(#name, x.name)
    void value(const GeometryPoint &x) {
        FIELD(x);
        FIELD(y);
        FIELD(z);
    }
    void value(const GeometryTransform &x) { FIELD(values); }
    void value(const GeometryMesh &x) {
        FIELD(positions);
        FIELD(triangles);
        FIELD(normals);
        FIELD(uvs);
        FIELD(patches);
        FIELD(tangents);
        FIELD(tangent_signs);
    }
    void value(const GeometryInstance &x) {
        FIELD(mesh_index);
        FIELD(transform);
        FIELD(non_opaque);
    }
    void value(const GeometryData &x) {
        FIELD(meshes);
        FIELD(instances);
    }
    void value(const SpectralKnot &x) {
        FIELD(wavelength);
        FIELD(value);
    }
    void value(const render_wire::SpectralKnot &x) {
        FIELD(wavelength);
        FIELD(value);
    }
    void value(const OpticalSpectrum &x) {
        FIELD(knots);
        FIELD(extend_endpoints);
    }
    void value(const SurfaceMaterialParameters &x) {
        FIELD(eta_root);
        FIELD(k_root);
        FIELD(conductor_reflectance_root);
        FIELD(transmittance);
        FIELD(transmittance_root);
        FIELD(transmission_scale);
        FIELD(transmission_scale_root);
        FIELD(interface_eta);
        FIELD(interface_eta_root);
        FIELD(interface_u_roughness);
        FIELD(interface_v_roughness);
        FIELD(interface_u_roughness_root);
        FIELD(interface_v_roughness_root);
        FIELD(thickness);
        FIELD(thickness_root);
        FIELD(albedo);
        FIELD(albedo_root);
        FIELD(g);
        FIELD(g_root);
        FIELD(max_depth);
        FIELD(samples);
        FIELD(first_material);
        FIELD(second_material);
        FIELD(mix_amount);
        FIELD(mix_amount_root);
    }
    void value(const GltfMaterialParameters &x) {
        FIELD(base_color);
        FIELD(metallic);
        FIELD(roughness);
        FIELD(normal_scale);
        FIELD(alpha_cutoff);
        FIELD(alpha_mode);
        FIELD(double_sided);
        FIELD(base_color_texture);
        FIELD(metallic_roughness_texture);
        FIELD(normal_texture);
        FIELD(emissive_texture);
        FIELD(emissive);
    }
    void value(const SpectralMaterial &x) {
        FIELD(reflectance);
        FIELD(knots);
        FIELD(kind);
        FIELD(constant);
        FIELD(coefficients);
        FIELD(image_texture);
        FIELD(temperature_kelvin);
        FIELD(texture_root);
        FIELD(bump_root);
        FIELD(normal_map);
        FIELD(scattering);
        FIELD(eta);
        FIELD(k);
        FIELD(u_roughness);
        FIELD(v_roughness);
        FIELD(u_roughness_root);
        FIELD(v_roughness_root);
        FIELD(remap_roughness);
        FIELD(surface);
        FIELD(table);
        FIELD(gltf);
    }
    void value(const BsdfTable &x) {
        FIELD(kind);
        FIELD(words);
    }
    void value(const RenderCamera &x) {
        FIELD(origin);
        FIELD(target);
        FIELD(up);
        FIELD(vertical_fov_degrees);
    }
    void value(const RasterCamera &x) {
        FIELD(origin);
        FIELD(corner);
        FIELD(dx);
        FIELD(dy);
    }
    void value(const PixelFilter &x) {
        FIELD(kind);
        FIELD(x_radius);
        FIELD(y_radius);
    }
    void value(const AreaEmission &x) {
        FIELD(radiance);
        FIELD(two_sided);
        FIELD(image_texture);
        FIELD(image_scale);
        FIELD(gltf_material);
    }
    void value(const SurfaceTransforms &x) {
        FIELD(shape);
        FIELD(placement);
        FIELD(authored_reverse);
    }
    void value(const HomogeneousMedium &x) {
        FIELD(sigma_a);
        FIELD(sigma_s);
        FIELD(emission);
        FIELD(g);
    }
    void value(const MediumInterface &x) {
        FIELD(inside);
        FIELD(outside);
    }
    void value(const ImageMipLevel &x) {
        FIELD(width);
        FIELD(height);
        FIELD(offset);
    }
    void value(const ImagePyramid &x) {
        FIELD(encoding);
        FIELD(wrap);
        FIELD(levels);
        FIELD(texels);
        FIELD(storage);
        FIELD(source_channels);
        FIELD(custom_transfer);
        FIELD(native_base);
        FIELD(gltf);
    }
    void value(const GltfSampler &x) {
        FIELD(wrap_s);
        FIELD(wrap_t);
        FIELD(min_filter);
        FIELD(mag_filter);
    }
    void value(const ImageTexture &x) {
        FIELD(image);
        FIELD(uscale);
        FIELD(vscale);
        FIELD(udelta);
        FIELD(vdelta);
        FIELD(scale);
        FIELD(invert);
        FIELD(filter);
        FIELD(max_anisotropy);
        FIELD(gltf_sampler);
    }
    void value(const TextureCamera &x) {
        FIELD(world_from_camera);
        FIELD(camera_from_world);
    }
    void value(const TextureTriangleFrame &x) {
        FIELD(dpdu);
        FIELD(dpdv);
    }
    void value(const texture_graph_wire::Mapping &x) {
        FIELD(kind);
        FIELD(values);
        FIELD(texture_from_render);
    }
    void value(const texture_graph_wire::Node &x) {
        FIELD(kind);
        FIELD(type);
        FIELD(children);
        FIELD(payload);
        FIELD(values);
        FIELD(mapping);
    }
    void value(const TextureGraph &x) {
        FIELD(nodes);
        FIELD(knots);
    }
    void value(const RenderScene &x) {
        FIELD(geometry);
        FIELD(materials);
        FIELD(instance_materials);
        FIELD(camera);
        FIELD(model);
        FIELD(raster_camera);
        FIELD(environment);
        FIELD(max_depth);
        FIELD(instance_reverse_orientation);
        FIELD(area_emissions);
        FIELD(instance_emissions);
        FIELD(filter);
        FIELD(image_textures);
        FIELD(texture_camera);
        FIELD(instance_texture_frames);
        FIELD(texture_graph);
        FIELD(instance_alpha_roots);
        FIELD(instance_invisible_emitters);
        FIELD(environment_image);
        FIELD(environment_image_scale);
        FIELD(environment_texture_from_world);
        FIELD(instance_surface_transforms);
        FIELD(volume_transport);
        FIELD(media);
        FIELD(camera_medium);
        FIELD(instance_media);
    }
    void value(const RenderSampler &x) {
        FIELD(kind);
        FIELD(randomization);
    }
#undef FIELD
    Sha256Digest finish() { return hash_.finish(); }
};
} // namespace
Sha256Digest render_identity(const RenderScene &scene, const RenderSettings &settings) {
    Canonical out;
    out.tag("rypath.render.identity");
    out.field("identity", checkpoint_contract::identity);
    out.field("renderer", checkpoint_contract::renderer);
    out.field("sampling", checkpoint_contract::sampling);
    out.field("accumulation", checkpoint_contract::accumulation);
    out.field("scene", scene);
    out.field("width", settings.width);
    out.field("height", settings.height);
    // Fixed target spp affects texture footprints and is part of the integrand.
    out.field("samples", settings.samples);
    if (settings.sampler.kind == SamplerKind::sobol)
        out.field("seed", static_cast<std::uint32_t>(sobol_signed_seed(settings.seed)));
    else
        out.field("seed", settings.seed);
    out.field("sampler", settings.sampler);
    return out.finish();
}
} // namespace rypath
