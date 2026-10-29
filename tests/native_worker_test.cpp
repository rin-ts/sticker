#include "sticker_pipeline.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
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

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/mem.h>
}

namespace {

using Byte = std::uint8_t;

void require(const bool condition, const std::string &message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
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

[[nodiscard]] std::vector<Byte> encode_exif_boundary_source() {
  constexpr int kWidth = 512;
  constexpr int kHeight = 512;
  constexpr std::uint32_t kDetail = 32U;
  std::vector<Byte> pixels(static_cast<std::size_t>(kWidth) * kHeight * 4U);
  std::uint32_t state = 0x12345678U;
  for (int y = 0; y < kHeight; ++y) {
    for (int x = 0; x < kWidth; ++x) {
      state = state * 1664525U + 1013904223U;
      const std::uint32_t base = (static_cast<std::uint32_t>(x) * 3U +
                                  static_cast<std::uint32_t>(y) * 5U) &
                                 0xffU;
      const std::size_t offset =
          (static_cast<std::size_t>(y) * kWidth + x) * 4U;
      pixels[offset] =
          static_cast<Byte>((base + ((state >> 24U) * kDetail >> 8U)) & 0xffU);
      pixels[offset + 1U] = static_cast<Byte>(
          (base * 3U + (((state >> 16U) & 0xffU) * kDetail >> 8U)) & 0xffU);
      pixels[offset + 2U] = static_cast<Byte>(
          (base * 7U + (((state >> 8U) & 0xffU) * kDetail >> 8U)) & 0xffU);
      pixels[offset + 3U] = 255U;
    }
  }

  Byte *encoded = nullptr;
  const std::size_t encoded_size = WebPEncodeLosslessRGBA(
      pixels.data(), kWidth, kHeight, kWidth * 4, &encoded);
  require(encoded_size != 0U && encoded != nullptr,
          "failed to encode EXIF boundary source");
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

void write_video_packets(AVFormatContext *const format,
                         AVCodecContext *const encoder, AVStream *const stream,
                         AVPacket *const packet) {
  while (true) {
    const int receive_status = avcodec_receive_packet(encoder, packet);
    if (receive_status == AVERROR(EAGAIN) || receive_status == AVERROR_EOF) {
      return;
    }
    require(receive_status >= 0, "failed to encode a video packet");
    av_packet_rescale_ts(packet, encoder->time_base, stream->time_base);
    packet->stream_index = stream->index;
    const int write_status = av_interleaved_write_frame(format, packet);
    av_packet_unref(packet);
    require(write_status >= 0, "failed to mux a video packet");
  }
}

[[nodiscard]] std::vector<Byte> encode_video_source() {
  AVFormatContext *raw_format = nullptr;
  require(avformat_alloc_output_context2(&raw_format, nullptr, "mp4",
                                         nullptr) >= 0 &&
              raw_format != nullptr,
          "failed to allocate MP4 test output");
  std::unique_ptr<AVFormatContext, AvFormatContextDeleter> format(raw_format);

  const AVCodec *const codec = avcodec_find_encoder(AV_CODEC_ID_MPEG4);
  require(codec != nullptr, "MPEG-4 test encoder is unavailable");
  AVStream *const stream = avformat_new_stream(format.get(), nullptr);
  require(stream != nullptr, "failed to allocate MP4 test stream");

  std::unique_ptr<AVCodecContext, AvCodecContextDeleter> encoder(
      avcodec_alloc_context3(codec));
  require(encoder != nullptr, "failed to allocate video encoder context");
  encoder->codec_id = codec->id;
  encoder->codec_type = AVMEDIA_TYPE_VIDEO;
  encoder->pix_fmt = AV_PIX_FMT_YUV420P;
  encoder->width = 2;
  encoder->height = 2;
  encoder->time_base = AVRational{.num = 1, .den = 10};
  encoder->framerate = AVRational{.num = 10, .den = 1};
  encoder->gop_size = 3;
  encoder->max_b_frames = 0;
  if ((format->oformat->flags & AVFMT_GLOBALHEADER) != 0) {
    encoder->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
  }
  require(avcodec_open2(encoder.get(), codec, nullptr) >= 0,
          "failed to open MPEG-4 test encoder");

  stream->time_base = encoder->time_base;
  require(avcodec_parameters_from_context(stream->codecpar, encoder.get()) >= 0,
          "failed to copy video encoder parameters");
  require(avio_open_dyn_buf(&format->pb) >= 0,
          "failed to open in-memory MP4 output");
  require(avformat_write_header(format.get(), nullptr) >= 0,
          "failed to write MP4 test header");

  std::unique_ptr<AVFrame, AvFrameDeleter> frame(av_frame_alloc());
  std::unique_ptr<AVPacket, AvPacketDeleter> packet(av_packet_alloc());
  require(frame != nullptr && packet != nullptr,
          "failed to allocate video encoding buffers");
  frame->format = encoder->pix_fmt;
  frame->width = encoder->width;
  frame->height = encoder->height;
  require(av_frame_get_buffer(frame.get(), 0) >= 0,
          "failed to allocate video test frame pixels");

  constexpr std::array<std::array<Byte, 3U>, 3U> kYuvFrames = {
      std::array<Byte, 3U>{76U, 84U, 255U},
      std::array<Byte, 3U>{149U, 43U, 21U},
      std::array<Byte, 3U>{29U, 255U, 107U},
  };
  for (std::size_t index = 0U; index < kYuvFrames.size(); ++index) {
    require(av_frame_make_writable(frame.get()) >= 0,
            "failed to make a video test frame writable");
    for (int row = 0; row < encoder->height; ++row) {
      const std::ptrdiff_t row_offset =
          static_cast<std::ptrdiff_t>(row) * frame->linesize[0];
      std::fill_n(frame->data[0] + row_offset, encoder->width,
                  kYuvFrames[index][0]);
    }
    frame->data[1][0] = kYuvFrames[index][1];
    frame->data[2][0] = kYuvFrames[index][2];
    frame->pts = static_cast<std::int64_t>(index);
    require(avcodec_send_frame(encoder.get(), frame.get()) >= 0,
            "failed to send a video test frame to the encoder");
    write_video_packets(format.get(), encoder.get(), stream, packet.get());
  }
  require(avcodec_send_frame(encoder.get(), nullptr) >= 0,
          "failed to flush the video test encoder");
  write_video_packets(format.get(), encoder.get(), stream, packet.get());
  require(av_write_trailer(format.get()) >= 0,
          "failed to write MP4 test trailer");

  Byte *encoded = nullptr;
  const int encoded_size = avio_close_dyn_buf(format->pb, &encoded);
  format->pb = nullptr;
  std::unique_ptr<Byte, decltype(&av_free)> owned_encoded(encoded, av_free);
  require(encoded_size > 0 && owned_encoded != nullptr,
          "failed to finalize MP4 test source");
  return {owned_encoded.get(), owned_encoded.get() + encoded_size};
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
                                     .exif_json = std::nullopt,
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

void test_static_encoding_preferences() {
  const std::vector<Byte> source = encode_two_color_source();
  const sticker::StickerResult fast =
      sticker::transcode(source, sticker::TranscodeOptions{
                                     .trim = false,
                                     .exif_json = std::nullopt,
                                 });
  const sticker::StickerResult lossless =
      sticker::transcode(source, sticker::TranscodeOptions{
                                     .trim = false,
                                     .prefer_lossless = true,
                                     .exif_json = std::nullopt,
                                 });

  WebPBitstreamFeatures fast_features{};
  WebPBitstreamFeatures lossless_features{};
  require(WebPGetFeatures(fast.bytes.data(), fast.bytes.size(),
                          &fast_features) == VP8_STATUS_OK,
          "unable to inspect fast static output");
  require(WebPGetFeatures(lossless.bytes.data(), lossless.bytes.size(),
                          &lossless_features) == VP8_STATUS_OK,
          "unable to inspect lossless static output");
  require(fast_features.format == 1,
          "default static output did not use lossy WebP encoding");
  require(lossless_features.format == 2,
          "lossless static preference did not use lossless WebP encoding");
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

void test_video_transcode() {
  const std::vector<Byte> source = encode_video_source();
  const sticker::StickerResult result =
      sticker::transcode(source, sticker::TranscodeOptions{
                                     .trim = false,
                                     .exif_json = std::nullopt,
                                 });
  require(result.animated, "video source did not produce animated output");
  require(result.width == 512U && result.height == 512U,
          "video output dimensions are not 512 by 512");
  require(result.frame_count > 1U, "video output has too few frames");
  require(result.duration_ms == 200U,
          "video output had " + std::to_string(result.frame_count) +
              " frames and duration " + std::to_string(result.duration_ms) +
              " ms instead of two frames and 200 ms");
  require(result.bytes.size() <= static_cast<std::size_t>(500U) * 1024U,
          "video output exceeds the WhatsApp size limit");
}

void test_edge_pixels_are_clamped_before_interpolation() {
  const std::vector<Byte> source = encode_two_color_source();
  const sticker::StickerResult result =
      sticker::transcode(source, sticker::TranscodeOptions{
                                     .trim = false,
                                     .exif_json = std::nullopt,
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

void test_exif_metadata_is_muxed() {
  constexpr std::string_view kMetadata =
      R"({"sticker-pack-id":"test-pack","sticker-pack-name":"Test Pack","emojis":["😀"]})";
  const sticker::StickerResult result = sticker::transcode(
      encode_static_source(),
      sticker::TranscodeOptions{
          .exif_json = std::vector<Byte>(kMetadata.begin(), kMetadata.end()),
      });

  const WebPData data = {
      .bytes = result.bytes.data(),
      .size = result.bytes.size(),
  };
  std::unique_ptr<WebPMux, decltype(&WebPMuxDelete)> mux(
      WebPMuxCreate(&data, 0), &WebPMuxDelete);
  require(mux != nullptr, "EXIF output could not be read by WebPMux");

  WebPData exif{};
  WebPDataInit(&exif);
  require(WebPMuxGetChunk(mux.get(), "EXIF", &exif) == WEBP_MUX_OK,
          "EXIF output is missing its metadata chunk");
  require(exif.size == 22U + kMetadata.size(),
          "EXIF output has an unexpected metadata length");
  require(exif.bytes[0U] == 0x49U && exif.bytes[1U] == 0x49U &&
              exif.bytes[10U] == 0x41U && exif.bytes[11U] == 0x57U,
          "EXIF output does not use WhatsApp's TIFF prefix");
  const std::uint32_t json_length =
      static_cast<std::uint32_t>(exif.bytes[14U]) |
      (static_cast<std::uint32_t>(exif.bytes[15U]) << 8U) |
      (static_cast<std::uint32_t>(exif.bytes[16U]) << 16U) |
      (static_cast<std::uint32_t>(exif.bytes[17U]) << 24U);
  require(json_length == kMetadata.size(),
          "EXIF output stores an incorrect JSON payload length");
  const std::string actual_metadata(
      reinterpret_cast<const char *>(exif.bytes + 22U), kMetadata.size());
  require(actual_metadata == kMetadata,
          "EXIF output stores an unexpected JSON payload: " + actual_metadata);
}

void test_exif_metadata_can_extend_the_image_budget() {
  constexpr std::size_t kStaticImageBudget =
      static_cast<std::size_t>(100U) * 1024U;
  constexpr std::size_t kMaximumExifMuxOverhead = 128U;
  std::string metadata =
      R"({"sticker-pack-id":"test-pack","sticker-pack-name":")";
  metadata.append(3900U, 'x');
  metadata += R"(","emojis":[]})";
  require(metadata.size() <= static_cast<std::size_t>(4U) * 1024U,
          "EXIF boundary metadata exceeds the supported limit");

  const sticker::StickerResult result = sticker::transcode(
      encode_exif_boundary_source(),
      sticker::TranscodeOptions{
          .trim = false,
          .exif_json = std::vector<Byte>(metadata.begin(), metadata.end()),
      });
  require(result.bytes.size() > kStaticImageBudget,
          "EXIF metadata did not extend the static image byte budget");
  require(result.bytes.size() <=
              kStaticImageBudget + metadata.size() + kMaximumExifMuxOverhead,
          "EXIF output exceeds its bounded byte budget");
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
    constexpr std::array<TestCase, 11U> kTestCases = {
        TestCase{"static", test_static_transcode},
        TestCase{"jpeg", test_jpeg_transcode},
        TestCase{"encoding-preferences", test_static_encoding_preferences},
        TestCase{"malformed-jpeg", test_malformed_jpeg_is_rejected},
        TestCase{"animated", test_animated_transcode},
        TestCase{"video", test_video_transcode},
        TestCase{"interpolation",
                 test_edge_pixels_are_clamped_before_interpolation},
        TestCase{"exif", test_exif_metadata_is_muxed},
        TestCase{"exif-byte-budget",
                 test_exif_metadata_can_extend_the_image_budget},
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
