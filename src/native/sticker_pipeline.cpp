#include "sticker_pipeline.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
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

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/mem.h>
#include <libavutil/rational.h>
#include <libswscale/swscale.h>
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
constexpr std::size_t kMaximumExifMetadataBytes =
    static_cast<std::size_t>(4U) * 1024U;
// Allows the RIFF EXIF chunk, alignment, and a VP8X chunk when muxing adds it.
constexpr std::size_t kMaximumExifMuxOverheadBytes = 128U;
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

struct VideoInfo {
  std::uint32_t width = 0U;
  std::uint32_t height = 0U;
  std::vector<std::uint32_t> frame_durations_ms;
};

struct VideoTiming {
  std::int64_t stream_duration_ms = AV_NOPTS_VALUE;
  std::uint32_t fallback_frame_duration_ms = kMinimumOutputFrameDurationMs;
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

[[nodiscard]] std::string ffmpeg_error_message(const int error) {
  std::array<char, AV_ERROR_MAX_STRING_SIZE> text{};
  if (av_strerror(error, text.data(), text.size()) < 0) {
    return "an unknown FFmpeg error";
  }
  return std::string(text.data());
}

struct VideoInput {
  std::span<const Byte> bytes;
  std::size_t offset = 0U;
};

[[nodiscard]] int read_video_input(void *const opaque,
                                   std::uint8_t *const buffer,
                                   const int buffer_size) noexcept {
  auto *const input = static_cast<VideoInput *>(opaque);
  if (input == nullptr || buffer == nullptr || buffer_size <= 0 ||
      input->offset >= input->bytes.size()) {
    return AVERROR_EOF;
  }

  const std::size_t requested = static_cast<std::size_t>(buffer_size);
  const std::size_t available = input->bytes.size() - input->offset;
  const std::size_t copied = std::min(requested, available);
  std::memcpy(buffer, input->bytes.data() + input->offset, copied);
  input->offset += copied;
  return static_cast<int>(copied);
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): FFmpeg AVIO callback
// ABI.
[[nodiscard]] std::int64_t seek_video_input(void *const opaque,
                                            const std::int64_t offset,
                                            const int whence) noexcept {
  auto *const input = static_cast<VideoInput *>(opaque);
  if (input == nullptr) {
    return AVERROR(EINVAL);
  }

  const int origin = whence & ~AVSEEK_FORCE;
  if (origin == AVSEEK_SIZE) {
    return static_cast<std::int64_t>(input->bytes.size());
  }

  std::int64_t base = 0;
  switch (origin) {
  case SEEK_SET:
    base = 0;
    break;
  case SEEK_CUR:
    base = static_cast<std::int64_t>(input->offset);
    break;
  case SEEK_END:
    base = static_cast<std::int64_t>(input->bytes.size());
    break;
  default:
    return AVERROR(EINVAL);
  }

  if (offset > 0 && offset > std::numeric_limits<std::int64_t>::max() - base) {
    return AVERROR(EINVAL);
  }
  if (offset < 0 && offset < -base) {
    return AVERROR(EINVAL);
  }
  const std::int64_t target = base + offset;
  if (target < 0 || target > static_cast<std::int64_t>(input->bytes.size())) {
    return AVERROR(EINVAL);
  }
  input->offset = static_cast<std::size_t>(target);
  return target;
}

struct AvioContextDeleter {
  void operator()(AVIOContext *const context) const noexcept {
    AVIOContext *owned = context;
    if (owned != nullptr) {
      av_freep(static_cast<void *>(&owned->buffer));
    }
    avio_context_free(&owned);
  }
};

struct FormatContextDeleter {
  void operator()(AVFormatContext *const context) const noexcept {
    AVFormatContext *owned = context;
    avformat_close_input(&owned);
  }
};

struct CodecContextDeleter {
  void operator()(AVCodecContext *const context) const noexcept {
    AVCodecContext *owned = context;
    avcodec_free_context(&owned);
  }
};

struct PacketDeleter {
  void operator()(AVPacket *const packet) const noexcept {
    AVPacket *owned = packet;
    av_packet_free(&owned);
  }
};

struct FrameDeleter {
  void operator()(AVFrame *const frame) const noexcept {
    AVFrame *owned = frame;
    av_frame_free(&owned);
  }
};

struct ScalerDeleter {
  void operator()(SwsContext *const context) const noexcept {
    sws_freeContext(context);
  }
};

class VideoDecoder final {
public:
  explicit VideoDecoder(const std::span<const Byte> source)
      : input_{.bytes = source} {
    constexpr int kAvioBufferBytes = 4096;
    Byte *const avio_buffer = static_cast<Byte *>(av_malloc(kAvioBufferBytes));
    if (avio_buffer == nullptr) {
      fail(ErrorCode::kResourceLimit,
           "unable to allocate a video input buffer");
    }
    AVIOContext *const avio =
        avio_alloc_context(avio_buffer, kAvioBufferBytes, 0, &input_,
                           read_video_input, nullptr, seek_video_input);
    if (avio == nullptr) {
      av_free(avio_buffer);
      fail(ErrorCode::kResourceLimit,
           "unable to initialize video input buffering");
    }
    avio_.reset(avio);

    AVFormatContext *format = avformat_alloc_context();
    if (format == nullptr) {
      fail(ErrorCode::kResourceLimit,
           "unable to allocate a video format context");
    }
    format->pb = avio_.get();
    format->flags |= AVFMT_FLAG_CUSTOM_IO;
    const int open_status =
        avformat_open_input(&format, nullptr, nullptr, nullptr);
    if (open_status < 0) {
      if (format != nullptr) {
        avformat_free_context(format);
      }
      fail(ErrorCode::kUnsupportedInput,
           "unsupported video input: " + ffmpeg_error_message(open_status));
    }
    format_.reset(format);

    const int stream_info_status =
        avformat_find_stream_info(format_.get(), nullptr);
    if (stream_info_status < 0) {
      fail(ErrorCode::kInvalidInput,
           "unable to read video stream information: " +
               ffmpeg_error_message(stream_info_status));
    }
    video_stream_index_ = av_find_best_stream(format_.get(), AVMEDIA_TYPE_VIDEO,
                                              -1, -1, nullptr, 0);
    if (video_stream_index_ < 0) {
      fail(ErrorCode::kUnsupportedInput,
           "input does not contain a decodable video stream");
    }

    stream_ = format_->streams[static_cast<std::size_t>(video_stream_index_)];
    const AVCodec *const codec =
        avcodec_find_decoder(stream_->codecpar->codec_id);
    if (codec == nullptr) {
      fail(ErrorCode::kUnsupportedInput,
           "video stream uses an unsupported codec");
    }

    codec_.reset(avcodec_alloc_context3(codec));
    if (codec_ == nullptr) {
      fail(ErrorCode::kResourceLimit,
           "unable to allocate a video decoder context");
    }
    const int parameters_status =
        avcodec_parameters_to_context(codec_.get(), stream_->codecpar);
    if (parameters_status < 0) {
      fail(ErrorCode::kInvalidInput,
           "unable to read video codec parameters: " +
               ffmpeg_error_message(parameters_status));
    }
    const int codec_status = avcodec_open2(codec_.get(), codec, nullptr);
    if (codec_status < 0) {
      fail(ErrorCode::kUnsupportedInput,
           "unable to initialize the video decoder: " +
               ffmpeg_error_message(codec_status));
    }

    if (codec_->width <= 0 || codec_->height <= 0) {
      fail(ErrorCode::kInvalidInput,
           "video stream does not declare valid dimensions");
    }
    dimensions_ = FrameDimensions{
        .width = static_cast<std::uint32_t>(codec_->width),
        .height = static_cast<std::uint32_t>(codec_->height),
    };
    static_cast<void>(
        checked_pixel_bytes(dimensions_.width, dimensions_.height));

    packet_.reset(av_packet_alloc());
    frame_.reset(av_frame_alloc());
    if (packet_ == nullptr || frame_ == nullptr) {
      fail(ErrorCode::kResourceLimit,
           "unable to allocate video decoder buffers");
    }

    time_base_ = stream_->time_base;
    frame_rate_ = av_guess_frame_rate(format_.get(), stream_, nullptr);
    stream_duration_ = stream_->duration;
  }

