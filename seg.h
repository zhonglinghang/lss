#ifndef SEG_H_
#define SEG_H_

#include "flv_amf_common.h"
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/time.h>

#define TIMEOUT 10000000 //us

#define NONSEG_BUF_TIME 1000000

// error code
#define EC_OK            0
#define EC_FAIL          1
#define EC_MEM           2
#define EC_OPEN_FAIL     3
#define EC_STREAM_ERR    4
#define EC_READ_FAIL     5
#define EC_OUTPUT_FAIL   6
#define EC_TS_ERR        7
#define EC_UNEXP_STREAM  8
#define EC_NO_KEYFRAME   9

// notify flags
#define NF_WRITE_ERROR   0x00000001
#define NF_FILTER_ERROR  0x00000002
#define NF_KEYF_WARN     0x00000004
#define NF_ADTS_WARN     0x00000008
#define NF_DTS_WARN      0x00000010
#define NF_TOO_LONG      0x00000020
#define NF_PTS_WARN      0x00000040
#define NF_EXTSEQ_WARN   0x00000080
#define NF_NO_VIDEO      0x00010000
#define NF_NO_AUDIO      0x00020000
#define NF_NEWEXTRADATA  0x10000000

struct SegHandler;

#define MAX_N_METAKEYS 4
#define MAX_LEN_METAKEY 32
#define MAX_LEN_METAKEY_CONTENT 64

typedef enum {
    METAKEY_UPDATE_ONCE = 0,
    METAKEY_UPDATE_SEG_FIRST,
    METAKEY_UPDATE_SEG_LAST,
    METAKEY_UPDATE_LATEST
} MetaKeyUpdateType;

#define METAKEY_FLAG_NO_NOTIFY (1 << 0)
#define METAKEY_FLAG_RTC_BASE_TIME (1 << 1)

typedef struct {
    char key[MAX_LEN_METAKEY];
    AMFDataType type;
    MetaKeyUpdateType update_type;
    int flag;
} MetaKeyDesc;

union MetaValue {
    // number
    int64_t i64;
    // string
    char str[MAX_LEN_METAKEY_CONTENT];
};

#define METAKEY_VAL_STAT_INIT (0)
#define METAKEY_VAL_STAT_GOT (1)
#define METAKEY_VAL_STAT_SENT (2)

typedef struct {
    const MetaKeyDesc *desc;
    union MetaValue value;
    int status;
} MetaKeyInfo;

#define OUTPUT_NONINTERLEAVED_NORMAL (0)
#define OUTPUT_NONINTERLEAVED_NIL (1)
#define OUTPUT_NONINTERLEAVED_CLR (2)

#define FLV_SEG_FLAGS_NONE (0)
#define FLV_SEG_FLAGS_ALIGN_DTS (1)
#define FLV_SEG_FLAGS_INTERLEAVE_PKTS (1 << 1)

typedef struct {
    const char *tid;
    const char *url;
    const char *name;
    const char *logdir;
    const char *nurl;
    const char *continue_abst;
    int duration;
    int duration_ms;
    int chunk_duration_ms;
    int maxframes;
    void (*notify)(struct SegHandler *sh, int last);
    void (*chunk_notify)(struct SegHandler *sh);
    void (*timer) (struct SegHandler *sh);
    int test_vcid;
    int align;
    int seq_sync;
    int flv_meta;
    int is_hds;
    int is_rptp;
    int start_number;
    int only_audio;
    int only_video;
    int only_has_audio;
    int only_has_video;
    int skip_video_complement;
    int output_absolute_timestamp;
    int output_noninterleaved;
    int workaround_cra;
    int workaround_hevcaud;
    int workaround_h264aud;
    int copyts;
    int is_lhls;
    // when both audio and video ext changed, force seg and notify discontinue
    int seg_on_ext;
    // for lhls and llhls, segment chunk by dts rather than pts, 
    // in order trying to fix the problem chunk duration must be over 85% of target duration when B frames exist.
    int llhls_seg_by_dts;
    // llhls notify independent chunks
    int llhls_notify_independent;
    int flv_seg_flags;
    // fixed gop duration, use fixed gop mode if larger than zero.
    int probe_gop;
    // chunk duration range
    int chunk_duration_lower_ms;
    int chunk_duration_higher_ms;
    int do_judge_discontinuity;
    const char *custom_metakey;
    MetaKeyDesc metakey_desc[MAX_N_METAKEYS];
} SegParams;

#define MAX_STREAMS 8
#define MAX_STREAMS_PLUSONE 9
#define MAX_EXT_SEQHEAD_SIZE 2047

#define EXTSEG_MAX_CACHES MAX_STREAMS_PLUSONE

typedef struct {
    AVStream *in_stream;
    AVStream *out_stream;
    int64_t count;
    int64_t idts;
    int64_t odts;
    int64_t duration;
    uint8_t ext_seqhead[MAX_EXT_SEQHEAD_SIZE + 1];
    int ext_seqhead_size;
} StreamInfo;

