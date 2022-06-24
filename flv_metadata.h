#ifndef FLV_METADATA_H_
#define FLV_METADATA_H_

#include <libavformat/avformat.h>
#include "seg.h"

enum {
    FLV_CODECID_H263 = 2,
    FLV_CODECID_SCREEN = 3,
    FLV_CODECID_VP6 = 4,
    FLV_CODECID_VP6A = 5,
    FLV_CODECID_SCREEN2 = 6,
    FLV_CODECID_H264 = 7,
    FLV_CODECID_REALH263 = 8,
    FLV_CODECID_MPEG4 = 9,
};

int get_video_codec_id(int flv_codecid);

enum {
    FLV_CODECID_PCM = 0,
    FLV_CODECID_ADPCM = 1,
    FLV_CODECID_MP3 = 2,
    FLV_CODECID_PCM_LE = 3,
    FLV_CODECID_NELLYMOSER_16KHZ_MONO = 4,
    FLV_CODECID_NELLYMOSER_8KHZ_MONO = 5,
    FLV_CODECID_NELLYMOSER = 6,
    FLV_CODECID_PCM_ALAW = 7,
    FLV_CODECID_PCM_MULAW = 8,
    FLV_CODECID_AAC = 10,
    FLV_CODECID_SPEEX = 11,
};

typedef struct {
    int videocodecid;
    int audiocodecid;
    int width;
    int height;
    int64_t cyclebasetime;
    int64_t filename_basetime;
    int64_t live_publish_timestamp;
} FlvMetadata;

int probe_flv_metadata(AVIOContext *io, SegHandler *sh, FlvMetadata *meta);

#endif