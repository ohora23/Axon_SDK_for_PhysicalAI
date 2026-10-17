// SPDX-License-Identifier: Apache-2.0
// Refuse to benchmark an unbounded ROS 2 message that cannot fit Fast DDS's
// shared-memory segment. Fast DDS then sends it as UDP fragments, and under
// best-effort one lost fragment loses the whole frame: a silent drop that looks
// like a transport result. rclcpp offers no way to ask Fast DDS for the segment
// size, so this reads it from the XML profile the run uses.
#pragma once

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>

namespace axon_bench {

// Fast DDS's default SHM segment (no <segment_size> in the profile).
constexpr std::size_t kFastDdsDefaultShmSegment = 512 * 1024;

// The shared-memory segment size the current FASTRTPS_DEFAULT_PROFILES_FILE sets,
// or the Fast DDS default. ponytail: a regex on the first <segment_size>, not an
// XML parser; enough for the profiles this directory ships.
inline std::size_t fastdds_shm_segment_bytes() {
    const char* path = std::getenv("FASTRTPS_DEFAULT_PROFILES_FILE");
    if (!path) return kFastDdsDefaultShmSegment;
    std::ifstream f(path);
    std::stringstream ss;
    ss << f.rdbuf();
    std::smatch m;
    const std::string xml = ss.str();
    if (std::regex_search(xml, m, std::regex(R"(<segment_size>\s*(\d+)\s*</segment_size>)")))
        return static_cast<std::size_t>(std::stoull(m[1].str()));
    return kFastDdsDefaultShmSegment;
}

// Exit with status 2 if an unbounded message of `bytes` cannot go through shared
// memory. A segment holds whole samples plus Fast DDS's own bookkeeping, so the
// payload must leave headroom. ponytail: "at most half the segment" is a
// conservative heuristic, not Fast DDS's exact overhead.
inline void require_fits_shm_segment(std::size_t bytes, const char* what) {
    const std::size_t seg = fastdds_shm_segment_bytes();
    if (bytes <= seg / 2) return;
    std::fprintf(stderr,
                 "error: %s is %zu bytes but the Fast DDS shared-memory segment is %zu bytes.\n"
                 "  Fast DDS would send it as UDP fragments and drop whole frames under best-effort.\n"
                 "  Raise <segment_size> in %s (at least %zu).\n",
                 what, bytes, seg,
                 std::getenv("FASTRTPS_DEFAULT_PROFILES_FILE") ? std::getenv("FASTRTPS_DEFAULT_PROFILES_FILE")
                                                               : "a Fast DDS XML profile",
                 2 * bytes);
    std::exit(2);
}

}  // namespace axon_bench
