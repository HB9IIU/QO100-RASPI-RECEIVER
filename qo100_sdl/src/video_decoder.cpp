#include "video_decoder.h"
#include "app_log.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/mathematics.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstddef>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <utility>

namespace qo100 {
namespace {

/* Must match receiver.cpp's ts_destination_address()/ts_destination_port()
 * defaults - this is the receiving end of the same UDP feed Longmynd sends
 * to. Overridable via the same QO100_TS_ADDR/QO100_TS_PORT env vars in case
 * the multicast group needs to change (e.g. to avoid a clash on the LAN). */
std::string input_url()
{
    const char * address = std::getenv("QO100_TS_ADDR");
    const char * port = std::getenv("QO100_TS_PORT");
    return "udp://" + std::string(address != nullptr ? address : "239.1.1.1") +
        ":" + std::string(port != nullptr ? port : "5600") +
        "?fifo_size=65536&overrun_nonfatal=1&buffer_size=4194304&timeout=1000000";
}
constexpr AVRational kMicrosecondTimeBase{1, 1000000};
constexpr int kAudioSampleRate = 48000;
constexpr int kAudioChannels = 2;

std::string ffmpeg_error(int error)
{
    char buffer[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(error, buffer, sizeof(buffer));
    return buffer;
}

void copy_plane(uint8_t * destination, int destination_pitch,
                const uint8_t * source, int source_pitch,
                int row_bytes, int rows)
{
    if(source_pitch < 0) {
        source += static_cast<ptrdiff_t>(rows - 1) * source_pitch;
        source_pitch = -source_pitch;
    }
    for(int row = 0; row < rows; ++row) {
        std::memcpy(destination + static_cast<size_t>(row) * destination_pitch,
                    source + static_cast<ptrdiff_t>(row) * source_pitch,
                    static_cast<size_t>(row_bytes));
    }
}

} // namespace

struct VideoDecoder::Impl {
    explicit Impl(FrameCallback output, AudioCallback audio_output)
        : callback(std::move(output)), audio_callback(std::move(audio_output)) {}

    FrameCallback callback;
    AudioCallback audio_callback;
    std::atomic<bool> running{false};
    std::atomic<bool> reset_requested{false};
    std::thread thread;
    std::atomic<uint64_t> frame_count{0};
    std::atomic<uint64_t> audio_chunk_count{0};
    std::atomic<uint64_t> reopens{0};
    std::atomic<uint64_t> errors{0};
    std::atomic<uint64_t> audio_errors{0};
    mutable std::mutex codec_mutex;
    std::string codec;
    std::string audio_codec;

    static int interrupt(void * opaque)
    {
        auto * self = static_cast<Impl *>(opaque);
        return !self->running.load(std::memory_order_relaxed) ||
               self->reset_requested.load(std::memory_order_relaxed);
    }

    void set_codec(const char * value)
    {
        std::lock_guard<std::mutex> lock(codec_mutex);
        codec = value != nullptr ? value : "";
        std::transform(codec.begin(), codec.end(), codec.begin(),
                       [](unsigned char character) {
                           return static_cast<char>(std::toupper(character));
                       });
    }

    void set_audio_codec(const char * value)
    {
        std::lock_guard<std::mutex> lock(codec_mutex);
        audio_codec = value != nullptr ? value : "";
        std::transform(audio_codec.begin(), audio_codec.end(), audio_codec.begin(),
                       [](unsigned char character) {
                           return static_cast<char>(std::toupper(character));
                       });
    }

