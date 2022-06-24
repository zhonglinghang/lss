#include "seg_common.h"
#include "hevc_patch.h"
#include "ext_seqhead.h"
#include "adts.h"
#include <libavutil/intreadwrite.h>

#define TRACE_FRAME frame_trace_log(sh, pkt, __FUNCTION__)

static void copy_extradata(AVCodecContext *codec, uint8_t *side_data, 
    int side_size) 
{
    if (codec->extradata && codec->extradata_size > 0) {
        av_freep(&codec->extradata);
    }
    codec->extradata = av_mallocz(side_size);
    memcpy(codec->extradata, side_data, side_size);
    codec->extradata_size = side_size;
}

static void copy_extradata_new(AVCodecParameters *codec, uint8_t *side_data, int side_size)
{
    if (codec->extradata && codec->extradata_size > 0) {
        av_freep(&codec->extradata);
    }
    codec->extradata = av_mallocz(side_size);
    memcpy(codec->extradata, side_data, side_size);
    codec->extradata_size = side_size;
}

int read_input_frame(SegHandler *sh, AVPacket *pkt) 
{
    int ret = av_read_frame(sh->ic, pkt);
    if (ret < 0) {
        av_error("av_read_frame", ret);
        return -1;
    }

    AVStream *istream = get_input_stream(sh, pkt);
    AVStream *ostream = get_output_stream(sh, pkt);

    // patch for hevc if not annexb mode
    if (istream && istream->codec->codec_id == AV_CODEC_ID_HEVC) {
        if (!hevc_patch_is_annexb(pkt)) {
            if(sh->params.workaround_cra) {
                if(hevc_patch_hvcc_is_keyframe_ignore_cra(pkt)) {
                    pkt->flags |= AV_PKT_FLAG_KEY;
                } else {
                    pkt->flags &= ~AV_PKT_FLAG_KEY;
                }
            } else {
                if(hevc_patch_hvcc_is_keyframe(pkt)) {
                    pkt->flags |= AV_PKT_FLAG_KEY;
                } else {
                    pkt->flags &= ~AV_PKT_FLAG_KEY;
                }
            }
        }
    }

    sh->actived = av_gettime_relative();
    // sh->base_missing_trigger = BASE_MISSING_TRIGGER_NONE;
    // count non base streams's packet
    if(is_base_stream(sh, pkt)) {
        sh->nonbase_count = 0;
        if (sh->is_base_missing) {
            logger(LOG_WARN, "base stream is back");
            sh->is_base_missing = 0;
            // reset output dts
            StreamInfo * stream = get_stream_info(sh, pkt);
            stream->odts = -1;
        }
    } else {
        sh->nonbase_count++;
        // if comes 100 non-base stream's packets continuously
        // it means base stream is missing
        if (sh->nonbase_count > 100) {
            if(!sh->is_base_missing) {
                logger(LOG_WARN, "base stream is missing");
                sh->is_base_missing = 1;
            }
        }
    }

    sh->flags &= ~NF_NEWEXTRADATA;

    av_packet_split_side_data(pkt);
    int side_size = 0;
    uint8_t *side_data = av_packet_get_side_data(pkt, AV_PKT_DATA_NEW_EXTRADATA, &side_size);
    if (side_size > 0) {
        if(!side_size != istream->codec->extradata_size || memcmp(side_data, istream->codec->extradata, side_data)) {
            logger(LOG_WARN, "stream[%d] extradata changed! new/old = %d/%d", pkt->stream_index, 
                side_size, istream->codec->extradata_size);
            logger_binary(LOG_WARN, "new extradata", side_data, side_size);
            if (ostream != NULL) {
                copy_extradata(ostream->codec, side_data, side_size);
            }
            sh->flags |= NF_NEWEXTRADATA;
        }
    }

    return 0;
}

int check_input_timestamp(SegHandler *sh, AVPacket *pkt)
{
    TRACE_FRAME;
    StreamInfo *stream = get_stream_info(sh, pkt);
    AVStream *istream = get_input_stream(sh, pkt);
    int64_t cur_pkt_dts = pkt->dts;
    if(pkt->dts > stream->odts) {
        int64_t passed = pkt->dts - stream->odts;
        passed = av_rescale_q(passed, istream->time_base, AV_TIME_BASE_Q);
        if(passed > PASSEDTIME_LIMIT) {
            if (stream->odts < 0) {
                logger(LOG_WARN, "stream[%d] start DTS = %lld", istream->index, pkt->dts);
                stream->odts = pkt->dts;
            } else {
                logger(LOG_ERROR, "stream[%d] DTS passed too much: %lld - %lld", istream->index, pkt->dts, stream->odts);
                return -1;
            }
        }
    } else {
        int64_t duration = pkt->dts - stream->idts;
        if (duration <= 0) {
            if (pkt->dts == stream->idts) {
                logger(LOG_WARN, "stream[%d] SAME DTS", istream->index);
            } else {
                logger(LOG_WARN, "stream[%d] DST current[%lld] < previous[%lld]", istream->index, pkt->dts, stream->idts);
            }
            sh->flags |= NF_DTS_WARN;
            duration = 1;
        }
        int64_t composition_time = pkt->pts - pkt->dts;
        pkt->dts = stream->odts + duration;
        pkt->pts = pkt->dts + composition_time;
    }
    // modify PTS if abnormal
    if (pkt->pts < pkt->dts) {
        logger(LOG_WARN, "stream[%d] PTS < DTS, force adjust", istream->index);
        sh->flags |= NF_PTS_WARN;
        pkt->pts = pkt->dts;
    }

    stream->idts = cur_pkt_dts;
    stream->duration = pkt->dts - stream->odts;
    stream->odts = pkt->dts;

    // check offset compare to base stream
    StreamInfo *base_stream = get_base_stream_info(sh);
    int64_t input_offset = stream->idts - base_stream->idts;
    int64_t output_offset = stream->odts - base_stream->odts;
    int64_t offset_diff = abs(input_offset - output_offset);
    offset_diff = av_rescale_q(offset_diff, istream->time_base, AV_TIME_BASE_Q);
    if (offset_diff > IDTSOFFSET_DIFF) {
        logger(LOG_ERROR, "DTS I/O offset too large: In[%lld-%lld] Out[%lld-%lld]", 
            stream->idts, base_stream->idts, stream->odts, base_stream->odts);
        return -1;
    }
    return 0;
}

