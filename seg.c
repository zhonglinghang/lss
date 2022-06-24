#include "seg.h"
#include "log.h"
#include "flv_metadata.h"
#include "seg_common.h"
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>

#define MAX_WAIT_KEYFRAME_COUNT 1000
#define MAX_WRITE_FAIL_COUNT 20

#define COUNT_IF(count, limit) (count)++; if((count) >= (limit))

static int interrupt_callback(void *p) {
    SegHandler *sh = (SegHandler *) p;
    if (sh->interrupt) {
        logger(LOG_INFO, "interrupt outside!");
        return 1;
    }
    if (sh->actived > 0) {
        if (av_gettime_relative() - sh->actived > TIMEOUT) {
            logger(LOG_ERROR, "ffmpeg inactive, do interrupt!");
            return 1;
        }
    }

    if (sh->params.timer) {
        sh->params.timer(sh);
    }
    return 0;
}

static int chunk_begin(SegHandler *sh, int reinit, int got_pkt);

static int seg_file_begin(SegHandler *sh, int got_pkt) 
{
    int ret = 0;

    if (sh->params.align) {
        snprintf(sh->file, sizeof(sh->file), "%s-%u-%u.ts", sh->params.name, sh->index, sh->seg_index);
        sh->seg_index ++;
    } else {
        snprintf(sh->file, sizeof(sh->file), "%s-%u.ts", sh->params.name, sh->index);
    }

    AVDictionary *pb_options = NULL;
    ret = avio_open2(&sh->oc->pb, sh->file, AVIO_FLAG_WRITE, NULL, &pb_options);
    if(ret < 0) {
        av_error("avio_open2", ret);
        return -1;
    }

    AVDictionary *options = NULL;
    if (sh->params.workaround_hevcaud) {
        ret = av_dict_set(&options, "hevc_no_aud", "1", 0);
        if(ret < 0) {
            av_error("av_dict_set", ret);
            return -1;
        }
    }
    if (sh->params.workaround_h264aud) {
        ret = av_dict_set(&options, "h264_no_aud", "1", 0);
        if(ret < 0) {
            av_error("av_dict_set", ret);
            return -1;
        }
    }
    if (sh->params.copyts) {
        ret = av_dict_set(&options, "mpegts_copyts", "1", 0);
        if(ret < 0) {
            av_error("av_dict_set", ret);
            return -1;
        }
    }
    ret = avformat_write_header(sh->oc, &options);
    if (ret < 0) {
        av_error("avformat write header", ret);
        return -1;
    }
    memset(&sh->seg_data, 0, sizeof(sh->seg_data));

    if(sh->params.is_lhls) {
        ret = chunk_begin(sh, 1, got_pkt);
        if(ret < 0) {
            logger(LOG_ERROR, "chunk begin failed.\n");
            return -1;
        }
    }

    if (options) {
        av_dict_free(&options);
    }
    if(pb_options) {
        av_dict_free(&pb_options);
    }
    logger(LOG_INFO, "new ts: %s", sh->file);
    return 0;
}

static void do_judge_discontinuity_on_seg_end(SegHandler *sh) 
{
    sh->discontinuity_before = 0;

    if(sh->last_lost_video &&
        sh->seg_data.input_video_frames > 0) {
        sh->discontinuity_before = 1;
    } else if (sh->last_lost_audio &&
        sh->seg_data.input_audio_frames > 0) {
        sh->discontinuity_before = 1;
    }
    if ((sh->stream_flags & STREAM_FLAGS_HAS_VIDEO) &&
        sh->seg_data.input_video_frames == 0) {
        sh->last_lost_video = 1;
    } else {
        sh->last_lost_video = 0;
    }
    if ((sh->stream_flags & STREAM_FLAGS_HAS_AUDIO) &&
        sh->seg_data.input_audio_frames == 0) {
        sh->last_lost_audio = 1;
    } else {
        sh->last_lost_audio = 0;
    }
}

