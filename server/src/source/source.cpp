#include "source.h"

#include <cstring>

#include "source_file.h"
#include "source_module.h"
#include "source_test.h"
#include "source_udp.h"

namespace fernsdr {

bool sample_type_from_name(const std::string& name, SampleType& type, bool& implies_iq) {
    struct Entry { const char* name; SampleType type; bool iq; };
    static const Entry kEntries[] = {
        {"u8", SampleType::U8, false},   {"s8", SampleType::S8, false},
        {"u16", SampleType::U16, false}, {"s16", SampleType::S16, false},
        {"f32", SampleType::F32, false}, {"float", SampleType::F32, false},
        // Legacy spellings from the predecessor's configs; these name the
        // component type and also say the stream is complex.
        {"cu8", SampleType::U8, true},   {"cs8", SampleType::S8, true},
        {"cu16", SampleType::U16, true}, {"cs16", SampleType::S16, true},
        {"cf32", SampleType::F32, true},
    };
    for (const auto& entry : kEntries) {
        if (name == entry.name) {
            type = entry.type;
            implies_iq = entry.iq;
            return true;
        }
    }
    return false;
}

const char* sample_type_name(SampleType type) {
    switch (type) {
        case SampleType::U8: return "u8";
        case SampleType::S8: return "s8";
        case SampleType::U16: return "u16";
        case SampleType::S16: return "s16";
        case SampleType::F32: return "f32";
    }
    return "s16";
}

size_t bytes_per_component(SampleType type) {
    switch (type) {
        case SampleType::U8:
        case SampleType::S8: return 1;
        case SampleType::U16:
        case SampleType::S16: return 2;
        case SampleType::F32: return 4;
    }
    return 2;
}

namespace {

// Reads one component. Buffers off a socket or pipe are not guaranteed
// aligned, so the wide types are memcpy'd rather than cast.
inline float component(SampleType type, const uint8_t* raw) {
    switch (type) {
        case SampleType::U8:
            return (static_cast<float>(raw[0]) - 127.5f) * (1.0f / 127.5f);
        case SampleType::S8:
            return static_cast<float>(static_cast<int8_t>(raw[0])) * (1.0f / 128.0f);
        case SampleType::U16: {
            uint16_t v;
            std::memcpy(&v, raw, 2);
            return (static_cast<float>(v) - 32767.5f) * (1.0f / 32767.5f);
        }
        case SampleType::S16: {
            int16_t v;
            std::memcpy(&v, raw, 2);
            return static_cast<float>(v) * (1.0f / 32768.0f);
        }
        case SampleType::F32: {
            float v;
            std::memcpy(&v, raw, 4);
            return v;
        }
    }
    return 0.0f;
}

}  // namespace

void convert_iq(SampleType type, const uint8_t* raw, cfloat* out, size_t count) {
    const size_t stride = bytes_per_component(type);
    // The common cases get a tight loop the compiler can vectorise; the rest
    // fall back to the generic path.
    if (type == SampleType::S16) {
        for (size_t i = 0; i < count; i++) {
            int16_t re, im;
            std::memcpy(&re, raw + 4 * i, 2);
            std::memcpy(&im, raw + 4 * i + 2, 2);
            out[i] = cfloat(re * (1.0f / 32768.0f), im * (1.0f / 32768.0f));
        }
        return;
    }
    if (type == SampleType::U8) {
        for (size_t i = 0; i < count; i++) {
            out[i] = cfloat((static_cast<float>(raw[2 * i]) - 127.5f) * (1.0f / 127.5f),
                            (static_cast<float>(raw[2 * i + 1]) - 127.5f) * (1.0f / 127.5f));
        }
        return;
    }
    for (size_t i = 0; i < count; i++) {
        out[i] = cfloat(component(type, raw + 2 * stride * i),
                        component(type, raw + 2 * stride * i + stride));
    }
}

void convert_real(SampleType type, const uint8_t* raw, float* out, size_t count) {
    if (type == SampleType::S16) {
        for (size_t i = 0; i < count; i++) {
            int16_t v;
            std::memcpy(&v, raw + 2 * i, 2);
            out[i] = v * (1.0f / 32768.0f);
        }
        return;
    }
    const size_t stride = bytes_per_component(type);
    for (size_t i = 0; i < count; i++) out[i] = component(type, raw + stride * i);
}

bool parse_source_format(const ConfigSection& section, SourceFormat& out, std::string& error) {
    const std::string format_name = section.get("format", "s16");
    bool implies_iq = false;
    if (!sample_type_from_name(format_name, out.type, implies_iq)) {
        error = "[" + section.name() + "] unknown format '" + format_name +
                "'; expected u8, s8, u16, s16 or f32";
        return false;
    }

    const std::string signal = section.get("signal", implies_iq ? "iq" : "");
    if (signal == "iq") {
        out.kind = SignalKind::Iq;
    } else if (signal == "real") {
        out.kind = SignalKind::Real;
        if (implies_iq) {
            error = "[" + section.name() + "] format '" + format_name +
                    "' means complex samples, which contradicts signal = real; use '" +
                    sample_type_name(out.type) + "'";
            return false;
        }
    } else if (signal.empty()) {
        // No explicit choice and no complex format name: IQ is what almost
        // every front end delivers, so it stays the default.
        out.kind = SignalKind::Iq;
    } else {
        error = "[" + section.name() + "] signal must be 'iq' or 'real', not '" + signal + "'";
        return false;
    }
    return true;
}

std::unique_ptr<Source> make_source(const ConfigSection& section, const SourceContext& context, std::string& error) {
    const std::string kind = section.get("source", "test");

    if (kind == "test") return make_test_source(section, error);
    if (kind == "file" || kind == "pipe" || kind == "stdin") return make_file_source(section, error);
    if (kind == "udp") return make_udp_source(section, error);
    if (kind == "module") return make_module_source(section, context.modules, error);

    error = "unknown source '" + kind + "' in [" + section.name() +
            "]; expected one of: test, file, pipe, stdin, udp, module";
    return nullptr;
}

}  // namespace fernsdr
