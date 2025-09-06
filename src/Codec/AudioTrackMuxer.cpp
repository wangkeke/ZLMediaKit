#include "AudioTrackMuxer.h"
// 不再需要包含 Opus.h 或 Factory.h
#include "Codec/Transcode.h" // 只需要包含这个

namespace mediakit {

AudioTrackMuxer::AudioTrackMuxer(const AudioTrack::Ptr &origin_track) :
    AudioTrackImp(CodecOpus, 48000, origin_track->getAudioChannel(), 16),
    _origin_track(origin_track)
{
    // 创建并打开我们新的Transcode类
    _transcode = std::make_shared<Transcode>();
    if (_transcode->open(origin_track, CodecOpus, 48000, getAudioChannel())) {
        InfoL << ">>>>>>>>>>>>>>>>>>>>>Successfully opened AAC to Opus transcoder via Transcode class";
        // 【修正1, 2, 3】: 将转码结果通过回调送入本轨道
        _transcode->setOnFrame([this](const Frame::Ptr &opus_frame){
            if (opus_frame) {
                AudioTrack::inputFrame(opus_frame);
            }
        });
    }else {
        WarnL << ">>>>>>>>>>>>>>>>>>>>>Failed to open AAC to Opus transcoder via Transcode class";
        _transcode = nullptr;
    }
}

bool AudioTrackMuxer::inputFrame(const Frame::Ptr &frame) {
    if (_transcode) {
        _transcode->inputFrame(frame);
    } else {
        // 【新增】这是一个重要的检查点
        WarnL << ">>>>>>>>>> 探针 B-ERROR: _transcode is nullptr in AudioTrackMuxer, cannot process frame!";
    }
    return true;
}

Track::Ptr AudioTrackMuxer::clone() const {
    // 【修正1】: 使用 std::dynamic_pointer_cast 进行安全的类型转换
    auto cloned_origin = std::dynamic_pointer_cast<AudioTrack>(_origin_track->clone());
    if (cloned_origin) {
        return std::make_shared<AudioTrackMuxer>(cloned_origin);
    }
    // 如果克隆失败，返回nullptr或抛出异常
    return nullptr;
}

} // namespace mediakit