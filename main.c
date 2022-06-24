
#include <stdio.h>
#include <getopt.h>
#include <signal.h>
#include <pthread.h>
#include <libgen.h>
#include "libavutil/avstring.h"
#include "libavutil/mem.h"
#include "seg.h"
#include "flv_seg.h"
#include "log.h"
#include "version.h"
#include "m3u8.h"
#include "notify.h"
#include "srs_librtmp.h"

#define M3U8_VOD 1
#define M3U8_LIVE 2

#define STATIS_PERIOD 1000000

static SegParams g_params = {0};
static SegHandler g_seg;
static int g_log_level = LOG_INFO;
static int g_enable_m3u8 = 0;
static char m3u8_filename[1024] = {0};
static M3U8Context *m3u8_context = NULL;

static int g_flv_mod = 0;
static int g_rptp_mod = 0;
static int g_hds_mod = 0;

static int g_statis_notify = 0;
static int64_t g_statis_time = 0;
static StatisNotify g_statis = {0};
static int g_statis_count = 0;

void *g_custom_parms = NULL;

static void display_usage()
{
    printf("live stream segmenter version %s\n", VERSION);
    printf("Usage: livestream_segmenter [OPTION] ...\n");
    printf("\t-i --input FILE\n");
    printf("\t-d --duration SECONDS\n");
    printf("\t-D --duration MILLISECONDS\n");
    printf("\t-p --output-prefix PREFIX\n");
    printf("\t-t --task-id STRING\n");
    printf("\t-l --log-dir PATH\n");
    printf("\t-g --log-level LEVEL_NUMBER\n");
    printf("\t-u --notify-url URL\n");
    printf("\t-n --no-notify\n");
    printf("\t-s --statis-notify\n");
    printf("\t-m --generate-m3u8 VOD\n");
    printf("\t-M --generate-m3u8 LIVE\n");
    printf("\t-f --flv generate flv segments\n");
    printf("\t-r --rptp generate rptp segments\n");
    printf("\t-F --flv-meta generate flv segments with metadata ahead\n");
    printf("\t-a --align support multi bitrate\n");
    printf("\t-A --align support multi bitrate and sync sequence number\n");
    printf("\t-c --continue-abst the abst file path for gen continue hds index file\n");
    printf("\t-N --start-number set the start sequence number for hds\n");
    printf("\t-o --options can be: audio, video, has_audio, has_video\n");
    printf("\t-T --copyts (mpegts only)output the same timestamp as input\n");
    printf("\t-w --work-around can be: cra hevcaud\n");
    printf("\t-L --lhls use lhls mode\n");
    printf("\t-C --chunk-duration chunk duration in ms for lhls mode\n");
    printf("\t --custom customized options, a=xxx:b=xxx for further customized demands\n");
    printf("\t-h --help\n");
    exit(0);
}

static int try_apply_metakey_desc(SegParams *sp, int index, const char *metakey, 
    const char *value_type, const char *update_type) 
{
    size_t len_metakey, len_value_type, len_update_type;
    AMFDataType todo_value_type;
    MetaKeyUpdateType todo_update_type;
    MetaKeyDesc *dst = &sp->metakey_desc[index];
    if(!metakey || !value_type || !update_type) {
        logger(LOG_ERROR, "%s empty entry found", __FUNCTION__);
        return -1;
    }
    len_metakey = strlen(metakey);
    len_value_type = strlen(value_type);
    len_update_type = strlen(update_type);
    if (len_metakey == 0 || len_value_type == 0 || len_update_type == 0) {
        logger(LOG_ERROR, "%s 0-length entry found", __FUNCTION__);
        return -1;
    }

    if(len_metakey >= MAX_LEN_METAKEY) {
        logger(LOG_ERROR, "len of metakey %zu exceeds maximum %d", len_metakey, MAX_LEN_METAKEY);
        return -1;
    }

    if (!strcmp(value_type, "num")) {
        todo_value_type = AMF_DATA_TYPE_NUMBER;
    } else if (!strcmp(value_type, "bool")) {
        todo_value_type = AMF_DATA_TYPE_BOOL;
    } else if (!strcmp(value_type, "str")) {
        todo_value_type = AMF_DATA_TYPE_STRING;
    } else {
        logger(LOG_ERROR, "unrecognized value_type %s", value_type);
        return -1;
    }

    if(!strcmp(update_type, "once")) {
        todo_update_type = METAKEY_UPDATE_ONCE;
    } else if(!strcmp(update_type, "segfirst")) {
        todo_update_type = METAKEY_UPDATE_SEG_FIRST;
    } else if(!strcmp(update_type, "seglast")) {
        todo_update_type = METAKEY_UPDATE_SEG_LAST;
    } else if(!strcmp(update_type, "latest")) {
        todo_update_type = METAKEY_UPDATE_LATEST;
    } else {
        logger(LOG_ERROR, "unrecognized update_type %s", update_type);
        return -1;
    }

    strncpy(dst->key, metakey, sizeof(dst->key));
    dst->type = todo_value_type;
    dst->update_type = todo_update_type;
    return 0;
}

