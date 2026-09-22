package com.izzy2lost.x1box

import android.content.Context
import android.net.Uri
import android.provider.DocumentsContract
import java.io.Closeable
import java.io.File
import java.io.FileInputStream
import java.io.InputStream
import java.io.RandomAccessFile
import java.nio.ByteBuffer
import java.nio.channels.FileChannel

/**
 * Chihiro games reach the emulator as a directory of loose files: xemu's
 * Chihiro support scans it with opendir/readdir, builds the mbfs FATX image in
 * memory, and reads boot.id from it directly. SAF hands us content URIs rather
 * than POSIX paths, so a folder the user picks has to be materialised under the
 * app's own storage before native code can see it.
 *
 * A prebuilt mbfs image (a .bin whose first four bytes are "FATX", which is how
 * most Chihiro netboot dumps ship) is unpacked into the same shape, so one of
 * those can be used without the user having to extract it themselves.
 */
object ChihiroGameFolder {

  private const val SECTOR_SIZE = 512
  private const val SUPERBLOCK_SIZE = 4096
  private const val DIRENT_SIZE = 64
  private const val MAX_NAME = 42

  /** Cluster 0 and 1 are reserved; a chain ends at 0xFFF8..0xFFFF (FAT16). */
  private const val FAT16_END_MIN = 0xFFF8

  class Unsupported(message: String) : Exception(message)

  private data class Layout(
    val clusterSize: Int,
    val fatOffset: Long,
    val fatEntryBytes: Int,
    val totalClusters: Int,
    val dataOffset: Long,
  )

  /**
   * Seekable access to an image. A .bin in the library arrives as a SAF
   * content URI with no path behind it, so reads go through a channel that
   * can be opened from either a File or a file descriptor.
   */
  private class ImageSource(private val ch: FileChannel) : Closeable {
    private var pos = 0L

    fun seek(offset: Long) {
      pos = offset
    }

    fun read(b: ByteArray): Int = read(b, 0, b.size)

    fun read(b: ByteArray, off: Int, len: Int): Int {
      val bb = ByteBuffer.wrap(b, off, len)
      var total = 0
      while (bb.hasRemaining()) {
        val n = ch.read(bb, pos + total)
        if (n <= 0) break
        total += n
      }
      pos += total
      return if (total == 0) -1 else total
    }

    fun readFully(b: ByteArray) {
      if (read(b) != b.size) throw java.io.EOFException()
    }

    override fun close() {
      ch.close()
    }
  }

  fun isFatxImage(input: InputStream): Boolean {
    val magic = ByteArray(4)
    return input.read(magic) == 4 && String(magic, Charsets.US_ASCII) == "FATX"
  }

  /** Where an unpacked or copied game lives. Stable per source name. */
  fun gameDir(context: Context, name: String): File {
    val base = context.getExternalFilesDir(null) ?: context.filesDir
    return File(File(base, "x1box"), "chihiro/" + sanitize(name))
  }

  private fun sanitize(name: String): String =
    name.replace(Regex("[^A-Za-z0-9._-]"), "_").take(64).ifEmpty { "game" }

  private fun u16(b: ByteArray, o: Int): Int =
    (b[o].toInt() and 0xFF) or ((b[o + 1].toInt() and 0xFF) shl 8)

  private fun u32(b: ByteArray, o: Int): Long =
    (b[o].toLong() and 0xFF) or
      ((b[o + 1].toLong() and 0xFF) shl 8) or
      ((b[o + 2].toLong() and 0xFF) shl 16) or
      ((b[o + 3].toLong() and 0xFF) shl 24)

