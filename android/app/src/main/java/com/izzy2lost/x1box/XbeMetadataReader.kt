package com.izzy2lost.x1box

import android.content.Context
import android.graphics.Bitmap
import android.net.Uri
import java.io.File
import java.io.FileInputStream
import java.nio.ByteBuffer
import java.nio.channels.FileChannel
import java.security.MessageDigest

/**
 * What a disc image's default.xbe says about itself: the title id and name
 * from its certificate, and the title image the dashboard shows (the
 * $$XTIMAGE section, an XPR0 texture bundle).
 *
 * The library names games after their files and finds covers by that name;
 * the certificate's name is a second try at a match, and the title image
 * stands in when neither finds a cover.
 *
 * Adapted from xenia-edge's src/xenia/vfs/xbe_metadata.cc (BSD).
 */
object XbeMetadataReader {

  /** version: the certificate's, which tells a disc's releases apart (54430006 is .5 and .6). */
  class Metadata(val titleId: Long, val titleName: String, val icon: Bitmap?, val version: Long = 0) {
    /** The disc release's key, title ID and version (4D530004.9), as hakuX keys mods and cheats. */
    val gameKey: String get() = String.format(java.util.Locale.ROOT, "%08X.%d", titleId, version)
  }

  private const val SECTOR_SIZE = 2048L
  private const val VOLUME_DESCRIPTOR_OFFSET = 32L * SECTOR_SIZE
  private const val VOLUME_DESCRIPTOR_SIZE = 0x800
  private const val VOLUME_TRAILING_MAGIC_OFFSET = 0x7EC
  private const val VOLUME_ROOT_SECTOR = 0x14
  private const val VOLUME_ROOT_SIZE = 0x18
  private val VOLUME_MAGIC = "MICROSOFT*XBOX*MEDIA".toByteArray(Charsets.US_ASCII)

  /* Where the game partition starts: an XISO, then XGD1, XGD2 and XGD3 redumps. */
  private val PARTITION_OFFSETS = longArrayOf(0L, 405_798_912L, 265_879_552L, 34_078_720L)

  /* A directory entry: left and right subtree (in dwords), sector, size, attributes, name. */
  private const val DIRENT_HEADER_SIZE = 14
  private const val DIRENT_PADDING = 0xFFFF
  private const val MAX_ROOT_DIRECTORY_SIZE = 1024 * 1024

  private const val XBE_SIGNATURE = 0x48454258L // "XBEH"
  private const val XPR_SIGNATURE = 0x30525058L // "XPR0"

  /* XBE header fields. Addresses in it are where the loader maps the headers, from the base. */
  private const val XBE_BASE_ADDRESS = 0x104
  private const val XBE_SIZE_OF_HEADERS = 0x108
  private const val XBE_CERTIFICATE_ADDRESS = 0x118
  private const val XBE_SECTION_COUNT = 0x11C
  private const val XBE_SECTION_HEADERS_ADDRESS = 0x120
  private const val XBE_HEADER_MIN_SIZE = 0x178

  private const val SECTION_HEADER_SIZE = 0x38
  private const val SECTION_RAW_ADDRESS = 0xC
  private const val SECTION_RAW_SIZE = 0x10
  private const val SECTION_NAME_ADDRESS = 0x14

  private const val CERTIFICATE_TITLE_ID = 0x8
  private const val CERTIFICATE_TITLE_NAME = 0xC
  private const val CERTIFICATE_TITLE_NAME_LENGTH = 40
  private const val CERTIFICATE_VERSION = 0xAC
  private const val CERTIFICATE_MIN_SIZE = 0xB0

  /* Bounds on a well-formed executable, so a damaged one can't make this read gigabytes. */
  private const val MAX_HEADERS_SIZE = 1024 * 1024
  private const val MAX_TITLE_IMAGE_SIZE = 4 * 1024 * 1024

  private val TITLE_IMAGE_SECTION = "\$\$XTIMAGE\u0000".toByteArray(Charsets.US_ASCII)

  /* Xbox Direct3D texture formats a title image comes in. */
  private const val FORMAT_A8R8G8B8 = 0x06
  private const val FORMAT_X8R8G8B8 = 0x07
  private const val FORMAT_DXT1 = 0x0C
  private const val FORMAT_DXT3 = 0x0E
  private const val FORMAT_DXT5 = 0x0F
  private const val FORMAT_LINEAR_A8R8G8B8 = 0x12
  private const val FORMAT_LINEAR_X8R8G8B8 = 0x1E

