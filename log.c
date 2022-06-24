#include "log.h"
#include <time.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>

#define LOG_BUFFER_SIZE 4096

static const char *LV[] = {"ERROR", "WARN", "INFO", "VERB", "DEBUG"};
static int g_level = LOG_INFO;
static const char *g_filename = NULL;
static FILE *g_fp = NULL;
static time_t g_opentime = 0;
static char g_lastmsg[LOG_BUFFER_SIZE];
static int g_count = 0;

void logger_init(int level, const char *filename)
{
    g_level = level;
    g_filename = filename;
    if (filename) {
        g_fp = fopen(g_filename, "a+");
        g_opentime = time(NULL);
        if(!g_fp) {
            logger(LOG_ERROR, "cannot open log file [%s]", g_filename);
        }
    }
}

void logger_uninit()
{
    if(g_fp) {
        fclose(g_fp);
        g_fp = NULL;
    }
}

static void _logger(int level, const char *msg) 
{
    char date[64] = {0};
    struct timeval tp;
    gettimeofday(&tp, NULL);
    struct tm now_tm;
    localtime_r(&tp.tv_sec, &now_tm);
    strftime(date, sizeof(date) - 1, "%Y-%m-%d %H:%M:%S", &now_tm);
    char buffer[LOG_BUFFER_SIZE] = {0};
    snprintf(buffer, sizeof(buffer) - 1, "%s.%03ld %5s - %s\r\n", date, tp.tv_usec / 1000, LV[level], msg);

    if(g_fp) {
        fputs(buffer, g_fp);
        fflush(g_fp);
    }
}

void logger(int level, const char * fmt, ...)
{
    if (level > g_level) {
        return;
    }

    if(!g_fp && g_filename) {
        g_fp = fopen(g_filename, "a+");
        g_opentime = time(NULL);
    }

    char msg[LOG_BUFFER_SIZE] = {0};
    va_list argp;
    va_start(argp, fmt);
    vsnprintf(msg, sizeof(msg) - 1, fmt, argp);
    va_end(argp);

    if (g_count > 0) {
        if(g_count >= 100 || strcmp(g_lastmsg, msg) != 0) {
            char buffer[64] = { 0 };
            snprintf(buffer, sizeof(buffer) - 1, "(((merged %d same messages)))", g_count);
            _logger(LOG_WARN, buffer);
            g_count = 0;
        }
    }

    if(strcmp(g_lastmsg, msg) == 0) {
        g_count++;
        return;
    }

    _logger(level, msg);

    strcpy(g_lastmsg, msg);
    if(g_fp) {
        if(time(NULL) - g_opentime >= 60) {
            fclose(g_fp);
            g_fp = NULL;
        }
    }
}

void logger_binary(int level, const char* prefix, unsigned char *buf, int len) 
{
    if (len > 64) {
        len = 64;
    }

    char hex[256] = {0};
    int i;
    for(i = 0; i < len; i++) {
        snprintf(hex + 3 * i, sizeof(hex) - 3 * i - 1, "%02X ", buf[i]);
    }
    logger(level, "%s: %s", prefix, hex);
}

int logger_level() 
{
    return g_level;
}