static void seg_file_end(SegHandler *sh, int last) 
{
    int i;
    if (last || (sh->flags & NF_NO_VIDEO)) {
        av_write_trailer(sh->oc);
    } else if(sh->is_base_missing || sh->params.output_noninterleaved) {
        logger(LOG_WARN, "flush all av frames in interleave buffer");
        av_interleaved_write_frame(sh->oc, NULL);
        if(sh->params.output_noninterleaved >= OUTPUT_NONINTERLEAVED_CLR ||
            sh->params.seg_on_ext) {
            av_write_frame(sh->oc, NULL);
        }
    } else {
        av_write_frame(sh->oc, NULL);
    }
    if (sh->oc->pb) {
        avio_flush(sh->oc->pb);
        if(sh->params.is_lhls) {
            sh->chunk_end = avio_tell(sh->oc->pb);
        }
        avio_closep(&sh->oc->pb);
    }

    if(sh->params.do_judge_discontinuity) {
        do_judge_discontinuity_on_seg_end(sh);
    }

    logger(LOG_INFO, "seg cut[%d]: duration: %lld", sh->index, sh->duration);
    sh->params.notify(sh, last);
    memset(&sh->seg_data, 0, sizeof(sh->seg_data));

    sh->index++;
    sh->duration = 0;
    sh->count = 0;
    sh->wait_keyframe_count = 0;
    sh->write_fail_count = 0;
    sh->flags &= 0xffff0000; // only reset low bits

    sh->first_key_pts = -1;
    sh->first_nonbase_pts = -1;
    sh->first_nonkey_pts = -1;

    sh->cycle_base_time = sh->next_cycle_base_time;
    sh->filename_base_time = sh->next_filename_base_time;

    sh->insert_discontinuity = 0;
    
    for(int i = 0; i < sh->n_metakey; i++) {
        MetaKeyInfo *info = &sh->metakey_info[i];
        const MetaKeyDesc *desc = info->desc;
        if(desc) {
            if(desc->update_type == METAKEY_UPDATE_SEG_FIRST ||
                desc->update_type == METAKEY_UPDATE_SEG_LAST) {
                AVDictionaryEntry *entry = av_dict_get(sh->ic->metadata, desc->key, 0, AV_DICT_MATCH_CASE);
                info->status = METAKEY_VAL_STAT_INIT;
                if (entry) {
                    // delete key to detect the first one in next loop.
                    av_dict_set(&sh->ic->metadata, desc->key, NULL, 0);
                }
            }
        }
    }
}

static int chunk_begin(SegHandler *sh, int reinit, int got_pkt)
{
    if (reinit) {
        sh->chunk_start = 0;
        sh->chunk_end = 0;
        sh->chunk_index = 0;
    } else {
        sh->chunk_start = sh->chunk_end;
    }

    sh->chunk_base_packets_count = got_pkt;
    sh->curr_chunk_flag = CURR_CHUNK_FLAG_NONE;
    return 0;
}

static void chunk_end(SegHandler *sh) 
{
    av_interleaved_write_frame(sh->oc, NULL);

    if (sh->params.llhls_notify_independent) {
        av_write_frame(sh->oc, NULL);
    }

    if(sh->oc->pb) {
        avio_flush(sh->oc->pb);
        sh->chunk_end = avio_tell(sh->oc->pb);
    }
    logger(LOG_INFO, "chunk[%lld]: duration = %lld", sh->chunk_index, sh->chunk_duration);
    sh->params.chunk_notify(sh);
    memset(&sh->chunk_data, 0, sizeof(sh->chunk_data));

    sh->chunk_index++;
}

static void copy_streams(SegHandler *sh, enum AVMediaType type, int base)
{
    unsigned int i;

    for(i = 0; i < sh->ic->nb_streams; i++) {
        AVStream *in_stream = sh->ic->streams[i];
        if(in_stream->codec->codec_id == AV_CODEC_ID_NONE) {
            continue;
        }

        // set input stream all the time
        sh->streams[i].in_stream = in_stream;

        int copy = 0;
        if (type == AVMEDIA_TYPE_UNKNOWN) {
            if (in_stream->codec->codec_type != AVMEDIA_TYPE_VIDEO
                && in_stream->codec->codec_type != AVMEDIA_TYPE_AUDIO) {
                copy = 1;
            }
        } else if (type == in_stream->codec->codec_type) {  
            copy = 1;
        }

        if (copy) {
            AVStream *out_stream = avformat_new_stream(sh->oc, in_stream->codec->codec);
            avcodec_copy_context(out_stream->codec, in_stream->codec);
            out_stream->codec->codec_tag = 0;
            if (sh->oc->oformat->flags & AVFMT_GLOBALHEADER) {
                out_stream->codec->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
            }
            if (base && (sh->base_stream_index < 0)) {
                sh->base_stream_index = i;
            }
            logger(LOG_INFO, "stream [%d]=>[%d] codec[%d:%x]", i, out_stream->index,
                in_stream->codec->codec_type, in_stream->codec->codec_id);

            // set output stream if copy
            sh->streams[i].out_stream = out_stream;
        }
    }
}

