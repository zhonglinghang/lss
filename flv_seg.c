#include "srs_librtmp.h"
#include "seg.h"
#include "seg_common.h"
#include "flv_seg.h"
#include "log.h"
#include "time.h"
#include <unistd.h>
#include <sys/stat.h>
#include <sys/fcntl.h>

static char *FLV_FILE_FORMAT = "%s-%u.flv";
static char *HDS_FILE_FORMAT = "%sSeg1-Frag%d";
static char *HDS_ABST_FORMAT = "%s_%d.abst";
static char *RPTP_FILE_FORMAT = "%s-%u.rpts";

static int interrupt_callback(void *p)
{
    SegHandler *sh = (SegHandler*) p;
    if (sh->interrupt) {
        logger(LOG_INFO, "interrupt outside");
        return 1;
    }
    // TODO
    // if (sh->actived > 0) {
    //     if(av_gettime_relative() - sh->actived > TIMEOUT) {
    //         logger(LOG_ERROR, "ffmpeg inactive, do interrupt");
    //         return 1;
    //     }
    // }
    // if (sh->params.timer) {
    //     sh->params.timer(sh);
    // }
    return 0;
}

static int get_file_size(char* filename)
{
    struct stat statbuf;
    stat(filename, &statbuf);
    int size = statbuf.st_size;
    return size;
}

static void hds_write_4bytes(char *p, int32_t value)
{
    char *pp = (char*)&value;
    *p++ = pp[3];
    *p++ = pp[2];
    *p++ = pp[1];
    *p++ = pp[0];
}

static void hds_write_8bytes(char *p, int64_t value)
{
    char *pp = (char*)&value;
    *p++ = pp[7];
    *p++ = pp[6];
    *p++ = pp[5];
    *p++ = pp[4];
    *p++ = pp[3];
    *p++ = pp[2];
    *p++ = pp[1];
    *p++ = pp[0];
}

static int32_t hds_read_4bytes(char *p)
{
    int32_t value;
    char *pp = (char*)&value;
    *pp++ = p[3];
    *pp++ = p[2];
    *pp++ = p[1];
    *pp++ = p[0];
    return value;
}

static int hds_read_4bytes_file(FILE *pf, int32_t *res) {
    char data[4];

    size_t read_n = fread(data, 1, 4, pf);
    if (read_n != 4) {
        logger(LOG_ERROR, "read abst file 4 bytes fail");
        return -1;
    }
    *res = hds_read_4bytes(data);
    return 0;
}

static int64_t hds_read_8bytes(char *p)
{
    int64_t value;
    char *pp = (char*)&value;
    *pp++ = p[7];
    *pp++ = p[6];
    *pp++ = p[5];
    *pp++ = p[4];
    *pp++ = p[3];
    *pp++ = p[2];
    *pp++ = p[1];
    *pp++ = p[0];
    return value;
}

static int hds_read_8bytes_file(FILE *pf, int32_t *res) {
    char data[8];

    size_t read_n = fread(data, 1, 8, pf);
    if (read_n != 8) {
        logger(LOG_ERROR, "read abst file 8 bytes fail");
        return -1;
    }
    *res = hds_read_8bytes(data);
    return 0;
}

#define HDS_WRITE_4(p, v) do {\
    hds_write_4bytes(p, v);\
    p += 4; \
    } while(0)

#define HDS_WRITE_8(p, v) do {\
    hds_write_8bytes(p, v);\
    p += 8; \
    } while(0)

// init an rtmp handle
static srs_rtmp_t rtmp_init(const char *url)
{
    int res = 0;

    srs_rtmp_t r = srs_rtmp_create(url);
    if (r == NULL) {
        logger(LOG_ERROR, "create rtmp handle fail");
        return NULL;
    }

    srs_rtmp_set_timeout(r, 5000, 5000);

    res = srs_rtmp_handshake(r);
    if (res != 0) {
        logger(LOG_ERROR, "rtmp handshake fail %s %d", url, res);
        srs_rtmp_destroy(r);
        return NULL;
    }

    res = srs_rtmp_connect_app(r);
    if (res != 0) {
        logger(LOG_ERROR, "rtmp connect app fail %s %d", url, res);
        srs_rtmp_destroy(r);
        return NULL;
    }

    res = srs_rtmp_play_stream(r);
    if (res != 0) {
        logger(LOG_ERROR, "rtmp play stream fail %s %d", url, res);
        srs_rtmp_destroy(r);
        return NULL;
    }

    return r;
}

static int hds_update_frag(flv_context_t *fc, SegHandler *sh) 
{
    int count = 0;
    frag_info *last_p = fc->frag_list;
    frag_info *new_p;

    while (last_p != NULL) {
        logger(LOG_INFO, "hds before update frag [start:%ld] [duration:%d] [index:%d]", 
            last_p->start_time, last_p->duration, last_p->index);
        count++;
        if (last_p->next == NULL) {
            break;
        }
        last_p = last_p->next;
    }

    // avoid duplicate sequence number
    if (last_p != NULL) {
        if (last_p->index == fc->hds_frag_count) {
            last_p->duration = fc->curr_pkt.packet_time - fc->start_time;
            last_p->start_time = fc->start_time;

            return 0;
        }
    }

    new_p = (frag_info *)malloc(sizeof(frag_info));
    if (sh->duration != 0) {
        new_p->duration = sh->duration / 1000;
    } else if (fc->curr_pkt.packet_time != 0) {
        new_p->duration = fc->curr_pkt.packet_time - fc->start_time;
    } else {
        new_p->duration = 0;
    }
    new_p->index = fc->hds_frag_count;
    new_p->next = NULL;
    new_p->start_time = fc->start_time;

    if (count == 0) {
        fc->frag_list = new_p;
    } else {
        last_p->next = new_p;

        if (count > HDS_FRAG_WINDOW_SIZE) {
            last_p = fc->frag_list->next;
            free(fc->frag_list);
            fc->frag_list = last_p;
        }
    }

    // just for print log
    last_p = fc->frag_list;
    while(last_p != NULL) {
        logger(LOG_INFO, "hds fater update frag [start:%ld] [duration:%d] [index:%d]",
            last_p->start_time, last_p->duration, last_p->index);
        if (last_p->next == NULL) {
            break;
        }
        last_p = last_p->next;
    }
    return 0;
}

/**
 * generate new segment file
 * 
 */
static int flv_seg_file_begin(SegHandler *sh, srs_flv_t *flv, flv_context_t *fc)
{
    if (sh->params.is_hds) {
        snprintf(sh->file, sizeof(sh->file), HDS_FILE_FORMAT, sh->params.name, sh->index);
        snprintf(sh->hds_abst_file, sizeof(sh->hds_abst_file), HDS_ABST_FORMAT, sh->params.name, sh->index);
    } else if (sh->params.is_rptp) {
        snprintf(sh->file, sizeof(sh->file), RPTP_FILE_FORMAT, sh->params.name, sh->index);
    } else {
        snprintf(sh->file, sizeof(sh->file), FLV_FILE_FORMAT, sh->params.name, sh->index);
    }

    logger(LOG_INFO, "start a new segment %s", sh->file);

    *flv = srs_flv_open_write(sh->file);

    if (*flv == NULL) {
        logger(LOG_ERROR, "open flv fail");
        return EC_OPEN_FAIL;
    }

    memset(&sh->seg_data, 0, sizeof(sh->seg_data));

    sh->flags &= 0xffff0000;
    sh->duration = 0;

    return EC_OK;
}

static void update_box(char *start, int size)
{
    char *p_size = (char *)&size;
    start[0] = p_size[3];
    start[1] = p_size[2];
    start[2] = p_size[1];
    start[3] = p_size[0];
}

