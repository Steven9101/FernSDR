// A band input delivered by a module: a separate program that FernSDR starts,
// speaks to over pipes and restarts when it fails. See docs/MODULES.md.
#pragma once
#include <chrono>
#include <memory>
#include <string>

#include "source.h"

namespace fernsdr {

// How long each step of a module's life may take. Tests shorten them.
struct ModuleTiming {
    std::chrono::milliseconds hello{5000};
    std::chrono::milliseconds ready{15000};
    std::chrono::milliseconds stall{2000};
    std::chrono::milliseconds grace{2000};
    std::chrono::milliseconds kill_wait{1000};
};

std::unique_ptr<Source> make_module_source(const ConfigSection& section, const std::shared_ptr<ModuleStore>& store,
                                           std::string& error, const ModuleTiming& timing = {},
                                           bool strict = false);

}  // namespace fernsdr
