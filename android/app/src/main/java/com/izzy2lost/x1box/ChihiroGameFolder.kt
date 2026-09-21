package com.izzy2lost.x1box

import android.content.Context
import android.net.Uri
import android.provider.DocumentsContract
import java.io.File
import java.io.InputStream
import java.io.RandomAccessFile

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
  private fun detectLayout(raf: RandomAccessFile, imageSize: Long): Layout {
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
  private fun rootLooksSane(raf: RandomAccessFile, l: Layout, imageSize: Long): Boolean {
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

  private fun fatNext(raf: RandomAccessFile, l: Layout, cluster: Int): Int {
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
    RandomAccessFile(image, "r").use { raf ->
      val layout = detectLayout(raf, image.length())
      dest.mkdirs()
      var count = 0
      count = walk(raf, layout, image.length(), 1, dest, progress, count, 0)
      if (count == 0) throw Unsupported("No files found in the image")
      return count
    }
  }

  private fun walk(
    raf: RandomAccessFile,
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
    raf: RandomAccessFile,
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
