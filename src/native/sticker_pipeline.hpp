#pragma once

#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace sticker {

enum class ErrorCode : std::uint8_t {
  kNone = 0,
  kInvalidInput = 1,
  kUnsupportedInput = 2,
  kResourceLimit = 3,
  kEncodingFailed = 4,
  kValidationFailed = 5,
  kInternal = 6,
};

class StickerError final : public std::runtime_error {
 public:
  StickerError(ErrorCode code, std::string message);

  [[nodiscard]] ErrorCode code() const noexcept;

 private:
  ErrorCode code_;
};

struct RgbaColor {
  std::uint8_t red = 255U;
  std::uint8_t green = 255U;
  std::uint8_t blue = 255U;
  std::uint8_t alpha = 255U;
};

struct TranscodeOptions {
  bool trim = true;
  std::uint8_t alpha_threshold = 0U;
  std::uint8_t outline_width = 0U;
  RgbaColor outline_color{};
};

struct StickerResult {
  std::vector<std::uint8_t> bytes;
  std::uint32_t width = 0U;
  std::uint32_t height = 0U;
  std::uint32_t frame_count = 0U;
  std::uint32_t duration_ms = 0U;
  bool animated = false;
};

/**
 * Converts a supported source asset into a WhatsApp-compatible WebP sticker.
 *
 * Supported sources are PNG, JPEG, static WebP, animated WebP, and video
 * formats with an FFmpeg decoder. The caller owns the source bytes; the
 * returned value owns the encoded output bytes.
 */
[[nodiscard]] StickerResult transcode(
    std::span<const std::uint8_t> source,
    const TranscodeOptions& options);

}  // namespace sticker
