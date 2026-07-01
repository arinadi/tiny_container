/*
 * tiny_ipp_jni.c -- This file is part of tiny_container.
 *
 * Copyright (C) 2026 Caten Hu
 *
 * Tiny Container is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published
 * by the Free Software Foundation, either version 3 of the License,
 * or any later version.
 *
 * Tiny Container is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty
 * of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see http://www.gnu.org/licenses/.
 */

/*
 * tiny_ipp_jni.c – IPP server on a Unix domain socket.
 *
 * Listens at $cacheDir/run/cups/cups.sock.  For each connection:
 *   1. Parse HTTP/1.x POST request headers
 *   2. Decode IPP binary request (RFC 8010)
 *   3. Dispatch to handler (Print-Job / Validate-Job /
 *      Get-Printer-Attributes / Get-Jobs / Cancel-Job)
 *   4. Encode IPP binary response
 *   5. Write HTTP + IPP response, close connection
 *
 * For Print-Job the document data is spooled to a temp file and
 * forwarded to Android PrintManager via JNI upcall.
 *
 * Reference: ippsample (PWG), RFC 8010/8011.
 */

#include <jni.h>
#include <android/log.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <time.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>

#define LOG_TAG "TinyIpp-JNI"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

/* ================================================================= */
/*  Constants                                                         */
/* ================================================================= */

#define MAX_WORKERS       128
#define HDR_LINE_MAX       1024
#define MAX_NAME_LEN       256

/* IPP operation-ids we handle */
#define IPP_OP_PRINT_JOB              0x0002
#define IPP_OP_VALIDATE_JOB           0x0004
#define IPP_OP_CANCEL_JOB             0x0008
#define IPP_OP_GET_JOB_ATTRIBUTES     0x0009
#define IPP_OP_GET_JOBS               0x000A
#define IPP_OP_GET_PRINTER_ATTRIBUTES 0x000B
#define IPP_OP_CREATE_JOB             0x0005
#define IPP_OP_SEND_DOCUMENT          0x0006

/* CUPS-specific operations (for printer discovery) */
#define CUPS_OP_GET_DEFAULT  0x4001
#define CUPS_OP_GET_PRINTERS 0x4002
#define CUPS_OP_GET_CLASSES  0x4005

/* IPP status codes */
#define IPP_STATUS_OK                      0x0000
#define IPP_STATUS_OK_IGNORED_OR_SUBSTITUTED 0x0001
#define IPP_STATUS_ERROR_BAD_REQUEST       0x0400
#define IPP_STATUS_ERROR_NOT_FOUND         0x0406
#define IPP_STATUS_ERROR_INTERNAL          0x0500
#define IPP_STATUS_ERROR_NOT_ACCEPTING     0x0508

/* IPP attribute group tags */
#define IPP_TAG_OPERATION          0x01
#define IPP_TAG_JOB                0x02
#define IPP_TAG_END                0x03
#define IPP_TAG_PRINTER            0x04
#define IPP_TAG_UNSUPPORTED_GROUP  0x05

/* IPP value tags we need to read/write */
#define IPP_VTAG_INTEGER           0x21
#define IPP_VTAG_BOOLEAN           0x22
#define IPP_VTAG_ENUM              0x23
#define IPP_VTAG_TEXT_NOLANG       0x41
#define IPP_VTAG_NAME_NOLANG       0x42
#define IPP_VTAG_KEYWORD           0x44
#define IPP_VTAG_URI               0x45
#define IPP_VTAG_CHARSET           0x47
#define IPP_VTAG_NATURAL_LANGUAGE  0x48
#define IPP_VTAG_MIMETYPE          0x49

/* IPP job states */
#define IPP_JSTATE_PENDING         0x03
#define IPP_JSTATE_PENDING_HELD    0x04
#define IPP_JSTATE_COMPLETED       0x09
#define IPP_JSTATE_CANCELED        0x07

/* ================================================================= */
/*  Growable buffer (for building IPP responses)                     */
/* ================================================================= */

typedef struct {
    uint8_t *data;
    size_t   len;
    size_t   cap;
} buf_t;

static void buf_init(buf_t *b) {
    b->cap  = 4096;
    b->data = malloc(b->cap);
    b->len  = 0;
}

static void buf_grow(buf_t *b, size_t extra) {
    size_t need = b->len + extra;
    if (need <= b->cap) return;
    while (b->cap < need) b->cap *= 2;
    b->data = realloc(b->data, b->cap);
}

static void buf_write(buf_t *b, const void *src, size_t n) {
    buf_grow(b, n);
    memcpy(b->data + b->len, src, n);
    b->len += n;
}

static void buf_u16be(buf_t *b, uint16_t v) {
    uint8_t tmp[2] = { (uint8_t)(v >> 8), (uint8_t)v };
    buf_write(b, tmp, 2);
}

static void buf_u32be(buf_t *b, uint32_t v) {
    uint8_t tmp[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16),
                       (uint8_t)(v >> 8),  (uint8_t)v };
    buf_write(b, tmp, 4);
}

static void buf_attr_i32(buf_t *b, uint8_t vtag, const char *name, int32_t val) {
    uint8_t tmp[4];
    tmp[0] = (uint8_t)(val >> 24); tmp[1] = (uint8_t)(val >> 16);
    tmp[2] = (uint8_t)(val >> 8);  tmp[3] = (uint8_t)val;
    uint16_t nlen = (uint16_t)strlen(name);
    buf_write(b, &vtag, 1);
    buf_u16be(b, nlen);
    buf_write(b, name, nlen);
    buf_u16be(b, 4);
    buf_write(b, tmp, 4);
}

static void buf_attr_str(buf_t *b, uint8_t vtag, const char *name, const char *val) {
    uint16_t nlen = (uint16_t)strlen(name);
    uint16_t vlen = (uint16_t)strlen(val);
    buf_write(b, &vtag, 1);
    buf_u16be(b, nlen);
    buf_write(b, name, nlen);
    buf_u16be(b, vlen);
    buf_write(b, val, vlen);
}

static void buf_attr_kw(buf_t *b, const char *name, const char *val) {
    buf_attr_str(b, IPP_VTAG_KEYWORD, name, val);
}

static void buf_attr_bool(buf_t *b, const char *name, bool val) {
    uint8_t v = val ? 1 : 0;
    uint16_t nlen = (uint16_t)strlen(name);
    uint8_t vtag = IPP_VTAG_BOOLEAN;
    buf_write(b, &vtag, 1);
    buf_u16be(b, nlen);
    buf_write(b, name, nlen);
    buf_u16be(b, 1);
    buf_write(b, &v, 1);
}

static void buf_attr_enum(buf_t *b, const char *name, int32_t val) {
    buf_attr_i32(b, IPP_VTAG_ENUM, name, val);
}

static void buf_tag(buf_t *b, uint8_t tag) { buf_write(b, &tag, 1); }

static void buf_free(buf_t *b) { free(b->data); b->data = NULL; b->len = b->cap = 0; }

/* ================================================================= */
/*  IPP request parser state                                         */
/* ================================================================= */