  VideoDecoder(const VideoDecoder &) = delete;
  VideoDecoder &operator=(const VideoDecoder &) = delete;
  VideoDecoder(VideoDecoder &&) = delete;
  VideoDecoder &operator=(VideoDecoder &&) = delete;

  [[nodiscard]] FrameDimensions dimensions() const noexcept {
    return dimensions_;
  }

  [[nodiscard]] std::uint32_t default_frame_duration_ms() const {
    if (frame_rate_.num <= 0 || frame_rate_.den <= 0) {
      return kMinimumOutputFrameDurationMs;
    }
    const AVRational milliseconds = {.num = 1, .den = 1000};
    const std::int64_t duration =
        av_rescale_q(1, av_inv_q(frame_rate_), milliseconds);
    if (duration <= 0) {
      return 1U;
    }
    return static_cast<std::uint32_t>(
        std::min(duration, static_cast<std::int64_t>(1000)));
  }

  [[nodiscard]] std::int64_t stream_duration_ms() const {
    if (stream_duration_ == AV_NOPTS_VALUE) {
      return AV_NOPTS_VALUE;
    }
    const AVRational milliseconds = {.num = 1, .den = 1000};
    return av_rescale_q(stream_duration_, time_base_, milliseconds);
  }

  template <typename Callback> void visit_decoded_frames(Callback &&callback) {
    while (true) {
      const int read_status = av_read_frame(format_.get(), packet_.get());
      if (read_status == AVERROR_EOF) {
        break;
      }
      if (read_status < 0) {
        fail(ErrorCode::kInvalidInput, "unable to read a video packet: " +
                                           ffmpeg_error_message(read_status));
      }

      if (packet_->stream_index == video_stream_index_) {
        send_packet(packet_.get(), callback);
      }
      av_packet_unref(packet_.get());
    }
    send_packet(nullptr, callback);
  }