//length of from should be no less than length of to
static void str_replace_short(char *str, const char *from, const char *to) {
    while (1) {
        char *pos = strstr(str, from);
        if(pos == NULL) {
            break;
        }
        strcpy(pos, to);
        strcpy(pos + strlen(to), pos + strlen(from));
    }
}

void seg_init(SegHandler *sh, const SegParams *sp) 
{
    sh->params = *sp;
    sh->bsfc = NULL;
    sh->interrupt = 0;
    sh->index = 0;
    sh->seg_index = 0;
    sh->chunk_index = 0;
    sh->actived = av_gettime_relative();
    sh->begin = -1;
    sh->chunk_begin = -1;
    sh->duration = 0;
    sh->chunk_duration = 0;
    sh->chunk_start = 0;
    sh->chunk_end = 0;
    sh->count = 0;
    sh->wait_keyframe_count = 0;
    sh->write_fail_count = 0;
    sh->flags = 0;
    sh->align_flag = -1;
    sh->base_stream_index = -1;
    sh->audio_samples = AV_NOPTS_VALUE;
    sh->aac_ever_changed = 0;

    memset(&sh->statis, 0, sizeof(SegStatis));
    sh->statis.first_frame_pts = AV_NOPTS_VALUE;

    int i;
    for(i = 0; i < MAX_STREAMS; i++) {
        sh->streams[i].in_stream = NULL;
        sh->streams[i].out_stream = NULL;
        sh->streams[i].count = 0;
        sh->streams[i].idts = -1;
        sh->streams[i].odts = -1;
        sh->streams[i].duration = 0;
        sh->streams[i].ext_seqhead_size = 0;        
    }

    sh->nonbase_count = 0;
    sh->is_base_missing = 0;

    sh->cycle_base_time = 0;
    sh->next_cycle_base_time = 0;

    sh->filename_base_time = 0;
    sh->next_filename_base_time = 0;

    sh->first_key_pts = -1;
    sh->first_nonbase_pts = -1;
    sh->first_nonkey_pts = -1;

    sh->seg_start_dts = -1;

    sh->live_publish_timestamp = 0;
    sh->pts_rollback_flag = -1;

    {
        SegCacheContext *seg_cache_ctx = &sh->seg_cache_ctx;
        seg_cache_ctx->pkt_caches = NULL;
        seg_cache_ctx->pkt_caches_end = NULL;
        seg_cache_ctx->n_caches = 0;
        seg_cache_ctx->audio_cached = 0;
        seg_cache_ctx->video_cached = 0;
        seg_cache_ctx->stage = SEG_CACHECTX_STAGE_NONE;
        seg_cache_ctx->need_seg = 0;
    }

    sh->insert_discontinuity = 0;
    sh->curr_chunk_flag = CURR_CHUNK_FLAG_NONE;
    sh->stream_flags = STREAM_FLAGS_NONE;

    sh->probe_gop_flag = PROBE_GOP_FLAG_INIT;
    sh->probe_gop_analyze_start_dts = 0;
    sh->probe_gop_first_key_dts = 0;

    // 0 means not probed
    sh->probed_base_stream_gop = 0;
    sh->probed_base_stream_gop_packets = 0;

    sh->planned_chunk_duration = -1;
    sh->planned_chunk_n_packets = -1;

    sh->last_lost_video = 0;
    sh->last_lost_audio = 0;

    sh->n_metakey = 0;
    int no_link = 0;
    for(i = 0; i < MAX_N_METAKEYS; i++) {
        const MetaKeyDesc *desc = &sp->metakey_desc[i];
        MetaKeyInfo *dst = &sh->metakey_info[i];
        memset(dst, 0, sizeof(*dst));
        if (no_link) {
            if(desc->key[0] != '\0') {
                logger(LOG_WARN, "metakey %s not detected due to before hand empty key", desc->key);
            }
            continue;
        }
        if (desc->key[0] != '\0') {
            dst->desc = desc;
            sh->n_metakey++;
        } else {
            no_link = 1;
        }
    }
}

