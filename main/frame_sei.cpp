#include "frame_sei.hpp"

#include <array>
#include <cstring>

namespace {

constexpr std::array<std::uint8_t, 16> kUuid = {
    0x52, 0x45, 0x43, 0x41, 0x4d, 0x45, 0x52, 0x41,
    0x46, 0x52, 0x41, 0x4d, 0x45, 0x49, 0x44, 0x01
};

constexpr std::array<std::uint8_t, 4> kMagic = {'R', 'C', 'F', 'S'};
constexpr std::uint8_t kVersion = 1;

void appendBe64(std::vector<std::uint8_t>& output, std::uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8) {
        output.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffU));
    }
}

std::uint64_t readBe64(const std::uint8_t* data) {
    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value = (value << 8U) | static_cast<std::uint64_t>(data[i]);
    }
    return value;
}

void appendSeiFieldValue(std::vector<std::uint8_t>& rbsp, std::size_t value) {
    while (value >= 255U) {
        rbsp.push_back(0xffU);
        value -= 255U;
    }
    rbsp.push_back(static_cast<std::uint8_t>(value));
}

std::vector<std::uint8_t> addEmulationPrevention(const std::vector<std::uint8_t>& rbsp) {
    std::vector<std::uint8_t> escaped;
    escaped.reserve(rbsp.size() + rbsp.size() / 32U + 4U);

    int zeroCount = 0;
    for (const std::uint8_t value : rbsp) {
        if (zeroCount >= 2 && value <= 0x03U) {
            escaped.push_back(0x03U);
            zeroCount = 0;
        }
        escaped.push_back(value);
        if (value == 0x00U) {
            ++zeroCount;
        } else {
            zeroCount = 0;
        }
    }
    return escaped;
}

}  // namespace

std::vector<std::uint8_t> buildFrameSyncSei(FrameVideoCodec codec,
                                            const FrameSyncInfo& info) {
    std::vector<std::uint8_t> payload;
    payload.reserve(16U + 4U + 4U + 32U);
    payload.insert(payload.end(), kUuid.begin(), kUuid.end());
    payload.insert(payload.end(), kMagic.begin(), kMagic.end());
    payload.push_back(kVersion);
    payload.push_back(static_cast<std::uint8_t>(info.exactPtsMatch ? 1U : 0U));
    payload.push_back(0U);
    payload.push_back(0U);
    appendBe64(payload, info.frameId);
    appendBe64(payload, info.sourcePts);
    appendBe64(payload, info.vencPts);
    appendBe64(payload, static_cast<std::uint64_t>(info.captureUnixMs));

    std::vector<std::uint8_t> rbsp;
    rbsp.reserve(payload.size() + 8U);
    appendSeiFieldValue(rbsp, 5U);  // user_data_unregistered
    appendSeiFieldValue(rbsp, payload.size());
    rbsp.insert(rbsp.end(), payload.begin(), payload.end());
    rbsp.push_back(0x80U);  // rbsp_trailing_bits

    const std::vector<std::uint8_t> escaped = addEmulationPrevention(rbsp);

    std::vector<std::uint8_t> nal;
    nal.reserve(4U + 2U + escaped.size());
    nal.insert(nal.end(), {0x00U, 0x00U, 0x00U, 0x01U});
    if (codec == FrameVideoCodec::H264) {
        nal.push_back(0x06U);  // H.264 SEI
    } else {
        nal.push_back(0x4eU);  // H.265 prefix SEI, nal_unit_type=39
        nal.push_back(0x01U);  // layer_id=0, temporal_id_plus1=1
    }
    nal.insert(nal.end(), escaped.begin(), escaped.end());
    return nal;
}

bool parseFrameSyncSeiPayload(const std::uint8_t* data,
                              std::size_t size,
                              FrameSyncInfo& info) {
    constexpr std::size_t kRequiredSize = 16U + 4U + 4U + 32U;
    if (data == nullptr || size < kRequiredSize) {
        return false;
    }
    if (std::memcmp(data, kUuid.data(), kUuid.size()) != 0) {
        return false;
    }

    const std::uint8_t* cursor = data + kUuid.size();
    if (std::memcmp(cursor, kMagic.data(), kMagic.size()) != 0) {
        return false;
    }
    cursor += kMagic.size();

    if (cursor[0] != kVersion) {
        return false;
    }
    info.exactPtsMatch = cursor[1] != 0U;
    cursor += 4U;

    info.frameId = readBe64(cursor);
    cursor += 8U;
    info.sourcePts = readBe64(cursor);
    cursor += 8U;
    info.vencPts = readBe64(cursor);
    cursor += 8U;
    info.captureUnixMs = static_cast<std::int64_t>(readBe64(cursor));
    return true;
}
