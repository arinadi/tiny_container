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
 * tiny_ipp_jni.c – IPP (RFC 8010/8011) server on a Unix domain socket.
 *
 * Listens at $cacheDir/run/cups/cups.sock.  For each connection:
 *   1. Parse HTTP/1.x request headers (incl. Expect: 100-continue)
 *   2. Decode IPP binary request (attributes incl. collections)
 *   3. Dispatch to handler (Print-Job / Create-Job / Send-Document /
 *      Close-Job / Validate-Job / Get-Printer-Attributes / Get-Jobs /
 *      Get-Job-Attributes / Cancel-Job / CUPS-Get-*)
 *   4. Encode IPP binary response
 *   5. Write HTTP + IPP response, close connection
 *
 * A small job registry tracks state (pending → processing → completed /
 * canceled).  Documents are spooled to $cacheDir/run/cups/jobs/ and
 * forwarded to the Android Print framework via JNI upcall once the job
 * is complete (Print-Job, Send-Document last-document=true, Close-Job).
 *
 * Job template attributes (media, media-col, orientation-requested,
 * sides, print-color-mode, printer-resolution, copies) are parsed and
 * forwarded so the Android print dialog can be pre-configured.
 *
 * Reference: ippsample (PWG), RFC 8010/8011.
 */

#include <jni.h>
#include <android/log.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <errno.h>
#include <ctype.h>
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

#define MAX_WORKERS        128
#define HDR_LINE_MAX       1024
#define MAX_NAME_LEN       256
#define ATTR_SECTION_MAX   (1u << 20)   /* 1 MiB cap for IPP attributes */
#define MAX_JOBS           64
#define JOB_DONE_TIMEOUT   300          /* s; auto-complete stale jobs  */

/* IPP operation-ids we handle */
#define IPP_OP_PRINT_JOB              0x0002
#define IPP_OP_VALIDATE_JOB           0x0004
#define IPP_OP_CREATE_JOB             0x0005
#define IPP_OP_SEND_DOCUMENT          0x0006
#define IPP_OP_CANCEL_JOB             0x0008
#define IPP_OP_GET_JOB_ATTRIBUTES     0x0009
#define IPP_OP_GET_JOBS               0x000A
#define IPP_OP_GET_PRINTER_ATTRIBUTES 0x000B
#define IPP_OP_CLOSE_JOB              0x003B

/* CUPS-specific operations (for printer discovery) */
#define CUPS_OP_GET_DEFAULT  0x4001
#define CUPS_OP_GET_PRINTERS 0x4002
#define CUPS_OP_GET_CLASSES  0x4005
#define CUPS_OP_GET_PPD      0x400F

/* IPP status codes */
#define IPP_STATUS_OK                  0x0000
#define IPP_STATUS_ERROR_BAD_REQUEST   0x0400
#define IPP_STATUS_ERROR_NOT_FOUND     0x0406
#define IPP_STATUS_ERROR_INTERNAL      0x0500

/* IPP attribute group tags */
#define IPP_TAG_OPERATION          0x01
#define IPP_TAG_JOB                0x02
#define IPP_TAG_END                0x03
#define IPP_TAG_PRINTER            0x04

/* IPP value tags */
#define IPP_VTAG_INTEGER           0x21
#define IPP_VTAG_BOOLEAN           0x22
#define IPP_VTAG_ENUM              0x23
#define IPP_VTAG_RESOLUTION        0x32
#define IPP_VTAG_RANGE             0x33
#define IPP_VTAG_BEGCOLLECTION     0x34
#define IPP_VTAG_ENDCOLLECTION     0x37
#define IPP_VTAG_TEXT_NOLANG       0x41
#define IPP_VTAG_NAME_NOLANG       0x42
#define IPP_VTAG_KEYWORD           0x44
#define IPP_VTAG_URI               0x45
#define IPP_VTAG_CHARSET           0x47
#define IPP_VTAG_NATURAL_LANGUAGE  0x48
#define IPP_VTAG_MIMETYPE          0x49
#define IPP_VTAG_MEMBERNAME        0x4A

/* IPP job states */
#define JSTATE_PENDING     3
#define JSTATE_PROCESSING  5
#define JSTATE_CANCELED    7
#define JSTATE_COMPLETED   9

/* printer-type bitmask (CUPS 2.x convention) */
#define PRINTER_TYPE  0x000018DC  /* COLOR|DUPLEX|COPIES|COLLATE|MEDIUM|LARGE */

/* ================================================================= */
/*  Supported media sizes (dimensions in hundredths of a millimeter)  */
/* ================================================================= */

typedef struct {
    const char *name;
    int32_t     width;
    int32_t     height;
} media_size_t;

static const media_size_t media_table[] = {
    { "iso_a4_210x297mm",        21000, 29700 },
    { "na_letter_8.5x11in",      21590, 27940 },
    { "na_legal_8.5x14in",       21590, 35560 },
    { "iso_a5_148x210mm",        14800, 21000 },
    { "iso_a3_297x420mm",        29700, 42000 },
    { "iso_b5_176x250mm",        17600, 25000 },
    { "na_5x7_5x7in",            12700, 17780 },
    { "na_index-4x6_4x6in",      10160, 15240 },
    { "na_number-10_4.125x9.5in", 10477, 24130 },
};
#define MEDIA_COUNT ((int)(sizeof(media_table) / sizeof(media_table[0])))

/* PPD view of the same sizes (PostScript points, 1pt = 1/72in).
 * Must stay in sync with media_table above. */
typedef struct {
    const char *ppd_name;
    const char *label;
    int32_t     width_pt;
    int32_t     height_pt;
} ppd_media_t;

static const ppd_media_t ppd_media_table[] = {
    { "A4",     "A4",              595,  842  },
    { "Letter", "US Letter",       612,  792  },
    { "Legal",  "US Legal",        612,  1008 },
    { "A5",     "A5",              420,  595  },
    { "A3",     "A3",              842,  1191 },
    { "B5",     "B5",              499,  709  },
    { "5x7",    "5x7in",           360,  504  },
    { "4x6",    "4x6in",           288,  432  },
    { "Env10",  "#10 Envelope",    297,  684  },
};
#define PPD_MEDIA_COUNT ((int)(sizeof(ppd_media_table) / sizeof(ppd_media_table[0])))
#define PPD_MARGIN_PT 18   /* 635/100 mm */

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
    uint16_t nlen = (uint16_t)strlen(name);
    buf_write(b, &vtag, 1);
    buf_u16be(b, nlen);
    buf_write(b, name, nlen);
    buf_u16be(b, 4);
    buf_u32be(b, (uint32_t)val);
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

/* Multi-valued (1setOf) strings: subsequent values use zero name-length. */
static void buf_attr_strs(buf_t *b, uint8_t vtag, const char *name,
                          const char *const *vals, int count) {
    for (int i = 0; i < count; i++) {
        buf_write(b, &vtag, 1);
        if (i == 0) {
            buf_u16be(b, (uint16_t)strlen(name));
            buf_write(b, name, strlen(name));
        } else {
            buf_u16be(b, 0);
        }
        buf_u16be(b, (uint16_t)strlen(vals[i]));
        buf_write(b, vals[i], strlen(vals[i]));
    }
}

/* Multi-valued (1setOf) integers/enums. */
static void buf_attr_ints(buf_t *b, uint8_t vtag, const char *name,
                          const int32_t *vals, int count) {
    for (int i = 0; i < count; i++) {
        buf_write(b, &vtag, 1);
        if (i == 0) {
            buf_u16be(b, (uint16_t)strlen(name));
            buf_write(b, name, strlen(name));
        } else {
            buf_u16be(b, 0);
        }
        buf_u16be(b, 4);
        buf_u32be(b, (uint32_t)vals[i]);
    }
}

static void buf_attr_range(buf_t *b, const char *name, int32_t lo, int32_t hi) {
    uint8_t vtag = IPP_VTAG_RANGE;
    uint16_t nlen = (uint16_t)strlen(name);
    buf_write(b, &vtag, 1);
    buf_u16be(b, nlen);
    buf_write(b, name, nlen);
    buf_u16be(b, 8);
    buf_u32be(b, (uint32_t)lo);
    buf_u32be(b, (uint32_t)hi);
}