    bool convert_frame(const AVFrame * source, SwsContext *& scaler,
                       int64_t pts_us, VideoFrame & output)
    {
        if(source->width <= 0 || source->height <= 0) return false;
        output.width = source->width;
        output.height = source->height;
        output.y_pitch = source->width;
        output.uv_pitch = (source->width + 1) / 2;
        const int chroma_height = (source->height + 1) / 2;
        const size_t y_size = static_cast<size_t>(output.y_pitch) * source->height;
        const size_t uv_size = static_cast<size_t>(output.uv_pitch) * chroma_height;
        output.u_offset = y_size;
        output.v_offset = y_size + uv_size;
        output.yuv420.resize(y_size + uv_size * 2U);
        output.pts_us = pts_us;

        uint8_t * destinations[4] = {
            output.yuv420.data(), output.yuv420.data() + output.u_offset,
            output.yuv420.data() + output.v_offset, nullptr
        };
        int destination_strides[4] = {output.y_pitch, output.uv_pitch, output.uv_pitch, 0};
        const AVPixelFormat format = static_cast<AVPixelFormat>(source->format);
        if(format == AV_PIX_FMT_YUV420P || format == AV_PIX_FMT_YUVJ420P) {
            copy_plane(destinations[0], output.y_pitch, source->data[0], source->linesize[0],
                       source->width, source->height);
            copy_plane(destinations[1], output.uv_pitch, source->data[1], source->linesize[1],
                       output.uv_pitch, chroma_height);
            copy_plane(destinations[2], output.uv_pitch, source->data[2], source->linesize[2],
                       output.uv_pitch, chroma_height);
            return true;
        }

        scaler = sws_getCachedContext(scaler, source->width, source->height, format,
            source->width, source->height, AV_PIX_FMT_YUV420P,
            SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
        if(scaler == nullptr) return false;
        return sws_scale(scaler, source->data, source->linesize, 0, source->height,
                         destinations, destination_strides) == source->height;
    }

    bool convert_audio(AVFrame * source, SwrContext *& resampler,
                       AudioChunk & output)
    {
        const int input_rate = source->sample_rate;
        const AVSampleFormat input_format = static_cast<AVSampleFormat>(source->format);
        if(input_rate <= 0 || source->ch_layout.nb_channels <= 0 ||
           input_format == AV_SAMPLE_FMT_NONE)
            return false;

        if(resampler == nullptr) {
            AVChannelLayout output_layout = AV_CHANNEL_LAYOUT_STEREO;
            if(swr_alloc_set_opts2(&resampler,
                    &output_layout, AV_SAMPLE_FMT_S16, kAudioSampleRate,
                    &source->ch_layout, input_format, input_rate, 0, nullptr) < 0 ||
               resampler == nullptr || swr_init(resampler) < 0) {
                swr_free(&resampler);
                return false;
            }
        }

        const int output_samples = static_cast<int>(av_rescale_rnd(
            swr_get_delay(resampler, input_rate) + source->nb_samples,
            kAudioSampleRate, input_rate, AV_ROUND_UP));
        if(output_samples <= 0) return false;

        output.sample_rate = kAudioSampleRate;
        output.channels = kAudioChannels;
        output.pcm_s16.resize(static_cast<size_t>(output_samples) *
                              kAudioChannels * sizeof(int16_t));
        uint8_t * destination[] = {output.pcm_s16.data()};
        const uint8_t ** input = const_cast<const uint8_t **>(source->extended_data);
        const int converted = swr_convert(resampler, destination, output_samples,
                                          input, source->nb_samples);
        if(converted <= 0) return false;
        output.pcm_s16.resize(static_cast<size_t>(converted) *
                              kAudioChannels * sizeof(int16_t));
        return true;
    }

    void run_session()
    {
        AVFormatContext * format = avformat_alloc_context();
        if(format == nullptr) return;
        format->interrupt_callback.callback = &Impl::interrupt;
        format->interrupt_callback.opaque = this;

        AVDictionary * options = nullptr;
        av_dict_set(&options, "probesize", "2000000", 0);
        av_dict_set(&options, "analyzeduration", "2000000", 0);
        const std::string url = input_url();
        int result = avformat_open_input(&format, url.c_str(), nullptr, &options);
        av_dict_free(&options);
        if(result < 0) {
            avformat_free_context(format);
            if(running.load() && !reset_requested.load()) {
                ++errors;
                if(errors.load() <= 3 || errors.load() % 20 == 0)
                    qo100::log( "[VIDEO] input open: %s\n", ffmpeg_error(result).c_str());
            }
            return;
        }

        result = avformat_find_stream_info(format, nullptr);
        if(result < 0) {
            if(running.load() && !reset_requested.load()) {
                ++errors;
                qo100::log( "[VIDEO] stream info: %s\n", ffmpeg_error(result).c_str());
            }
            avformat_close_input(&format);
            return;
        }

        const AVCodec * decoder = nullptr;
        const int video_stream = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO,
                                                      -1, -1, &decoder, 0);
        if(video_stream < 0 || decoder == nullptr) {
            ++errors;
            qo100::log( "[VIDEO] no video stream: %s\n",
                         ffmpeg_error(video_stream).c_str());
            avformat_close_input(&format);
            return;
        }

        AVCodecContext * codec_context = avcodec_alloc_context3(decoder);
        if(codec_context == nullptr ||
           avcodec_parameters_to_context(codec_context,
               format->streams[video_stream]->codecpar) < 0) {
            avcodec_free_context(&codec_context);
            avformat_close_input(&format);
            return;
        }
        codec_context->thread_count = 2;
        codec_context->thread_type = FF_THREAD_FRAME;
        codec_context->flags |= AV_CODEC_FLAG_LOW_DELAY;
        result = avcodec_open2(codec_context, decoder, nullptr);
        if(result < 0) {
            ++errors;
            qo100::log( "[VIDEO] decoder open: %s\n", ffmpeg_error(result).c_str());
            avcodec_free_context(&codec_context);
            avformat_close_input(&format);
            return;
        }
        set_codec(decoder->name);

        const AVStream * stream = format->streams[video_stream];
        AVRational frame_rate = av_guess_frame_rate(format, const_cast<AVStream *>(stream), nullptr);
        int64_t nominal_duration_us = 40000;
        if(frame_rate.num > 0 && frame_rate.den > 0) {
            nominal_duration_us = std::max<int64_t>(1,
                av_rescale_q(1, av_inv_q(frame_rate), kMicrosecondTimeBase));
        }
        qo100::log( "[VIDEO] opened codec=%s %dx%d rate=%.3f\n",
            decoder->name, codec_context->width, codec_context->height,
            frame_rate.den != 0 ? av_q2d(frame_rate) : 0.0);

        const AVCodec * audio_decoder = nullptr;
        const int audio_stream = audio_callback
            ? av_find_best_stream(format, AVMEDIA_TYPE_AUDIO, -1, video_stream,
                                  &audio_decoder, 0)
            : AVERROR_STREAM_NOT_FOUND;
        AVCodecContext * audio_context = nullptr;
        set_audio_codec(nullptr);
        if(audio_stream >= 0 && audio_decoder != nullptr) {
            audio_context = avcodec_alloc_context3(audio_decoder);
            if(audio_context == nullptr ||
               avcodec_parameters_to_context(audio_context,
                   format->streams[audio_stream]->codecpar) < 0 ||
               avcodec_open2(audio_context, audio_decoder, nullptr) < 0) {
                ++audio_errors;
                qo100::log( "[AUDIO] decoder open failed for codec=%s\n",
                            audio_decoder->name);
                avcodec_free_context(&audio_context);
            }
            else {
                set_audio_codec(audio_decoder->name);
                qo100::log( "[AUDIO] opened codec=%s rate=%dHz channels=%d\n",
                            audio_decoder->name, audio_context->sample_rate,
                            audio_context->ch_layout.nb_channels);
            }
        }
        else if(audio_callback) {
            qo100::log( "[AUDIO] no audio stream in transport stream\n");
        }

        AVPacket * packet = av_packet_alloc();
        AVFrame * frame = av_frame_alloc();
        SwsContext * scaler = nullptr;
        SwrContext * resampler = nullptr;
        int64_t fallback_pts_us = 0;
        bool have_fallback = false;

        while(running.load(std::memory_order_relaxed) &&
              !reset_requested.load(std::memory_order_relaxed)) {
            result = av_read_frame(format, packet);
            if(result < 0) break;
            if(packet->stream_index == video_stream) {
                result = avcodec_send_packet(codec_context, packet);
                av_packet_unref(packet);
                if(result < 0 && result != AVERROR(EAGAIN)) {
                    ++errors;
                    continue;
                }

                while(running.load(std::memory_order_relaxed) &&
                      !reset_requested.load(std::memory_order_relaxed)) {
                    result = avcodec_receive_frame(codec_context, frame);
                    if(result == AVERROR(EAGAIN) || result == AVERROR_EOF) break;
                    if(result < 0) {
                        ++errors;
                        break;
                    }
                    int64_t pts_us = 0;
                    const int64_t timestamp = frame->best_effort_timestamp != AV_NOPTS_VALUE
                        ? frame->best_effort_timestamp : frame->pts;
                    if(timestamp != AV_NOPTS_VALUE) {
                        pts_us = av_rescale_q(timestamp, stream->time_base,
                                              kMicrosecondTimeBase);
                        fallback_pts_us = pts_us;
                        have_fallback = true;
                    }
                    else {
                        if(!have_fallback) {
                            fallback_pts_us = 0;
                            have_fallback = true;
                        }
                        else fallback_pts_us += nominal_duration_us;
                        pts_us = fallback_pts_us;
                    }

                    VideoFrame converted;
                    if(convert_frame(frame, scaler, pts_us, converted)) {
                        callback(std::move(converted));
                        ++frame_count;
                    }
                    else ++errors;
                    av_frame_unref(frame);
                }
            }
            else if(audio_context != nullptr && packet->stream_index == audio_stream) {
                result = avcodec_send_packet(audio_context, packet);
                av_packet_unref(packet);
                if(result < 0 && result != AVERROR(EAGAIN)) {
                    ++audio_errors;
                    continue;
                }
                while(running.load(std::memory_order_relaxed) &&
                      !reset_requested.load(std::memory_order_relaxed)) {
                    result = avcodec_receive_frame(audio_context, frame);
                    if(result == AVERROR(EAGAIN) || result == AVERROR_EOF) break;
                    if(result < 0) {
                        ++audio_errors;
                        break;
                    }
                    AudioChunk converted;
                    if(convert_audio(frame, resampler, converted)) {
                        audio_callback(std::move(converted));
                        ++audio_chunk_count;
                    }
                    else ++audio_errors;
                    av_frame_unref(frame);
                }
            }
            else av_packet_unref(packet);
        }

        swr_free(&resampler);
        sws_freeContext(scaler);
        av_frame_free(&frame);
        av_packet_free(&packet);
        avcodec_free_context(&audio_context);
        avcodec_free_context(&codec_context);
        avformat_close_input(&format);
    }

