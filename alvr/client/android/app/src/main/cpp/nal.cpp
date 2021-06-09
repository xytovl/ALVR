/// H.264 NAL Parser functions
// Extract NAL Units from packet by UDP/SRT socket.
////////////////////////////////////////////////////////////////////

#include <string>
#include <stdlib.h>
#include <android/log.h>
#include <pthread.h>
#include "nal.h"
#include "packet_types.h"

static const std::byte NAL_TYPE_SPS = static_cast<const std::byte>(7);

static const std::byte H265_NAL_TYPE_VPS = static_cast<const std::byte>(32);

static const std::byte H264_NAL_TYPE_FILLER = static_cast<const std::byte>(12);


NALParser::NALParser(JNIEnv *env, jobject udpManager, jclass nalClass, bool enableFEC)
    : m_enableFEC(enableFEC)
{
    LOGE("NALParser initialized %p", this);

    m_env = env;
    mUdpManager = env->NewGlobalRef(udpManager);

    NAL_length = env->GetFieldID(nalClass, "length", "I");
    NAL_frameIndex = env->GetFieldID(nalClass, "frameIndex", "J");
    NAL_buf = env->GetFieldID(nalClass, "buf", "[B");

    jclass activityClass = env->GetObjectClass(udpManager);
    mObtainNALMethodID = env->GetMethodID(activityClass, "obtainNAL",
                                          "(I)Lcom/polygraphene/alvr/NAL;");
    mPushNALMethodID = env->GetMethodID(activityClass, "pushNAL",
                                        "(Lcom/polygraphene/alvr/NAL;)V");
}

NALParser::~NALParser()
{
    m_env->DeleteGlobalRef(mUdpManager);
}

void NALParser::setCodec(int codec)
{
    m_codec = codec;
}

namespace
{

const std::byte* find_nal_end(const std::byte* current, const std::byte* end, int count = 1)
{
  // skip self header
  if (end - current < 3)
  {
    return end;
  }
  current += 3;

  // search for 001
  int zeroes = 0;
  for (; current != end; ++current)
  {
    if (*current == std::byte(0))
    {
      zeroes += 1;
    } else {
      if (zeroes >= 2 and *current == std::byte(1))
      {
        if (--count == 0)
        {
          return current - 3;
        }
      }
      zeroes = 0;
    }
  }
  // we reach the end of the stream and only need one, that's ok
  if (count == 1)
    return current;
  return nullptr;
}

std::byte nal_type(const std::byte* nal, int codec)
{
  switch (codec)
  {
    case ALVR_CODEC_H264:
      return nal[4] & std::byte(0x1F);
    case ALVR_CODEC_H265:
      return (nal[4] >> 1) & std::byte(0x3F);
  }
  assert(false);
}
}

bool NALParser::processPacket(VideoFrame *packet, int packetSize, bool &fecFailure)
{
    if (m_enableFEC) {
        m_queue.addVideoPacket(packet, packetSize, fecFailure);
    }

    bool result = m_queue.reconstruct();
    if (result)
    {
        const std::byte *frameBuffer, *end;
        int frameByteSize;
        if (m_enableFEC) {
            // Reconstructed
            frameBuffer = m_queue.getFrameBuffer();
            end = frameBuffer + m_queue.getFrameByteSize();
        } else {
            frameBuffer = reinterpret_cast<const std::byte *>(packet) + sizeof(VideoFrame);
            end = frameBuffer + packetSize - sizeof(VideoFrame);
        }

        std::byte NALType = nal_type(frameBuffer, m_codec);

        if ((m_codec == ALVR_CODEC_H264 && NALType == NAL_TYPE_SPS) ||
            (m_codec == ALVR_CODEC_H265 && NALType == H265_NAL_TYPE_VPS))
        {
            // This frame contains (VPS + )SPS + PPS + IDR on NVENC H.264 (H.265) stream.
            // (VPS + )SPS + PPS has short size (8bytes + 28bytes in some environment), so we can assume SPS + PPS is contained in first fragment.

            const int num_nal = m_codec == ALVR_CODEC_H265 ? 3 : 2;
            const std::byte *vpssps_end = find_nal_end(frameBuffer, end, num_nal);
            if (vpssps_end == nullptr)
            {
                // Invalid frame.
                LOG("Got invalid frame. Too large SPS or PPS?");
                return false;
            }
            LOGI("Got frame=%d %d, Codec=%d", (std::int32_t) NALType, int(vpssps_end - frameBuffer), m_codec);
            push(frameBuffer, vpssps_end, packet->trackingFrameIndex);
            frameBuffer = vpssps_end;

            m_queue.clearFecFailure();
        }
        for (int nal_counter = 1; frameBuffer != end; ++nal_counter)
        {
          const std::byte* nal_end = find_nal_end(frameBuffer, end);
          NALType = nal_type(frameBuffer, m_codec);
          if (
              (m_codec == ALVR_CODEC_H264 and NALType != H264_NAL_TYPE_FILLER) // avoid filler data
              or (m_codec == ALVR_CODEC_H265 and int(NALType) < 35)) // avoid filler, aud etc.
          {
            push(frameBuffer, nal_end, packet->trackingFrameIndex);
          }
          frameBuffer = nal_end;
        }
        return true;
    }
    return false;
}

void NALParser::push(const std::byte *buffer, const std::byte *end, uint64_t frameIndex)
{
    jobject nal;
    jbyteArray buf;

    nal = m_env->CallObjectMethod(mUdpManager, mObtainNALMethodID, static_cast<jint>(end - buffer));
    if (nal == nullptr)
    {
        LOGE("NAL Queue is full.");
        return;
    }

    m_env->SetIntField(nal, NAL_length, end - buffer);
    m_env->SetLongField(nal, NAL_frameIndex, frameIndex);

    buf = (jbyteArray) m_env->GetObjectField(nal, NAL_buf);
    std::byte *cbuf = (std::byte *) m_env->GetByteArrayElements(buf, NULL);

    std::copy(buffer, end, cbuf);
    m_env->ReleaseByteArrayElements(buf, (jbyte *) cbuf, 0);
    m_env->DeleteLocalRef(buf);

    m_env->CallVoidMethod(mUdpManager, mPushNALMethodID, nal);

    m_env->DeleteLocalRef(nal);
}

bool NALParser::fecFailure()
{
    return m_queue.fecFailure();
}
