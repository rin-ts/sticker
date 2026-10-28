#include "sticker_pipeline.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <csetjmp>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <string>
#include <utility>
#include <vector>

extern "C" {
#include <jpeglib.h>
}

#include <png.h>
#include <webp/decode.h>
#include <webp/demux.h>
#include <webp/encode.h>
#include <webp/mux.h>

namespace sticker {

StickerError::StickerError(const ErrorCode code, std::string message)
    : std::runtime_error(std::move(message)), code_(code) {}

ErrorCode StickerError::code() const noexcept { return code_; }

namespace {

using Byte = std::uint8_t;

constexpr std::uint32_t kCanvasSize = 512U;
constexpr std::size_t kMaxInputBytes =
    static_cast<std::size_t>(16U) * 1024U * 1024U;
constexpr std::uint32_t kMaxSourceDimension = 4096U;
constexpr std::uint64_t kMaxSourcePixels =
    static_cast<std::uint64_t>(16U) * 1024U * 1024U;
constexpr std::uint32_t kMaxInputFrames = 600U;
constexpr std::uint64_t kMaxAnimationSourcePixels = 128ULL * 1024ULL * 1024ULL;
constexpr std::uint32_t kMaxAnimationDurationMs = 10'000U;
constexpr std::uint32_t kMinimumOutputFrameDurationMs = 50U;
constexpr std::size_t kMaximumStaticStickerBytes =
    static_cast<std::size_t>(100U) * 1024U;
constexpr std::size_t kMaximumAnimatedStickerBytes =
    static_cast<std::size_t>(500U) * 1024U;
constexpr std::uint8_t kMaximumOutlineWidth = 64U;
constexpr int kDistanceInfinity = 1 << 26;

struct Image {
  std::uint32_t width = 0U;
  std::uint32_t height = 0U;
  std::vector<Byte> rgba;
};

struct FrameDimensions {
  std::uint32_t width = 0U;
  std::uint32_t height = 0U;
};

struct Bounds {
  bool found = false;
  std::uint32_t left = 0U;
  std::uint32_t top = 0U;
  std::uint32_t right = 0U;
  std::uint32_t bottom = 0U;
};

struct GeometryPlan {
  Bounds bounds;
  std::uint32_t output_width = 0U;
  std::uint32_t output_height = 0U;
  std::uint32_t output_left = 0U;
  std::uint32_t output_top = 0U;
};

struct WebpInfo {
  std::uint32_t width = 0U;
  std::uint32_t height = 0U;
  std::vector<std::uint32_t> frame_durations_ms;

  [[nodiscard]] bool animated() const { return frame_durations_ms.size() > 1U; }
};

struct FrameSelection {
  std::uint32_t source_index = 0U;
  std::uint32_t duration_ms = 0U;
};

struct AnimationPlan {
  GeometryPlan geometry;
  std::vector<FrameSelection> selections;
  std::uint32_t duration_ms = 0U;
};

[[noreturn]] void fail(const ErrorCode code, const std::string &message) {
  throw StickerError(code, message);
}

[[nodiscard]] std::size_t checked_pixel_bytes(const std::uint32_t width,
                                              const std::uint32_t height) {
  if (width == 0U || height == 0U || width > kMaxSourceDimension ||
      height > kMaxSourceDimension) {
    fail(ErrorCode::kResourceLimit,
         "source dimensions exceed the native worker limit");
  }

  const std::uint64_t pixels =
      static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height);
  if (pixels > kMaxSourcePixels ||
      pixels > std::numeric_limits<std::size_t>::max() / 4U) {
    fail(ErrorCode::kResourceLimit,
         "source pixel count exceeds the native worker limit");
  }

  return static_cast<std::size_t>(pixels) * 4U;
}

[[nodiscard]] std::size_t pixel_offset(const std::uint32_t x,
                                       const std::uint32_t y,
                                       const std::uint32_t width) {
  return (static_cast<std::size_t>(y) * width + x) * 4U;
}

template <std::size_t N>
[[nodiscard]] bool starts_with(const std::span<const Byte> bytes,
                               const std::array<Byte, N> &prefix) {
  return bytes.size() >= N && std::memcmp(bytes.data(), prefix.data(), N) == 0;
}

[[nodiscard]] bool is_png(const std::span<const Byte> bytes) {
  constexpr std::array<Byte, 8U> kPngSignature = {0x89U, 0x50U, 0x4eU, 0x47U,
                                                  0x0dU, 0x0aU, 0x1aU, 0x0aU};
  return starts_with(bytes, kPngSignature);
}

[[nodiscard]] std::uint32_t
read_big_endian_u32(const std::span<const Byte> bytes,
                    const std::size_t offset) {
  return (static_cast<std::uint32_t>(bytes[offset]) << 24U) |
         (static_cast<std::uint32_t>(bytes[offset + 1U]) << 16U) |
         (static_cast<std::uint32_t>(bytes[offset + 2U]) << 8U) |
         static_cast<std::uint32_t>(bytes[offset + 3U]);
}

[[nodiscard]] bool is_apng(const std::span<const Byte> bytes) {
  constexpr std::array<Byte, 4U> kAnimationControl = {'a', 'c', 'T', 'L'};
  constexpr std::size_t kPngSignatureBytes = 8U;
  constexpr std::size_t kPngChunkOverheadBytes = 12U;

  if (!is_png(bytes)) {
    return false;
  }

  std::size_t offset = kPngSignatureBytes;
  while (bytes.size() - offset >= kPngChunkOverheadBytes) {
    const std::size_t bytes_remaining = bytes.size() - offset;
    const std::size_t data_size =
        static_cast<std::size_t>(read_big_endian_u32(bytes, offset));
    if (data_size > bytes_remaining - kPngChunkOverheadBytes) {
      return false;
    }
    if (std::memcmp(bytes.data() + offset + 4U, kAnimationControl.data(),
                    kAnimationControl.size()) == 0) {
      return true;
    }
    offset += kPngChunkOverheadBytes + data_size;
  }
  return false;
}

[[nodiscard]] bool is_jpeg(const std::span<const Byte> bytes) {
  return bytes.size() >= 3U && bytes[0] == 0xffU && bytes[1] == 0xd8U &&
         bytes[2] == 0xffU;
}

[[nodiscard]] bool is_webp(const std::span<const Byte> bytes) {
  constexpr std::array<Byte, 4U> kRiff = {0x52U, 0x49U, 0x46U, 0x46U};
  constexpr std::array<Byte, 4U> kWebp = {0x57U, 0x45U, 0x42U, 0x50U};
  return bytes.size() >= 12U && starts_with(bytes, kRiff) &&
         std::memcmp(bytes.data() + 8U, kWebp.data(), kWebp.size()) == 0;
}

class PngImage final {
public:
  PngImage() { image_.version = PNG_IMAGE_VERSION; }

