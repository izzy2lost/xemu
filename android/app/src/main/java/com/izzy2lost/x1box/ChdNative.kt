package com.izzy2lost.x1box

import android.os.ParcelFileDescriptor
import java.io.Closeable

/**
 * A MAME CHD disc image (chdman createdvd, or createcd of one data track), read as the
 * disc image it holds (libx1chd: x1box_chd.c over libchdr). Both cores run CHDs
 * themselves; this is for the library's reads (XbeMetadataReader).
 */
class ChdNative private constructor(private var handle: Long) : Closeable {
  /** The size of the image it holds, in bytes. */
  val size: Long get() = nativeSize(handle)

  /** length bytes of the image at offset; null past its end or on a bad hunk. */
  fun read(offset: Long, length: Int): ByteArray? {
    if (offset < 0 || length < 0 || offset + length > size) return null
    val bytes = ByteArray(length)
    return if (nativeRead(handle, offset, bytes)) bytes else null
  }

  override fun close() {
    if (handle != 0L) {
      nativeClose(handle)
      handle = 0L
    }
  }

  companion object {
    private val MAGIC = "MComprHD".toByteArray(Charsets.US_ASCII)

    private val isLibraryLoaded: Boolean = try {
      System.loadLibrary("x1chd")
      true
    } catch (_: UnsatisfiedLinkError) {
      false
    }

    /** True when head (a file's first bytes) is a CHD's. */
    fun isChd(head: ByteArray): Boolean =
      head.size >= MAGIC.size && MAGIC.indices.all { head[it] == MAGIC[it] }

    fun isChdName(name: String): Boolean = name.endsWith(".chd", ignoreCase = true)

    /** Opens the CHD at descriptor (a dup of it: the descriptor stays the caller's). */
    fun open(descriptor: ParcelFileDescriptor): ChdNative? {
      if (!isLibraryLoaded) return null
      val fd = runCatching { descriptor.dup().detachFd() }.getOrNull() ?: return null
      val handle = nativeOpen(fd)
      return if (handle != 0L) ChdNative(handle) else null
    }

    @JvmStatic private external fun nativeOpen(fd: Int): Long
    @JvmStatic private external fun nativeSize(handle: Long): Long
    @JvmStatic private external fun nativeRead(handle: Long, offset: Long, out: ByteArray): Boolean
    @JvmStatic private external fun nativeClose(handle: Long)
  }
}
