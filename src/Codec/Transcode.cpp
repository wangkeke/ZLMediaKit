/*
 * Copyright (c) 2016-present The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/ZLMediaKit/ZLMediaKit).
 *
 * Use of this source code is governed by MIT-like license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#if defined(ENABLE_FFMPEG)
#if !defined(_WIN32)
#include <dlfcn.h>
#endif
#include "Util/File.h"
#include "Util/uv_errno.h"
#include "Transcode.h"
#include "Common/config.h"
#include "Common/Stamp.h"  // 添加Stamp头文件包含
#include <fstream>
#include <deque>

#define MAX_DELAY_SECOND 3

using namespace std;
using namespace toolkit;

// 【修正5】: 添加必要的辅助函数
namespace mediakit {
// 将 ZLMediaKit::CodecId 转换为 FFmpeg 的 AVCodecID
static AVCodecID get_avcodec_id(CodecId codec) {
    switch (codec) {
        case CodecAAC: return AV_CODEC_ID_AAC;
        case CodecOpus: return AV_CODEC_ID_OPUS;
        // ...可以添加更多...
        default: return AV_CODEC_ID_NONE;
    }
}
// 将 FFmpeg 的 AVCodecID 转换为 ZLMediaKit::CodecId
static CodecId get_codec_id(AVCodecID av_codec) {
    switch (av_codec) {
        case AV_CODEC_ID_AAC: return CodecAAC;
        case AV_CODEC_ID_OPUS: return CodecOpus;
        // ...可以添加更多...
        default: return CodecInvalid;
    }
}
} // namespace mediakit

namespace mediakit {

static string ffmpeg_err(int errnum) {
    char errbuf[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(errnum, errbuf, AV_ERROR_MAX_STRING_SIZE);
    return errbuf;
}

std::unique_ptr<AVPacket, void (*)(AVPacket *)> alloc_av_packet() {
    return std::unique_ptr<AVPacket, void (*)(AVPacket *)>(av_packet_alloc(), [](AVPacket *pkt) { av_packet_free(&pkt); });
}

//////////////////////////////////////////////////////////////////////////////////////////
static void on_ffmpeg_log(void *ctx, int level, const char *fmt, va_list args) {
    GET_CONFIG(bool, enable_ffmpeg_log, General::kEnableFFmpegLog);
    if (!enable_ffmpeg_log) {
        return;
    }
    LogLevel lev;
    switch (level) {
        case AV_LOG_FATAL: lev = LError; break;
        case AV_LOG_ERROR: lev = LError; break;
        case AV_LOG_WARNING: lev = LWarn; break;
        case AV_LOG_INFO: lev = LInfo; break;
        case AV_LOG_VERBOSE: lev = LDebug; break;
        case AV_LOG_DEBUG: lev = LDebug; break;
        case AV_LOG_TRACE: lev = LTrace; break;
        default: lev = LTrace; break;
    }
    LoggerWrapper::printLogV(::toolkit::getLogger(), lev, __FILE__, ctx ? av_default_item_name(ctx) : "NULL", level, fmt, args);
}

static bool setupFFmpeg_l() {
    av_log_set_level(AV_LOG_TRACE);
    av_log_set_flags(AV_LOG_PRINT_LEVEL);
    av_log_set_callback(on_ffmpeg_log);
#if (LIBAVCODEC_VERSION_MAJOR < 58)
    avcodec_register_all();
#endif
    return true;
}

static void setupFFmpeg() {
    static auto flag = setupFFmpeg_l();
}

static bool checkIfSupportedNvidia_l() {
#if !defined(_WIN32)
    GET_CONFIG(bool, check_nvidia_dev, General::kCheckNvidiaDev);
    if (!check_nvidia_dev) {
        return false;
    }
    auto so = dlopen("libnvcuvid.so.1", RTLD_LAZY);
    if (!so) {
        WarnL << "libnvcuvid.so.1加载失败:" << get_uv_errmsg();
        return false;
    }
    dlclose(so);

    bool find_driver = false;
    File::scanDir("/dev", [&](const string &path, bool is_dir) {
        if (!is_dir && start_with(path, "/dev/nvidia")) {
            // 找到nvidia的驱动  [AUTO-TRANSLATED:5b87bf81]
            // Find the Nvidia driver
            find_driver = true;
            return false;
        }
        return true;
    }, false);

    if (!find_driver) {
        WarnL << "英伟达硬件编解码器驱动文件 /dev/nvidia* 不存在";
    }
    return find_driver;
#else
    return false;
#endif
}

static bool checkIfSupportedNvidia() {
    static auto ret = checkIfSupportedNvidia_l();
    return ret;
}

//////////////////////////////////////////////////////////////////////////////////////////

bool TaskManager::addEncodeTask(function<void()> task) {
    {
        lock_guard<mutex> lck(_task_mtx);
        _task.emplace_back(std::move(task));
        if (_task.size() > _max_task) {
            WarnL << "encoder thread task is too more, now drop frame!";
            _task.pop_front();
        }
    }
    _sem.post();
    return true;
}

bool TaskManager::addDecodeTask(bool key_frame, function<void()> task) {
    {
        lock_guard<mutex> lck(_task_mtx);
        if (_decode_drop_start) {
            if (!key_frame) {
                TraceL << "decode thread drop frame";
                return false;
            }
            _decode_drop_start = false;
            InfoL << "decode thread stop drop frame";
        }

        _task.emplace_back(std::move(task));
        if (_task.size() > _max_task) {
            _decode_drop_start = true;
            WarnL << "decode thread start drop frame";
        }
    }
    _sem.post();
    return true;
}

void TaskManager::setMaxTaskSize(size_t size) {
    CHECK(size >= 3 && size <= 1000, "async task size limited to 3 ~ 1000, now size is:", size);
    _max_task = size;
}

void TaskManager::startThread(const string &name) {
    _thread.reset(new thread([this, name]() {
        onThreadRun(name);
    }), [](thread *ptr) {
        if (ptr->joinable()) {
            ptr->join();
        }
        delete ptr;
    });
}

void TaskManager::stopThread(bool drop_task) {
    TimeTicker();
    if (!_thread) {
        return;
    }
    {
        lock_guard<mutex> lck(_task_mtx);
        if (drop_task) {
            _exit = true;
            _task.clear();
        }
        _task.emplace_back([]() {
            throw ThreadExitException();
        });
    }
    _sem.post(10);
    _thread = nullptr;
}

TaskManager::~TaskManager() {
    stopThread(true);
}

bool TaskManager::isEnabled() const {
    return _thread.operator bool();
}

void TaskManager::onThreadRun(const string &name) {
    setThreadName(name.data());
    function<void()> task;
    _exit = false;
    while (!_exit) {
        _sem.wait();
        {
            unique_lock<mutex> lck(_task_mtx);
            if (_task.empty()) {
                continue;
            }
            task = _task.front();
            _task.pop_front();
        }

        try {
            TimeTicker2(50, TraceL);
            task();
            task = nullptr;
        } catch (ThreadExitException &ex) {
            break;
        } catch (std::exception &ex) {
            WarnL << ex.what();
            continue;
        } catch (...) {
            WarnL << "catch one unknown exception";
            throw;
        }
    }
    InfoL << name << " exited!";
}

//////////////////////////////////////////////////////////////////////////////////////////

FFmpegFrame::FFmpegFrame(std::shared_ptr<AVFrame> frame) {
    if (frame) {
        _frame = std::move(frame);
    } else {
        _frame.reset(av_frame_alloc(), [](AVFrame *ptr) {
            av_frame_free(&ptr);
        });
    }
}

FFmpegFrame::~FFmpegFrame() {
    if (_data) {
        delete[] _data;
        _data = nullptr;
    }
}

AVFrame *FFmpegFrame::get() const {
    return _frame.get();
}

void FFmpegFrame::fillPicture(AVPixelFormat target_format, int target_width, int target_height) {
    assert(_data == nullptr);
    _data = new char[av_image_get_buffer_size(target_format, target_width, target_height, 32)];
    av_image_fill_arrays(_frame->data, _frame->linesize, (uint8_t *) _data,  target_format, target_width, target_height, 32);
}

///////////////////////////////////////////////////////////////////////////

template<bool decoder = true>
static inline const AVCodec *getCodec_l(const char *name) {
    auto codec = decoder ? avcodec_find_decoder_by_name(name) : avcodec_find_encoder_by_name(name);
    if (codec) {
        InfoL << (decoder ? "got decoder:" : "got encoder:") << name;
    } else {
        TraceL << (decoder ? "decoder:" : "encoder:") << name << " not found";
    }
    return codec;
}

template<bool decoder = true>
static inline const AVCodec *getCodec_l(enum AVCodecID id) {
    auto codec = decoder ? avcodec_find_decoder(id) : avcodec_find_encoder(id);
    if (codec) {
        InfoL << (decoder ? "got decoder:" : "got encoder:") << avcodec_get_name(id);
    } else {
        TraceL << (decoder ? "decoder:" : "encoder:") << avcodec_get_name(id) << " not found";
    }
    return codec;
}

class CodecName {
public:
    CodecName(string name) : _codec_name(std::move(name)) {}
    CodecName(enum AVCodecID id) : _id(id) {}

    template <bool decoder>
    const AVCodec *getCodec() const {
        if (!_codec_name.empty()) {
            return getCodec_l<decoder>(_codec_name.data());
        }
        return getCodec_l<decoder>(_id);
    }

private:
    string _codec_name;
    enum AVCodecID _id;
};

template <bool decoder = true>
static inline const AVCodec *getCodec(const std::initializer_list<CodecName> &codec_list) {
    const AVCodec *ret = nullptr;
    for (int i = codec_list.size(); i >= 1; --i) {
        ret = codec_list.begin()[i - 1].getCodec<decoder>();
        if (ret) {
            return ret;
        }
    }
    return ret;
}

template<bool decoder = true>
static inline const AVCodec *getCodecByName(const std::vector<std::string> &codec_list) {
    const AVCodec *ret = nullptr;
    for (auto &codec : codec_list) {
        ret = getCodec_l<decoder>(codec.data());
        if (ret) {
            return ret;
        }
    }
    return ret;
}

FFmpegDecoder::FFmpegDecoder(const Track::Ptr &track, int thread_num, const std::vector<std::string> &codec_name) {
    setupFFmpeg();
    _frame_pool.setSize(AV_NUM_DATA_POINTERS);
    const AVCodec *codec = nullptr;
    const AVCodec *codec_default = nullptr;
    if (!codec_name.empty()) {
        codec = getCodecByName(codec_name);
    }
    switch (track->getCodecId()) {
        case CodecH264:
            codec_default = getCodec({AV_CODEC_ID_H264});
            if (codec && codec->id == AV_CODEC_ID_H264) {
                break;
            }
            if (checkIfSupportedNvidia()) {
                codec = getCodec({{"libopenh264"}, {AV_CODEC_ID_H264}, {"h264_qsv"}, {"h264_videotoolbox"}, {"h264_cuvid"}, {"h264_nvmpi"}});
            } else {
                codec = getCodec({{"libopenh264"}, {AV_CODEC_ID_H264}, {"h264_qsv"}, {"h264_videotoolbox"}, {"h264_nvmpi"}});
            }
            break;
        case CodecH265:
            codec_default = getCodec({AV_CODEC_ID_HEVC});
            if (codec && codec->id == AV_CODEC_ID_HEVC) {
                break;
            }
            if (checkIfSupportedNvidia()) {
                codec = getCodec({{AV_CODEC_ID_HEVC}, {"hevc_qsv"}, {"hevc_videotoolbox"}, {"hevc_cuvid"}, {"hevc_nvmpi"}});
            } else {
                codec = getCodec({{AV_CODEC_ID_HEVC}, {"hevc_qsv"}, {"hevc_videotoolbox"}, {"hevc_nvmpi"}});
            }
            break;
        case CodecAAC:
            if (codec && codec->id == AV_CODEC_ID_AAC) {
                break;
            }
            codec = getCodec({AV_CODEC_ID_AAC});
            break;
        case CodecG711A:
            if (codec && codec->id == AV_CODEC_ID_PCM_ALAW) {
                break;
            }
            codec = getCodec({AV_CODEC_ID_PCM_ALAW});
            break;
        case CodecG711U:
            if (codec && codec->id == AV_CODEC_ID_PCM_MULAW) {
                break;
            }
            codec = getCodec({AV_CODEC_ID_PCM_MULAW});
            break;
        case CodecOpus:
            if (codec && codec->id == AV_CODEC_ID_OPUS) {
                break;
            }
            codec = getCodec({AV_CODEC_ID_OPUS});
            break;
        case CodecJPEG:
            if (codec && codec->id == AV_CODEC_ID_MJPEG) {
                break;
            }
            codec = getCodec({AV_CODEC_ID_MJPEG});
            break;
        case CodecVP8:
            if (codec && codec->id == AV_CODEC_ID_VP8) {
                break;
            }
            codec = getCodec({AV_CODEC_ID_VP8});
            break;
        case CodecVP9:
            if (codec && codec->id == AV_CODEC_ID_VP9) {
                break;
            }
            codec = getCodec({AV_CODEC_ID_VP9});
            break;
        default: codec = nullptr; break;
    }

    codec = codec ? codec : codec_default;
    if (!codec) {
        throw std::runtime_error("未找到解码器");
    }

    while (true) {
        _context.reset(avcodec_alloc_context3(codec), [](AVCodecContext *ctx) {
            avcodec_free_context(&ctx);
        });

        if (!_context) {
            throw std::runtime_error("创建解码器失败");
        }

        // 保存AVFrame的引用  [AUTO-TRANSLATED:2df53d07]
        // Save the AVFrame reference
#ifdef FF_API_OLD_ENCDEC
        _context->refcounted_frames = 1;
#endif
        _context->flags |= AV_CODEC_FLAG_LOW_DELAY;
        _context->flags2 |= AV_CODEC_FLAG2_FAST;
        if (track->getTrackType() == TrackVideo) {
            _context->width = static_pointer_cast<VideoTrack>(track)->getVideoWidth();
            _context->height = static_pointer_cast<VideoTrack>(track)->getVideoHeight();
            InfoL << "media source :" << _context->width << " X " << _context->height;
        }

        switch (track->getCodecId()) {
            case CodecG711A:
            case CodecG711U: {
                AudioTrack::Ptr audio = static_pointer_cast<AudioTrack>(track);

#if LIBAVCODEC_VERSION_INT >= FF_CODEC_VER_7_1
                av_channel_layout_default(&_context->ch_layout, audio->getAudioChannel());
#else
                _context->channels = audio->getAudioChannel();
                _context->channel_layout = av_get_default_channel_layout(_context->channels);
#endif

                _context->sample_rate = audio->getAudioSampleRate();
                break;
            }
            default:
                break;
        }
        AVDictionary *dict = nullptr;
        if (thread_num <= 0) {
            av_dict_set(&dict, "threads", "auto", 0);
        } else {
            av_dict_set(&dict, "threads", to_string(MIN((unsigned int)thread_num, thread::hardware_concurrency())).data(), 0);
        }
        av_dict_set(&dict, "zerolatency", "1", 0);
        av_dict_set(&dict, "strict", "-2", 0);

#ifdef AV_CODEC_CAP_TRUNCATED
        if (codec->capabilities & AV_CODEC_CAP_TRUNCATED) {
            /* we do not send complete frames */
            _context->flags |= AV_CODEC_FLAG_TRUNCATED;
            _do_merger = false;
        } else {
            // 此时业务层应该需要合帧  [AUTO-TRANSLATED:8dea0fff]
            // The business layer should need to merge frames at this time
            _do_merger = true;
        }