  ~PngImage() { png_image_free(&image_); }

  PngImage(const PngImage &) = delete;
  PngImage &operator=(const PngImage &) = delete;
  PngImage(PngImage &&) = delete;
  PngImage &operator=(PngImage &&) = delete;

  [[nodiscard]] png_image *get() { return &image_; }

private:
  png_image image_{};
};

[[nodiscard]] Image decode_png(const std::span<const Byte> bytes) {
  PngImage image;
  if (png_image_begin_read_from_memory(image.get(), bytes.data(),
                                       bytes.size()) == 0) {
    fail(ErrorCode::kInvalidInput, "unable to decode PNG input");
  }

  const std::size_t byte_count =
      checked_pixel_bytes(image.get()->width, image.get()->height);
  Image decoded{
      .width = image.get()->width,
      .height = image.get()->height,
      .rgba = std::vector<Byte>(byte_count),
  };
  image.get()->format = PNG_FORMAT_RGBA;
  const int succeeded = png_image_finish_read(image.get(), nullptr,
                                              decoded.rgba.data(), 0, nullptr);
  if (succeeded == 0) {
    fail(ErrorCode::kInvalidInput, "unable to finish PNG decode");
  }
  return decoded;
}

struct JpegErrorManager {
  jpeg_error_mgr base{};
  std::jmp_buf jump_buffer{};
  char message[JMSG_LENGTH_MAX]{};
};

struct FreeByteBuffer {
  void operator()(Byte *const buffer) const noexcept { std::free(buffer); }
};

struct JpegDecodeState {
  jpeg_decompress_struct decoder{};
  JpegErrorManager error{};
  bool decoder_created = false;
  std::unique_ptr<Image> decoded;
  std::unique_ptr<Byte, FreeByteBuffer> scanline;
};

extern "C" void sticker_jpeg_error_exit(j_common_ptr cinfo) {
  auto *const error = reinterpret_cast<JpegErrorManager *>(cinfo->err);
  (*cinfo->err->format_message)(cinfo, error->message);
  std::longjmp(error->jump_buffer, 1);
}

void destroy_jpeg_decode_state(JpegDecodeState *const state) noexcept {
  if (state == nullptr) {
    return;
  }
  if (state->decoder_created) {
    jpeg_destroy_decompress(&state->decoder);
  }
  delete state;
}

[[nodiscard]] Image decode_jpeg(const std::span<const Byte> bytes) {
  // libjpeg reports malformed input through longjmp. State modified during
  // decoding lives on the heap, while the unmodified owner exists before
  // setjmp; no mutated C++ automatic object is observed after a longjmp.
  using JpegStatePtr =
      std::unique_ptr<JpegDecodeState, decltype(&destroy_jpeg_decode_state)>;
  JpegStatePtr state(new (std::nothrow) JpegDecodeState{},
                     destroy_jpeg_decode_state);
  if (!state) {
    fail(ErrorCode::kResourceLimit, "unable to allocate JPEG decoder state");
  }

  state->decoder.err = jpeg_std_error(&state->error.base);
  state->error.base.error_exit = sticker_jpeg_error_exit;
  if (setjmp(state->error.jump_buffer) != 0) {
    fail(ErrorCode::kInvalidInput,
         std::string("unable to decode JPEG input: ") + state->error.message);
  }

  jpeg_create_decompress(&state->decoder);
  state->decoder_created = true;
  jpeg_mem_src(&state->decoder,
               reinterpret_cast<const unsigned char *>(bytes.data()),
               static_cast<unsigned long>(bytes.size()));
  if (jpeg_read_header(&state->decoder, TRUE) != JPEG_HEADER_OK) {
    fail(ErrorCode::kInvalidInput, "unable to read JPEG header");
  }

  if (state->decoder.image_width == 0U || state->decoder.image_height == 0U ||
      state->decoder.image_width > kMaxSourceDimension ||
      state->decoder.image_height > kMaxSourceDimension) {
    fail(ErrorCode::kResourceLimit,
         "JPEG dimensions exceed the native worker limit");
  }

  state->decoder.out_color_space = JCS_RGB;
  if (jpeg_start_decompress(&state->decoder) == FALSE ||
      state->decoder.output_components != 3) {
    fail(ErrorCode::kInvalidInput, "unable to start JPEG RGB decode");
  }

  const std::uint32_t width = state->decoder.output_width;
  const std::uint32_t height = state->decoder.output_height;
  const std::size_t output_size = checked_pixel_bytes(width, height);
  state->decoded = std::make_unique<Image>(Image{
      .width = width,
      .height = height,
      .rgba = std::vector<Byte>(output_size),
  });
  state->scanline.reset(
      static_cast<Byte *>(std::malloc(static_cast<std::size_t>(width) * 3U)));
  if (!state->scanline) {
    fail(ErrorCode::kResourceLimit, "unable to allocate JPEG scanline");
  }

  for (std::uint32_t y = 0U; y < height; ++y) {
    JSAMPROW row = state->scanline.get();
    if (jpeg_read_scanlines(&state->decoder, &row, 1U) != 1U) {
      fail(ErrorCode::kInvalidInput, "unable to read a JPEG scanline");
    }

    for (std::uint32_t x = 0U; x < width; ++x) {
      const std::size_t source_offset = static_cast<std::size_t>(x) * 3U;
      const std::size_t destination_offset = pixel_offset(x, y, width);
      state->decoded->rgba[destination_offset] =
          state->scanline.get()[source_offset];
      state->decoded->rgba[destination_offset + 1U] =
          state->scanline.get()[source_offset + 1U];
      state->decoded->rgba[destination_offset + 2U] =
          state->scanline.get()[source_offset + 2U];
      state->decoded->rgba[destination_offset + 3U] = 255U;
    }
  }

  if (jpeg_finish_decompress(&state->decoder) == FALSE) {
    fail(ErrorCode::kInvalidInput, "unable to finish JPEG decode");
  }
  jpeg_destroy_decompress(&state->decoder);
  state->decoder_created = false;
  Image decoded = std::move(*state->decoded);
  state->decoded.reset();
  return decoded;
}

[[nodiscard]] WebpInfo inspect_webp(const std::span<const Byte> bytes) {
  WebPData data{};
  data.bytes = bytes.data();
  data.size = bytes.size();
  using DemuxerPtr = std::unique_ptr<WebPDemuxer, decltype(&WebPDemuxDelete)>;
  DemuxerPtr demuxer(WebPDemux(&data), WebPDemuxDelete);
  if (!demuxer) {
    fail(ErrorCode::kInvalidInput, "unable to parse WebP input");
  }

  const std::uint32_t width =
      WebPDemuxGetI(demuxer.get(), WEBP_FF_CANVAS_WIDTH);
  const std::uint32_t height =
      WebPDemuxGetI(demuxer.get(), WEBP_FF_CANVAS_HEIGHT);
  static_cast<void>(checked_pixel_bytes(width, height));

  const std::uint32_t frame_count =
      WebPDemuxGetI(demuxer.get(), WEBP_FF_FRAME_COUNT);
  if (frame_count == 0U || frame_count > kMaxInputFrames) {
    fail(ErrorCode::kResourceLimit,
         "WebP frame count exceeds the native worker limit");
  }
  const std::uint64_t source_pixels =
      static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height);
  if (static_cast<std::uint64_t>(frame_count) >
      kMaxAnimationSourcePixels / source_pixels) {
    fail(ErrorCode::kResourceLimit,
         "WebP animation exceeds the native worker aggregate pixel limit");
  }

