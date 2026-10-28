#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

extern "C" {
#include <jpeglib.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/mem.h>
}

#include <png.h>
#include <webp/encode.h>
#include <webp/mux.h>

namespace {

using Byte = std::uint8_t;

constexpr std::uint32_t kPhotoWidth = 1280U;
constexpr std::uint32_t kPhotoHeight = 854U;
constexpr std::uint32_t kGraphicSize = 1024U;
constexpr std::uint32_t kAnimationSize = 384U;
constexpr std::uint32_t kVideoWidth = 640U;
constexpr std::uint32_t kVideoHeight = 360U;
constexpr std::uint32_t kAnimationFrameCount = 3U;
constexpr std::uint32_t kVideoFrameCount = 3U;
constexpr int kFrameDurationMs = 100;

[[noreturn]] void fail(const std::string_view message) {
  throw std::runtime_error(std::string(message));
}

[[nodiscard]] std::size_t pixel_offset(const std::uint32_t x,
                                       const std::uint32_t y,
                                       const std::uint32_t width) {
  return (static_cast<std::size_t>(y) * width + x) * 4U;
}

[[nodiscard]] Byte byte_from_unit(const double value) {
  const double clamped = std::clamp(value, 0.0, 1.0);
  return static_cast<Byte>(std::lround(clamped * 255.0));
}

struct Image {
  std::uint32_t width = 0U;
  std::uint32_t height = 0U;
  std::vector<Byte> rgba;
};

struct Dimensions {
  std::uint32_t width = 0U;
  std::uint32_t height = 0U;
};

[[nodiscard]] Image make_photo(const Dimensions dimensions,
                               const std::uint32_t frame = 0U) {
  const std::uint32_t width = dimensions.width;
  const std::uint32_t height = dimensions.height;
  Image image{
      .width = width,
      .height = height,
      .rgba = std::vector<Byte>(static_cast<std::size_t>(width) * height * 4U,
                                255U),
  };
  const double phase = static_cast<double>(frame) * 0.18;

  for (std::uint32_t y = 0U; y < height; ++y) {
    const double vertical =
        static_cast<double>(y) / static_cast<double>(std::max(height - 1U, 1U));
    for (std::uint32_t x = 0U; x < width; ++x) {
      const double horizontal = static_cast<double>(x) /
                                static_cast<double>(std::max(width - 1U, 1U));
      const double ripple =
          0.5 + 0.5 * std::sin(horizontal * 24.0 + vertical * 17.0 + phase);
      const double fine_detail =
          0.5 + 0.5 * std::sin(horizontal * 111.0 - vertical * 83.0 + phase);
      const bool sky = vertical < 0.53;
      double red = 0.0;
      double green = 0.0;
      double blue = 0.0;
      if (sky) {
        const double altitude = 1.0 - vertical / 0.53;
        red = 0.16 + altitude * 0.35 + ripple * 0.06;
        green = 0.34 + altitude * 0.33 + ripple * 0.04;
        blue = 0.48 + altitude * 0.34 + ripple * 0.08;
      } else {
        const double ground = (vertical - 0.53) / 0.47;
        red = 0.08 + ground * 0.20 + ripple * 0.16;
        green = 0.24 + ground * 0.28 + ripple * 0.20;
        blue = 0.10 + ground * 0.09 + fine_detail * 0.08;
      }

      const double subject_x = horizontal - (0.48 + 0.16 * std::sin(phase));
      const double subject_y = vertical - 0.57;
      const double subject = std::exp(
          -(subject_x * subject_x * 65.0 + subject_y * subject_y * 26.0));
      red = red * (1.0 - subject) + 0.86 * subject;
      green = green * (1.0 - subject) + 0.36 * subject;
      blue = blue * (1.0 - subject) + 0.12 * subject;

      const std::size_t offset = pixel_offset(x, y, width);
      image.rgba[offset] = byte_from_unit(red);
      image.rgba[offset + 1U] = byte_from_unit(green);
      image.rgba[offset + 2U] = byte_from_unit(blue);
    }
  }
  return image;
}

[[nodiscard]] Image make_graphic(const std::uint32_t size) {
  Image image{
      .width = size,
      .height = size,
      .rgba = std::vector<Byte>(static_cast<std::size_t>(size) * size * 4U, 0U),
  };
  const double center = static_cast<double>(size - 1U) / 2.0;
  const double radius = static_cast<double>(size) * 0.39;
  for (std::uint32_t y = 0U; y < size; ++y) {
    for (std::uint32_t x = 0U; x < size; ++x) {
      const double dx = static_cast<double>(x) - center;
      const double dy = static_cast<double>(y) - center;
      const double distance = std::sqrt(dx * dx + dy * dy);
      if (distance > radius) {
        continue;
      }
      const double edge = std::clamp(radius - distance, 0.0, 8.0) / 8.0;
      const double angle = std::atan2(dy, dx);
      const double wave = 0.5 + 0.5 * std::sin(angle * 7.0 + distance / 42.0);
      const std::size_t offset = pixel_offset(x, y, size);
      image.rgba[offset] = byte_from_unit(0.25 + 0.55 * wave);
      image.rgba[offset + 1U] = byte_from_unit(0.20 + 0.65 * (1.0 - wave));
      image.rgba[offset + 2U] = byte_from_unit(0.65 + 0.28 * wave);
      image.rgba[offset + 3U] = byte_from_unit(edge);
    }
  }
  return image;
}

void write_bytes(const std::filesystem::path &path,
                 const std::span<const Byte> bytes) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  if (!file) {
    fail("unable to open fixture output file");
  }
  file.write(reinterpret_cast<const char *>(bytes.data()),
             static_cast<std::streamsize>(bytes.size()));
  if (!file) {
    fail("unable to write fixture output file");
  }
}