// if need to cut, return -1, else 0
int check_align(SegHandler *sh, int64_t pts)
{
    int64_t flag;

    if (sh->filename_base_time) {
        pts += sh->filename_base_time * 1000;
    } else {
        pts += sh->cycle_base_time * 1000;
    }

    if (sh->params.duration == 0) {
        flag = 0;
    } else {
        flag = pts / (sh->params.duration * 1000000);
    }

    logger(LOG_DEBUG, "check align %ld %ld %ld %ld", sh->params.duration, pts, flag, sh->align_flag);
    if (sh->align_flag < 0) {
        sh->align_flag = flag;
    }

    if (sh->align_flag != flag) {
        sh->align_flag = flag;
        if(sh->params.seq_sync) {
            sh->index = flag;
        }
        return -1;
    }
    return 0;
}

// return -1 if need cut
int check_ext_seqhead_changed(SegHandler *sh, AVPacket *pkt)
{
    AVStream *istream = get_input_stream(sh, pkt);
    int ext_size = 0;
    int pos = ext_seqhead_search(istream->codec->extradata, istream->codec->extradata_size, &ext_size);
    if(pos < 0) {
        return 0;
    }
    uint8_t *ext_seqhead = istream->codec->extradata + pos;
    StreamInfo *si = get_stream_info(sh, pkt);
    if (sh->count > 0) {
        if(ext_size != si->ext_seqhead_size || memcmp(si->ext_seqhead, ext_seqhead, ext_size)) {
            logger(LOG_WARN, "stream[%d] ext_seqhead changed", pkt->stream_index);
            return -1;
        }
    }
    return 0;
}

int check_duration(SegHandler *sh, AVPacket *pkt)
{
    TRACE_FRAME;

    if(is_base_stream(sh, pkt) || sh->is_base_missing || sh->seg_cache_ctx.need_seg) {
        AVStream *istream = get_input_stream(sh, pkt);
        int64_t pts = av_rescale_q(pkt->pts, istream->time_base, AV_TIME_BASE_Q);
        int64_t dts = av_rescale_q(pkt->dts, istream->time_base, AV_TIME_BASE_Q);
        if(sh->begin < 0) {
            sh->begin = pts;
        } 
        logger(LOG_DEBUG, "pts = %lld, begin = %lld", pts, sh->begin);
        sh->duration = pts - sh->begin;

        // need_seg = 1 have ensured key video frame is met beforehands
        if (sh->seg_cache_ctx.need_seg) {
            sh->seg_cache_ctx.need_seg = 0;
            sh->insert_discontinuity = 1;
            sh->begin = pts;
            if(sh->params.is_lhls) {
                if(sh->params.llhls_seg_by_dts) {
                    sh->chunk_begin = dts;
                } else {
                    sh->chunk_begin = pts;
                }
            }
            return -1;
        }

        // check ext_seqhead whether changed first
        if (check_ext_seqhead_changed(sh, pkt) < 0) {
            logger(LOG_WARN, "ext_seqhead changed, force cut!");
            sh->begin = pts;
            if (sh->params.is_lhls) {
                if(sh->params.llhls_seg_by_dts) {
                    sh->chunk_begin = dts;
                } else {
                    sh->chunk_begin = pts;
                }
            }
            return -1;
        }

        if (pkt->flags & AV_PKT_FLAG_KEY) {
            if (sh->params.align) {
                // check_input_timestamp is done before check_duration, so dts is increasing
                int64_t cur_dts = av_rescale_q(pkt->dts, istream->time_base, AV_TIME_BASE_Q);
                // for lhls, buf_time is not used to avoid non even segment duration.
                // todo: may just delete NONSEG_BUF_TIME later? it is used for segment after transcode.
                // which may send metadata twice but actually not happening.
                if (!sh->params.is_lhls && cur_dts - sh->seg_start_dts < NONSEG_BUF_TIME) {
                    logger(LOG_WARN, "cur_dts[%lld] - seg_start_dts[%lld] within %lld, no seg judge!", cur_dts, sh->seg_start_dts, NONSEG_BUF_TIME);
                } else if (check_align(sh, pts) == -1) {
                    sh->begin = pts;
                    if (sh->params.is_lhls) {
                        if(sh->params.llhls_seg_by_dts) {
                            sh->chunk_begin = dts;
                        } else {
                            sh->chunk_begin = pts;
                        }
                    }
                    return -1;
                }
            } else if (sh->duration >= sh->params.duration * 1000000 + sh->params.duration_ms * 1000) {
                sh->begin = pts;
                if (sh->params.is_lhls) {
                    if (sh->params.llhls_seg_by_dts) {
                        sh->chunk_begin = dts;
                    } else {
                        sh->chunk_begin = pts;
                    }
                }
                return -1;
            }
        }
    }

    // bugfix : avoid too long ts slice
    if (sh->count >= sh->params.maxframes) {
        AVStream *istream = get_input_stream(sh, pkt);
        int64_t pts = av_rescale_q(pkt->pts, istream->time_base, AV_TIME_BASE_Q);
        int64_t dts = av_rescale_q(pkt->dts, istream->time_base, AV_TIME_BASE_Q);
        sh->flags |= NF_TOO_LONG;
        logger(LOG_WARN, "too many frames, force cut!");
        sh->begin = pts;
        if (sh->params.is_lhls) {
            if (sh->params.llhls_seg_by_dts) {
                sh->chunk_begin = dts;
            } else {
                sh->chunk_begin = pts;
            }
        }
        return -1;
    }
    return 0;
}