void seg_uninit(SegHandler *sh, SegParams *sp)
{
    if (sp->custom_metakey) {
        av_free(&sp->custom_metakey);
    }
}

static void ff_logger(void *avcl, int level, const char *fmt, va_list vl)
{
    int mylv = LOG_INFO;
    if(level <= AV_LOG_ERROR) {
        mylv = LOG_ERROR;
    } else if(level <= AV_LOG_WARNING) {
        mylv = LOG_WARN;
    } else if(level <= AV_LOG_INFO) {
        mylv = LOG_INFO;
    } else if(level <= AV_LOG_VERBOSE) {
        mylv = LOG_VERB;
    } else {
        mylv = LOG_DEBUG;
    }
    if (mylv > logger_level()) {
        return;
    }
    char line[1024] = {0};
    static int print_prefix = 1;
    av_log_format_line(avcl, level, fmt, vl, line, sizeof(line) - 1, &print_prefix);
    if (line[strlen(line) - 1] == '\n') {
        line[strlen(line) - 1] = 0;
    }
    logger(mylv, line);
}

static void try_update_metakey(SegHandler *sh) 
{
    int i;
    for(i = 0; i < sh->n_metakey; i++) {
        MetaKeyInfo *info = &sh->metakey_info[i];
        const MetaKeyDesc *desc = info->desc;
        if((desc->update_type == METAKEY_UPDATE_ONCE || 
                desc->update_type == METAKEY_UPDATE_SEG_FIRST) &&
                info->status >= METAKEY_VAL_STAT_GOT) {
            continue;
        }
        AVDictionaryEntry *entry = av_dict_get(sh->ic->metadata, desc->key, 0, AV_DICT_MATCH_CASE);
        if (entry != NULL) {
            int len_value = strlen(entry->value);
            if(entry->value == NULL || strlen(entry->value) == 0) {
                continue;
            }

            if(desc->type == AMF_DATA_TYPE_NUMBER || desc->type == AMF_DATA_TYPE_BOOL) {
                info->value.i64 = atol(entry->value);
            } else if (desc->type == AMF_DATA_TYPE_STRING) {
                if (len_value >= sizeof(info->value.str)) {
                    continue;
                }
                strncpy(info->value.str, entry->value, len_value);
            } else {
                continue;
            }

            info->status = METAKEY_VAL_STAT_GOT;
            av_dict_set(&sh->ic->metadata, desc->key, NULL, 0);
        }
    }
}

