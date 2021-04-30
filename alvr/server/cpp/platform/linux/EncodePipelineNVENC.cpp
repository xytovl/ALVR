#include "EncodePipelineNVENC.h"
#include "ALVR-common/packet_types.h"
#include "ffmpeg_helper.h"
#include "alvr_server/Settings.h"
#include <chrono>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
}

namespace
{

const char * encoder(ALVR_CODEC codec)
{
  switch (codec)
  {
    case ALVR_CODEC_H264:
      return "h264_vaapi";
    case ALVR_CODEC_H265:
      return "hevc_vaapi";
  }
  throw std::runtime_error("invalid codec " + std::to_string(codec));
}

void set_hwframe_ctx(AVCodecContext *ctx, AVBufferRef *hw_device_ctx)
{
  AVBufferRef *hw_frames_ref;
  AVHWFramesContext *frames_ctx = NULL;
  int err = 0;

  if (!(hw_frames_ref = av_hwframe_ctx_alloc(hw_device_ctx))) {
    throw std::runtime_error("Failed to create NVENC frame context.");
  }
  frames_ctx = (AVHWFramesContext *)(hw_frames_ref->data);
  frames_ctx->format = AV_PIX_FMT_CUDA;
  frames_ctx->sw_format = AV_PIX_FMT_NV12;
  frames_ctx->width = ctx->width;
  frames_ctx->height = ctx->height;
  frames_ctx->initial_pool_size = 3;
  if ((err = av_hwframe_ctx_init(hw_frames_ref)) < 0) {
    av_buffer_unref(&hw_frames_ref);
    throw alvr::AvException("Failed to initialize NVENC frame context:", err);
  }
  ctx->hw_frames_ctx = av_buffer_ref(hw_frames_ref);
  if (!ctx->hw_frames_ctx)
    err = AVERROR(ENOMEM);

  av_buffer_unref(&hw_frames_ref);
}

AVFrame * make_transferred_frame(AVFrame *vk_frame, AVBufferRef *hw_device_ctx)
{
  AVBufferRef *hw_frames_ref;
  AVHWFramesContext *frames_ctx = NULL;
  AVHWFramesContext *vk_frames_ctx = (AVHWFramesContext *)vk_frame->hw_frames_ctx;
  int err = 0;

  if (!(hw_frames_ref = av_hwframe_ctx_alloc(hw_device_ctx))) {
    throw std::runtime_error("Failed to create NVENC frame context.");
  }
  frames_ctx = (AVHWFramesContext *)(hw_frames_ref->data);
  frames_ctx->format = AV_PIX_FMT_CUDA;
  switch (vk_frames_ctx->format)
  {
    case AV_PIX_FMT_ABGR:
      frames_ctx->sw_format = AV_PIX_FMT_0BGR;
      break;
    case AV_PIX_FMT_ARGB:
      frames_ctx->sw_format = AV_PIX_FMT_0RGB;
      break;
    default:
      frames_ctx->sw_format = vk_frames_ctx->sw_format;
  }
  frames_ctx->width = vk_frames_ctx->width;
  frames_ctx->height = vk_frames_ctx->height;
  frames_ctx->initial_pool_size = 1;
  if ((err = av_hwframe_ctx_init(hw_frames_ref)) < 0) {
    av_buffer_unref(&hw_frames_ref);
    throw alvr::AvException("Failed to initialize NVENC frame context:", err);
  }

  AVFrame * frame = av_frame_alloc();
  err = av_hwframe_get_buffer(hw_frames_ref, frame, 0);
  av_buffer_unref(&hw_frames_ref);
  if (err != 0)
    throw alvr::AvException("Failed to create CUDA frame:", err);

  return frame;
}

}

alvr::EncodePipelineNVENC::EncodePipelineNVENC(std::vector<VkFrame>& input_frames, VkFrameCtx& vk_frame_ctx)
{
  for (auto& input_frame: input_frames)
  {
    vk_frames.push_back(input_frame.make_av_frame(vk_frame_ctx).release());
  }

  int err = av_hwdevice_ctx_create(&hw_ctx, AV_HWDEVICE_TYPE_CUDA, NULL, NULL, 0);
  if (err < 0) {
    throw alvr::AvException("Failed to create a NVENC device:", err);
  }

  const auto& settings = Settings::Instance();

  auto codec_id = ALVR_CODEC(settings.m_codec);
  const char * encoder_name = encoder(codec_id);
  AVCodec *codec = avcodec_find_encoder_by_name(encoder_name);
  if (codec == nullptr)
  {
    throw std::runtime_error(std::string("Failed to find encoder ") + encoder_name);
  }

  encoder_ctx = avcodec_alloc_context3(codec);
  if (not encoder_ctx)
  {
    throw std::runtime_error("failed to allocate NVENC encoder");
  }

  switch (codec_id)
  {
    case ALVR_CODEC_H264:
      encoder_ctx->profile = FF_PROFILE_H264_MAIN;
      av_opt_set(encoder_ctx, "rc_mode", "2", 0); //CBR
      break;
    case ALVR_CODEC_H265:
      encoder_ctx->profile = FF_PROFILE_HEVC_MAIN;
      av_opt_set(encoder_ctx, "rc_mode", "2", 0);
      break;
  }

  encoder_ctx->width = settings.m_renderWidth;
  encoder_ctx->height = settings.m_renderHeight;
  encoder_ctx->time_base = {std::chrono::steady_clock::period::num, std::chrono::steady_clock::period::den};
  encoder_ctx->framerate = AVRational{settings.m_refreshRate, 1};
  encoder_ctx->sample_aspect_ratio = AVRational{1, 1};
  encoder_ctx->pix_fmt = AV_PIX_FMT_CUDA;
  encoder_ctx->max_b_frames = 0;
  encoder_ctx->bit_rate = settings.mEncodeBitrateMBs * 1024 * 1024;

  set_hwframe_ctx(encoder_ctx, hw_ctx);

  transferred_frame = make_transferred_frame(vk_frames[0], hw_ctx);

  err = avcodec_open2(encoder_ctx, codec, NULL);
  if (err < 0) {
    throw alvr::AvException("Cannot open video encoder codec:", err);
  }

}

alvr::EncodePipelineNVENC::~EncodePipelineNVENC()
{
  av_frame_free(&transferred_frame);
  for (auto frame: vk_frames)
  {
    av_frame_free(&frame);
  }
  avcodec_free_context(&encoder_ctx);
  av_buffer_unref(&hw_ctx);
}

void alvr::EncodePipelineNVENC::EncodeFrame(uint32_t frame_index, bool idr, std::vector<uint8_t>& out)
{
  assert(frame_index < vk_frames.size());
  int err = av_hwframe_transfer_data(transferred_frame, vk_frames[frame_index], 0);

  transferred_frame->pict_type = idr ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
  transferred_frame->pts = std::chrono::steady_clock::now().time_since_epoch().count();

  if ((err = avcodec_send_frame(encoder_ctx, transferred_frame)) < 0) {
    throw alvr::AvException("avcodec_send_frame failed: ", err);
  }

  out.clear();
  while (1) {
    AVPacket * enc_pkt = av_packet_alloc();

    err = avcodec_receive_packet(encoder_ctx, enc_pkt);
    if (err == AVERROR(EAGAIN)) {
      break;
    } else if (err) {
      throw std::runtime_error("failed to encode");
    }
    uint8_t *frame_data = enc_pkt->data;
    int frame_size = enc_pkt->size;
    out.insert(out.end(), frame_data, frame_data + frame_size);
    av_packet_free(&enc_pkt);
  }
}