  /**
   * The superblock records the cluster size but not the FAT length, so the FAT
   * has to be sized from the partition the image was built for. Try the sizes a
   * Chihiro mbfs image is actually built at, largest first, and accept the one
   * whose root directory parses.
   */
  private fun detectLayout(raf: ImageSource, imageSize: Long): Layout {
    val sb = ByteArray(16)
    raf.seek(0)
    raf.readFully(sb)
    if (String(sb, 0, 4, Charsets.US_ASCII) != "FATX") {
      throw Unsupported("Not a FATX image")
    }
    val clusterSectors = u32(sb, 8).toInt()
    if (clusterSectors <= 0 || clusterSectors > 128) {
      throw Unsupported("Bad FATX cluster size ($clusterSectors sectors)")
    }
    val clusterSize = clusterSectors * SECTOR_SIZE

    // 0xF8000 sectors is the mbfs partition inside a 512 MiB DIMM; also try the
    // 1 GiB board and the image's own length for images built to fit exactly.
    val candidates = linkedSetOf(
      0xF8000L * SECTOR_SIZE,
      0x1F8000L * SECTOR_SIZE,
      imageSize,
    )

    for (partitionBytes in candidates) {
      val totalClusters = (partitionBytes / clusterSize).toInt() + 1
      val entryBytes = if (totalClusters >= 0xFFF0) 4 else 2
      val fatBytes = totalClusters.toLong() * entryBytes
      val fatAligned = (fatBytes + SUPERBLOCK_SIZE - 1) / SUPERBLOCK_SIZE * SUPERBLOCK_SIZE
      val dataOffset = SUPERBLOCK_SIZE + fatAligned
      if (dataOffset >= imageSize) continue
      val layout = Layout(clusterSize, SUPERBLOCK_SIZE.toLong(), entryBytes,
                          totalClusters, dataOffset)
      if (rootLooksSane(raf, layout, imageSize)) return layout
    }
    throw Unsupported("Could not work out the FATX layout for this image")
  }

  /** The root directory is cluster 1; a sane one starts with a usable entry. */
  private fun rootLooksSane(raf: ImageSource, l: Layout, imageSize: Long): Boolean {
    val off = clusterOffset(l, 1)
    if (off < 0 || off + DIRENT_SIZE > imageSize) return false
    val e = ByteArray(DIRENT_SIZE)
    raf.seek(off)
    if (raf.read(e) != DIRENT_SIZE) return false
    val nameLen = e[0].toInt() and 0xFF
    if (nameLen == 0 || nameLen == 0xFF || nameLen > MAX_NAME) return false
    val attr = e[1].toInt() and 0xFF
    if (attr != 0x10 && attr != 0x20 && attr != 0x00) return false
    for (i in 0 until nameLen) {
      val c = e[2 + i].toInt() and 0xFF
      if (c < 0x20 || c > 0x7E) return false
    }
    return true
  }

  private fun clusterOffset(l: Layout, cluster: Int): Long =
    l.dataOffset + (cluster - 1).toLong() * l.clusterSize

  private fun fatNext(raf: ImageSource, l: Layout, cluster: Int): Int {
    val off = l.fatOffset + cluster.toLong() * l.fatEntryBytes
    raf.seek(off)
    val b = ByteArray(l.fatEntryBytes)
    if (raf.read(b) != l.fatEntryBytes) return FAT16_END_MIN
    return if (l.fatEntryBytes == 2) u16(b, 0) else u32(b, 0).toInt()
  }

  private fun isChainEnd(l: Layout, v: Int): Boolean =
    if (l.fatEntryBytes == 2) v >= FAT16_END_MIN || v == 0
    else v >= 0xFFFFFFF8.toInt() || v == 0

  /**
   * Unpack a FATX image into [dest]. Returns the number of files written.
   * [progress] is called with the running file count so a long unpack can be
   * reported to the user.
   */
  fun extract(
    image: File,
    dest: File,
    progress: (Int, String) -> Unit = { _, _ -> },
  ): Int {
    ImageSource(RandomAccessFile(image, "r").channel).use { raf ->
      val layout = detectLayout(raf, image.length())
      dest.mkdirs()
      var count = 0
      count = walk(raf, layout, image.length(), 1, dest, progress, count, 0)
      if (count == 0) throw Unsupported("No files found in the image")
      return count
    }
  }

  private fun walk(
    raf: ImageSource,
    l: Layout,
    imageSize: Long,
    firstCluster: Int,
    dest: File,
    progress: (Int, String) -> Unit,
    startCount: Int,
    depth: Int,
  ): Int {
    if (depth > 8) return startCount
    var count = startCount
    var cluster = firstCluster
    val seen = HashSet<Int>()
    val entry = ByteArray(DIRENT_SIZE)

    while (cluster >= 1 && !isChainEnd(l, cluster) && seen.add(cluster)) {
      val base = clusterOffset(l, cluster)
      if (base < 0 || base >= imageSize) break
      val perCluster = l.clusterSize / DIRENT_SIZE
      for (i in 0 until perCluster) {
        val off = base + i.toLong() * DIRENT_SIZE
        if (off + DIRENT_SIZE > imageSize) break
        raf.seek(off)
        if (raf.read(entry) != DIRENT_SIZE) break

        val nameLen = entry[0].toInt() and 0xFF
        if (nameLen == 0x00 || nameLen == 0xFF) continue   // free / end of dir
        if (nameLen == 0xE5) continue                       // deleted
        if (nameLen > MAX_NAME) continue

        val name = String(entry, 2, nameLen, Charsets.US_ASCII)
          .replace('\u0000', ' ').trim()
        if (name.isEmpty() || name == "." || name == "..") continue
        if (name.contains('/') || name.contains('\\')) continue

        val isDir = (entry[1].toInt() and 0x10) != 0
        val start = u32(entry, 44).toInt()
        val size = u32(entry, 48)

        if (isDir) {
          val sub = File(dest, name)
          sub.mkdirs()
          if (start >= 1) {
            count = walk(raf, l, imageSize, start, sub, progress, count, depth + 1)
          }
        } else {
          if (start < 1 || size < 0) continue
          writeFile(raf, l, imageSize, start, size, File(dest, name))
          count++
          progress(count, name)
        }
      }
      cluster = fatNext(raf, l, cluster)
    }
    return count
  }