static int parse_abst(SegHandler *sh, flv_context_t *fc, const char *path)
{
    int ret = 0;

    // open file
    FILE *pf = fopen(path, "r");
    if (pf == NULL) {
        logger(LOG_ERROR, "open continue abst file fail");
        return -1;
    }

    // seek to frag infos
    ret = fseek(pf, 86, SEEK_SET);
    if (ret != 0) {
        logger(LOG_ERROR, "seek abst file fail");
        fclose(pf);
        return ret;
    }
    // read hds abst data
    int32_t frag_count;
    int32_t frag_index;
    int32_t frag_starttime;
    int32_t frag_duration;

    ret =  hds_read_4bytes_file(pf, &frag_count);
    if (ret != 0) {
        logger(LOG_ERROR, "read abst frag count fail");
        fclose(pf);
        return ret;
    }
    logger(LOG_INFO, "get frag count from abst file %d", frag_count);

    int i;
    for(i = 0; i < frag_count; i++) {
        ret =  hds_read_4bytes_file(pf, &frag_index);
        if (ret != 0) {
            logger(LOG_ERROR, "read abst frag index fail");
            fclose(pf);
            return ret;
        }

        ret =  hds_read_8bytes_file(pf, &frag_starttime);
        if (ret != 0) {
            logger(LOG_ERROR, "read abst frag starttime fail");
            fclose(pf);
            return ret;
        }

        ret =  hds_read_4bytes_file(pf, &frag_duration);
        if (ret != 0) {
            logger(LOG_ERROR, "read abst frag duration fail");
            fclose(pf);
            return ret;
        }

        // update fc
        logger(LOG_INFO, "get continue abst info [start:%ld] [duration:%d] [index:%d]",
            frag_starttime, frag_duration, frag_index);
        fc->start_time = frag_starttime;
        fc->curr_pkt.packet_time = frag_starttime + frag_duration;
        fc->hds_frag_count = frag_index;
        hds_update_frag(fc, sh);
    }

    // start from next index
    fc->hds_frag_count++;
    sh->index = fc->hds_frag_count;

    fclose(pf);
    return ret;
}

static int flush_hds_abst(SegHandler *sh, flv_context_t *fc) 
{
    char abst_data[1024*100], *cur, *start_asrt, *start_afrt;

    memset(abst_data, 0, 1024*100);
    cur = abst_data;

    cur += 4;

    //**************write abst*****************
    *cur = 'a'; cur++;
    *cur = 'b'; cur++;
    *cur = 's'; cur++;
    *cur = 't'; cur++;

    cur += 4;
    HDS_WRITE_4(cur, fc->hds_frag_count);
    *cur = 0x20; cur++;
    HDS_WRITE_4(cur, 1000);

    // write last frag ts
    HDS_WRITE_8(cur, fc->start_time);
    cur += 8+5;
    *cur = 1; cur++;

    //**************write asrt*****************
    start_asrt = cur;
    cur += 4;

    *cur = 'a'; cur++;
    *cur = 's'; cur++;
    *cur = 'r'; cur++;
    *cur = 't'; cur++;

    cur += 5;
    HDS_WRITE_4(cur, 1);
    HDS_WRITE_4(cur, 1);
    HDS_WRITE_4(cur, fc->hds_frag_count);

    update_box(start_asrt, cur - start_asrt);

    *cur = 1; cur++;
    //**************write afrt*****************
    start_afrt = cur;
    cur += 4;

    *cur = 'a'; cur++;
    *cur = 'f'; cur++;
    *cur = 'r'; cur++;
    *cur = 't'; cur++;
    cur += 4;
    HDS_WRITE_4(cur, 1000);
    cur++;
    logger(LOG_INFO, "abst header seek %d", cur - abst_data);

    // write frags info
    char *frags_size_data = cur;
    int32_t frags_count = 0;
    cur += 4;

    frag_info *tmp_p = fc->frag_list;
    while(tmp_p != NULL) {
        HDS_WRITE_4(cur, tmp_p->index);
        HDS_WRITE_8(cur, tmp_p->start_time);
        HDS_WRITE_4(cur, tmp_p->duration);

        tmp_p = tmp_p->next;
        frags_count++;
    }
    hds_write_4bytes(frags_size_data, frags_count);

    update_box(start_afrt, cur - start_afrt);
    update_box(abst_data, cur - abst_data);

    int fd = open(sh->hds_abst_file, O_CREAT|O_WRONLY|O_TRUNC, S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP|S_IROTH);
    if (fd < 0) {
        logger(LOG_ERROR, "open bootstrap file failed, path=%s", sh->hds_abst_file);
        return -1;
    }
    if(write(fd, abst_data, cur - abst_data) != cur - abst_data) {
        logger(LOG_ERROR, "write bootstrap file failed, path=%s", sh->hds_abst_file);
        close(fd);
        return -1;
    }
    close(fd);
    return 0;
}

static void update_duration_on_valid(flv_context_t *fc, SegHandler *sh, int *no_seg);
static void flv_context_clear_packet(flv_context_t *fc);
static int try_get_interleaved_packet(SegHandler *sh, flv_context_t *fc, int force);
static void flush_interleaved_packet(SegHandler *sh, srs_flv_t flv, flv_context_t *fc) {
    int n_flush = 0;
    logger(LOG_INFO, "do flush interleaved packets on %s", __FUNCTION__);
    while(1) {
        int ret = try_get_interleaved_packet(sh, fc, 1);
        if (ret < 0) {
            logger(LOG_ERROR, "unexpected, try_get_interleaved_packet return %d < 0"
                    "on %s", ret, __FUNCTION__);
            break;
        } else if (ret == 0) {
            break;
        }

        n_flush ++;

        update_duration_on_valid(fc, sh, NULL);

        // TODO(mt): test on interrupted condition no mem leak;
        // on no flv, where program is interrupted, do pop but no write.
        if (flv) {
            u_int32_t revised_packet_time = fc->curr_pkt.packet_time;

            if (sh->params.flv_seg_flags & FLV_SEG_FLAGS_ALIGN_DTS) {
                if (revised_packet_time < fc->start_time) {
                    // seq_header or metadata
                    revised_packet_time = 0;
                } else {
                    revised_packet_time = revised_packet_time - fc->start_time;
                }
            }
            ret = srs_flv_write_tag(flv, fc->curr_pkt.packet_type, revised_packet_time,
                                    fc->curr_pkt.packet_buf, fc->curr_pkt.packet_size);
            if (ret != 0) {
                logger(LOG_ERROR, "write flv tag fail %d on %s", ret, __FUNCTION__);
                sh->flags |= NF_WRITE_ERROR;
            }
        }
        flv_context_clear_packet(fc);
    }
    logger(LOG_INFO, "flush interleaved packets done, n_flush = %d", n_flush);
}


static int flv_seg_file_end(SegHandler *sh, srs_flv_t flv, flv_context_t *fc, int last)
{
    if (last && sh->params.flv_seg_flags & FLV_SEG_FLAGS_INTERLEAVE_PKTS) {
        flush_interleaved_packet(sh, flv, fc);
    }

    if (flv != NULL) {
        logger(LOG_INFO, "finish segment %s", sh->file);
        srs_flv_close(flv);

        if (sh->params.is_hds) {
            flv = srs_flv_open_overwrite(sh->file);
            char *value = (char *)HDS_HEADER;

            //get size from real file size
            hds_write_4bytes(value, get_file_size(sh->file));

            int ret = srs_hds_write_header(flv, HDS_HEADER);
            if (ret != 0) {
                logger(LOG_INFO, "rewrite hds header fail %d", ret);
                return ret;
            }

            srs_flv_close(flv);

            hds_update_frag(fc, sh);

            flush_hds_abst(sh, fc);

            fc->hds_frag_count = sh->index;
        }

        sh->params.notify(sh, last);

        // if sequence number sync, the number should be decided by check_align
        if (!sh->params.seq_sync) {
            sh->index++;
        }
    }
    flv = NULL;
    return 0;
}