static void buf_attr_resolution(buf_t *b, const char *name,
                                int32_t xdpi, int32_t ydpi) {
    uint8_t vtag = IPP_VTAG_RESOLUTION;
    uint16_t nlen = (uint16_t)strlen(name);
    buf_write(b, &vtag, 1);
    buf_u16be(b, nlen);
    buf_write(b, name, nlen);
    buf_u16be(b, 9);
    buf_u32be(b, (uint32_t)xdpi);
    buf_u32be(b, (uint32_t)ydpi);
    buf_write(b, "\003", 1);    /* units = dots-per-inch */
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

/* ---- collection encoding helpers (RFC 8010 §3.1.7) ---- */

static void col_start(buf_t *b, const char *name /* NULL = continuation */) {
    uint8_t tag = IPP_VTAG_BEGCOLLECTION;
    buf_write(b, &tag, 1);
    if (name) {
        buf_u16be(b, (uint16_t)strlen(name));
        buf_write(b, name, strlen(name));
    } else {
        buf_u16be(b, 0);
    }
    buf_u16be(b, 0);            /* value-length always 0 */
}

static void col_end(buf_t *b) {
    uint8_t tag = IPP_VTAG_ENDCOLLECTION;
    buf_write(b, &tag, 1);
    buf_u16be(b, 0);
    buf_u16be(b, 0);
}

static void col_member_str(buf_t *b, uint8_t vtag, const char *mname, const char *mval) {
    uint8_t mt = IPP_VTAG_MEMBERNAME;
    buf_write(b, &mt, 1);
    buf_u16be(b, 0);
    buf_u16be(b, (uint16_t)strlen(mname));
    buf_write(b, mname, strlen(mname));
    buf_write(b, &vtag, 1);
    buf_u16be(b, 0);
    buf_u16be(b, (uint16_t)strlen(mval));
    buf_write(b, mval, strlen(mval));
}

static void col_member_int(buf_t *b, const char *mname, int32_t v) {
    uint8_t mt = IPP_VTAG_MEMBERNAME;
    uint8_t vt = IPP_VTAG_INTEGER;
    buf_write(b, &mt, 1);
    buf_u16be(b, 0);
    buf_u16be(b, (uint16_t)strlen(mname));
    buf_write(b, mname, strlen(mname));
    buf_write(b, &vt, 1);
    buf_u16be(b, 0);
    buf_u16be(b, 4);
    buf_u32be(b, (uint32_t)v);
}

static void col_media_size(buf_t *b, int32_t x, int32_t y) {
    uint8_t mt = IPP_VTAG_MEMBERNAME;
    buf_write(b, &mt, 1);
    buf_u16be(b, 0);
    buf_u16be(b, 10);
    buf_write(b, "media-size", 10);
    col_start(b, NULL);
    col_member_int(b, "x-dimension", x);
    col_member_int(b, "y-dimension", y);
    col_end(b);
}

/* Encode one media-col collection value (media-col-database/default/ready). */
static void put_media_col(buf_t *b, const char *attr_name /* NULL = continuation */,
                          const media_size_t *m) {
    col_start(b, attr_name);
    col_member_str(b, IPP_VTAG_KEYWORD, "media-key", m->name);
    col_member_str(b, IPP_VTAG_KEYWORD, "media-source", "auto");
    col_member_str(b, IPP_VTAG_KEYWORD, "media-type", "stationery");
    col_media_size(b, m->width, m->height);
    col_member_int(b, "media-bottom-margin", 635);
    col_member_int(b, "media-left-margin", 635);
    col_member_int(b, "media-right-margin", 635);
    col_member_int(b, "media-top-margin", 635);
    col_end(b);
}

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

static int write_all(int fd, const void *buf, size_t count);

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

/** Read one line terminated by \n (includes \r stripping). Returns >0 incl \0, 0=EOF. */
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

/* ================================================================= */
/*  IPP request representation                                        */
/* ================================================================= */

typedef struct {
    uint8_t     ver_major, ver_minor;
    uint16_t    operation_id;
    uint32_t    request_id;

    /* operation attributes */
    char        job_name    [MAX_NAME_LEN];
    char        doc_format  [MAX_NAME_LEN];
    char        user_name   [MAX_NAME_LEN];
    char        which_jobs  [64];
    int32_t     target_job_id;
    bool        have_doc_format;
    bool        last_document;
    bool        have_last_document;

    /* job template attributes (all optional) */
    int32_t     copies;
    int32_t     orientation;            /* 0=unknown, 3..6 per RFC 8011 */
    char        media      [128];
    int32_t     media_w, media_h;       /* hundredths of mm, 0=unknown */
    char        sides      [64];
    char        color_mode [64];
    int32_t     res_x, res_y;           /* dpi, 0=unknown */
    int32_t     print_quality;
} ipp_request_t;

/* ================================================================= */
/*  Cursor-based IPP attribute parser                                 */
/* ================================================================= */

typedef struct {
    const uint8_t *d;
    size_t         len;
    size_t         pos;
} cur_t;

static int cur_u8(cur_t *c, uint8_t *v) {
    if (c->pos >= c->len) return -1;
    *v = c->d[c->pos++];
    return 0;
}

static int cur_u16(cur_t *c, uint16_t *v) {
    if (c->pos + 2 > c->len) return -1;
    *v = rd16be(c->d + c->pos);
    c->pos += 2;
    return 0;
}

/** Read one attribute entry: tag, name-length, name, value-length, value. */
static int cur_entry(cur_t *c, uint8_t *tag, const uint8_t **name, uint16_t *nlen,
                     const uint8_t **val, uint16_t *vlen) {
    if (cur_u8(c, tag)) return -1;
    if (cur_u16(c, nlen)) return -1;
    if (c->pos + *nlen > c->len) return -1;
    *name = c->d + c->pos;
    c->pos += *nlen;
    if (cur_u16(c, vlen)) return -1;
    if (c->pos + *vlen > c->len) return -1;
    *val = c->d + c->pos;
    c->pos += *vlen;
    return 0;
}

/** Skip a collection body (called after its begCollection header was consumed). */
static int skip_collection(cur_t *c) {
    int depth = 1;
    while (depth > 0) {
        uint8_t tag; const uint8_t *n, *v; uint16_t nl, vl;
        if (cur_entry(c, &tag, &n, &nl, &v, &vl)) return -1;
        if (tag == IPP_VTAG_BEGCOLLECTION) depth++;
        else if (tag == IPP_VTAG_ENDCOLLECTION) depth--;
    }
    return 0;
}

/** Parse a media-size nested collection body. */
static int parse_media_size(cur_t *c, ipp_request_t *req) {
    for (;;) {
        uint8_t mtag; const uint8_t *mn, *mv; uint16_t mnl, mvl;
        if (cur_entry(c, &mtag, &mn, &mnl, &mv, &mvl)) return -1;
        if (mtag == IPP_VTAG_ENDCOLLECTION) return 0;
        if (mtag != IPP_VTAG_MEMBERNAME) continue;

        uint8_t vtag; const uint8_t *vn, *vv; uint16_t vnl, vvl;
        if (cur_entry(c, &vtag, &vn, &vnl, &vv, &vvl)) return -1;
        if (vtag == IPP_VTAG_BEGCOLLECTION) { if (skip_collection(c)) return -1; continue; }
        if (vtag == IPP_VTAG_INTEGER && vvl == 4) {
            int32_t iv = (int32_t)rd32be(vv);
            if (mvl == 11 && !memcmp(mv, "x-dimension", 11)) req->media_w = iv;
            else if (mvl == 11 && !memcmp(mv, "y-dimension", 11)) req->media_h = iv;
        }
    }
}

/** Parse a media-col collection body (called after its header was consumed). */
static int parse_media_col(cur_t *c, ipp_request_t *req) {
    for (;;) {
        uint8_t mtag; const uint8_t *mn, *mv; uint16_t mnl, mvl;
        if (cur_entry(c, &mtag, &mn, &mnl, &mv, &mvl)) return -1;
        if (mtag == IPP_VTAG_ENDCOLLECTION) return 0;
        if (mtag != IPP_VTAG_MEMBERNAME) continue;

        uint8_t vtag; const uint8_t *vn, *vv; uint16_t vnl, vvl;
        if (cur_entry(c, &vtag, &vn, &vnl, &vv, &vvl)) return -1;
        if (vtag == IPP_VTAG_BEGCOLLECTION) {
            if (mvl == 10 && !memcmp(mv, "media-size", 10)) {
                if (parse_media_size(c, req)) return -1;
            } else if (skip_collection(c)) return -1;
            continue;
        }
        /* scalar members: media-key (per RFC 8011 media-col wins over media) */
        if (mvl == 9 && !memcmp(mv, "media-key", 9) && vvl > 0 && vvl < sizeof(req->media)) {
            memcpy(req->media, vv, vvl);
            req->media[vvl] = '\0';
        }
    }
}

/**
 * Parse the IPP attribute section (starting at the first group tag,
 * i.e. right after the 8-byte header).  Stops at end-of-attributes (0x03).
 * Returns 0 on success and sets *doc_ofs to the document-data offset
 * (relative to `data`).
 */
static int ipp_parse_attrs(const uint8_t *data, size_t len,
                           ipp_request_t *req, size_t *doc_ofs)
{
    memset(req, 0, sizeof(*req));
    req->target_job_id = -1;
    req->copies        = 1;

    cur_t c = { data, len, 0 };

    for (;;) {
        uint8_t tag;
        if (cur_u8(&c, &tag)) return -1;
        if (tag == IPP_TAG_END) { *doc_ofs = c.pos; return 0; }
        if (tag <= 0x0F) continue;              /* group tag */

        /* value attribute header */
        uint16_t nlen, vlen;
        const uint8_t *name, *value;
        if (cur_u16(&c, &nlen)) return -1;
        if (c.pos + nlen > c.len) return -1;
        name = c.d + c.pos; c.pos += nlen;
        if (cur_u16(&c, &vlen)) return -1;
        if (c.pos + vlen > c.len) return -1;
        value = c.d + c.pos; c.pos += vlen;

        if (nlen == 0) continue;                /* 1setOf continuation: first wins */

        #define NAME_IS(s) (nlen == sizeof(s)-1 && !memcmp(name, s, sizeof(s)-1))

        if (tag == IPP_VTAG_BEGCOLLECTION) {
            if (NAME_IS("media-col")) {
                /* rewind so parse_media_col sees a fresh cursor at member list */
                cur_t mc = { data, len, c.pos };
                if (parse_media_col(&mc, req)) return -1;
                c.pos = mc.pos;
            } else {
                cur_t sc = { data, len, c.pos };
                if (skip_collection(&sc)) return -1;
                c.pos = sc.pos;
            }
            continue;
        }

        if (NAME_IS("job-name")) {
            size_t cp = vlen < MAX_NAME_LEN-1 ? vlen : MAX_NAME_LEN-1;
            memcpy(req->job_name, value, cp);
        }
        else if (NAME_IS("document-format")) {
            size_t cp = vlen < MAX_NAME_LEN-1 ? vlen : MAX_NAME_LEN-1;
            memcpy(req->doc_format, value, cp);
            req->have_doc_format = true;
        }
        else if (NAME_IS("requesting-user-name")) {
            size_t cp = vlen < MAX_NAME_LEN-1 ? vlen : MAX_NAME_LEN-1;
            memcpy(req->user_name, value, cp);
        }
        else if (NAME_IS("which-jobs")) {
            size_t cp = vlen < sizeof(req->which_jobs)-1 ? vlen : sizeof(req->which_jobs)-1;
            memcpy(req->which_jobs, value, cp);
        }
        else if (NAME_IS("media") && tag == IPP_VTAG_KEYWORD) {
            size_t cp = vlen < sizeof(req->media)-1 ? vlen : sizeof(req->media)-1;
            memcpy(req->media, value, cp);
        }
        else if (NAME_IS("sides")) {
            size_t cp = vlen < sizeof(req->sides)-1 ? vlen : sizeof(req->sides)-1;
            memcpy(req->sides, value, cp);
        }
        else if (NAME_IS("print-color-mode")) {
            size_t cp = vlen < sizeof(req->color_mode)-1 ? vlen : sizeof(req->color_mode)-1;
            memcpy(req->color_mode, value, cp);
        }
        else if (NAME_IS("job-id") && (tag == IPP_VTAG_INTEGER || tag == IPP_VTAG_ENUM) && vlen >= 4) {
            req->target_job_id = (int32_t)rd32be(value);
        }
        else if (NAME_IS("job-uri") && tag == IPP_VTAG_URI) {
            /* e.g. ipp://localhost/ipp/print/job-12 — take trailing digits */
            int i = (int)vlen;
            while (i > 0 && isdigit(value[i-1])) i--;
            if (i < (int)vlen) {
                char tmp[32];
                int dlen = (int)vlen - i;
                if (dlen >= (int)sizeof(tmp)) dlen = sizeof(tmp)-1;
                memcpy(tmp, value + i, dlen);
                tmp[dlen] = '\0';
                long id = atol(tmp);
                if (id > 0) req->target_job_id = (int32_t)id;
            }
        }
        else if (NAME_IS("last-document") && tag == IPP_VTAG_BOOLEAN && vlen >= 1) {
            req->last_document      = value[0] != 0;
            req->have_last_document = true;
        }
        else if (NAME_IS("copies") && (tag == IPP_VTAG_INTEGER || tag == IPP_VTAG_ENUM) && vlen >= 4) {
            req->copies = (int32_t)rd32be(value);
        }
        else if (NAME_IS("orientation-requested") && tag == IPP_VTAG_ENUM && vlen >= 4) {
            req->orientation = (int32_t)rd32be(value);
        }
        else if (NAME_IS("print-quality") && tag == IPP_VTAG_ENUM && vlen >= 4) {
            req->print_quality = (int32_t)rd32be(value);
        }
        else if (NAME_IS("printer-resolution") && tag == IPP_VTAG_RESOLUTION && vlen >= 9) {
            int32_t x = (int32_t)rd32be(value);
            int32_t y = (int32_t)rd32be(value + 4);
            if (value[8] == 4) {            /* dots-per-centimeter → dpi */
                x = x * 254 / 100;
                y = y * 254 / 100;
            }
            req->res_x = x;
            req->res_y = y;
        }
        #undef NAME_IS
    }
}

/**
 * Read the IPP attribute section from the socket into `out`
 * (including the final end-of-attributes tag).  Returns 0 on success.
 */
static int read_attr_section(int fd, buf_t *out) {
    buf_init(out);
    for (;;) {
        uint8_t tag;
        if (read(fd, &tag, 1) != 1) { buf_free(out); return -1; }
        buf_write(out, &tag, 1);
        if (out->len > ATTR_SECTION_MAX) { LOGE("attr section too large"); buf_free(out); return -1; }
        if (tag == IPP_TAG_END) return 0;
        if (tag <= 0x0F) continue;              /* group tag */

        uint8_t hdr2[2];
        if (sock_read_full(fd, hdr2, 2) < 0) { buf_free(out); return -1; }
        buf_write(out, hdr2, 2);
        uint16_t nlen = rd16be(hdr2);
        char tmp[1024];
        uint16_t rem = nlen;
        while (rem) {                           /* attribute name */
            uint16_t chunk = rem < sizeof(tmp) ? rem : sizeof(tmp);
            if (sock_read_full(fd, tmp, chunk) < 0) { buf_free(out); return -1; }
            buf_write(out, tmp, chunk);
            rem -= chunk;
        }
        if (sock_read_full(fd, hdr2, 2) < 0) { buf_free(out); return -1; }
        buf_write(out, hdr2, 2);
        uint16_t vlen = rd16be(hdr2);
        rem = vlen;
        while (rem) {                           /* attribute value */
            uint16_t chunk = rem < sizeof(tmp) ? rem : sizeof(tmp);
            if (sock_read_full(fd, tmp, chunk) < 0) { buf_free(out); return -1; }
            buf_write(out, tmp, chunk);
            rem -= chunk;
        }
    }
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

    pthread_mutex_t workers_lock;
    pthread_t       workers[MAX_WORKERS];
    int             worker_count;

    char   spool_dir[512];
    char   jobs_dir[512];

    pthread_mutex_t job_id_lock;
    int32_t         next_job_id;
} server_t;

static server_t g_srv;

/* ================================================================= */
/*  Job registry                                                      */
/* ================================================================= */

typedef struct {
    bool     used;
    int32_t  id;
    int      state;
    char     name [MAX_NAME_LEN];
    char     user [MAX_NAME_LEN];
    char     format[MAX_NAME_LEN];
    char     file [512];
    time_t   t_create, t_processing, t_done;
    bool     has_doc;
    bool     notified;               /* Android print dialog dispatched */

    /* job template attributes (forwarded to Android) */
    int32_t  copies;
    int32_t  orientation;
    char     media[128];
    int32_t  media_w, media_h;
    char     sides[64];
    char     color_mode[64];
    int32_t  res_x, res_y;
} job_t;

static job_t          g_jobs[MAX_JOBS];
static pthread_mutex_t g_jobs_lock;

static void job_apply_template(job_t *j, const ipp_request_t *req) {
    j->copies      = req->copies > 0 ? req->copies : 1;
    j->orientation = req->orientation;
    j->media_w     = req->media_w;
    j->media_h     = req->media_h;
    j->res_x       = req->res_x;
    j->res_y       = req->res_y;
    strncpy(j->media,      req->media,      sizeof(j->media) - 1);
    strncpy(j->sides,      req->sides,      sizeof(j->sides) - 1);
    strncpy(j->color_mode, req->color_mode, sizeof(j->color_mode) - 1);
}

/** Allocate a job slot; caller must NOT hold g_jobs_lock.  Returns job id or -1. */
static int32_t job_create(const ipp_request_t *req, const char *jobs_dir,
                          int32_t *id_out, job_t *snapshot_out)
{
    pthread_mutex_lock(&g_jobs_lock);

    job_t *j = NULL;
    for (int i = 0; i < MAX_JOBS; i++) {
        if (!g_jobs[i].used) { j = &g_jobs[i]; break; }
    }
    if (!j) {
        /* reuse the oldest finished slot */
        for (int i = 0; i < MAX_JOBS; i++) {
            if (g_jobs[i].state >= JSTATE_CANCELED &&
                (!j || g_jobs[i].t_done < j->t_done)) j = &g_jobs[i];
        }
    }
    if (!j) { pthread_mutex_unlock(&g_jobs_lock); return -1; }

    int32_t id;
    pthread_mutex_lock(&g_srv.job_id_lock);
    id = g_srv.next_job_id++;
    pthread_mutex_unlock(&g_srv.job_id_lock);

    memset(j, 0, sizeof(*j));
    j->used     = true;
    j->id       = id;
    j->state    = JSTATE_PENDING;
    j->t_create = time(NULL);
    snprintf(j->file, sizeof(j->file), "%s/job_%06d.pdf", jobs_dir, (int)id);
    strncpy(j->name, req->job_name, sizeof(j->name) - 1);
    strncpy(j->user, req->user_name, sizeof(j->user) - 1);
    strncpy(j->format, req->have_doc_format ? req->doc_format : "application/pdf",
            sizeof(j->format) - 1);
    job_apply_template(j, req);

    if (snapshot_out) *snapshot_out = *j;
    if (id_out) *id_out = id;
    pthread_mutex_unlock(&g_jobs_lock);
    return id;
}

static job_t *job_find_locked(int32_t id) {
    for (int i = 0; i < MAX_JOBS; i++)
        if (g_jobs[i].used && g_jobs[i].id == id) return &g_jobs[i];
    return NULL;
}

/** Auto-complete jobs that have been "processing" for too long
 *  (e.g. no foreground activity or the app was killed mid-print).
 *  Caller must hold g_jobs_lock. */
static void job_refresh_locked(job_t *j) {
    if (j->used && j->state == JSTATE_PROCESSING &&
        time(NULL) - j->t_processing > JOB_DONE_TIMEOUT) {
        j->state = JSTATE_COMPLETED;
        j->t_done = time(NULL);
        if (j->has_doc) { unlink(j->file); j->has_doc = false; }
    }
}

static const char *job_state_reason(int state) {
    switch (state) {
    case JSTATE_PROCESSING: return "job-printing";
    case JSTATE_COMPLETED:  return "job-completed-successfully";
    case JSTATE_CANCELED:   return "job-canceled-by-user";
    default:                return "none";
    }
}

static const char *job_state_message(int state) {
    switch (state) {
    case JSTATE_PROCESSING: return "Job printing.";
    case JSTATE_COMPLETED:  return "Job completed successfully.";
    case JSTATE_CANCELED:   return "Job canceled.";
    default:                return "Job pending.";
    }
}

/* ================================================================= */
/*  IPP response builders                                             */
/* ================================================================= */

/** Write IPP response header (version echoed from request + status + request-id). */
static void ipp_resp_begin(buf_t *b, const ipp_request_t *req, uint16_t status) {
    uint8_t hdr[8];
    uint8_t maj = req->ver_major, min = req->ver_minor;
    if (maj < 1 || maj > 2) { maj = 1; min = 1; }
    hdr[0] = maj; hdr[1] = min;
    hdr[2] = (uint8_t)(status >> 8); hdr[3] = (uint8_t)status;
    hdr[4] = (uint8_t)(req->request_id >> 24); hdr[5] = (uint8_t)(req->request_id >> 16);
    hdr[6] = (uint8_t)(req->request_id >> 8);  hdr[7] = (uint8_t)req->request_id;
    buf_write(b, hdr, 8);
}

static void put_op_group(buf_t *b, const char *msg) {
    buf_tag(b, IPP_TAG_OPERATION);
    buf_attr_str(b, IPP_VTAG_CHARSET,          "attributes-charset",          "utf-8");
    buf_attr_str(b, IPP_VTAG_NATURAL_LANGUAGE, "attributes-natural-language", "en-us");
    buf_attr_str(b, IPP_VTAG_TEXT_NOLANG,      "status-message",              msg);
}

static void put_job_attrs(buf_t *b, const job_t *j) {
    char uri[512];
    snprintf(uri, sizeof(uri), "ipp://localhost/ipp/print/job-%d", (int)j->id);

    buf_tag(b, IPP_TAG_JOB);
    buf_attr_i32(b, IPP_VTAG_INTEGER, "job-id", j->id);
    buf_attr_str(b, IPP_VTAG_URI,     "job-uri", uri);
    buf_attr_str(b, IPP_VTAG_NAME_NOLANG, "job-name", j->name[0] ? j->name : "Untitled");
    buf_attr_str(b, IPP_VTAG_NAME_NOLANG, "job-originating-user-name",
                 j->user[0] ? j->user : "guest");
    buf_attr_enum(b, "job-state", j->state);
    buf_attr_str(b, IPP_VTAG_TEXT_NOLANG, "job-state-message", job_state_message(j->state));
    buf_attr_kw(b,  "job-state-reasons", job_state_reason(j->state));
    buf_attr_i32(b, IPP_VTAG_INTEGER, "time-at-creation", (int32_t)j->t_create);
    if (j->t_processing)
        buf_attr_i32(b, IPP_VTAG_INTEGER, "time-at-processing", (int32_t)j->t_processing);
    if (j->t_done)
        buf_attr_i32(b, IPP_VTAG_INTEGER, "time-at-completed", (int32_t)j->t_done);
}

/** Minimal success response (operation group only). */
static void ipp_resp_ok(buf_t *b, const ipp_request_t *req) {
    ipp_resp_begin(b, req, IPP_STATUS_OK);
    put_op_group(b, "successful-ok");
    buf_tag(b, IPP_TAG_END);
}

/** Simple error response. */
static void ipp_resp_error(buf_t *b, const ipp_request_t *req,
                           uint16_t status, const char *msg) {
    ipp_resp_begin(b, req, status);
    put_op_group(b, msg);
    buf_tag(b, IPP_TAG_END);
}

/** Full printer attribute set (IPP Everywhere-style), shared by
 *  Get-Printer-Attributes and CUPS-Get-Printers / CUPS-Get-Default. */
static void put_printer_attrs(buf_t *b) {
    static const char *const charset_supported[] = { "us-ascii", "utf-8" };
    static const char *const versions[] = { "1.0", "1.1", "2.0" };
    static const int32_t ops_supported[] = {
        IPP_OP_PRINT_JOB, IPP_OP_VALIDATE_JOB, IPP_OP_CREATE_JOB,
        IPP_OP_SEND_DOCUMENT, IPP_OP_CANCEL_JOB, IPP_OP_GET_JOB_ATTRIBUTES,
        IPP_OP_GET_JOBS, IPP_OP_GET_PRINTER_ATTRIBUTES, IPP_OP_CLOSE_JOB
    };
    static const char *const formats[] = { "application/pdf" };
    static const char *const job_creation_attrs[] = {
        "copies", "finishings", "job-name", "media", "media-col",
        "orientation-requested", "print-color-mode", "print-quality",
        "printer-resolution", "sides"
    };
    static const int32_t orientations[] = { 3, 4, 5, 6 };
    static const int32_t qualities[] = { 3, 4, 5 };
    static const char *const color_modes[] = { "auto", "color", "monochrome" };
    static const char *const sides_list[] = {
        "one-sided", "two-sided-long-edge", "two-sided-short-edge"
    };
    static const char *const mdh[] = {
        "separate-documents-uncollated-copies", "separate-documents-collated-copies"
    };
    static const char *const media_col_supported[] = {
        "media-bottom-margin", "media-key", "media-left-margin",
        "media-right-margin", "media-size", "media-size-name",
        "media-source", "media-top-margin", "media-type"
    };

    /* count active jobs for printer-state / queued-job-count */
    int active = 0;
    pthread_mutex_lock(&g_jobs_lock);
    for (int i = 0; i < MAX_JOBS; i++) {
        job_refresh_locked(&g_jobs[i]);
        if (g_jobs[i].used && g_jobs[i].state < JSTATE_CANCELED) active++;
    }
    pthread_mutex_unlock(&g_jobs_lock);

    buf_tag(b, IPP_TAG_PRINTER);

    /* identification */
    buf_attr_str(b, IPP_VTAG_NAME_NOLANG, "printer-name", "TinyPrint");
    buf_attr_str(b, IPP_VTAG_TEXT_NOLANG, "printer-info", "Tiny Container Print");
    buf_attr_str(b, IPP_VTAG_TEXT_NOLANG, "printer-location", "");
    buf_attr_str(b, IPP_VTAG_TEXT_NOLANG, "printer-make-and-model", "Tiny IPP Bridge");
    buf_attr_str(b, IPP_VTAG_URI, "printer-uri-supported", "ipp://localhost/ipp/print");
    buf_attr_str(b, IPP_VTAG_URI, "printer-uuid",
                 "urn:uuid:00000000-0000-0000-0000-000000000001");
    buf_attr_bool(b, "printer-is-shared", false);

    /* state */
    buf_attr_enum(b, "printer-state", active > 0 ? 4 : 3);
    buf_attr_str(b, IPP_VTAG_TEXT_NOLANG, "printer-state-message", "");
    buf_attr_kw(b, "printer-state-reasons", "none");
    buf_attr_bool(b, "printer-is-accepting-jobs", true);
    buf_attr_i32(b, IPP_VTAG_INTEGER, "printer-type", PRINTER_TYPE);
    buf_attr_i32(b, IPP_VTAG_INTEGER, "printer-up-time", (int32_t)time(NULL));
    buf_attr_i32(b, IPP_VTAG_INTEGER, "queued-job-count", active);

    /* languages & versions */
    buf_attr_str(b, IPP_VTAG_CHARSET, "charset-configured", "utf-8");
    buf_attr_strs(b, IPP_VTAG_CHARSET, "charset-supported", charset_supported, 2);
    buf_attr_str(b, IPP_VTAG_NATURAL_LANGUAGE, "natural-language-configured", "en-us");
    buf_attr_str(b, IPP_VTAG_NATURAL_LANGUAGE, "generated-natural-language-supported", "en-us");
    buf_attr_strs(b, IPP_VTAG_KEYWORD, "ipp-versions-supported", versions, 3);
    buf_attr_kw(b, "ipp-features-supported", "ipp-everywhere");

    /* operations & security */
    buf_attr_ints(b, IPP_VTAG_ENUM, "operations-supported",
                  ops_supported, (int)(sizeof(ops_supported)/sizeof(ops_supported[0])));
    buf_attr_kw(b, "uri-authentication-supported", "none");
    buf_attr_kw(b, "uri-security-supported", "none");

    /* documents */
    buf_attr_str(b, IPP_VTAG_MIMETYPE, "document-format-default", "application/pdf");
    buf_attr_strs(b, IPP_VTAG_MIMETYPE, "document-format-supported", formats, 1);
    buf_attr_kw(b, "compression-supported", "none");
    buf_attr_bool(b, "color-supported", true);
    buf_attr_bool(b, "multiple-document-jobs-supported", true);
    buf_attr_strs(b, IPP_VTAG_KEYWORD, "multiple-document-handling-supported", mdh, 2);
    buf_attr_i32(b, IPP_VTAG_INTEGER, "multiple-operation-time-out", 300);
    buf_attr_kw(b, "pdl-override-supported", "not-attempted");

    /* job creation */
    buf_attr_strs(b, IPP_VTAG_KEYWORD, "job-creation-attributes-supported",
                  job_creation_attrs, (int)(sizeof(job_creation_attrs)/sizeof(job_creation_attrs[0])));
    buf_attr_bool(b, "job-ids-supported", true);
    buf_attr_i32(b, IPP_VTAG_INTEGER, "copies-default", 1);
    buf_attr_range(b, "copies-supported", 1, 99);
    buf_attr_enum(b, "finishings-default", 3);
    buf_attr_enum(b, "finishings-supported", 3);
    buf_attr_bool(b, "page-ranges-supported", false);

    /* media (paper) */
    buf_attr_kw(b, "media-default", media_table[0].name);
    buf_attr_kw(b, "media-ready", media_table[0].name);
    {
        const char *names[MEDIA_COUNT];
        for (int i = 0; i < MEDIA_COUNT; i++) names[i] = media_table[i].name;
        buf_attr_strs(b, IPP_VTAG_KEYWORD, "media-supported", names, MEDIA_COUNT);
    }
    buf_attr_i32(b, IPP_VTAG_INTEGER, "media-bottom-margin-supported", 635);
    buf_attr_i32(b, IPP_VTAG_INTEGER, "media-left-margin-supported", 635);
    buf_attr_i32(b, IPP_VTAG_INTEGER, "media-right-margin-supported", 635);
    buf_attr_i32(b, IPP_VTAG_INTEGER, "media-top-margin-supported", 635);
    buf_attr_kw(b, "media-source-supported", "auto");
    {
        static const char *const types[] = { "auto", "stationery" };
        buf_attr_strs(b, IPP_VTAG_KEYWORD, "media-type-supported", types, 2);
    }

    /* media-size-supported: collection per size */
    for (int i = 0; i < MEDIA_COUNT; i++) {
        col_start(b, i == 0 ? "media-size-supported" : NULL);
        col_member_int(b, "x-dimension", media_table[i].width);
        col_member_int(b, "y-dimension", media_table[i].height);
        col_end(b);
    }

    /* media-col-database: full media-col collection per size */
    for (int i = 0; i < MEDIA_COUNT; i++)
        put_media_col(b, i == 0 ? "media-col-database" : NULL, &media_table[i]);

    /* media-col-default / media-col-ready */
    put_media_col(b, "media-col-default", &media_table[0]);
    put_media_col(b, "media-col-ready", &media_table[0]);

    buf_attr_strs(b, IPP_VTAG_KEYWORD, "media-col-supported",
                  media_col_supported, (int)(sizeof(media_col_supported)/sizeof(media_col_supported[0])));

    /* rendering */
    buf_attr_enum(b, "orientation-requested-default", 3);
    buf_attr_ints(b, IPP_VTAG_ENUM, "orientation-requested-supported", orientations, 4);
    buf_attr_kw(b, "print-color-mode-default", "auto");
    buf_attr_strs(b, IPP_VTAG_KEYWORD, "print-color-mode-supported", color_modes, 3);
    buf_attr_enum(b, "print-quality-default", 4);
    buf_attr_ints(b, IPP_VTAG_ENUM, "print-quality-supported", qualities, 3);
    buf_attr_resolution(b, "printer-resolution-default", 600, 600);
    buf_attr_resolution(b, "printer-resolution-supported", 600, 600);
    buf_attr_kw(b, "sides-default", "one-sided");
    buf_attr_strs(b, IPP_VTAG_KEYWORD, "sides-supported", sides_list, 3);
}

/** Get-Printer-Attributes / CUPS-Get-* response. */
static void ipp_resp_printer_attrs(buf_t *b, const ipp_request_t *req) {
    ipp_resp_begin(b, req, IPP_STATUS_OK);
    put_op_group(b, "successful-ok");
    put_printer_attrs(b);
    buf_tag(b, IPP_TAG_END);
}

/* ================================================================= */
/*  PPD generator (for CUPS-Get-PPD)                                  */
/*  Legacy clients (Chrome PPD backend, Qt/OnlyOffice, GTK) fetch a   */
/*  PPD and parse it with ppdOpenFile() to obtain capabilities.       */
/* ================================================================= */

static void ppd_puts(buf_t *b, const char *s) {
    buf_write(b, s, strlen(s));
    buf_write(b, "\n", 1);
}

static void ppd_fmt(buf_t *b, const char *fmt, ...) {
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n > 0) buf_write(b, tmp, (size_t)n < sizeof(tmp) ? (size_t)n : sizeof(tmp) - 1);
    buf_write(b, "\n", 1);
}