  private fun writeFile(
    raf: ImageSource,
    l: Layout,
    imageSize: Long,
    firstCluster: Int,
    size: Long,
    out: File,
  ) {
    out.parentFile?.mkdirs()
    val buf = ByteArray(l.clusterSize)
    var remaining = size
    var cluster = firstCluster
    val seen = HashSet<Int>()
    out.outputStream().buffered().use { os ->
      while (remaining > 0 && cluster >= 1 && !isChainEnd(l, cluster) && seen.add(cluster)) {
        val off = clusterOffset(l, cluster)
        if (off < 0 || off >= imageSize) break
        val want = minOf(remaining, l.clusterSize.toLong()).toInt()
        raf.seek(off)
        val got = raf.read(buf, 0, want)
        if (got <= 0) break
        os.write(buf, 0, got)
        remaining -= got
        cluster = fatNext(raf, l, cluster)
      }
    }
  }

  /*
   * ---------------------------------------------------------------------
   * Reading the game's real name out of an image
   *
   * A netboot .bin is named for the board ("CT3.bin"), not the game, so the
   * filename is no use for matching cover art. The title is inside: every
   * Xbox executable carries a certificate holding a 40-character UTF-16
   * title, and the mbfs image holds the game's .xbe. boot.id names which
   * .xbe is the game, so prefer that and fall back to the first one found.
   * ---------------------------------------------------------------------
   */

  private const val XBE_MAGIC = "XBEH"
  private const val XBE_BASE_ADDR_OFFSET = 0x104
  private const val XBE_CERT_ADDR_OFFSET = 0x118
  private const val XBE_CERT_TITLE_OFFSET = 0x0C
  private const val XBE_TITLE_CHARS = 40

  /*
   * boot.id, as SEGA writes it:
   *   +0x00 "BTID"   +0x20 "XBAM"   +0x30 "SBFY"
   *   +0x60 publisher            ("Hitmaker co.,ltd.")
   *   +0x80 game name, 32 bytes, space padded  ("CrazyTaxi HighRoller")
   *   +0xA0 the game's executable ("\\ctx_ac[r].xbe")
   */
  private const val BOOTID_MAGIC_OFFSET = 0x00
  private const val BOOTID_XBAM_OFFSET = 0x20
  private const val BOOTID_NAME_OFFSET = 0x80
  private const val BOOTID_NAME_LENGTH = 32
  private const val BOOTID_EXECUTABLE_OFFSET = 0xA0

  private data class Entry(
    val name: String,
    val isDir: Boolean,
    val startCluster: Int,
    val size: Long,
  )

  /**
   * The game's own title, read out of [image], or null when the image is not
   * FATX, holds no usable executable, or the certificate is unreadable.
   */
  fun readTitle(image: File): String? = try {
    ImageSource(RandomAccessFile(image, "r").channel).use { raf ->
      readTitle(raf, image.length())
    }
  } catch (_: Exception) {
    null
  }

  /** As above, for a .bin the library only has a content URI for. */
  fun readTitle(context: Context, uri: Uri): String? = try {
    context.contentResolver.openFileDescriptor(uri, "r")?.use { pfd ->
      FileInputStream(pfd.fileDescriptor).use { stream ->
        readTitle(ImageSource(stream.channel), pfd.statSize)
      }
    }
  } catch (_: Exception) {
    null
  }