  [[nodiscard]] Image convert_to_rgba(const AVFrame &frame) {
    if (frame.width != static_cast<int>(dimensions_.width) ||
        frame.height != static_cast<int>(dimensions_.height) ||
        frame.format < 0) {
      fail(ErrorCode::kInvalidInput,
           "video frame dimensions or pixel format changed during decoding");
    }
    const AVPixelFormat source_format =
        static_cast<AVPixelFormat>(frame.format);
    if (scaler_ == nullptr || source_format != scaler_source_format_) {
      scaler_.reset(sws_getContext(frame.width, frame.height, source_format,
                                   frame.width, frame.height, AV_PIX_FMT_RGBA,
                                   SWS_BILINEAR, nullptr, nullptr, nullptr));
      if (scaler_ == nullptr) {
        fail(ErrorCode::kUnsupportedInput,
             "unable to convert the video pixel format to RGBA");
      }
      scaler_source_format_ = source_format;
    }

    Image image{
        .width = dimensions_.width,
        .height = dimensions_.height,
        .rgba = std::vector<Byte>(
            checked_pixel_bytes(dimensions_.width, dimensions_.height)),
    };
    std::array<Byte *, 4U> destination = {nullptr, nullptr, nullptr, nullptr};
    std::array<int, 4U> destination_stride = {0, 0, 0, 0};
    const int destination_size = av_image_alloc(
        destination.data(), destination_stride.data(),
        static_cast<int>(dimensions_.width),
        static_cast<int>(dimensions_.height), AV_PIX_FMT_RGBA, 32);
    if (destination_size < 0 || destination[0] == nullptr) {
      fail(ErrorCode::kResourceLimit,
           "unable to allocate an aligned video RGBA frame");
    }
    std::unique_ptr<Byte, decltype(&av_free)> owned_destination(destination[0],
                                                                av_free);
    const int converted =
        sws_scale(scaler_.get(), frame.data, frame.linesize, 0, frame.height,
                  destination.data(), destination_stride.data());
    if (converted != frame.height) {
      fail(ErrorCode::kInvalidInput,
           "unable to convert a decoded video frame to RGBA");
    }
    for (std::uint32_t row = 0U; row < dimensions_.height; ++row) {
      std::memcpy(image.rgba.data() +
                      static_cast<std::size_t>(row) * dimensions_.width * 4U,
                  destination[0] +
                      static_cast<std::size_t>(row) *
                          static_cast<std::size_t>(destination_stride[0]),
                  static_cast<std::size_t>(dimensions_.width) * 4U);
    }
    return image;
  }

private:
  template <typename Callback>
  void send_packet(const AVPacket *const packet, Callback &&callback) {
    const int send_status = avcodec_send_packet(codec_.get(), packet);
    if (send_status < 0) {
      fail(ErrorCode::kInvalidInput,
           "unable to send a video packet to the decoder: " +
               ffmpeg_error_message(send_status));
    }

    while (true) {
      const int receive_status =
          avcodec_receive_frame(codec_.get(), frame_.get());
      if (receive_status == AVERROR(EAGAIN) || receive_status == AVERROR_EOF) {
        return;
      }
      if (receive_status < 0) {
        fail(ErrorCode::kInvalidInput,
             "unable to decode a video frame: " +
                 ffmpeg_error_message(receive_status));
      }

      std::int64_t timestamp = frame_->best_effort_timestamp;
      if (timestamp != AV_NOPTS_VALUE) {
        const AVRational milliseconds = {.num = 1, .den = 1000};
        timestamp = av_rescale_q(timestamp, time_base_, milliseconds);
      }
      callback(*frame_, timestamp);
      av_frame_unref(frame_.get());
    }
  }