int check_ts_chunk_duration(SegHandler *sh, AVPacket *pkt) 
{
    if(is_base_stream(sh, pkt) || sh->is_base_missing) {
        AVStream *istream = get_input_stream(sh, pkt);
        int64_t pts = av_rescale_q(pkt->pts, istream->time_base, AV_TIME_BASE_Q);
        int64_t dts = av_rescale_q(pkt->dts, istream->time_base, AV_TIME_BASE_Q);
        if (sh->chunk_begin < 0) {
            if (sh->params.llhls_seg_by_dts) {
                sh->chunk_begin = dts;
            } else {
                sh->chunk_begin = pts;
            }
        }

        if(sh->params.llhls_seg_by_dts) {
            logger(LOG_DEBUG, "dts = %lld, begin = %lld", dts, sh->chunk_begin);
            sh->chunk_begin = dts - sh->chunk_begin;
        } else {
            logger(LOG_DEBUG, "pts = %lld, begin = %lld", pts, sh->chunk_begin);
            sh->chunk_begin = dts - sh->chunk_begin;
        }
        // logger(LOG_DEBUG, "chunk_duration_ms is %lld", sh->params.chunk_duration_ms);

        if (sh->planned_chunk_n_packets > 0) {
            if (sh->chunk_base_packets_count >= sh->planned_chunk_n_packets ||
                sh->chunk_duration >= sh->planned_chunk_duration * 1000) {
                if (sh->params.llhls_seg_by_dts) {
                    sh->chunk_begin = dts;
                } else {
                    sh->chunk_begin = pts;
                }
                return -1;
            }

            if(is_base_stream(sh, pkt)) {
                sh->chunk_base_packets_count ++;
            } 
        } else {
            // align mode for ts chunk is not supported yet.
            // if to support, remember to check audio size, may not always be flushed
            if (sh->chunk_duration >= sh->params.chunk_duration_ms * 1000) {
                if (sh->params.llhls_seg_by_dts) {
                    sh->chunk_begin = dts;
                } else {
                    sh->chunk_begin = pts;
                }
                return -1;
            }
        }
    }
    return 0;
}

void check_timestamp_rollback(SegHandler *sh, AVPacket *pkt) 
{
    if (!is_base_stream(sh, pkt)) {
        return;
    }
    AVStream *ostream = get_output_stream(sh, pkt);
    if (ostream == NULL) {
        return;
    } else {
        AVStream *istream = get_input_stream(sh, pkt);
        int64_t new_pts_rollback_flag = av_rescale_q(pkt->pts, istream->time_base, ostream->time_base);
        new_pts_rollback_flag = new_pts_rollback_flag >> 33;
        //todo: check over 46 days;
        if (sh->pts_rollback_flag < 0) {
            sh->pts_rollback_flag = new_pts_rollback_flag;
        }
        if (new_pts_rollback_flag != sh->pts_rollback_flag) {
            logger(LOG_WARN, "timestamp rollback happened. (flag %lld vs %lld)", new_pts_rollback_flag, sh->pts_rollback_flag);
            sh->pts_rollback_flag = new_pts_rollback_flag;
        }
    }
}

int check_ext_seqhead(SegHandler *sh, AVPacket *pkt)
{
    AVStream *istream = get_input_stream(sh, pkt);
    int ext_size = 0;
    int pos = ext_seqhead_search(istream->codec->extradata, istream->codec->extradata_size, &ext_size);
    if(pos < 0) {
        return 0;
    }
    if (ext_size > MAX_EXT_SEQHEAD_SIZE) {
        logger(LOG_ERROR, "ext_seqhead_size(%d) too larget", ext_size);
        ext_size = MAX_EXT_SEQHEAD_SIZE;
    }
    uint8_t *ext_seqhead = istream->codec->extradata + pos;
    StreamInfo *si = get_stream_info(sh, pkt);
    if (ext_size != si->ext_seqhead_size || memcmp(si->ext_seqhead, ext_seqhead, ext_size)) {
        memcpy(si->ext_seqhead, ext_seqhead, ext_size);
        si->ext_seqhead[ext_size] = 0;
        si->ext_seqhead_size = ext_size;
        logger(LOG_WARN, "stream[%d] ext_seqhead[%d]: %s", pkt->stream_index, si->ext_seqhead_size, si->ext_seqhead);
    }
    if (!is_base_stream(sh, pkt)) {
        StreamInfo *base_si = get_base_stream_info(sh);
        if (ext_size != base_si->ext_seqhead_size || memcmp(base_si->ext_seqhead, ext_seqhead, ext_size)) {
            logger(LOG_WARN, "stream[%d] ext_seqhead NOT same with base stream.", pkt->stream_index);
            sh->flags |= NF_EXTSEQ_WARN;
            return -1;
        }
    }
    return 0;
}

