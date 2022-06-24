#include "m3u8.h"
#include "log.h"
#include <stdio.h>
#include <string.h>

void m3u8_get_default_slice_props(M3U8SliceProps *props)
{
    if (!props) {
        return;
    }
    memset(props, 0, sizeof(*props));
}

int m3u8_begin(const char *filename, int duration, M3U8Context *ctx)
{
    if (ctx) {
        // initialize and return
        memset(ctx, 0, sizeof(M3U8Context));
        ctx->duration = duration;
    } else {
        FILE *fp = fopen(filename, "w");
        if (!fp) {
            logger(LOG_ERROR, "open m3u8[%s] failed.", filename);
            return -1;
        }

        fprintf(fp, "#EXTM3U\r\n");
        fprintf(fp, "#EXT-X-VERSION:3\r\n");
        fprintf(fp, "#EXT-X-TARGETDURATION:%d\r\n", duration);

        fclose(fp);
    }
    return 0;
}

int m3u8_input_slice(const char *filename, const char *slice, int duration, M3U8Context *ctx, M3U8SliceProps *props)
{
    if (ctx) {
        FILE *fp = fopen(filename, "w");
        if (!fp) {
            logger(LOG_ERROR, "open m3u8[%s] failed.", filename);
            return -1;
        }

        M3U8Slice *info = &(ctx->slice[ctx->sequence % SLICE_NUM]);
        if (info->duration > 0) {
            remove(info->path);
        }
        info->duration = duration;
        strncpy(info->path, slice, sizeof(info->path) - 1);
        ctx->sequence++;

        fprintf(fp, "#EXTM3U\r\n");
        fprintf(fp, "#EXT-X-VERSION:3\r\n");
        fprintf(fp, "#EXT-X-MEDIA-SEQUENCE:%d\r\n", ctx->sequence);
        fprintf(fp, "#EXT-X-TARGETDURATION:%d\r\n", ctx->duration);

        int i;
        for (i = 0; i < SLICE_NUM; i++) {
            M3U8Slice *info = &(ctx->slice[(ctx->sequence + i) % SLICE_NUM]);
            if (info->duration > 0) {
                fprintf(fp, "EXTINF:%g,\r\n", (float) info->duration / 1000);
                fprintf(fp, "%s\r\n", info->path);
            }
        }
        fclose(fp);
    } else {
        FILE *fp = fopen(filename, "a");
        if (!fp) {
            logger(LOG_ERROR, "open m3u8[%s] failed.", filename);
            return -1;
        }

        if (props) {
            if (props->discontinuity_before) {
                fprintf(fp, "#EXT-X-DISCONTINUITY\r\n");
            }
        }

        fprintf(fp, "#EXTINF:%g,\r\n", (float) duration / 1000);
        fprintf(fp, "%s\r\n", slice);

        fclose(fp);
    }
    return 0;
}

int m3u8_end(const char *filename, M3U8Context *ctx) 
{
    if (ctx) {
        // no end label
    } else {
        FILE *fp = fopen(filename, "a");
        if (!fp) {
            logger(LOG_ERROR, "open m3u8[%s] failed.", filename);
            return -1;
        }

        fprintf(fp, "#EXT-X-ENDLIST\r\n");
        fclose(fp);
    }
    return 0;
}