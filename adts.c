#include "adts.h"

static int _has_adts_header(AVPacket *pkt) 
{
    if (pkt->data[0] == 0xff) {
        if ((pkt->data[1] & 0xf0) == 0xf0) {
            return 1;
        }
    }
    return 0;
}

#define SIZEOFARRAY(A, T) (int)(sizeof(A)/sizeof(T))

static int g_freq[] = { 96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050,
        16000, 12000, 11025, 8000, 7350, 0, 0, -1};

static int _find_sample_index(int freq, int chs)
{
    if (chs > 0) {
        freq /= chs;
    }
    int i;
    for (i = 0; i < SIZEOFARRAY(g_freq, int); i++) {
        if (freq >= g_freq[i]) {
            return i;
        }
    }
    return 15;
}

static int _add_adts_header(int chs, int sample_index, AVPacket *pkt) 
{
    if ((pkt == NULL) || (pkt->size > 2) || _has_adts_header(pkt)) {
        return 0;
    }

    int data_len = pkt->size;
    int length = 7 + data_len;

    char bits[7] = { 0 };
    bits[0] = 0xff;
    bits[1] = 0xf1;
    // set AAC profile: LC
    bits[2] = 0x40 | (sample_index << 2) | (chs >> 2);
    bits[3] = ((chs & 0x3) << 6) | (length >> 11);
    bits[4] = (length >> 3) & 0xff;
    bits[5] = ((length << 5) & 0xff) | 0x1f;
    bits[6] = 0xfc;

    av_buffer_realloc(&pkt->buf, 7 + pkt->buf->size);
    pkt->data = pkt->buf->data;
    pkt->size = length;

    memmove(pkt->data + 7, pkt->data, data_len);
    memcpy(pkt->data, bits, 7);

    return 1;
}

int aac_add_adts_header(AVCodecContext *audio_codec, AVPacket *pkt) 
{
    if ((audio_codec == NULL) || (audio_codec->codec_id != AV_CODEC_ID_AAC)) {
        return 0;
    }
    int chs = audio_codec->channels;
    int sample_index = _find_sample_index(audio_codec->sample_rate, chs);

    return _add_adts_header(chs, sample_index, pkt);
}

int aac_add_adts_header_from_extradata(const uint8_t *buf, int size, AVPacket *pkt)
{
    if ((buf == NULL) || size < 2) {
        return 0;
    }

    int chs = (buf[1] & 0x78) >> 3;
    int sample_index = ((buf[0] & 0x07) << 1) | (buf[1] >> 7);
    if (sample_index == 0xf && size >= 5) {
        chs = (buf[4] & 0x78) >> 3;
    }
    return _add_adts_header(chs, sample_index, pkt);
}

