#include "notify.h"
#include "log.h"
#include <stdlib.h>
#include <curl/curl.h>

static int g_on = 1;

void set_notify_flag(int flag) {
    g_on = flag;
}

static char *curl_urlencode(const char *str) 
{
    CURL *curl = curl_easy_init();
    char *v = curl_easy_escape(curl, str, strlen(str));
    curl_easy_cleanup(curl);
    return v;
}

static int http_send(const char *url, const char *data) 
{
    if (!g_on) {
        logger(LOG_WARN, "notify is off");
        return 0;
    }

    CURL *curl  = curl_easy_init();
    if(!curl) {
        logger(LOG_ERROR, "curl_easy_init fail");
        return -1;
    }

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Connection: close");

    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, CONNECT_TIMEOUT);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, READ_TIMEOUT);

    if(data) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, data);
    }

    logger(LOG_INFO, "http send url: %s", url);

    long code = -1;

    CURLcode res = curl_easy_perform(curl);
    if(res == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
        logger(LOG_INFO, "notify response code = %ld", code);
    } else {
        logger(LOG_ERROR, "curl easy perform fail[%d]: %s", res, curl_easy_strerror(res));
    }

    curl_slist_free_all(headers);

    curl_easy_cleanup(curl);
    return (int) code;
}

void segment_notify(const char *url, const char *session, const SegmentNotify *info)
{
    // todo
}

void segment_notify_pipe(const char *url, const char *session, const SegmentNotify *info)
{
    // todo
}

void chunk_notify_pipe(const char *url, const char *session, const ChunkNotify *info)
{
    // todo
}

void statis_notify(const char *url, const char *session, const StatisNotify *info)
{
    // todo
}

void statis_notify_pipe(const char *url, const char *session, const StatisNotify *info)
{
    // todo
}