static void build_ppd(buf_t *b) {
    ppd_puts(b, "*PPD-Adobe: \"4.3\"");
    ppd_puts(b, "*% Tiny Container driverless printer PPD");
    ppd_puts(b, "*FormatVersion: \"4.3\"");
    ppd_puts(b, "*FileVersion: \"1.0\"");
    ppd_puts(b, "*LanguageVersion: English");
    ppd_puts(b, "*LanguageEncoding: ISOLatin1");
    ppd_puts(b, "*PCFileName: \"TINY.PPD\"");
    ppd_puts(b, "*Manufacturer: \"Tiny\"");
    ppd_puts(b, "*Product: \"(Tiny IPP Bridge)\"");
    ppd_puts(b, "*ModelName: \"Tiny IPP Bridge\"");
    ppd_puts(b, "*NickName: \"Tiny IPP Bridge, driverless\"");
    ppd_puts(b, "*ShortNickName: \"TinyPrint\"");
    ppd_puts(b, "*PSVersion: \"(3010.000) 0\"");
    ppd_puts(b, "*LanguageLevel: \"3\"");
    ppd_puts(b, "*ColorDevice: True");
    ppd_puts(b, "*DefaultColorSpace: RGB");
    ppd_puts(b, "*FileSystem: False");
    ppd_puts(b, "*Throughput: \"1\"");
    ppd_puts(b, "*LandscapeOrientation: Plus90");
    ppd_puts(b, "*TTRasterizer: Type42");
    ppd_puts(b, "*cupsVersion: 2.4");
    ppd_puts(b, "*cupsSNMPSupplies: False");
    ppd_puts(b, "*cupsLanguages: \"en\"");
    ppd_puts(b, "*cupsManualCopies: True");
    ppd_puts(b, "*cupsFilter2: \"application/vnd.cups-pdf application/pdf 0 -\"");
    ppd_fmt(b, "*HWMargins: %d %d %d %d",
            PPD_MARGIN_PT, PPD_MARGIN_PT, PPD_MARGIN_PT, PPD_MARGIN_PT);

    /* ---- PageSize ---- */
    ppd_puts(b, "*OpenUI *PageSize/Page Size: PickOne");
    ppd_puts(b, "*OrderDependency: 10 AnySetup *PageSize");
    ppd_puts(b, "*DefaultPageSize: A4");
    for (int i = 0; i < PPD_MEDIA_COUNT; i++)
        ppd_fmt(b, "*PageSize %s/%s: \"<</PageSize[%d %d]>>setpagedevice\"",
                ppd_media_table[i].ppd_name, ppd_media_table[i].label,
                (int)ppd_media_table[i].width_pt, (int)ppd_media_table[i].height_pt);
    ppd_puts(b, "*CloseUI: *PageSize");

    /* ---- PageRegion (required by ppdOpenFile consumers) ---- */
    ppd_puts(b, "*OpenUI *PageRegion: PickOne");
    ppd_puts(b, "*OrderDependency: 20 AnySetup *PageRegion");
    ppd_puts(b, "*DefaultPageRegion: A4");
    for (int i = 0; i < PPD_MEDIA_COUNT; i++)
        ppd_fmt(b, "*PageRegion %s/%s: \"<</PageSize[%d %d]>>setpagedevice\"",
                ppd_media_table[i].ppd_name, ppd_media_table[i].label,
                (int)ppd_media_table[i].width_pt, (int)ppd_media_table[i].height_pt);
    ppd_puts(b, "*CloseUI: *PageRegion");

    /* ---- ImageableArea / PaperDimension ---- */
    ppd_puts(b, "*DefaultImageableArea: A4");
    for (int i = 0; i < PPD_MEDIA_COUNT; i++)
        ppd_fmt(b, "*ImageableArea %s/%s: \"%d %d %d %d\"",
                ppd_media_table[i].ppd_name, ppd_media_table[i].label,
                PPD_MARGIN_PT, PPD_MARGIN_PT,
                (int)ppd_media_table[i].width_pt - PPD_MARGIN_PT,
                (int)ppd_media_table[i].height_pt - PPD_MARGIN_PT);

    ppd_puts(b, "*DefaultPaperDimension: A4");
    for (int i = 0; i < PPD_MEDIA_COUNT; i++)
        ppd_fmt(b, "*PaperDimension %s/%s: \"%d %d\"",
                ppd_media_table[i].ppd_name, ppd_media_table[i].label,
                (int)ppd_media_table[i].width_pt, (int)ppd_media_table[i].height_pt);

    /* ---- Duplex ---- */
    ppd_puts(b, "*OpenUI *Duplex/2-Sided Printing: PickOne");
    ppd_puts(b, "*OrderDependency: 30 AnySetup *Duplex");
    ppd_puts(b, "*DefaultDuplex: None");
    ppd_puts(b, "*Duplex None/Off: \"\"");
    ppd_puts(b, "*Duplex DuplexNoTumble/Long-Edge (Portrait): \"<</Duplex true /Tumble false>>setpagedevice\"");
    ppd_puts(b, "*Duplex DuplexTumble/Short-Edge (Landscape): \"<</Duplex true /Tumble true>>setpagedevice\"");
    ppd_puts(b, "*CloseUI: *Duplex");

    /* ---- ColorModel ---- */
    ppd_puts(b, "*OpenUI *ColorModel/Print Color Mode: PickOne");
    ppd_puts(b, "*OrderDependency: 40 AnySetup *ColorModel");
    ppd_puts(b, "*DefaultColorModel: RGB");
    ppd_puts(b, "*ColorModel Gray/Monochrome: \"<</cupsColorSpace 0 /cupsColorOrder 0 /cupsCompression 0>>setpagedevice\"");
    ppd_puts(b, "*ColorModel RGB/Color: \"<</cupsColorSpace 1 /cupsColorOrder 0 /cupsCompression 0>>setpagedevice\"");
    ppd_puts(b, "*CloseUI: *ColorModel");

    /* ---- Print quality ---- */
    ppd_puts(b, "*OpenUI *cupsPrintQuality/Print Quality: PickOne");
    ppd_puts(b, "*OrderDependency: 50 AnySetup *cupsPrintQuality");
    ppd_puts(b, "*DefaultcupsPrintQuality: Normal");
    ppd_puts(b, "*cupsPrintQuality Draft/Draft: \"<</HWResolution[300 300]>>setpagedevice\"");
    ppd_puts(b, "*cupsPrintQuality Normal/Normal: \"<</HWResolution[600 600]>>setpagedevice\"");
    ppd_puts(b, "*cupsPrintQuality High/High: \"<</HWResolution[1200 1200]>>setpagedevice\"");
    ppd_puts(b, "*CloseUI: *cupsPrintQuality");
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

static int http_write_response(int fd, uint16_t status_code,
                               const uint8_t *body, size_t body_len) {
    char hdr[256];
    const char *status_text = (status_code == 200) ? "OK" : "Internal Server Error";
    int n = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: application/ipp\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n",
        (int)status_code, status_text, body_len);

    if (write_all(fd, hdr, (size_t)n) < 0) return -1;
    if (body_len && write_all(fd, body, body_len) < 0) return -1;
    return 0;
}