// since meet_first_keyframe is called before set_output_timestamp, dts is not updated to absolute value yet.
int meet_first_keyframe(SegHandler *sh, AVPacket *pkt) 
{
    TRACE_FRAME;

    if (sh->count == 0) {
        if (is_base_stream(sh, pkt) && (pkt->flags * AV_PKT_FLAG_KEY)) {
            logger(LOG_VERB, "count key frame from base stream");
            AVStream *istream = get_input_stream(sh, pkt);
            sh->first_key_pts = av_rescale_q(pkt->pts, istream->time_base, (AVRational){1, 1000});
        } else if (sh->is_base_missing) {
            logger(LOG_VERB, "allow non base stream at the begining");
            AVStream *istream = get_input_stream(sh, pkt);
            sh->first_nonbase_pts = av_rescale_q(pkt->pts, istream->time_base, (AVRational){1, 1000});
        } else {
            logger(LOG_WARN, "discard this frame until key frame");
            sh->flags |= NF_KEYF_WARN;
            if (sh->first_nonkey_pts == -1) {
                AVStream *istream = get_input_stream(sh, pkt);
                sh->first_nonkey_pts = av_rescale_q(pkt->pts, istream->time_base, (AVRational){1, 1000});
            }
            return -1;
        }
    }
    return 0;
}

static void strip_AVCC_AUD(AVPacket *pkt)
{
    const uint8_t AVCC_AUD[] = {0, 0, 0, 2, 0x09, 0xF0};
    if (pkt->size > 6 && 0 == memcmp(pkt->data, AVCC_AUD, 6)) {
        memmove(pkt->data, pkt->data + 6, pkt->size - 6);
        pkt->size -= 6;
    }
}

static void strip_empty_nalu(AVPacket *pkt)
{
    int last_end_plusone = -1;
    if (pkt->size > 3 && pkt->data[0] == 0 && pkt->data[1] == 0 &&
        pkt->data[2] == 1) {
        last_end_plusone = 3;
    }
    int cur = 1;
    int size = pkt->size;
    while(cur < size - 2) {
        if (pkt->data[cur] == 0 &&
            pkt->data[cur+1] == 0 &&
            pkt->data[cur+2] == 1) {
            int offset = 0;
            if (pkt->data[cur-1] == 0) {
                offset = 1;
            }
            int tmp = cur - offset;
            if (tmp == last_end_plusone) {
                logger(LOG_WARN, "empty NALU discarded");
                if (cur == size - 3) {
                    pkt->size = last_end_plusone; 
                    size = pkt->size;
                } else {
                    uint8_t *dst = pkt->data + last_end_plusone;
                    uint8_t *src = pkt->data + cur + 3;
                    int size_copy = pkt->size - cur - 1;
                    memmove(dst, src, size_copy);
                    pkt->size = pkt->size - 3 - offset;
                    size = pkt->size;
                    cur = last_end_plusone - 1;
                }
            } else {
                last_end_plusone = cur + 3;
            }
        }
        cur++;
    }
    if (last_end_plusone > 0 && last_end_plusone == pkt->size) {
        int offset = 0;
        logger(LOG_WARN, "empty nale at end discarded");
        if (cur > 0 && pkt->data[cur-1] == 0) {
            offset = 1;
        }
        pkt->size = pkt->size - 3 - offset;
    }
}

#define MAX_BSF_ERROR_COUNT 20

