/*
 * tiny_storage_jni.c -- This file is part of tiny_container.
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
 * tiny_storage_jni.c – Client that sends external storage mount/umount
 * events to the proot-side socket server at $PROOT_TMP_DIR/.tiny.storage.
 *
 * Protocol: fixed-size binary struct (one message per write).
 *
 *   struct storage_msg {
 *       uint8_t action;               // 'A' = add, 'R' = remove
 *       char    path[SM_PATH_MAX];    // host device path (e.g. /mnt/media_rw/...)
 *       char    name[SM_NAME_MAX];    // mount-name under /mnt/ (e.g. "KINGSTON")
 *   };
 *
 * connect()  →  keep fd open  →  nativeSend() writes msg  →  stop closes
 *
 * Socket I/O: NDK <sys/socket.h> / <sys/un.h>
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
#include <sys/socket.h>
#include <sys/un.h>

#define LOG_TAG "TinyStorage-JNI"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

/* Must match proot-side definition exactly */
#define SM_PATH_MAX  512
#define SM_NAME_MAX  128

typedef struct {
    uint8_t action;                /* 'A' = add, 'R' = remove */
    char    path[SM_PATH_MAX];     /* null-terminated host device path */
    char    name[SM_NAME_MAX];     /* null-terminated mount-name for /mnt/<name> */
} storage_msg_t;

/* ---- state ---- */
typedef struct {
    atomic_bool  running;
    int          sock_fd;
    pthread_t    sock_thread;
} st_t;

static st_t g;

/* ---- socket helpers ---- */

static int connect_unix(const char *path)
{
    struct sockaddr_un a;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    strncpy(a.sun_path, path, sizeof(a.sun_path) - 1);
    if (connect(fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
        close(fd);
        return -1;
    }
    LOGI("connected to %s", path);
    return fd;
}

static int write_all(int fd, const void *buf, size_t n)
{
    const uint8_t *p = (const uint8_t *)buf;
    size_t rem = n;
    while (rem) {
        ssize_t w = write(fd, p, rem);
        if (w <= 0) return -1;
        p += (size_t)w;
        rem -= (size_t)w;
    }
    return 0;
}

/* ---- socket thread: keeps fd alive, waits for shutdown ---- */
static void *sock_func(void *arg)
{
    (void)arg;
    /* Just idle; the socket is kept alive until nativeStop() closes it. */
    int fd = g.sock_fd;
    char dummy[1];
    while (atomic_load_explicit(&g.running, memory_order_acquire)) {
        ssize_t r = read(fd, dummy, sizeof(dummy));
        if (r <= 0) {
            if (r < 0 && errno == EINTR) continue;
            break; /* EOF or error → server closed */
        }
    }
    LOGI("sock thread ended, server disconnected");
    atomic_store_explicit(&g.running, false, memory_order_release);
    return NULL;
}

/* ================================================================= */
/*  JNI entry points                                                  */
/* ================================================================= */

JNIEXPORT jboolean JNICALL
Java_com_fct_tc4_TinyStorage_nativeStart(JNIEnv *env, jclass cls, jstring socketPath)
{
    (void)cls;
    const char *path = (*env)->GetStringUTFChars(env, socketPath, NULL);
    if (!path) return JNI_FALSE;
    LOGI("nativeStart → %s", path);

    memset(&g, 0, sizeof(g));
    g.sock_fd = -1;
    atomic_init(&g.running, true);

    g.sock_fd = connect_unix(path);
    (*env)->ReleaseStringUTFChars(env, socketPath, path);

    if (g.sock_fd < 0) {
        LOGE("connect failed");
        return JNI_FALSE;
    }

    if (pthread_create(&g.sock_thread, NULL, sock_func, NULL) != 0) {
        LOGE("pthread_create failed");
        close(g.sock_fd);
        g.sock_fd = -1;
        return JNI_FALSE;
    }

    return JNI_TRUE;
}

JNIEXPORT void JNICALL
Java_com_fct_tc4_TinyStorage_nativeStop(JNIEnv *env, jclass cls)
{
    (void)env; (void)cls;
    LOGI("nativeStop");

    atomic_store_explicit(&g.running, false, memory_order_release);

    if (g.sock_fd >= 0) {
        shutdown(g.sock_fd, SHUT_RDWR);
        close(g.sock_fd);
        g.sock_fd = -1;
    }
    if (g.sock_thread) {
        pthread_join(g.sock_thread, NULL);
        g.sock_thread = 0;
    }
}

JNIEXPORT jboolean JNICALL
Java_com_fct_tc4_TinyStorage_nativeSend(
    JNIEnv *env, jclass cls,
    jbyte action, jstring path, jstring name)
{
    (void)cls;

    if (!atomic_load_explicit(&g.running, memory_order_acquire))
        return JNI_FALSE;

    storage_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.action = (uint8_t)action;

    const char *p = (*env)->GetStringUTFChars(env, path, NULL);
    if (!p) return JNI_FALSE;
    strncpy(msg.path, p, sizeof(msg.path) - 1);
    (*env)->ReleaseStringUTFChars(env, path, p);

    if (name) {
        const char *n = (*env)->GetStringUTFChars(env, name, NULL);
        if (n) {
            strncpy(msg.name, n, sizeof(msg.name) - 1);
            (*env)->ReleaseStringUTFChars(env, name, n);
        }
    }

    if (write_all(g.sock_fd, &msg, sizeof(msg)) < 0) {
        LOGE("write msg failed: %s", strerror(errno));
        return JNI_FALSE;
    }

    LOGI("sent: action=%c path=%s name=%s", msg.action, msg.path, msg.name);
    return JNI_TRUE;
}