  WebpInfo info{
      .width = width,
      .height = height,
      .frame_durations_ms = {},
  };
  info.frame_durations_ms.reserve(frame_count);
  for (std::uint32_t index = 1U; index <= frame_count; ++index) {
    WebPIterator iterator{};
    if (WebPDemuxGetFrame(demuxer.get(), static_cast<int>(index), &iterator) ==
        0) {
      fail(ErrorCode::kInvalidInput, "unable to inspect a WebP frame");
    }
    info.frame_durations_ms.push_back(
        static_cast<std::uint32_t>(std::max(iterator.duration, 0)));
    WebPDemuxReleaseIterator(&iterator);
  }
  return info;
}

[[nodiscard]] Image decode_static_webp(const std::span<const Byte> bytes,
                                       const WebpInfo &info) {
  WebPBitstreamFeatures features{};
  if (WebPGetFeatures(bytes.data(), bytes.size(), &features) != VP8_STATUS_OK) {
    fail(ErrorCode::kInvalidInput, "unable to read WebP features");
  }
  if (features.width != static_cast<int>(info.width) ||
      features.height != static_cast<int>(info.height)) {
    fail(ErrorCode::kInvalidInput, "WebP canvas dimensions are inconsistent");
  }

  Image decoded{
      .width = info.width,
      .height = info.height,
      .rgba = std::vector<Byte>(checked_pixel_bytes(info.width, info.height)),
  };
  if (WebPDecodeRGBAInto(bytes.data(), bytes.size(), decoded.rgba.data(),
                         decoded.rgba.size(),
                         static_cast<int>(info.width * 4U)) == nullptr) {
    fail(ErrorCode::kInvalidInput, "unable to decode WebP pixels");
  }
  return decoded;
}

