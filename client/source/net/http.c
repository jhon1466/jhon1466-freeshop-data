#include "http.h"

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} MemBuffer;

static size_t mem_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    MemBuffer *buf = (MemBuffer *)userdata;
    size_t add = size * nmemb;
    if (buf->len + add + 1 > buf->cap) {
        size_t new_cap = buf->cap == 0 ? 4096 : buf->cap;
        while (new_cap < buf->len + add + 1) new_cap *= 2;
        char *grown = (char *)realloc(buf->data, new_cap);
        if (!grown) return 0; // signals error to curl (short write)
        buf->data = grown;
        buf->cap = new_cap;
    }
    memcpy(buf->data + buf->len, ptr, add);
    buf->len += add;
    buf->data[buf->len] = '\0';
    return add;
}

static void set_curl_error(char *err_buf, size_t err_buf_size, CURLcode code) {
    if (!err_buf || err_buf_size == 0) return;
    snprintf(err_buf, err_buf_size, "%s", curl_easy_strerror(code));
}

HttpResult http_get(const char *url, char **out_buf, size_t *out_len,
                     char *err_buf, size_t err_buf_size) {
    CURL *curl = curl_easy_init();
    if (!curl) {
        if (err_buf) snprintf(err_buf, err_buf_size, "curl_easy_init failed");
        return HTTP_ERR_INIT;
    }

    MemBuffer buf = {0};
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, mem_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "freeshop-client/0.1");

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        free(buf.data);
        set_curl_error(err_buf, err_buf_size, res);
        return HTTP_ERR_REQUEST;
    }

    *out_buf = buf.data ? buf.data : strdup("");
    *out_len = buf.len;
    return HTTP_OK;
}

typedef struct {
    HttpProgressCallback cb;
    void *userdata;
} ProgressCtx;

static int xfer_progress_cb(void *clientp, curl_off_t dltotal, curl_off_t dlnow,
                             curl_off_t ultotal, curl_off_t ulnow) {
    (void)ultotal;
    (void)ulnow;
    ProgressCtx *ctx = (ProgressCtx *)clientp;
    if (ctx->cb) ctx->cb((long)dltotal, (long)dlnow, ctx->userdata);
    return 0; // non-zero would abort the transfer
}

HttpResult http_download_to_file(const char *url, const char *dest_path,
                                  HttpProgressCallback cb, void *userdata,
                                  char *err_buf, size_t err_buf_size) {
    FILE *fp = fopen(dest_path, "wb");
    if (!fp) {
        if (err_buf) snprintf(err_buf, err_buf_size, "could not open %s for writing", dest_path);
        return HTTP_ERR_FILE;
    }

    CURL *curl = curl_easy_init();
    if (!curl) {
        fclose(fp);
        if (err_buf) snprintf(err_buf, err_buf_size, "curl_easy_init failed");
        return HTTP_ERR_INIT;
    }

    ProgressCtx ctx = { .cb = cb, .userdata = userdata };

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, fwrite);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, xfer_progress_cb);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "freeshop-client/0.1");

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    fclose(fp);

    if (res != CURLE_OK) {
        set_curl_error(err_buf, err_buf_size, res);
        remove(dest_path);
        return HTTP_ERR_REQUEST;
    }

    return HTTP_OK;
}