  /** Null for anything that isn't an original Xbox disc image, or can't be read. icon: decode the title image. */
  fun read(context: Context, uri: Uri, icon: Boolean = true): Metadata? {
    return try {
      context.contentResolver.openFileDescriptor(uri, "r")?.use { descriptor ->
        FileInputStream(descriptor.fileDescriptor).use { input -> readDiscImage(input.channel, icon) }
      }
    } catch (_: Exception) {
      null
    }
  }

  private fun readDiscImage(channel: FileChannel, wantIcon: Boolean): Metadata? {
    for (partition in PARTITION_OFFSETS) {
      val volume = readAt(channel, partition + VOLUME_DESCRIPTOR_OFFSET, VOLUME_DESCRIPTOR_SIZE)
        ?: continue
      if (!startsWith(volume, 0, VOLUME_MAGIC) ||
        !startsWith(volume, VOLUME_TRAILING_MAGIC_OFFSET, VOLUME_MAGIC)
      ) {
        continue
      }
      val rootSector = u32(volume, VOLUME_ROOT_SECTOR)
      val rootSize = u32(volume, VOLUME_ROOT_SIZE)
      if (rootSize <= 0L || rootSize > MAX_ROOT_DIRECTORY_SIZE) {
        return null
      }
      val root = readAt(channel, partition + rootSector * SECTOR_SIZE, rootSize.toInt())
        ?: return null
      val (sector, size) = findFile(root, "default.xbe") ?: return null
      val start = partition + sector * SECTOR_SIZE
      return parseXbe(wantIcon) { offset, length ->
        if (offset < 0 || offset + length > size) null else readAt(channel, start + offset, length)
      }
    }
    return null
  }

  /*
   * Walks every node of a directory's tree rather than searching it by name,
   * so an image whose authoring tool sorted names differently still resolves.
   */
  private fun findFile(directory: ByteArray, name: String): Pair<Long, Long>? {
    val pending = ArrayDeque<Int>()
    val visited = HashSet<Int>()
    pending.add(0)
    while (pending.isNotEmpty()) {
      val entry = pending.removeLast() * 4
      if (!visited.add(entry) || entry + DIRENT_HEADER_SIZE > directory.size) {
        continue
      }
      val left = u16(directory, entry)
      val right = u16(directory, entry + 2)
      if (left == DIRENT_PADDING) {
        continue
      }
      val nameLength = directory[entry + 13].toInt() and 0xFF
      if (entry + DIRENT_HEADER_SIZE + nameLength > directory.size) {
        continue
      }
      val entryName = String(directory, entry + DIRENT_HEADER_SIZE, nameLength, Charsets.ISO_8859_1)
      if (entryName.equals(name, ignoreCase = true)) {
        return u32(directory, entry + 4) to u32(directory, entry + 8)
      }
      if (left != 0) pending.add(left)
      if (right != 0) pending.add(right)
    }
    return null
  }

  private fun parseXbe(wantIcon: Boolean, read: (offset: Long, length: Int) -> ByteArray?): Metadata? {
    val prefix = read(0, XBE_HEADER_MIN_SIZE) ?: return null
    if (u32(prefix, 0) != XBE_SIGNATURE) {
      return null
    }
    val base = u32(prefix, XBE_BASE_ADDRESS)
    val headersSize = u32(prefix, XBE_SIZE_OF_HEADERS)
    if (headersSize < XBE_HEADER_MIN_SIZE || headersSize > MAX_HEADERS_SIZE) {
      return null
    }
    val headers = read(0, headersSize.toInt()) ?: return null

    /* Where a header address lands in the headers, if |size| bytes there fit. */
    fun headerOffset(address: Long, size: Long): Int? {
      val offset = address - base
      if (offset < 0 || offset > headersSize || headersSize - offset < size) {
        return null
      }
      return offset.toInt()
    }

    val certificate = headerOffset(
      u32(headers, XBE_CERTIFICATE_ADDRESS),
      CERTIFICATE_MIN_SIZE.toLong(),
    ) ?: return null
    val titleId = u32(headers, certificate + CERTIFICATE_TITLE_ID)
    val version = u32(headers, certificate + CERTIFICATE_VERSION)
    val name = StringBuilder()
    for (i in 0 until CERTIFICATE_TITLE_NAME_LENGTH) {
      val c = u16(headers, certificate + CERTIFICATE_TITLE_NAME + i * 2)
      if (c == 0) {
        break
      }
      name.append(c.toChar())
    }

    var icon: Bitmap? = null
    val sectionCount = u32(headers, XBE_SECTION_COUNT)
    val sections = headerOffset(
      u32(headers, XBE_SECTION_HEADERS_ADDRESS),
      sectionCount * SECTION_HEADER_SIZE,
    )
    if (sections != null && wantIcon) {
      for (i in 0 until sectionCount.toInt()) {
        val section = sections + i * SECTION_HEADER_SIZE
        val nameOffset = headerOffset(
          u32(headers, section + SECTION_NAME_ADDRESS),
          TITLE_IMAGE_SECTION.size.toLong(),
        )
        if (nameOffset == null || !startsWith(headers, nameOffset, TITLE_IMAGE_SECTION)) {
          continue
        }
        val imageSize = u32(headers, section + SECTION_RAW_SIZE)
        if (imageSize <= MAX_TITLE_IMAGE_SIZE) {
          read(u32(headers, section + SECTION_RAW_ADDRESS), imageSize.toInt())
            ?.let { icon = decodeTitleImage(it) }
        }
        break
      }
    }
    return Metadata(titleId, name.toString().trim(), icon, version)
  }