/** Send buffer built by one of the ipp_resp_* helpers as a 200 response. */
static void send_ipp(int fd, buf_t *resp) {
    http_write_response(fd, 200, resp->data, resp->len);
    buf_free(resp);
}

/* ================================================================= */
/*  JNI upcall                                                        */
/* ================================================================= */

static void upcall_on_print_job(JNIEnv *env, const job_t *j)
{
    jstring j_name   = (*env)->NewStringUTF(env, j->name[0] ? j->name : "Untitled");
    jstring j_path   = (*env)->NewStringUTF(env, j->file);
    jstring j_fmt    = (*env)->NewStringUTF(env, j->format[0] ? j->format : "application/pdf");
    jstring j_media  = (*env)->NewStringUTF(env, j->media);
    jstring j_sides  = (*env)->NewStringUTF(env, j->sides);
    jstring j_color  = (*env)->NewStringUTF(env, j->color_mode);

    (*env)->CallStaticVoidMethod(env, g_srv.cls_tiny_ipp, g_srv.mid_on_print_job,
                                 (jint)j->id, j_name, j_path, j_fmt,
                                 (jint)j->copies, j_media,
                                 (jint)j->media_w, (jint)j->media_h,
                                 (jint)j->orientation, j_sides, j_color,
                                 (jint)j->res_x, (jint)j->res_y);

    (*env)->DeleteLocalRef(env, j_name);
    (*env)->DeleteLocalRef(env, j_path);
    (*env)->DeleteLocalRef(env, j_fmt);
    (*env)->DeleteLocalRef(env, j_media);
    (*env)->DeleteLocalRef(env, j_sides);
    (*env)->DeleteLocalRef(env, j_color);
}

