/*
	音频静音处理，支持分段静音
*/
#include <iostream>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <algorithm>
#include <cmath>

extern "C" {
#include "libavformat/avformat.h"
#include "libavcodec/avcodec.h"
#include "libswresample/swresample.h"
}

struct MuteSection {
    double start;
    double end;
};

class AudioMuter {
private:
    const char* m_in_file;
    const char* m_out_file;

    AVFormatContext* m_ifmt_ctx;
    AVFormatContext* m_ofmt_ctx;

    int m_audio_idx;
    int m_video_idx;
    int m_out_video_idx;

    AVStream* m_in_audio_stream;
    AVCodecContext* m_dec_ctx;
    AVCodec* m_decoder;

    AVCodecContext* m_enc_ctx;
    AVCodec* m_encoder;
    AVStream* m_out_audio_stream;

    SwrContext* m_swr;

    MuteSection* m_mute_sections;
    int m_mute_count;
    double m_input_duration_sec;

private:
    bool is_mp4_file(const char* filename);
    int need_mute(double pts);
    void mute_frame_generic(AVFrame* frame, double start_pts);
    void flush_encoder();
    int init_decoder();
    int init_encoder();
    int init_swr();
    void process_audio_frame(AVFrame* frame, AVFrame* frame_resampled);
    void release_resources();

public:
    AudioMuter(const char* in_file, const char* out_file, MuteSection* mute_sections, int mute_count);
    ~AudioMuter();
    int run();
};

static void set_ffmpeg_log_level() {
    av_log_set_level(AV_LOG_ERROR);
}