template <typename Callback>
void visit_animation_frames(const std::span<const Byte> bytes,
                            const WebpInfo &info, Callback &&callback) {
  WebPData data{};
  data.bytes = bytes.data();
  data.size = bytes.size();

  WebPAnimDecoderOptions options{};
  if (WebPAnimDecoderOptionsInit(&options) == 0) {
    fail(ErrorCode::kInternal,
         "unable to initialize the WebP animation decoder");
  }
  options.color_mode = MODE_RGBA;
  options.use_threads = 1;

  using DecoderPtr =
      std::unique_ptr<WebPAnimDecoder, decltype(&WebPAnimDecoderDelete)>;
  DecoderPtr decoder(WebPAnimDecoderNew(&data, &options),
                     WebPAnimDecoderDelete);
  if (!decoder) {
    fail(ErrorCode::kInvalidInput,
         "unable to create the WebP animation decoder");
  }

  WebPAnimInfo animation_info{};
  if (WebPAnimDecoderGetInfo(decoder.get(), &animation_info) == 0 ||
      animation_info.canvas_width != info.width ||
      animation_info.canvas_height != info.height ||
      static_cast<std::size_t>(animation_info.frame_count) !=
          info.frame_durations_ms.size()) {
    fail(ErrorCode::kInvalidInput, "WebP animation metadata is inconsistent");
  }

  const std::size_t frame_bytes = checked_pixel_bytes(info.width, info.height);
  std::uint32_t index = 0U;
  while (WebPAnimDecoderHasMoreFrames(decoder.get()) != 0) {
    Byte *rgba = nullptr;
    int timestamp = 0;
    if (WebPAnimDecoderGetNext(decoder.get(), &rgba, &timestamp) == 0 ||
        rgba == nullptr) {
      fail(ErrorCode::kInvalidInput, "unable to decode a WebP animation frame");
    }
    static_cast<void>(timestamp);
    callback(index, std::span<const Byte>(rgba, frame_bytes));
    ++index;
  }

  if (index != info.frame_durations_ms.size()) {
    fail(ErrorCode::kInvalidInput,
         "WebP animation frame count changed during decode");
  }
}

void extend_bounds(Bounds &bounds, const std::span<const Byte> rgba,
                   const FrameDimensions dimensions,
                   const std::uint8_t alpha_threshold) {
  for (std::uint32_t y = 0U; y < dimensions.height; ++y) {
    for (std::uint32_t x = 0U; x < dimensions.width; ++x) {
      if (rgba[pixel_offset(x, y, dimensions.width) + 3U] <= alpha_threshold) {
        continue;
      }
      if (!bounds.found) {
        bounds = Bounds{
            .found = true,
            .left = x,
            .top = y,
            .right = x,
            .bottom = y,
        };
        continue;
      }
      bounds.left = std::min(bounds.left, x);
      bounds.top = std::min(bounds.top, y);
      bounds.right = std::max(bounds.right, x);
      bounds.bottom = std::max(bounds.bottom, y);
    }
  }
}

[[nodiscard]] GeometryPlan make_geometry_plan(Bounds bounds,
                                              const std::uint32_t source_width,
                                              const std::uint32_t source_height,
                                              const TranscodeOptions &options) {
  if (!options.trim) {
    bounds = Bounds{
        .found = true,
        .left = 0U,
        .top = 0U,
        .right = source_width - 1U,
        .bottom = source_height - 1U,
    };
  }
  if (!bounds.found) {
    fail(ErrorCode::kInvalidInput,
         "the sticker source has no visible pixels after alpha trimming");
  }
  if (options.outline_width > kMaximumOutlineWidth ||
      options.outline_width * 2U >= kCanvasSize) {
    fail(ErrorCode::kInvalidInput,
         "outline width is outside the supported range");
  }

  const std::uint32_t bounds_width = bounds.right - bounds.left + 1U;
  const std::uint32_t bounds_height = bounds.bottom - bounds.top + 1U;
  const std::uint32_t usable_size =
      kCanvasSize - static_cast<std::uint32_t>(options.outline_width) * 2U;
  const double scale = std::min(
      static_cast<double>(usable_size) / static_cast<double>(bounds_width),
      static_cast<double>(usable_size) / static_cast<double>(bounds_height));
  const auto scaled_width = static_cast<std::uint32_t>(std::clamp(
      std::lround(bounds_width * scale), 1L, static_cast<long>(usable_size)));
  const auto scaled_height = static_cast<std::uint32_t>(std::clamp(
      std::lround(bounds_height * scale), 1L, static_cast<long>(usable_size)));
  return GeometryPlan{
      .bounds = bounds,
      .output_width = scaled_width,
      .output_height = scaled_height,
      .output_left = (kCanvasSize - scaled_width) / 2U,
      .output_top = (kCanvasSize - scaled_height) / 2U,
  };
}

[[nodiscard]] Byte rounded_byte(const double value) {
  return static_cast<Byte>(
      std::clamp(std::lround(value), 0L, static_cast<long>(255U)));
}