static void upcall_print_job(const job_t *snapshot) {
    JNIEnv *env = NULL;
    jboolean need_detach = JNI_FALSE;
    jint ret = (*g_srv.jvm)->GetEnv(g_srv.jvm, (void **)&env, JNI_VERSION_1_6);
    if (ret == JNI_EDETACHED) {
        if ((*g_srv.jvm)->AttachCurrentThread(g_srv.jvm, &env, NULL) == JNI_OK)
            need_detach = JNI_TRUE;
    }
    if (env) {
        upcall_on_print_job(env, snapshot);
        if (need_detach) (*g_srv.jvm)->DetachCurrentThread(g_srv.jvm);
    } else {
        LOGE("JNI: cannot obtain env for upcall");
    }
}

/**
 * Mark a job ready for printing and dispatch the Android upcall.
 * Takes a snapshot so the upcall happens outside the lock.
 * Returns true (and fills snapshot) when an upcall must be made.
 * Caller must hold g_jobs_lock.
 */
static bool job_finalize_locked(job_t *j, job_t *snapshot) {
    if (j->notified || !j->has_doc || j->state >= JSTATE_CANCELED) return false;
    j->notified     = true;
    j->state        = JSTATE_PROCESSING;
    j->t_processing = time(NULL);
    *snapshot = *j;
    return true;
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
    bool expect_100;
} http_request_t;

