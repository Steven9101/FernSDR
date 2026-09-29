// Reads IQ from a file, FIFO, or standard input.
//
// This is the path most real hardware takes:
//   rx888_stream -f 0 -s 1536000 | fernsdr fernsdr.conf
//   rtl_sdr -f 7100000 -s 2048000 - | fernsdr fernsdr.conf
// A recorded file replays at its true rate, which makes a fault reproducible
// off-air.
#pragma once
#include <memory>
#include <string>

#include "source.h"

namespace fernsdr {

std::unique_ptr<Source> make_file_source(const ConfigSection& section, std::string& error);

}  // namespace fernsdr
