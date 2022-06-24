#ifndef NOTIFY_H_
#define NOTIFY_H_

#include "seg.h"

#define CONNECT_TIMEOUT 1
#define READ_TIMEOUT 2

#define PERIOD_SIZE 5

void set_notify_flag(int on);

typedef struct {

} SegmentNotify;

void segment_notify(const char *url, const char * session, const SegmentNotify *info);

void segment_notify_pipe(const char *url, const char * session, const SegmentNotify *info);

typedef struct {

} ChunkNotify;

void chunk_notify_pipe(const char *url, const char * session, const ChunkNotify *info);

typedef struct {
    const char *url;
} StatisNotify;

void statis_notify(const char *url, const char * session, const StatisNotify *info);

void statis_notify_pipe(const char *url, const char * session, const StatisNotify *info);

#endif