/** Case-insensitive substring search. */
static bool contains_ci(const char *hay, const char *needle) {
    size_t nl = strlen(needle);
    for (; *hay; hay++)
        if (!strncasecmp(hay, needle, nl)) return true;
    return false;
}

/**
 * Read HTTP request line + headers from the socket.
 * Returns 0 on success, -1 on error / non-IPP / GET.
 */
static int http_parse_request(int fd, http_request_t *hreq) {
    char line[HDR_LINE_MAX];

    /* request line */
    if (sock_read_line(fd, line, sizeof(line)) <= 0) { LOGE("http: request line eof"); return -1; }

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
    hreq->is_ipp     = false;
    hreq->is_get     = false;
    hreq->is_chunked = false;
    hreq->expect_100 = false;

    /* Allow GET (health-check), but only POST is IPP */
    if (strcmp(hreq->method, "POST")) {
        if (!strcmp(hreq->method, "GET")) { hreq->is_get = true; return 0; }
        LOGE("http: non-POST method: %s", hreq->method); return -1;
    }

    /* headers */
    for (;;) {
        if (sock_read_line(fd, line, sizeof(line)) <= 0) { LOGE("http: headers eof"); return -1; }
        if (!line[0]) break; /* empty line = end of headers */

        if (strncasecmp(line, "content-length:", 15) == 0) {
            const char *v = line + 15;
            while (*v == ' ' || *v == '\t') v++;
            hreq->content_length = atol(v);
        }
        else if (strncasecmp(line, "content-type:", 13) == 0) {
            if (contains_ci(line + 13, "application/ipp")) hreq->is_ipp = true;
        }
        else if (strncasecmp(line, "transfer-encoding:", 18) == 0) {
            if (contains_ci(line + 18, "chunked")) hreq->is_chunked = true;
        }
        else if (strncasecmp(line, "expect:", 7) == 0) {
            if (contains_ci(line + 7, "100-continue")) hreq->expect_100 = true;
        }
    }

    LOGI("http: %s %s CL=%ld ipp=%d chunked=%d expect100=%d",
         hreq->method, hreq->uri, hreq->content_length,
         (int)hreq->is_ipp, (int)hreq->is_chunked, (int)hreq->expect_100);
    return 0;
}

/* ================================================================= */
/*  Document spooling                                                 */
/* ================================================================= */

/**
 * Spool document data to `path`.  When `mem` is non-NULL the data is
 * already in memory (chunked path); otherwise `len` bytes are streamed
 * from socket `fd`.
 */
static int spool_document(int fd, const uint8_t *mem, long len,
                          const char *path, bool append) {
    int out_fd = open(path, O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC), 0600);
    if (out_fd < 0) { LOGE("open(%s): %s", path, strerror(errno)); return -1; }

    int rc = 0;
    if (mem) {
        if (write_all(out_fd, mem, (size_t)len) < 0) rc = -1;
    } else {
        char buf[65536];
        long rem = len;
        while (rem > 0) {
            size_t chunk = rem < (long)sizeof(buf) ? (size_t)rem : sizeof(buf);
            if (sock_read_full(fd, buf, chunk) < 0) { rc = -1; break; }
            if (write_all(out_fd, buf, chunk) < 0) { rc = -1; break; }
            rem -= (long)chunk;
        }
    }
    close(out_fd);
    if (rc < 0) { unlink(path); return -1; }
    chmod(path, 0644);
    return 0;
}

/** Drain document bytes that we are not going to consume (error paths). */
static void drain_document(int fd, const uint8_t *mem, long len) {
    if (!mem && len > 0) skip_socket_bytes(fd, (size_t)len);
}

/* ================================================================= */
/*  Operation dispatch                                                */
/*  doc: mem_doc != NULL → in-memory document (chunked path);        */
/*       otherwise doc_len bytes remain to be streamed from fd.      */
/* ================================================================= */