void write_png(const std::filesystem::path &path, const Image &image) {
  png_image writer{};
  writer.version = PNG_IMAGE_VERSION;
  writer.width = image.width;
  writer.height = image.height;
  writer.format = PNG_FORMAT_RGBA;
  if (png_image_write_to_file(&writer, path.c_str(), 0, image.rgba.data(), 0,
                              nullptr) == 0) {
    fail("unable to encode PNG benchmark fixture");
  }
}

class JpegCompressor final {
public:
  JpegCompressor() {
    compressor_.err = jpeg_std_error(&error_);
    jpeg_create_compress(&compressor_);
  }

  ~JpegCompressor() { jpeg_destroy_compress(&compressor_); }

  JpegCompressor(const JpegCompressor &) = delete;
  JpegCompressor &operator=(const JpegCompressor &) = delete;

  [[nodiscard]] jpeg_compress_struct *get() { return &compressor_; }

private:
  jpeg_compress_struct compressor_{};
  jpeg_error_mgr error_{};
};

void write_jpeg(const std::filesystem::path &path, const Image &image) {
  JpegCompressor encoder;
  jpeg_compress_struct *const compressor = encoder.get();
  std::FILE *const output = std::fopen(path.c_str(), "wb");
  if (output == nullptr) {
    fail("unable to open JPEG benchmark fixture");
  }
  const auto close_output = [&output]() { std::fclose(output); };
  struct FileCloser final {
    decltype(close_output) close;
    ~FileCloser() { close(); }
  } closer{close_output};

  jpeg_stdio_dest(compressor, output);
  compressor->image_width = image.width;
  compressor->image_height = image.height;
  compressor->input_components = 3;
  compressor->in_color_space = JCS_RGB;
  jpeg_set_defaults(compressor);
  jpeg_set_quality(compressor, 88, TRUE);
  jpeg_start_compress(compressor, TRUE);

  std::vector<Byte> row(static_cast<std::size_t>(image.width) * 3U);
  while (compressor->next_scanline < compressor->image_height) {
    const std::uint32_t y = compressor->next_scanline;
    for (std::uint32_t x = 0U; x < image.width; ++x) {
      const std::size_t source_offset = pixel_offset(x, y, image.width);
      const std::size_t destination_offset = static_cast<std::size_t>(x) * 3U;
      row[destination_offset] = image.rgba[source_offset];
      row[destination_offset + 1U] = image.rgba[source_offset + 1U];
      row[destination_offset + 2U] = image.rgba[source_offset + 2U];
    }
    JSAMPROW scanline = row.data();
    if (jpeg_write_scanlines(compressor, &scanline, 1U) != 1U) {
      fail("unable to write JPEG benchmark fixture scanline");
    }
  }
  jpeg_finish_compress(compressor);
}

