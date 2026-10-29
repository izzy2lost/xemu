/*
 * ChdNative.kt: the game library reads a CHD's image (XbeMetadataReader)
 * through x1box_chd.c.
 */
#include <jni.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

#include "x1box_chd.h"

typedef struct JniChd {
    X1Chd *chd;
    int fd;
} JniChd;

JNIEXPORT jlong JNICALL
Java_com_izzy2lost_x1box_ChdNative_nativeOpen(JNIEnv *env, jclass cls, jint fd)
{
    /* The descriptor is ours from here (ParcelFileDescriptor.detachFd). */
    JniChd *j = malloc(sizeof(*j));
    if (!j) {
        close(fd);
        return 0;
    }
    j->fd = fd;
    j->chd = x1box_chd_open_fd(fd, NULL, 0);
    if (!j->chd) {
        close(fd);
        free(j);
        return 0;
    }
    return (jlong)(intptr_t)j;
}

JNIEXPORT jlong JNICALL
Java_com_izzy2lost_x1box_ChdNative_nativeSize(JNIEnv *env, jclass cls, jlong h)
{
    return (jlong)x1box_chd_size(((JniChd *)(intptr_t)h)->chd);
}

JNIEXPORT jboolean JNICALL
Java_com_izzy2lost_x1box_ChdNative_nativeRead(JNIEnv *env, jclass cls, jlong h,
                                              jlong off, jbyteArray out)
{
    const jsize len = (*env)->GetArrayLength(env, out);
    jbyte *buf = (*env)->GetByteArrayElements(env, out, NULL);
    if (!buf) {
        return JNI_FALSE;
    }
    const bool ok = off >= 0 &&
                    x1box_chd_read(((JniChd *)(intptr_t)h)->chd, (uint64_t)off, buf, (size_t)len);
    (*env)->ReleaseByteArrayElements(env, out, buf, ok ? 0 : JNI_ABORT);
    return ok ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_izzy2lost_x1box_ChdNative_nativeClose(JNIEnv *env, jclass cls, jlong h)
{
    JniChd *j = (JniChd *)(intptr_t)h;
    if (j) {
        x1box_chd_close(j->chd);
        close(j->fd);
        free(j);
    }
}