int do_stream_filters(SegHandler *sh, AVPacket *pkt)
{
    TRACE_FRAME;

    AVStream *istream = get_input_stream(sh, pkt);
    AVStream *ostream = get_output_stream(sh, pkt);

    if (istream->codec->codec_id = AV_CODEC_ID_H264) {
        // strip AUD first (if AUD exists)
        strip_AVCC_AUD(pkt);

        if (sh->flags & NF_NEWEXTRADATA) {
            // restart bsf: close first
            if (sh->bsfc != NULL) {
                av_bitstream_filter_close(sh->bsfc);
                sh->bsfc = NULL;
            }
        }
        if (sh->bsfc == NULL) {
            logger(LOG_WARN, "init filter: h264_mp4toannexb");
            sh->bsfc = av_bitstream_filter_init("h264_mp4toannexb");
            if (sh->bsfc == NULL) {
                logger(LOG_ERROR, "av_bitstream_filter init h264_mp4toannexb fails");
                sh->flags |= NF_FILTER_ERROR;
            }
            sh->bsf_error_count = 0;
        }
        if (sh->bsfc != NULL) {
            int ret = av_apply_bitstream_filters(ostream->codec, pkt, sh->bsfc);
            if(ret < 0) {
                av_error("av_apply_bitstream_filters", ret);
                sh->flags |= NF_FILTER_ERROR;
            }
            AVCodecParameters *codecpar = ostream->codecpar;
            AVCodecContext *codec = ostream->codec;
            if (codecpar->extradata_size != codec->extradata_size
                || memcmp(codecpar->extradata, codec->extradata, codec->extradata_size)) {
                logger_binary(LOG_WARN, "extradata from", codecpar->extradata, codecpar->extradata_size);
                logger_binary(LOG_WARN, "extradata to", codec->extradata, codec->extradata_size);
                copy_extradata_new(codecpar, codec->extradata, codec->extradata_size);
            }
        }
        // if h264 is not in annex-b mode lasts X times, return fail
        if (pkt->size < 5 || (AV_RB32(pkt->data) != 0x0000001 && AV_RB24(pkt->data) != 0x000001)) {
            sh->flags |= NF_FILTER_ERROR;
            sh->bsf_error_count++;
            if(sh->bsf_error_count > MAX_BSF_ERROR_COUNT) {
                logger(LOG_ERROR, "bsf error too many times");
                return -1;
            }
        } else {
            sh->bsf_error_count = 0;
            strip_empty_nalu(pkt);
        }
    }

    if (istream->codec->codec_id == AV_CODEC_ID_HEVC) {
        // currently no strip AUD

        if (sh->flags & NF_NEWEXTRADATA) {
            // restart bsf: close first
            if (sh->bsfc != NULL) {
                av_bitstream_filter_close(sh->bsfc);
                sh->bsfc = NULL;
            }
        }
        if (sh->bsfc == NULL) {
            logger(LOG_WARN, "init filter: hevc_mp4toannexb");
            sh->bsfc = av_bitstream_filter_init("hevc_mp4toannexb");
            if (sh->bsfc == NULL) {
                logger(LOG_ERROR, "av_bitstream_filter_init hevc_mp4toannexb fail");
                sh->flags |= NF_FILTER_ERROR;
            }
            sh->bsf_error_count = 0;
        }
        if (sh->bsfc != NULL) {
            int ret = av_apply_bitstream_filters(ostream->codec, pkt, sh->bsfc);
            if (ret < 0) {
                av_error("av_apply_bitstream_filters", ret);
                sh->flags |= NF_FILTER_ERROR;
            }
            AVCodecParameters *codecpar = ostream->codecpar;
            AVCodecContext * codec = ostream->codec;
            if (codecpar->extradata_size != codec->extradata_size
                || memcmp(codecpar->extradata, codec->extradata, codec->extradata_size)) {
                logger_binary(LOG_WARN, "extradata from", codecpar->extradata, codecpar->extradata_size);
                logger_binary(LOG_WARN, "extradata to", codec->extradata, codec->extradata_size);
                copy_extradata_new(codecpar, codec->extradata, codec->extradata_size);
            }
        }

        // if hevc is not in annex-b mode lasts X times, return fail
        if (pkt->size < 5 || (AV_RB32(pkt->data) != 0x0000001 && AV_RB24(pkt->data) != 0x000001)) {
            logger(LOG_WARN, "output frame is not in annex-b mode");
            sh->flags |= NF_FILTER_ERROR;
            sh->bsf_error_count ++;
            if (sh->bsf_error_count > MAX_BSF_ERROR_COUNT) {
                logger(LOG_ERROR, "bsf error too many times");
                return -1;
            }
            // notify skip this frame
            return 1;
        } else {
            sh->bsf_error_count = 0;
            strip_empty_nalu(pkt);
        }
    }

    if (istream->codec->codec_type == AVMEDIA_TYPE_AUDIO) {
        AVCodecContext * audio_codec = istream->codec;
        if (sh->flags & NF_NEWEXTRADATA) {
            sh->aac_ever_changed = 1;
        }
        if (audio_codec->extradata_size > 0) {
            if (sh->aac_ever_changed) {
                aac_add_adts_header_from_extradata(audio_codec->extradata, audio_codec->extradata_size, pkt);
            }
        } else {
            int add = aac_add_adts_header(audio_codec, pkt);
            if (add) {
                logger(LOG_WARN, "AAC add ADTS header!");
                sh->flags |= NF_ADTS_WARN;
            }
        }
    }
    return 0;
}

int set_output_timestamp(SegHandler *sh, AVPacket *pkt)
{
    TRACE_FRAME;

    AVStream *istream = get_input_stream(sh, pkt);
    AVStream *ostream = get_output_stream(sh, pkt);
    AVRational itb = istream->time_base;
    AVRational otb = ostream->time_base;

    if (sh->params.output_absolute_timestamp) {
        pkt->dts += sh->cycle_base_time;
        pkt->pts += sh->cycle_base_time;
    }

    if (istream->codec->codec_type == AVMEDIA_TYPE_AUDIO) {
        AVRational stb = { 1, istream->codec->sample_rate };
        int duration = av_get_audio_frame_duration(istream->codec, pkt->size);
        if (!duration) {
            duration = istream->codec->frame_size;
        }
        // if audio stream is base stream,
        //      audio timestamp is simply increased by frame samples;
        if (is_base_stream(sh, pkt) && duration) {
            if (sh->audio_samples == AV_NOPTS_VALUE) {
                sh->audio_samples = av_rescale_q(pkt->dts, itb, stb);
            }
            pkt->pts = pkt->dts = av_rescale_q(sh->audio_samples, stb, otb);
            sh->audio_samples += duration;
        }
        // else if packet has dts,
        //      calculate audio timestamp by using ffmpeg method;
        else if (pkt->dts != AV_NOPTS_VALUE) {
            // !!! BE CAREFUL !!!
            // If pkt->dts == AV_NOPTS_VALUE, this function will be assert
            pkt->pts = pkt->dts = av_rescale_delta(itb, pkt->dts, stb, duration,
                    &sh->audio_samples, otb);
        } 
        // otherwise,
        //      do same as video.
        else {
            pkt->pts = av_rescale_q(pkt->pts, itb, otb);
            pkt->dts = av_rescale_q(pkt->dts, itb, otb);
        }
        pkt->duration = av_rescale_q(pkt->duration, itb, otb);
    } else {
        pkt->pts = av_rescale_q(pkt->pts, itb, otb);
        pkt->dts = av_rescale_q(pkt->dts, itb, otb);
        pkt->duration = av_rescale_q(pkt->duration, itb, otb);
    }
    return 0;
}