// if internal exists, it means it's inited already, but could not be referring to real buf,
// or it would be causing underfined behavior.
// all existing internal structure would be cleared by ref and unref.
static int init_flv_empty_packet(flv_referenced_packet *pkt) {
    flv_reference_internal *internal = pkt->internal;
    // do not set all variables to 0, which means we keep old packet time
    // memset(pkt, 0, sizeof(*pkt));
    pkt->packet_buf = NULL;
    pkt->packet_size = 0;
    if (internal) {
        if (internal->reference) {
            logger(LOG_ERROR, "unexpected, %s found pkt with non-zero reference", __FUNCTION__);
        }
        pkt->internal = internal;
    } else {
        pkt->internal = malloc(sizeof(flv_reference_internal));
        if (!pkt->internal) {
            logger(LOG_ERROR, "%s failed, out of memory", __FUNCTION__);
            return -1;
        }
    }
    memset(pkt->internal, 0, sizeof(flv_reference_internal));
    return 0;
}

static int flv_packet_is_valid(flv_referenced_packet *pkt) 
{
    return pkt->internal != NULL && pkt->internal->reference > 0;
}

static void flv_packet_set_reffered(flv_referenced_packet *pkt) 
{
    pkt->internal->reference = 1;
}

static void flv_packet_unref(flv_referenced_packet *pkt) 
{
    if (pkt->internal != NULL) {
        if (pkt->internal->reference > 0) {
            pkt->internal->reference--;
        }
        if (pkt->internal->reference == 0) {
            free(pkt->internal);
            if(pkt->packet_buf) {
                free(pkt->packet_buf);
            }
        }
    }
    pkt->internal = NULL;
    pkt->packet_buf = NULL;
    pkt->packet_size = 0;
}

static void flv_pakcet_ref(flv_referenced_packet *dst, flv_referenced_packet *src) 
{
    if (dst->internal) {
        flv_packet_unref(dst);
    }
    memcpy(dst, src, sizeof(flv_referenced_packet));
    dst->internal->reference++;
}

static void free_flv_empty_packet(flv_referenced_packet *pkt) 
{
    if (pkt->internal != NULL && pkt->internal->reference == 0) {
        free(pkt->internal);
        pkt->internal = NULL;
        pkt->packet_buf = NULL;
        pkt->packet_size = 0;
    }
}

static flv_stream_info_t *get_flv_stream_info(flv_context_t *fc, char packet_type) 
{
    if (packet_type == SRS_RTMP_TYPE_VIDEO) {
        return &(fc->stream_info[STREAM_INFO_IDX_VIDEO]);
    } else if (packet_type == SRS_RTMP_TYPE_AUDIO) {
        return &(fc->stream_info[STREAM_INFO_IDX_AUDIO]);
    } else {
        return &(fc->stream_info[STREAM_INFO_IDX_METADATA]);
    }
}

static int flv_is_base_stream(flv_context_t *fc)
{
    return fc->curr_pkt.packet_type == fc->base_stream_type;
}

/**
 * record avc sh and aac sh
 * manage timestamp
 * @param fc 
 * @param sh 
 */
static void flv_context_update(flv_context_t *fc, SegHandler *sh) 
{
    int is_valid_ts = 0;
    fc->curr_pkt.is_seq_header = 0;

    if (flv_is_base_stream(fc)) {
        sh->nonbase_count = 0;
        if (sh->is_base_missing) {
            logger(LOG_WARN, "base stream is back");
            sh->is_base_missing = 0;
        }
    } else {
        sh->nonbase_count ++;
        if (sh->nonbase_count > 100) {
            if (!sh->is_base_missing) {
                logger(LOG_WARN, "base stream is missing");
                sh->is_base_missing = 1;
            }
        }
    }

    if (fc->curr_pkt.packet_type == SRS_RTMP_TYPE_VIDEO) {
        if (srs_flv_is_sequence_header(fc->curr_pkt.packet_buf, fc->curr_pkt.packet_size)) {
            fc->curr_pkt.is_seq_header = 1;
        } else {
            is_valid_ts = 1;
        }
    }

    if (fc->curr_pkt.packet_type == SRS_RTMP_TYPE_AUDIO) {
        if (srs_utils_flv_audio_sound_format(fc->curr_pkt.packet_buf, fc->curr_pkt.packet_size) == 10 &&
            srs_utils_flv_audio_aac_packet_type(fc->curr_pkt.packet_buf, fc->curr_pkt.packet_size) == 0) {
                fc->curr_pkt.is_seq_header = 1;
        } else {
            is_valid_ts = 1;
        } 
    }

    if (fc->is_first_frame && is_valid_ts) {
        fc->start_time = fc->curr_pkt.packet_time;
        logger(LOG_INFO, "record start timestamp %u", fc->start_time);

        fc->is_first_frame = 0;
        if (sh->cycle_base_time == 0) {
            struct timeval tp;
            gettimeofday(&tp, NULL);
            sh->cycle_base_time = tp.tv_sec * 1000 + tp.tv_usec / 1000 - fc->start_time;

            logger(LOG_INFO, "calculate cycle base time based on system time %ld", sh->cycle_base_time);
        }
    }
    return ;
}

static void update_extradata_on_needed(flv_context_t *fc, SegHandler *sh) {
    if (sh->params.flv_meta) {
        if (srs_rtmp_is_onMetaData(fc->curr_pkt.packet_type, fc->curr_pkt.packet_buf,
                                    fc->curr_pkt.packet_size)) {
            logger(LOG_INFO, "buffer metadata, size:%d", fc->curr_pkt.packet_size);

            if (flv_packet_is_valid(&fc->metadata_buf)) {
                flv_packet_unref(&fc->metadata_buf);
            }
            flv_pakcet_ref(&fc->metadata_buf, &fc->curr_pkt);
        }
    }
    if (fc->curr_pkt.packet_type == SRS_RTMP_TYPE_VIDEO &&
        fc->curr_pkt.is_seq_header) {
        int old_buf_is_valid = flv_packet_is_valid(&fc->avc_sh_buf);

        if (!old_buf_is_valid || (old_buf_is_valid && (fc->avc_sh_buf.packet_size != fc->curr_pkt.packet_size||
            memcmp(fc->avc_sh_buf.packet_buf, fc->curr_pkt.packet_buf, fc->avc_sh_buf.packet_size)))) {
            logger(LOG_INFO, "update avc sequence header buffer, size:%d", fc->curr_pkt.packet_size);
            if (old_buf_is_valid) {
                flv_packet_unref(&fc->aac_sh_buf);
            }
            flv_pakcet_ref(&fc->avc_sh_buf, &fc->curr_pkt);
        }
    }

    if (fc->curr_pkt.packet_type == SRS_RTMP_TYPE_AUDIO && fc->curr_pkt.is_seq_header) {
        int old_buf_is_valid = flv_packet_is_valid(&fc->aac_sh_buf);

        if (!old_buf_is_valid || (old_buf_is_valid && (fc->aac_sh_buf.packet_size != fc->curr_pkt.packet_size ||
            memcmp(fc->aac_sh_buf.packet_buf, fc->curr_pkt.packet_buf, fc->aac_sh_buf.packet_size)))) {
            logger(LOG_INFO, "update aac sequence header buffer, size: %d", fc->curr_pkt.packet_size);

            if (old_buf_is_valid) {
                flv_packet_unref(&fc->aac_sh_buf);
            }
            flv_pakcet_ref(&fc->aac_sh_buf, &fc->curr_pkt);
        }
    }
}

