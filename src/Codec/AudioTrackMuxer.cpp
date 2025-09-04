#include "AudioTrackMuxer.h"
#include "Rtsp/Rtsp.h"

// 不直接包含Opus.h，而是直接声明我们需要的函数
namespace mediakit {
    Sdp::Ptr getSdpFromOpusTrack(uint8_t payload_type, int sample_rate, int channels);
}

namespace mediakit {

AudioTrackMuxer::AudioTrackMuxer(const AudioTrack::Ptr &origin_track) :
AudioTrack(), // 调用基类构造函数
_origin_track(origin_track)
{
#ifdef ENABLE_FFMPEG
_transcode = std::make_shared<Transcode>();
// 直接使用Transcode的open(Track, CodecId, ...)重载，更简洁
if (!_transcode->open(origin_track, CodecOpus, 48000, origin_track->getAudioChannel())) {
WarnL << "Failed to open AAC to Opus transcoder";
_transcode = nullptr;
}
#endif
}

// 这个方法由原始AAC轨道的 `addDelegate` 机制自动调用
bool AudioTrackMuxer::inputFrame(const Frame::Ptr &frame) {
#ifdef ENABLE_FFMPEG
if (_transcode) {
// 1. 输入AAC帧进行转码，得到Opus帧
auto opus_frame = _transcode->inputFrame(frame);
if (opus_frame) {
// 2. 【关键】调用基类的inputFrame，将Opus帧分发给自己的RtpEncoder
// RtpEncoder会自动打包成RTP，并放入RingBuffer
AudioTrack::inputFrame(opus_frame);
}
}
#endif
return true; // 总是返回true，表示我们处理了这一帧
}

// 以下函数用于对外声明自己是Opus轨道
CodecId AudioTrackMuxer::getCodecId() const {
return CodecOpus;
}

int AudioTrackMuxer::getAudioSampleRate() const {
return 48000;
}

int AudioTrackMuxer::getAudioChannel() const {
return _origin_track->getAudioChannel();
}

int AudioTrackMuxer::getAudioSampleBit() const {
return 16;
}

bool AudioTrackMuxer::ready() const {
return true;
}

Sdp::Ptr AudioTrackMuxer::getSdp(uint8_t payload_type) const {
// 使用DefaultSdp类来生成SDP
return std::make_shared<DefaultSdp>(payload_type, *this);
}

} // namespace mediakit