static int fill_metakey(SegParams *sp, const char *p) 
{
    int kidx_st;
    int i;
    const char *p_orig = p;
    for (kidx_st = 0; kidx_st < MAX_N_METAKEYS; kidx_st++) {
        if(sp->metakey_desc[kidx_st].key[0] == '\0') {
            break;
        }
    }
    if(kidx_st != 0) {
        logger(LOG_WARN, "%d metakeys have been filled but %s is called again!", kidx_st, __FUNCTION__);
    }

    i = kidx_st;
    while(p && *p) {

#define METAKEY_FREE(v) \
        do { \
            if (v) \
                av_free(&v); \
        } while(0)

#define METAKEY_FREE_ALL \
        do { \
            METAKEY_FREE(to_dealloc); \
            METAKEY_FREE(to_dealloc2); \
            METAKEY_FREE(to_dealloc3); \
        } while(0)
    
        const char *p2;
        const char *metakey;
        const char *value_type = NULL;
        const char *update_type = NULL;
        const char *to_dealloc = NULL;
        const char *to_dealloc2 = NULL;
        const char *to_dealloc3 = NULL;
        int ret;
        if (i >= MAX_N_METAKEYS) {
            logger(LOG_ERROR, "trying to use detect more metadata but num limit %d reached", MAX_N_METAKEYS);
            logger(LOG_ERROR, "configuration afterwards is ignored: %s", p);
            return 0;
        }

        p2 = av_get_token(&p, ",");
        to_dealloc = p2;
        if(p2 == NULL) {
            logger(LOG_ERROR, "av_get_token return null, internal error, string %s", p_orig);
            return -1;
        }
        if(*p) p++;
        metakey = av_get_token(&p2, "=");
        to_dealloc2 = metakey;
        if(metakey == NULL) {
            logger(LOG_ERROR, "metakey not found for %s", to_dealloc);
            METAKEY_FREE_ALL;
            continue;
        }

        if(*p2) p2++;
        if(!(*p2)) {
            logger(LOG_ERROR, "error splitting metakey desc %s: no info after metakey", to_dealloc);
            METAKEY_FREE_ALL;
            continue;
        }

        value_type = av_get_token(&p2, "=");
        to_dealloc3 = value_type;
        if(value_type == NULL) {
            logger(LOG_ERROR, "value_type not found for %s", to_dealloc);
            METAKEY_FREE_ALL;
            continue;;
        }

        if(*p2) p2++;
        if(!(*p2)) {
            logger(LOG_ERROR, "error splitting metakey desc %s: no info after value_tyep", to_dealloc);
            METAKEY_FREE_ALL;
            continue;
        }

        update_type = p2;
        ret = try_apply_metakey_desc(sp, i, metakey, value_type, update_type);
        if(ret < 0) {
            logger(LOG_ERROR, "failed to apply metakey (%s %s %s)", metakey, value_type, update_type);
            METAKEY_FREE_ALL;
            continue;
        }

        logger(LOG_INFO, "metakey set success: %s %d %d",
            sp->metakey_desc[i].key, sp->metakey_desc[i].type,
            sp->metakey_desc[i].update_type);
        i++;
        METAKEY_FREE_ALL;

#undef METAKEY_FREE
#undef METAKEY_FREE_ALL
    
    }

    return 0;
}