void write_static_webp(const std::filesystem::path &path, const Image &image) {
  Byte *encoded = nullptr;
  const std::size_t encoded_size =
      WebPEncodeRGBA(image.rgba.data(), static_cast<int>(image.width),
                     static_cast<int>(image.height),
                     static_cast<int>(image.width * 4U), 82.0F, &encoded);
  std::unique_ptr<Byte, decltype(&WebPFree)> owned_encoded(encoded, WebPFree);
  if (encoded_size == 0U || !owned_encoded) {
    fail("unable to encode static WebP benchmark fixture");
  }
  write_bytes(path, std::span<const Byte>(owned_encoded.get(), encoded_size));
}

class WebpPicture final {
public:
  WebpPicture() {
    if (WebPPictureInit(&picture_) == 0) {
      fail("unable to initialize WebP benchmark fixture picture");
    }
  }

  ~WebpPicture() { WebPPictureFree(&picture_); }

  WebpPicture(const WebpPicture &) = delete;
  WebpPicture &operator=(const WebpPicture &) = delete;

  void import_rgba(const Image &image) {
    picture_.width = static_cast<int>(image.width);
    picture_.height = static_cast<int>(image.height);
    if (WebPPictureImportRGBA(&picture_, image.rgba.data(),
                              static_cast<int>(image.width * 4U)) == 0) {
      fail("unable to import benchmark fixture animation frame");
    }
  }

  [[nodiscard]] WebPPicture *get() { return &picture_; }

private:
  WebPPicture picture_{};
};

void write_animated_webp(const std::filesystem::path &path) {
  WebPAnimEncoderOptions options{};
  if (WebPAnimEncoderOptionsInit(&options) == 0) {
    fail("unable to initialize WebP animation fixture options");
  }
  options.anim_params.loop_count = 0;
  options.minimize_size = 1;
  using EncoderPtr =
      std::unique_ptr<WebPAnimEncoder, decltype(&WebPAnimEncoderDelete)>;
  EncoderPtr encoder(WebPAnimEncoderNew(static_cast<int>(kAnimationSize),
                                        static_cast<int>(kAnimationSize),
                                        &options),
                     WebPAnimEncoderDelete);
  if (!encoder) {
    fail("unable to allocate animated WebP benchmark fixture encoder");
  }

  WebPConfig config{};
  if (WebPConfigPreset(&config, WEBP_PRESET_PICTURE, 75.0F) == 0) {
    fail("unable to configure animated WebP benchmark fixture");
  }
  config.method = 4;
  config.thread_level = 1;
  if (WebPValidateConfig(&config) == 0) {
    fail("animated WebP benchmark fixture configuration is invalid");
  }

  for (std::uint32_t frame = 0U; frame < kAnimationFrameCount; ++frame) {
    WebpPicture picture;
    picture.import_rgba(make_photo(
        Dimensions{.width = kAnimationSize, .height = kAnimationSize}, frame));
    if (WebPAnimEncoderAdd(encoder.get(), picture.get(),
                           static_cast<int>(frame) * kFrameDurationMs,
                           &config) == 0) {
      fail("unable to add animated WebP benchmark fixture frame");
    }
  }
  if (WebPAnimEncoderAdd(encoder.get(), nullptr,
                         static_cast<int>(kAnimationFrameCount) *
                             kFrameDurationMs,
                         nullptr) == 0) {
    fail("unable to finalize animated WebP benchmark fixture");
  }

  WebPData encoded{};
  WebPDataInit(&encoded);
  const auto clear_encoded = [&encoded]() { WebPDataClear(&encoded); };
  struct DataCloser final {
    decltype(clear_encoded) clear;
    ~DataCloser() { clear(); }
  } closer{clear_encoded};
  if (WebPAnimEncoderAssemble(encoder.get(), &encoded) == 0 ||
      encoded.bytes == nullptr || encoded.size == 0U) {
    fail("unable to assemble animated WebP benchmark fixture");
  }
  write_bytes(path, std::span<const Byte>(encoded.bytes, encoded.size));
}

struct AvFormatContextDeleter {
  void operator()(AVFormatContext *const context) const noexcept {
    avformat_free_context(context);
  }
};

