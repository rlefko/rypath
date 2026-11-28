// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// Included inside rypath_bdpt. Observers run synchronously while the original
// endpoint/direct helper's factors are live; no speculative terminal write can
// overwrite an earlier WRS winner's immutable suffix.
namespace enhanced_suffix_runtime {
namespace tape = ::rypath_enhanced_suffix_tape;
namespace ew = ::rypath::enhanced_wire;

template <typename Sink, typename Lambda> struct Capture {
    thread Sink &sink;
    thread wire::Sample &sample;
    const thread Lambda &lambda;
    uint slot, prepared_mask;
    bool supported;

    bool selected(float4 term) thread {
        return sample.status == wire::success && !sink.state.failed && sink.add(slot, term, lambda);
    }
    float4 plain(float4 term) thread {
        if (selected(term))
            sink.cache_sink().invalidate_suffix();
        return term;
    }
    bool publishable(float4 term) const thread {
        return supported && sink.has_interior() && nonzero(term) &&
               sink.state.suffix_count <= tape::maximum_operations &&
               slot / 2u > sink.state.hybrid.active_record.event_ordinal &&
               sink.state.suffix_count ==
                   slot / 2u - sink.state.hybrid.active_record.event_ordinal - 1u;
    }
    void publish(float4 term, float4 a, float4 b, float cosine, float4 bsdf_pdf, float light_pdf,
                 float3 immediate_direction, uint pdf_mask, bool direct, bool delta) thread {
        if (!selected(term))
            return;
        const uint owner = sink.cache_sink().suffix_capture_index();
        if (owner == ew::invalid)
            return; // Includes immutable source==result imports, even fresh-bank reverses.
        device auto &entry = sink.cache_sink().suffixes[owner];
        entry.version = 0;
        if (!publishable(term))
            return;
        const auto receipt = sink.cache_sink().evaluations[sink.cache_sink().pixel];
        for (uint k = 0; k < 4; ++k) {
            entry.factor_a[k] = a[k];
            entry.factor_b[k] = b[k];
            entry.bsdf_pdf[k] = bsdf_pdf[k];
        }
        entry.cosine = cosine;
        entry.light_pdf = light_pdf;
        for (uint k = 0; k < 3; ++k)
            entry.immediate_direction[k] = immediate_direction[k];
        entry.flags = sink.state.suffix_count | tape::required_capture_flags |
                      (direct ? tape::nee : 0u) |
                      (delta ? tape::delta_light | tape::mis_bypass : 0u);
        // Response values are wavelength-local, but unprepared source lanes
        // remain missing data. Never turn them into a physically black lane.
        entry.coverage = sink.state.suffix_coverage |
                         (prepared_mask << tape::terminal_prepared_shift) |
                         (prepared_mask << tape::terminal_response_shift) |
                         (pdf_mask << tape::terminal_pdf_shift);
        entry.selected_slot = slot;
        entry.pivot_event = sink.state.hybrid.active_record.event_ordinal;
        entry.context_low = receipt.suffix_context_low;
        entry.context_high = receipt.suffix_context_high;
        entry.version = tape::version; // Published with the containing record at the next barrier.
    }
    float4 endpoint(float4 term, float4 radiance, float4 bsdf_pdf, float light_pdf,
                    float3 represented_direction, uint pdf_mask) thread {
        publish(term, radiance, float4(0), 0, bsdf_pdf, light_pdf, represented_direction, pdf_mask,
                false, false);
        return term;
    }
    float4 direct(float4 term, float4 f, float4 incident, float cosine, float4 bsdf_pdf,
                  float light_pdf, uint pdf_mask, bool delta) thread {
        publish(term, f, incident, cosine, bsdf_pdf, light_pdf, float3(0), pdf_mask, true, delta);
        return term;
    }
};
} // namespace enhanced_suffix_runtime