    void run()
    {
        while(running.load(std::memory_order_relaxed)) {
            reset_requested = false;
            ++reopens;
            run_session();
            if(running.load(std::memory_order_relaxed))
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
};

VideoDecoder::VideoDecoder(FrameCallback callback, AudioCallback audio_callback)
    : impl_(std::make_unique<Impl>(std::move(callback), std::move(audio_callback)))
{}

VideoDecoder::~VideoDecoder() { stop(); }

void VideoDecoder::start()
{
    if(impl_->running.exchange(true)) return;
    impl_->thread = std::thread([this] { impl_->run(); });
}

void VideoDecoder::stop()
{
    if(!impl_->running.exchange(false)) return;
    impl_->reset_requested = true;
    if(impl_->thread.joinable()) impl_->thread.join();
}

void VideoDecoder::request_reset()
{
    impl_->set_codec(nullptr);
    impl_->set_audio_codec(nullptr);
    impl_->reset_requested = true;
}

std::string VideoDecoder::codec_name() const
{
    std::lock_guard<std::mutex> lock(impl_->codec_mutex);
    return impl_->codec;
}

std::string VideoDecoder::audio_codec_name() const
{
    std::lock_guard<std::mutex> lock(impl_->codec_mutex);
    return impl_->audio_codec;
}

uint64_t VideoDecoder::decoded_frames() const { return impl_->frame_count.load(); }
uint64_t VideoDecoder::decoded_audio_chunks() const
{
    return impl_->audio_chunk_count.load();
}
uint64_t VideoDecoder::reopen_count() const { return impl_->reopens.load(); }
uint64_t VideoDecoder::decode_errors() const { return impl_->errors.load(); }
uint64_t VideoDecoder::audio_decode_errors() const
{
    return impl_->audio_errors.load();
}

} // namespace qo100
