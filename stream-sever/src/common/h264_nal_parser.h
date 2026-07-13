/**
 * @file h264_nal_parser.h
 * @brief H.264 Annex-B NAL 单元解析工具
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace h264 {

struct NalUnit {
    const uint8_t* annexb_start = nullptr;  ///< 含 start code 的起始地址
    size_t annexb_size = 0;                 ///< 含 start code 的 NAL 大小
    const uint8_t* start = nullptr;         ///< 不含 start code 的起始地址
    size_t size = 0;                        ///< 不含 start code 的 NAL 大小
    uint8_t type = 0;                       ///< H.264 NAL unit type
};

inline bool IsStartCode3(const uint8_t* data, size_t size, size_t offset) {
    return offset + 3 <= size && data[offset] == 0 && data[offset + 1] == 0 && data[offset + 2] == 1;
}

inline bool IsStartCode4(const uint8_t* data, size_t size, size_t offset) {
    return offset + 4 <= size && data[offset] == 0 && data[offset + 1] == 0 &&
           data[offset + 2] == 0 && data[offset + 3] == 1;
}

inline bool FindNextNal(const uint8_t* data, size_t size, size_t& offset, NalUnit& out) {
    if (!data || offset >= size) {
        return false;
    }

    size_t start_code = offset;
    size_t start_code_len = 0;
    while (start_code + 3 <= size) {
        if (IsStartCode3(data, size, start_code)) {
            start_code_len = 3;
            break;
        }
        if (IsStartCode4(data, size, start_code)) {
            start_code_len = 4;
            break;
        }
        ++start_code;
    }

    if (start_code_len == 0) {
        offset = size;
        return false;
    }

    const size_t nal_start = start_code + start_code_len;
    if (nal_start >= size) {
        offset = size;
        return false;
    }

    size_t nal_end = nal_start;
    while (nal_end + 3 <= size) {
        if (IsStartCode3(data, size, nal_end) || IsStartCode4(data, size, nal_end)) {
            break;
        }
        ++nal_end;
    }
    if (nal_end + 3 > size) {
        nal_end = size;
    }

    out.annexb_start = data + start_code;
    out.annexb_size = nal_end - start_code;
    out.start = data + nal_start;
    out.size = nal_end - nal_start;
    out.type = out.size > 0 ? (out.start[0] & 0x1F) : 0;

    offset = nal_end;
    return out.size > 0;
}

inline bool FindSpsPps(const uint8_t* data, size_t size, NalUnit* sps, NalUnit* pps) {
    bool found_sps = false;
    bool found_pps = false;
    size_t offset = 0;
    NalUnit nal;

    while (FindNextNal(data, size, offset, nal)) {
        if (nal.type == 7 && sps && !found_sps) {
            *sps = nal;
            found_sps = true;
        } else if (nal.type == 8 && pps && !found_pps) {
            *pps = nal;
            found_pps = true;
        }

        if ((!sps || found_sps) && (!pps || found_pps)) {
            return true;
        }
    }

    return (!sps || found_sps) && (!pps || found_pps);
}

inline bool IsKeyframe(const uint8_t* data, size_t size) {
    size_t offset = 0;
    NalUnit nal;
    while (FindNextNal(data, size, offset, nal)) {
        if (nal.type == 5 || nal.type == 7) {
            return true;
        }
    }
    return false;
}

} // namespace h264