#endif

        int ret = avcodec_open2(_context.get(), codec, &dict);
        av_dict_free(&dict);
        if (ret >= 0) {
            // 成功  [AUTO-TRANSLATED:7d878ca9]
            // Success
            InfoL << "打开解码器成功:" << codec->name;
            break;
        }

        if (codec_default && codec_default != codec) {
            // 硬件编解码器打开失败，尝试软件的  [AUTO-TRANSLATED:060200f4]
            // Hardware codec failed to open, try software codec
            WarnL << "打开解码器" << codec->name << "失败，原因是:" << ffmpeg_err(ret) << ", 再尝试打开解码器" << codec_default->name;
            codec = codec_default;
            continue;
        }
        throw std::runtime_error(StrPrinter << "打开解码器" << codec->name << "失败:" << ffmpeg_err(ret));
    }
}

FFmpegDecoder::~FFmpegDecoder() {
    stopThread(true);
    if (_do_merger) {
        _merger.flush();
    }
    flush();
}

void FFmpegDecoder::flush() {
    while (true) {
        auto out_frame = _frame_pool.obtain2();
        auto ret = avcodec_receive_frame(_context.get(), out_frame->get());
        if (ret == AVERROR(EAGAIN)) {
            avcodec_send_packet(_context.get(), nullptr);
            continue;
        }
        if (ret == AVERROR_EOF) {
            break;
        }
        if (ret < 0) {
            WarnL << "avcodec_receive_frame failed:" << ffmpeg_err(ret);
            break;
        }
        onDecode(out_frame);
    }
}

