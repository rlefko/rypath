// SPDX-License-Identifier: GPL-3.0-only
// Diagnostic-only scene policy, kept equivalent to pipeline_qualification.cpp.
// Included inside the diagnostic driver namespace after Options.
void set_parameter(pbrt::Descriptor &d, std::string name, std::optional<std::int32_t> value) {
    // Preserve the preflight-selected last matching parameters while indices move.
    std::vector<pbrt::Parameter> effective;
    for (const auto index : d.effective_authored)
        effective.push_back(d.authored.parameters[index]);
    d.authored.parameters = std::move(effective);
    std::erase_if(d.authored.parameters, [&](const auto &p) { return p.name == name; });
    std::erase_if(d.inherited, [&](const auto &p) { return p.parameter->name == name; });
    if (value) {
        pbrt::Parameter p;
        p.type = "integer";
        p.name = name;
        pbrt::Value v;
        v.kind = pbrt::ValueKind::integer;
        v.integer = *value;
        v.number = *value;
        v.text = std::to_string(*value);
        p.values.push_back(v);
        d.authored.parameters.push_back(p);
    }
    d.effective_authored.clear();
    for (std::size_t i = 0; i < d.authored.parameters.size(); ++i)
        d.effective_authored.push_back(i);
}
// Overrides apply only to this benchmark's checked copy, before rendering translation.
std::vector<std::string> overrides(pbrt::Result &checked, const Options &o) {
    std::vector<std::string> provenance;
    for (const auto &[name, type] :
         {std::pair{"Sampler", "independent"}, std::pair{"Film", "rgb"}}) {
        if (std::ranges::none_of(checked.scene->options,
                                 [&](const auto &d) { return d.authored.name == name; })) {
            pbrt::Descriptor descriptor;
            descriptor.authored.name = name;
            pbrt::Value value;
            value.kind = pbrt::ValueKind::string;
            value.text = type;
            descriptor.authored.arguments.push_back(value);
            checked.scene->options.push_back(std::move(descriptor));
            provenance.push_back(std::string("implicit ") + name +
                                 " overridden for pipeline qualification");
        }
    }
    for (auto &d : checked.scene->options) {
        if (d.authored.name == "Film" || d.authored.name == "Sampler" ||
            d.authored.name == "Integrator") {
            const auto retain = [&](const auto &p, std::string_view source) {
                std::string text =
                    std::string(source) + " " + d.authored.name + " " + p.type + " " + p.name;
                for (const auto &v : p.values)
                    text += " " + v.text;
                provenance.push_back(text);
            };
            for (const auto &p : d.authored.parameters)
                retain(p, "authored");
            for (const auto &p : d.inherited)
                retain(*p.parameter, "inherited");
        }
        if (d.authored.name == "Integrator") {
            provenance.push_back("authored Integrator " + d.authored.arguments.front().text);
            if (d.authored.arguments.front().text == "bdpt")
                d.authored.arguments.front().text = "path";
        } else if (d.authored.name == "Sampler") {
            provenance.push_back("authored Sampler " + d.authored.arguments.front().text);
            d.authored.arguments.front().text = "independent";
            d.authored.parameters.clear();
            d.inherited.clear();
            d.effective_authored.clear();
            set_parameter(d, "pixelsamples", 1);
        } else if (d.authored.name == "Film") {
            set_parameter(d, "xresolution", std::int32_t(o.width));
            set_parameter(d, "yresolution", std::int32_t(o.height));
            set_parameter(d, "maxcomponentvalue", {});
        }
    }
    return provenance;
}

struct Scene {
    RenderScene value;
    std::vector<std::string> provenance;
    std::vector<fs::path> dependencies;
};
Scene load_scene(const Options &o) {
    auto checked = pbrt::check(o.scene, resource_limits(o.resource_profile).pbrt);
    require(checked.exit_code() == 0 && checked.scene.has_value(), pbrt::report(checked));
    Scene result;
    result.dependencies.push_back(o.scene);
    for (const auto &dependency : checked.document.dependencies)
        result.dependencies.push_back(dependency.resolved_path);
    result.provenance = overrides(checked, o);
    auto translated =
        pbrt::translate(checked, {o.scene.string()}, resource_limits(o.resource_profile).pbrt,
                        o.spectral_transport);
    for (const auto &diagnostic : translated.diagnostics)
        std::cerr << pbrt::format_diagnostic(diagnostic);
    require(translated.exit_code() == 0 && translated.scene.has_value(),
            "Qualification PBRT translation failed");
    result.provenance.insert(result.provenance.end(), translated.provenance.begin(),
                             translated.provenance.end());
    result.value = std::move(*translated.scene);
    auto &scene = result.value;
    if (o.motion_offset)
        require(std::ranges::none_of(
                    scene.instance_media,
                    [](const auto &medium) { return medium.inside != medium.outside; }),
                "Motion cannot classify distinct medium interfaces");

    scene.integrator = scene.volume_transport ? IntegratorKind::volpath : IntegratorKind::path;
    scene.max_component_value = std::numeric_limits<double>::infinity();
    std::string spectral_provenance;
    const auto spectral_error =
        select_spectral_transport(scene, o.spectral_transport, &spectral_provenance);
    require(spectral_error.empty(), spectral_error);
    result.provenance.push_back(spectral_provenance);
    result.provenance.push_back("all arms and reference use native wavefront, independent tape, "
                                "fixed footprint_samples=1, material_seed=0, unexposed output");
    return result;
}