struct AvCodecContextDeleter {
  void operator()(AVCodecContext *const context) const noexcept {
    AVCodecContext *owned = context;
    avcodec_free_context(&owned);
  }
};

struct AvPacketDeleter {
  void operator()(AVPacket *const packet) const noexcept {
    AVPacket *owned = packet;
    av_packet_free(&owned);
  }
};

struct AvFrameDeleter {
  void operator()(AVFrame *const frame) const noexcept {
    AVFrame *owned = frame;
    av_frame_free(&owned);
  }
};

void write_video_packets(AVFormatContext *const format,
                         AVCodecContext *const encoder, AVStream *const stream,
                         AVPacket *const packet) {
  while (true) {
    const int receive_status = avcodec_receive_packet(encoder, packet);
    if (receive_status == AVERROR(EAGAIN) || receive_status == AVERROR_EOF) {
      return;
    }
    if (receive_status < 0) {
      fail("unable to encode MP4 benchmark fixture packet");
    }
    av_packet_rescale_ts(packet, encoder->time_base, stream->time_base);
    packet->stream_index = stream->index;
    const int write_status = av_interleaved_write_frame(format, packet);
    av_packet_unref(packet);
    if (write_status < 0) {
      fail("unable to mux MP4 benchmark fixture packet");
    }
  }
}

[[nodiscard]] Byte clamp_to_byte(const int value) {
  return static_cast<Byte>(std::clamp(value, 0, 255));
}

void import_yuv420(AVFrame *const frame, const Image &image) {
  for (std::uint32_t y = 0U; y < image.height; ++y) {
    for (std::uint32_t x = 0U; x < image.width; ++x) {
      const std::size_t offset = pixel_offset(x, y, image.width);
      const int red = image.rgba[offset];
      const int green = image.rgba[offset + 1U];
      const int blue = image.rgba[offset + 2U];
      frame->data[0][static_cast<std::ptrdiff_t>(y) * frame->linesize[0] +
                     static_cast<std::ptrdiff_t>(x)] =
          clamp_to_byte(((66 * red + 129 * green + 25 * blue + 128) >> 8) + 16);
    }
  }

  for (std::uint32_t y = 0U; y < image.height; y += 2U) {
    for (std::uint32_t x = 0U; x < image.width; x += 2U) {
      int red = 0;
      int green = 0;
      int blue = 0;
      for (std::uint32_t sample_y = 0U; sample_y < 2U; ++sample_y) {
        for (std::uint32_t sample_x = 0U; sample_x < 2U; ++sample_x) {
          const std::size_t offset =
              pixel_offset(x + sample_x, y + sample_y, image.width);
          red += image.rgba[offset];
          green += image.rgba[offset + 1U];
          blue += image.rgba[offset + 2U];
        }
      }
      red /= 4;
      green /= 4;
      blue /= 4;
      const std::ptrdiff_t output_offset =
          static_cast<std::ptrdiff_t>(y / 2U) * frame->linesize[1] +
          static_cast<std::ptrdiff_t>(x / 2U);
      frame->data[1][output_offset] = clamp_to_byte(
          ((-38 * red - 74 * green + 112 * blue + 128) >> 8) + 128);
      frame->data[2][output_offset] = clamp_to_byte(
          ((112 * red - 94 * green - 18 * blue + 128) >> 8) + 128);
    }
  }
}

