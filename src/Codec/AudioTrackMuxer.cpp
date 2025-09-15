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
    _origin_track(origin_track)
{
#ifdef ENABLE_FFMPEG
    // 创建并打开我们新的Transcode类
    // InfoL << ">>>>>>>>>>>>>>>>>>>>>ENABLE_FFMPEG = true, AAC to Opus transcoder via Transcode class";
    // 1. 创建RingBuffer
    _ring = std::make_shared<RingBufferType>();
    // 创建一个用于RTP编码器的中间RingBuffer
    auto rtp_ring = std::make_shared<toolkit::RingBuffer<RtpPacket::Ptr>>();
    
    // 2. 创建Opus的RTP打包器
    _rtp_encoder = Factory::getRtpEncoderByCodecId(CodecOpus, 111);  // 使用WebRTC标准的PT 111
    if (!_rtp_encoder) {
        WarnL << ">>>>>>>>>>>>>>>>>Failed to create Opus RTP encoder.";
        return;
    }
    
    // 设置RTP编码器的参数
    GET_CONFIG(uint32_t, audio_mtu, Rtp::kAudioMtuSize);
    // 使用更可靠的SSRC生成方式，避免重复
    uint32_t ssrc = (uint32_t)(std::chrono::high_resolution_clock::now().time_since_epoch().count() & 0xFFFFFFFF);
    // 确保SSRC不为0
    if (ssrc == 0) {
        ssrc = 0x12345678;
    }
    _rtp_encoder->setRtpInfo(ssrc, audio_mtu, 48000, 111);  // 确保使用PT 111
    
    // 3. 将RTP打包器的输出定向到中间RingBuffer，并通过代理将单个RTP包聚合成列表
    rtp_ring->setDelegate(std::make_shared<RtpListDelegate>(_ring));
    _rtp_encoder->setRtpRing(rtp_ring);
    
    // 确保总是获取同一个线程，保证任务顺序执行
    _transcode_poller = toolkit::WorkThreadPool::Instance().getPoller();
    
    _transcode = std::make_shared<Transcode>();
    if (_transcode->open(origin_track, CodecOpus, 48000, getAudioChannel())) {
        // 将转码结果通过回调送入本轨道
        _transcode->setOnFrame([this](const Frame::Ptr &opus_frame){
            // 【探针 D】: 确认转码后的Opus帧已到达Muxer
            if (opus_frame) {
                // InfoL << ">>>>>>>>>> 探针 D: Muxer收到Opus帧, size=" << opus_frame->size()
                //     << ", dts=" << opus_frame->dts() << ", pts=" << opus_frame->pts();
            } else {
                WarnL << ">>>>>>>>>> 探针 D-ERROR: Muxer收到了空的Opus帧!";
                return;
            }
            
            // 使用实际的转码帧
            if (opus_frame && _rtp_encoder) {
                // 添加调试日志
                // InfoL << ">>>>>>>>>>>>>>>>>Sending Opus frame to RTP encoder, size: " << opus_frame->size() 
                //       << ", dts: " << opus_frame->dts() << ", pts: " << opus_frame->pts();
                // 这个inputFrame会将Opus Frame打包成RTP，并通过rtp_ring最终写入_ring
                _rtp_encoder->inputFrame(opus_frame);
            } else if (!opus_frame) {
                WarnL << ">>>>>>>>>>>>>>>>>Received null Opus frame from transcoder";
            } else if (!_rtp_encoder) {
                WarnL << ">>>>>>>>>>>>>>>>>RTP encoder is null";
            }


            // 【新增】当一个任务完成后，检查队列中是否还有下一个任务
            Frame::Ptr next_frame;
            {
                std::lock_guard<std::mutex> lock(_queue_mutex);
                if (!_frame_queue.empty()) {
                    next_frame = _frame_queue.front();
                    _frame_queue.pop_front();
                }
            }

            if (next_frame) {
                // 如果有，立即在同一个后台线程上开始处理下一个
                _transcode->inputFrame(next_frame);
            }


        });
    }else {
        WarnL << ">>>>>>>>>>>>>>>>>>>>>Failed to open AAC to Opus transcoder via Transcode class";
        _transcode = nullptr;
    }
#endif
}

bool AudioTrackMuxer::inputFrame(const Frame::Ptr &frame) {
#ifdef ENABLE_FFMPEG
    if (_transcode) {
        
        bool should_start_task = false;
        {
            std::lock_guard<std::mutex> lock(_queue_mutex);
            // 只有当队列为空时，我们才需要启动一个新的异步任务
            // 如果队列不为空，说明后台线程已经在忙了，我们只需把帧放进队列即可
            should_start_task = _frame_queue.empty();
            _frame_queue.push_back(Frame::getCacheAbleFrame(frame));
        }
        
        if (should_start_task) {
            auto strong_transcode = _transcode;
            Frame::Ptr first_frame;
            {
                std::lock_guard<std::mutex> lock(_queue_mutex);
                if (!_frame_queue.empty()) {
                    first_frame = _frame_queue.front();
                    _frame_queue.pop_front();
                }
            }

            if (first_frame) {
                _transcode_poller->async([strong_transcode, first_frame]() {
                    strong_transcode->inputFrame(first_frame);
                });
            }
        }
        
    } else {
        // 这是一个重要的检查点
        WarnL << ">>>>>>>>>> 探针 B-ERROR: _transcode is nullptr in AudioTrackMuxer, cannot process frame!";
    }
#endif
    return true;
}

Track::Ptr AudioTrackMuxer::clone() const {
    // 使用 std::dynamic_pointer_cast 进行安全的类型转换
    auto cloned_origin = std::dynamic_pointer_cast<AudioTrack>(_origin_track->clone());
    if (cloned_origin) {
        return std::make_shared<AudioTrackMuxer>(cloned_origin);
    }
    // 如果克隆失败，返回nullptr
    return nullptr;
}

// 提供访问其内部RingBuffer的方法
const AudioTrackMuxer::RingBufferType::Ptr& AudioTrackMuxer::getRing() const {
    return _ring;
}

} // namespace mediakit