[[nodiscard]] Image render_frame(const std::span<const Byte> source,
                                 const std::uint32_t source_width,
                                 const std::uint32_t source_height,
                                 const GeometryPlan &plan) {
  const std::size_t expected_source_bytes =
      checked_pixel_bytes(source_width, source_height);
  if (source.size() != expected_source_bytes) {
    fail(ErrorCode::kInternal,
         "renderer source buffer does not match its dimensions");
  }
  if (!plan.bounds.found || plan.bounds.left > plan.bounds.right ||
      plan.bounds.top > plan.bounds.bottom ||
      plan.bounds.right >= source_width ||
      plan.bounds.bottom >= source_height || plan.output_width == 0U ||
      plan.output_height == 0U || plan.output_width > kCanvasSize ||
      plan.output_height > kCanvasSize ||
      plan.output_left > kCanvasSize - plan.output_width ||
      plan.output_top > kCanvasSize - plan.output_height) {
    fail(ErrorCode::kInternal, "renderer received an invalid geometry plan");
  }

  Image output{
      .width = kCanvasSize,
      .height = kCanvasSize,
      .rgba =
          std::vector<Byte>(checked_pixel_bytes(kCanvasSize, kCanvasSize), 0U),
  };

  const std::uint32_t crop_width = plan.bounds.right - plan.bounds.left + 1U;
  const std::uint32_t crop_height = plan.bounds.bottom - plan.bounds.top + 1U;
  for (std::uint32_t destination_y = 0U; destination_y < plan.output_height;
       ++destination_y) {
    const double source_y = static_cast<double>(plan.bounds.top) +
                            ((static_cast<double>(destination_y) + 0.5) *
                                 crop_height / plan.output_height -
                             0.5);
    const double clamped_source_y =
        std::clamp(source_y, static_cast<double>(plan.bounds.top),
                   static_cast<double>(plan.bounds.bottom));
    const double source_y_floor = std::floor(clamped_source_y);
    const auto source_y0 = static_cast<std::uint32_t>(source_y_floor);
    const auto source_y1 = std::min(source_y0 + 1U, plan.bounds.bottom);
    const double y_weight = clamped_source_y - source_y_floor;

    for (std::uint32_t destination_x = 0U; destination_x < plan.output_width;
         ++destination_x) {
      const double source_x = static_cast<double>(plan.bounds.left) +
                              ((static_cast<double>(destination_x) + 0.5) *
                                   crop_width / plan.output_width -
                               0.5);
      const double clamped_source_x =
          std::clamp(source_x, static_cast<double>(plan.bounds.left),
                     static_cast<double>(plan.bounds.right));
      const double source_x_floor = std::floor(clamped_source_x);
      const auto source_x0 = static_cast<std::uint32_t>(source_x_floor);
      const auto source_x1 = std::min(source_x0 + 1U, plan.bounds.right);
      const double x_weight = clamped_source_x - source_x_floor;

      const std::array<std::size_t, 4U> offsets = {
          pixel_offset(source_x0, source_y0, source_width),
          pixel_offset(source_x1, source_y0, source_width),
          pixel_offset(source_x0, source_y1, source_width),
          pixel_offset(source_x1, source_y1, source_width),
      };
      const std::array<double, 4U> weights = {
          (1.0 - x_weight) * (1.0 - y_weight),
          x_weight * (1.0 - y_weight),
          (1.0 - x_weight) * y_weight,
          x_weight * y_weight,
      };

      double alpha = 0.0;
      double premultiplied_red = 0.0;
      double premultiplied_green = 0.0;
      double premultiplied_blue = 0.0;
      for (std::size_t sample = 0U; sample < offsets.size(); ++sample) {
        const std::size_t offset = offsets[sample];
        const double sample_alpha =
            static_cast<double>(source[offset + 3U]) * weights[sample];
        alpha += sample_alpha;
        premultiplied_red += static_cast<double>(source[offset]) * sample_alpha;
        premultiplied_green +=
            static_cast<double>(source[offset + 1U]) * sample_alpha;
        premultiplied_blue +=
            static_cast<double>(source[offset + 2U]) * sample_alpha;
      }

      const std::uint32_t output_x = plan.output_left + destination_x;
      const std::uint32_t output_y = plan.output_top + destination_y;
      const std::size_t output_offset =
          pixel_offset(output_x, output_y, kCanvasSize);
      if (alpha <= 0.0) {
        continue;
      }
      output.rgba[output_offset] = rounded_byte(premultiplied_red / alpha);
      output.rgba[output_offset + 1U] =
          rounded_byte(premultiplied_green / alpha);
      output.rgba[output_offset + 2U] =
          rounded_byte(premultiplied_blue / alpha);
      output.rgba[output_offset + 3U] = rounded_byte(alpha);
    }
  }

  return output;
}

void distance_transform_1d(const std::vector<int> &source,
                           std::vector<int> &destination,
                           std::vector<int> &vertices,
                           std::vector<double> &intersections) {
  const std::size_t length = source.size();
  std::size_t parabola = 0U;
  vertices[0] = 0;
  intersections[0] = -std::numeric_limits<double>::infinity();
  intersections[1] = std::numeric_limits<double>::infinity();

  for (std::size_t query = 1U; query < length; ++query) {
    double intersection = 0.0;
    while (true) {
      const int previous = vertices[parabola];
      const double query_position = static_cast<double>(query);
      const double previous_position = static_cast<double>(previous);
      intersection = ((static_cast<double>(source[query]) +
                       query_position * query_position) -
                      (static_cast<double>(source[previous]) +
                       previous_position * previous_position)) /
                     (2.0 * (query_position - previous_position));
      if (intersection > intersections[parabola] || parabola == 0U) {
        break;
      }
      --parabola;
    }

    ++parabola;
    vertices[parabola] = static_cast<int>(query);
    intersections[parabola] = intersection;
    intersections[parabola + 1U] = std::numeric_limits<double>::infinity();
  }

  std::size_t active = 0U;
  for (std::size_t query = 0U; query < length; ++query) {
    while (intersections[active + 1U] < static_cast<double>(query)) {
      ++active;
    }
    const int delta = static_cast<int>(query) - vertices[active];
    destination[query] = delta * delta + source[vertices[active]];
  }
}