static int set_custom_args(SegParams *params, const char *key, const char *value) 
{
    if (!params || !key || !value || strlen(key) == 0 || strlen(value) == 0) {
        return -1;
    }
    logger(LOG_DEBUG, "--custom param: [key]%s -> [value]%s", key, value);
    if(!strcmp(key, "metakey")) {
        int ret = fill_metakey(params, value);
        if(ret < 0) {
            logger(LOG_ERROR, "fill metakey failed");
        }
        params->custom_metakey = av_strdup(value);
    } else if(!strcmp(key, "llhls_seg_dts")) {
        params->llhls_seg_by_dts = atoi(value);
        logger(LOG_WARN, "set llhls chunk segment by dts flags to %d", params->llhls_seg_by_dts);
    } else if(!strcmp(key, "flv_align_dts")) {
        int v = atoi(value);
        if(v) {
            params->flv_seg_flags |= FLV_SEG_FLAGS_ALIGN_DTS;
            logger(LOG_WARN, "set flv seg align dts mode");
        } else {
            params->flv_seg_flags &= ~FLV_SEG_FLAGS_ALIGN_DTS;
            logger(LOG_WARN, "set flv seg non align dts mode");
        }
    } else if(!strcmp(key, "flv_interleave_pkts")) {
        int v = atoi(value);
        if(v) {
            params->flv_seg_flags |= FLV_SEG_FLAGS_INTERLEAVE_PKTS;
            logger(LOG_WARN, "set flv seg force interleave mode");
        } else {
            params->flv_seg_flags &= ~FLV_SEG_FLAGS_INTERLEAVE_PKTS;
            logger(LOG_WARN, "set flv seg non force interleave mode");
        }
    } else if(!strcmp(key, "probe_gop_ms")) {
        params->probe_gop = atoi(value);
        logger(LOG_WARN, "set probe gop ms to %d", params->probe_gop);
    } else if(!strcmp(key, "chunk_duration_lower_ms")) {
        params->chunk_duration_lower_ms = atoi(value);
        logger(LOG_WARN, "set chunk_duration_lower_ms=%d", params->chunk_duration_lower_ms);
    } else if(!strcmp(key, "chunk_duration_higher_ms")) {
        params->chunk_duration_higher_ms = atoi(value);
        logger(LOG_WARN, "set chunk_duration_higher_ms=%d", params->chunk_duration_higher_ms);
    } else if(!strcmp(key, "cb_discontinuity")) {
        params->do_judge_discontinuity = atoi(value);
        logger(LOG_WARN, "set do_judge_discontinuity=%s", params->do_judge_discontinuity ? "true" : "false");
    } else {
        logger(LOG_ERROR, "unknown custom param [key]%s [value]%s", key, value);
        return -1;
    }
    return 0;
}

static int strheadcmp(const char *str, const char *pre) 
{
    return strncmp(str, pre, strlen(pre));
}

typedef int (*func_setargs)(SegParams *sp, const char *key, const char *value);

static int parse_args_list(SegParams *sp, const char *p, func_setargs cb) 
{
    int ret;
    while(p && *p) {
        const char *p2 = av_get_token(&p, ":");
        const char *to_dealloc = p2;
        char *key;
        if (!p2) {
            logger(LOG_ERROR, "No string after ':' character");
            return -1;
        }
        if (*p) p++;

        key = av_get_token(&p2, "=");
        if(!key) {
            av_freep(&to_dealloc);
            logger(LOG_ERROR, "No '=' character in %s", p2);
            return -1;
        }
        if(!*p2) {
            av_freep(&to_dealloc);
            av_freep(&key);
            logger(LOG_ERROR, "av_get_token error p2 empty string");
            return -1;
        }
        p2++;

        ret = cb(sp, key, p2);
        av_freep(&to_dealloc);
        av_freep(&key);

        if(ret < 0) {
            if(p && *p) {
                logger(LOG_ERROR, "next --custom string is ignored: %s", p);
            }
            return ret;
        }
    }
    return 0;
}