  private fun readTitle(raf: ImageSource, size: Long): String? = try {
    // A provider that cannot stat the file reports -1; there is nothing to
    // size the FAT against, so leave the title to the filename.
    if (size <= SUPERBLOCK_SIZE) {
      null
    } else {
      val layout = detectLayout(raf, size)
      val entries = listEntries(raf, layout, size, 1, 0)
      val bootId = readBootId(raf, layout, size, entries)

      /*
       * Prefer the name boot.id carries. An arcade build leaves the XBE
       * certificate's title empty -- every Chihiro .xbe checked reads back
       * as blank there -- so the certificate is only a fallback, for an
       * image with no usable boot.id.
       */
      bootIdName(bootId) ?: run {
        val preferred = bootIdExecutable(bootId)
        val xbe = entries.firstOrNull {
          !it.isDir && preferred != null && it.name.equals(preferred, ignoreCase = true)
        } ?: entries.firstOrNull {
          !it.isDir && it.name.endsWith(".xbe", ignoreCase = true)
        }
        if (xbe == null) {
          null
        } else {
          readXbeTitle(readEntryBytes(raf, layout, size, xbe, XBE_READ_LIMIT))
        }
      }
    }
  } catch (_: Exception) {
    null
  }

  /** Enough for the XBE header and the certificate that follows it. */
  private const val XBE_READ_LIMIT = 64 * 1024

  /** The raw boot.id bytes from [entries], or null when there is no usable one. */
  private fun readBootId(
    raf: ImageSource,
    l: Layout,
    imageSize: Long,
    entries: List<Entry>,
  ): ByteArray? {
    val bootId = entries.firstOrNull {
      !it.isDir && it.name.equals("boot.id", ignoreCase = true)
    } ?: return null
    val data = readEntryBytes(raf, l, imageSize, bootId, 0x200)
    if (data.size < BOOTID_XBAM_OFFSET + 4) return null
    if (String(data, BOOTID_MAGIC_OFFSET, 4, Charsets.US_ASCII) != "BTID") return null
    if (String(data, BOOTID_XBAM_OFFSET, 4, Charsets.US_ASCII) != "XBAM") return null
    return data
  }

  /** The game's own name, as boot.id spells it. */
  private fun bootIdName(data: ByteArray?): String? {
    if (data == null || data.size < BOOTID_NAME_OFFSET + BOOTID_NAME_LENGTH) return null
    return asciiField(data, BOOTID_NAME_OFFSET, BOOTID_NAME_LENGTH)
  }

  /** The executable boot.id names, with any leading path stripped. */
  private fun bootIdExecutable(data: ByteArray?): String? {
    if (data == null || data.size < BOOTID_EXECUTABLE_OFFSET + 1) return null
    val raw = asciiField(data, BOOTID_EXECUTABLE_OFFSET, 64) ?: return null
    return raw.trimStart('\\', '/').substringAfterLast('\\').substringAfterLast('/')
      .takeIf { it.isNotEmpty() }
  }

  /** A fixed-width, NUL- or space-padded ASCII field. */
  private fun asciiField(data: ByteArray, offset: Int, maxLength: Int): String? {
    val end = minOf(data.size, offset + maxLength)
    val sb = StringBuilder()
    for (i in offset until end) {
      val c = data[i].toInt() and 0xFF
      if (c == 0) break
      if (c < 0x20 || c > 0x7E) continue
      sb.append(c.toChar())
    }
    return sb.toString().trim().ifEmpty { null }
  }

  /** Pull the certificate title out of an XBE image's leading bytes. */
  private fun readXbeTitle(xbe: ByteArray): String? {
    if (xbe.size < XBE_CERT_ADDR_OFFSET + 4) return null
    if (String(xbe, 0, 4, Charsets.US_ASCII) != XBE_MAGIC) return null

    val base = u32(xbe, XBE_BASE_ADDR_OFFSET)
    val certVa = u32(xbe, XBE_CERT_ADDR_OFFSET)
    if (certVa <= base) return null
    val certOffset = (certVa - base).toInt()
    val titleAt = certOffset + XBE_CERT_TITLE_OFFSET
    if (titleAt < 0 || titleAt + XBE_TITLE_CHARS * 2 > xbe.size) return null

    val sb = StringBuilder()
    for (i in 0 until XBE_TITLE_CHARS) {
      val c = u16(xbe, titleAt + i * 2)
      if (c == 0) break
      // Keep it to characters that can appear in a title.
      if (c < 0x20) continue
      sb.append(c.toChar())
    }
    return sb.toString().trim().ifEmpty { null }
  }