static int is_flv_avc_seq_end(char type, char *buf, int size);
// return no_seg to avoid wrong seg point, such as seq header with pts = 0
static void update_duration_on_valid(flv_context_t *fc, SegHandler *sh, int *no_seg)
{   
    int no_upd_duration = 0;
    int is_avc_seq_end = is_flv_avc_seq_end(fc->curr_pkt.packet_type, fc->curr_pkt.packet_buf, fc->curr_pkt.packet_size);
    // duration < 0 underflow cause a new seg
    if (fc->curr_pkt.packet_time < fc->start_time) {
        if (fc->curr_pkt.is_seq_header || is_avc_seq_end) {
            logger(LOG_WARN, "no seg for seq %s at dts (%u)  < start(%u)",
                    fc->curr_pkt.is_seq_header? "header" : "end",
                    fc->curr_pkt.packet_time, fc->start_time);
            if (no_seg) {
                *no_seg = 1;
            }
            no_upd_duration = 1;
        } else {
            if (flv_is_base_stream(fc) || sh->is_base_missing) {
                logger(LOG_WARN, "pkt dts (%u) < start (%u)", fc->curr_pkt.packet_time, fc->start_time);
            }
            no_upd_duration = 1;
        }
    } else {
        if (is_avc_seq_end) {
            // no seg for avc sequence end
            logger(LOG_INFO, "no seg at avc sequence end. dts %u", fc->curr_pkt.packet_time);
            if (no_seg) {
                *no_seg = 1;
            }
        }
    }

    if (!no_upd_duration) {
        sh->duration = (fc->curr_pkt.packet_time - fc->start_time) * 1000;
    }
}

/**
 * clear packet memory
 * 
 * @param fc 
 */
static void flv_context_clear_packet(flv_context_t *fc) 
{
    flv_packet_unref(&fc->curr_pkt);
}

/**
 * free flv context
 * 
 * @param fc 
 */
static void flv_context_free(flv_context_t *fc) 
{
    flv_context_clear_packet(fc);
    if (flv_packet_is_valid(&fc->aac_sh_buf)) {
        flv_packet_unref(&fc->aac_sh_buf);
    } else {
        free_flv_empty_packet(&fc->aac_sh_buf);
    }

    if (flv_packet_is_valid(&fc->avc_sh_buf)) {
        flv_packet_unref(&fc->avc_sh_buf);
    } else {
        free_flv_empty_packet(&fc->avc_sh_buf);
    }

    if (flv_packet_is_valid(&fc->metadata_buf)) {
        flv_packet_unref(&fc->metadata_buf);
    } else {
        free_flv_empty_packet(&fc->metadata_buf);
    }
}

static int write_aac_seq_header(SegHandler *sh, srs_flv_t flv, flv_context_t *fc)
{
    int res = 0;
    if (flv_packet_is_valid(&fc->aac_sh_buf)) {
        if (sh->params.is_hds) {
            res = srs_flv_write_tag(flv, SRS_RTMP_TYPE_AUDIO, fc->start_time, 
                    fc->aac_sh_buf.packet_buf, fc->aac_sh_buf.packet_size);
        } else if (sh->params.is_rptp) {
            // do not add aac sh
        } else {
            res = srs_flv_write_tag(flv, SRS_RTMP_TYPE_AUDIO, 0, 
                    fc->aac_sh_buf.packet_buf, fc->aac_sh_buf.packet_size);
        }
        if (res != 0) {
            logger(LOG_ERROR, "write aac sh fail %d", res);
            return EC_OUTPUT_FAIL;
        }
        logger(LOG_DEBUG, "write aac sh prefix success");
    }
    return EC_OK;
}

static int write_avc_seq_header(SegHandler *sh, srs_flv_t flv, flv_context_t *fc)
{
    int res = 0;
    if (flv_packet_is_valid(&fc->avc_sh_buf)) {
        if (sh->params.is_hds) {
            res = srs_flv_write_tag(flv, SRS_RTMP_TYPE_VIDEO, fc->start_time, 
                    fc->avc_sh_buf.packet_buf, fc->avc_sh_buf.packet_size);
        } else if (sh->params.is_rptp) {
            // do not add avc sh
        } else {
            res = srs_flv_write_tag(flv, SRS_RTMP_TYPE_VIDEO, 0, 
                    fc->avc_sh_buf.packet_buf, fc->avc_sh_buf.packet_size);
        }
        if (res != 0) {
            logger(LOG_ERROR, "write avc sh fail %d", res);
            return EC_OUTPUT_FAIL;
        }
        logger(LOG_DEBUG, "write avc sh prefix success");
    }
    return EC_OK;
}

static int flv_seg_file_init(SegHandler *sh, srs_flv_t flv, flv_context_t *fc)
{
    logger(LOG_INFO, "start init flv file header");

    int res = 0;

    if (sh->params.is_hds) {
        res = srs_hds_write_header(flv, HDS_HEADER);
    } else if (sh->params.is_rptp) {
        if (fc->is_first_frame) {
            res = srs_flv_write_header(flv, FLV_HEADER);
        }
    } else {
        res = srs_flv_write_header(flv, FLV_HEADER);
    }

    if (res != 0) {
        logger(LOG_ERROR, "write flv header fail %d", res);
        return EC_OUTPUT_FAIL;
    }

    logger(LOG_DEBUG, "write flv header success");

    sh->rptp_is_keyframe = 0;
    sh->rptp_is_metadata = 0;
    sh->rptp_pts = 0;

    if (flv_packet_is_valid(&fc->metadata_buf)) {
        res = srs_flv_write_tag(flv, SRS_RTMP_TYPE_SCRIPT, 0, fc->metadata_buf.packet_buf, fc->metadata_buf.packet_size);
        if (res != 0) {
            logger(LOG_ERROR, "write metadata fail %d", res);
            return EC_OUTPUT_FAIL;
        }
        logger(LOG_DEBUG, "write metadata prefix success");
    }

    // always write avc seq header first to avoid ffmpeg concat bug
    res = write_avc_seq_header(sh, flv, fc);
    if (res != EC_OK) {
        return res;
    }

    res = write_aac_seq_header(sh, flv, fc);
    if (res != EC_OK) {
        return res;
    }

    // should be key frame which is not written in last segment
    if (flv_packet_is_valid(&fc->curr_pkt)) {
        u_int32_t revised_packet_time;
        if (sh->params.is_rptp) {
            sh->rptp_pts = fc->curr_pkt.packet_time;
            if (srs_flv_is_keyframe(fc->curr_pkt.packet_buf, fc->curr_pkt.packet_size)) {
                sh->rptp_is_keyframe = 1;
            }
        }

        revised_packet_time = fc->curr_pkt.packet_time;
        if (sh->params.flv_seg_flags & FLV_SEG_FLAGS_ALIGN_DTS) {
            // set to zero whatever
            revised_packet_time = 0;
        }
        res = srs_flv_write_tag(flv, fc->curr_pkt.packet_type, revised_packet_time,
                fc->curr_pkt.packet_buf, fc->curr_pkt.packet_size);
        if (res != 0) {
            logger(LOG_ERROR, "write first key frame in init fail %d", res);
            return EC_OUTPUT_FAIL;
        }
        logger(LOG_DEBUG, "write first key frame in init success");

        flv_context_clear_packet(fc);
    }
    return EC_OK;
}

/**
 * no_seg pervents normal seg, but not for abnormal conditions.
 * return 1 if need cut a new seg
 * @param sh 
 * @param fc 
 * @param no_seg 
 * @return int 
 */
