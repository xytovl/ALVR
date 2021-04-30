#pragma once

#include "EncodePipeline.h"

extern "C" struct AVBufferRef;
extern "C" struct AVCodecContext;
extern "C" struct AVFilterContext;
extern "C" struct AVFilterGraph;
extern "C" struct AVFrame;

namespace alvr
{

class EncodePipelineNVENC: public EncodePipeline
{
public:
  ~EncodePipelineNVENC();
  EncodePipelineNVENC(std::vector<VkFrame> &input_frames, VkFrameCtx& vk_frame_ctx);

  void EncodeFrame(uint32_t frame_index, bool idr, std::vector<uint8_t>& out) override;
private:
  AVBufferRef *hw_ctx = nullptr;
  AVCodecContext *encoder_ctx = nullptr;
  std::vector<AVFrame *> vk_frames;
  AVFrame * transferred_frame = nullptr;
};
}
