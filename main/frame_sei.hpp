#pragma once

#include "frame_sync.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

enum class FrameVideoCodec {
    H264,
    H265,
};

// Builds one Annex-B user_data_unregistered SEI NAL.
std::vector<std::uint8_t> buildFrameSyncSei(FrameVideoCodec codec,
                                            const FrameSyncInfo& info);

// Parses the raw payload returned by FFmpeg AV_FRAME_DATA_SEI_UNREGISTERED.
// The buffer must begin with the 16-byte UUID.
bool parseFrameSyncSeiPayload(const std::uint8_t* data,
                              std::size_t size,
                              FrameSyncInfo& info);