static int is_seg_need_cut(SegHandler *sh, flv_context_t *fc, int no_seg) 
{
    // too much frames blocked
    int flv_av_frames_num = sh->seg_data.input_audio_frames + sh->seg_data.input_video_frames;

    if (flv_av_frames_num > FLV_MAX_FRAMES_PER_SECOND * sh->params.duration
        && flv_av_frames_num > FLV_MAX_FRAMES_TH) {
        logger(LOG_INFO, "too much frames, force cut. (%d vs %d)", flv_av_frames_num, FLV_MAX_FRAMES_TH);
        return 1;
    }

    if (fc->is_first_frame) {
        return 0;
    }

    if (no_seg) {
        return 0;
    }

    int threshold_ms = sh->params.duration * 1000 + sh->params.duration_ms;

    if (sh->params.is_rptp) {
        if (srs_flv_is_keyframe(fc->curr_pkt.packet_buf, fc->curr_pkt.packet_size)) {
            logger(LOG_INFO, "rptp key frame, new seg.");
            return 1;
        }
        if (fc->curr_pkt.packet_time - fc->start_time > threshold_ms) {
            logger(LOG_INFO, "rptp duration surpasses threshold, new seg. (%u %u vs %d)",
                    fc->curr_pkt.packet_time, fc->start_time, threshold_ms);
            return 1;
        }
        return 0;
    }

    if (fc->curr_pkt.packet_time - fc->start_time < threshold_ms && !sh->params.align) {
        return 0;
    }

    // srs_flv_is_keyframe only judges bits in flv format, not sth in nal
    // here fc->avc_sh_buf == NULL supports pure audio segment
    if ((fc->curr_pkt.packet_type == SRS_RTMP_TYPE_VIDEO &&
        srs_flv_is_keyframe(fc->curr_pkt.packet_buf, fc->curr_pkt.packet_size)) ||
        !flv_packet_is_valid(&fc->avc_sh_buf) || sh->is_base_missing) {
        if (sh->params.align) {
            if (check_align(sh, ((int64_t)fc->curr_pkt.packet_time) * 1000) == -1) {
                logger(LOG_INFO, "cut align %d, packet_time %u", sh->align_flag, fc->curr_pkt.packet_time);
                return 1;
            }
        } else {
            logger(LOG_INFO, "duration surpasses threshold, new seg. (%u %u vs %d)",
                    fc->curr_pkt.packet_time, fc->start_time, threshold_ms);
            return 1;
        }
    }
    return 0;
}

static int parse_metadata(SegHandler *sh, flv_context_t *fc) 
{
    char *meta_data = fc->curr_pkt.packet_buf;
    int32_t meta_size = fc->curr_pkt.packet_size;
    int amf_size;
    int amf_offset;
    char script_name[32] = {'\0'};
    int force_drop = 0;
    int do_regen = 0;
    int is_object = 0;
    int is_mixed_array = 0;

    //get script name, such as onMetaData
    srs_amf0_t meta_amf = srs_amf0_parse(meta_data, meta_size, &amf_offset);
    if (meta_amf == NULL) {
        logger(LOG_ERROR, "parse meta data name fail");
        return 0;
    }
    if (srs_amf0_is_string(meta_amf)) {
        // orig_script_name length could be surpass 32 bytes, which has already been
        // judged in srs_rtmp_is_on_MetaData
        const char *orig_script_name = srs_amf0_to_string(meta_amf);
        int script_name_len = strlen(orig_script_name);
        if (script_name_len < sizeof(script_name)) {
            snprintf(script_name, sizeof(script_name), "%s", orig_script_name);
        } else {
            logger(LOG_WARN, "unexpected, script name %s len %d < %d", orig_script_name, script_name_len, sizeof(script_name));
        }
    } else {
        logger(LOG_WARN, "unexpected, metadata start is not string");
    }

    srs_amf0_free(meta_amf);

    meta_amf = srs_amf0_parse(meta_data + amf_offset, meta_size - amf_offset, &amf_size);
    if (meta_amf == NULL) {
        logger(LOG_ERROR, "parse meta data name fail");
        return 0;
    }

    if (srs_amf0_is_object(meta_amf)) {
        logger(LOG_INFO, "metadata content is object type");
        is_object = 1;
    } else if (srs_amf0_is_ecma_array(meta_amf)) {
        logger(LOG_INFO, "metadata content is ecma array(mixed array) type");
        is_mixed_array = 1;
    } else {
        logger(LOG_ERROR, "unsupported metadata type, neither object nor mixed array");
        srs_amf0_free(meta_amf);
        return 1;
    }

    if (sh->params.seq_sync) {
        srs_amf0_t basetime = NULL;
        srs_amf0_t abs_basetime = NULL;

        if (is_object) {
            basetime = srs_amf0_object_property(meta_amf, "cyclebasetime");
            abs_basetime = srs_amf0_object_property(meta_amf, "abs_timestamp");
        } else if (is_mixed_array) {
            basetime = srs_amf0_ecma_array_property(meta_amf, "cyclebasetime");
            abs_basetime = srs_amf0_ecma_array_property(meta_amf, "abs_timestamp");
        }

        // if there is no basetime in metadata
        if (basetime == NULL && abs_basetime == NULL) {
            sh->cycle_base_time = 0;
            logger(LOG_INFO, "can not find base time");
            srs_amf0_free(meta_amf);
            // metadata is dropped according to original logic, return 0 for now
            return 0;
        }

        if (basetime != NULL) {
            const char *basetime_str = srs_amf0_to_string(basetime);
            sh->cycle_base_time = atol(basetime_str);
            logger(LOG_INFO, "get cntv base time %l", sh->cycle_base_time);
        } else if (abs_basetime != NULL) {
            sh->cycle_base_time = (int64_t) srs_amf0_to_number(abs_basetime);
            logger(LOG_INFO, "get abs base time %l", sh->cycle_base_time);
        }

        logger(LOG_INFO, "get cycle base time %ld", sh->cycle_base_time);
        force_drop = 1;
    }

    srs_amf0_t metadata_duration = NULL;
    if (is_object) {
        metadata_duration = srs_amf0_object_property(meta_amf, "duration");
    } else if (is_mixed_array) {    
        metadata_duration = srs_amf0_ecma_array_property(meta_amf, "duration");
    } 

    if (metadata_duration != NULL) {
        if (is_object) {
            srs_amf0_object_property_remove(meta_amf, "duration");
        } else if (is_mixed_array) {
            srs_amf0_ecma_array_property_remove(meta_amf, "duraiton");
        }
        logger(LOG_INFO, "strip \"duration\" in metadata");
        do_regen = 1;
    }

    if (do_regen) {
        int meta_amf_len = srs_amf0_size(meta_amf);
        int new_size = amf_offset + meta_amf_len;
        char *new_buf = malloc(new_size);
        srs_amf0_t key;
        int key_len;
        if (!new_buf) {
            logger(LOG_ERROR, "%s failed to alloc new metadata memory(%d bytes)", new_size);
            srs_amf0_free(meta_amf);
            return -1;
        }
        key = srs_amf0_create_string(script_name);
        if (key == NULL) {
            logger(LOG_ERROR, "%s failed to alloc create amf string, content = %s", script_name);
            free(new_buf);
            srs_amf0_free(meta_amf);
            return -1;
        }
        key_len = srs_amf0_size(key);
        if (amf_offset != key_len) {
            logger(LOG_ERROR, "unexpected, script name %s len %d != %d", key, key_len, amf_offset);
            free(new_buf);
            srs_amf0_free(key);
            srs_amf0_free(meta_amf);
            if (force_drop)  return 0;
            return -1;
        }
        if(srs_amf0_serialize(key, new_buf, key_len) != 0) {
            logger(LOG_ERROR, "unexpected, script name %s failed to serialize", script_name);
            free(new_buf);
            srs_amf0_free(key);
            srs_amf0_free(meta_amf);
            if (force_drop)  return 0;
            return 1;
        }
        srs_amf0_free(key);
        if(srs_amf0_serialize(meta_amf, new_buf + key_len, meta_amf_len) != 0) {    
            logger(LOG_ERROR, "unexpected, meta amf %s failed to serialize", script_name);
            free(new_buf);
            srs_amf0_free(key);
            srs_amf0_free(meta_amf);
            if (force_drop)  return 0;
            return 1;
        }
        // this does not reset variables such as packet time
        flv_packet_unref(&fc->curr_pkt);
        if (init_flv_empty_packet(&fc->curr_pkt) < 0) {
            logger(LOG_ERROR, "init_flv_empty_packet failed in %s", __FUNCTION__);
            free(new_buf);
            srs_amf0_free(meta_amf);
            return -1;
        }
        fc->curr_pkt.packet_buf = new_buf;
        fc->curr_pkt.packet_size = new_size;
        flv_packet_set_reffered(&fc->curr_pkt);
    }

    srs_amf0_free(meta_amf);

    if (force_drop)
        return 0;
    return 1;
}