int seg_run(SegHandler *sh) 
{
    int ret = EC_OK;
    int i_metakey;

    av_register_all();
    av_log_set_callback(ff_logger);
    avformat_network_init();

    logger(LOG_INFO, "seg run start.\n");

    AVFormatContext *ic = NULL;
    AVFormatContext *oc = NULL;

    do {
        ic = avformat_alloc_context();
        if (ic == NULL) {
            logger(LOG_ERROR, "avformat_alloc_context fail");
            ret = EC_MEM;
            break;
        }
        sh->ic = ic;
        ic->interrupt_callback.callback = interrupt_callback;
        ic->interrupt_callback.opaque = sh;

        // avoid too long time for analyzing
        // set max analyze duration 3s
        ic->max_analyze_duration = 3000000;

        ret = avformat_alloc_output_context2(&oc, NULL, "mpegts", NULL);
        if (ret < 0) {
            av_error("avformat_alloc_output_context2", ret);
            ret = EC_MEM;
            break;
        }
        sh->oc = oc;
        oc->interrupt_callback.callback = interrupt_callback;
        oc->interrupt_callback.opaque = sh;
        oc->opaque = sh;

        // bugfix for safari compatibility
        // set output muxer delay 0.7s
        oc->max_delay = 700000;

        AVDictionary *options = NULL;
        av_dict_set(&options, "rtsp_transport", "tcp", 0);
        if (sh->metakey_info[0].desc && sh->params.custom_metakey) {
            av_dict_set(&options, "custom_metakey", sh->params.custom_metakey, 0);
        }

        ret = avformat_open_input(&ic, sh->params.url, NULL, &options);
        if (ret < 0) {
            av_error("avformat_open_input", ret);
            ret = EC_OPEN_FAIL;
            break;
        }

        if (options) {
            av_dict_free(&options);
        }

        // probe flv metadata by ourselves and set flags initial
        for (i_metakey = 0; i_metakey < sh->n_metakey; i_metakey++) {
            const MetaKeyDesc *desc = sh->metakey_info[i_metakey].desc;
            if (!desc) {
                break;
            }
            logger(LOG_INFO, "detect customized metakey %s, type %d, update type %d",
                desc->key, desc->type, desc->update_type);
        }
        FlvMetadata flvmeta = {0};
        flvmeta.cyclebasetime = 0;
        flvmeta.filename_basetime = 0;
        ret = probe_flv_metadata(ic->pb, sh, &flvmeta);
        if(ret < 0) {
            logger(LOG_ERROR, "probe_flv_metadata fail");
        } else {
            if (sh->params.test_vcid == 1 && flvmeta.videocodecid == 0) {
                logger(LOG_WARN, "set videocodecid for test");
                flvmeta.videocodecid = FLV_CODECID_H264;
                flvmeta.width = 1280;
                flvmeta.height = 720;
            }
            logger(LOG_INFO, "onMetaData: videocodecid[%d] audiocodecid[%d] width[%d] height[%d] cyclebasetime[%d] abs_base_time[%d] live_publish_timestamp[%d]",
                    flvmeta.videocodecid, flvmeta.audiocodecid, flvmeta.width, flvmeta.height, flvmeta.cyclebasetime,
                    flvmeta.filename_basetime, flvmeta.live_publish_timestamp);
            // only videocodecid, width and height > 0, consider the stream has video frame
            if (flvmeta.videocodecid > 0 && flvmeta.width > 0 && flvmeta.height > 0) {
                sh->flags |= NF_NO_VIDEO;
            }
            if (flvmeta.audiocodecid > 0) {
                sh->flags |= NF_NO_AUDIO;
            }

            sh->next_cycle_base_time = flvmeta.cyclebasetime;
            sh->cycle_base_time = flvmeta.cyclebasetime;
            sh->next_filename_base_time = flvmeta.filename_basetime;
            sh->filename_base_time = flvmeta.filename_basetime;
            sh->live_publish_timestamp = flvmeta.live_publish_timestamp;
        }

        // probe by ffmpeg
        ret = avformat_find_stream_info(ic, NULL);
        if (ret < 0) {
            av_error("avformat_find_stream_info", ret);
            ret = EC_STREAM_ERR;
            break;
        }

        // check metadata
        int i;
        int nb_av_streams = 0;
        for(i = 0; i < sh->ic->nb_streams; i++) {
            AVStream *in_stream = sh->ic->streams[i];
            if (in_stream->codec->codec_type == AVMEDIA_TYPE_VIDEO) {
                sh->flags &= ~NF_NO_VIDEO;
                nb_av_streams++;
            }
            if(in_stream->codec->codec_type == AVMEDIA_TYPE_AUDIO) {
                sh->flags &= ~NF_NO_AUDIO;
                nb_av_streams++;
            }
        }

        if (nb_av_streams > 2) {
            int old_maxframes = sh->params.maxframes;
            if(nb_av_streams > MAX_STREAMS) {
                nb_av_streams = MAX_STREAMS;
            }
            sh->params.maxframes = old_maxframes * nb_av_streams / 2;
            logger(LOG_WARN, "number of av streams (%d) surpasses 2, "
                    "revise max frame restriction from %d to %d",
                    nb_av_streams, old_maxframes, sh->params.maxframes);
        }

        // do statis
        statis_on_connected(sh);

        if (ic->nb_streams > MAX_STREAMS) {
            logger(LOG_ERROR, "too many streams[%u]", ic->nb_streams);
            ret = EC_STREAM_ERR;
            break;
        }

        // do not force to add video stream 
        if (!sh->params.skip_video_complement) {
            // add video stream if there is video stream in 'onMetaData' but probed nothing
            if(sh->flags & NF_NO_VIDEO) {
                AVStream *out_stream = avformat_new_stream(sh->oc, NULL);
                out_stream->codec->codec_tag = 0;
                out_stream->codec->codec_type = AVMEDIA_TYPE_VIDEO;
                out_stream->codec->codec_id = get_video_codec_id(flvmeta.videocodecid);
                out_stream->codec->width = flvmeta.width;
                out_stream->codec->height = flvmeta.height;
                logger(LOG_WARN, "add video stream forcedly");
            }
        }

        // copy all streams by order: VIDEO, AUDIO, other
        if (sh->params.only_audio) {
            copy_streams(sh, AVMEDIA_TYPE_AUDIO, 1);
        } else if (sh->params.only_video) {
            copy_streams(sh, AVMEDIA_TYPE_VIDEO, 1);
        } else {
            copy_streams(sh, AVMEDIA_TYPE_VIDEO, 1);
            copy_streams(sh, AVMEDIA_TYPE_AUDIO, 1);
        }

        copy_streams(sh, AVMEDIA_TYPE_UNKNOWN, 0);
        logger(LOG_INFO, "base_stream_index = %d", sh->base_stream_index);
        if (sh->base_stream_index < 0) {
            logger(LOG_INFO, "cannot find video or audio stream");
            ret = EC_STREAM_ERR;
            break;
        }

        // check output streams
        int has_video = 0;
        int has_audio = 0;
        for (i = 0; i < sh->oc->nb_streams; i++) {
            AVStream *out_stream = sh->oc->streams[i];
            if (out_stream->codec->codec_type == AVMEDIA_TYPE_VIDEO) {
                has_video = 1;
                sh->stream_flags |= STREAM_FLAGS_HAS_VIDEO;
            } else if (out_stream->codec->codec_type == AVMEDIA_TYPE_AUDIO) {
                has_audio = 1;
                sh->stream_flags |= STREAM_FLAGS_HAS_AUDIO;
            } else {
                sh->stream_flags |= STREAM_FLAGS_HAS_OTHERS;
            }
        }

        if (!has_video && sh->params.only_has_video) {
            logger(LOG_ERROR, "has no video stream but options contain `-o has_video`");
            ret = EC_STREAM_ERR;
            break;
        }
        if (!has_audio && sh->params.only_has_audio) {
            logger(LOG_ERROR, "has no audio stream but options contain `-o has_audio`");
            ret = EC_STREAM_ERR;
            break;
        }

        if(seg_file_begin(sh, 0) < 0) {
            ret = EC_OUTPUT_FAIL;
            // todo: add callback or seg file end
            break;
        }

        AVPacket pkt;
        av_init_packet(&pkt);

        while (1) {
            if(sh->seg_cache_ctx.stage != SEG_CACHECTX_STAGE_FLUSH && sh->seg_cache_ctx.need_seg) {
                logger(LOG_WARN, "unexpected! cache ctx stage %d but need seg, force clear", sh->seg_cache_ctx.stage);
                sh->seg_cache_ctx.need_seg = 0;
            }

            if (sh->seg_cache_ctx.stage != SEG_CACHECTX_STAGE_FLUSH) {
                // read in
                if(read_input_frame(sh, &pkt) < 0) {
                    ret = EC_READ_FAIL;
                    break;
                }

                if(sh->seg_start_dts < 0) {
                    AVStream * istream_tmp = get_input_stream(sh, &pkt);
                    // copy original logic
                    if(istream_tmp == NULL) {
                        ret = EC_UNEXP_STREAM;
                        break;
                    }
                    sh->seg_start_dts = av_rescale_q(pkt.dts, istream_tmp->time_base, AV_TIME_BASE_Q);
                }

                if (sh->ic->metadata) {
                    AVDictionaryEntry *abs_base_time = av_dict_get(sh->ic->metadata, "abs_base_time", 0, AV_DICT_MATCH_CASE);
                    AVDictionaryEntry *cyclebasetime = av_dict_get(sh->ic->metadata, "cyclebasetime", 0, AV_DICT_MATCH_CASE);

                    // customized meta keys
                    try_update_metakey(sh);

                    if (abs_base_time != NULL && cyclebasetime != NULL) {
                        int64_t tmp1 = atol(abs_base_time->value);
                        int64_t tmp2 = atol(cyclebasetime->value);
                        if(tmp1 > 0 && tmp2 > 0) {
                            AVStream *istream_tmp = get_input_stream(sh, &pkt);
                            if(istream_tmp == NULL) {
                                ret = EC_UNEXP_STREAM;
                                break;
                            }
                            int64_t cur_dts = av_rescale_q(pkt.dts, istream_tmp->time_base, AV_TIME_BASE_Q);
                            if (!sh->params.is_lhls && (cur_dts - sh->seg_start_dts) < NONSEG_BUF_TIME) {
                                logger(LOG_WARN, "Update cyclebasetime and abs_base_time immediately, since cur_dts(%lld) - seg_start_dts(%lld) < %lld", cur_dts, sh->seg_start_dts, NONSEG_BUF_TIME);
                                logger(LOG_WARN, "abs_base_time old value[%lld], update to new value[%lld]", sh->filename_base_time, tmp1);
                                logger(LOG_WARN, "cyclebasetime old value[%lld], update to new value[%lld]", sh->cycle_base_time, tmp2);
                                sh->filename_base_time = tmp1;
                                sh->next_filename_base_time = tmp1;
                                sh->cycle_base_time = tmp2;
                                sh->next_cycle_base_time = tmp2;
                            } else {
                                sh->next_filename_base_time = tmp1;
                                sh->next_cycle_base_time = tmp2;
                                logger(LOG_WARN, "new cyclebasetime and abs_base_time found in metadata, to be updated in the next ts");
                                logger(LOG_WARN, "abs_base_time old value[%lld], update to new value[%lld]", sh->filename_base_time, sh->next_filename_base_time);
                                logger(LOG_WARN, "cyclebasetime old value[%lld], update to new value[%lld]", sh->cycle_base_time, sh->next_cycle_base_time);
                            }
                            av_dict_set(&sh->ic->metadata, "abs_base_time", "-1", 0);
                            av_dict_set(&sh->ic->metadata, "cyclebasetime", "-1", 0);
                        }
                    }
                }
                if (sh->params.only_audio) {
                    if (sh->ic->streams[pkt.stream_index]->codec->codec_type == AVMEDIA_TYPE_VIDEO) {
                        av_packet_unref(&pkt);
                        continue;
                    }
                }

                if (sh->params.only_video) {
                    if (sh->ic->streams[pkt.stream_index]->codec->codec_type == AVMEDIA_TYPE_AUDIO) {
                        av_packet_unref(&pkt);
                        continue;
                    }
                }
                // if input stream exist in the beginning
                if(get_input_stream(sh, &pkt) == NULL) {
                    ret = EC_UNEXP_STREAM;
                    break;
                }

                // do after having validated pkt could be output
                // it's enough to open all cache ctx function
                if ((sh->params.is_lhls && sh->params.probe_gop > 0) ||
                    sh->params.seg_on_ext) {
                        seg_cachectx_update_status(sh, &pkt);
                }
            } else {
                // stage is flush, pop pkt and restore seghandler ctx
                int restore_ret = seg_cachectx_pop_and_restore_ctx(sh, &pkt);
                if(restore_ret < 0) {
                    seg_cachectx_clear_caches(sh);
                    continue;
                }
            }

            if (sh->seg_cache_ctx.stage == SEG_CACHECTX_STAGE_CACHE ||
                sh->seg_cache_ctx.stage == SEG_CACHECTX_STAGE_CACHE2FLUSH) {
                int max_caches = -1;
                if (sh->params.seg_on_ext) {
                    max_caches = EXTSEG_MAX_CACHES;
                }
                // cache a copy
                int cache_ret = seg_cachectx_cache_pkt(sh, &pkt, max_caches);
                av_packet_unref(&pkt);
                if (cache_ret < 0) {
                    ret = EC_MEM;
                    break;
                }

                if (cache_ret > 0) {
                    sh->seg_cache_ctx.stage = SEG_CACHECTX_STAGE_FLUSH;
                }
                if (sh->seg_cache_ctx.stage == SEG_CACHECTX_STAGE_CACHE2FLUSH) {
                    sh->seg_cache_ctx.stage = SEG_CACHECTX_STAGE_FLUSH;
                }
                continue;
            }

            statis_on_frame_input(sh, &pkt);

            // check frame dts
            if (check_input_timestamp(sh, &pkt)) {
                av_packet_unref(&pkt);
                ret = EC_TS_ERR;
                break;
            }

            // calculate duration and check file rotate
            if (check_duration(sh, &pkt) < 0) {
                seg_file_end(sh, 0);
                if(seg_file_begin(sh, 1) < 0) {
                    av_packet_unref(&pkt);
                    ret = EC_OUTPUT_FAIL;
                    break;
                }
            } else if (sh->params.is_lhls) {
                if(check_ts_chunk_duration(sh, &pkt) < 0) {
                    chunk_end(sh);
                    if(chunk_begin(sh, 0, 1) < 0) {
                        av_packet_unref(&pkt);
                        ret = EC_OUTPUT_FAIL;
                        break;
                    }
                }
            }

            check_timestamp_rollback(sh, &pkt);

            // discard frame if ext_seqhead conflicts
            if (check_ext_seqhead(sh, &pkt)) {
                av_packet_unref(&pkt);
                continue;
            }

            // wait until meet the first keyframe in base stream
            if (meet_first_keyframe(sh, &pkt) < 0) {
                av_packet_unref(&pkt);
                COUNT_IF(sh->wait_keyframe_count, MAX_WAIT_KEYFRAME_COUNT)
                {
                    ret = EC_NO_KEYFRAME;
                    break;
                } else {
                    continue;
                }
            }

            // frame count++
            frame_count_increase(sh, &pkt);

            // do stream filters
            {
                int bsf_ret = do_stream_filters(sh, &pkt);
                if(bsf_ret) {
                    av_packet_unref(&pkt);
                    continue;
                } else if(bsf_ret < 0) {
                    av_packet_unref(&pkt);
                    ret = EC_STREAM_ERR;
                    break;
                }
            }

            // calculate output timestamp and set
            set_output_timestamp(sh, &pkt);

            // rewrite stream index
            set_output_stream_index(sh, &pkt);

            if(sh->params.is_lhls && sh->params.llhls_notify_independent &&
                !(sh->curr_chunk_flag & CURR_CHUNK_FLAG_GOT_FIRST_PKT)) {
                AVStream *istream = get_input_stream(sh, &pkt);
                uint8_t * side_data;

                if(((pkt.flags & AV_PKT_FLAG_KEY) &&
                    istream->codec->codec_type == AVMEDIA_TYPE_VIDEO) ||
                    !(sh->stream_flags & STREAM_FLAGS_HAS_VIDEO)) {
                    sh->curr_chunk_flag |= CURR_CHUNK_FLAG_KEY_IN_HEAD;
                }

                // side_data = av_packet_new_side_data(&pkt, AV_PKT_DATA_MPEGTS_RESEND_HEADER, 1);
                side_data = av_packet_new_side_data(&pkt, AV_PKT_DATA_MPEGTS_STREAM_ID, 1);
                if(!side_data) {
                    logger(LOG_ERROR, "failed to alloc side data AV_PKT_DATA_MPEGTS_RESEND_HEADER, out of memory");
                    ret = EC_MEM;
                    break;
                }
            }

            //write out
            if (write_output_frame(sh, &pkt) < 0) {
                av_packet_unref(&pkt);
                COUNT_IF(sh->write_fail_count, MAX_WRITE_FAIL_COUNT)
                {
                    logger(LOG_ERROR, "too many write fails.. quit!");
                    ret = EC_OUTPUT_FAIL;
                    break;
                } else {
                    continue;
                }
            } 

            if (sh->params.is_lhls && !(sh->curr_chunk_flag & CURR_CHUNK_FLAG_GOT_FIRST_PKT)) {
                sh->curr_chunk_flag |= CURR_CHUNK_FLAG_GOT_FIRST_PKT;
            }

            // do statis
            statis_on_frame_output(sh);

            av_packet_unref(&pkt);
        }

        seg_file_end(sh, 1);
    } while (0);

    if (sh->seg_cache_ctx.n_caches) {
        SegCacheContext *ctx = &sh->seg_cache_ctx;
        logger(LOG_WARN, "cache frames not fully flushed at exit, clearing (%d pkts)", ctx->n_caches);
        seg_cachectx_clear_caches(sh);
    }

    if (sh->bsfc != NULL) {
        av_bitstream_filter_close(sh->bsfc);
    }
    avformat_close_input(&ic);
    avformat_free_context(oc);

    logger(LOG_INFO, "seg run end.");
    if (sh->interrupt) {
        ret = EC_OK;
    }
    return ret;
}

void seg_stop(SegHandler *sh) 
{
    sh->interrupt = 1;
}