static void parse_opt(int argc, char *argv[]) 
{
    int lopt;
#define LOPT_CUSTOM (1001)
    const char *optstring = "c:i:d:D:p:t:l:g:u:N:o:w:C:nfrFHsmMhvaATL";
    const struct option opts[] = {
        {"continue-abst",   required_argument, NULL, 'c'},
        {"input",           required_argument, NULL, 'i'},
        {"duration",        required_argument, NULL, 'd'},
        {"duration-ms",     required_argument, NULL, 'D'},
        {"output-prefix",   required_argument, NULL, 'p'},
        {"task-id",         required_argument, NULL, 't'},
        {"log-dir",         required_argument, NULL, 'l'},
        {"log-level",       required_argument, NULL, 'g'},
        {"notify-url",      required_argument, NULL, 'u'},
        {"start-number",    required_argument, NULL, 'N'},
        {"only",            required_argument, NULL, 'o'},
        {"work-around",     required_argument, NULL, 'w'},
        {"chunk-around",    required_argument, NULL, 'C'},
        {"no-notify",       no_argument, NULL, 'n'},
        {"flv",             no_argument, NULL, 'f'},
        {"rptp",            no_argument, NULL, 'r'},
        {"flv-meta",        no_argument, NULL, 'F'},
        {"statis-notify",   no_argument, NULL, 's'},
        {"m3u8",            no_argument, NULL, 'm'},
        {"m3u8-live",       no_argument, NULL, 'M'},
        {"help",            no_argument, NULL, 'h'},
        {"test-vcid",       no_argument, NULL, 'v'},
        {"align",           no_argument, NULL, 'a'},
        {"align-seq",       no_argument, NULL, 'A'},
        {"hds",             no_argument, NULL, 'H'},
        {"copyts",          no_argument, NULL, 'T'},
        {"lhls",            no_argument, NULL, 'L'},
        {"custom",          required_argument, &lopt, LOPT_CUSTOM},
        {0, 0, 0, 0}
    };

    int opt;
    int long_index;
    do {
        opt = getopt_long(argc, argv, optstring, opts, &long_index);
        switch (opt) {
            case 0: {
                switch (lopt){
                    case LOPT_CUSTOM : {
                        const char* optarg_str = optarg;
                        int ret;
                        if (!optarg_str) {
                            logger(LOG_ERROR, "failed to parse custom args, optarg null");
                            break;
                        }
                        ret = parse_args_list(&g_params, optarg_str, set_custom_args);
                        if (ret < 0) {
                            logger(LOG_ERROR, "failed to parse custom args, optarg %s", optarg_str);
                        }
                        g_custom_parms = malloc(strlen(optarg_str) + 1);
                        if (!g_custom_parms) {
                            logger(LOG_ERROR, "failed to malloc custom args");
                        } else {
                            memcpy(g_custom_parms, optarg_str, strlen(optarg_str) + 1);
                        }
                        break;
                    }
                    default:
                        logger(LOG_ERROR, "unknown param with lopt %d", lopt);
                        break;
                }
                break;
            }
            case 'a': {
                g_params.align = 1;
                break;
            }
            case 'A': {
                g_params.align = 1;
                g_params.seq_sync = 1;
                break;
            }
            case 'i': {
                g_params.url = optarg;
                break;
            }
            case 'c': {
                g_params.continue_abst = optarg;
                break;
            }
            case 'C': {
                g_params.chunk_duration_ms = atoi(optarg);
                break;
            }
            case 'd': {
                g_params.duration = atoi(optarg);
                break;
            }
            case 'o': {
                if(!strheadcmp(optarg, "audio")) {
                    g_params.only_audio = 1;
                    g_params.skip_video_complement = 1;
                    logger(LOG_WARN, "output only audio");
                } else if(!strheadcmp(optarg, "video")) {
                    g_params.only_video = 1;
                    logger(LOG_WARN, "output only video");
                } else if(!strheadcmp(optarg, "has_audio")) {
                    g_params.only_has_audio = 1;
                    logger(LOG_WARN, "output must has audio");
                } else if(!strheadcmp(optarg, "has_video")) {
                    g_params.only_has_video = 1;
                    g_params.skip_video_complement = 1;
                    logger(LOG_WARN, "output must has video");
                }
                char *label = NULL;
                label = strstr(optarg, "ATS");
                if (label != NULL) {
                    g_params.output_absolute_timestamp = 1;
                    logger(LOG_WARN, "use absolute timestamp for output stream");
                } 
                label = strstr(optarg, "NIL");
                if (label != NULL) {
                    g_params.output_noninterleaved = OUTPUT_NONINTERLEAVED_NIL;
                    logger(LOG_WARN, "flush all non-interleaved frames at cutting");
                }
                label = strstr(optarg, "CLR");
                if (label != NULL) {
                    g_params.output_noninterleaved = OUTPUT_NONINTERLEAVED_CLR;
                    logger(LOG_WARN, "flush all non-interleaved frames and force packet to write down to segment file");
                }
                break;
            }
            case 'w': {
                if (optarg == NULL) {
                    logger(LOG_WARN, "optarg is NULL, quit -w parse.");
                    break;
                }
                char* label_w = NULL;
                label_w = strstr(optarg, "cra");
                if (label_w != NULL) {
                    g_params.workaround_cra = 1;
                    logger(LOG_WARN, "work around for CRA frame enabled, no cut at CRA.");
                }
                label_w = strstr(optarg, "hevcaud");
                if (label_w != NULL) {
                    g_params.workaround_hevcaud = 1;
                    logger(LOG_WARN, "do not add extra AUD for HEVC key frames in mpegts.");
                }
                label_w = strstr(optarg, "h264aud");
                if (label_w != NULL) {
                    g_params.workaround_h264aud = 1;
                    logger(LOG_WARN, "do not add extra AUD for H264 key frames in mpegts.");
                }
                label_w = strstr(optarg, "h2645aud");
                if (label_w != NULL) {
                    g_params.workaround_h264aud = 1;
                    g_params.workaround_hevcaud = 1;
                   logger(LOG_WARN, "do not add extra AUD for H264 and HEVC key frames in mpegts.");
                }
                break;
            }
            case 'D': {
                g_params.duration_ms = atoi(optarg);
                break;
            }
            case 'T': {
                g_params.copyts = 1;
                break;
            }
            case 'L': {
                g_params.is_lhls = 1;
                break;
            }
            case 'N': {
                g_params.start_number = atoi(optarg);
                break;
            }
            case 't': {
                g_params.tid = optarg;
                break;
            }
            case 'p': {
                g_params.name = optarg;
                break;
            }
            case 'l': {
                g_params.logdir = optarg;
                break;
            }
            case 'g': {
                g_log_level = atoi(optarg);
                if(g_log_level < LOG_ERROR || g_log_level > LOG_DEBUG) {
                    g_log_level = LOG_INFO;
                }
                break;
            }
            case 'u': {
                g_params.nurl = optarg;
                break;
            }
            case 'n': {
                set_notify_flag(0);
                break;
            }
            case 's': {
                g_statis_notify = 1;
                break;
            }
            case 'm': {
                g_enable_m3u8 = M3U8_VOD;
                break;
            }
            case 'M': {
                g_enable_m3u8 = M3U8_LIVE;
                break;
            }
            case 'f': {
                g_flv_mod = 1;
                logger(LOG_INFO, "use flv mod");
                break;
            }
            case 'r': {
                g_params.is_rptp = 1;
                g_rptp_mod = 1;
                logger(LOG_INFO, "use rptp mod");
                break;
            }
            case 'F': {
                g_params.flv_meta = 1;
                g_flv_mod = 1;
                logger(LOG_INFO, "use flv mod with metadata ahead");
                break;
            }
            case 'H':{
                g_params.is_hds = 1;
                g_hds_mod = 1;
                logger(LOG_INFO, "use hds mod");
                break;
            } 
            case 'h': {
                display_usage();
                break;
            }
        } 
        
    } while(opt != -1);
}

