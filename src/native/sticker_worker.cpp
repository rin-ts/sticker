#include "sticker_pipeline.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using Byte = std::uint8_t;

constexpr std::array<Byte, 4U> kRequestMagic = {'S', 'T', 'K', '1'};
constexpr std::array<Byte, 4U> kResponseMagic = {'S', 'T', 'R', '1'};
constexpr Byte kResponseProtocolVersion = 1U;
constexpr Byte kRequestProtocolVersion1 = 1U;
constexpr Byte kRequestProtocolVersion2 = 2U;
constexpr Byte kCommandTranscode = 1U;
constexpr Byte kTrimFlag = 1U << 0U;
constexpr Byte kPreferLosslessFlag = 1U << 1U;
constexpr Byte kSupportedRequestFlags = kTrimFlag | kPreferLosslessFlag;
constexpr Byte kUnsupportedRequestFlags =
    static_cast<Byte>(~kSupportedRequestFlags);
constexpr std::size_t kRequestHeaderSize = 18U;
constexpr std::size_t kExifRequestHeaderSize = 22U;
constexpr std::size_t kResponseHeaderSize = 26U;
constexpr std::size_t kMaximumFrameBytes =
    static_cast<std::size_t>(17U) * 1024U * 1024U;
constexpr std::size_t kMaximumExifMetadataBytes = 4096U;

enum class ResponseStatus : Byte {
  kOk = 0U,
  kInvalidRequest = 1U,
  kProcessingError = 2U,
};

struct WorkerRequest {
  sticker::TranscodeOptions options;
  std::vector<Byte> input;
};

struct WorkerResponse {
  ResponseStatus status = ResponseStatus::kProcessingError;
  sticker::ErrorCode error_code = sticker::ErrorCode::kInternal;
  sticker::StickerResult result;
  std::string message;
};

[[nodiscard]] std::uint32_t read_u32(const std::span<const Byte> bytes,
                                     const std::size_t offset) {
  return static_cast<std::uint32_t>(bytes[offset]) |
         (static_cast<std::uint32_t>(bytes[offset + 1U]) << 8U) |
         (static_cast<std::uint32_t>(bytes[offset + 2U]) << 16U) |
         (static_cast<std::uint32_t>(bytes[offset + 3U]) << 24U);
}

void write_u16(std::vector<Byte> &bytes, const std::size_t offset,
               const std::uint16_t value) {
  bytes[offset] = static_cast<Byte>(value & 0xffU);
  bytes[offset + 1U] = static_cast<Byte>((value >> 8U) & 0xffU);
}

void write_u32(std::vector<Byte> &bytes, const std::size_t offset,
               const std::uint32_t value) {
  bytes[offset] = static_cast<Byte>(value & 0xffU);
  bytes[offset + 1U] = static_cast<Byte>((value >> 8U) & 0xffU);
  bytes[offset + 2U] = static_cast<Byte>((value >> 16U) & 0xffU);
  bytes[offset + 3U] = static_cast<Byte>((value >> 24U) & 0xffU);
}

[[nodiscard]] bool read_exact(std::istream &input, Byte *const destination,
                              const std::size_t byte_count) {
  input.read(reinterpret_cast<char *>(destination),
             static_cast<std::streamsize>(byte_count));
  return input.good() || static_cast<std::size_t>(input.gcount()) == byte_count;
}

[[nodiscard]] bool read_frame(std::istream &input, std::vector<Byte> &frame,
                              bool &clean_end_of_file) {
  std::array<Byte, 4U> length_bytes{};
  input.read(reinterpret_cast<char *>(length_bytes.data()),
             static_cast<std::streamsize>(length_bytes.size()));
  const std::streamsize read_count = input.gcount();
  if (read_count == 0 && input.eof()) {
    clean_end_of_file = true;
    return false;
  }
  if (read_count != static_cast<std::streamsize>(length_bytes.size())) {
    clean_end_of_file = false;
    return false;
  }

  const std::uint32_t length = read_u32(length_bytes, 0U);
  if (length == 0U || length > kMaximumFrameBytes) {
    clean_end_of_file = false;
    return false;
  }
  frame.resize(length);
  if (!read_exact(input, frame.data(), frame.size())) {
    clean_end_of_file = false;
    return false;
  }
  clean_end_of_file = false;
  return true;
}

[[nodiscard]] WorkerRequest parse_request(const std::span<const Byte> frame) {
  if (frame.size() < kRequestHeaderSize ||
      std::memcmp(frame.data(), kRequestMagic.data(), kRequestMagic.size()) !=
          0) {
    throw sticker::StickerError(sticker::ErrorCode::kInvalidInput,
                                "invalid sticker worker request");
  }
  const Byte version = frame[4U];
  const std::size_t header_size =
      version == kRequestProtocolVersion1   ? kRequestHeaderSize
      : version == kRequestProtocolVersion2 ? kExifRequestHeaderSize
                                            : 0U;
  if (header_size == 0U || frame.size() < header_size ||
      frame[5U] != kCommandTranscode || frame[7U] != 0U) {
    throw sticker::StickerError(
        sticker::ErrorCode::kInvalidInput,
        "unsupported sticker worker request version or command");
  }

  const Byte flags = frame[6U];
  if ((flags & kUnsupportedRequestFlags) != 0) {
    throw sticker::StickerError(
        sticker::ErrorCode::kInvalidInput,
        "sticker worker request contains unsupported flags");
  }

  const std::uint32_t input_length = read_u32(frame, 14U);
  const std::uint32_t exif_length =
      version == kRequestProtocolVersion2 ? read_u32(frame, 18U) : 0U;
  const std::size_t actual_payload_length = frame.size() - header_size;
  if (input_length > actual_payload_length ||
      exif_length > kMaximumExifMetadataBytes ||
      exif_length != actual_payload_length - input_length) {
    throw sticker::StickerError(
        sticker::ErrorCode::kInvalidInput,
        "sticker worker request length does not match its payload");
  }

  const std::uint8_t outline_width = frame[9U];
  if (outline_width > 64U) {
    throw sticker::StickerError(
        sticker::ErrorCode::kInvalidInput,
        "sticker worker outline width is outside the supported range");
  }

  WorkerRequest request{
      .options =
          sticker::TranscodeOptions{
              .trim = (flags & kTrimFlag) != 0,
              .alpha_threshold = frame[8U],
              .outline_width = outline_width,
              .outline_color =
                  sticker::RgbaColor{
                      .red = frame[10U],
                      .green = frame[11U],
                      .blue = frame[12U],
                      .alpha = frame[13U],
                  },
              .prefer_lossless = (flags & kPreferLosslessFlag) != 0,
              .exif_json =
                  exif_length == 0U
                      ? std::nullopt
                      : std::optional<std::vector<Byte>>(
                            std::in_place,
                            frame.begin() + static_cast<std::ptrdiff_t>(
                                                header_size + input_length),
                            frame.end()),
          },
      .input = std::vector<Byte>(
          frame.begin() + static_cast<std::ptrdiff_t>(header_size),
          frame.begin() +
              static_cast<std::ptrdiff_t>(header_size + input_length)),
  };
  return request;
}