void apply_outline(Image &image, const TranscodeOptions &options) {
  if (options.outline_width == 0U || options.outline_color.alpha == 0U) {
    return;
  }

  const std::size_t pixel_count =
      static_cast<std::size_t>(image.width) * image.height;
  std::vector<int> distances(pixel_count, kDistanceInfinity);
  std::vector<int> source(std::max(image.width, image.height));
  std::vector<int> transformed(source.size());
  std::vector<int> vertices(source.size());
  std::vector<double> intersections(source.size() + 1U);

  for (std::uint32_t y = 0U; y < image.height; ++y) {
    for (std::uint32_t x = 0U; x < image.width; ++x) {
      source[x] = image.rgba[pixel_offset(x, y, image.width) + 3U] >
                          options.alpha_threshold
                      ? 0
                      : kDistanceInfinity;
    }
    distance_transform_1d(source, transformed, vertices, intersections);
    for (std::uint32_t x = 0U; x < image.width; ++x) {
      distances[static_cast<std::size_t>(y) * image.width + x] = transformed[x];
    }
  }

  for (std::uint32_t x = 0U; x < image.width; ++x) {
    for (std::uint32_t y = 0U; y < image.height; ++y) {
      source[y] = distances[static_cast<std::size_t>(y) * image.width + x];
    }
    distance_transform_1d(source, transformed, vertices, intersections);
    for (std::uint32_t y = 0U; y < image.height; ++y) {
      distances[static_cast<std::size_t>(y) * image.width + x] = transformed[y];
    }
  }

  const int radius_squared = static_cast<int>(options.outline_width) *
                             static_cast<int>(options.outline_width);
  for (std::uint32_t y = 0U; y < image.height; ++y) {
    for (std::uint32_t x = 0U; x < image.width; ++x) {
      const std::size_t image_offset = pixel_offset(x, y, image.width);
      const std::size_t distance_offset =
          static_cast<std::size_t>(y) * image.width + x;
      if (image.rgba[image_offset + 3U] != 0U ||
          distances[distance_offset] > radius_squared) {
        continue;
      }
      image.rgba[image_offset] = options.outline_color.red;
      image.rgba[image_offset + 1U] = options.outline_color.green;
      image.rgba[image_offset + 2U] = options.outline_color.blue;
      image.rgba[image_offset + 3U] = options.outline_color.alpha;
    }
  }
}

class Picture final {
public:
  Picture() {
    if (WebPPictureInit(&picture_) == 0) {
      fail(ErrorCode::kInternal, "unable to initialize the WebP picture");
    }
  }

  ~Picture() { WebPPictureFree(&picture_); }

  Picture(const Picture &) = delete;
  Picture &operator=(const Picture &) = delete;
  Picture(Picture &&) = delete;
  Picture &operator=(Picture &&) = delete;

  [[nodiscard]] WebPPicture *get() { return &picture_; }

  void import_rgba(const Image &image) {
    picture_.width = static_cast<int>(image.width);
    picture_.height = static_cast<int>(image.height);
    if (WebPPictureImportRGBA(&picture_, image.rgba.data(),
                              static_cast<int>(image.width * 4U)) == 0) {
      fail(ErrorCode::kEncodingFailed, "unable to import pixels into WebP");
    }
  }

private:
  WebPPicture picture_{};
};

class MemoryWriter final {
public:
  MemoryWriter() { WebPMemoryWriterInit(&writer_); }

  ~MemoryWriter() { WebPFree(writer_.mem); }

  MemoryWriter(const MemoryWriter &) = delete;
  MemoryWriter &operator=(const MemoryWriter &) = delete;
  MemoryWriter(MemoryWriter &&) = delete;
  MemoryWriter &operator=(MemoryWriter &&) = delete;

  [[nodiscard]] WebPMemoryWriter *get() { return &writer_; }

  [[nodiscard]] std::vector<Byte> copy() const {
    if (writer_.mem == nullptr || writer_.size == 0U) {
      return {};
    }
    return {writer_.mem, writer_.mem + writer_.size};
  }

private:
  WebPMemoryWriter writer_{};
};

class EncodedData final {
public:
  EncodedData() { WebPDataInit(&data_); }

  ~EncodedData() { WebPDataClear(&data_); }

  EncodedData(const EncodedData &) = delete;
  EncodedData &operator=(const EncodedData &) = delete;
  EncodedData(EncodedData &&) = delete;
  EncodedData &operator=(EncodedData &&) = delete;

  [[nodiscard]] WebPData *get() { return &data_; }

  [[nodiscard]] std::vector<Byte> copy() const {
    if (data_.bytes == nullptr || data_.size == 0U) {
      return {};
    }
    return {data_.bytes, data_.bytes + data_.size};
  }

private:
  WebPData data_{};
};

[[nodiscard]] WebPConfig make_webp_config(const int quality,
                                          const bool lossless) {
  WebPConfig config{};
  if (WebPConfigPreset(&config, WEBP_PRESET_DRAWING,
                       static_cast<float>(quality)) == 0) {
    fail(ErrorCode::kInternal, "unable to initialize the WebP encoder config");
  }
  if (lossless) {
    if (WebPConfigLosslessPreset(&config, 6) == 0) {
      fail(ErrorCode::kInternal,
           "unable to initialize the lossless WebP encoder config");
    }
  } else {
    config.lossless = 0;
    config.method = 6;
    config.alpha_quality = 100;
    config.thread_level = 1;
    config.exact = 0;
    config.use_sharp_yuv = 1;
  }
  if (WebPValidateConfig(&config) == 0) {
    fail(ErrorCode::kInternal, "invalid WebP encoder configuration");
  }
  return config;
}

[[nodiscard]] std::vector<Byte>
encode_static(const Image &image, const int quality, const bool lossless) {
  WebPConfig config = make_webp_config(quality, lossless);
  Picture picture;
  picture.import_rgba(image);
  MemoryWriter writer;
  picture.get()->writer = WebPMemoryWrite;
  picture.get()->custom_ptr = writer.get();
  if (WebPEncode(&config, picture.get()) == 0) {
    fail(ErrorCode::kEncodingFailed, "unable to encode static WebP output");
  }
  return writer.copy();
}

