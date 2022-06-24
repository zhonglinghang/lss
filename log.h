#ifndef LOG_H_
#define LOG_H_

enum {
    LOG_ERROR = 0,
    LOG_WARN,
    LOG_INFO,
    LOG_VERB,
    LOG_DEBUG,
};

void logger_init(int level, const char*filename);
void logger_uninit();
void logger(int level, const char *fmt, ...);

void logger_binary(int level, const char *prefix, unsigned char *buf, int len);

#define av_error(words, ec) logger(LOG_ERROR, "[ffmpeg] %s error: %d (%s)", words, ec, av_err2str(ec))

int logger_level();

#endif