int set_output_stream_index(SegHandler *sh, AVPacket *pkt) 
{
    TRACE_FRAME;

    pkt->stream_index = get_output_stream(sh, pkt)->index;
    return 0;
}

int write_output_frame(SegHandler *sh, AVPacket *pkt) 
{
    TRACE_FRAME;
    int ret;

    // intend to put last pkt's ts fully interleaved on base missing,
    // for example, video = 0 ~ 10.05s and audio 0 ~ 20s, when time reached 10.05, 
    // all video packets all sent, but 10.0 ~ 10.05s video data are not flushed, 
    // which is not done until time reached some point (20s for example), 
    // when ts shall be cut, and flush is done in seg_file_end, making 10.0 ~ 10.5s data behind audio 20s data.
    // But 2 points making it difficult:
    // 1. detecting base misgging is not useful for this, audio packets are already sent
    // into local file while video packets flushed in base missing. 
    // There is no good way to interleave packets with mpegsenc cache logic behind...
    // except that we could auto flush video in mpegsenc when audio ts found bigger
    // 2. it is maybe useless for general players, as chrome not support m2ts, 
    // letting hls.js transmuxing it to fmp4, which already separates video and audio
    // (checked and no packets are lost in transmuxing), so interleaving is not necessary.
    // but sth like mac & ios VideoToolbox does have clearley behavior on such case...
    // if ((sh->params.output_noninterleaved >= OUTPUT_NONINTERLEAVED_CLR) &&
    //     (sh->base_missing_trigger & BASE_MISSING_TRIGGER_TO_MISSING)) {
    //     ret = av_write_frame(sh->oc, NULL);
    //     if (ret < 0) {
    //         av_error("av_write_frame flush_on_base_missing", ret);
    //         sh->flags |= NF_WRITE_ERROR;
    //         // fall through, not return
    //     }
    // }

    ret = av_interleaved_write_frame(sh->oc, pkt);
    if (ret < 0) {
        av_error("av_interleaved_write_frame", ret);
        sh->flags |= NF_WRITE_ERROR;
        return -1;
    }
    return 0;
}

static void fetch_metadata(SegHandler *sh, const char *name, char *buffer, int size) 
{
    AVDictionaryEntry *tag = NULL;
    tag = av_dict_get(sh->ic->metadata, name, NULL, AV_DICT_MATCH_CASE);
    if (tag != NULL) {
        snprintf(buffer, size - 1, "%s", tag->value);
        logger(LOG_INFO, "metadata[%s]: %s", name, tag->value);
    }
}

#define FETCH_METADATA(sh, name) fetch_metadata(sh, #name, sh->statis.name, sizeof(sh->statis.name))

void statis_on_connected(SegHandler *sh) 
{
    SegStatis *statis = &sh->statis;
    statis->connected_time = av_gettime();

    unsigned int i;
    for (i = 0; i < sh->ic->nb_streams; i++) {
        AVCodecContext *codec = sh->ic->streams[i]->codec;
        if (codec->codec_type == AVMEDIA_TYPE_VIDEO) {
            double framerate = av_q2d(codec->framerate);
            snprintf(statis->vcodec, sizeof(statis->vcodec) - 1, "%s %dx%d %g",
                    avcodec_get_name(codec->codec_id), codec->width, codec->height, framerate);
            logger(LOG_INFO, "VIDEO: %s", framerate);
        } else if (codec->codec_type == AVMEDIA_TYPE_AUDIO) {
            snprintf(statis->acodec, sizeof(statis->acodec) - 1, "%s %d %d",
                    avcodec_get_name(codec->codec_id), codec->sample_rate, codec->channels);
            logger(LOG_INFO, "AUDIO: %s", statis->acodec);
        }
    }
    FETCH_METADATA(sh, live_streamid);
    FETCH_METADATA(sh, via);
}

void statis_on_frame_input(SegHandler *sh, const AVPacket *pkt) 
{

}

void statis_on_frame_output(SegHandler *sh) 
{

}

static void set_will_flush(SegCacheContext *ctx) 
{
    if (ctx->stage != SEG_CACHECTX_STAGE_CACHE) {
        logger(LOG_WARN, "try flush but no frame cached");
        ctx->stage = SEG_CACHECTX_STAGE_CACHE2FLUSH;
        return;
    }
    ctx->stage = SEG_CACHECTX_STAGE_CACHE2FLUSH;
}

static void set_will_flush_ifneeded(SegCacheContext *ctx) 
{
    if (ctx->stage != SEG_CACHECTX_STAGE_CACHE) {
        return;
    }
    set_will_flush(ctx);
}

static void set_will_cache(SegCacheContext *ctx) 
{
    ctx->stage = SEG_CACHECTX_STAGE_CACHE;
}

static void set_will_cache_or_flush_for_extseg(SegCacheContext *ctx) 
{
    if (ctx->audio_cached && ctx->video_cached) {
        logger(LOG_WARN, "sequential av new extradata, force seg and flush");
        ctx->need_seg = 1;
        set_will_flush(ctx);
        return;
    }

    if (ctx->stage == SEG_CACHECTX_STAGE_NONE) {
        logger(LOG_WARN, "start caching due to new extradata");
    }
    set_will_cache(ctx);
}

static void plan_lhls_chunk_point_on_info_probed(SegHandler *sh);

