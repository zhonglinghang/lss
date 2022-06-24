#ifndef SEG_COMMON_H_
#define SEG_COMMON_H_

#include "seg.h"
#include "log.h"

#define PASSEDTIME_LIMIT 60000000 // 60s
#define IDTSOFFSET_DIFF 100000 // 100ms

int read_input_frame(SegHandler *sh, AVPacket *pkt);
int check_input_timestamp(SegHandler *sh, AVPacket *pkt);
int check_duration(SegHandler *sh, AVPacket *pkt);
int check_ts_chunk_duration(SegHandler *sh, AVPacket *pkt);
int check_ext_seqhead(SegHandler *sh, AVPacket *pkt);
int meet_first_keyframe(SegHandler *sh, AVPacket *pkt);
int do_stream_filters(SegHandler *sh, AVPacket *pkt);

int set_output_timestamp(SegHandler *sh, AVPacket *pkt);
int set_output_stream_index(SegHandler *sh, AVPacket *pkt);
int write_output_frame(SegHandler *sh, AVPacket *pkt);

void check_timestamp_rollback(SegHandler *sh, AVPacket *pkt);

void statis_on_connected(SegHandler *sh);
void statis_on_frame_input(SegHandler *sh, const AVPacket *pkt);
void statis_on_frame_output(SegHandler *sh);

int check_align(SegHandler *sh, int64_t pts);
int check_ext_seqhead_changed(SegHandler *sh, AVPacket *pkt);

void seg_cachectx_update_status(SegHandler *sh, AVPacket *pkt);

int seg_cachectx_cache_pkt(SegHandler *sh, AVPacket *pkt, int max_caches);

int seg_cachectx_pop_and_restore_ctx(SegHandler *sh, AVPacket *pkt);

void seg_cachectx_clear_caches(SegHandler *sh);

inline static StreamInfo *get_stream_info(SegHandler *sh, const AVPacket *pkt) 
{
    return &sh->streams[pkt->stream_index];
}

inline static StreamInfo *get_base_stream_info(SegHandler *sh) 
{
    return &sh->streams[sh->base_stream_index];
}

inline static AVStream *get_input_stream(SegHandler *sh, const AVPacket *pkt) 
{
    return sh->streams[pkt->stream_index].in_stream;
}

inline static AVStream *get_output_stream(SegHandler *sh, const AVPacket *pkt) 
{
    return sh->streams[pkt->stream_index].out_stream;
}

inline static int is_base_stream(SegHandler *sh, const AVPacket *pkt) 
{
    return (pkt->stream_index == sh->base_stream_index);
}

inline static void frame_count_increase(SegHandler *sh, const AVPacket *pkt) 
{
    sh->count++;
    sh->streams[pkt->stream_index].count++;
}

inline static void frame_trace_log(SegHandler *sh, const AVPacket *pkt, const char *phase) 
{
    logger(LOG_VERB, "TRACE<%s> [%d:%lld] %lld / %lld size = %d", phase, pkt->stream_index, 
        sh->streams[pkt->stream_index].count, pkt->dts, pkt->pts, pkt->size);
}

#endif