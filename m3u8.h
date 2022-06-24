#ifndef M3U8_H_
#define M3U8_H_

#define SLICE_NUM 10

typedef struct {
    int duration;
    char path[128];
} M3U8Slice;

typedef struct {
    int discontinuity_before;
} M3U8SliceProps;

typedef struct {
    int duration;
    int sequence;
    M3U8Slice slice[SLICE_NUM];
} M3U8Context;

void m3u8_get_default_slice_props(M3U8SliceProps *props);

int m3u8_begin(const char *filename, int duration, M3U8Context *ctx);

int m3u8_input_slice(const char *filename, const char *slice, int duration, M3U8Context *ctx, M3U8SliceProps *props);

int m3u8_end(const char *filename, M3U8Context *ctx);

#endif