static void notify_callback(SegHandler *sh, int last) 
{
    logger(LOG_INFO, "notify: tid[%s] file[%s] duration[%lldms] last[%d] flags[%08x]",
        sh->params.tid, sh->file, sh->duration/1000, last, sh->flags);
    if (sh->params.nurl) {
        SegmentNotify segment = {};

        if(!sh->params.is_lhls) {
            segment_notify(sh->params.nurl, sh->params.tid, &segment);
        } else {
            segment_notify_pipe(sh->params.nurl, sh->params.tid, &segment);
        }
    }

    if(g_enable_m3u8) {
        M3U8SliceProps slice_props;
        if (strlen(m3u8_filename) == 0) {
            snprintf(m3u8_filename, sizeof(m3u8_filename) - 1, "%s.m3u8", g_params.name);
            if (M3U8_LIVE == g_enable_m3u8) {
                m3u8_context = (M3U8Context *) malloc(sizeof(M3U8Context));
            }
            m3u8_begin(m3u8_filename, sh->params.duration + 1, m3u8_context);
        }
        m3u8_get_default_slice_props(&slice_props);
        if(sh->discontinuity_before) {
            slice_props.discontinuity_before = 1;
        }
        m3u8_input_slice(m3u8_filename, basename(sh->file), (int)(sh->duration / 1000), m3u8_context, &slice_props);
        if(last) {
            m3u8_end(m3u8_filename, m3u8_context);
            if(m3u8_context) {
                free(m3u8_context);
                m3u8_context = NULL;
            }
        }
    }
}