// stage is seg_cachectx_stage_cache or seg_cachectx_stage_none, which has been ensured by caller
void seg_cachectx_update_status(SegHandler *sh, AVPacket *pkt)
{
    SegCacheContext *ctx = &sh->seg_cache_ctx;
    #if 0
    if (ctx->need_flush) {
        //unexpected, seg.c has avoided this condition
        logger(LOG_WARN, "last loop in flushe stage may fail, do it again");
        ctx->need_cache = 0;
        return;
    }
    #endif

    if (sh->params.is_lhls && sh->params.probe_gop > 0 &&
        sh->probe_gop_flag < PROBE_GOP_FLAG_DONE) {
        AVStream *istream = get_input_stream(sh, pkt);
        int64_t curr_dts = av_rescale_q(pkt->dts, istream->time_base, AV_TIME_BASE_Q);
        if (!(sh->stream_flags & STREAM_FLAGS_HAS_VIDEO)) {
            logger(LOG_WARN, "[probe gop] done early because no video streams exist.");
            sh->probe_gop_flag = PROBE_GOP_FLAG_DONE;
            set_will_flush_ifneeded(ctx);
            // NOTE: not able to used together with extseg.
            return;
        }
        if (sh->probe_gop_flag == PROBE_GOP_FLAG_INIT) {
            sh->probe_gop_flag = PROBE_GOP_FLAG_GOT_FIRST_PKT;
            sh->probe_gop_analyze_start_dts = curr_dts;
            set_will_cache(ctx);
        }
        // not equal, in case gop is just probe_gop.
        if (curr_dts - sh->probe_gop_analyze_start_dts > (int64_t)(sh->params.probe_gop) * 1000) {
            // it's not likely but possible that dts overflow, where we just stop probing gop.
            logger(LOG_WARN, "[probe gop] done but gop not found (probe duration is %d ms, "
                    "current dts is %" PRId64 " us, start dts is %" PRId64 " us)", sh->params.probe_gop,
                    curr_dts, sh->probe_gop_analyze_start_dts);
            sh->probe_gop_flag = PROBE_GOP_FLAG_DONE;
            set_will_flush(ctx);
            return;
        }

        if (sh->probe_gop_flag <= PROBE_GOP_FLAG_GOT_FIRST_PKT) {
            if (istream->codec->codec_type == AVMEDIA_TYPE_VIDEO &&
                (pkt->flags & AV_PKT_FLAG_KEY)) {
                sh->probe_gop_first_key_dts = curr_dts / 1000;
                sh->probe_gop_flag = PROBE_GOP_FLAG_GOT_FIRST_KEY;
                logger(LOG_INFO, "[probe gop] got first video key packet dts %" PRId64 " ms",
                    sh->probe_gop_first_key_dts);
                sh->probed_base_stream_gop_packets ++;
            }
        } else if (istream->codec->codec_type == AVMEDIA_TYPE_AUDIO) {
            if (pkt->flags & AV_PKT_FLAG_KEY) {
                sh->probed_base_stream_gop = curr_dts / 1000 - sh->probe_gop_first_key_dts;
                sh->probe_gop_flag = PROBE_GOP_FLAG_DONE;
                set_will_flush(ctx);
                logger(LOG_INFO, "[probe gop] done, probed video stream gop is %d ms,"
                    "current dts is %" PRId64 " ms, start dts is %" PRId64 " ms",
                    sh->probed_base_stream_gop, curr_dts / 1000, sh->probe_gop_first_key_dts);
                plan_lhls_chunk_point_on_info_probed(sh);
            } else {
                sh->probed_base_stream_gop_packets ++;
            }
        }
        return; 
    }
    if (sh->params.seg_on_ext) {
        if (sh->flags & NF_NEWEXTRADATA) {
            AVStream *istream = get_input_stream(sh, pkt);
            #if 0
            if (!istream) {
                // unexpected since istream != NULL has been checked.
                set_will_flush_ifneeded(ctx);
                return;
            }
            #endif

            if (istream->codec->codec_type == AVMEDIA_TYPE_AUDIO) {
                ctx->audio_cached = 1;
                set_will_cache_or_flush_for_extseg(ctx);
            } else if (istream->codec->codec_type == AVMEDIA_TYPE_VIDEO) {
                if (pkt->flags & AV_PKT_FLAG_KEY) {
                    ctx->video_cached = 1;
                    set_will_cache_or_flush_for_extseg(ctx);
                } else {
                    logger(LOG_WARN, "new extradata in non-key video pkt, reset cache status");
                    set_will_flush_ifneeded(ctx);
                }
            } else {
                // pkts that do not support cache, try flush if there are caches.
                set_will_flush_ifneeded(ctx);
            }
        } else {
            // 1. if in caching stage, change to flush
            // 2. other cases, ignore, just normal pkts
            if (ctx->stage == SEG_CACHECTX_STAGE_CACHE) {
                logger(LOG_WARN, "cache stage change to flush");
                set_will_flush(ctx);
            }
        }
    }
}