typedef struct {
    int64_t in_frames;
    int64_t out_frames;
    int64_t in_bytes;
    int64_t out_bytes;
    int64_t timestamp;
} StreamStatis;

typedef struct {
    char live_streamid[1024];
    char via[1024];
    char vcodec[1024];
    char acodec[1024];
    int64_t connected_time;
    int64_t first_frame_time;
    int64_t first_frame_pts;
    int64_t last_keyframe_count;
    int64_t gop;
    StreamStatis vss;
    StreamStatis ass;
    enum AVMediaType last_pkt_type;
    int last_pkt_size;
} SegStatis;

typedef struct {
    int video_width;
    int video_height;
    int input_video_frames;
    int input_audio_frames;
    int output_video_frames;
    int output_audio_frames;
} SegData;

typedef struct {
    int input_video_frames;
    int input_audio_frames;
    int output_video_frames;
    int output_audio_frames;
} ChunkData;

typedef struct AVPktCache {
    AVPacket *cache;

    struct AVPktCache *next;
    int seg_flags; // seghandler flags
    int is_base_missing; // base could come back
} AVPktCache;

#define SEG_CACHECTX_STAGE_NONE (0)
#define SEG_CACHECTX_STAGE_CACHE (1)
#define SEG_CACHECTX_STAGE_CACHE2FLUSH (2)
#define SEG_CACHECTX_STAGE_FLUSH (3)

typedef struct {
    AVPktCache *pkt_caches;
    AVPktCache *pkt_caches_end;

    int n_caches;
    int8_t audio_cached;
    int8_t video_cached;

    int8_t stage;
    int8_t need_seg;
} SegCacheContext;

#define CURR_CHUNK_FLAG_NONE (0)
#define CURR_CHUNK_FLAG_KEY_IN_HEAD (1 << (0))
#define CURR_CHUNK_FLAG_GOT_FIRST_PKT (1 << (1))

#define PROBE_GOP_FLAG_INIT (0)
#define PROBE_GOP_FLAG_GOT_FIRST_PKT (1)
#define PROBE_GOP_FLAG_GOT_FIRST_KEY (2)
#define PROBE_GOP_FLAG_DONE (3)

#define STREAM_FLAGS_NONE (0)
#define STREAM_FLAGS_HAS_VIDEO (1 << (0))
#define STREAM_FLAGS_HAS_AUDIO (1 << (1))
#define STREAM_FLAGS_HAS_OTHERS (1 << (2))

#define BASE_MISSING_TRIGGER_NONE (0)
#define BASE_MISSING_TRIGGER_TO_MISSING (1 << (0))
#define BASE_MISSING_TRIGGER_BACK (1 << (1))

typedef struct SegHandler {
    SegParams params;
    char file[1024];
    char hds_abst_file[1024];
    AVFormatContext *ic;
    AVFormatContext *oc;
    AVBitStreamFilterContext *bsfc;
    int interrupt;
    int index;
    int seg_index;
    int chunk_index;
    int flags;
    int base_stream_index;
    int aac_ever_changed;
    int64_t actived;
    int64_t begin;
    int64_t chunk_begin;
    int64_t duration;
    int64_t chunk_duration;
    int64_t chunk_start;
    int64_t chunk_end;
    int64_t count;
    int64_t wait_keyframe_count;
    int64_t write_fail_count;
    int64_t audio_samples;
    int64_t align_flag;
    StreamInfo streams[MAX_STREAMS];
    SegStatis statis;
    SegData seg_data;
    ChunkData chunk_data;

    int64_t cycle_base_time;
    int64_t next_cycle_base_time;

    int64_t filename_base_time;
    int64_t next_filename_base_time;

    int64_t first_key_pts;
    int64_t first_nonbase_pts;
    int64_t first_nonkey_pts;

    int64_t seg_start_dts;

    int64_t live_publish_timestamp;
    int64_t pts_rollback_flag;

    int rptp_is_keyframe;
    int rptp_is_metadata;
    u_int32_t rptp_pts;

    // pes encryption handle
    int need_encrypt;
    void *pes_encryption;
    char pes_encryption_key[16];

    int nonbase_count;
    int is_base_missing;

    int bsf_error_count;

    SegCacheContext seg_cache_ctx;
    int insert_discontinuity;

    int8_t curr_chunk_flag;

    int8_t probe_gop_flag;

    int stream_flags;

    int64_t probe_gop_analyze_start_dts;
    int64_t probe_gop_first_key_dts;

    int probed_base_stream_gop;
    int probed_base_stream_gop_packets;

    int planned_chunk_duration;
    int planned_chunk_n_packets;

    int chunk_base_packets_count;

    int8_t last_lost_video;
    int8_t last_lost_audio;
    int8_t discontinuity_before;

    // usage see comments in write_output_frame.
    int8_t base_missing_trigger;

    int n_metakey;
    MetaKeyInfo metakey_info[MAX_N_METAKEYS];
} SegHandler;

void seg_init(SegHandler *sh, const SegParams *sp);
void seg_uninit(SegHandler *sh, SegParams *sp);
int seg_run(SegHandler *sh);
void seg_stop(SegHandler *sh);

#endif