void write_mp4(const std::filesystem::path &path) {
  AVFormatContext *raw_format = nullptr;
  if (avformat_alloc_output_context2(&raw_format, nullptr, "mp4", nullptr) <
          0 ||
      raw_format == nullptr) {
    fail("unable to allocate MP4 benchmark fixture output");
  }
  std::unique_ptr<AVFormatContext, AvFormatContextDeleter> format(raw_format);

  const AVCodec *const codec = avcodec_find_encoder(AV_CODEC_ID_MPEG4);
  if (codec == nullptr) {
    fail("MPEG-4 encoder is unavailable");
  }
  AVStream *const stream = avformat_new_stream(format.get(), nullptr);
  if (stream == nullptr) {
    fail("unable to allocate MP4 benchmark fixture stream");
  }

  std::unique_ptr<AVCodecContext, AvCodecContextDeleter> encoder(
      avcodec_alloc_context3(codec));
  if (!encoder) {
    fail("unable to allocate MP4 benchmark fixture encoder");
  }
  encoder->codec_id = codec->id;
  encoder->codec_type = AVMEDIA_TYPE_VIDEO;
  encoder->pix_fmt = AV_PIX_FMT_YUV420P;
  encoder->width = static_cast<int>(kVideoWidth);
  encoder->height = static_cast<int>(kVideoHeight);
  encoder->time_base = AVRational{.num = 1, .den = 10};
  encoder->framerate = AVRational{.num = 10, .den = 1};
  encoder->gop_size = 12;
  encoder->max_b_frames = 0;
  if ((format->oformat->flags & AVFMT_GLOBALHEADER) != 0) {
    encoder->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
  }
  if (avcodec_open2(encoder.get(), codec, nullptr) < 0) {
    fail("unable to open MP4 benchmark fixture encoder");
  }

  stream->time_base = encoder->time_base;
  if (avcodec_parameters_from_context(stream->codecpar, encoder.get()) < 0 ||
      avio_open_dyn_buf(&format->pb) < 0 ||
      avformat_write_header(format.get(), nullptr) < 0) {
    fail("unable to initialize MP4 benchmark fixture");
  }

  std::unique_ptr<AVFrame, AvFrameDeleter> frame(av_frame_alloc());
  std::unique_ptr<AVPacket, AvPacketDeleter> packet(av_packet_alloc());
  if (!frame || !packet) {
    fail("unable to allocate MP4 benchmark fixture buffers");
  }
  frame->format = encoder->pix_fmt;
  frame->width = encoder->width;
  frame->height = encoder->height;
  if (av_frame_get_buffer(frame.get(), 0) < 0) {
    fail("unable to allocate MP4 benchmark fixture frame");
  }

  for (std::uint32_t index = 0U; index < kVideoFrameCount; ++index) {
    if (av_frame_make_writable(frame.get()) < 0) {
      fail("unable to make MP4 benchmark fixture frame writable");
    }
    import_yuv420(frame.get(), make_photo(Dimensions{.width = kVideoWidth,
                                                     .height = kVideoHeight},
                                          index));
    frame->pts = static_cast<std::int64_t>(index);
    if (avcodec_send_frame(encoder.get(), frame.get()) < 0) {
      fail("unable to send MP4 benchmark fixture frame");
    }
    write_video_packets(format.get(), encoder.get(), stream, packet.get());
  }
  if (avcodec_send_frame(encoder.get(), nullptr) < 0) {
    fail("unable to flush MP4 benchmark fixture encoder");
  }
  write_video_packets(format.get(), encoder.get(), stream, packet.get());
  if (av_write_trailer(format.get()) < 0) {
    fail("unable to finalize MP4 benchmark fixture");
  }

  Byte *encoded = nullptr;
  const int encoded_size = avio_close_dyn_buf(format->pb, &encoded);
  format->pb = nullptr;
  std::unique_ptr<Byte, decltype(&av_free)> owned_encoded(encoded, av_free);
  if (encoded_size <= 0 || !owned_encoded) {
    fail("unable to read MP4 benchmark fixture");
  }
  write_bytes(path,
              std::span<const Byte>(owned_encoded.get(),
                                    static_cast<std::size_t>(encoded_size)));
}

void generate(const std::filesystem::path &output_directory) {
  std::filesystem::create_directories(output_directory);
  const Image photo =
      make_photo(Dimensions{.width = kPhotoWidth, .height = kPhotoHeight});
  write_png(output_directory / "photo.png", photo);
  write_jpeg(output_directory / "photo.jpg", photo);
  write_static_webp(output_directory / "photo.webp", photo);
  write_static_webp(output_directory / "graphic.webp",
                    make_graphic(kGraphicSize));
  write_animated_webp(output_directory / "animated.webp");
  write_mp4(output_directory / "clip.mp4");
}

} // namespace

int main(const int argc, char *const argv[]) {
  try {
    if (argc != 2) {
      fail("usage: sticker_benchmark_fixtures <output-directory>");
    }
    generate(argv[1]);
    std::cout << "Generated benchmark fixtures in " << argv[1] << '\n';
    return EXIT_SUCCESS;
  } catch (const std::exception &error) {
    std::cerr << "Benchmark fixture generation failed: " << error.what()
              << '\n';
    return EXIT_FAILURE;
  }
}