static void dispatch_request(int fd, ipp_request_t *req,
                             const uint8_t *mem_doc, long doc_len)
{
    LOGI("IPP op=0x%04x req=%u job=%s fmt=%s doc=%ld media=%s %dx%d orient=%d",
         req->operation_id, req->request_id, req->job_name, req->doc_format,
         doc_len, req->media, (int)req->media_w, (int)req->media_h, (int)req->orientation);

    switch (req->operation_id) {

    case IPP_OP_PRINT_JOB:
    {
        if (doc_len <= 0) {
            buf_t resp; buf_init(&resp);
            ipp_resp_error(&resp, req, IPP_STATUS_ERROR_BAD_REQUEST, "No document data");
            send_ipp(fd, &resp);
            return;
        }

        job_t snapshot;
        int32_t jid = job_create(req, g_srv.jobs_dir, NULL, &snapshot);
        if (jid < 0) {
            drain_document(fd, mem_doc, doc_len);
            buf_t resp; buf_init(&resp);
            ipp_resp_error(&resp, req, IPP_STATUS_ERROR_INTERNAL, "Too many jobs");
            send_ipp(fd, &resp);
            return;
        }

        /* spool without holding the jobs lock (large transfers) */
        if (spool_document(fd, mem_doc, doc_len, snapshot.file, false) < 0) {
            pthread_mutex_lock(&g_jobs_lock);
            job_t *j = job_find_locked(jid);
            if (j) { j->state = JSTATE_CANCELED; j->t_done = time(NULL); }
            pthread_mutex_unlock(&g_jobs_lock);
            buf_t resp; buf_init(&resp);
            ipp_resp_error(&resp, req, IPP_STATUS_ERROR_INTERNAL, "Failed to spool");
            send_ipp(fd, &resp);
            return;
        }

        bool do_upcall = false;
        pthread_mutex_lock(&g_jobs_lock);
        job_t *j = job_find_locked(jid);
        if (j && j->state < JSTATE_CANCELED) {
            j->has_doc = true;
            do_upcall = job_finalize_locked(j, &snapshot);
        }
        pthread_mutex_unlock(&g_jobs_lock);

        if (do_upcall) upcall_print_job(&snapshot);

        buf_t resp; buf_init(&resp);
        ipp_resp_begin(&resp, req, IPP_STATUS_OK);
        put_op_group(&resp, "successful-ok");
        put_job_attrs(&resp, &snapshot);
        buf_tag(&resp, IPP_TAG_END);
        send_ipp(fd, &resp);
        return;
    }

    case IPP_OP_CREATE_JOB:
    {
        job_t snapshot;
        int32_t jid = job_create(req, g_srv.jobs_dir, NULL, &snapshot);
        if (jid < 0) {
            buf_t resp; buf_init(&resp);
            ipp_resp_error(&resp, req, IPP_STATUS_ERROR_INTERNAL, "Too many jobs");
            send_ipp(fd, &resp);
            return;
        }
        buf_t resp; buf_init(&resp);
        ipp_resp_begin(&resp, req, IPP_STATUS_OK);
        put_op_group(&resp, "successful-ok");
        put_job_attrs(&resp, &snapshot);
        buf_tag(&resp, IPP_TAG_END);
        send_ipp(fd, &resp);
        return;
    }

    case IPP_OP_SEND_DOCUMENT:
    {
        /* validate job and grab spool path without holding the lock during I/O */
        char spool_path[512];
        bool append = false;
        bool valid  = false;

        pthread_mutex_lock(&g_jobs_lock);
        job_t *j = req->target_job_id > 0 ? job_find_locked(req->target_job_id) : NULL;
        if (j) {
            job_refresh_locked(j);
            if (j->state < JSTATE_CANCELED) {
                valid  = true;
                append = j->has_doc;
                strncpy(spool_path, j->file, sizeof(spool_path) - 1);
                spool_path[sizeof(spool_path) - 1] = '\0';
                if (req->have_doc_format)
                    strncpy(j->format, req->doc_format, sizeof(j->format) - 1);
            }
        }
        pthread_mutex_unlock(&g_jobs_lock);

        if (!valid) {
            drain_document(fd, mem_doc, doc_len);
            buf_t resp; buf_init(&resp);
            ipp_resp_error(&resp, req, IPP_STATUS_ERROR_NOT_FOUND, "Job not found");
            send_ipp(fd, &resp);
            return;
        }

        if (doc_len > 0 &&
            spool_document(fd, mem_doc, doc_len, spool_path, append) < 0) {
            pthread_mutex_lock(&g_jobs_lock);
            j = job_find_locked(req->target_job_id);
            if (j) { j->state = JSTATE_CANCELED; j->t_done = time(NULL); j->has_doc = false; }
            pthread_mutex_unlock(&g_jobs_lock);
            buf_t resp; buf_init(&resp);
            ipp_resp_error(&resp, req, IPP_STATUS_ERROR_INTERNAL, "Failed to spool");
            send_ipp(fd, &resp);
            return;
        }

        job_t snapshot; bool do_upcall = false;
        memset(&snapshot, 0, sizeof(snapshot));

        pthread_mutex_lock(&g_jobs_lock);
        j = job_find_locked(req->target_job_id);
        if (j && j->state < JSTATE_CANCELED) {
            if (doc_len > 0) j->has_doc = true;
            if (req->have_last_document && req->last_document)
                do_upcall = job_finalize_locked(j, &snapshot);
            if (!do_upcall) snapshot = *j;
        } else if (doc_len > 0) {
            unlink(spool_path);     /* job was canceled mid-transfer */
        }
        pthread_mutex_unlock(&g_jobs_lock);

        if (do_upcall) upcall_print_job(&snapshot);

        buf_t resp; buf_init(&resp);
        ipp_resp_begin(&resp, req, IPP_STATUS_OK);
        put_op_group(&resp, "successful-ok");
        put_job_attrs(&resp, &snapshot);
        buf_tag(&resp, IPP_TAG_END);
        send_ipp(fd, &resp);
        return;
    }

    case IPP_OP_CLOSE_JOB:
    {
        pthread_mutex_lock(&g_jobs_lock);
        job_t *j = req->target_job_id > 0 ? job_find_locked(req->target_job_id) : NULL;
        job_t snapshot; bool do_upcall = false;
        memset(&snapshot, 0, sizeof(snapshot));
        bool found = j != NULL;
        if (found) {
            job_refresh_locked(j);
            do_upcall = job_finalize_locked(j, &snapshot);
            if (!do_upcall) snapshot = *j;
        }
        pthread_mutex_unlock(&g_jobs_lock);

        if (!found) {
            buf_t resp; buf_init(&resp);
            ipp_resp_error(&resp, req, IPP_STATUS_ERROR_NOT_FOUND, "Job not found");
            send_ipp(fd, &resp);
            return;
        }
        if (do_upcall) upcall_print_job(&snapshot);

        buf_t resp; buf_init(&resp);
        ipp_resp_begin(&resp, req, IPP_STATUS_OK);
        put_op_group(&resp, "successful-ok");
        put_job_attrs(&resp, &snapshot);
        buf_tag(&resp, IPP_TAG_END);
        send_ipp(fd, &resp);
        return;
    }

    case IPP_OP_VALIDATE_JOB:
    {
        buf_t resp; buf_init(&resp);
        ipp_resp_ok(&resp, req);
        send_ipp(fd, &resp);
        return;
    }

    case IPP_OP_GET_PRINTER_ATTRIBUTES:
    case CUPS_OP_GET_PRINTERS:
    case CUPS_OP_GET_DEFAULT:
    case CUPS_OP_GET_CLASSES:
    {
        buf_t resp; buf_init(&resp);
        ipp_resp_printer_attrs(&resp, req);
        send_ipp(fd, &resp);
        return;
    }

    case CUPS_OP_GET_PPD:
    {
        /* Legacy clients (Chrome PPD backend, Qt, GTK) parse a PPD for
         * capabilities.  Respond with raw PPD text, not an IPP message. */
        buf_t ppd; buf_init(&ppd);
        build_ppd(&ppd);
        char hdr[128];
        int n = snprintf(hdr, sizeof(hdr),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/vnd.cups-ppd\r\n"
            "Content-Length: %zu\r\n"
            "Connection: close\r\n\r\n", ppd.len);
        write_all(fd, hdr, (size_t)n);
        write_all(fd, ppd.data, ppd.len);
        buf_free(&ppd);
        return;
    }

    case IPP_OP_GET_JOB_ATTRIBUTES:
    {
        pthread_mutex_lock(&g_jobs_lock);
        job_t *j = req->target_job_id > 0 ? job_find_locked(req->target_job_id) : NULL;
        job_t snapshot;
        memset(&snapshot, 0, sizeof(snapshot));
        bool found = j != NULL;
        if (found) { job_refresh_locked(j); snapshot = *j; }
        pthread_mutex_unlock(&g_jobs_lock);

        buf_t resp; buf_init(&resp);
        if (!found) {
            ipp_resp_error(&resp, req, IPP_STATUS_ERROR_NOT_FOUND, "Job not found");
        } else {
            ipp_resp_begin(&resp, req, IPP_STATUS_OK);
            put_op_group(&resp, "successful-ok");
            put_job_attrs(&resp, &snapshot);
            buf_tag(&resp, IPP_TAG_END);
        }
        send_ipp(fd, &resp);
        return;
    }

    case IPP_OP_GET_JOBS:
    {
        bool want_completed = !strcmp(req->which_jobs, "completed");
        bool want_all       = !strcmp(req->which_jobs, "all");

        buf_t resp; buf_init(&resp);
        ipp_resp_begin(&resp, req, IPP_STATUS_OK);
        put_op_group(&resp, "successful-ok");

        pthread_mutex_lock(&g_jobs_lock);
        for (int i = 0; i < MAX_JOBS; i++) {
            job_t *j = &g_jobs[i];
            if (!j->used) continue;
            job_refresh_locked(j);
            bool completed = j->state >= JSTATE_CANCELED;
            if (!want_all && completed != want_completed) continue;
            put_job_attrs(&resp, j);
        }
        pthread_mutex_unlock(&g_jobs_lock);

        buf_tag(&resp, IPP_TAG_END);
        send_ipp(fd, &resp);
        return;
    }

    case IPP_OP_CANCEL_JOB:
    {
        pthread_mutex_lock(&g_jobs_lock);
        job_t *j = req->target_job_id > 0 ? job_find_locked(req->target_job_id) : NULL;
        bool found = j != NULL;
        if (found && j->state < JSTATE_CANCELED) {
            j->state = JSTATE_CANCELED;
            j->t_done = time(NULL);
            if (j->has_doc) { unlink(j->file); j->has_doc = false; }
        }
        pthread_mutex_unlock(&g_jobs_lock);

        buf_t resp; buf_init(&resp);
        if (!found)
            ipp_resp_error(&resp, req, IPP_STATUS_ERROR_NOT_FOUND, "Job not found");
        else
            ipp_resp_ok(&resp, req);
        send_ipp(fd, &resp);
        return;
    }

    default:
    {
        drain_document(fd, mem_doc, doc_len);
        buf_t resp; buf_init(&resp);
        ipp_resp_error(&resp, req, IPP_STATUS_ERROR_BAD_REQUEST, "Operation not supported");
        send_ipp(fd, &resp);
        return;
    }
    }
}