[[nodiscard]] StickerResult validate_output(std::vector<Byte> bytes,
                                            const bool expect_animation) {
  if (bytes.empty()) {
    fail(ErrorCode::kEncodingFailed, "WebP encoder returned no output");
  }
  const WebpInfo info = inspect_webp(bytes);
  if (info.width != kCanvasSize || info.height != kCanvasSize) {
    fail(ErrorCode::kValidationFailed,
         "encoded WebP does not have a 512 by 512 canvas");
  }

  const bool animated = info.animated();
  if (animated != expect_animation) {
    fail(ErrorCode::kValidationFailed,
         "encoded WebP animation state differs from the requested output");
  }

  std::uint64_t duration = 0U;
  for (const std::uint32_t frame_duration : info.frame_durations_ms) {
    if (animated && frame_duration < 8U) {
      fail(ErrorCode::kValidationFailed,
           "encoded WebP contains a frame shorter than 8 milliseconds");
    }
    duration += frame_duration;
  }
  if (animated && duration > kMaxAnimationDurationMs) {
    fail(ErrorCode::kValidationFailed,
         "encoded WebP animation exceeds 10 seconds");
  }

  const std::size_t limit =
      animated ? kMaximumAnimatedStickerBytes : kMaximumStaticStickerBytes;
  if (bytes.size() > limit) {
    fail(ErrorCode::kValidationFailed,
         "encoded WebP exceeds the WhatsApp size limit");
  }

  return StickerResult{
      .bytes = std::move(bytes),
      .width = kCanvasSize,
      .height = kCanvasSize,
      .frame_count = static_cast<std::uint32_t>(info.frame_durations_ms.size()),
      .duration_ms = static_cast<std::uint32_t>(duration),
      .animated = animated,
  };
}

[[nodiscard]] StickerResult encode_static_limited(const Image &image) {
  std::vector<Byte> encoded = encode_static(image, 100, true);
  if (encoded.size() <= kMaximumStaticStickerBytes) {
    return validate_output(std::move(encoded), false);
  }

  constexpr std::array<int, 9U> kQualityCandidates = {90, 80, 70, 60, 50,
                                                      40, 30, 20, 10};
  for (const int quality : kQualityCandidates) {
    encoded = encode_static(image, quality, false);
    if (encoded.size() <= kMaximumStaticStickerBytes) {
      return validate_output(std::move(encoded), false);
    }
  }
  fail(ErrorCode::kEncodingFailed,
       "unable to fit the static sticker inside the WhatsApp size limit");
}

[[nodiscard]] AnimationPlan plan_animation(const std::span<const Byte> source,
                                           const WebpInfo &info,
                                           const TranscodeOptions &options) {
  Bounds bounds{};
  std::vector<FrameSelection> selections;
  selections.reserve(info.frame_durations_ms.size());
  std::uint64_t duration_ms = 0U;

  visit_animation_frames(
      source, info,
      [&](const std::uint32_t index, const std::span<const Byte> frame) {
        extend_bounds(bounds, frame,
                      FrameDimensions{
                          .width = info.width,
                          .height = info.height,
                      },
                      options.alpha_threshold);
        std::uint32_t frame_duration = info.frame_durations_ms[index];
        if (frame_duration == 0U) {
          frame_duration = kMinimumOutputFrameDurationMs;
        }
        duration_ms += frame_duration;
        if (duration_ms > kMaxAnimationDurationMs) {
          fail(ErrorCode::kResourceLimit,
               "source animation exceeds the WhatsApp duration limit");
        }

        if (selections.empty() ||
            selections.back().duration_ms >= kMinimumOutputFrameDurationMs) {
          selections.push_back(FrameSelection{
              .source_index = index,
              .duration_ms = frame_duration,
          });
        } else {
          selections.back().duration_ms += frame_duration;
        }
      });

  if (selections.size() > 1U &&
      selections.back().duration_ms < kMinimumOutputFrameDurationMs) {
    selections[selections.size() - 2U].duration_ms +=
        selections.back().duration_ms;
    selections.pop_back();
  }
  if (selections.empty()) {
    fail(ErrorCode::kInvalidInput, "WebP animation has no frames");
  }

  return AnimationPlan{
      .geometry = make_geometry_plan(bounds, info.width, info.height, options),
      .selections = std::move(selections),
      .duration_ms = static_cast<std::uint32_t>(duration_ms),
  };
}

[[nodiscard]] Image render_animation_frame(const std::span<const Byte> source,
                                           const WebpInfo &info,
                                           const AnimationPlan &plan,
                                           const std::uint32_t wanted_index,
                                           const TranscodeOptions &options) {
  Image rendered{};
  bool found = false;
  visit_animation_frames(
      source, info,
      [&](const std::uint32_t index, const std::span<const Byte> frame) {
        if (index != wanted_index) {
          return;
        }
        rendered = render_frame(frame, info.width, info.height, plan.geometry);
        apply_outline(rendered, options);
        found = true;
      });
  if (!found) {
    fail(ErrorCode::kInvalidInput, "requested animation frame is unavailable");
  }
  return rendered;
}