  /*
   * Decodes an XPR0 bundle's first texture. Null for a format it doesn't know.
   * The HDD's TitleImage.xbx and saveimage.xbx are the same kind of bundle.
   */
  internal fun decodeTitleImage(xpr: ByteArray): Bitmap? {
    val headerSize = 12
    val resourceData = headerSize + 4
    val resourceFormat = headerSize + 12
    if (xpr.size < resourceFormat + 4 || u32(xpr, 0) != XPR_SIGNATURE) {
      return null
    }
    val dataStart = u32(xpr, 8) + u32(xpr, resourceData)
    val format = u32(xpr, resourceFormat)
    val texelFormat = ((format shr 8) and 0xFF).toInt()
    val widthLog2 = ((format shr 20) and 0xF).toInt()
    val heightLog2 = ((format shr 24) and 0xF).toInt()
    if (widthLog2 > 10 || heightLog2 > 10 || dataStart > xpr.size) {
      return null
    }
    val width = 1 shl widthLog2
    val height = 1 shl heightLog2
    val data = dataStart.toInt()
    val pixels = IntArray(width * height)

    when (texelFormat) {
      FORMAT_A8R8G8B8, FORMAT_X8R8G8B8, FORMAT_LINEAR_A8R8G8B8, FORMAT_LINEAR_X8R8G8B8 -> {
        if (xpr.size - data < pixels.size * 4) {
          return null
        }
        val swizzled = texelFormat == FORMAT_A8R8G8B8 || texelFormat == FORMAT_X8R8G8B8
        val opaque = texelFormat == FORMAT_X8R8G8B8 || texelFormat == FORMAT_LINEAR_X8R8G8B8
        for (y in 0 until height) {
          for (x in 0 until width) {
            val texel = if (swizzled) swizzledOffset(x, y, width, height) else y * width + x
            /* Little-endian BGRA in memory reads as ARGB, Bitmap's own layout. */
            val argb = u32(xpr, data + texel * 4).toInt()
            pixels[y * width + x] = if (opaque) argb or (0xFF shl 24) else argb
          }
        }
      }
      FORMAT_DXT1, FORMAT_DXT3, FORMAT_DXT5 -> {
        if (width < 4 || height < 4) {
          return null
        }
        val dxt1 = texelFormat == FORMAT_DXT1
        val blockSize = if (dxt1) 8 else 16
        if (xpr.size - data < (width / 4) * (height / 4) * blockSize) {
          return null
        }
        var block = data
        for (y in 0 until height step 4) {
          for (x in 0 until width step 4) {
            val origin = y * width + x
            decodeDxtColorBlock(xpr, if (dxt1) block else block + 8, dxt1, pixels, origin, width)
            if (!dxt1) {
              decodeDxtAlphaBlock(xpr, block, texelFormat == FORMAT_DXT5, pixels, origin, width)
            }
            block += blockSize
          }
        }
      }
      else -> return null
    }
    return Bitmap.createBitmap(pixels, width, height, Bitmap.Config.ARGB_8888)
  }

  /*
   * Index into a swizzled texture of a texel. Bits of x and y interleave, x
   * first, until the smaller dimension runs out of them.
   */
  private fun swizzledOffset(x: Int, y: Int, width: Int, height: Int): Int {
    var offset = 0
    var bit = 1
    var mask = 1
    while (mask < width || mask < height) {
      if (mask < width) {
        if (x and mask != 0) offset = offset or bit
        bit = bit shl 1
      }
      if (mask < height) {
        if (y and mask != 0) offset = offset or bit
        bit = bit shl 1
      }
      mask = mask shl 1
    }
    return offset
  }

