#ifndef ZLMEDIAKIT_AUDIOTRACKMUXER_H
#define ZLMEDIAKIT_AUDIOTRACKMUXER_H

#include "Extension/Track.h" // AudioTrackImp 定义在这里
#include "Util/RingBuffer.h" // 包含RingBuffer
#include "Rtsp/RtpCodec.h"   // 包含RtpPacket
#include "Util/List.h"
#include <mutex>       

#ifdef ENABLE_FFMPEG
// 【关键】使用前向声明，而不是包含完整的 "Transcode.h"
// 这可以避免头文件循环依赖，是C++的最佳实践
namespace mediakit {
    class Transcode;
}
// 添加 WorkThreadPool 头文件包含
#include "Thread/WorkThreadPool.h"
#endif

namespace mediakit {

/**
 * 一个特殊的音频轨道，作为装饰器存在。
 * 它接收一个原始的AAC轨道，并通过FFmpeg将其转码为Opus。
 * 对外，它把自己伪装成一个原生的Opus轨道。
 */
class AudioTrackMuxer : public AudioTrackImp, public std::enable_shared_from_this<AudioTrackMuxer> {
public:
    using Ptr = std::shared_ptr<AudioTrackMuxer>;
    using RingBufferType = toolkit::RingBuffer<std::shared_ptr<toolkit::List<RtpPacket::Ptr>>>;

    /**
     * 构造函数
     * @param origin_track 原始的AAC音频轨道
     */
    AudioTrackMuxer(const AudioTrack::Ptr &origin_track);
    ~AudioTrackMuxer() override = default;

    /**
     * 输入帧。这个方法将被原始AAC轨道的addDelegate机制自动调用。
     * @param frame 原始的AAC帧
     * @return bool
     */
    bool inputFrame(const Frame::Ptr &frame) override;

    /**
     * 克隆本轨道。
     * @return Track::Ptr
     */
    Track::Ptr clone() const override;

    // 【关键】: 提供访问其内部RingBuffer的方法
    const RingBufferType::Ptr& getRing() const;

private:
    AudioTrack::Ptr _origin_track;
    RingBufferType::Ptr _ring; // 【关键】: 拥有自己的RingBuffer
#ifdef ENABLE_FFMPEG
    // 编译器在这里只需要知道 Transcode 是一个类型就足够了，
    // 因为我们只声明了一个智能指针，并没有访问它的任何成员
    std::shared_ptr<Transcode> _transcode;
    RtpCodec::Ptr _rtp_encoder; // 用于将Opus Frame打包成RTP
    // 添加转码线程池成员变量
    toolkit::EventPoller::Ptr _transcode_poller;

    // 一个线程安全的队列作为缓冲区
    toolkit::List<Frame::Ptr> _frame_queue;
    std::mutex _queue_mutex;
#endif
};

} // namespace mediakit
#endif // ZLMEDIAKIT_AUDIOTRACKMUXER_H