// may return -1 on parse_metadata error
static int is_packet_meet(SegHandler *sh, flv_context_t *fc)
{
    if (srs_rtmp_is_onMetaData(fc->curr_pkt.packet_type, fc->curr_pkt.packet_type, fc->curr_pkt.packet_size)) {
        sh->rptp_is_metadata = 1;

        logger(LOG_INFO, "meet an metadata with %d bytes", fc->curr_pkt.packet_size);
        return parse_metadata(sh, fc);
    }
    if (fc->curr_pkt.packet_type == SRS_RTMP_TYPE_AUDIO) {
        logger(LOG_DEBUG, "meet audio frame");
        sh->seg_data.input_audio_frames++;
        sh->seg_data.output_audio_frames++;

        sh->flags &= ~NF_NO_AUDIO;
        return 1;
    }
    if (fc->curr_pkt.packet_type == SRS_RTMP_TYPE_VIDEO) {
        logger(LOG_DEBUG, "meet video frame");
        sh->seg_data.input_video_frames++;
        sh->seg_data.output_video_frames++;

        sh->flags &= ~NF_NO_VIDEO;
        return 1;
    }
    return 0;
}

static int is_flv_avc_seq_end(char type, char *buf, int size)
{
    if (type != SRS_RTMP_TYPE_VIDEO) {
        return 0;
    }
    if (size < 2) {
        return 0;
    }
    // avc end of sequence and no data besides VideoTagHeader
    if ((buf[0] & 0x0f) == 7 && buf[1] == 2 && size == 5) {
        return 1;
    }

    return 0;
}

static flv_interleaved_packet *make_interleaved_packet(flv_context_t *fc) 
{
    flv_interleaved_packet *pkt = malloc(sizeof(flv_interleaved_packet));
    if (!pkt) {
        return NULL;
    }
    memset(pkt, 0, sizeof(flv_interleaved_packet));
    flv_pakcet_ref(&pkt->content, &fc->curr_pkt);
    return pkt;
}

static int insert_pkt_to_stream_queue(SegHandler *sh, flv_context_t *fc, flv_stream_info_t *stream_info) 
{
    flv_interleaved_packet *pkt = make_interleaved_packet(fc);
    if (!pkt) {
        logger(LOG_ERROR, "%s out of memory allocating flv_interleaved_packet", __FUNCTION__);
        return -1;
    }
    stream_info->n_buffer++;

    if (fc->interleave_buffer == NULL) {
        fc->interleave_buffer = pkt;
        fc->interleave_buffer_end = pkt;
    } else {
        flv_interleaved_packet *last = fc->interleave_buffer_end;
        while(last) {
            if (pkt->content.packet_time < last->content.packet_time) {
                last = last->prev;
            } else {
                break;
            }
        }

        if (last) {
            flv_interleaved_packet *last_next = last->next;
            last->next = pkt;
            pkt->prev = last;
            pkt->next = last_next;
            if (last_next) {
                last_next->prev = pkt;
            } else {
                fc->interleave_buffer_end = pkt;
            }
        } else {
            pkt->next = fc->interleave_buffer;
            fc->interleave_buffer->prev = pkt;
            fc->interleave_buffer = pkt;
        }
    }
    return 0;
}

/**
 * return 1 when data has been popped
 * return 0 when no data could be popped, which means read_packet needed.
 * return -1 on error.
 * 
 * @param sh 
 * @param fc 
 * @param force 
 * @return int 
 */
static int try_get_interleaved_packet(SegHandler *sh, flv_context_t *fc, int force)
{
    flv_stream_info_t *stream_info = NULL;
    flv_interleaved_packet *pkt;

    if (!force) {
        int have_stream = 0;
        int stream_no_pkt = 0;
        int stream_buf_limited = 0;
        stream_info = &(fc->stream_info[STREAM_INFO_IDX_VIDEO]);
        if (stream_info->exist) {
            have_stream = 1;
            if (stream_info->n_buffer == 0) {
                stream_no_pkt = 1;
            } else if (stream_info->n_buffer >= MAX_N_BUFFER_VIDEO) {
                stream_buf_limited = 1;
            }
        }

        stream_info = &(fc->stream_info[STREAM_INFO_IDX_AUDIO]);
        if (stream_info->exist) {
            have_stream = 1;
            if (stream_info->n_buffer == 0) {
                stream_no_pkt = 1;
            } else if (stream_info->n_buffer >= MAX_N_BUFFER_AUDIO) {
                stream_buf_limited = 1;
            }
        }

        stream_info = &(fc->stream_info[STREAM_INFO_IDX_METADATA]);
        if (stream_info->exist) {
            have_stream = 1;
            // do not need to judge metadata stream.
        }

        // first metadata packet will be put through immediately, because there's only metadata
        // stream at first
        if (!stream_buf_limited) {
            if (!have_stream || stream_no_pkt) {
                return 0;
            }
        } 
    }
    // do pop
    pkt = fc->interleave_buffer;
    if (!pkt) {
        flv_interleaved_packet *next = pkt->next;
        next->prev = NULL;
        fc->interleave_buffer = next;
    } else {
        fc->interleave_buffer = NULL;
        fc->interleave_buffer_end = NULL;
    }
    stream_info = get_flv_stream_info(fc, pkt->content.packet_type);
    stream_info->n_buffer--;

    flv_pakcet_ref(&fc->curr_pkt, &pkt->content);
    flv_packet_unref(&pkt->content);
    free(pkt);
    return 1;
}

static int init_flv_context(flv_context_t *fc) 
{
    int ret;
    memset(fc, 0, sizeof(*fc));

    ret = init_flv_empty_packet(&fc->avc_sh_buf);
    if (ret < 0) return ret;
    ret = init_flv_empty_packet(&fc->aac_sh_buf);
    if (ret < 0) return ret;
    ret = init_flv_empty_packet(&fc->metadata_buf);
    if (ret < 0) return ret;

    ret = init_flv_empty_packet(&fc->curr_pkt);
    if (ret < 0) return ret;

    fc->is_first_frame = 1;

    // set base_stream_type to video, may further support only audio/video flv seg
    fc->base_stream_type = SRS_RTMP_TYPE_VIDEO;

    fc->got_first_content = 0;
    return 0;
}