  private fun rgb565ToArgb(color: Int): Int {
    val r = (color shr 11) and 0x1F
    val g = (color shr 5) and 0x3F
    val b = color and 0x1F
    return argb(255, (r shl 3) or (r shr 2), (g shl 2) or (g shr 4), (b shl 3) or (b shr 2))
  }

  private fun argb(a: Int, r: Int, g: Int, b: Int): Int = (a shl 24) or (r shl 16) or (g shl 8) or b

  private fun channel(color: Int, shift: Int): Int = (color shr shift) and 0xFF

  /* Three colors and transparent black only for a DXT1 block with c0 <= c1. */
  private fun decodeDxtColorBlock(
    src: ByteArray,
    block: Int,
    dxt1: Boolean,
    pixels: IntArray,
    origin: Int,
    pitch: Int,
  ) {
    val c0 = u16(src, block)
    val c1 = u16(src, block + 2)
    val p0 = rgb565ToArgb(c0)
    val p1 = rgb565ToArgb(c1)
    val fourColors = !dxt1 || c0 > c1
    val palette = IntArray(4)
    palette[0] = p0
    palette[1] = p1
    val mixed = IntArray(6)
    for ((i, shift) in intArrayOf(16, 8, 0).withIndex()) {
      val a = channel(p0, shift)
      val b = channel(p1, shift)
      if (fourColors) {
        mixed[i] = (2 * a + b) / 3
        mixed[i + 3] = (a + 2 * b) / 3
      } else {
        mixed[i] = (a + b) / 2
        mixed[i + 3] = 0
      }
    }
    palette[2] = argb(255, mixed[0], mixed[1], mixed[2])
    palette[3] = argb(if (fourColors) 255 else 0, mixed[3], mixed[4], mixed[5])
    val indices = u32(src, block + 4)
    for (y in 0 until 4) {
      for (x in 0 until 4) {
        val index = ((indices shr (2 * (y * 4 + x))) and 3).toInt()
        pixels[origin + y * pitch + x] = palette[index]
      }
    }
  }

  /* Replaces the alpha of a 4x4 block's texels from a DXT3 or DXT5 alpha block. */
  private fun decodeDxtAlphaBlock(
    src: ByteArray,
    block: Int,
    dxt5: Boolean,
    pixels: IntArray,
    origin: Int,
    pitch: Int,
  ) {
    val alphas = IntArray(16)
    if (!dxt5) {
      val bits = u32(src, block) or (u32(src, block + 4) shl 32)
      for (i in 0 until 16) {
        alphas[i] = ((bits ushr (4 * i)) and 0xF).toInt() * 17
      }
    } else {
      val palette = IntArray(8)
      palette[0] = src[block].toInt() and 0xFF
      palette[1] = src[block + 1].toInt() and 0xFF
      if (palette[0] > palette[1]) {
        for (i in 1 until 7) {
          palette[i + 1] = ((7 - i) * palette[0] + i * palette[1]) / 7
        }
      } else {
        for (i in 1 until 5) {
          palette[i + 1] = ((5 - i) * palette[0] + i * palette[1]) / 5
        }
        palette[6] = 0
        palette[7] = 255
      }
      var indices = 0L
      for (i in 0 until 6) {
        indices = indices or ((src[block + 2 + i].toLong() and 0xFF) shl (8 * i))
      }
      for (i in 0 until 16) {
        alphas[i] = palette[((indices shr (3 * i)) and 7).toInt()]
      }
    }
    for (i in 0 until 16) {
      val texel = origin + (i / 4) * pitch + (i % 4)
      pixels[texel] = (pixels[texel] and 0x00FFFFFF) or (alphas[i] shl 24)
    }
  }

  private fun readAt(channel: FileChannel, position: Long, length: Int): ByteArray? {
    if (position < 0 || length < 0) {
      return null
    }
    val bytes = ByteArray(length)
    val buffer = ByteBuffer.wrap(bytes)
    while (buffer.hasRemaining()) {
      if (channel.read(buffer, position + buffer.position()) <= 0) {
        return null
      }
    }
    return bytes
  }

