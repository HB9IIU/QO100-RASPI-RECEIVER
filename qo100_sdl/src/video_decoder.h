#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace qo100 {

struct VideoFrame {
    std::vector<uint8_t> yuv420;
    int width = 0;
    int height = 0;
    int y_pitch = 0;
    int uv_pitch = 0;
    size_t u_offset = 0;
    size_t v_offset = 0;
    int64_t pts_us = 0;

    const uint8_t * y_plane() const { return yuv420.data(); }
    const uint8_t * u_plane() const { return yuv420.data() + u_offset; }
    const uint8_t * v_plane() const { return yuv420.data() + v_offset; }
};

struct AudioChunk {
    std::vector<uint8_t> pcm_s16;
    int sample_rate = 48000;
    int channels = 2;
};

class VideoDecoder {
public:
    using FrameCallback = std::function<void(VideoFrame &&)>;
    using AudioCallback = std::function<void(AudioChunk &&)>;

    explicit VideoDecoder(FrameCallback callback, AudioCallback audio_callback = {});
    ~VideoDecoder();

    VideoDecoder(const VideoDecoder &) = delete;
    VideoDecoder & operator=(const VideoDecoder &) = delete;

    void start();
    void stop();
    void request_reset();

    std::string codec_name() const;
    std::string audio_codec_name() const;
    uint64_t decoded_frames() const;
    uint64_t decoded_audio_chunks() const;
    uint64_t reopen_count() const;
    uint64_t decode_errors() const;
    uint64_t audio_decode_errors() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace qo100