  /** Directory entries under [firstCluster], following subdirectories shallowly. */
  private fun listEntries(
    raf: ImageSource,
    l: Layout,
    imageSize: Long,
    firstCluster: Int,
    depth: Int,
  ): List<Entry> {
    if (depth > 2) return emptyList()
    val out = ArrayList<Entry>()
    var cluster = firstCluster
    val seen = HashSet<Int>()
    val entry = ByteArray(DIRENT_SIZE)

    while (cluster >= 1 && !isChainEnd(l, cluster) && seen.add(cluster)) {
      val base = clusterOffset(l, cluster)
      if (base < 0 || base >= imageSize) break
      for (i in 0 until l.clusterSize / DIRENT_SIZE) {
        val off = base + i.toLong() * DIRENT_SIZE
        if (off + DIRENT_SIZE > imageSize) break
        raf.seek(off)
        if (raf.read(entry) != DIRENT_SIZE) break

        val nameLen = entry[0].toInt() and 0xFF
        if (nameLen == 0x00 || nameLen == 0xFF || nameLen == 0xE5) continue
        if (nameLen > MAX_NAME) continue
        val name = String(entry, 2, nameLen, Charsets.US_ASCII)
          .replace('\u0000', ' ').trim()
        if (name.isEmpty() || name == "." || name == "..") continue

        val isDir = (entry[1].toInt() and 0x10) != 0
        val start = u32(entry, 44).toInt()
        val fileSize = u32(entry, 48)
        out.add(Entry(name, isDir, start, fileSize))
        if (isDir && start >= 1) {
          out.addAll(listEntries(raf, l, imageSize, start, depth + 1))
        }
      }
      cluster = fatNext(raf, l, cluster)
    }
    return out
  }

  /** Read at most [limit] bytes of [entry] by following its cluster chain. */
  private fun readEntryBytes(
    raf: ImageSource,
    l: Layout,
    imageSize: Long,
    entry: Entry,
    limit: Int,
  ): ByteArray {
    val want = minOf(entry.size, limit.toLong())
    if (want <= 0 || entry.startCluster < 1) return ByteArray(0)
    val out = java.io.ByteArrayOutputStream(want.toInt())
    val buf = ByteArray(l.clusterSize)
    var remaining = want
    var cluster = entry.startCluster
    val seen = HashSet<Int>()
    while (remaining > 0 && cluster >= 1 && !isChainEnd(l, cluster) && seen.add(cluster)) {
      val off = clusterOffset(l, cluster)
      if (off < 0 || off >= imageSize) break
      val chunk = minOf(remaining, l.clusterSize.toLong()).toInt()
      raf.seek(off)
      val got = raf.read(buf, 0, chunk)
      if (got <= 0) break
      out.write(buf, 0, got)
      remaining -= got
      cluster = fatNext(raf, l, cluster)
    }
    return out.toByteArray()
  }

  /** Copy a SAF folder tree into [dest] so native code can scan it by path. */
  fun copyTree(
    context: Context,
    treeUri: Uri,
    dest: File,
    progress: (Int, String) -> Unit = { _, _ -> },
  ): Int {
    dest.mkdirs()
    val rootId = DocumentsContract.getTreeDocumentId(treeUri)
    return copyDoc(context, treeUri, rootId, dest, progress, 0, 0)
  }

  private fun copyDoc(
    context: Context,
    treeUri: Uri,
    docId: String,
    dest: File,
    progress: (Int, String) -> Unit,
    startCount: Int,
    depth: Int,
  ): Int {
    if (depth > 8) return startCount
    var count = startCount
    val children = DocumentsContract.buildChildDocumentsUriUsingTree(treeUri, docId)
    context.contentResolver.query(
      children,
      arrayOf(
        DocumentsContract.Document.COLUMN_DOCUMENT_ID,
        DocumentsContract.Document.COLUMN_DISPLAY_NAME,
        DocumentsContract.Document.COLUMN_MIME_TYPE,
      ),
      null, null, null,
    )?.use { c ->
      while (c.moveToNext()) {
        val childId = c.getString(0) ?: continue
        val name = c.getString(1) ?: continue
        val mime = c.getString(2) ?: ""
        if (name.contains('/') || name.contains('\\') || name == "." || name == "..") continue
        if (mime == DocumentsContract.Document.MIME_TYPE_DIR) {
          val sub = File(dest, name)
          sub.mkdirs()
          count = copyDoc(context, treeUri, childId, sub, progress, count, depth + 1)
        } else {
          val uri = DocumentsContract.buildDocumentUriUsingTree(treeUri, childId)
          val out = File(dest, name)
          context.contentResolver.openInputStream(uri)?.use { ins ->
            out.outputStream().buffered().use { os -> ins.copyTo(os) }
          }
          count++
          progress(count, name)
        }
      }
    }
    return count
  }
}
