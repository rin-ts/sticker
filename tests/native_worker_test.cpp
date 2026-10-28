#include "sticker_pipeline.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <webp/decode.h>
#include <webp/encode.h>
#include <webp/mux.h>

extern "C" {
#include <jpeglib.h>
}

namespace {

using Byte = std::uint8_t;

void require(const bool condition, const std::string &message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

[[nodiscard]] std::vector<Byte> encode_static_source() {
  constexpr int kWidth = 4;
  constexpr int kHeight = 4;
  const std::array<Byte, static_cast<std::size_t>(kWidth) * kHeight * 4U>
      pixels = {
          0U,   0U,   0U, 0U, 0U,   0U, 0U, 0U,   0U, 0U, 0U,   0U,   0U,
          0U,   0U,   0U, 0U, 0U,   0U, 0U, 255U, 0U, 0U, 255U, 255U, 0U,
          0U,   255U, 0U, 0U, 0U,   0U, 0U, 0U,   0U, 0U, 255U, 0U,   0U,
          255U, 255U, 0U, 0U, 255U, 0U, 0U, 0U,   0U, 0U, 0U,   0U,   0U,
          0U,   0U,   0U, 0U, 0U,   0U, 0U, 0U,   0U, 0U, 0U,   0U,
      };
  Byte *encoded = nullptr;
  const std::size_t encoded_size = WebPEncodeLosslessRGBA(
      pixels.data(), kWidth, kHeight, kWidth * 4, &encoded);
  require(encoded_size != 0U && encoded != nullptr,
          "failed to encode source WebP");
  const std::vector<Byte> result(encoded, encoded + encoded_size);
  WebPFree(encoded);
  return result;
}

[[nodiscard]] std::vector<Byte> encode_two_color_source() {
  constexpr int kWidth = 2;
  constexpr int kHeight = 1;
  const std::array<Byte, static_cast<std::size_t>(kWidth) * kHeight * 4U>
      pixels = {
          255U, 0U, 0U, 255U, 0U, 0U, 255U, 255U,
      };
  Byte *encoded = nullptr;
  const std::size_t encoded_size = WebPEncodeLosslessRGBA(
      pixels.data(), kWidth, kHeight, kWidth * 4, &encoded);
  require(encoded_size != 0U && encoded != nullptr,
          "failed to encode two-color source");
  const std::vector<Byte> result(encoded, encoded + encoded_size);
  WebPFree(encoded);
  return result;
}

[[nodiscard]] std::vector<Byte> decode_rgba(const std::vector<Byte> &bytes,
                                            int &width, int &height) {
  std::unique_ptr<Byte, decltype(&WebPFree)> decoded(
      WebPDecodeRGBA(bytes.data(), bytes.size(), &width, &height), WebPFree);
  require(decoded != nullptr, "failed to decode WebP output");
  require(width > 0 && height > 0, "decoded output has invalid dimensions");
  const std::size_t pixel_count =
      static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
  return {decoded.get(), decoded.get() + pixel_count * 4U};
}

[[nodiscard]] std::vector<Byte> encode_jpeg_source() {
  constexpr int kWidth = 2;
  constexpr int kHeight = 2;
  std::array<Byte, static_cast<std::size_t>(kWidth) * kHeight * 3U> pixels = {
      255U, 0U, 0U, 0U, 255U, 0U, 0U, 0U, 255U, 255U, 255U, 255U,
  };

  jpeg_compress_struct encoder{};
  jpeg_error_mgr error{};
  encoder.err = jpeg_std_error(&error);
  jpeg_create_compress(&encoder);

  unsigned char *encoded = nullptr;
  unsigned long encoded_size = 0UL;
  jpeg_mem_dest(&encoder, &encoded, &encoded_size);
  encoder.image_width = kWidth;
  encoder.image_height = kHeight;
  encoder.input_components = 3;
  encoder.in_color_space = JCS_RGB;
  jpeg_set_defaults(&encoder);
  jpeg_set_quality(&encoder, 90, TRUE);
  jpeg_start_compress(&encoder, TRUE);
  while (encoder.next_scanline < encoder.image_height) {
    const std::size_t offset =
        static_cast<std::size_t>(encoder.next_scanline) * kWidth * 3U;
    JSAMPROW row = pixels.data() + offset;
    require(jpeg_write_scanlines(&encoder, &row, 1U) == 1U,
            "failed to write JPEG source scanline");
  }
  jpeg_finish_compress(&encoder);
  jpeg_destroy_compress(&encoder);

  require(encoded != nullptr && encoded_size != 0UL,
          "failed to encode JPEG source");
  std::unique_ptr<unsigned char, decltype(&std::free)> owned_encoded(
      encoded, &std::free);
  const std::vector<Byte> result(owned_encoded.get(),
                                 owned_encoded.get() + encoded_size);
  return result;
}

class Picture final {
public:
  Picture(const std::array<Byte, 16U> &pixels) {
    require(WebPPictureInit(&picture_) != 0,
            "failed to initialize test picture");
    picture_.width = 2;
    picture_.height = 2;
    require(WebPPictureImportRGBA(&picture_, pixels.data(), 2 * 4) != 0,
            "failed to import test picture");
  }

  ~Picture() { WebPPictureFree(&picture_); }

  Picture(const Picture &) = delete;
  Picture &operator=(const Picture &) = delete;

  [[nodiscard]] WebPPicture *get() { return &picture_; }

private:
  WebPPicture picture_{};
};

[[nodiscard]] std::vector<Byte> encode_animated_source() {
  WebPAnimEncoderOptions options{};
  require(WebPAnimEncoderOptionsInit(&options) != 0,
          "failed to initialize animation options");
  options.anim_params.loop_count = 0;
  options.minimize_size = 1;

  WebPAnimEncoder *const encoder = WebPAnimEncoderNew(2, 2, &options);
  require(encoder != nullptr, "failed to create source animation encoder");

  WebPConfig config{};
  require(WebPConfigInit(&config) != 0, "failed to initialize source config");
  config.lossless = 1;
  config.method = 6;

  const std::array<Byte, 16U> first = {
      255U, 0U, 0U, 255U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U,
  };
  const std::array<Byte, 16U> second = {
      0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 255U, 0U, 255U,
  };
  {
    Picture picture(first);
    require(WebPAnimEncoderAdd(encoder, picture.get(), 0, &config) != 0,
            "failed to add first source animation frame");
  }
  {
    Picture picture(second);
    require(WebPAnimEncoderAdd(encoder, picture.get(), 100, &config) != 0,
            "failed to add second source animation frame");
  }
  require(WebPAnimEncoderAdd(encoder, nullptr, 200, nullptr) != 0,
          "failed to finalize source animation timing");

  WebPData data{};
  WebPDataInit(&data);
  const int assembled = WebPAnimEncoderAssemble(encoder, &data);
  WebPAnimEncoderDelete(encoder);
  require(assembled != 0, "failed to assemble source animation");
  const std::vector<Byte> result(data.bytes, data.bytes + data.size);
  WebPDataClear(&data);
  return result;
}

void test_static_transcode() {
  const std::vector<Byte> source = encode_static_source();
  const sticker::StickerResult result =
      sticker::transcode(source, sticker::TranscodeOptions{
                                     .trim = true,
                                     .alpha_threshold = 0U,
                                     .outline_width = 8U,
                                     .outline_color =
                                         sticker::RgbaColor{
                                             .red = 255U,
                                             .green = 255U,
                                             .blue = 255U,
                                             .alpha = 255U,
                                         },
                                 });
  require(!result.animated, "static source produced an animated output");
  require(result.width == 512U && result.height == 512U,
          "static output dimensions are not 512 by 512");
  require(result.frame_count == 1U, "static output does not have one frame");
  require(result.bytes.size() <= static_cast<std::size_t>(100U) * 1024U,
          "static output exceeds the WhatsApp size limit");

  int width = 0;
  int height = 0;
  const std::vector<Byte> decoded = decode_rgba(result.bytes, width, height);
  const std::size_t outline_offset = static_cast<std::size_t>(256U) * 512U * 4U;
  require(decoded[outline_offset] > 240U &&
              decoded[outline_offset + 1U] > 240U &&
              decoded[outline_offset + 2U] > 240U &&
              decoded[outline_offset + 3U] == 255U,
          "outline does not cover the requested distance from opaque pixels");
}

void test_jpeg_transcode() {
  const std::vector<Byte> source = encode_jpeg_source();
  const sticker::StickerResult result =
      sticker::transcode(source, sticker::TranscodeOptions{});
  require(!result.animated, "JPEG source produced an animated output");
  require(result.width == 512U && result.height == 512U,
          "JPEG output dimensions are not 512 by 512");
  require(result.bytes.size() <= static_cast<std::size_t>(100U) * 1024U,
          "JPEG output exceeds the WhatsApp size limit");
}

void test_malformed_jpeg_is_rejected() {
  const std::array<Byte, 3U> malformed = {0xffU, 0xd8U, 0xffU};
  try {
    static_cast<void>(
        sticker::transcode(malformed, sticker::TranscodeOptions{}));
  } catch (const sticker::StickerError &error) {
    require(error.code() == sticker::ErrorCode::kInvalidInput,
            "malformed JPEG returned the wrong error code");
    return;
  }
  throw std::runtime_error("malformed JPEG input did not fail");
}

void test_animated_transcode() {
  const std::vector<Byte> source = encode_animated_source();
  const sticker::StickerResult result =
      sticker::transcode(source, sticker::TranscodeOptions{});
  require(result.animated,
          "animated source did not produce an animated output");
  require(result.width == 512U && result.height == 512U,
          "animated output dimensions are not 512 by 512");
  require(result.frame_count > 1U, "animated output has too few frames");
  require(result.duration_ms == 200U,
          "animated output did not preserve its normalized duration");
  require(result.bytes.size() <= static_cast<std::size_t>(500U) * 1024U,
          "animated output exceeds the WhatsApp size limit");
}

void test_edge_pixels_are_clamped_before_interpolation() {
  const std::vector<Byte> source = encode_two_color_source();
  const sticker::StickerResult result =
      sticker::transcode(source, sticker::TranscodeOptions{
                                     .trim = false,
                                 });
  int width = 0;
  int height = 0;
  const std::vector<Byte> decoded = decode_rgba(result.bytes, width, height);
  require(width == 512 && height == 512,
          "edge test output has wrong dimensions");

  constexpr std::size_t kSampleY = 256U;
  const std::size_t left_offset = (kSampleY * 512U) * 4U;
  const std::size_t right_offset = (kSampleY * 512U + 511U) * 4U;
  require(decoded[left_offset] > 240U && decoded[left_offset + 2U] < 16U &&
              decoded[left_offset + 3U] == 255U,
          "left edge was blended with a pixel outside the source crop");
  require(decoded[right_offset] < 16U && decoded[right_offset + 2U] > 240U &&
              decoded[right_offset + 3U] == 255U,
          "right edge was blended with a pixel outside the source crop");
}

void test_apng_is_explicitly_rejected() {
  const std::array<Byte, 28U> apng_with_animation_control = {
      0x89U, 0x50U, 0x4eU, 0x47U, 0x0dU, 0x0aU, 0x1aU, 0x0aU, 0x00U, 0x00U,
      0x00U, 0x08U, 'a',   'c',   'T',   'L',   0x00U, 0x00U, 0x00U, 0x01U,
      0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
  };
  try {
    static_cast<void>(sticker::transcode(apng_with_animation_control,
                                         sticker::TranscodeOptions{}));
  } catch (const sticker::StickerError &error) {
    require(error.code() == sticker::ErrorCode::kUnsupportedInput,
            "APNG returned the wrong error code");
    return;
  }
  throw std::runtime_error("APNG input was not rejected");
}

void test_invalid_input() {
  const std::array<Byte, 3U> invalid = {0U, 1U, 2U};
  try {
    static_cast<void>(sticker::transcode(invalid, sticker::TranscodeOptions{}));
  } catch (const sticker::StickerError &error) {
    require(error.code() == sticker::ErrorCode::kUnsupportedInput,
            "invalid input returned the wrong error code");
    return;
  }
  throw std::runtime_error("invalid input did not fail");
}

} // namespace

int main(const int argc, char *const argv[]) {
  try {
    struct TestCase {
      std::string_view name;
      void (*run)();
    };
    constexpr std::array<TestCase, 7U> kTestCases = {
        TestCase{"static", test_static_transcode},
        TestCase{"jpeg", test_jpeg_transcode},
        TestCase{"malformed-jpeg", test_malformed_jpeg_is_rejected},
        TestCase{"animated", test_animated_transcode},
        TestCase{"interpolation",
                 test_edge_pixels_are_clamped_before_interpolation},
        TestCase{"apng", test_apng_is_explicitly_rejected},
        TestCase{"invalid", test_invalid_input},
    };
    if (argc > 2) {
      throw std::runtime_error("expected at most one native test case name");
    }

    const std::string_view requested = argc == 2 ? argv[1] : "";
    bool found = requested.empty();
    for (const TestCase &test_case : kTestCases) {
      if (!requested.empty() && test_case.name != requested) {
        continue;
      }
      test_case.run();
      found = true;
    }
    if (!found) {
      throw std::runtime_error("unknown native test case: " +
                               std::string(requested));
    }
    std::cout << "native sticker tests passed\n";
    return EXIT_SUCCESS;
  } catch (const std::exception &error) {
    std::cerr << "native sticker tests failed: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