#ifndef NDEBUG
int flv_seg_transave(SegHandler *sh, srs_rtmp_t r, srs_flv_t in_flv, srs_flv_t flv, flv_context_t *fc) 
{
#else
int flv_seg_transave(SegHandler *sh, srs_rtmp_t r, srs_flv_t flv, flv_context_t *fc)
{
#endif // NDEBUG
#ifdef UNIT_TEST
    static int _unit_test_count = 0;
    _unit_test_count++;

    if (_unit_test_count == 3) {
        sh->interrupt = 1;
        _unit_test_count = 0;
    }
#endif
    int res;
    int is_avc_seq_end = 0;
    int no_dts_increase_judge = 0;
    int no_seg = 0;
    u_int32_t revised_packet_time;

    if (fc->is_first_frame) {
        logger(LOG_INFO, "start transvae new segment %d", sh->index);
    } else {
        logger(LOG_INFO, "start transvae new segment %d, start_time %u", 
            sh->index, fc->curr_pkt.packet_time);
    }

    // in hds mode, packet time may be got by continue_abst
    fc->start_time = fc->curr_pkt.packet_time;

    res = flv_seg_file_init(sh, flv, fc);
    if (res != EC_OK) {
        logger(LOG_ERROR, "init segment file fail %d", res);
        return res;
    }

    while (1) {
        flv_stream_info_t *stream_info;
        int pkt_popped = 0;

        if (sh->interrupt) {
            return EC_OK;
        }

        if (sh->params.flv_seg_flags & FLV_SEG_FLAGS_INTERLEAVE_PKTS) {
            pkt_popped = try_get_interleaved_packet(sh, fc, 0);
            if (pkt_popped < 0) {
                logger(LOG_ERROR, "unexpeced, try_get_interleaved_packet return %d < 0", pkt_popped);
                return EC_FAIL;
            } 
        }

        if (!pkt_popped) {
            res = init_flv_empty_packet(&fc->curr_pkt);
            if (res < 0) {
                logger(LOG_ERROR, "init flv packet failed, out of memory");
                return EC_MEM;
            }
#ifndef NDEBUG
            if (r != NULL) {
#endif // NDEBUG
                // support pure audio/video seg for input with av input.
                res = srs_rtmp_read_packet(r, &(fc->curr_pkt.packet_type),
                                            &(fc->curr_pkt.packet_time),
                                            &(fc->curr_pkt.packet_buf),
                                            &(fc->curr_pkt.packet_size));
                if (res != 0) {
                    // error interrupted
                    if (res == 1500) {
                        logger(LOG_ERROR, "read rtmp packet fail, interrupted outside");
                    } else {
                        logger(LOG_ERROR, "read rtmp packet fail %d", res);
                    }
                    return EC_READ_FAIL;
                }
                flv_packet_set_reffered(&fc->curr_pkt);
#ifndef NDEBUG
            } else if (in_flv != NULL) {
                res = srs_flv_read_tag_header(in_flv, &(fc->curr_pkt.packet_type),
                                                &(fc->curr_pkt.packet_size),
                                                &(fc->curr_pkt.packet_time));
                if (res != 0) {
                    logger(LOG_ERROR, "failed to read flv header %d", res);
                    return EC_READ_FAIL;
                }
                if (fc->curr_pkt.packet_size < 0) {
                    logger(LOG_ERROR, "invalid flv packet size %d", res);
                    return EC_READ_FAIL;
                }
                fc->curr_pkt.packet_buf = malloc(fc->curr_pkt.packet_size);
                if (!fc->curr_pkt.packet_buf) {
                    logger(LOG_ERROR, "failed to malloc flv packet size %d",
                            fc->curr_pkt.packet_size);
                    return EC_READ_FAIL;
                }
                res = srs_flv_read_tag_data(in_flv, fc->curr_pkt.packet_buf, fc->curr_pkt.packet_size);
                if (res != 0) {
                    logger(LOG_ERROR, "failed to read flv tag %d", res);
                    return EC_READ_FAIL;
                }
                flv_packet_set_reffered(&fc->curr_pkt);
            } else {
                logger(LOG_ERROR, "unexpected: rtmp and flv all null");
                return EC_READ_FAIL;
            }
#endif
            logger(LOG_DEBUG, "read one rtmp packet. type:%d ts:%u size:%d",
                    fc->curr_pkt.packet_type, fc->curr_pkt.packet_time,
                    fc->curr_pkt.packet_size);
            
            if (fc->curr_pkt.packet_size == 0 || fc->curr_pkt.packet_buf == NULL) {
                logger(LOG_WARN, "empty packet found, discard");
                fc->curr_pkt.packet_size = 0;
                fc->curr_pkt.packet_buf = NULL;
                flv_context_clear_packet(fc);
                continue;
            }

            // just process meta, video, audio packet
            res = is_packet_meet(sh, fc);
            if (res < 0) {
                return EC_MEM;
            } if (res == 0) {
                flv_context_clear_packet(fc);
                continue;
            }

            flv_context_update(fc, sh);

            stream_info = get_flv_stream_info(fc, fc->curr_pkt.packet_type);
            stream_info->exist = 1;

            is_avc_seq_end = is_flv_avc_seq_end(fc->curr_pkt.packet_type, fc->curr_pkt.packet_buf,
                                                fc->curr_pkt.packet_size);
            no_dts_increase_judge = (fc->curr_pkt.is_seq_header || is_avc_seq_end ||
                                    fc->curr_pkt.packet_type == SRS_RTMP_TYPE_SCRIPT);
            if (!fc->curr_pkt.is_seq_header && fc->curr_pkt.packet_type != SRS_RTMP_TYPE_SCRIPT) {
                if (fc->curr_pkt.packet_time <= FLV_ROLLBACK_SMALL_THRESHOLD &&
                    stream_info->last_dts >= FLV_ROLLBACK_LARGE_THRESHOLD &&
                    stream_info->met_first) {
                    logger(LOG_WARN, "ts rollback found in stream type %d (%u to %u)",
                            (int)(fc->curr_pkt.packet_type), stream_info->last_dts,
                            fc->curr_pkt.packet_time);
                    return EC_TS_ERR;
                }
            }

            // here only check dts
            // does not check flv packet dts increase, for streams only
            if (!stream_info->met_first) {
                stream_info->last_dts = fc->curr_pkt.packet_time;
                stream_info->met_first = 1;
            } else if (fc->curr_pkt.packet_time > stream_info->last_dts &&
                        !no_dts_increase_judge) {
                stream_info->last_dts = fc->curr_pkt.packet_time;
            } else {
                if (!stream_info->last_is_seq_header && !no_dts_increase_judge) {
                    logger(LOG_WARN, "stream %d dts non increasing (%u vs %u)",
                            (int)(fc->curr_pkt.packet_type), fc->curr_pkt.packet_time,
                            stream_info->last_dts);
                }
            }
            stream_info->last_is_seq_header = fc->curr_pkt.is_seq_header;

            // do ts check before discard, avoid fatal ts error not triggering program exit
            if (stream_info->last_dts > fc->curr_pkt.packet_time && !no_dts_increase_judge) {
                int64_t offset = stream_info->last_dts;
                offset = offset - (int64_t)(fc->curr_pkt.packet_time);
                if (offset > FLV_TS_TOO_LARGE_OFFSET) {
                    logger(LOG_ERROR, "stream type %d dts offset too large type (%u - %u > %u)",
                                        (int)(fc->curr_pkt.packet_type), fc->curr_pkt.packet_time,
                                        stream_info->last_dts, (u_int32_t)(FLV_TS_TOO_LARGE_OFFSET));
                    return EC_TS_ERR;
                }
            }

            if (sh->params.flv_seg_flags & FLV_SEG_FLAGS_ALIGN_DTS) {
                if (fc->curr_pkt.packet_time < fc->start_time &&
                    (!fc->curr_pkt.is_seq_header && fc->curr_pkt.packet_type != SRS_RTMP_TYPE_SCRIPT)) {
                    logger(LOG_WARN, "align dts buf packet time (%u) < start time (%u), "
                                    "discarded", fc->curr_pkt.packet_time, fc->start_time);
                    flv_context_clear_packet(fc);
                    continue;
                }
            }

            // TODO: make a better video existence judgement instead of using fc->avc_sh_buf
            // if no seq head exists, unexpected conditions still happen
            if (fc->curr_pkt.packet_time < fc->start_time &&
                ((fc->curr_pkt.packet_type == SRS_RTMP_TYPE_VIDEO && !fc->curr_pkt.is_seq_header) ||
                 (sh->is_base_missing && !flv_packet_is_valid(&fc->avc_sh_buf) && 
                  fc->curr_pkt.packet_type == SRS_RTMP_TYPE_AUDIO && !fc->curr_pkt.is_seq_header))) {
                logger(LOG_INFO, "timestamp reverse, %u < %u, exit", fc->curr_pkt.packet_time, fc->start_time);
                return EC_TS_ERR;
            }

            // cache pkt here
            if (sh->params.flv_seg_flags & FLV_SEG_FLAGS_INTERLEAVE_PKTS) {
                int ret;

                ret = insert_pkt_to_stream_queue(sh, fc, stream_info);
                if (ret < 0) {
                    // no need to clean, all done by flv_context_free
                    return EC_MEM;
                }

                flv_packet_unref(&fc->curr_pkt);

                ret = try_get_interleaved_packet(sh, fc, 0);
                if (ret < 0) {
                    logger(LOG_ERROR, "unexpected, try_get_interleaved_packet return %d < 0", ret);
                    return EC_FAIL;
                } else if (ret == 0) {
                    // better to call flv_context_clear_packet, for now just flv_packet_unref
                    continue;
                }
            }
        }
        update_extradata_on_needed(fc, sh);

        if (!fc->got_first_content) {
            if (fc->curr_pkt.is_seq_header) {
                flv_context_clear_packet(fc);
                continue;
            } else if (fc->curr_pkt.packet_type != SRS_RTMP_TYPE_SCRIPT) {
                res = write_avc_seq_header(sh, flv, fc);
                if (res != EC_OK) {
                    return res;
                }

                res = write_aac_seq_header(sh, flv, fc);
                if (res != EC_OK) {
                    return res;
                }
                fc->got_first_content = 1;
            }
        }

        if (!(fc->common_flag & FLV_LIVE_COMMON_FLAG_PRINTED_NO_SEQ_HEADER) &&
            fc->curr_pkt.packet_type == SRS_RTMP_TYPE_VIDEO &&
            !flv_packet_is_valid(&fc->avc_sh_buf)) {
            logger(LOG_WARN, "video packet met but no seq header found!");
            fc->common_flag |= FLV_LIVE_COMMON_FLAG_PRINTED_NO_SEQ_HEADER;
        }

        if (!(fc->common_flag & FLV_LIVE_COMMON_FLAG_PRINTED_VIDEO_CODEC_ID) &&
            fc->curr_pkt.packet_type == SRS_RTMP_TYPE_VIDEO) {
            logger(LOG_WARN, "found video stream, codec is %s", 
                srs_flv_get_video_codec_name(fc->curr_pkt.packet_buf, fc->curr_pkt.packet_size));
            fc->common_flag |= FLV_LIVE_COMMON_FLAG_PRINTED_VIDEO_CODEC_ID;
        }

        if (!(fc->common_flag & FLV_LIVE_COMMON_FLAG_PRINTED_AUDIO_CODEC_ID) &&
            fc->curr_pkt.packet_type == SRS_RTMP_TYPE_AUDIO) {
            logger(LOG_WARN, "found audio stream, codec is %s", 
                srs_flv_get_audio_codec_name(fc->curr_pkt.packet_buf, fc->curr_pkt.packet_size));
            fc->common_flag |= FLV_LIVE_COMMON_FLAG_PRINTED_AUDIO_CODEC_ID;
        }

        // re-get stream info for current output packet
        stream_info = get_flv_stream_info(fc, fc->curr_pkt.packet_type);
        stream_info->exist = 1;

        no_seg = 0;
        update_duration_on_valid(fc, sh, &no_seg);

        if (is_seg_need_cut(sh, fc, no_seg)) {
            return EC_OK;
        }

        revised_packet_time = fc->curr_pkt.packet_time;
        if (sh->params.flv_seg_flags & FLV_SEG_FLAGS_ALIGN_DTS) {
            if (fc->is_first_frame) {
                // start_time may not be set, here just set revised_packet_time to 0
                // such as sequence header with large dts
                revised_packet_time = 0;
            } else if (revised_packet_time < fc->start_time) {
                // seq_header or metadata
                revised_packet_time = 0;
            } else {
                revised_packet_time = revised_packet_time - fc->start_time;
            }
            logger(LOG_DEBUG, "change ts from %u to %u (st = %u)",
                                fc->curr_pkt.packet_time, revised_packet_time,
                                fc->start_time);
        }
        res = srs_flv_write_tag(flv, fc->curr_pkt.packet_type, revised_packet_time,
                fc->curr_pkt.packet_buf, fc->curr_pkt.packet_size);
        if (res != 0) {
            logger(LOG_ERROR, "write flv tag fail %d", res);
            sh->flags |= NF_WRITE_ERROR;
            return EC_OUTPUT_FAIL;
        }
        flv_context_clear_packet(fc);
    }
    return EC_OK;
}

int flv_seg_run(SegHandler *sh)
{
    int now_s;
    int ret = EC_OK;

    srs_rtmp_t r = NULL;
#ifndef NDEBUG
    srs_flv_t in_flv = NULL;
#endif // NDEBUG

    // SetSrsInterruptInfo((SrsInterruptCall) interrupt_callback, (SrsInterruptContext)s);

#ifndef NDEBUG
    if (sh->params.url && !strncmp(sh->params.url, "rtmp://", 7)) {
        logger(LOG_WARN, "use rtmp version");
#endif // NDEBUG
        // TODO: support interrupt on handshake
        r = rtmp_init(sh->params.url);
        if (r == NULL) {
            logger(LOG_ERROR, "create rtmp handle fail");
            return EC_OPEN_FAIL;
        }
#ifndef NDEBUG
    } else {
        char flv_header[9];
        logger(LOG_WARN, "use file version");
        in_flv = srs_flv_open_read(sh->params.url);
        if (!in_flv) {
            logger(LOG_ERROR, "create flv handle fail");
            return EC_OPEN_FAIL;
        }
        ret = srs_flv_read_header(in_flv, flv_header);
        if (ret != 0) {
            logger(LOG_ERROR, "failed to read flv header");
            return EC_OPEN_FAIL;
        }
    }
#endif // NDEBUG

    logger(LOG_INFO, "create rtmp connect success");

    flv_context_t fc;
    srs_flv_t flv = NULL;

    ret = init_flv_context(&fc);
    if (ret < 0) {
        logger(LOG_ERROR, "fail to init flv context");
        return EC_MEM;
    }

    if (sh->params.is_hds) {
        // hds frag start with 1, not 0
        sh->index = 1;
        if (sh->params.start_number != 0) {
            sh->index = sh->params.start_number;
        }

        fc.hds_frag_count = sh->index - 1;
    }

    if (sh->params.seq_sync) {
        now_s = time((time_t *)NULL);
        sh->index = now_s / sh->params.duration;
        fc.hds_frag_count = sh->index;
    }

    if (sh->params.is_hds) {
        if (sh->params.continue_abst != NULL) {
            ret = parse_abst(sh, &fc, sh->params.continue_abst);
            if (ret != 0) {
                return EC_OPEN_FAIL;
            }
        }
    }

    while(1) {
        ret = flv_seg_file_begin(sh, &flv, &fc);
        if (ret != EC_OK) {
            break;
        }

#ifndef NDEBUG
        ret = flv_seg_transave(sh, r, in_flv, flv, &fc);
#else
        ret = flv_seg_transave(sh, r, flv, &fc);
#endif // NDEBUG
        if (ret != EC_OK) {
            break;
        }

        if (sh->interrupt) {
            logger(LOG_INFO, "recv interrupt signal. ready to quit");
            break;
        }

        flv_seg_file_end(sh, flv, &fc, 0);
    }

    flv_seg_file_end(sh, flv, &fc, 1);

    flv_context_free(&fc);

    if (r)
        srs_rtmp_destroy(r);
#ifndef NDEBUG
    if (in_flv)
        srs_flv_close(in_flv);
#endif

    return ret;
}