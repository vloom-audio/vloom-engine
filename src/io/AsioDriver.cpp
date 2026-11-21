// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vloom Audio contributors
#include "io/AsioDriver.hpp"

#include <algorithm>

namespace vloom::io {

std::string asioErrorName(std::int32_t error)
{
    switch (error) {
    case asio::kOk:
        return "ASE_OK";
    case asio::kSuccess:
        return "ASE_SUCCESS";
    case asio::kNotPresent:
        return "ASE_NotPresent";
    case asio::kHwMalfunction:
        return "ASE_HWMalfunction";
    case asio::kInvalidParameter:
        return "ASE_InvalidParameter";
    case asio::kInvalidMode:
        return "ASE_InvalidMode";
    case asio::kSpNotAdvancing:
        return "ASE_SPNotAdvancing";
    case asio::kNoClock:
        return "ASE_NoClock";
    case asio::kNoMemory:
        return "ASE_NoMemory";
    default:
        return "ASIO error " + std::to_string(error);
    }
}

std::string asioSampleTypeName(std::int32_t type)
{
    switch (type) {
    case asio::kInt16Msb:
        return "Int16MSB";
    case asio::kInt24Msb:
        return "Int24MSB";
    case asio::kInt32Msb:
        return "Int32MSB";
    case asio::kFloat32Msb:
        return "Float32MSB";
    case asio::kFloat64Msb:
        return "Float64MSB";
    case asio::kInt32Msb16:
        return "Int32MSB16";
    case asio::kInt32Msb18:
        return "Int32MSB18";
    case asio::kInt32Msb20:
        return "Int32MSB20";
    case asio::kInt32Msb24:
        return "Int32MSB24";
    case asio::kInt16Lsb:
        return "Int16LSB";
    case asio::kInt24Lsb:
        return "Int24LSB";
    case asio::kInt32Lsb:
        return "Int32LSB";
    case asio::kFloat32Lsb:
        return "Float32LSB";
    case asio::kFloat64Lsb:
        return "Float64LSB";
    case asio::kInt32Lsb16:
        return "Int32LSB16";
    case asio::kInt32Lsb18:
        return "Int32LSB18";
    case asio::kInt32Lsb20:
        return "Int32LSB20";
    case asio::kInt32Lsb24:
        return "Int32LSB24";
    case asio::kDsdInt8Lsb1:
        return "DSDInt8LSB1";
    case asio::kDsdInt8Msb1:
        return "DSDInt8MSB1";
    case asio::kDsdInt8Ner8:
        return "DSDInt8NER8";
    default:
        return "type " + std::to_string(type);
    }
}

std::optional<SampleFormat> sampleFormatForAsioType(std::int32_t type) noexcept
{
    switch (type) {
    case asio::kInt16Lsb:
        return SampleFormat::Int16;
    case asio::kInt24Lsb:
        return SampleFormat::Int24Packed;
    case asio::kInt32Lsb:
        return SampleFormat::Int32;
    case asio::kFloat32Lsb:
        return SampleFormat::Float32;
    case asio::kFloat64Lsb:
        return SampleFormat::Float64;
    case asio::kInt32Lsb16:
        return SampleFormat::Int32Lsb16;
    case asio::kInt32Lsb18:
        return SampleFormat::Int32Lsb18;
    case asio::kInt32Lsb20:
        return SampleFormat::Int32Lsb20;
    case asio::kInt32Lsb24:
        return SampleFormat::Int32Lsb24;
    default:
        return std::nullopt;
    }
}

bool isValidAsioBufferSize(int frames, const AsioBufferSizes& sizes) noexcept
{
    if (frames <= 0 || sizes.minSize <= 0 || frames < sizes.minSize
        || frames > std::max(sizes.maxSize, sizes.minSize))
        return false;
    if (sizes.minSize == sizes.maxSize)
        return frames == sizes.minSize;
    if (sizes.granularity == -1) {
        // minSize, then doubled up to maxSize.
        std::int64_t size = sizes.minSize;
        while (size < frames)
            size *= 2;
        return size == frames;
    }
    if (sizes.granularity > 0)
        return (frames - sizes.minSize) % sizes.granularity == 0;
    // 0 or another negative step with a range: the spec gives no rule, so
    // only the preferred size counts.
    return frames == sizes.preferred;
}

int chooseAsioBufferSize(int requested, const AsioBufferSizes& sizes) noexcept
{
    if (isValidAsioBufferSize(requested, sizes))
        return requested;
    if (isValidAsioBufferSize(sizes.preferred, sizes))
        return sizes.preferred;
    if (sizes.minSize > 0)
        return sizes.minSize;
    return std::max(sizes.preferred, 0);
}

std::int32_t handleAsioMessage(
    std::int32_t selector, std::int32_t value, std::atomic<std::uint32_t>& resets) noexcept
{
    switch (selector) {
    case asio::kSelectorSupported:
        switch (value) {
        case asio::kSelectorSupported:
        case asio::kEngineVersion:
        case asio::kResetRequest:
        case asio::kBufferSizeChange:
        case asio::kResyncRequest:
        case asio::kLatenciesChanged:
        case asio::kSupportsTimeInfo:
            return 1;
        default:
            return 0;
        }
    case asio::kEngineVersion:
        return asio::kHostAsioVersion;
    case asio::kResetRequest:
        resets.fetch_or(bit(AsioReset::Request));
        return 1;
    case asio::kBufferSizeChange:
        resets.fetch_or(bit(AsioReset::BufferSize));
        return 1;
    case asio::kResyncRequest:
        resets.fetch_or(bit(AsioReset::Resync));
        return 1;
    case asio::kLatenciesChanged:
        resets.fetch_or(bit(AsioReset::Latencies));
        return 1;
    case asio::kSupportsTimeInfo:
        return 1;
    default:
        return 0;
    }
}

} // namespace vloom::io
