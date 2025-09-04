#ifndef ZLMEDIAKIT_AUDIOTRACKMUXER_H
#define ZLMEDIAKIT_AUDIOTRACKMUXER_H

#include "Extension/Track.h"
#ifdef ENABLE_FFMPEG
#include "Codec/Transcode.h"
#endif

namespace mediakit {

// 它就是一个Track，不需要继承MediaSinkInterface
class AudioTrackMuxer : public AudioTrack {
public:
using Ptr = std::shared_ptr<AudioTrackMuxer>;

// 构造时就初始化好一切
AudioTrackMuxer(const AudioTrack::Ptr &origin_track);
~AudioTrackMuxer() override = default;

// 【关键】这个inputFrame会被原始轨道的addDelegate机制调用
bool inputFrame(const Frame::Ptr &frame) override;

// 重写AudioTrack的接口，伪装成Opus轨道
CodecId getCodecId() const override;
int getAudioSampleRate() const override;
int getAudioChannel() const override;
int getAudioSampleBit() const override;
bool ready() const override;
Sdp::Ptr getSdp(uint8_t payload_type) const override;

private:
AudioTrack::Ptr _origin_track;
#ifdef ENABLE_FFMPEG
std::shared_ptr<Transcode> _transcode;
#endif
};

} // namespace mediakit
#endif // ZLMEDIAKIT_AUDIOTRACKMUXER_H