const AVCodecContext *FFmpegDecoder::getContext() const {
    return _context.get();
}

bool FFmpegDecoder::inputFrame_l(const Frame::Ptr &frame, bool live, bool enable_merge) {
    if (_do_merger && enable_merge) {
        return _merger.inputFrame(frame, [this, live](uint64_t dts, uint64_t pts, const Buffer::Ptr &buffer, bool have_idr) {
            decodeFrame(buffer->data(), buffer->size(), dts, pts, live, have_idr);
        });
    }

    return decodeFrame(frame->data(), frame->size(), frame->dts(), frame->pts(), live, frame->keyFrame());
}

bool FFmpegDecoder::inputFrame(const Frame::Ptr &frame, bool live, bool async, bool enable_merge) {
    if (async && !TaskManager::isEnabled() && getContext()->codec_type == AVMEDIA_TYPE_VIDEO) {
        // 开启异步编码，且为视频，尝试启动异步解码线程  [AUTO-TRANSLATED:17a68fc6]
        // Enable asynchronous encoding, and it is video, try to start asynchronous decoding thread
        startThread("decoder thread");
    }

    if (!async || !TaskManager::isEnabled()) {
        return inputFrame_l(frame, live, enable_merge);
    }

    auto frame_cache = Frame::getCacheAbleFrame(frame);
    return addDecodeTask(frame->keyFrame(), [this, live, frame_cache, enable_merge]() {
        inputFrame_l(frame_cache, live, enable_merge);
        // 此处模拟解码太慢导致的主动丢帧  [AUTO-TRANSLATED:fc8bea8a]
        // Here simulates decoding too slow, resulting in active frame dropping
        // usleep(100 * 1000);
    });
}