  VideoInput input_;
  std::unique_ptr<AVIOContext, AvioContextDeleter> avio_;
  std::unique_ptr<AVFormatContext, FormatContextDeleter> format_;
  std::unique_ptr<AVCodecContext, CodecContextDeleter> codec_;
  std::unique_ptr<AVPacket, PacketDeleter> packet_;
  std::unique_ptr<AVFrame, FrameDeleter> frame_;
  std::unique_ptr<SwsContext, ScalerDeleter> scaler_;
  AVStream *stream_ = nullptr;
  FrameDimensions dimensions_{};
  AVRational time_base_{};
  AVRational frame_rate_{};
  std::int64_t stream_duration_ = AV_NOPTS_VALUE;
  int video_stream_index_ = -1;
  AVPixelFormat scaler_source_format_ = AV_PIX_FMT_NONE;
};

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

[[nodiscard]] bool is_gif(const std::span<const Byte> bytes) {
  constexpr std::array<Byte, 6U> kGif87a = {'G', 'I', 'F', '8', '7', 'a'};
  constexpr std::array<Byte, 6U> kGif89a = {'G', 'I', 'F', '8', '9', 'a'};
  return starts_with(bytes, kGif87a) || starts_with(bytes, kGif89a);
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

[[nodiscard]] std::vector<std::uint32_t>
video_frame_durations(const std::vector<std::int64_t> &timestamps,
                      const VideoTiming timing) {
  if (timestamps.empty()) {
    fail(ErrorCode::kInvalidInput, "video stream has no decoded frames");
  }

  std::vector<std::uint32_t> durations(timestamps.size(),
                                       timing.fallback_frame_duration_ms);
  std::uint64_t total_duration_ms = 0U;
  for (std::size_t index = 0U; index < timestamps.size(); ++index) {
    std::int64_t duration =
        static_cast<std::int64_t>(timing.fallback_frame_duration_ms);
    if (index + 1U < timestamps.size()) {
      duration = timestamps[index + 1U] - timestamps[index];
    } else if (timing.stream_duration_ms > timestamps[index]) {
      duration = timing.stream_duration_ms - timestamps[index];
    }
    if (duration <= 0) {
      duration = static_cast<std::int64_t>(timing.fallback_frame_duration_ms);
    }
    if (duration > static_cast<std::int64_t>(kMaxAnimationDurationMs)) {
      fail(ErrorCode::kResourceLimit,
           "video frame duration exceeds the native worker limit");
    }

    total_duration_ms += static_cast<std::uint64_t>(duration);
    if (total_duration_ms > kMaxAnimationDurationMs) {
      fail(ErrorCode::kResourceLimit,
           "video duration exceeds the WhatsApp duration limit");
    }
    durations[index] = static_cast<std::uint32_t>(duration);
  }
  return durations;
}

[[nodiscard]] VideoInfo inspect_video(const std::span<const Byte> source) {
  VideoDecoder decoder(source);
  const FrameDimensions dimensions = decoder.dimensions();
  const std::uint64_t source_pixels =
      static_cast<std::uint64_t>(dimensions.width) * dimensions.height;
  const std::uint32_t fallback_duration_ms =
      decoder.default_frame_duration_ms();

  std::vector<std::int64_t> timestamps;
  timestamps.reserve(kMaxInputFrames);
  bool has_timestamp_origin = false;
  std::int64_t timestamp_origin = 0;
  std::int64_t previous_timestamp = 0;

  decoder.visit_decoded_frames([&](const AVFrame &frame,
                                   const std::int64_t timestamp) {
    if (frame.width != static_cast<int>(dimensions.width) ||
        frame.height != static_cast<int>(dimensions.height)) {
      fail(ErrorCode::kInvalidInput,
           "video frame dimensions changed during decoding");
    }
    if (timestamps.size() >= kMaxInputFrames ||
        static_cast<std::uint64_t>(timestamps.size() + 1U) >
            kMaxAnimationSourcePixels / source_pixels) {
      fail(ErrorCode::kResourceLimit,
           "video animation exceeds the native worker frame or pixel limit");
    }

    std::int64_t normalized = 0;
    if (timestamp != AV_NOPTS_VALUE) {
      if (!has_timestamp_origin) {
        timestamp_origin = timestamp;
        has_timestamp_origin = true;
      }
      if (timestamp >= timestamp_origin) {
        if (timestamp_origin < 0 &&
            timestamp >
                std::numeric_limits<std::int64_t>::max() + timestamp_origin) {
          fail(ErrorCode::kInvalidInput,
               "video frame timestamps exceed the native worker range");
        }
        normalized = timestamp - timestamp_origin;
      }
    }
    if (!timestamps.empty()) {
      if (timestamp == AV_NOPTS_VALUE || normalized <= previous_timestamp) {
        if (previous_timestamp >
            std::numeric_limits<std::int64_t>::max() -
                static_cast<std::int64_t>(fallback_duration_ms)) {
          fail(ErrorCode::kInvalidInput,
               "video frame timestamps overflow the native worker range");
        }
        normalized = previous_timestamp +
                     static_cast<std::int64_t>(fallback_duration_ms);
      }
    }
    timestamps.push_back(normalized);
    previous_timestamp = normalized;
  });

  return VideoInfo{
      .width = dimensions.width,
      .height = dimensions.height,
      .frame_durations_ms = video_frame_durations(
          timestamps,
          VideoTiming{
              .stream_duration_ms = decoder.stream_duration_ms(),
              .fallback_frame_duration_ms = fallback_duration_ms,
          }),
  };
}

template <typename Callback>
void visit_video_frames(const std::span<const Byte> source,
                        const VideoInfo &info, Callback &&callback) {
  VideoDecoder decoder(source);
  const FrameDimensions dimensions = decoder.dimensions();
  if (dimensions.width != info.width || dimensions.height != info.height) {
    fail(ErrorCode::kInvalidInput,
         "video dimensions changed between decoding passes");
  }

  std::uint32_t index = 0U;
  decoder.visit_decoded_frames(
      [&](const AVFrame &frame, const std::int64_t timestamp) {
        static_cast<void>(timestamp);
        if (index >= info.frame_durations_ms.size()) {
          fail(ErrorCode::kInvalidInput,
               "video frame count changed between decoding passes");
        }
        const Image rgba = decoder.convert_to_rgba(frame);
        callback(index, std::span<const Byte>(rgba.rgba));
        ++index;
      });
  if (index != info.frame_durations_ms.size()) {
    fail(ErrorCode::kInvalidInput,
         "video frame count changed between decoding passes");
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
    // The default static path favors bounded latency. Transparent artwork can
    // spend seconds optimizing a lossless-quality alpha plane at method 6.
    // Method 4 keeps alpha edges exact without exhaustive analysis.
    config.method = 4;
    config.alpha_quality = 100;
    config.thread_level = 1;
    config.exact = 0;
    config.use_sharp_yuv = 0;
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

[[nodiscard]] std::vector<Byte>
inject_exif(const std::vector<Byte> &encoded,
            const std::span<const Byte> metadata_json) {
  if (metadata_json.empty() ||
      metadata_json.size() > kMaximumExifMetadataBytes) {
    fail(ErrorCode::kInvalidInput, "invalid sticker EXIF metadata");
  }

  constexpr std::array<Byte, 22U> kExifHeader = {
      0x49U, 0x49U, 0x2aU, 0x00U, 0x08U, 0x00U, 0x00U, 0x00U,
      0x01U, 0x00U, 0x41U, 0x57U, 0x07U, 0x00U, 0x00U, 0x00U,
      0x00U, 0x00U, 0x16U, 0x00U, 0x00U, 0x00U,
  };
  std::vector<Byte> exif(kExifHeader.size() + metadata_json.size());
  std::copy(kExifHeader.begin(), kExifHeader.end(), exif.begin());
  std::copy(metadata_json.begin(), metadata_json.end(),
            exif.begin() + static_cast<std::ptrdiff_t>(kExifHeader.size()));
  const std::uint32_t json_size =
      static_cast<std::uint32_t>(metadata_json.size());
  exif[14U] = static_cast<Byte>(json_size & 0xffU);
  exif[15U] = static_cast<Byte>((json_size >> 8U) & 0xffU);
  exif[16U] = static_cast<Byte>((json_size >> 16U) & 0xffU);
  exif[17U] = static_cast<Byte>((json_size >> 24U) & 0xffU);

  const WebPData input = {
      .bytes = encoded.data(),
      .size = encoded.size(),
  };
  std::unique_ptr<WebPMux, decltype(&WebPMuxDelete)> mux(
      WebPMuxCreate(&input, 1), &WebPMuxDelete);
  if (mux == nullptr) {
    fail(ErrorCode::kEncodingFailed, "unable to create WebP metadata mux");
  }
  const WebPData chunk = {
      .bytes = exif.data(),
      .size = exif.size(),
  };
  if (WebPMuxSetChunk(mux.get(), "EXIF", &chunk, 1) != WEBP_MUX_OK) {
    fail(ErrorCode::kEncodingFailed, "unable to attach WebP EXIF metadata");
  }

  EncodedData assembled;
  if (WebPMuxAssemble(mux.get(), assembled.get()) != WEBP_MUX_OK) {
    fail(ErrorCode::kEncodingFailed,
         "unable to assemble WebP with EXIF metadata");
  }
  return assembled.copy();
}

[[nodiscard]] StickerResult validate_output(std::vector<Byte> bytes,
                                            const bool expect_animation,
                                            const std::size_t byte_limit) {
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

  if (bytes.size() > byte_limit) {
    fail(ErrorCode::kValidationFailed,
         "encoded WebP exceeds the permitted output size");
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

[[nodiscard]] StickerResult encode_static_limited(const Image &image,
                                                  const bool prefer_lossless) {
  std::vector<Byte> encoded;
  if (prefer_lossless) {
    encoded = encode_static(image, 100, true);
    if (encoded.size() <= kMaximumStaticStickerBytes) {
      return validate_output(std::move(encoded), false,
                             kMaximumStaticStickerBytes);
    }
  }

  constexpr std::array<int, 9U> kQualityCandidates = {90, 80, 70, 60, 50,
                                                      40, 30, 20, 10};
  for (const int quality : kQualityCandidates) {
    encoded = encode_static(image, quality, false);
    if (encoded.size() <= kMaximumStaticStickerBytes) {
      return validate_output(std::move(encoded), false,
                             kMaximumStaticStickerBytes);
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
      return validate_output(std::move(encoded), true,
                             kMaximumAnimatedStickerBytes);
    }
  }
  fail(ErrorCode::kEncodingFailed,
       "unable to fit the animated sticker inside the WhatsApp size limit");
}

[[nodiscard]] AnimationPlan
plan_video_animation(const std::span<const Byte> source, const VideoInfo &info,
                     const TranscodeOptions &options) {
  Bounds bounds{};
  std::vector<FrameSelection> selections;
  selections.reserve(info.frame_durations_ms.size());
  std::uint64_t duration_ms = 0U;

  visit_video_frames(
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
               "video duration exceeds the WhatsApp duration limit");
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
    fail(ErrorCode::kInvalidInput, "video animation has no frames");
  }

  return AnimationPlan{
      .geometry = make_geometry_plan(bounds, info.width, info.height, options),
      .selections = std::move(selections),
      .duration_ms = static_cast<std::uint32_t>(duration_ms),
  };
}

[[nodiscard]] Image
render_video_animation_frame(const std::span<const Byte> source,
                             const VideoInfo &info, const AnimationPlan &plan,
                             const std::uint32_t wanted_index,
                             const TranscodeOptions &options) {
  Image rendered{};
  bool found = false;
  visit_video_frames(
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
    fail(ErrorCode::kInvalidInput, "requested video frame is unavailable");
  }
  return rendered;
}

[[nodiscard]] std::vector<Byte>
encode_video_animation(const std::span<const Byte> source,
                       const VideoInfo &info, const AnimationPlan &plan,
                       const TranscodeOptions &options, const int quality) {
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
  visit_video_frames(
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
               "unable to encode a video animation frame");
        }
        timestamp_ms += plan.selections[selection_index].duration_ms;
        ++selection_index;
      });

  if (selection_index != plan.selections.size()) {
    fail(ErrorCode::kInvalidInput,
         "video frame selection could not be reconstructed");
  }
  if (WebPAnimEncoderAdd(encoder.get(), nullptr, static_cast<int>(timestamp_ms),
                         nullptr) == 0) {
    fail(ErrorCode::kEncodingFailed,
         "unable to finalize the video animation timing");
  }

  EncodedData encoded;
  if (WebPAnimEncoderAssemble(encoder.get(), encoded.get()) == 0) {
    fail(ErrorCode::kEncodingFailed,
         "unable to assemble animated WebP video output");
  }
  return encoded.copy();
}

[[nodiscard]] StickerResult
encode_video_animation_limited(const std::span<const Byte> source,
                               const VideoInfo &info, const AnimationPlan &plan,
                               const TranscodeOptions &options) {
  constexpr std::array<int, 9U> kQualityCandidates = {90, 80, 70, 60, 50,
                                                      40, 30, 20, 10};
  for (const int quality : kQualityCandidates) {
    std::vector<Byte> encoded =
        encode_video_animation(source, info, plan, options, quality);
    if (encoded.size() <= kMaximumAnimatedStickerBytes) {
      return validate_output(std::move(encoded), true,
                             kMaximumAnimatedStickerBytes);
    }
  }
  fail(ErrorCode::kEncodingFailed,
       "unable to fit the video sticker inside the WhatsApp size limit");
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
  return encode_static_limited(rendered, options.prefer_lossless);
}

[[nodiscard]] StickerResult
transcode_animated_webp(const std::span<const Byte> source,
                        const WebpInfo &info, const TranscodeOptions &options) {
  const AnimationPlan plan = plan_animation(source, info, options);
  if (plan.selections.size() == 1U) {
    return encode_static_limited(
        render_animation_frame(source, info, plan,
                               plan.selections.front().source_index, options),
        options.prefer_lossless);
  }
  return encode_animation_limited(source, info, plan, options);
}

[[nodiscard]] StickerResult
transcode_animated_video(const std::span<const Byte> source,
                         const VideoInfo &info,
                         const TranscodeOptions &options) {
  const AnimationPlan plan = plan_video_animation(source, info, options);
  if (plan.selections.size() == 1U) {
    return encode_static_limited(
        render_video_animation_frame(
            source, info, plan, plan.selections.front().source_index, options),
        options.prefer_lossless);
  }
  return encode_video_animation_limited(source, info, plan, options);
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
    if (options.exif_json.has_value() &&
        (options.exif_json->empty() ||
         options.exif_json->size() > kMaximumExifMetadataBytes)) {
      fail(ErrorCode::kInvalidInput, "invalid sticker EXIF metadata");
    }

    StickerResult result;
    if (is_webp(source)) {
      const WebpInfo info = inspect_webp(source);
      if (info.animated()) {
        result = transcode_animated_webp(source, info, options);
      } else {
        result = transcode_static(decode_static_webp(source, info), options);
      }
    } else if (is_png(source)) {
      if (is_apng(source)) {
        fail(ErrorCode::kUnsupportedInput,
             "APNG input is not supported; convert it to WebP first");
      }
      result = transcode_static(decode_png(source), options);
    } else if (is_jpeg(source)) {
      result = transcode_static(decode_jpeg(source), options);
    } else if (is_gif(source)) {
      fail(ErrorCode::kUnsupportedInput,
           "GIF input is not supported; use a video or animated WebP instead");
    } else {
      result = transcode_animated_video(source, inspect_video(source), options);
    }
    if (options.exif_json.has_value()) {
      return validate_output(
          inject_exif(result.bytes, *options.exif_json), result.animated,
          (result.animated ? kMaximumAnimatedStickerBytes
                           : kMaximumStaticStickerBytes) +
              options.exif_json->size() + kMaximumExifMuxOverheadBytes);
    }
    return result;
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