bool AudioMuter::is_mp4_file(const char* filename) {
    if (!filename) return false;
    std::string name = filename;
    size_t pos = name.find_last_of(".");
    if (pos == std::string::npos) return false;
    std::string ext = name.substr(pos + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return ext == "mp4";
}

int AudioMuter::need_mute(double pts) {
    if (!m_mute_sections || m_mute_count <= 0) return 0;
    for (int i = 0; i < m_mute_count; i++) {
        if (pts >= m_mute_sections[i].start && pts <= m_mute_sections[i].end) {
            return 1;
        }
    }
    return 0;
}

void AudioMuter::mute_frame_generic(AVFrame* frame, double start_pts) {
    if (!frame) return;
    AVSampleFormat fmt = (AVSampleFormat)frame->format;
    int planar = av_sample_fmt_is_planar(fmt);
    int channels = frame->channels;
    int samples = frame->nb_samples;
    int bps = av_get_bytes_per_sample(fmt);
    double sr = frame->sample_rate;

    for (int i = 0; i < samples; i++) {
        double pts = start_pts + (double)i / sr;
        if (!need_mute(pts)) continue;

        if (planar) {
            for (int ch = 0; ch < channels; ch++) {
                if (frame->data[ch]) {
                    memset(frame->data[ch] + i * bps, 0, bps);
                }
            }
        } else {
            if (frame->data[0]) {
                memset(frame->data[0] + i * bps * channels, 0, bps * channels);
            }
        }
    }
}

void AudioMuter::flush_encoder() {
    if (!m_enc_ctx || !m_ofmt_ctx || !m_out_audio_stream) return;
    AVPacket opkt;
    memset(&opkt, 0, sizeof(opkt));
    while (avcodec_send_frame(m_enc_ctx, NULL) >= 0) {
        while (avcodec_receive_packet(m_enc_ctx, &opkt) == 0) {
            opkt.stream_index = m_out_audio_stream->index;
            av_interleaved_write_frame(m_ofmt_ctx, &opkt);
            av_packet_unref(&opkt);
        }
    }
}

int AudioMuter::init_decoder() {
    if (avformat_open_input(&m_ifmt_ctx, m_in_file, NULL, NULL) < 0) {
        fprintf(stderr, "打开输入文件失败\n");
        return -1;
    }
    avformat_find_stream_info(m_ifmt_ctx, NULL);

    m_audio_idx = m_video_idx = -1;
    for (int i = 0; i < m_ifmt_ctx->nb_streams; i++) {
        AVCodecParameters* par = m_ifmt_ctx->streams[i]->codecpar;
        if (par->codec_type == AVMEDIA_TYPE_AUDIO) m_audio_idx = i;
        if (par->codec_type == AVMEDIA_TYPE_VIDEO) m_video_idx = i;
    }

    if (m_audio_idx < 0) {
        fprintf(stderr, "未找到音频流\n");
        return -1;
    }

    m_in_audio_stream = m_ifmt_ctx->streams[m_audio_idx];
    m_dec_ctx = avcodec_alloc_context3(NULL);
    avcodec_parameters_to_context(m_dec_ctx, m_in_audio_stream->codecpar);

    if (m_dec_ctx->channel_layout == 0) {
        m_dec_ctx->channel_layout = av_get_default_channel_layout(m_dec_ctx->channels);
    }

    m_decoder = avcodec_find_decoder(m_dec_ctx->codec_id);
    if (!m_decoder || avcodec_open2(m_dec_ctx, m_decoder, NULL) < 0) {
        fprintf(stderr, "解码器打开失败\n");
        return -1;
    }

    m_input_duration_sec = m_in_audio_stream->duration * av_q2d(m_in_audio_stream->time_base);
    if (m_input_duration_sec <= 0) {
        m_input_duration_sec = m_ifmt_ctx->duration / 1000000.0;
    }
    return 0;
}

int AudioMuter::init_encoder() {
    // MP4复用器不支持amr，使用3gp复用器
    if (is_mp4_file(m_out_file) && m_dec_ctx->codec_id == AV_CODEC_ID_AMR_NB) {
        avformat_alloc_output_context2(&m_ofmt_ctx, av_guess_format("3gp", NULL, NULL), NULL, m_out_file);
    } else {
        avformat_alloc_output_context2(&m_ofmt_ctx, NULL, NULL, m_out_file);
    }

    if (!m_ofmt_ctx) return -1;

    if (m_video_idx >= 0) {
        AVStream* in_v = m_ifmt_ctx->streams[m_video_idx];
        AVStream* out_v = avformat_new_stream(m_ofmt_ctx, NULL);
        avcodec_parameters_copy(out_v->codecpar, in_v->codecpar);
        m_out_video_idx = out_v->index;
    }

    m_encoder = avcodec_find_encoder(m_dec_ctx->codec_id);
    if (!m_encoder) {
        m_encoder = avcodec_find_encoder(AV_CODEC_ID_AMR_NB);
    }
    if (!m_encoder) {
        fprintf(stderr, "未找到编码器\n");
        return -1;
    }
    
    m_enc_ctx = avcodec_alloc_context3(m_encoder);
    
    m_enc_ctx->channels       = m_dec_ctx->channels;
    m_enc_ctx->channel_layout = m_dec_ctx->channel_layout;
    m_enc_ctx->sample_rate    = m_dec_ctx->sample_rate;
    m_enc_ctx->bit_rate       = m_dec_ctx->bit_rate;
    m_enc_ctx->sample_fmt     = m_encoder->sample_fmts[0];
    m_enc_ctx->time_base      = m_in_audio_stream->time_base;

    if (avcodec_open2(m_enc_ctx, m_encoder, NULL) < 0) {
        fprintf(stderr, "编码器打开失败\n");
        return -1;
    }

    m_out_audio_stream = avformat_new_stream(m_ofmt_ctx, NULL);
    avcodec_parameters_from_context(m_out_audio_stream->codecpar, m_enc_ctx);

    return 0;
}

int AudioMuter::init_swr() {
    m_swr = swr_alloc_set_opts(NULL,
        m_enc_ctx->channel_layout, m_enc_ctx->sample_fmt, m_enc_ctx->sample_rate,
        m_dec_ctx->channel_layout, m_dec_ctx->sample_fmt, m_dec_ctx->sample_rate, 0, NULL);
    return swr_init(m_swr);
}

void AudioMuter::process_audio_frame(AVFrame* frame, AVFrame* frame_resampled) {
    if (!frame || !frame_resampled) return;
    double pts = frame->pts * av_q2d(m_in_audio_stream->time_base);
    if (pts >= m_input_duration_sec) {
        av_frame_unref(frame);
        return;
    }

    mute_frame_generic(frame, pts);

    frame_resampled->nb_samples = frame->nb_samples;
    frame_resampled->format = m_enc_ctx->sample_fmt;
    frame_resampled->channels = m_enc_ctx->channels;
    frame_resampled->channel_layout = m_enc_ctx->channel_layout;
    av_frame_get_buffer(frame_resampled, 0);

    swr_convert(m_swr, frame_resampled->data, frame_resampled->nb_samples,
                (const uint8_t**)frame->data, frame->nb_samples);
    frame_resampled->pts = frame->pts;

    avcodec_send_frame(m_enc_ctx, frame_resampled);
    AVPacket opkt;
    memset(&opkt, 0, sizeof(opkt));

    if (avcodec_receive_packet(m_enc_ctx, &opkt) == 0) {
        double opkt_pts = opkt.pts * av_q2d(m_enc_ctx->time_base);
        if (opkt_pts < m_input_duration_sec) {
            av_packet_rescale_ts(&opkt, m_enc_ctx->time_base, m_out_audio_stream->time_base);
            opkt.stream_index = m_out_audio_stream->index;
            av_interleaved_write_frame(m_ofmt_ctx, &opkt);
        }
        av_packet_unref(&opkt);
    }
    av_frame_unref(frame_resampled);
}

void AudioMuter::release_resources() {
    if (m_swr) swr_free(&m_swr);
    if (m_enc_ctx) avcodec_free_context(&m_enc_ctx);
    if (m_dec_ctx) avcodec_free_context(&m_dec_ctx);
    if (m_ofmt_ctx) {
        if (m_ofmt_ctx->pb) avio_close(m_ofmt_ctx->pb);
        avformat_free_context(m_ofmt_ctx);
    }
    if (m_ifmt_ctx) avformat_close_input(&m_ifmt_ctx);
}

AudioMuter::AudioMuter(const char* in_file, const char* out_file, MuteSection* mute_sections, int mute_count)
    : m_in_file(in_file), m_out_file(out_file),
      m_mute_sections(mute_sections), m_mute_count(mute_count),
      m_ifmt_ctx(nullptr), m_ofmt_ctx(nullptr),
      m_audio_idx(-1), m_video_idx(-1), m_out_video_idx(-1),
      m_in_audio_stream(nullptr), m_dec_ctx(nullptr), m_decoder(nullptr),
      m_enc_ctx(nullptr), m_encoder(nullptr), m_out_audio_stream(nullptr),
      m_swr(nullptr), m_input_duration_sec(0.0)
{
    avformat_network_init();
    set_ffmpeg_log_level();
}

AudioMuter::~AudioMuter() {
    release_resources();
}

int AudioMuter::run() {
    if (init_decoder() < 0 || init_encoder() < 0 || init_swr() < 0)
        return -1;

    if (avio_open(&m_ofmt_ctx->pb, m_out_file, AVIO_FLAG_WRITE) < 0) {
        fprintf(stderr, "打开输出文件失败\n");
        return -1;
    }

    AVDictionary* opts = NULL;
    av_dict_set(&opts, "strict", "-2", 0);
    
    int ret = avformat_write_header(m_ofmt_ctx, &opts);
    av_dict_free(&opts);

    if (ret < 0) {
        char err_buf[1024] = {0};
        av_strerror(ret, err_buf, sizeof(err_buf));
        fprintf(stderr, "写入文件头失败：%s\n", err_buf);
        return -1;
    }

    AVPacket pkt;
    AVFrame* frame = av_frame_alloc();
    AVFrame* frame_resampled = av_frame_alloc();

    while (av_read_frame(m_ifmt_ctx, &pkt) == 0) {
        if (m_video_idx >= 0 && pkt.stream_index == m_video_idx) {
            av_packet_rescale_ts(&pkt,
                m_ifmt_ctx->streams[m_video_idx]->time_base,
                m_ofmt_ctx->streams[m_out_video_idx]->time_base);
            pkt.stream_index = m_out_video_idx;
            av_interleaved_write_frame(m_ofmt_ctx, &pkt);
            av_packet_unref(&pkt);
            continue;
        }

        if (pkt.stream_index != m_audio_idx) {
            av_packet_unref(&pkt);
            continue;
        }

        if (avcodec_send_packet(m_dec_ctx, &pkt) == 0) {
            while (avcodec_receive_frame(m_dec_ctx, frame) == 0) {
                process_audio_frame(frame, frame_resampled);
            }
        }
        av_packet_unref(&pkt);
    }

    flush_encoder();
    av_write_trailer(m_ofmt_ctx);

    av_frame_free(&frame);
    av_frame_free(&frame_resampled);
    printf("处理完成！\n");
    return 0;
}

int main(int argc, char* argv[]) {
    if (argc != 3) {
        fprintf(stderr, "用法: %s <输入> <输出>\n", argv[0]);
        return -1;
    }

    MuteSection mute_sections[] = {
        {2.0, 4.0},
        {6.0, 8.0}
    };
    int cnt = sizeof(mute_sections)/sizeof(MuteSection);
    AudioMuter muter(argv[1], argv[2], mute_sections, cnt);
    return muter.run();
}
