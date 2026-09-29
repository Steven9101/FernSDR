// Receives IQ over UDP, which is how ka9q-radio and similar distribute a
// channel.  Optionally strips an RTP header.
#pragma once
#include <memory>
#include <string>

#include "source.h"

namespace fernsdr {

std::unique_ptr<Source> make_udp_source(const ConfigSection& section, std::string& error);

}  // namespace fernsdr