[[nodiscard]] WorkerResponse process_request(const WorkerRequest &request) {
  try {
    return WorkerResponse{
        .status = ResponseStatus::kOk,
        .error_code = sticker::ErrorCode::kNone,
        .result = sticker::transcode(request.input, request.options),
        .message = {},
    };
  } catch (const sticker::StickerError &error) {
    return WorkerResponse{
        .status = ResponseStatus::kProcessingError,
        .error_code = error.code(),
        .result = {},
        .message = error.what(),
    };
  } catch (const std::exception &error) {
    return WorkerResponse{
        .status = ResponseStatus::kProcessingError,
        .error_code = sticker::ErrorCode::kInternal,
        .result = {},
        .message = error.what(),
    };
  }
}

[[nodiscard]] std::vector<Byte>
encode_response(const WorkerResponse &response) {
  const std::size_t output_size = response.result.bytes.size();
  const std::size_t message_size = response.message.size();
  if (output_size > std::numeric_limits<std::uint32_t>::max() ||
      message_size > std::numeric_limits<std::uint16_t>::max()) {
    throw std::runtime_error("sticker worker response exceeds protocol limits");
  }

  std::vector<Byte> frame(kResponseHeaderSize + output_size + message_size);
  std::memcpy(frame.data(), kResponseMagic.data(), kResponseMagic.size());
  frame[4U] = kResponseProtocolVersion;
  frame[5U] = static_cast<Byte>(response.status);
  frame[6U] = static_cast<Byte>(response.error_code);
  frame[7U] = response.result.animated ? 1U : 0U;
  write_u16(frame, 8U, static_cast<std::uint16_t>(response.result.width));
  write_u16(frame, 10U, static_cast<std::uint16_t>(response.result.height));
  write_u32(frame, 12U, response.result.frame_count);
  write_u32(frame, 16U, response.result.duration_ms);
  write_u32(frame, 20U, static_cast<std::uint32_t>(output_size));
  write_u16(frame, 24U, static_cast<std::uint16_t>(message_size));

  if (output_size != 0U) {
    std::memcpy(frame.data() + kResponseHeaderSize,
                response.result.bytes.data(), output_size);
  }
  if (message_size != 0U) {
    std::memcpy(frame.data() + kResponseHeaderSize + output_size,
                response.message.data(), message_size);
  }
  return frame;
}

void write_frame(std::ostream &output, const std::span<const Byte> frame) {
  if (frame.size() > std::numeric_limits<std::uint32_t>::max()) {
    throw std::runtime_error("sticker worker response frame is too large");
  }
  const std::uint32_t length = static_cast<std::uint32_t>(frame.size());
  const std::array<Byte, 4U> length_bytes = {
      static_cast<Byte>(length & 0xffU),
      static_cast<Byte>((length >> 8U) & 0xffU),
      static_cast<Byte>((length >> 16U) & 0xffU),
      static_cast<Byte>((length >> 24U) & 0xffU),
  };
  output.write(reinterpret_cast<const char *>(length_bytes.data()),
               static_cast<std::streamsize>(length_bytes.size()));
  output.write(reinterpret_cast<const char *>(frame.data()),
               static_cast<std::streamsize>(frame.size()));
  output.flush();
  if (!output) {
    throw std::runtime_error("unable to write sticker worker response");
  }
}

[[nodiscard]] WorkerResponse
invalid_request_response(const std::string &message) {
  return WorkerResponse{
      .status = ResponseStatus::kInvalidRequest,
      .error_code = sticker::ErrorCode::kInvalidInput,
      .result = {},
      .message = message,
  };
}

} // namespace

int main() {
  std::ios::sync_with_stdio(false);
  std::cin.tie(nullptr);

  while (true) {
    std::vector<Byte> frame;
    bool clean_end_of_file = false;
    if (!read_frame(std::cin, frame, clean_end_of_file)) {
      return clean_end_of_file ? 0 : 1;
    }

    WorkerResponse response{};
    try {
      response = process_request(parse_request(frame));
    } catch (const sticker::StickerError &error) {
      response = invalid_request_response(error.what());
    } catch (const std::exception &error) {
      response = invalid_request_response(error.what());
    }

    try {
      const std::vector<Byte> encoded = encode_response(response);
      write_frame(std::cout, encoded);
    } catch (const std::exception &) {
      return 1;
    }
  }
}