int seg_cachectx_cache_pkt(SegHandler *sh, AVPacket *pkt, int max_caches)
{
    SegCacheContext * ctx = &sh->seg_cache_ctx;
    AVPacket *copy = NULL;
    if (max_caches >= 0 && ctx->n_caches >= max_caches) {
        logger(LOG_ERROR, "unexpected: SegCacheContext n_caches %d reaches max, pkt may be lost", ctx->n_caches);
        return 1;
    }
    // we do not use av_packet_move_ref now to force use pkt->buf, but may be the same.
    copy = av_packet_clone(pkt);
    AVPktCache *pkt_cache = av_mallocz(sizeof(AVPktCache));
    if (!pkt_cache) {
        logger(LOG_ERROR, "failed to malloc AVPktCache, out of memory");
        return -1;
    }
    pkt_cache->cache = copy;
    pkt_cache->seg_flags = sh->flags;
    pkt_cache->is_base_missing = sh->is_base_missing;

    if (!ctx->pkt_caches) {
        ctx->pkt_caches = pkt_cache;
        ctx->pkt_caches_end = pkt_cache;
    } else {
        ctx->pkt_caches_end->next = pkt_cache;
        ctx->pkt_caches_end = pkt_cache;
    }
    ctx->n_caches++;
    if (max_caches >= 0 && ctx->n_caches >= max_caches) {
        logger(LOG_WARN, "segCacheContext n_caches %d reaches max, force flush", ctx->n_caches);
        return 1;
    }
    return 0;
}

int seg_cachectx_pop_and_restore_ctx(SegHandler *sh, AVPacket *pkt) 
{
    SegCacheContext *ctx = &sh->seg_cache_ctx;
    if (ctx->n_caches == 0) {
        logger(LOG_ERROR, "no cache but try to pop");
        return -1;
    }

    AVPktCache *cache = ctx->pkt_caches;
    AVPacket *cache_pkt = cache->cache;
    av_packet_move_ref(pkt, cache_pkt);

    // restore context
    sh->flags = cache->seg_flags;
    sh->is_base_missing = cache->is_base_missing;

    ctx->pkt_caches = cache->next;
    if(!ctx->pkt_caches) {
        ctx->pkt_caches_end = NULL;
    }
    ctx->n_caches--;
    av_packet_free(&cache_pkt);
    free(cache);

    if (ctx->n_caches == 0) {
        seg_cachectx_clear_caches(sh);
    }
    return 0;
}

void seg_cachectx_clear_caches(SegHandler *sh)
{
    SegCacheContext *ctx = &sh->seg_cache_ctx;
    AVPktCache *cache = ctx->pkt_caches;
    while(cache) {
        AVPktCache *next_cache = cache->next;
        AVPacket *cache_pkt = cache->cache;
        av_packet_free(&cache_pkt);
        free(cache);
        cache = next_cache;
    }
    ctx->pkt_caches = NULL;
    ctx->pkt_caches_end = NULL;
    ctx->n_caches = 0;

    ctx->stage = SEG_CACHECTX_STAGE_NONE;
    // do not reset need_seg flag, here caches are just poped not flushed out
    ctx->audio_cached = 0;
    ctx->video_cached = 0;
}

static void plan_lhls_chunk_point_on_info_probed(SegHandler *sh)
{
    logger(LOG_INFO, "[plan lhls] start chunk duration, gop=%d ms, gop_packets=%d",
        sh->probed_base_stream_gop, sh->probed_base_stream_gop_packets);
    int chunk_duration_lower_border;
    int chunk_duration_higher_border;
    int i;
    if (sh->params.chunk_duration_lower_ms >= 0 &&
        sh->params.chunk_duration_higher_ms > 0) {
        chunk_duration_lower_border = sh->params.chunk_duration_lower_ms;
        chunk_duration_higher_border = sh->params.chunk_duration_higher_ms;
        logger(LOG_INFO, "[plan lhls] chunk duration range is [%d, %d] due to configuration",
            chunk_duration_lower_border, chunk_duration_higher_border);
    } else if (sh->params.chunk_duration_ms > 0) {
        chunk_duration_lower_border = sh->params.chunk_duration_ms * 3 / 4;
        chunk_duration_higher_border = sh->params.chunk_duration_ms * 3 / 2;
        logger(LOG_INFO, "[plan lhls] chunk duration range is [%d, %d] due to chunk_duration_ms set (%d ms)",
            chunk_duration_lower_border, chunk_duration_higher_border, sh->params.chunk_duration_ms);
    } else {
        chunk_duration_lower_border = 225;
        chunk_duration_higher_border = 450;
        logger(LOG_INFO, "[plan lhls] chunk duration range is [%d, %d] due to default setting",
            chunk_duration_lower_border, chunk_duration_higher_border);
    }

    for (i = sh->probed_base_stream_gop_packets / 2; i > 0; i--) {
        int n_chunk_try = i;
        double n_pkts_try_d = sh->probed_base_stream_gop_packets;
        int n_pkts_try;
        n_pkts_try_d = n_pkts_try_d / (double)n_chunk_try;
        n_pkts_try = (int)(n_pkts_try_d + __DBL_EPSILON__);
        if (fabs(n_pkts_try_d - (double)n_pkts_try) < __DBL_EPSILON__) {
            int duration_try = sh->probed_base_stream_gop * n_pkts_try / sh->probed_base_stream_gop_packets;
            if (duration_try >= chunk_duration_lower_border &&
                duration_try <= chunk_duration_higher_border) {
                sh->planned_chunk_duration = duration_try;
                sh->planned_chunk_n_packets = n_pkts_try;
                logger(LOG_INFO, "[plan lhls] success: chunk_duration=%d ms, n_packets=%d",
                    sh->planned_chunk_duration, sh->planned_chunk_n_packets);
                break;
            }
        }
    }
    if (sh->planned_chunk_duration < 0) {
        logger(LOG_WARN, "[plan lhls] failed: no suitable chunk duration found");
    }
    return;
}