typedef struct {
    uint16_t    operation_id;
    uint32_t    request_id;
    char        job_name       [MAX_NAME_LEN];
    char        doc_format     [MAX_NAME_LEN];
    char        user_name      [MAX_NAME_LEN];
    int32_t     target_job_id;
    bool        have_doc_format;
} ipp_request_t;

/* ================================================================= */
/*  Byte-order helpers                                                */
/* ================================================================= */

static inline uint16_t rd16be(const uint8_t *p) { return ((uint16_t)p[0] << 8) | p[1]; }
static inline uint32_t rd32be(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

/* ================================================================= */
/*  Socket I/O helpers                                                */
/* ================================================================= */

/** Read exactly n bytes (retries on EINTR, gives up on EOF/error). */
static int sock_read_full(int fd, void *buf, size_t n) {
    uint8_t *p = buf; size_t rem = n;
    while (rem) {
        ssize_t r = read(fd, p, rem);
        if (r <= 0) return (r == 0 || errno != EINTR) ? -1 : 0;
        p += r; rem -= (size_t)r;
    }
    return 0;
}

/** Read one line terminated by \n (includes \r stripping). Returns bytes read incl \0, 0=EOF. */
static int sock_read_line(int fd, char *buf, size_t max) {
    size_t i = 0;
    while (i + 1 < max) {
        char c;
        if (read(fd, &c, 1) != 1) return 0;
        if (c == '\r') continue;
        if (c == '\n') { buf[i] = '\0'; return (int)(i + 1); }
        buf[i++] = c;
    }
    buf[i] = '\0'; return (int)(i + 1);
}

/* ================================================================= */
/*  IPP request decoder (parses raw bytes, extracts key attributes)   */
/*  Used for non-Print-Job operations (tiny bodies).                 */
/* ================================================================= */

/**
 * Parse an IPP request from raw bytes.
 * On success returns 0 and fills `req`; on error returns -1.
 * Stops parsing when end-of-attributes tag (0x03) is reached.
 * Returns the document-data offset in *doc_ofs.
 */
static int ipp_parse_request(const uint8_t *data, size_t len,
                             ipp_request_t *req, size_t *doc_ofs)
{
    if (len < 8) return -1;

    /* header */
    req->operation_id = rd16be(data + 2);
    req->request_id   = rd32be(data + 4);

    memset(req->job_name,   0, sizeof(req->job_name));
    memset(req->doc_format, 0, sizeof(req->doc_format));
    memset(req->user_name,  0, sizeof(req->user_name));
    req->target_job_id  = -1;
    req->have_doc_format = false;

    size_t pos = 8;

    while (pos < len) {
        uint8_t tag = data[pos++];
        if (tag == IPP_TAG_END) { *doc_ofs = pos; return 0; }
        if (tag <= 0x0F) continue;

        if (pos + 2 > len) return -1;
        uint16_t nlen = rd16be(data + pos); pos += 2;
        if (pos + nlen + 2 > len) return -1;
        const char *name = (const char *)(data + pos); pos += nlen;
        uint16_t vlen = rd16be(data + pos); pos += 2;
        if (pos + vlen > len) return -1;
        const char *value = (const char *)(data + pos); pos += vlen;

        #define STREQ(s,n) ((nlen == sizeof(s)-1) && !memcmp(name, s, nlen))

        if (tag == IPP_VTAG_NAME_NOLANG && STREQ("job-name", 8)) {
            size_t cp = vlen < MAX_NAME_LEN-1 ? vlen : MAX_NAME_LEN-1;
            memcpy(req->job_name, value, cp);
        }
        else if (tag == IPP_VTAG_MIMETYPE && STREQ("document-format", 15)) {
            size_t cp = vlen < MAX_NAME_LEN-1 ? vlen : MAX_NAME_LEN-1;
            memcpy(req->doc_format, value, cp);
            req->have_doc_format = true;
        }
        else if (tag == IPP_VTAG_NAME_NOLANG && STREQ("requesting-user-name", 20)) {
            size_t cp = vlen < MAX_NAME_LEN-1 ? vlen : MAX_NAME_LEN-1;
            memcpy(req->user_name, value, cp);
        }
        else if (tag == IPP_VTAG_INTEGER && STREQ("job-id", 6)) {
            if (vlen >= 4) req->target_job_id = (int32_t)rd32be((const uint8_t *)value);
        }
        #undef STREQ
    }
    return -1;
}

/* ================================================================= */
/*  Streaming parser helpers (Print-Job only — no buffer limit)      */
/* ================================================================= */

static int write_all(int fd, const void *buf, size_t count);

/** Discard exactly n bytes from the socket. */
static int skip_socket_bytes(int fd, size_t n) {
    char trash[4096];
    while (n) {
        size_t chunk = n < sizeof(trash) ? n : sizeof(trash);
        if (sock_read_full(fd, trash, chunk) < 0) return -1;
        n -= chunk;
    }
    return 0;
}

/**
 * Read IPP attributes streamed from the socket.
 * Stops at end-tag (0x03); fills req->job_name / req->doc_format / req->user_name.
 * Does NOT set operation_id / request_id — caller already read the header.
 * Returns total attribute bytes read from socket (including values skipped).
 * Returns -1 on error.
 */
static int ipp_read_attrs_stream(int fd, ipp_request_t *req) {
    int total = 0;
    char name[256];
    #define STREQ_S(s) ((size_t)nlen == sizeof(s)-1 && !memcmp(name, s, sizeof(s)-1))

    while (1) {
        uint8_t tag;
        if (read(fd, &tag, 1) != 1) return -1;
        total++;

        if (tag == IPP_TAG_END) return total;
        if (tag <= 0x0F) continue;

        /* name-length (2 bytes BE) */
        uint8_t nb[2];
        if (sock_read_full(fd, nb, 2) < 0) return -1;
        total += 2;
        uint16_t nlen = ((uint16_t)nb[0] << 8) | nb[1];

        /* name (truncated to 255) */
        uint16_t rn = nlen < 255 ? nlen : 255;
        if (sock_read_full(fd, name, rn) < 0) return -1;
        total += nlen;
        if (nlen > 255 && skip_socket_bytes(fd, nlen - 255) < 0) return -1;

        /* value-length (2 bytes BE) */
        uint8_t vb[2];
        if (sock_read_full(fd, vb, 2) < 0) return -1;
        total += 2;
        uint16_t vlen = ((uint16_t)vb[0] << 8) | vb[1];

        /* extract known attributes; skip others */
        if (tag == IPP_VTAG_NAME_NOLANG && STREQ_S("job-name")) {
            uint16_t cp = vlen < MAX_NAME_LEN - 1 ? vlen : MAX_NAME_LEN - 1;
            if (sock_read_full(fd, req->job_name, cp) < 0) return -1;
            req->job_name[cp] = '\0';
            total += vlen;
            if (vlen > cp && skip_socket_bytes(fd, vlen - cp) < 0) return -1;
        }
        else if (tag == IPP_VTAG_MIMETYPE && STREQ_S("document-format")) {
            uint16_t cp = vlen < MAX_NAME_LEN - 1 ? vlen : MAX_NAME_LEN - 1;
            if (sock_read_full(fd, req->doc_format, cp) < 0) return -1;
            req->doc_format[cp] = '\0';
            req->have_doc_format = true;
            total += vlen;
            if (vlen > cp && skip_socket_bytes(fd, vlen - cp) < 0) return -1;
        }
        else if (tag == IPP_VTAG_NAME_NOLANG && STREQ_S("requesting-user-name")) {
            uint16_t cp = vlen < MAX_NAME_LEN - 1 ? vlen : MAX_NAME_LEN - 1;
            if (sock_read_full(fd, req->user_name, cp) < 0) return -1;
            req->user_name[cp] = '\0';
            total += vlen;
            if (vlen > cp && skip_socket_bytes(fd, vlen - cp) < 0) return -1;
        }
        else {
            /* skip this value entirely */
            if (skip_socket_bytes(fd, vlen) < 0) return -1;
            total += vlen;
        }
    }
    #undef STREQ_S
}

/** Stream n bytes from socket directly to a file. Returns 0 on success. */
static int stream_socket_to_file(int fd, const char *fpath, size_t n) {
    int out_fd = open(fpath, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (out_fd < 0) { LOGE("open(%s): %s", fpath, strerror(errno)); return -1; }

    char buf[65536];
    size_t rem = n;
    while (rem) {
        size_t chunk = rem < sizeof(buf) ? rem : sizeof(buf);
        if (sock_read_full(fd, buf, chunk) < 0) { close(out_fd); unlink(fpath); return -1; }
        if (write_all(out_fd, buf, chunk) < 0) { close(out_fd); unlink(fpath); return -1; }
        rem -= chunk;
    }
    close(out_fd);
    chmod(fpath, 0644);
    return 0;
}

/* ================================================================= */
/*  IPP response builders                                            */
/* ================================================================= */

/** Write IPP response header (version + status + request-id). */
static void ipp_resp_begin(buf_t *b, uint16_t status, uint32_t request_id) {
    uint8_t hdr[8];
    hdr[0] = 2; hdr[1] = 0;          /* version 2.0 */
    hdr[2] = (uint8_t)(status >> 8); hdr[3] = (uint8_t)status;
    hdr[4] = (uint8_t)(request_id >> 24); hdr[5] = (uint8_t)(request_id >> 16);
    hdr[6] = (uint8_t)(request_id >> 8);  hdr[7] = (uint8_t)request_id;
    buf_write(b, hdr, 8);
}

/** Build a standard success response with job attributes.
 *  job_state is an IPP enum value (IPP_JSTATE_PENDING=3, IPP_JSTATE_PENDING_HELD=4, etc.).
 *  job_state_message is a human-readable text (REQUIRED per RFC 8011). */
static void ipp_resp_print_job_ok(buf_t *b, uint32_t request_id,
                                  int32_t job_id, const char *server_uri,
                                  int32_t job_state,
                                  const char *job_state_message,
                                  const char *job_reasons) {
    ipp_resp_begin(b, IPP_STATUS_OK, request_id);

    /* Operation Attributes */
    buf_tag(b, IPP_TAG_OPERATION);
    buf_attr_str(b, IPP_VTAG_CHARSET,          "attributes-charset",            "utf-8");
    buf_attr_str(b, IPP_VTAG_NATURAL_LANGUAGE, "attributes-natural-language",   "en-us");
    buf_attr_str(b, IPP_VTAG_TEXT_NOLANG,      "status-message",                "successful-ok");

    /* Job Attributes */
    buf_tag(b, IPP_TAG_JOB);
    buf_attr_i32(b, IPP_VTAG_INTEGER, "job-id", job_id);
    buf_attr_enum(b, "job-state", job_state);
    buf_attr_str(b, IPP_VTAG_TEXT_NOLANG, "job-state-message", job_state_message);
    buf_attr_kw(b,  "job-state-reasons", job_reasons);

    /* job-uri */
    {
        char uri[512];
        snprintf(uri, sizeof(uri), "%s/ipp/print/job-%d", server_uri, (int)job_id);
        buf_attr_str(b, IPP_VTAG_URI, "job-uri", uri);
    }

    buf_tag(b, IPP_TAG_END);
}

/** Build Get-Printer-Attributes response. */
static void ipp_resp_printer_attrs(buf_t *b, uint32_t request_id) {
    ipp_resp_begin(b, IPP_STATUS_OK, request_id);

    buf_tag(b, IPP_TAG_OPERATION);
    buf_attr_str(b, IPP_VTAG_CHARSET,          "attributes-charset",           "utf-8");
    buf_attr_str(b, IPP_VTAG_NATURAL_LANGUAGE, "attributes-natural-language",  "en-us");
    buf_attr_str(b, IPP_VTAG_TEXT_NOLANG,      "status-message",               "successful-ok");

    buf_tag(b, IPP_TAG_PRINTER);
    /* printer-state is type2 enum → enum in response (3=idle) */
    buf_attr_enum(b, "printer-state", 3);
    buf_attr_str(b, IPP_VTAG_TEXT_NOLANG, "printer-state-message", "");
    buf_attr_kw(b, "printer-state-reasons", "none");
    buf_attr_kw(b, "ipp-versions-supported", "1.1");
    buf_attr_kw(b, "ipp-versions-supported", "2.0");
    buf_attr_kw(b, "ipp-versions-supported", "2.1");
    buf_attr_kw(b, "operations-supported", "Print-Job");
    buf_attr_kw(b, "operations-supported", "Create-Job");
    buf_attr_kw(b, "operations-supported", "Send-Document");
    buf_attr_kw(b, "operations-supported", "Validate-Job");
    buf_attr_kw(b, "operations-supported", "Cancel-Job");
    buf_attr_kw(b, "operations-supported", "Get-Job-Attributes");
    buf_attr_kw(b, "operations-supported", "Get-Jobs");
    buf_attr_kw(b, "operations-supported", "Get-Printer-Attributes");
    buf_attr_str(b, IPP_VTAG_MIMETYPE, "document-format-default", "application/pdf");
    buf_attr_str(b, IPP_VTAG_MIMETYPE, "document-format-supported", "application/pdf");
    buf_attr_str(b, IPP_VTAG_MIMETYPE, "document-format-supported", "application/postscript");
    buf_attr_str(b, IPP_VTAG_MIMETYPE, "document-format-supported", "image/jpeg");
    buf_attr_str(b, IPP_VTAG_MIMETYPE, "document-format-supported", "image/png");
    buf_attr_str(b, IPP_VTAG_MIMETYPE, "document-format-supported", "application/octet-stream");
    buf_attr_kw(b, "compression-supported", "none");
    buf_attr_kw(b, "color-supported", "true");
    buf_attr_kw(b, "sides-supported", "one-sided");
    buf_attr_bool(b, "printer-is-accepting-jobs", true);
    buf_attr_enum(b, "printer-type", 0);
    buf_attr_i32(b, IPP_VTAG_INTEGER, "queued-job-count", 0);
    {
        time_t now = time(NULL);
        buf_attr_i32(b, IPP_VTAG_INTEGER, "printer-up-time", (int32_t)now);
    }
    buf_attr_str(b, IPP_VTAG_TEXT_NOLANG, "printer-info", "Tiny Container Print");
    buf_attr_str(b, IPP_VTAG_TEXT_NOLANG, "printer-location", "");
    buf_attr_str(b, IPP_VTAG_TEXT_NOLANG, "printer-make-and-model", "Tiny IPP Bridge");
    buf_attr_str(b, IPP_VTAG_NAME_NOLANG, "printer-name", "TinyPrint");
    buf_attr_str(b, IPP_VTAG_URI, "printer-uri-supported", "ipp://localhost/ipp/print");

    buf_tag(b, IPP_TAG_END);
}

/** Build Get-Jobs response (empty job list). */
static void ipp_resp_get_jobs(buf_t *b, uint32_t request_id) {
    ipp_resp_begin(b, IPP_STATUS_OK, request_id);

    buf_tag(b, IPP_TAG_OPERATION);
    buf_attr_str(b, IPP_VTAG_CHARSET,          "attributes-charset",           "utf-8");
    buf_attr_str(b, IPP_VTAG_NATURAL_LANGUAGE, "attributes-natural-language",  "en-us");
    buf_attr_str(b, IPP_VTAG_TEXT_NOLANG,      "status-message",               "successful-ok");

    buf_tag(b, IPP_TAG_END);
}

/** Build CUPS-Get-Printers / CUPS-Get-Default response.
 *  Returns exactly one printer so it appears in lpstat / print dialogs. */
static void ipp_resp_cups_get_printers(buf_t *b, uint32_t request_id) {
    ipp_resp_begin(b, IPP_STATUS_OK, request_id);
    buf_tag(b, IPP_TAG_OPERATION);
    buf_attr_str(b, IPP_VTAG_CHARSET,          "attributes-charset",           "utf-8");
    buf_attr_str(b, IPP_VTAG_NATURAL_LANGUAGE, "attributes-natural-language",  "en-us");
    buf_attr_str(b, IPP_VTAG_TEXT_NOLANG,      "status-message",               "successful-ok");

    /* printer attributes (one printer group — mirror ipp_resp_printer_attrs) */
    buf_tag(b, IPP_TAG_PRINTER);
    buf_attr_str(b, IPP_VTAG_NAME_NOLANG, "printer-name",           "TinyPrint");
    buf_attr_str(b, IPP_VTAG_TEXT_NOLANG, "printer-info",           "Tiny Container Print");
    buf_attr_str(b, IPP_VTAG_TEXT_NOLANG, "printer-location",       "");
    buf_attr_str(b, IPP_VTAG_TEXT_NOLANG, "printer-make-and-model", "Tiny IPP Bridge");
    buf_attr_enum(b, "printer-state", 3);
    buf_attr_str(b, IPP_VTAG_TEXT_NOLANG, "printer-state-message", "");
    buf_attr_kw(b,  "printer-state-reasons",      "none");
    buf_attr_bool(b, "printer-is-accepting-jobs", true);
    /* printer-type: bitmask.  0x00000001=printer, 0x00000008=color */
    buf_attr_enum(b, "printer-type", 0);
    buf_attr_kw(b,  "color-supported",            "true");
    buf_attr_kw(b,  "sides-supported",            "one-sided");
    buf_attr_kw(b,  "ipp-versions-supported",     "1.1");
    buf_attr_kw(b,  "ipp-versions-supported",     "2.0");
    buf_attr_kw(b,  "ipp-versions-supported",     "2.1");
    buf_attr_kw(b,  "operations-supported",       "Print-Job");
    buf_attr_kw(b,  "operations-supported",       "Create-Job");
    buf_attr_kw(b,  "operations-supported",       "Send-Document");
    buf_attr_kw(b,  "operations-supported",       "Validate-Job");
    buf_attr_kw(b,  "operations-supported",       "Cancel-Job");
    buf_attr_kw(b,  "operations-supported",       "Get-Job-Attributes");
    buf_attr_kw(b,  "operations-supported",       "Get-Jobs");
    buf_attr_kw(b,  "operations-supported",       "Get-Printer-Attributes");
    buf_attr_str(b, IPP_VTAG_MIMETYPE, "document-format-default", "application/pdf");
    buf_attr_str(b, IPP_VTAG_MIMETYPE, "document-format-supported", "application/pdf");
    buf_attr_str(b, IPP_VTAG_MIMETYPE, "document-format-supported", "application/postscript");
    buf_attr_str(b, IPP_VTAG_MIMETYPE, "document-format-supported", "image/jpeg");
    buf_attr_str(b, IPP_VTAG_MIMETYPE, "document-format-supported", "image/png");
    buf_attr_str(b, IPP_VTAG_MIMETYPE, "document-format-supported", "application/octet-stream");
    buf_attr_kw(b,  "compression-supported", "none");
    buf_attr_str(b, IPP_VTAG_URI,  "printer-uri-supported", "ipp://localhost/ipp/print");
    buf_attr_i32(b, IPP_VTAG_INTEGER, "queued-job-count", 0);

    buf_tag(b, IPP_TAG_END);
}

/** Build a simple error response. */
static void ipp_resp_error(buf_t *b, uint16_t status, uint32_t request_id,
                           const char *msg) {
    ipp_resp_begin(b, status, request_id);

    buf_tag(b, IPP_TAG_OPERATION);
    buf_attr_str(b, IPP_VTAG_CHARSET,          "attributes-charset",           "utf-8");
    buf_attr_str(b, IPP_VTAG_NATURAL_LANGUAGE, "attributes-natural-language",  "en-us");
    buf_attr_str(b, IPP_VTAG_TEXT_NOLANG,      "status-message",               msg);

    buf_tag(b, IPP_TAG_END);
}

/* ================================================================= */
/*  HTTP response writer                                              */
/* ================================================================= */

static int write_all(int fd, const void *buf, size_t count) {
    const uint8_t *p = buf; size_t rem = count;
    while (rem) {
        ssize_t w = write(fd, p, rem);
        if (w <= 0) return -1;
        p += w; rem -= (size_t)w;
    }
    return 0;
}

static void log_hex(const char *prefix, const uint8_t *data, size_t len) {
    /* dump in 256-byte chunks so we can see the full IPP response body */
    for (size_t off = 0; off < len; off += 256) {
        char buf[1024]; size_t bo = 0;
        size_t chunk = (len - off) < 256 ? (len - off) : 256;
        for (size_t i = 0; i < chunk && bo + 3 < sizeof(buf); i++)
            bo += (size_t)snprintf(buf + bo, sizeof(buf) - bo,
                                   "%02x", data[off + i]);
        LOGI("%s[%zu..%zu] len=%zu: %s",
             prefix, off, off + chunk, len, buf);
    }
}

static int http_write_response(int fd, uint16_t status_code,
                               const uint8_t *body, size_t body_len) {
    char hdr[512];
    const char *status_text = (status_code == 200) ? "OK" : "Internal Server Error";
    int n = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: application/ipp\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n",
        (int)status_code, status_text, body_len);

    /* header */
    if (write_all(fd, hdr, (size_t)n) < 0) return -1;
    /* diagnostic: dump first 128 bytes of IPP response body */
    if (body_len >= 8)
        log_hex("IPP-RESP", body, body_len);
    /* body */
    if (body_len && write_all(fd, body, body_len) < 0) return -1;
    return 0;
}

/* ================================================================= */
/*  Server state                                                      */
/* ================================================================= */

typedef struct {
    JavaVM      *jvm;
    jclass       cls_tiny_ipp;
    jmethodID    mid_on_print_job;

    atomic_bool  running;
    int          listen_fd;
    pthread_t    accept_thread;

    /* worker thread list */
    pthread_mutex_t workers_lock;
    pthread_t       workers[MAX_WORKERS];
    int             worker_count;

    /* paths */
    char   spool_dir[512];
    char   jobs_dir[512];

    /* job id counter */
    pthread_mutex_t job_id_lock;
    int32_t         next_job_id;
} server_t;

static server_t g_srv;

/* ================================================================= */
/*  JNI upcall                                                        */
/* ================================================================= */

static void upcall_on_print_job(JNIEnv *env, const char *job_name,
                                const char *file_path, const char *doc_format)
{
    jstring j_name = (*env)->NewStringUTF(env, job_name);
    jstring j_path = (*env)->NewStringUTF(env, file_path);
    jstring j_fmt  = (*env)->NewStringUTF(env, doc_format);
    (*env)->CallStaticVoidMethod(env, g_srv.cls_tiny_ipp,
                                 g_srv.mid_on_print_job, j_name, j_path, j_fmt);
    (*env)->DeleteLocalRef(env, j_name);
    (*env)->DeleteLocalRef(env, j_path);
    (*env)->DeleteLocalRef(env, j_fmt);
}

/* ================================================================= */
/*  HTTP request parser                                               */
/* ================================================================= */

typedef struct {
    char method[16];
    char uri[256];
    long content_length;
    bool is_ipp;
    bool is_get;
    bool is_chunked;
} http_request_t;

/**
 * Read HTTP request line + headers from the socket.
 * Returns 0 on success, -1 on error / non-IPP / GET.
 */
static int http_parse_request(int fd, http_request_t *hreq) {
    char line[HDR_LINE_MAX];

    /* request line */
    if (sock_read_line(fd, line, sizeof(line)) <= 0) { LOGE("http: request line timeout/eof"); return -1; }

    {
        char *p = line;
        char *method = p;
        while (*p && *p != ' ') p++;
        if (*p != ' ') { LOGE("http: bad request line: %s", line); return -1; }
        *p++ = '\0';
        {
            char *uri = p;
            while (*p && *p != ' ') p++;
            if (*p != ' ') { LOGE("http: bad request line: %s", line); return -1; }
            *p++ = '\0';
            strncpy(hreq->method, method, sizeof(hreq->method) - 1);
            strncpy(hreq->uri,    uri,    sizeof(hreq->uri) - 1);
        }
    }

    hreq->content_length = -1;
    hreq->is_ipp = false;
    hreq->is_get = false;
    hreq->is_chunked = false;

    /* Allow GET (CUPS health-check), but only POST is IPP */
    if (strcmp(hreq->method, "POST")) {
        if (!strcmp(hreq->method, "GET")) { hreq->is_get = true; return 0; }
        LOGE("http: non-POST method: %s", hreq->method); return -1;
    }

    /* headers */
    for (;;) {
        char *hdrs;
        if (sock_read_line(fd, line, sizeof(line)) <= 0) { LOGE("http: headers timeout/eof"); return -1; }
        if (!line[0]) break; /* empty line = end of headers */

        /* look for Content-Length */
        hdrs = line;
        if (strncasecmp(hdrs, "content-length:", 15) == 0) {
            hdrs += 15;
            while (*hdrs == ' ' || *hdrs == '\t') hdrs++;
            hreq->content_length = atol(hdrs);
        }
        /* look for Content-Type: application/ipp */
        if (strncasecmp(hdrs, "content-type:", 13) == 0) {
            hdrs += 13;
            while (*hdrs == ' ' || *hdrs == '\t') hdrs++;
            if (strstr(hdrs, "application/ipp"))
                hreq->is_ipp = true;
        }
        /* detect Transfer-Encoding: chunked */
        if (strncasecmp(hdrs, "transfer-encoding:", 18) == 0) {
            hdrs += 18;
            while (*hdrs == ' ' || *hdrs == '\t') hdrs++;
            if (strstr(hdrs, "chunked") || strstr(hdrs, "Chunked"))
                hreq->is_chunked = true;
            LOGI("http: Transfer-Encoding: %s is_chunked=%d", hdrs, (int)hreq->is_chunked);
        }
    }

    LOGI("http: %s %s Content-Length=%ld is_ipp=%d is_get=%d",
         hreq->method, hreq->uri, hreq->content_length,
         (int)hreq->is_ipp, (int)hreq->is_get);

    if (!hreq->is_ipp && hreq->content_length <= 0) {
        LOGE("http: not IPP and no Content-Length");
        return -1;
    }
    return 0;
}

/* ================================================================= */
/*  IPP operation handlers (buffered path — non-Print-Job ops)       */
/* ================================================================= */

static void handle_ipp_from_buffer(int fd, const uint8_t *body, size_t body_len) {
    ipp_request_t req;
    size_t doc_offs;
    if (ipp_parse_request(body, body_len, &req, &doc_offs) < 0) {
        buf_t resp; buf_init(&resp);
        ipp_resp_error(&resp, IPP_STATUS_ERROR_BAD_REQUEST, 0, "Bad IPP request");
        http_write_response(fd, 200, resp.data, resp.len);
        buf_free(&resp);
        return;
    }

    LOGI("IPP op=0x%04x req=%u doc_fmt=%s job=%s user=%s",
         req.operation_id, req.request_id, req.doc_format,
         req.job_name, req.user_name);

    /* hex dump first 256 bytes for diagnostic ops */
    if (req.operation_id == IPP_OP_CREATE_JOB ||
        req.operation_id == IPP_OP_CANCEL_JOB ||
        req.operation_id == IPP_OP_SEND_DOCUMENT ||
        req.operation_id == CUPS_OP_GET_PRINTERS ||
        req.operation_id == IPP_OP_GET_PRINTER_ATTRIBUTES) {
        char hex[1024]; int off = 0;
        size_t dump_n = body_len < 256 ? body_len : 256;
        for (size_t i = 0; i < dump_n; i++) {
            off += snprintf(hex + off, sizeof(hex) - off,
                           "%02x%s", body[i], (i + 1) % 32 ? "" : "\n");
        }
        LOGI("IPP body[0..%zu]: %s", dump_n, hex);
    }

    switch (req.operation_id) {
    case IPP_OP_CREATE_JOB:
    {
        /* Return a job-id so the client can follow up with Send-Document */
        pthread_mutex_lock(&g_srv.job_id_lock);
        int32_t jid = g_srv.next_job_id++;
        pthread_mutex_unlock(&g_srv.job_id_lock);
        buf_t resp; buf_init(&resp);
        ipp_resp_print_job_ok(&resp, req.request_id, jid, "ipp://localhost",
                              IPP_JSTATE_PENDING, "Job created.", "none");
        http_write_response(fd, 200, resp.data, resp.len);
        buf_free(&resp);
        break;
    }
    case IPP_OP_VALIDATE_JOB:
    {
        buf_t resp; buf_init(&resp);
        ipp_resp_begin(&resp, IPP_STATUS_OK, req.request_id);
        buf_tag(&resp, IPP_TAG_OPERATION);
        buf_attr_str(&resp, IPP_VTAG_CHARSET,          "attributes-charset",           "utf-8");
        buf_attr_str(&resp, IPP_VTAG_NATURAL_LANGUAGE, "attributes-natural-language",  "en-us");
        buf_attr_str(&resp, IPP_VTAG_TEXT_NOLANG,      "status-message",               "successful-ok");
        buf_tag(&resp, IPP_TAG_END);
        http_write_response(fd, 200, resp.data, resp.len);
        buf_free(&resp);
        break;
    }
    case IPP_OP_GET_PRINTER_ATTRIBUTES:
    {
        buf_t resp; buf_init(&resp);
        ipp_resp_printer_attrs(&resp, req.request_id);
        http_write_response(fd, 200, resp.data, resp.len);
        buf_free(&resp);
        break;
    }
    case IPP_OP_GET_JOBS:
    {
        buf_t resp; buf_init(&resp);
        ipp_resp_get_jobs(&resp, req.request_id);
        http_write_response(fd, 200, resp.data, resp.len);
        buf_free(&resp);
        break;
    }
    case IPP_OP_CANCEL_JOB:
    case IPP_OP_GET_JOB_ATTRIBUTES:
    {
        buf_t resp; buf_init(&resp);
        ipp_resp_begin(&resp, IPP_STATUS_OK, req.request_id);
        buf_tag(&resp, IPP_TAG_OPERATION);
        buf_attr_str(&resp, IPP_VTAG_CHARSET,          "attributes-charset",           "utf-8");
        buf_attr_str(&resp, IPP_VTAG_NATURAL_LANGUAGE, "attributes-natural-language",  "en-us");
        buf_attr_str(&resp, IPP_VTAG_TEXT_NOLANG,      "status-message",               "successful-ok");
        buf_tag(&resp, IPP_TAG_END);
        http_write_response(fd, 200, resp.data, resp.len);
        buf_free(&resp);
        break;
    }
    case CUPS_OP_GET_PRINTERS:
    case CUPS_OP_GET_DEFAULT:
    case CUPS_OP_GET_CLASSES:
    {
        buf_t resp; buf_init(&resp);
        ipp_resp_cups_get_printers(&resp, req.request_id);
        http_write_response(fd, 200, resp.data, resp.len);
        buf_free(&resp);
        break;
    }
    default:
    {
        buf_t resp; buf_init(&resp);
        ipp_resp_error(&resp, IPP_STATUS_ERROR_BAD_REQUEST, req.request_id,
                       "Operation not supported");
        http_write_response(fd, 200, resp.data, resp.len);
        buf_free(&resp);
        break;
    }
    }
}

/* ================================================================= */
/*  Print-Job – streaming path (no size limit)                        */
/* ================================================================= */

static void upcall_print_job(const char *name, const char *fpath, const char *fmt) {
    JNIEnv *env = NULL;
    jboolean need_detach = JNI_FALSE;
    jint ret = (*g_srv.jvm)->GetEnv(g_srv.jvm, (void **)&env, JNI_VERSION_1_6);
    if (ret == JNI_EDETACHED) {
        if ((*g_srv.jvm)->AttachCurrentThread(g_srv.jvm, &env, NULL) == JNI_OK)
            need_detach = JNI_TRUE;
    }
    if (env) {
        upcall_on_print_job(env, name, fpath, fmt);
        if (need_detach) (*g_srv.jvm)->DetachCurrentThread(g_srv.jvm);
    } else {
        LOGE("JNI: cannot obtain env for upcall");
    }
}

static void handle_print_job_stream(int fd, uint32_t req_id, long remaining) {
    ipp_request_t req;
    memset(&req, 0, sizeof(req));

    int attrs_bytes = ipp_read_attrs_stream(fd, &req);
    if (attrs_bytes < 0) {
        LOGE("ipp_read_attrs_stream failed for req=%u", req_id);
        buf_t resp; buf_init(&resp);
        ipp_resp_error(&resp, IPP_STATUS_ERROR_BAD_REQUEST, req_id, "Bad IPP attributes");
        http_write_response(fd, 200, resp.data, resp.len);
        buf_free(&resp);
        return;
    }

    long doc_bytes = remaining - attrs_bytes;

    LOGI("IPP stream doc_fmt=%s job=%s user=%s (attrs=%d bytes, doc=%ld bytes)",
         req.doc_format, req.job_name, req.user_name, attrs_bytes, doc_bytes);

    if (doc_bytes <= 0) {
        buf_t resp; buf_init(&resp);
        ipp_resp_error(&resp, IPP_STATUS_ERROR_BAD_REQUEST, req_id, "No document data");
        http_write_response(fd, 200, resp.data, resp.len);
        buf_free(&resp);
        return;
    }

    /* stream doc to file – no size limit */
    pthread_mutex_lock(&g_srv.job_id_lock);
    int32_t jid = g_srv.next_job_id++;
    pthread_mutex_unlock(&g_srv.job_id_lock);
    char fpath[512];
    snprintf(fpath, sizeof(fpath), "%s/job_%06d.pdf", g_srv.jobs_dir, (int)jid);

    LOGI("streaming %ld bytes to %s", doc_bytes, fpath);
    if (stream_socket_to_file(fd, fpath, (size_t)doc_bytes) < 0) {
        buf_t resp; buf_init(&resp);
        ipp_resp_error(&resp, IPP_STATUS_ERROR_INTERNAL, req_id, "Failed to spool");
        http_write_response(fd, 200, resp.data, resp.len);
        buf_free(&resp);
        return;
    }

    upcall_print_job(req.job_name[0] ? req.job_name : "Untitled",
                     fpath,
                     req.have_doc_format ? req.doc_format : "application/pdf");

    buf_t resp; buf_init(&resp);
    ipp_resp_print_job_ok(&resp, req_id, jid, "ipp://localhost",
                          IPP_JSTATE_PENDING, "Job pending.", "none");
    http_write_response(fd, 200, resp.data, resp.len);
    buf_free(&resp);
}

/* ================================================================= */
/*  Worker thread                                                     */
/* ================================================================= */

/** Read all chunks from a chunked Transfer-Encoding body.
 *  Returns malloc'd buffer (caller must free), sets *out_len.
 *  Returns NULL on error. */
static uint8_t *read_chunked_body(int fd, size_t *out_len) {
    size_t cap = 8192;
    size_t len = 0;
    uint8_t *buf = malloc(cap);
    if (!buf) { LOGE("chunked: OOM"); return NULL; }

    while (1) {
        char line[64];
        if (sock_read_line(fd, line, sizeof(line)) <= 0) {
            LOGE("chunked: failed to read chunk-size line");
            free(buf); return NULL;
        }
        long chunk_size = strtol(line, NULL, 16);
        if (chunk_size < 0) { LOGE("chunked: bad chunk size: %s", line); free(buf); return NULL; }

        if (chunk_size == 0) {
            /* final chunk — read trailing CRLF + possible trailer headers */
            char trail[256];
            while (1) {
                if (sock_read_line(fd, trail, sizeof(trail)) <= 0) break;
                if (!trail[0]) break; /* empty line = end of trailers */
            }
            *out_len = len;
            return buf;
        }

        /* grow buffer if needed */
        while (len + (size_t)chunk_size > cap) {
            size_t newcap = cap * 2;
            uint8_t *newbuf = realloc(buf, newcap);
            if (!newbuf) { LOGE("chunked: realloc OOM"); free(buf); return NULL; }
            buf = newbuf; cap = newcap;
        }

        /* read chunk data */
        if (sock_read_full(fd, buf + len, (size_t)chunk_size) < 0) {
            LOGE("chunked: short read for chunk data (%ld bytes)", chunk_size);
            free(buf); return NULL;
        }
        len += (size_t)chunk_size;

        /* read trailing CRLF */
        char crlf[2];
        read(fd, crlf, 2);
    }
}

/** Handle a chunked IPP request body.
 *  For Print-Job / Send-Document we spool doc data to a file and upcall.
 *  For other operations we use the normal buffered path. */
static void handle_chunked_ipp(int fd, uint8_t *body, size_t body_len) {
    ipp_request_t req;
    size_t doc_offs;
    if (ipp_parse_request(body, body_len, &req, &doc_offs) < 0) {
        buf_t resp; buf_init(&resp);
        ipp_resp_error(&resp, IPP_STATUS_ERROR_BAD_REQUEST, 0, "Bad IPP request");
        http_write_response(fd, 200, resp.data, resp.len);
        buf_free(&resp);
        return;
    }

    LOGI("IPP chunked op=0x%04x req=%u doc_fmt=%s job=%s user=%s",
         req.operation_id, req.request_id, req.doc_format,
         req.job_name, req.user_name);

    if (req.operation_id == IPP_OP_PRINT_JOB ||
        req.operation_id == IPP_OP_SEND_DOCUMENT) {

        size_t doc_bytes = body_len - doc_offs;
        if (doc_bytes <= 0) {
            buf_t resp; buf_init(&resp);
            ipp_resp_error(&resp, IPP_STATUS_ERROR_BAD_REQUEST, req.request_id,
                           "No document data");
            http_write_response(fd, 200, resp.data, resp.len);
            buf_free(&resp);
            return;
        }

        pthread_mutex_lock(&g_srv.job_id_lock);
        int32_t jid = g_srv.next_job_id++;
        pthread_mutex_unlock(&g_srv.job_id_lock);
        char fpath[512];
        snprintf(fpath, sizeof(fpath), "%s/job_%06d.pdf", g_srv.jobs_dir, (int)jid);

        LOGI("chunked: writing %zu doc bytes to %s", doc_bytes, fpath);
        int out_fd = open(fpath, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (out_fd < 0) {
            LOGE("chunked: open(%s): %s", fpath, strerror(errno));
            buf_t resp; buf_init(&resp);
            ipp_resp_error(&resp, IPP_STATUS_ERROR_INTERNAL, req.request_id,
                           "Failed to spool");
            http_write_response(fd, 200, resp.data, resp.len);
            buf_free(&resp);
            return;
        }
        if (write_all(out_fd, body + doc_offs, doc_bytes) < 0) {
            LOGE("chunked: write(%s): %s", fpath, strerror(errno));
            close(out_fd); unlink(fpath);
            buf_t resp; buf_init(&resp);
            ipp_resp_error(&resp, IPP_STATUS_ERROR_INTERNAL, req.request_id,
                           "Failed to spool");
            http_write_response(fd, 200, resp.data, resp.len);
            buf_free(&resp);
            return;
        }
        close(out_fd);
        chmod(fpath, 0644);

        upcall_print_job(req.job_name[0] ? req.job_name : "Untitled",
                         fpath,
                         req.have_doc_format ? req.doc_format : "application/pdf");

        buf_t resp; buf_init(&resp);
        ipp_resp_print_job_ok(&resp, req.request_id, jid, "ipp://localhost",
                              IPP_JSTATE_PENDING, "Job pending.", "none");
        http_write_response(fd, 200, resp.data, resp.len);
        buf_free(&resp);
    } else {
        /* non-streaming operation — use normal buffered handler */
        handle_ipp_from_buffer(fd, body, body_len);
    }
}

static void *worker_func(void *arg) {
    int fd = (int)(intptr_t)arg;

    struct timeval tv = { .tv_sec = 30, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    http_request_t hreq;
    if (http_parse_request(fd, &hreq) != 0) {
        LOGE("http_parse_request failed — non-IPP or timeout"); close(fd); goto unregister; }

    /* CUPS sends GET as health-check between IPP operations.
     * Respond with minimal 200 so it doesn't think the server is dead. */
    if (hreq.is_get) {
        const char *ok = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n"
                          "Connection: close\r\n\r\n{}";
        write(fd, ok, strlen(ok));
        close(fd);
        goto unregister;
    }

    /* Handle chunked Transfer-Encoding */
    if (hreq.is_chunked) {
        LOGI("chunked body detected, reading chunks...");
        size_t body_len = 0;
        uint8_t *body = read_chunked_body(fd, &body_len);
        if (!body || body_len < 8) {
            LOGE("chunked: failed to read body or body too short (%zu bytes)", body_len);
            free(body);
            buf_t resp; buf_init(&resp);
            ipp_resp_error(&resp, IPP_STATUS_ERROR_BAD_REQUEST, 0,
                           body ? "Body too short" : "Failed to read chunked body");
            http_write_response(fd, 200, resp.data, resp.len);
            buf_free(&resp);
            close(fd);
            goto unregister;
        }
        handle_chunked_ipp(fd, body, body_len);
        free(body);
        close(fd);
        goto unregister;
    }

    if (hreq.content_length < 8) {
        buf_t resp; buf_init(&resp);
        ipp_resp_error(&resp, IPP_STATUS_ERROR_BAD_REQUEST, 0, "Body too short");
        http_write_response(fd, 200, resp.data, resp.len);
        buf_free(&resp);
        close(fd);
        goto unregister;
    }

    /* Read IPP header (8 bytes) to determine operation */
    uint8_t hdr[8];
    if (sock_read_full(fd, hdr, 8) < 0) { LOGE("short read for IPP header (Content-Length=%ld)", hreq.content_length); close(fd); goto unregister; }
    uint16_t op_id  = rd16be(hdr + 2);
    uint32_t req_id = rd32be(hdr + 4);
    long     remaining = hreq.content_length - 8;

    LOGI("worker op=0x%04x req=%u Content-Length=%ld remaining=%ld",
         op_id, req_id, hreq.content_length, remaining);

    if (op_id == IPP_OP_PRINT_JOB || op_id == IPP_OP_SEND_DOCUMENT) {
        handle_print_job_stream(fd, req_id, remaining);
    } else {
        /* buffered path — non-Print-Job ops have tiny bodies */
        size_t n = (size_t)remaining;
        uint8_t *body = malloc(n + 8);
        if (!body) { LOGE("OOM for IPP body"); close(fd); goto unregister; }
        memcpy(body, hdr, 8);
        if (sock_read_full(fd, body + 8, n) < 0) {
            LOGE("short read for IPP body");
            free(body); close(fd); goto unregister;
        }
        handle_ipp_from_buffer(fd, body, n + 8);
        free(body);
    }

    close(fd);

unregister:
    pthread_mutex_lock(&g_srv.workers_lock);
    for (int i = 0; i < g_srv.worker_count; i++) {
        if (pthread_equal(g_srv.workers[i], pthread_self())) {
            g_srv.workers[i] = g_srv.workers[g_srv.worker_count - 1];
            g_srv.worker_count--;
            break;
        }
    }
    pthread_mutex_unlock(&g_srv.workers_lock);

    return NULL;
}

/* ================================================================= */
/*  Accept thread                                                     */
/* ================================================================= */

static void *accept_func(void *arg) {
    (void)arg;
    LOGI("accept thread started (fd=%d)", g_srv.listen_fd);

    while (atomic_load_explicit(&g_srv.running, memory_order_acquire)) {

        int client_fd = accept(g_srv.listen_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(100000); continue;
            }
            LOGE("accept: %s", strerror(errno));
            break;
        }

        LOGI("accepted client fd=%d", client_fd);

        /* cap workers */
        pthread_mutex_lock(&g_srv.workers_lock);
        if (g_srv.worker_count >= MAX_WORKERS) {
            pthread_mutex_unlock(&g_srv.workers_lock);
            LOGE("too many workers, rejecting fd=%d", client_fd);
            /* send 503 and close */
            {
                const char *msg = "HTTP/1.1 503 Service Unavailable\r\n"
                                  "Content-Length: 0\r\nConnection: close\r\n\r\n";
                write(client_fd, msg, strlen(msg));
            }
            close(client_fd);
            continue;
        }

        pthread_t tid;
        if (pthread_create(&tid, NULL, worker_func,
                          (void *)(intptr_t)client_fd) == 0) {
            g_srv.workers[g_srv.worker_count++] = tid;
        } else {
            LOGE("pthread_create worker failed");
            close(client_fd);
        }
        pthread_mutex_unlock(&g_srv.workers_lock);
    }

    LOGI("accept thread stopped");
    return NULL;
}

/* ================================================================= */
/*  JNI entry points                                                  */
/* ================================================================= */

JNIEXPORT jboolean JNICALL
Java_com_fct_tc4_TinyIpp_nativeStart(JNIEnv *env, jclass cls, jstring socketPath)
{
    (void)cls;
    const char *path = (*env)->GetStringUTFChars(env, socketPath, NULL);
    if (!path) return JNI_FALSE;
    LOGI("nativeStart → %s", path);

    /* ----- init state ----- */
    memset(&g_srv, 0, sizeof(g_srv));
    g_srv.listen_fd = -1;
    atomic_init(&g_srv.running, true);
    pthread_mutex_init(&g_srv.workers_lock, NULL);
    pthread_mutex_init(&g_srv.job_id_lock, NULL);
    g_srv.next_job_id = 1;

    /* derive spool_dir / jobs_dir from socket path */
    {
        size_t plen = strlen(path);
        const char *last_slash = NULL;
        for (size_t i = 0; i < plen; i++)
            if (path[i] == '/') last_slash = path + i;

        if (last_slash) {
            size_t dlen = (size_t)(last_slash - path);
            if (dlen >= sizeof(g_srv.spool_dir)) dlen = sizeof(g_srv.spool_dir) - 1;
            memcpy(g_srv.spool_dir, path, dlen);
            g_srv.spool_dir[dlen] = '\0';
        } else {
            strncpy(g_srv.spool_dir, ".", sizeof(g_srv.spool_dir) - 1);
        }
        snprintf(g_srv.jobs_dir, sizeof(g_srv.jobs_dir),
                 "%s/jobs", g_srv.spool_dir);
    }

    /* ----- cache JNI refs ----- */
    {
        jclass local = (*env)->FindClass(env, "com/fct/tc4/TinyIpp");
        g_srv.cls_tiny_ipp = (*env)->NewGlobalRef(env, local);
        (*env)->DeleteLocalRef(env, local);
    }
    g_srv.mid_on_print_job = (*env)->GetStaticMethodID(env, g_srv.cls_tiny_ipp,
        "onPrintJob", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V");
    if (!g_srv.mid_on_print_job) {
        LOGE("GetStaticMethodID(onPrintJob) failed");
        (*env)->DeleteGlobalRef(env, g_srv.cls_tiny_ipp);
        (*env)->ReleaseStringUTFChars(env, socketPath, path);
        return JNI_FALSE;
    }

    (*env)->GetJavaVM(env, &g_srv.jvm);

    /* ----- bind & listen ----- */
    {
        struct sockaddr_un addr;
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) { LOGE("socket: %s", strerror(errno)); goto fail; }

        /* unlink stale socket */
        unlink(path);

        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

        if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            LOGE("bind(%s): %s", path, strerror(errno));
            close(fd);
            goto fail;
        }
        /* make socket accessible to container processes
         * that run under the same UID (proot shares UID) */
        chmod(path, 0666);

        if (listen(fd, 8) < 0) {
            LOGE("listen: %s", strerror(errno));
            close(fd); unlink(path);
            goto fail;
        }

        g_srv.listen_fd = fd;
    }

    (*env)->ReleaseStringUTFChars(env, socketPath, path);

    /* ----- launch accept thread ----- */
    if (pthread_create(&g_srv.accept_thread, NULL, accept_func, NULL) != 0) {
        LOGE("pthread_create accept failed");
        close(g_srv.listen_fd); g_srv.listen_fd = -1;
        goto fail;
    }

    LOGI("IPP server ready: %s", g_srv.spool_dir);
    return JNI_TRUE;

fail:
    (*env)->DeleteGlobalRef(env, g_srv.cls_tiny_ipp);
    (*env)->ReleaseStringUTFChars(env, socketPath, path);
    return JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_fct_tc4_TinyIpp_nativeStop(JNIEnv *env, jclass cls)
{
    (void)env; (void)cls;
    LOGI("nativeStop");

    atomic_store_explicit(&g_srv.running, false, memory_order_release);

    /* close listen fd → accept() returns */
    if (g_srv.listen_fd >= 0) {
        shutdown(g_srv.listen_fd, SHUT_RDWR);
        close(g_srv.listen_fd);
        g_srv.listen_fd = -1;
    }

    /* join accept thread */
    pthread_join(g_srv.accept_thread, NULL);

    /* join remaining workers */
    pthread_mutex_lock(&g_srv.workers_lock);
    for (int i = 0; i < g_srv.worker_count; i++) {
        pthread_join(g_srv.workers[i], NULL);
    }
    g_srv.worker_count = 0;
    pthread_mutex_unlock(&g_srv.workers_lock);

    /* release JNI refs */
    if (g_srv.cls_tiny_ipp) {
        (*env)->DeleteGlobalRef(env, g_srv.cls_tiny_ipp);
        g_srv.cls_tiny_ipp = NULL;
    }

    pthread_mutex_destroy(&g_srv.workers_lock);
    pthread_mutex_destroy(&g_srv.job_id_lock);

    LOGI("nativeStop complete");
}
