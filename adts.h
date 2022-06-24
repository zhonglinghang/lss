#ifndef ADTS_H_
#define ADTS_H_

#include "libavformat/avformat.h"
#include "libavcodec/avcodec.h"
#include "libavutil/avutil.h"

int aac_add_adts_header(AVCodecContext *audio_codec, AVPacket *pkt);

int aac_add_adts_header_from_extradata(const uint8_t *buf, int size, AVPacket *pkt);

#endif /* ADTS_H_ */