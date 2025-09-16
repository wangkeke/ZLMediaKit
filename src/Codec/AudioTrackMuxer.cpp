#include "AudioTrackMuxer.h"
#include "Codec/Transcode.h"
#include "Extension/Factory.h"
#include "Common/config.h"
#include "Rtsp/RtpCodec.h"


namespace mediakit {

class RtpListDelegate : public toolkit::RingDelegate<RtpPacket::Ptr> {
public:
    RtpListDelegate(const AudioTrackMuxer::RingBufferType::Ptr& target_ring) : _target_ring(target_ring) {}
    
    void onWrite(RtpPacket::Ptr in, bool is_key) override {
        // 【探针 E】: 确认Opus帧已成功打包成RTP并准备写入RingBuffer
        // InfoL << ">>>>>>>>>> 探针 E: RTP打包成功, seq=" << in->getSeq()
        //   << ", stamp=" << in->getStamp() << ", payload_size=" << in->getPayloadSize();
        auto rtp_list = std::make_shared<toolkit::List<RtpPacket::Ptr>>();
        rtp_list->emplace_back(std::move(in));
        _target_ring->write(std::move(rtp_list), is_key);
    }

private:
    AudioTrackMuxer::RingBufferType::Ptr _target_ring;
};

AudioTrackMuxer::AudioTrackMuxer(const AudioTrack::Ptr &origin_track) :
    AudioTrackImp(CodecOpus, 48000, origin_track->getAudioChannel(), 16),
    _origin_track_wptr(origin_track) {}


AudioTrackMuxer::AudioTrackMuxer(const AudioTrackMuxer &that) :
    AudioTrackImp(that),
    _origin_track_wptr(that._origin_track_wptr),
    _ring(that._ring)
{
#ifdef ENABLE_FFMPEG
    // 注意：我们不复制_transcode和_rtp_encoder，因为它们是重量级组件
    // 每个克隆实例应该有自己的转码器实例，但可以共享_ring
    _transcode = nullptr;
    _rtp_encoder = nullptr;
#endif
}

AudioTrackMuxer::~AudioTrackMuxer() {
    // 可以在这里添加日志，确认析构函数被调用
    InfoL << ">>>>>>>>>>>>>>>>>>>AudioTrackMuxer destroyed.";
}

void AudioTrackMuxer::init() {
#ifdef ENABLE_FFMPEG
    // 设置为0会禁用GOP缓存，使其作为一个普通的、有大小限制的环形缓冲区工作。
    _ring = std::make_shared<RingBufferType>(1024, nullptr, 0);
    auto rtp_ring = std::make_shared<toolkit::RingBuffer<RtpPacket::Ptr>>();
    _rtp_encoder = Factory::getRtpEncoderByCodecId(CodecOpus, 111);
    
    // 设置RTP编码器的参数
    GET_CONFIG(uint32_t, audio_mtu, Rtp::kAudioMtuSize);
    uint32_t ssrc = (uint32_t)(std::chrono::high_resolution_clock::now().time_since_epoch().count() & 0xFFFFFFFF);
    if (ssrc == 0) ssrc = 0x12345678;
    _rtp_encoder->setRtpInfo(ssrc, audio_mtu, 48000, 111);

    rtp_ring->setDelegate(std::make_shared<RtpListDelegate>(_ring));
    _rtp_encoder->setRtpRing(rtp_ring);
    
    _transcode = std::make_shared<Transcode>();
    if (auto strong_origin = _origin_track_wptr.lock()) {
        if (_transcode->open(strong_origin, CodecOpus, 48000, getAudioChannel())) {
            // 【修正】: 在这里安全地使用 shared_from_this() 来创建 weak_ptr
            std::weak_ptr<AudioTrackMuxer> weak_self = shared_from_this();
            _transcode->setOnFrame([weak_self](const Frame::Ptr &opus_frame){
                auto strong_self = weak_self.lock();
                if (!strong_self) return;
                
                if (opus_frame && strong_self->_rtp_encoder) {
                    strong_self->_rtp_encoder->inputFrame(opus_frame);
                }
            });
        } else { _transcode = nullptr; }
    } else { _transcode = nullptr; }
#endif
}

Track::Ptr AudioTrackMuxer::clone() const {
    // 使用轻量级克隆方式，避免创建重量级组件
    return std::make_shared<AudioTrackMuxer>(*this);
}

bool AudioTrackMuxer::inputFrame(const Frame::Ptr &frame) {
#ifdef ENABLE_FFMPEG
    if (_transcode) {
        // 添加调试日志
        // InfoL << ">>>>>>>>>> Sending AAC frame to transcoder, size: " << frame->size() 
        //       << ", dts: " << frame->dts() << ", pts: " << frame->pts();
        _transcode->inputFrame(frame);
    } else {
        // 这是一个重要的检查点
        WarnL << ">>>>>>>>>> 探针 B-ERROR: _transcode is nullptr in AudioTrackMuxer, cannot process frame!";
    }
#endif
    return true;
}

// 提供访问其内部RingBuffer的方法
const AudioTrackMuxer::RingBufferType::Ptr& AudioTrackMuxer::getRing() const {
    return _ring;
}

} // namespace mediakit