  private fun startsWith(buffer: ByteArray, offset: Int, prefix: ByteArray): Boolean {
    if (offset < 0 || buffer.size - offset < prefix.size) {
      return false
    }
    for (i in prefix.indices) {
      if (buffer[offset + i] != prefix[i]) {
        return false
      }
    }
    return true
  }

  private fun u16(buffer: ByteArray, offset: Int): Int =
    (buffer[offset].toInt() and 0xFF) or ((buffer[offset + 1].toInt() and 0xFF) shl 8)

  private fun u32(buffer: ByteArray, offset: Int): Long =
    u16(buffer, offset).toLong() or (u16(buffer, offset + 2).toLong() shl 16)
}

/**
 * What XbeMetadataReader found per disc image, kept on disk so each image is
 * read once: a TSV of name and title id by image, and the title images as
 * PNGs beside it. An image with nothing to read is remembered too.
 */
class XbeMetadataCache(context: Context) {

  class Entry(val titleId: Long, val titleName: String, val icon: File?)

  private val appContext = context.applicationContext
  private val indexFile = File(appContext.filesDir, "xbe_metadata_cache.tsv")
  private val iconsDir = File(appContext.filesDir, "xbe_icons")
  private val lock = Any()
  private val entries = HashMap<String, Entry?>()
  private var loaded = false
  private var dirty = false

  /** Null when the image has no original Xbox executable to read. */
  fun get(uri: Uri, sizeBytes: Long, seenKeys: MutableSet<String>): Entry? {
    val key = "$uri|$sizeBytes"
    seenKeys.add(key)
    synchronized(lock) {
      loadIfNeeded()
      if (entries.containsKey(key)) {
        return entries[key]
      }
    }

    val entry = XbeMetadataReader.read(appContext, uri)?.let { metadata ->
      val icon = metadata.icon?.let { bitmap -> saveIcon(key, bitmap) }
      Entry(metadata.titleId, metadata.titleName, icon)
    }
    synchronized(lock) {
      entries[key] = entry
      dirty = true
    }
    return entry
  }

  /** Every game the library has read, by title id, for naming saves on the HDD. */
  fun knownTitles(): Map<Long, Entry> {
    synchronized(lock) {
      loadIfNeeded()
      return entries.values.filterNotNull().associateBy { it.titleId }
    }
  }

  /** Forgets images no longer in the library and writes the index if it changed. */
  fun persist(seenKeys: Set<String>) {
    synchronized(lock) {
      if (!loaded) {
        return
      }
      val iterator = entries.entries.iterator()
      while (iterator.hasNext()) {
        val (key, entry) = iterator.next()
        if (key !in seenKeys) {
          entry?.icon?.delete()
          iterator.remove()
          dirty = true
        }
      }
      if (!dirty) {
        return
      }
      runCatching {
        indexFile.parentFile?.mkdirs()
        indexFile.bufferedWriter(Charsets.UTF_8).use { writer ->
          for ((key, entry) in entries) {
            writer.append(key).append('\t')
            if (entry != null) {
              writer.append(java.lang.Long.toHexString(entry.titleId)).append('\t')
              writer.append(entry.titleName.replace(Regex("[\\t\\r\\n]"), " ")).append('\t')
              writer.append(if (entry.icon != null) "1" else "0")
            }
            writer.append('\n')
          }
        }
        dirty = false
      }
    }
  }

  private fun loadIfNeeded() {
    if (loaded) {
      return
    }
    if (indexFile.isFile) {
      runCatching {
        indexFile.forEachLine(Charsets.UTF_8) { line ->
          val fields = line.split('\t')
          val key = fields[0]
          if (key.isEmpty()) {
            return@forEachLine
          }
          if (fields.size < 4) {
            entries[key] = null
            return@forEachLine
          }
          val titleId = fields[1].toLongOrNull(16) ?: return@forEachLine
          val icon = iconFile(key).takeIf { fields[3] == "1" && it.isFile }
          entries[key] = Entry(titleId, fields[2], icon)
        }
      }
    }
    loaded = true
  }

  private fun saveIcon(key: String, bitmap: Bitmap): File? {
    val file = iconFile(key)
    return runCatching {
      iconsDir.mkdirs()
      file.outputStream().use { bitmap.compress(Bitmap.CompressFormat.PNG, 100, it) }
      file
    }.getOrNull()
  }

  private fun iconFile(key: String): File {
    val digest = MessageDigest.getInstance("SHA-1").digest(key.toByteArray(Charsets.UTF_8))
    val name = digest.take(10).joinToString("") { "%02x".format(it) }
    return File(iconsDir, "$name.png")
  }
}