/* ================================================================= */
/*  Chunked body reader                                               */
/* ================================================================= */

/** Read all chunks of a chunked Transfer-Encoding body.
 *  Returns malloc'd buffer (caller frees), sets *out_len. NULL on error. */
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
                if (!trail[0]) break;
            }
            *out_len = len;
            return buf;
        }

        while (len + (size_t)chunk_size > cap) {
            size_t newcap = cap * 2;
            uint8_t *newbuf = realloc(buf, newcap);
            if (!newbuf) { LOGE("chunked: realloc OOM"); free(buf); return NULL; }
            buf = newbuf; cap = newcap;
        }

        if (sock_read_full(fd, buf + len, (size_t)chunk_size) < 0) {
            LOGE("chunked: short read for chunk data (%ld bytes)", chunk_size);
            free(buf); return NULL;
        }
        len += (size_t)chunk_size;

        /* read trailing CRLF */
        char crlf[2];
        if (sock_read_full(fd, crlf, 2) < 0) {
            LOGE("chunked: missing CRLF after chunk");
            free(buf); return NULL;
        }
    }
}

/* ================================================================= */
/*  Worker thread                                                     */
/* ================================================================= */

static void *worker_func(void *arg) {
    int fd = (int)(intptr_t)arg;

    struct timeval tv = { .tv_sec = 60, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    http_request_t hreq;
    if (http_parse_request(fd, &hreq) != 0) { close(fd); goto unregister; }

    if (hreq.is_get) {
        LOGI("http: GET %s", hreq.uri);

        /* cupsGetPPD3() fetches the PPD via HTTP GET on
         * "<printer-resource>.ppd" (e.g. /ipp/print.ppd).  Legacy clients
         * (Chrome PPD backend, Qt, GTK) need this for capabilities. */
        size_t uri_len = strlen(hreq.uri);
        if (uri_len >= 4 && !strcmp(hreq.uri + uri_len - 4, ".ppd")) {
            buf_t ppd; buf_init(&ppd);
            build_ppd(&ppd);

            char date[64];
            time_t now = time(NULL);
            struct tm tm_now;
            gmtime_r(&now, &tm_now);
            strftime(date, sizeof(date), "%a, %d %b %Y %H:%M:%S GMT", &tm_now);

            char hdr[512];
            int n = snprintf(hdr, sizeof(hdr),
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: application/vnd.cups-ppd\r\n"
                "Content-Length: %zu\r\n"
                "Date: %s\r\n"
                "Last-Modified: %s\r\n"
                "Connection: close\r\n\r\n", ppd.len, date, date);
            write_all(fd, hdr, (size_t)n);
            write_all(fd, ppd.data, ppd.len);
            LOGI("served PPD (%zu bytes) for GET %s", ppd.len, hreq.uri);
            buf_free(&ppd);
        } else {
            /* CUPS sends GET as health-check between IPP operations. */
            const char *ok = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n"
                             "Connection: close\r\n\r\n{}";
            write(fd, ok, strlen(ok));
        }
        close(fd);
        goto unregister;
    }

    /* Tell the client to go ahead with the body. */
    if (hreq.expect_100) {
        const char *cont = "HTTP/1.1 100 Continue\r\n\r\n";
        if (write_all(fd, cont, strlen(cont)) < 0) { close(fd); goto unregister; }
    }

    if (hreq.is_chunked) {
        size_t body_len = 0;
        uint8_t *body = read_chunked_body(fd, &body_len);
        if (!body || body_len < 8) {
            free(body);
            ipp_request_t req; memset(&req, 0, sizeof(req));
            req.ver_major = 1; req.ver_minor = 1;
            buf_t resp; buf_init(&resp);
            ipp_resp_error(&resp, &req, IPP_STATUS_ERROR_BAD_REQUEST,
                           body ? "Body too short" : "Failed to read chunked body");
            send_ipp(fd, &resp);
            close(fd);
            goto unregister;
        }

        ipp_request_t req;
        req.ver_major   = body[0];
        req.ver_minor   = body[1];
        req.operation_id = rd16be(body + 2);
        req.request_id   = rd32be(body + 4);

        /* save header fields, then parse attrs (parser memsets the struct) */
        uint8_t vmaj = req.ver_major, vmin = req.ver_minor;
        uint16_t op  = req.operation_id;
        uint32_t rid = req.request_id;

        size_t doc_ofs = 0;
        if (ipp_parse_attrs(body + 8, body_len - 8, &req, &doc_ofs) < 0) {
            free(body);
            memset(&req, 0, sizeof(req));
            req.ver_major = vmaj; req.ver_minor = vmin; req.request_id = rid;
            buf_t resp; buf_init(&resp);
            ipp_resp_error(&resp, &req, IPP_STATUS_ERROR_BAD_REQUEST, "Bad IPP attributes");
            send_ipp(fd, &resp);
            close(fd);
            goto unregister;
        }
        req.ver_major = vmaj; req.ver_minor = vmin;
        req.operation_id = op; req.request_id = rid;

        long doc_len = (long)(body_len - 8 - doc_ofs);
        dispatch_request(fd, &req, body + 8 + doc_ofs, doc_len);
        free(body);
        close(fd);
        goto unregister;
    }

    if (hreq.content_length < 8) {
        ipp_request_t req; memset(&req, 0, sizeof(req));
        req.ver_major = 1; req.ver_minor = 1;
        buf_t resp; buf_init(&resp);
        ipp_resp_error(&resp, &req, IPP_STATUS_ERROR_BAD_REQUEST, "Body too short");
        send_ipp(fd, &resp);
        close(fd);
        goto unregister;
    }

    /* Read IPP header (8 bytes) to determine operation */
    uint8_t hdr[8];
    if (sock_read_full(fd, hdr, 8) < 0) { LOGE("short read for IPP header"); close(fd); goto unregister; }

    /* Read the attribute section (bounded), then stream the document if any. */
    buf_t attrs;
    if (read_attr_section(fd, &attrs) < 0) {
        LOGE("failed to read IPP attributes");
        close(fd);
        goto unregister;
    }

    ipp_request_t req;
    size_t doc_ofs = 0;
    if (ipp_parse_attrs(attrs.data, attrs.len, &req, &doc_ofs) < 0) {
        buf_free(&attrs);
        memset(&req, 0, sizeof(req));
        req.ver_major = hdr[0]; req.ver_minor = hdr[1]; req.request_id = rd32be(hdr + 4);
        buf_t resp; buf_init(&resp);
        ipp_resp_error(&resp, &req, IPP_STATUS_ERROR_BAD_REQUEST, "Bad IPP attributes");
        send_ipp(fd, &resp);
        close(fd);
        goto unregister;
    }
    req.ver_major    = hdr[0];
    req.ver_minor    = hdr[1];
    req.operation_id = rd16be(hdr + 2);
    req.request_id   = rd32be(hdr + 4);
    buf_free(&attrs);

    long doc_len = hreq.content_length - 8 - (long)doc_ofs;
    dispatch_request(fd, &req, NULL, doc_len);
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

        /* cap workers */
        pthread_mutex_lock(&g_srv.workers_lock);
        if (g_srv.worker_count >= MAX_WORKERS) {
            pthread_mutex_unlock(&g_srv.workers_lock);
            const char *msg = "HTTP/1.1 503 Service Unavailable\r\n"
                              "Content-Length: 0\r\nConnection: close\r\n\r\n";
            write(client_fd, msg, strlen(msg));
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
    pthread_mutex_init(&g_jobs_lock, NULL);
    memset(g_jobs, 0, sizeof(g_jobs));
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
        "onPrintJob",
        "(ILjava/lang/String;Ljava/lang/String;Ljava/lang/String;"
        "ILjava/lang/String;IIILjava/lang/String;Ljava/lang/String;II)V");
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

    /* cancel & clean up remaining jobs */
    pthread_mutex_lock(&g_jobs_lock);
    for (int i = 0; i < MAX_JOBS; i++) {
        if (g_jobs[i].used && g_jobs[i].has_doc) unlink(g_jobs[i].file);
        g_jobs[i].used = false;
        g_jobs[i].has_doc = false;
    }
    pthread_mutex_unlock(&g_jobs_lock);

    /* release JNI refs */
    if (g_srv.cls_tiny_ipp) {
        (*env)->DeleteGlobalRef(env, g_srv.cls_tiny_ipp);
        g_srv.cls_tiny_ipp = NULL;
    }

    pthread_mutex_destroy(&g_srv.workers_lock);
    pthread_mutex_destroy(&g_srv.job_id_lock);
    pthread_mutex_destroy(&g_jobs_lock);

    LOGI("nativeStop complete");
}

/** Called from Kotlin when the Android print interaction for a job has
 *  finished (printed, canceled or failed) — mark the job completed and
 *  remove the spool file. */
JNIEXPORT void JNICALL
Java_com_fct_tc4_TinyIpp_nativeJobFinished(JNIEnv *env, jclass cls, jint jobId)
{
    (void)env; (void)cls;
    pthread_mutex_lock(&g_jobs_lock);
    job_t *j = job_find_locked((int32_t)jobId);
    if (j) {
        if (j->state < JSTATE_CANCELED) {
            j->state = JSTATE_COMPLETED;
            j->t_done = time(NULL);
        }
        if (j->has_doc) { unlink(j->file); j->has_doc = false; }
    }
    pthread_mutex_unlock(&g_jobs_lock);
    LOGI("job %d finished", (int)jobId);
}