bool FFmpegDecoder::decodeFrame(const char *data, size_t size, uint64_t dts, uint64_t pts, bool live, bool key_frame) {
    TimeTicker2(30, TraceL);

    auto pkt = alloc_av_packet();
    pkt->data = (uint8_t *)data;
    pkt->size = size;
    pkt->dts = dts;
    pkt->pts = pts;
    if (key_frame) {
        pkt->flags |= AV_PKT_FLAG_KEY;
    }

    // 【调试探针 1】: 确认数据包是否送达解码器
    InfoL << "FFmpegDecoder::decodeFrame: Sending packet, size=" << size << ", pts=" << pts;

    auto ret = avcodec_send_packet(_context.get(), pkt.get());
    if (ret < 0) {
        // 【调试探针 2】: 捕获发送失败的错误
        WarnL << "avcodec_send_packet failed: " << ffmpeg_err(ret);

        if (ret != AVERROR_INVALIDDATA) {
            WarnL << "avcodec_send_packet failed:" << ffmpeg_err(ret);
        }
        return false;
    }

    for (;;) {
        auto out_frame = _frame_pool.obtain2();
        ret = avcodec_receive_frame(_context.get(), out_frame->get());
        
        // 【调试探针 3】: 确认解码器是否输出了帧
        if (ret >= 0) {
            InfoL << "FFmpegDecoder::decodeFrame: !!! SUCCESSFULLY RECEIVED A PCM FRAME !!!, samples=" << out_frame->get()->nb_samples;
        }
        
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        }
        if (ret < 0) {
            WarnL << "avcodec_receive_frame failed:" << ffmpeg_err(ret);
            break;
        }
        if (live && pts - out_frame->get()->pts > MAX_DELAY_SECOND * 1000 && _ticker.createdTime() > 10 * 1000) {
            // 后面的帧才忽略,防止Track无法ready  [AUTO-TRANSLATED:23f1a7c9]
            // The following frames are ignored to prevent the Track from being ready
            WarnL << "解码时，忽略" << MAX_DELAY_SECOND << "秒前的数据:" << pts << " " << out_frame->get()->pts;
            continue;
        }
        onDecode(out_frame);
    }
    return true;
}

