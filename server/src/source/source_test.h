// Synthetic band generator.
//
// Not a toy: this is how the whole chain is exercised end to end without
// hardware, how a new deployment is smoke-tested before the antenna is
// connected, and how the integration tests verify that a signal put in at a
// known frequency comes out of the codec at the right pitch and level.
#pragma once
#include <memory>
#include <string>

#include "source.h"

namespace fernsdr {

std::unique_ptr<Source> make_test_source(const ConfigSection& section, std::string& error);

}  // namespace fernsdr
