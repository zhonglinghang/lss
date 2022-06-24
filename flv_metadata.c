#include "flv_metadata.h"
#include "flv_amf_common.h"
#include "seg.h"
#include "log.h"

int get_video_codec_id(int flv_codecid)
{
    switch (flv_codecid) {
    case FLV_CODECID_H263:
        return AV_CODEC_ID_FLV1;
    case FLV_CODECID_SCREEN:
        return AV_CODEC_ID_FLASHSV;
    case FLV_CODECID_SCREEN2:
        return AV_CODEC_ID_FLASHSV2;
    case FLV_CODECID_VP6:
        return AV_CODEC_ID_VP6F;
    case FLV_CODECID_VP6A:
        return AV_CODEC_ID_VP6A;
    case FLV_CODECID_H264:
        return AV_CODEC_ID_H264;
    default:
        return flv_codecid;
    }
}

static int amf_get_string(AVIOContext *io, char *buffer, int buffsize)
{
    int length = avio_rb16(io);
    if (length >= buffsize) {
        avio_skip(io, length);
        return -1;
    }
    avio_read(io, (unsigned char *) buffer, length);
    buffer[length] = '\0';
    return length;
}

// return -1 on error
// return 0 on not found
// return > 0 on found
static int amf_parse_custom_metakey(SegHandler *sh, const char *key, int amf_type, 
        double num_val, char *str_val)
{
    int i;
    int got = 0;
    for (i = 0; i < sh->n_metakey; i++) {
        MetaKeyInfo *info = &sh->metakey_info[i];
        const MetaKeyDesc *desc = info->desc;
        if (!desc) {
            break;
        }
        if (!strcmp(key, desc->key)) {
            if (amf_type != desc->type) {
                logger(LOG_WARN, "expect %s to be type %d, but actually %d, not recorded",
                        key, desc->type, amf_type);
                break;
            }
            if ((desc->update_type == METAKEY_UPDATE_ONCE &&
                        info->status >= METAKEY_VAL_STAT_GOT) ||
                    (desc->update_type == METAKEY_UPDATE_SEG_FIRST &&
                        info->status == METAKEY_VAL_STAT_GOT)) {
                return 0;
            }
            if (amf_type == AMF_DATA_TYPE_NUMBER ||
                amf_type == AMF_DATA_TYPE_BOOL) {
                info->value.i64 = (int64_t) num_val;
                got = 1;
            } else if (amf_type == AMF_DATA_TYPE_STRING) {
                int len_str_val = strlen(str_val);
                if (len_str_val >= MAX_LEN_METAKEY_CONTENT) {
                    logger(LOG_WARN, "value for metakey %s too long, ignored: %s", key, str_val);
                    return -1;
                }
                strncpy(info->value.str, str_val, MAX_LEN_METAKEY_CONTENT);
                got = 1;
            } else {
                logger(LOG_ERROR, "%s unexpected amf_type %d", __FUNCTION__, amf_type);
                return -1;
            }
            if (got) {
                info->status = METAKEY_VAL_STAT_GOT;
                break;
            }
        }
    }
    return got;
}