void FFmpegDecoder::setOnDecode(FFmpegDecoder::onDec cb) {
    _cb = std::move(cb);
}

void FFmpegDecoder::onDecode(const FFmpegFrame::Ptr &frame) {
    if (_cb) {
        _cb(frame);
    }
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////
#if LIBAVCODEC_VERSION_INT >= FF_CODEC_VER_7_1
FFmpegSwr::FFmpegSwr(AVSampleFormat output, AVChannelLayout *ch_layout, int samplerate) {
    _target_format = output;
    av_channel_layout_copy(&_target_ch_layout, ch_layout);
    _target_samplerate = samplerate;
}
#else
FFmpegSwr::FFmpegSwr(AVSampleFormat output, int channel, int channel_layout, int samplerate) {
    _target_format = output;
    _target_channels = channel;
    _target_channel_layout = channel_layout;
    _target_samplerate = samplerate;

    _swr_frame_pool.setSize(AV_NUM_DATA_POINTERS);
}
#endif

FFmpegSwr::~FFmpegSwr() {
    if (_ctx) {
        swr_free(&_ctx);
    }
#if LIBAVCODEC_VERSION_INT >= FF_CODEC_VER_7_1
    av_channel_layout_uninit(&_target_ch_layout);
#endif
}

FFmpegFrame::Ptr FFmpegSwr::inputFrame(const FFmpegFrame::Ptr &frame) {
    if (frame->get()->format == _target_format &&

#if LIBAVCODEC_VERSION_INT >= FF_CODEC_VER_7_1
        !av_channel_layout_compare(&(frame->get()->ch_layout), &_target_ch_layout) &&
#else
        frame->get()->channels == _target_channels && frame->get()->channel_layout == (uint64_t)_target_channel_layout &&
#endif

        frame->get()->sample_rate == _target_samplerate) {
        // 不转格式  [AUTO-TRANSLATED:31dc6ae1]
        // Do not convert format
        return frame;
    }
    if (!_ctx) {

#if LIBAVCODEC_VERSION_INT >= FF_CODEC_VER_7_1
        _ctx = swr_alloc();
        swr_alloc_set_opts2(&_ctx, 
                    &_target_ch_layout, _target_format, _target_samplerate, 
                    &frame->get()->ch_layout, (AVSampleFormat)frame->get()->format, frame->get()->sample_rate,
                     0, nullptr);
#else
        _ctx = swr_alloc_set_opts(nullptr, _target_channel_layout, _target_format, _target_samplerate,
                                  frame->get()->channel_layout, (AVSampleFormat) frame->get()->format,
                                  frame->get()->sample_rate, 0, nullptr);
#endif

        InfoL << "swr_alloc_set_opts:" << av_get_sample_fmt_name((enum AVSampleFormat) frame->get()->format) << " -> "
              << av_get_sample_fmt_name(_target_format);
    }
    if (_ctx) {
        auto out = _swr_frame_pool.obtain2();
        out->get()->format = _target_format;

#if LIBAVCODEC_VERSION_INT >= FF_CODEC_VER_7_1
        out->get()->ch_layout = _target_ch_layout;
        av_channel_layout_copy(&(out->get()->ch_layout), &_target_ch_layout);
#else
        out->get()->channel_layout = _target_channel_layout;
        out->get()->channels = _target_channels;
#endif

        out->get()->sample_rate = _target_samplerate;
        out->get()->pkt_dts = frame->get()->pkt_dts;
        out->get()->pts = frame->get()->pts;

        int ret = 0;
        if (0 != (ret = swr_convert_frame(_ctx, out->get(), frame->get()))) {
            WarnL << "swr_convert_frame failed:" << ffmpeg_err(ret);
            return nullptr;
        }
        return out;
    }

    return nullptr;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////

FFmpegSws::FFmpegSws(AVPixelFormat output, int width, int height) {
    _target_format = output;
    _target_width = width;
    _target_height = height;

    _sws_frame_pool.setSize(AV_NUM_DATA_POINTERS);
}

FFmpegSws::~FFmpegSws() {
    if (_ctx) {
        sws_freeContext(_ctx);
        _ctx = nullptr;
    }
}

int FFmpegSws::inputFrame(const FFmpegFrame::Ptr &frame, uint8_t *data) {
    int ret;
    inputFrame(frame, ret, data);
    return ret;
}

FFmpegFrame::Ptr FFmpegSws::inputFrame(const FFmpegFrame::Ptr &frame) {
    int ret;
    return inputFrame(frame, ret, nullptr);
}

FFmpegFrame::Ptr FFmpegSws::inputFrame(const FFmpegFrame::Ptr &frame, int &ret, uint8_t *data) {
    ret = -1;
    TimeTicker2(30, TraceL);
    auto target_width = _target_width ? _target_width : frame->get()->width;
    auto target_height = _target_height ? _target_height : frame->get()->height;
    if (frame->get()->format == _target_format && frame->get()->width == target_width && frame->get()->height == target_height) {
        // 不转格式  [AUTO-TRANSLATED:31dc6ae1]
        // Do not convert format
        return frame;
    }
    if (_ctx && (_src_width != frame->get()->width || _src_height != frame->get()->height || _src_format != (enum AVPixelFormat)frame->get()->format)) {
        // 输入分辨率发生变化了  [AUTO-TRANSLATED:0e4ea2e8]
        // Input resolution has changed
        sws_freeContext(_ctx);
        _ctx = nullptr;
    }
    if (!_ctx) {
        _src_format = (enum AVPixelFormat) frame->get()->format;
        _src_width = frame->get()->width;
        _src_height = frame->get()->height;
        _ctx = sws_getContext(frame->get()->width, frame->get()->height, (enum AVPixelFormat) frame->get()->format, target_width, target_height, _target_format, SWS_FAST_BILINEAR, NULL, NULL, NULL);
        InfoL << "sws_getContext:" << av_get_pix_fmt_name((enum AVPixelFormat) frame->get()->format) << " -> " << av_get_pix_fmt_name(_target_format);
    }
    if (_ctx) {
        auto out = _sws_frame_pool.obtain2();
        if (!out->get()->data[0]) {
            if (data) {
                av_image_fill_arrays(out->get()->data, out->get()->linesize, data, _target_format, target_width, target_height, 32);
            } else {
                out->fillPicture(_target_format, target_width, target_height);
            }
        }
        if (0 >= (ret = sws_scale(_ctx, frame->get()->data, frame->get()->linesize, 0, frame->get()->height, out->get()->data, out->get()->linesize))) {
            WarnL << "sws_scale failed:" << ffmpeg_err(ret);
            return nullptr;
        }

        out->get()->format = _target_format;
        out->get()->width = target_width;
        out->get()->height = target_height;
        out->get()->pkt_dts = frame->get()->pkt_dts;
        out->get()->pts = frame->get()->pts;
        return out;
    }
    return nullptr;
}

std::tuple<bool, std::string> FFmpegUtils::saveFrame(const FFmpegFrame::Ptr &frame, const char *filename, AVPixelFormat fmt) {
    _StrPrinter ss;
    const AVCodec *jpeg_codec = avcodec_find_encoder(fmt == AV_PIX_FMT_YUVJ420P ? AV_CODEC_ID_MJPEG : AV_CODEC_ID_PNG);
    std::unique_ptr<AVCodecContext, void (*)(AVCodecContext *)> jpeg_codec_ctx(
        jpeg_codec ? avcodec_alloc_context3(jpeg_codec) : nullptr, [](AVCodecContext *ctx) { avcodec_free_context(&ctx); });

    if (!jpeg_codec_ctx) {
        ss << "Could not allocate JPEG/PNG codec context";
        DebugL << ss;
        return make_tuple<bool, std::string>(false, ss.data());
    }

    jpeg_codec_ctx->width = frame->get()->width;
    jpeg_codec_ctx->height = frame->get()->height;
    jpeg_codec_ctx->pix_fmt = fmt;
    jpeg_codec_ctx->time_base = { 1, 1 };

    auto ret = avcodec_open2(jpeg_codec_ctx.get(), jpeg_codec, NULL);
    if (ret < 0) {
        ss << "Could not open JPEG/PNG codec, " << ffmpeg_err(ret);
        DebugL << ss;
        return make_tuple<bool, std::string>(false, ss.data());
    }

    FFmpegSws sws(fmt, 0, 0);
    auto new_frame = sws.inputFrame(frame);
    if (!new_frame) {
        ss << "Could not scale the frame: " << ffmpeg_err(ret);
        DebugL << ss;
        return make_tuple<bool, std::string>(false, ss.data());
    }

    auto pkt = alloc_av_packet();
    ret = avcodec_send_frame(jpeg_codec_ctx.get(), new_frame->get());
    if (ret < 0) {
        ss << "Error sending a frame for encoding, " << ffmpeg_err(ret);
        DebugL << ss;
        return make_tuple<bool, std::string>(false, ss.data());
    }

    std::unique_ptr<FILE, void (*)(FILE *)> tmp_save_file_jpg(File::create_file(filename, "wb"), [](FILE *fp) {
        if (fp) {
            fclose(fp);
        }
    });

    if (!tmp_save_file_jpg) {
        ss << "Could not open the file " << filename;
        DebugL << ss;
        return make_tuple<bool, std::string>(false, ss.data());
    }

    while (avcodec_receive_packet(jpeg_codec_ctx.get(), pkt.get()) == 0) {
        fwrite(pkt.get()->data, pkt.get()->size, 1, tmp_save_file_jpg.get());
    }
    DebugL << "Screenshot successful: " << filename;
    return make_tuple<bool, std::string>(true, "");
}


class Transcode::Imp {
public:
    FFmpegDecoder::Ptr _decoder;
    FFmpegSwr::Ptr _swr;
    std::shared_ptr<AVCodecContext> _encoder_ctx;
    on_transcoded_frame _on_frame;
    
    // 【修正4】: 添加frame_size成员
    int _frame_size = 0;
    
    // 【修正1】: 时间戳管理，只在输出域工作
    int64_t _total_output_samples = 0;
    
    AVAudioFifo *_audio_fifo = nullptr;
    std::shared_ptr<AVFrame> _fifo_frame;

    ~Imp() {
        if (_audio_fifo) {
            av_audio_fifo_free(_audio_fifo);
        }
    }
    
    // 【修正2】: 统一函数签名
    void encode_frame(AVFrame *frame_to_encode) {
        if (!_encoder_ctx) return;

        int ret = avcodec_send_frame(_encoder_ctx.get(), frame_to_encode);
        if (ret < 0) {
            if (ret != AVERROR_EOF) { WarnL << "avcodec_send_frame failed: " << ffmpeg_err(ret); }
            return;
        }

        while (true) {
            auto pkt = alloc_av_packet();
            ret = avcodec_receive_packet(_encoder_ctx.get(), pkt.get());
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) { break; }
            if (ret < 0) { WarnL << "FFmpeg encoding failed: " << ffmpeg_err(ret); break; }
            
            auto out_dts = av_rescale_q(pkt->dts, _encoder_ctx->time_base, {1, 1000});
            auto out_pts = av_rescale_q(pkt->pts, _encoder_ctx->time_base, {1, 1000});

            auto frame = std::make_shared<FrameFromPtr>(get_codec_id(_encoder_ctx->codec_id), (char*)pkt->data, pkt->size, out_dts, out_pts);
            if (_on_frame) {
                _on_frame(frame);
            }
        }
    }
    
    void flush_fifo() {
        if (!_audio_fifo || !_encoder_ctx || !_fifo_frame) return;
        
        while (av_audio_fifo_size(_audio_fifo) > 0) {
            int available_samples = av_audio_fifo_size(_audio_fifo);
            int read_samples = (std::min)(available_samples, _frame_size);

            av_frame_make_writable(_fifo_frame.get());
            
            av_audio_fifo_read(_audio_fifo, (void **)_fifo_frame->data, read_samples);
            
            // 【修正3】: 使用安全的静音填充
            if (read_samples < _frame_size) {
#if LIBAVCODEC_VERSION_INT >= FF_CODEC_VER_7_1
                av_samples_set_silence(_fifo_frame->data, read_samples, _frame_size - read_samples, _fifo_frame->ch_layout.nb_channels, (AVSampleFormat)_fifo_frame->format);
#else
                av_samples_set_silence(_fifo_frame->data, read_samples, _frame_size - read_samples, _fifo_frame->channels, (AVSampleFormat)_fifo_frame->format);
#endif
            }
            
            _fifo_frame->nb_samples = _frame_size;
            
            // 【修正1】: 直接使用输出样本数作为PTS
            _fifo_frame->pts = _total_output_samples;
            _total_output_samples += _fifo_frame->nb_samples;
            
            encode_frame(_fifo_frame.get());
        }
    }
};

Transcode::Transcode() {
    _imp.reset(new Imp());
}

Transcode::~Transcode() {
    if (_imp) {
        flush();
    }
}

bool Transcode::open(const Track::Ptr &src_track, CodecId dst_codec, int dst_samplerate, int dst_channels) {
    InfoL << ">>>>>>>>>> Transcode::open entered.";
    
    auto audio_track = std::dynamic_pointer_cast<AudioTrack>(src_track);
    if (!audio_track) { 
        WarnL << ">>>>>>>>>> Transcode::open ERROR: Source is not an audio track.";
        return false; 
    }
    
    _imp->_decoder = std::make_shared<FFmpegDecoder>(src_track);
    
    dst_samplerate = 48000;
    dst_channels = audio_track->getAudioChannel();

    InfoL << ">>>>>>>>>> Transcode::open Stage 1: Decoder created. Target format: " << dst_samplerate << "Hz, " << dst_channels << "ch.";
    
    // 【最终修正】: 始终使用旧版API，不再需要 #if ... #endif
    _imp->_swr = std::make_shared<FFmpegSwr>(AV_SAMPLE_FMT_S16P, dst_channels,
                                       av_get_default_channel_layout(dst_channels), dst_samplerate);

    const AVCodec *encoder = avcodec_find_encoder(get_avcodec_id(dst_codec));
    if(!encoder) { 
        WarnL << ">>>>>>>>>> Transcode::open ERROR: Opus encoder not found.";
        return false; 
    }

    InfoL << ">>>>>>>>>> Transcode::open Stage 2: Swr and Encoder found. Allocating context...";
    
    _imp->_encoder_ctx.reset(avcodec_alloc_context3(encoder), [](AVCodecContext *ctx) {
        if (ctx) avcodec_free_context(&ctx);
    });

    auto ctx = _imp->_encoder_ctx.get();
    ctx->sample_fmt = AV_SAMPLE_FMT_S16P;
    ctx->sample_rate = dst_samplerate;
    ctx->time_base = {1, dst_samplerate};
    ctx->bit_rate = 128000;

    // 【最终修正】: 始终使用旧版API
    ctx->channels = dst_channels;
    ctx->channel_layout = av_get_default_channel_layout(dst_channels);
    
    AVDictionary *opts = nullptr;
    av_dict_set(&opts, "application", "audio", 0);
    av_dict_set(&opts, "vbr", "on", 0);

    InfoL << ">>>>>>>>>> Transcode::open Stage 2.5: Context configured. Opening codec...";

    int ret = avcodec_open2(ctx, encoder, &opts);
    av_dict_free(&opts);
    
    if (ret < 0) {
        WarnL << ">>>>>>>>>> Transcode::open ERROR: avcodec_open2 failed with error: " << ffmpeg_err(ret);
        return false;
    }
    
    InfoL << ">>>>>>>>>> Transcode::open Stage 3: Codec opened successfully.";

    int frame_size = ctx->frame_size ? ctx->frame_size : 960;
    _imp->_frame_size = frame_size;
    
    // 【最终修正】: 始终使用旧版API
    _imp->_audio_fifo = av_audio_fifo_alloc(ctx->sample_fmt, ctx->channels, frame_size * 4);
    
    _imp->_fifo_frame.reset(av_frame_alloc(), [](AVFrame *ptr){ av_frame_free(&ptr); });
    _imp->_fifo_frame->nb_samples = frame_size;
    _imp->_fifo_frame->sample_rate = dst_samplerate;
    _imp->_fifo_frame->format = ctx->sample_fmt;
    
    // 【最终修正】: 始终使用旧版API
    _imp->_fifo_frame->channel_layout = ctx->channel_layout;
    _imp->_fifo_frame->channels = ctx->channels;
    
    if (av_frame_get_buffer(_imp->_fifo_frame.get(), 0) < 0) { 
        WarnL << ">>>>>>>>>> Transcode::open ERROR: Failed to allocate buffer for FIFO frame.";
        return false; 
    }



    _imp->_decoder->setOnDecode([this](const FFmpegFrame::Ptr &pcm_frame) {
        auto resampled_frame = _imp->_swr->inputFrame(pcm_frame);
        InfoL << ">>>>>>>>>>>>>>>>>>>>>>>>>>> 4";
        if (!resampled_frame) return;
        InfoL << ">>>>>>>>>>>>>>>>>>>>>>>>>>> 5";
        av_audio_fifo_write(_imp->_audio_fifo, (void **)resampled_frame->get()->data, resampled_frame->get()->nb_samples);

        while (av_audio_fifo_size(_imp->_audio_fifo) >= _imp->_frame_size) {
            av_audio_fifo_read(_imp->_audio_fifo, (void **)_imp->_fifo_frame->data, _imp->_frame_size);
            
            // 【修正1】: 直接使用输出样本数作为PTS
            _imp->_fifo_frame->pts = _imp->_total_output_samples;
            _imp->_total_output_samples += _imp->_fifo_frame->nb_samples;

            _imp->encode_frame(_imp->_fifo_frame.get());
        }
    });

    return true;
}

void Transcode::inputFrame(const Frame::Ptr &frame) {
    if (_imp && _imp->_decoder) {
        _imp->_decoder->inputFrame(frame, false, false);
    }
}

void Transcode::flush() {
    if (_imp) {
        _imp->flush_fifo();
        if (_imp->_encoder_ctx) {
            _imp->encode_frame(nullptr);
        }
    }
}

void Transcode::setOnFrame(on_transcoded_frame cb) {
    if (_imp) {
        _imp->_on_frame = std::move(cb);
    }
}


} // namespace mediakit
#endif // ENABLE_FFMPEG