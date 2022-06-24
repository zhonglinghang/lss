#ifndef FLV_SEG_H_
#define FLV_SEG_H_

#include "seg.h"
#include "srs_librtmp.h"

static char FLV_HEADER[9] = {'F', 'L', 'V', 0x01, 0x05, 0x0, 0x0, 0x0, 0x09};
static char HDS_HEADER[8] = {0x0, 0x0, 0x0, 0x0, 'm', 'd', 'a', 't'};

#define HDS_FRAG_WINDOW_SIZE 24
#define FLV_MAX_FRAMES_PER_SECOND 500
#define FLV_MAX_FRAMES_TH 2000

typedef struct flv_reference_internal {
    int reference;
} flv_reference_internal;

typedef struct flv_referenced_packet {
    flv_reference_internal *internal;

    char *packet_buf;
    int packet_size;
    char packet_type;
    u_int32_t packet_time;

    int is_seq_header;
} flv_referenced_packet;

typedef struct frag_info_t {
    int duration;
    u_int64_t start_time;
    int index;

    struct frag_info_t *next;
} frag_info;

typedef struct flv_interleaved_packet {
    flv_referenced_packet content;

    struct flv_interleaved_packet *prev;
    struct flv_interleaved_packet *next;

#if 0
    struct flv_interleaved_packet *prev_in_stream;
    struct flv_interleaved_packet *next_in_stream;
#endif
} flv_interleaved_packet;

#define MAX_N_BUFFER_VIDEO (7)
#define MAX_N_BUFFER_AUDIO (15)

// show it and compare value.
#define FLV_ROLLBACK_LARGE_THRESHOLD ((((int64_t)1) << 31) - 60000)
#define FLV_ROLLBACK_SMALL_THRESHOLD (60000)

#define FLV_MAX_TS (((int64_t)1) << 31)

// 1s
#define FLV_TS_TOO_LARGE_OFFSET (1000)

typedef struct flv_stream_info_t {
    u_int32_t last_dts;
    // exist == 0 means no packet for this stream
    int8_t exist;
    int8_t met_first;
    int8_t last_is_seq_header;

#if 0
    flv_interleaved_packet *interleave_buffer;
    flv_interleaved_packet *interleave_buffer_end;
#endif
    // limited by max buffer number.
    int n_buffer;
} flv_stream_info_t;

#define FLV_LIVE_COMMON_FLAG_NONE (0)
#define FLV_LIVE_COMMON_FLAG_PRINTED_NO_SEQ_HEADER (1 << 0)
#define FLV_LIVE_COMMON_FLAG_PRINTED_VIDEO_CODEC_ID (1 << 1)
#define FLV_LIVE_COMMON_FLAG_PRINTED_AUDIO_CODEC_ID (1 << 1)

#define STREAM_INFO_IDX_VIDEO (0)
#define STREAM_INFO_IDX_AUDIO (1)
#define STREAM_INFO_IDX_METADATA (2)

#define STREAM_INFO_TOTAL_NUM (3)

typedef struct {
    u_int32_t start_time;
    u_int32_t init_time;

    flv_stream_info_t stream_info[STREAM_INFO_TOTAL_NUM];

    int is_first_frame;

    flv_referenced_packet avc_sh_buf;
    flv_referenced_packet aac_sh_buf;
    flv_referenced_packet metadata_buf;

    flv_referenced_packet curr_pkt;

    int hds_frag_count;
    frag_info *frag_list;

    char base_stream_type;

    int common_flag;

    int got_first_content;

    flv_interleaved_packet *interleave_buffer;
    flv_interleaved_packet *interleave_buffer_end;
} flv_context_t;

int flv_seg_run(SegHandler *sh);

#endif