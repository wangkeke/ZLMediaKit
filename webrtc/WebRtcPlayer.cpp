/*
 * Copyright (c) 2016-present The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/ZLMediaKit/ZLMediaKit).
 *
 * Use of this source code is governed by MIT-like license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#include "WebRtcPlayer.h"

#include "Common/config.h"
#include "Extension/Factory.h"
#include "Util/base64.h"
#include "Common/MultiMediaSourceMuxer.h"

using namespace std;

namespace mediakit {

namespace Rtc {
#define RTC_FIELD "rtc."
const string kBfilter = RTC_FIELD "bfilter";
static onceToken token([]() { mINI::Instance()[kBfilter] = 0; });
} // namespace Rtc

H264BFrameFilter::H264BFrameFilter()
    : _last_seq(0)
    , _last_stamp(0)
    , _first_packet(true) {}

RtpPacket::Ptr H264BFrameFilter::processPacket(const RtpPacket::Ptr &packet) {
    if (!packet) {
        return nullptr;
    }

    if (isH264BFrame(packet)) {
        return nullptr;
    }

    auto cur_stamp = packet->getStamp();
    auto cur_seq = packet->getSeq();

    if (_first_packet) {
        _first_packet = false;
        _last_seq = cur_seq;
        _last_stamp = cur_stamp;
    }

    // 处理时间戳连续性问题
    if (cur_stamp < _last_stamp) {
        return nullptr;
    }
    _last_stamp = cur_stamp;

    // 处理 seq 连续性问题
    if (cur_seq > _last_seq + 4) {
        RtpHeader *header = packet->getHeader();
        _last_seq = (_last_seq + 1) & 0xFFFF;
        header->seq = htons(_last_seq);
    }

    return packet;
}

bool H264BFrameFilter::isH264BFrame(const RtpPacket::Ptr &packet) const {
    uint8_t *payload = packet->getPayload();
    size_t payload_size = packet->getPayloadSize();

    if (payload_size < 1) {
        return false;
    }

    uint8_t nal_unit_type = payload[0] & 0x1F;
    switch (nal_unit_type) {
        case 24: // STAP-A
            return handleStapA(payload, payload_size);
        case 28: // FU-A
            return handleFua(payload, payload_size);
        default:
            if (nal_unit_type < 24) {
                return isBFrameByNalType(nal_unit_type, payload + 1, payload_size - 1);
            }
            return false;
    }
}

bool H264BFrameFilter::handleStapA(const uint8_t *payload, size_t payload_size) const {
    size_t offset = 1;
    while (offset + 2 <= payload_size) {
        uint16_t nalu_size = (payload[offset] << 8) | payload[offset + 1];
        offset += 2;
        if (offset + nalu_size > payload_size || nalu_size < 1) {
            return false;
        }
        uint8_t original_nal_type = payload[offset] & 0x1F;
        if (original_nal_type < 24) {
            if (isBFrameByNalType(original_nal_type, payload + offset + 1, nalu_size - 1)) {
                return true;
            }
        }
        offset += nalu_size;
    }
    return false;
}

bool H264BFrameFilter::handleFua(const uint8_t *payload, size_t payload_size) const {
    if (payload_size < 2) {
        return false;
    }
    uint8_t fu_header = payload[1];
    uint8_t original_nal_type = fu_header & 0x1F;
    bool start_bit = fu_header & 0x80;
    if (start_bit) {
        return isBFrameByNalType(original_nal_type, payload + 2, payload_size - 2);
    }
    return false;
}

bool H264BFrameFilter::isBFrameByNalType(uint8_t nal_type, const uint8_t *data, size_t size) const {
    if (size < 1) {
        return false;
    }

    if (nal_type != NAL_NIDR && nal_type != NAL_PARTITION_A && nal_type != NAL_PARTITION_B && nal_type != NAL_PARTITION_C) {
        return false;
    }

    uint8_t slice_type = extractSliceType(data, size);
    return slice_type == H264SliceTypeB || slice_type == H264SliceTypeB1;
}

int H264BFrameFilter::decodeExpGolomb(const uint8_t *data, size_t size, size_t &bitPos) const {
    if (bitPos >= size * 8)
        return -1;

    int leadingZeroBits = 0;
    while (bitPos < size * 8 && !getBit(data, bitPos++)) {
        leadingZeroBits++;
    }

    int result = (1 << leadingZeroBits) - 1;
    for (int i = 0; i < leadingZeroBits; i++) {
        if (bitPos < size * 8) {
            result += getBit(data, bitPos++) << (leadingZeroBits - i - 1);
        }
    }

    return result;
}

int H264BFrameFilter::getBit(const uint8_t *data, size_t pos) const {
    size_t byteIndex = pos / 8;
    size_t bitOffset = pos % 8;
    uint8_t byte = data[byteIndex];
    return (byte >> (7 - bitOffset)) & 0x01;
}

uint8_t H264BFrameFilter::extractSliceType(const uint8_t *data, size_t size) const {
    size_t bitPos = 0;
    int first_mb_in_slice = decodeExpGolomb(data, size, bitPos);
    int slice_type = decodeExpGolomb(data, size, bitPos);

    if (slice_type >= 0 && slice_type <= 9) {
        return static_cast<uint8_t>(slice_type);
    }
    return -1;
}

WebRtcPlayer::Ptr WebRtcPlayer::create(const EventPoller::Ptr &poller, const RtspMediaSource::Ptr &src, const MediaInfo &info) {
    WebRtcPlayer::Ptr ret(new WebRtcPlayer(poller, src, info), [](WebRtcPlayer *ptr) {
        ptr->onDestory();
        delete ptr;
    });
    ret->onCreate();
    return ret;
}

WebRtcPlayer::WebRtcPlayer(const EventPoller::Ptr &poller, const RtspMediaSource::Ptr &src, const MediaInfo &info)
    : WebRtcTransportImp(poller) {
    _media_info = info;
    _play_src = src;
    CHECK(src);

    GET_CONFIG(bool, direct_proxy, Rtsp::kDirectProxy);
    _send_config_frames_once = direct_proxy;

    GET_CONFIG(bool, enable, Rtc::kBfilter);
    _bfliter_flag = enable;
    _is_h264 = false;
    _bfilter = std::make_shared<H264BFrameFilter>();
}

void WebRtcPlayer::onStartWebRTC() {
    auto playSrc = _play_src.lock();
    if (!playSrc) {
        onShutdown(SockException(Err_shutdown, "rtsp media source was shutdown"));
        return;
    }
    WebRtcTransportImp::onStartWebRTC();
    if (canSendRtp()) {
        playSrc->pause(false);

        // 检查是否有Opus轨道，如果有则从Opus轨道读取音频数据，从原始RTSP源读取视频数据
        auto muxer = playSrc->getMuxer();
        auto opus_track = muxer ? muxer->getOpusTrack() : nullptr;
        
        // 创建原始RTSP源的读取器来处理视频数据
        auto rtsp_reader = playSrc->getRing()->attach(getPoller(), true);
        
        if (opus_track) {
            // 创建Opus轨道的读取器来处理音频数据
            _audio_reader = opus_track->getRing()->attach(getPoller(), true);
            // InfoL << ">>>>>>>>>>>>>>>>>>WebRtcPlayer: Using Opus track ring buffer for audio data";
            
            // 设置Opus读取器的回调
            weak_ptr<WebRtcPlayer> weak_self = static_pointer_cast<WebRtcPlayer>(shared_from_this());
            weak_ptr<Session> weak_session = static_pointer_cast<Session>(getSession());
            
            _audio_reader->setGetInfoCB([weak_session]() {
                Any ret;
                ret.set(static_pointer_cast<Session>(weak_session.lock()));
                return ret;
            });
            
            _audio_reader->setReadCB([weak_self](const RtspMediaSource::RingDataType &pkt) {
                auto strong_self = weak_self.lock();
                if (!strong_self) {
                    return;
                }

                size_t i = 0;
                pkt->for_each([&](const RtpPacket::Ptr &rtp) {
                    // 只处理音频包
                    if (rtp->type == TrackAudio) {
                        // 【探针 F】: 确认WebRtcPlayer正在拉取Opus音频RTP包
                        // InfoL << ">>>>>>>>>> 探针 F: WebRtcPlayer拉取到Opus RTP包, seq=" << rtp->getSeq() 
                        //     << ", stamp=" << rtp->getStamp() << ", size=" << rtp->getPayloadSize();
                        strong_self->onSendRtp(rtp, ++i == pkt->size());
                    }
                });
            });
            
            _audio_reader->setDetachCB([weak_self]() {
                auto strong_self = weak_self.lock();
                if (!strong_self) {
                    return;
                }
                // 不直接关闭，因为还有rtsp_reader
                // InfoL << "Opus reader detached";
            });
        } else {
            // InfoL << ">>>>>>>>>>>>>>>>>>WebRtcPlayer: Using original RTSP source ring buffer for audio data";
        }

        // 设置RTSP读取器的回调来处理视频数据（以及在没有Opus轨道时处理音频数据）
        weak_ptr<WebRtcPlayer> weak_self = static_pointer_cast<WebRtcPlayer>(shared_from_this());
        weak_ptr<Session> weak_session = static_pointer_cast<Session>(getSession());
        
        rtsp_reader->setGetInfoCB([weak_session]() {
            Any ret;
            ret.set(static_pointer_cast<Session>(weak_session.lock()));
            return ret;
        });
        
        rtsp_reader->setReadCB([weak_self, opus_track](const RtspMediaSource::RingDataType &pkt) {
            auto strong_self = weak_self.lock();
            if (!strong_self) {
                return;
            }

            if (strong_self->_send_config_frames_once && !pkt->empty()) {
                const auto &first_rtp = pkt->front();
                strong_self->sendConfigFrames(first_rtp->getSeq(), first_rtp->sample_rate, first_rtp->getStamp(), first_rtp->ntp_stamp);
                strong_self->_send_config_frames_once = false;
            }

            size_t i = 0;
            pkt->for_each([&](const RtpPacket::Ptr &rtp) {
                // 如果是视频包，直接处理
                if (rtp->type == TrackVideo) {
                    // 添加视频帧检测日志
                    // InfoL << ">>>>>>>>>>>>>>>>>>>>>>>WebRtcPlayer: Receiving video RTP packet, seq=" << rtp->getSeq() 
                    //     << ", stamp=" << rtp->getStamp() << ", size=" << rtp->getPayloadSize();
                    
                    if (strong_self->_bfliter_flag) {
                        if (strong_self->_is_h264) {
                            auto rtp_filter = strong_self->_bfilter->processPacket(rtp);
                            if (rtp_filter) {
                                strong_self->onSendRtp(rtp_filter, ++i == pkt->size());
                            }
                        } else {
                            strong_self->onSendRtp(rtp, ++i == pkt->size());
                        }
                    } else {
                        strong_self->onSendRtp(rtp, ++i == pkt->size());
                    }
                } 
                // 如果没有Opus轨道且是音频包，则处理音频包
                else if (!opus_track && rtp->type == TrackAudio) {
                    // InfoL << ">>>>>>>>>>>>>>>>>>>>WebRtcPlayer: Receiving audio RTP packet from RTSP source, seq=" << rtp->getSeq() 
                    //     << ", stamp=" << rtp->getStamp() << ", size=" << rtp->getPayloadSize();
                    strong_self->onSendRtp(rtp, ++i == pkt->size());
                }
            });
        });
        
        rtsp_reader->setDetachCB([weak_self]() {
            auto strong_self = weak_self.lock();
            if (!strong_self) {
                return;
            }
            strong_self->onShutdown(SockException(Err_shutdown, "rtsp ring buffer detached"));
        });

        // 存储读取器引用以防止它们被销毁
        _reader = rtsp_reader;
    }
}
void WebRtcPlayer::onDestory() {
    auto duration = getDuration();
    auto bytes_usage = getBytesUsage();
    // 流量统计事件广播  [AUTO-TRANSLATED:6b0b1234]
    // Traffic statistics event broadcast
    GET_CONFIG(uint32_t, iFlowThreshold, General::kFlowThreshold);
    if (_reader && getSession()) {
        WarnL << "RTC播放器(" << _media_info.shortUrl() << ")结束播放,耗时(s):" << duration;
        if (bytes_usage >= iFlowThreshold * 1024) {
            NOTICE_EMIT(BroadcastFlowReportArgs, Broadcast::kBroadcastFlowReport, _media_info, bytes_usage, duration, true, *getSession());
        }
    }
    WebRtcTransportImp::onDestory();
}

void WebRtcPlayer::onRtcConfigure(RtcConfigure &configure) const {
    auto playSrc = _play_src.lock();
    if (!playSrc) {
        return;
    }
    WebRtcTransportImp::onRtcConfigure(configure);
    
    // 这是播放场景，所有轨道都应该是 sendonly
    configure.audio.direction = RtpDirection::sendonly;
    configure.video.direction = RtpDirection::sendonly;

    // 直接从 MediaSource 获取当前所有可用的、就绪的轨道
    // getTracks(true) 返回的轨道列表，包含了我们动态添加的 AudioTrackMuxer
    auto tracks = playSrc->getTracks(true);

    bool has_audio = false;
    bool has_video = false;

    // 获取MultiMediaSourceMuxer以访问Opus轨道
    auto muxer = playSrc->getMuxer();
    if (muxer) {
        // 检查是否有专门的Opus轨道
        auto opus_track = muxer->getOpusTrack();
        if (opus_track) {
            // 使用Opus轨道
            configure.audio.preferred_codec.clear();
            configure.audio.preferred_codec.emplace_back(CodecOpus);
            has_audio = true;
            // InfoL << ">>>>>>>>>>>>>>>>Using dedicated Opus track for WebRTC, codec=" << opus_track->getCodecName();
        } 
    }

    if(!has_audio){
        for (const auto &track : tracks) {
            if (track->getTrackType() == TrackAudio) {
                if (track->getCodecId() == CodecOpus) {
                    // 【优先选择Opus】
                    configure.audio.preferred_codec.clear();
                    configure.audio.preferred_codec.emplace_back(CodecOpus);
                    has_audio = true;
                    // InfoL << ">>>>>>>>>>>>>>>>Using Opus track from source tracks";
                    // 找到最优的Opus后，就不再关心其他音频轨道了
                    break; 
                }
            }
        }
    }
    
    // 再次遍历以处理视频和备用音频（如果需要）
    for (const auto &track : tracks) {
        if (track->getTrackType() == TrackVideo) {
            has_video = true;
            configure.video.preferred_codec.clear();
            configure.video.preferred_codec.emplace_back(track->getCodecId());
        } else if (track->getTrackType() == TrackAudio && configure.audio.preferred_codec.empty()) {
            // 如果上面没有找到Opus，这里可以配置一个备用的音频编码
            has_audio = true;
            configure.audio.preferred_codec.emplace_back(track->getCodecId());
            // InfoL << ">>>>>>>>>>>>>>>>Using fallback audio codec: " << track->getCodecName();
        }
    }

    // 如果遍历后发现根本没有音频或视频轨道，则将其设置为 inactive
    if (!has_audio) {
        configure.audio.direction = RtpDirection::inactive;
        // InfoL << ">>>>>>>>>>>>>>>>No audio track found, setting audio to inactive";
    }
    if (!has_video) {
        configure.video.direction = RtpDirection::inactive;
        // InfoL << ">>>>>>>>>>>>>>>>No video track found, setting video to inactive";
    }

    // 【关键】不再调用 configure.setPlayRtspInfo()，因为它只会读取旧的静态SDP。
    // 我们已经通过直接设置 preferred_codec 来完成了轨道选择。
}

void WebRtcPlayer::sendConfigFrames(uint32_t before_seq, uint32_t sample_rate, uint32_t timestamp, uint64_t ntp_timestamp) {
    auto play_src = _play_src.lock();
    if (!play_src) {
        return;
    }
    SdpParser parser(play_src->getSdp());
    auto video_sdp = parser.getTrack(TrackVideo);
    if (!video_sdp) {
        return;
    }
    auto video_track = dynamic_pointer_cast<VideoTrack>(Factory::getTrackBySdp(video_sdp));
    if (!video_track) {
        return;
    }
    _is_h264 = video_track->getCodecId() == CodecH264;
    auto frames = video_track->getConfigFrames();
    if (frames.empty()) {
        return;
    }
    auto encoder = mediakit::Factory::getRtpEncoderByCodecId(video_track->getCodecId(), 0);
    if (!encoder) {
        return;
    }

    GET_CONFIG(uint32_t, video_mtu, Rtp::kVideoMtuSize);
    encoder->setRtpInfo(0, video_mtu, sample_rate, 0, 0, 0);

    auto seq = before_seq - frames.size();
    for (const auto &frame : frames) {
        auto rtp = encoder->getRtpInfo().makeRtp(TrackVideo, frame->data() + frame->prefixSize(), frame->size() - frame->prefixSize(), false, 0);
        auto header = rtp->getHeader();
        header->seq = htons(seq++);
        header->stamp = htonl(timestamp);
        rtp->ntp_stamp = ntp_timestamp;
        onSendRtp(rtp, false);
    }
}

}// namespace mediakit