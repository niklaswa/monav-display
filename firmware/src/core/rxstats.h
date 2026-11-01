#pragma once

#include <atomic>
#include <cstdint>

/**
 * What came in from the phone since the last frame stats line: printed with it every 10 s, so the
 * serial log says whether the phone sends its position, scenes and map images at all.
 */
namespace monav::rxstats {
inline std::atomic<uint32_t> vehicle{0}, vehicleWithPos{0}, scene{0}, config{0}, bye{0};
inline std::atomic<uint32_t> mapBegin{0}, mapRejected{0}, routeBegin{0}, bulkBytes{0};
}  // namespace monav::rxstats