static void chunk_notify_callback(SegHandler *sh) 
{
    logger(LOG_INFO, "chunk notify: tid[%s] file[%s] duration[%lldms]",
        sh->params.tid, sh->file, sh->chunk_duration / 1000);
    if(sh->params.nurl) {
        ChunkNotify chunk = {};
        if(sh->params.is_lhls) {
            chunk_notify_pipe(sh->params.nurl, sh->params.tid, &chunk);
        }
    }
}

static void fill_statis(SegHandler *sh, StatisNotify *statis, int count) 
{
    statis->url = sh->params.url;
}

static void timer_callback(SegHandler *sh) 
{
    int64_t now_time = av_gettime();
    if(g_statis_time == 0) {
        g_statis_time = now_time + STATIS_PERIOD;
    }
    if(now_time >= g_statis_time) {
        fill_statis(sh, &g_statis, g_statis_count);
        g_statis_count++;
        if(g_statis_count >= PERIOD_SIZE) {
            if(g_statis_notify && sh->params.nurl) {
                if(!sh->params.is_lhls) {
                    statis_notify(sh->params.nurl, sh->params.tid, &g_statis);
                } else {
                    statis_notify_pipe(sh->params.nurl, sh->params.tid, &g_statis);
                }
            }
            g_statis_count = 0;
        }
        g_statis_time += STATIS_PERIOD;
    }
}

static void check_params() {
    if(!g_params.url) {
        logger(LOG_ERROR, "FAIL: input null");
        exit(EC_FAIL);
    }
    if(!g_params.tid) {
        logger(LOG_ERROR, "FAIL: task id null");
        exit(EC_FAIL);
    }
    if(!g_params.name) {
        logger(LOG_ERROR, "FAIL: output prefix null");
        exit(EC_FAIL);
    }
    if(!g_params.duration < 0) {
        logger(LOG_ERROR, "FAIL: duration <= 0");
        exit(EC_FAIL);
    }
    g_params.maxframes = g_params.duration * 200;
    if(g_params.maxframes < 2000) {
        g_params.maxframes = 2000;
    }
    if(g_custom_parms) {
        logger(LOG_INFO, "custom setting: %s", (const char*)g_custom_parms);
        free(g_custom_parms);
        g_custom_parms = NULL;
    }
}

static void exit_when_timeup(int sec) 
{
    int i;
    for(i = 0; i < sec; i++) {
        usleep(1000000);
        logger(LOG_INFO, "wait for exit: %d", i + 1);
    }
    logger(LOG_WARN, "Time is up, force exit.");
    exit(0);
}

static void *signal_listener(void *param) 
{
    sigset_t *set = (sigset_t *) param;
    for(;;) {
        int signo = 0;
        sigwait(set, &signo);
        logger(LOG_WARN, "recv signal[%d]", signal);
        switch (signo)
        {
        case SIGINT:
        case SIGTERM:
            seg_stop(&g_seg);
            exit_when_timeup(60); // exit after 60s
            break;
        default:
            break;
        }
        usleep(1000);
    }
    return 0;
}

static void catch_signal() 
{
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &set, NULL);
    pthread_t thread;
    pthread_create(&thread, NULL, &signal_listener, (void *) &set);
}

int main(int argc, char* argv[]) 
{
    g_params.tid = "test";
    g_params.name = "out";
    g_params.duration = 10;
    g_params.duration_ms = 0;
    g_params.chunk_duration_ms = 300;
    g_params.logdir = "/home/admin/lss/logs/segmenter";
    g_params.nurl = "127.0.0.1/segmenter/notify/video";
    g_params.workaround_cra = 0;
    g_params.copyts = 0;
    parse_opt(argc, argv);
    g_params.notify = notify_callback;
    g_params.chunk_notify = chunk_notify_callback;
    g_params.timer = timer_callback;

    printf("live stream segmenter start...\n");
    char logfile[1024] = {0};
    snprintf(logfile, sizeof(logfile) - 1, "%s/%s.log", 
            g_params.logdir, g_params.tid);
    logger_init(g_log_level, logfile);

    logger(LOG_INFO, "> start live stream segmenter " VERSION);

    check_params();

    catch_signal();

    seg_init(&g_seg, &g_params);

    srs_initialize();

    int ret = 0;
    if(g_flv_mod == 1 || g_hds_mod == 1 || g_rptp_mod == 1) {
        ret = flv_seg_run(&g_seg);
    } else {
        ret = seg_run(&g_seg);
    }

    seg_uninit(&g_seg, &g_params);

    logger(LOG_INFO, "live stream segmenter ret: %d", ret);

    logger_uninit();

    srs_finalize();
    return ret;
}