static int amf_parse_object(FlvMetadata *meta, AVIOContext *ioc, SegHandler *sh, 
        const char *key, int64_t max_pos, int depth)
{
    char str_val[1024];
    double num_val = 0;

    int amf_type = avio_r8(ioc);

    switch (amf_type)
    {
    case AMF_DATA_TYPE_NUMBER:
        num_val = av_int2double(avio_rb64(ioc));
        break;
    case AMF_DATA_TYPE_BOOL:
        num_val = avio_r8(ioc);
        break;
    case AMF_DATA_TYPE_STRING:
        if (amf_get_string(ioc, str_val, sizeof(str_val)) < 0) {
            logger(LOG_WARN, "amf_get_string fail");
            return -1;
        }
        break;
    case AMF_DATA_TYPE_OBJECT:
        while (avio_tell(ioc) < max_pos - 2
            && amf_get_string(ioc, str_val, sizeof(str_val)) > 0) {
            if (amf_parse_object(meta, ioc, sh, str_val, max_pos, depth + 1) < 0) {
                return -1;
            }
        }
        if (avio_r8(ioc) != AMF_DATA_TYPE_OBJECT_END) {
            logger(LOG_WARN, "object lack of END_OF_OBJECT");
            return -1;
        }
        break;
    case AMF_DATA_TYPE_NULL:
    case AMF_DATA_TYPE_UNDEFINED:
    case AMF_DATA_TYPE_UNSUPPORTED:
        break; // these take up no additional space
    case AMF_DATA_TYPE_MIXEDARRAY: {
        unsigned v;
        avio_skip(ioc, 4);
        while (avio_tell(ioc) < max_pos - 2
            && amf_get_string(ioc, str_val, sizeof(str_val)) > 0) {
            // this is the only case in which we would want a nested
            // parse to not skip over the object
            if (amf_parse_object(meta, ioc, sh, str_val, max_pos, depth + 1) < 0) {
                return -1;
            }
        }
        v = avio_r8(ioc);
        if (v != AMF_DATA_TYPE_OBJECT_END) {
            logger(LOG_WARN, "mixedarray lack of END_OF_OBJECT");
            return -1;
        }
        break;
    }
    case AMF_DATA_TYPE_ARRAY: {
        unsigned int arraylen, i;

        arraylen = avio_rb32(ioc);
        for (i = 0; i < arraylen && avio_tell(ioc) < max_pos - 1; i++) {
            if (amf_parse_object(meta, ioc, sh, NULL, max_pos, depth + 1) < 0) {
                return -1; // if we couldn't skip, bomb out
            }
        }
        break;
    }
    case AMF_DATA_TYPE_DATE:
        avio_skip(ioc, 8 + 2); // timestamp (double) and utc offset (int16)
        break;
    default:  // unsupported type, couldn't skip
        logger(LOG_WARN, "unsupported type = %d", amf_type);
        return -1;
    }

    if (key) {
        // stream info doesn't live any deeper than the first object
        if (depth == 1) {
            int unmatched = 0;
            if (amf_type == AMF_DATA_TYPE_NUMBER) {
                if (!strcmp(key, "videocodecid")) {
                    meta->videocodecid = (int) num_val;
                } else if (!strcmp(key, "audiocodecid")) {
                    meta->audiocodecid = (int) num_val;
                } else if (!strcmp(key, "width")) {
                    meta->width = (int) (int) num_val;
                } else if (!strcmp(key, "height")) {
                    meta->height = (int) (int) num_val;
                } else if (!strcmp(key, "abs_timestamp")) {
                    if (meta->cyclebasetime == 0) { 
                        meta->cyclebasetime = (int64_t) num_val;
                    }
                } else if (!strcmp(key, "abs_base_time")) {
                    meta->filename_basetime = (int64_t) num_val;
                } else if (!strcmp(key, "publish_timestamp")) {
                    meta->live_publish_timestamp = (int64_t) num_val;
                } else {
                    unmatched = 1;
                } 
            } else if (amf_type == AMF_DATA_TYPE_STRING) {
                if (!strcmp(key, "cyclebasetime")) {
                    meta->cyclebasetime = atol(str_val);
                } else {
                    unmatched = 1;
                }
            } else {
                unmatched = 1;
            }
            if (unmatched) {
                int ret;
                ret = amf_parse_custom_metakey(sh, key, amf_type, num_val, str_val);
                if (ret < 0) {
                    // todo
                }
            }
        }
    }
    return 0;
}

static int parse_metadata(AVIOContext *io, SegHandler *sh, int64_t max_pos, FlvMetadata *meta)
{
    char buffer[32];
    int type = avio_r8(io);
    if (type != AMF_DATA_TYPE_STRING || amf_get_string(io, buffer, sizeof(buffer)) < 0) {
        return -1;
    }
    if (strcmp(buffer, "onMetaData") == 0) {
        logger(LOG_INFO, "probe ScriptData: onMetaData");
        return amf_parse_object(meta, io, sh, "onMetaData", max_pos, 0);
    }
    return -1;
}

typedef struct {
    int type;
    unsigned int size;
    unsigned int dts;
    unsigned int stream_id;
} FlvTag;

static int parse_flv_tag(AVIOContext *io, int64_t max_pos, FlvTag *tag)
{
    tag->type = (avio_r8(io) & 0x1F);
    tag->size = avio_rb24(io);
    tag->dts = avio_rb24(io);
    tag->dts |= (unsigned) avio_r8(io) << 24;
    tag->stream_id = avio_rb24(io);
    if (tag->size == 0) {
        logger(LOG_ERROR, "tag size == 0");
        return -1;
    }
    if (avio_tell(io) + tag->size + 4 > max_pos) {
        logger(LOG_WARN, "tag length exceeds max_pos");
        return -1;
    }
    return 0;
}

#define PROBE_SIZE 2000

int probe_flv_metadata(AVIOContext *io, SegHandler *sh, FlvMetadata *meta) 
{
    if (io == NULL) {
        logger(LOG_ERROR, "io context null");
        return -1;
    }
    int ret = -1;
    int64_t start_pos = avio_tell(io);
    int i;
    for (i = 0; i < 32; i++) {
        FlvTag tag = {0};
        if (parse_flv_tag(io, start_pos + PROBE_SIZE, &tag) < 0) {
            break;
        }
        int64_t tag_data_pos = avio_tell(io);
        if (tag.type == 18) {
            if (0 == parse_metadata(io, sh, tag_data_pos + tag.size, meta)) {
                logger(LOG_INFO, "process flv metadata success");
                ret = 0;
                break;
            }
        }
        avio_seek(io, tag_data_pos + tag.size + 4, SEEK_SET);
    }
    avio_seek(io, start_pos, SEEK_SET);
    return ret;
}