[[nodiscard]] std::vector<Byte>
encode_animation(const std::span<const Byte> source, const WebpInfo &info,
                 const AnimationPlan &plan, const TranscodeOptions &options,
                 const int quality) {
  WebPAnimEncoderOptions encoder_options{};
  if (WebPAnimEncoderOptionsInit(&encoder_options) == 0) {
    fail(ErrorCode::kInternal, "unable to initialize the animation encoder");
  }
  encoder_options.anim_params.bgcolor = 0U;
  encoder_options.anim_params.loop_count = 0;
  encoder_options.minimize_size = 0;
  encoder_options.allow_mixed = 1;

  using EncoderPtr =
      std::unique_ptr<WebPAnimEncoder, decltype(&WebPAnimEncoderDelete)>;
  EncoderPtr encoder(WebPAnimEncoderNew(static_cast<int>(kCanvasSize),
                                        static_cast<int>(kCanvasSize),
                                        &encoder_options),
                     WebPAnimEncoderDelete);
  if (!encoder) {
    fail(ErrorCode::kEncodingFailed, "unable to create the animation encoder");
  }

  WebPConfig config = make_webp_config(quality, false);
  std::size_t selection_index = 0U;
  std::uint32_t timestamp_ms = 0U;
  visit_animation_frames(
      source, info,
      [&](const std::uint32_t index, const std::span<const Byte> frame) {
        if (selection_index >= plan.selections.size() ||
            plan.selections[selection_index].source_index != index) {
          return;
        }

        Image rendered =
            render_frame(frame, info.width, info.height, plan.geometry);
        apply_outline(rendered, options);
        Picture picture;
        picture.import_rgba(rendered);
        if (WebPAnimEncoderAdd(encoder.get(), picture.get(),
                               static_cast<int>(timestamp_ms), &config) == 0) {
          fail(ErrorCode::kEncodingFailed,
               "unable to encode an animation frame");
        }
        timestamp_ms += plan.selections[selection_index].duration_ms;
        ++selection_index;
      });

  if (selection_index != plan.selections.size()) {
    fail(ErrorCode::kInvalidInput,
         "animation frame selection could not be reconstructed");
  }
  if (WebPAnimEncoderAdd(encoder.get(), nullptr, static_cast<int>(timestamp_ms),
                         nullptr) == 0) {
    fail(ErrorCode::kEncodingFailed, "unable to finalize the animation timing");
  }

  EncodedData encoded;
  if (WebPAnimEncoderAssemble(encoder.get(), encoded.get()) == 0) {
    fail(ErrorCode::kEncodingFailed, "unable to assemble animated WebP output");
  }
  return encoded.copy();
}

[[nodiscard]] StickerResult
encode_animation_limited(const std::span<const Byte> source,
                         const WebpInfo &info, const AnimationPlan &plan,
                         const TranscodeOptions &options) {
  constexpr std::array<int, 9U> kQualityCandidates = {90, 80, 70, 60, 50,
                                                      40, 30, 20, 10};
  for (const int quality : kQualityCandidates) {
    std::vector<Byte> encoded =
        encode_animation(source, info, plan, options, quality);
    if (encoded.size() <= kMaximumAnimatedStickerBytes) {
      return validate_output(std::move(encoded), true);
    }
  }
  fail(ErrorCode::kEncodingFailed,
       "unable to fit the animated sticker inside the WhatsApp size limit");
}

[[nodiscard]] StickerResult transcode_static(Image decoded,
                                             const TranscodeOptions &options) {
  Bounds bounds{};
  extend_bounds(bounds, decoded.rgba,
                FrameDimensions{
                    .width = decoded.width,
                    .height = decoded.height,
                },
                options.alpha_threshold);
  const GeometryPlan geometry =
      make_geometry_plan(bounds, decoded.width, decoded.height, options);
  Image rendered =
      render_frame(decoded.rgba, decoded.width, decoded.height, geometry);
  apply_outline(rendered, options);
  return encode_static_limited(rendered);
}

[[nodiscard]] StickerResult
transcode_animated_webp(const std::span<const Byte> source,
                        const WebpInfo &info, const TranscodeOptions &options) {
  const AnimationPlan plan = plan_animation(source, info, options);
  if (plan.selections.size() == 1U) {
    return encode_static_limited(render_animation_frame(
        source, info, plan, plan.selections.front().source_index, options));
  }
  return encode_animation_limited(source, info, plan, options);
}

} // namespace

StickerResult transcode(const std::span<const std::uint8_t> source,
                        const TranscodeOptions &options) {
  try {
    if (source.empty()) {
      fail(ErrorCode::kInvalidInput, "sticker input is empty");
    }
    if (source.size() > kMaxInputBytes) {
      fail(ErrorCode::kResourceLimit,
           "sticker input exceeds the native worker byte limit");
    }
    if (options.outline_width > kMaximumOutlineWidth) {
      fail(ErrorCode::kInvalidInput,
           "outline width is outside the supported range");
    }

    if (is_webp(source)) {
      const WebpInfo info = inspect_webp(source);
      if (info.animated()) {
        return transcode_animated_webp(source, info, options);
      }
      return transcode_static(decode_static_webp(source, info), options);
    }
    if (is_png(source)) {
      if (is_apng(source)) {
        fail(ErrorCode::kUnsupportedInput,
             "APNG input is not supported; convert it to WebP first");
      }
      return transcode_static(decode_png(source), options);
    }
    if (is_jpeg(source)) {
      return transcode_static(decode_jpeg(source), options);
    }
    fail(ErrorCode::kUnsupportedInput,
         "unsupported sticker input; use PNG, JPEG, or WebP");
  } catch (const StickerError &) {
    throw;
  } catch (const std::bad_alloc &) {
    fail(ErrorCode::kResourceLimit, "native worker ran out of memory");
  } catch (const std::exception &error) {
    fail(ErrorCode::kInternal,
         std::string("native sticker processing failed: ") + error.what());
  }
}

} // namespace sticker
