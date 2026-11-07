package com.izzy2lost.x1box.hakux

import android.util.Log
import java.io.File
import java.io.RandomAccessFile

/**
 * Re-signs a game's saves made under older keys to the keys the hakuX core signs with now
 * (xemu's console's: HakuxConsoleKeys), after upstream hakuX's SaveResigner. Before the core took
 * xemu's keys it signed as a console whose title keys are all zero, with an HD key of its own, so
 * the saves it made don't check under xemu, nor do xemu's under it.
 *
 * Games sign with XAPI's XCalculateSignature: roamable = HMAC-SHA1(title key, data), the title
 * key being HMAC-SHA1(certificate key, the certificate's signature key)[0:16] (zero before);
 * locked to the console = HMAC-SHA1(HD key, roamable). Downloaded content's ContentMeta.xbx:
 * HMAC-SHA1(HMAC-SHA1(HD key, title id), its header).
 *
 * A game puts the signature where it likes. The places XSavSig's resign.ini lists for most games
 * are tried (the signature at the start, at 4, or at the end, the end also before padding, over
 * the bytes around it, or in a .sig file of its own), and a signature is replaced only when it
 * checks with one of the old keys: a file whose signature isn't found is left as it is (already
 * xemu's, unsigned, or a layout not tried). A folder's signatures are all found before any is
 * written: a .sig file may sign itself too, over the others' signatures (Forza's CarIcons.sig),
 * and its own comes after theirs.
 */
class HakuxSaveResigner(
  /** The certificate keys the saves may have been signed under: null is the zero title keys. */
  private val fromCerts: List<ByteArray?>,
  /** The HD keys they may have been signed under. */
  private val fromHds: List<ByteArray>,
  /** What they are signed with from now on. */
  private val toCert: ByteArray?,
  private val toHd: ByteArray,
) {
  /**
   * The saves of these titles (UDATA and TDATA \<title id>\... under e, partition 1's folder)
   * re-signed; saveKeys: each title ID's raw signature key from its certificate
   * (XbeMetadataReader.Metadata.saveKeys). The number of files re-signed.
   */
  fun resign(e: File, saveKeys: Map<Long, ByteArray>): Int {
    var done = 0
    for ((tid, raw) in saveKeys) {
      val name = "%08x".format(tid)
      val dirs = AREAS.flatMap { area ->
        e.listFiles().orEmpty().filter { it.isDirectory && it.name.equals(area, ignoreCase = true) }
          .flatMap { a -> a.listFiles().orEmpty().filter { it.isDirectory && it.name.equals(name, ignoreCase = true) } }
      }
      val files = dirs.flatMap { d -> d.walkTopDown().filter(File::isFile).toList() }
      if (files.isNotEmpty()) done += resignTitle(files, raw)
    }
    return done
  }

  private fun auth(cert: ByteArray?, raw: ByteArray): ByteArray =
    cert?.let { XCrypto.hmac(it, raw).copyOf(16) } ?: ZERO

  private fun resignTitle(files: List<File>, raw: ByteArray): Int {
    val toAuth = auth(toCert, raw)
    val fromAuths = fromCerts.map { auth(it, raw) }.distinctBy { it.toList() }
    var done = 0
    val found = ArrayList<Pair<File, Hit>>()
    // a file of 20 bytes or less is no signed data (a .sig file's signature is found from its data's)
    for (f in files.filter { it.length() in 21L..MAX_FILE && !isMeta(it.name) }) {
      val d = runCatching { f.readBytes() }.getOrNull() ?: continue
      if (f.name.equals(CONTENT_META, ignoreCase = true)) {
        if (content(f, d)) done++
        continue
      }
      val siblings = f.parentFile!!.listFiles().orEmpty().filter { it.isFile && it != f }
      find(d, siblings, fromAuths)?.let { found += f to it }
    }
    // a file holding others' signatures signs itself last, over theirs
    val holders = found.mapNotNull { it.second.file }.toSet()
    for ((f, hit) in found.sortedBy { (f, hit) -> hit.file == null && f in holders }) {
      val d = f.readBytes()
      var sig = XCrypto.hmac(toAuth, d, hit.a, hit.b - hit.a)
      if (hit.noroam) sig = XCrypto.hmac(toHd, sig)
      if (write(hit.file ?: f, hit.off, sig)) done++
    }
    return done
  }

  /** A signature found: the data it covers [a, b), its kind and place (file null: this one). */
  private class Hit(val a: Int, val b: Int, val noroam: Boolean, val file: File?, val off: Long)

  /**
   * Where the file's signature is, when one of the old keys signed it: its data range, kind and
   * place, or null. The places are tried from the file's end, then from its content's end,
   * before padding (zeros or spaces): a game may write its data and the signature after it into
   * a file of a fixed size (Fable's 4 KiB Profile.bin).
   */
  private fun find(d: ByteArray, siblings: List<File>, auths: List<ByteArray>): Hit? {
    val n = d.size
    val small by lazy {
      siblings.filter { it.length() in 20..4096 }.mapNotNull { f -> runCatching { f to f.readBytes() }.getOrNull() }
    }
    for (auth in auths) {
      for (m in listOf(n, unpadded(d, 0), unpadded(d, 0x20)).filter { it > 20 }.distinct()) {
        val ends = listOf(m - 24, m - 20, m - 4, m).filter { it > 0 }.distinct()
        for (a in listOf(0, 4, 20, 24)) {
          val h = XCrypto.Hmac(auth)
          var at = a
          for (b in ends) {
            if (b <= a) continue
            h.update(d, at, b - at)
            at = b
            val roam = h.copy().finish()
            val sigs = listOf(false to roam) + fromHds.map { true to XCrypto.hmac(it, roam) }
            for (p in listOf(0, 4, m - 24, m - 20)) {
              if (p < 0 || p + 20 > m || (p + 20 > a && p < b)) continue
              for ((noroam, s) in sigs) if (matches(d, p, s)) return Hit(a, b, noroam, null, p.toLong())
            }
            if (a == 0 && b == n) {
              for ((file, bytes) in small) for ((noroam, s) in sigs) {
                val q = indexOf(bytes, s)
                if (q >= 0) return Hit(a, b, noroam, file, q.toLong())
              }
            }
          }
        }
      }
    }
    return null
  }

  /** d's length without the trailing run of pad bytes. */
  private fun unpadded(d: ByteArray, pad: Int): Int {
    var m = d.size
    while (m > 0 && d[m - 1].toInt() == pad) m--
    return m
  }

  /** ContentMeta.xbx: its header [0x14, header size) signed at 0 with HMAC(HD key, title id at 0x24). */
  private fun content(f: File, d: ByteArray): Boolean {
    if (d.size < 0x28) return false
    val size = le32(d, 0x18)
    if (size <= 0x14 || size > d.size) return false
    val tid = d.copyOfRange(0x24, 0x28)
    if (fromHds.none { matches(d, 0, XCrypto.hmac(XCrypto.hmac(it, tid), d, 0x14, size - 0x14)) }) return false
    return write(f, 0, XCrypto.hmac(XCrypto.hmac(toHd, tid), d, 0x14, size - 0x14))
  }

  /**
   * A signature written at off of f, when it isn't that already. Its date changes, so the
   * hard disk sync takes it back into xemu's image (HakuxHddSync compares size and date).
   */
  private fun write(f: File, off: Long, sig: ByteArray): Boolean {
    val old = ByteArray(20)
    RandomAccessFile(f, "rw").use { r ->
      r.seek(off)
      r.readFully(old)
      if (old.contentEquals(sig)) return false
      r.seek(off)
      r.write(sig)
    }
    f.setLastModified(System.currentTimeMillis())
    return true
  }

  private fun matches(d: ByteArray, p: Int, s: ByteArray): Boolean {
    for (i in 0 until 20) if (d[p + i] != s[i]) return false
    return true
  }

  private fun indexOf(hay: ByteArray, s: ByteArray): Int {
    outer@ for (i in 0..hay.size - s.size) {
      for (j in s.indices) if (hay[i + j] != s[j]) continue@outer
      return i
    }
    return -1
  }

  private fun le32(d: ByteArray, o: Int) =
    (d[o].toInt() and 0xFF) or ((d[o + 1].toInt() and 0xFF) shl 8) or ((d[o + 2].toInt() and 0xFF) shl 16) or
      ((d[o + 3].toInt() and 0xFF) shl 24)

  companion object {
    private const val TAG = "x1box-hakux-resign"
    private const val CONTENT_META = "ContentMeta.xbx"
    private const val MAX_FILE = 128L shl 20
    private val AREAS = listOf("UDATA", "TDATA")
    private val ZERO = ByteArray(16)
    private val META = setOf("titlemeta.xbx", "titleimage.xbx", "savemeta.xbx", "saveimage.xbx")
    /** What the core's EEPROM had as its HD key before it took xemu's (ex.c eeprom()). */
    private const val NATIVE_HD = ".x1box-native-hd"

    private fun isMeta(name: String) = name.lowercase() in META

    /**
     * The HD keys the core may have signed saves with before it took xemu's: the one in its
     * EEPROM's (clear) encrypted section at 0x1C, kept aside the first time (the core then
     * overwrites it); its bytes 0x36-0x45, which it used before keeping one at 0x1C; the zero
     * key (older XAPI with a zero HD key); and, before its first run, the identity every console
     * had (serial 000000000001, MAC 00:50:F2:00:00:01).
     */
    fun nativeHdKeys(hdd: File): List<ByteArray> {
      val kept = File(hdd, NATIVE_HD)
      val eeprom = runCatching { File(hdd, "eeprom.bin").takeIf { it.length() >= 0x46 }?.readBytes() }.getOrNull()
      val out = ArrayList<ByteArray>()
      runCatching { kept.takeIf { it.length() == 16L }?.readBytes() }.getOrNull()?.let(out::add)
      if (eeprom != null) {
        val at1c = eeprom.copyOfRange(0x1C, 0x2C)
        if (out.isEmpty() && !at1c.contentEquals(ZERO)) {
          runCatching { kept.writeBytes(at1c) }
          out += at1c
        }
        out += eeprom.copyOfRange(0x36, 0x46)
      }
      out += "0000000001".toByteArray() + byteArrayOf(0x00, 0x50, 0xF2.toByte(), 0x00, 0x00, 0x01)
      out += ZERO
      return out.distinctBy { it.toList() }
    }

    /**
     * Before a hakuX run of a game: its saves made under the core's old keys re-signed to
     * xemu's. Nothing when xemu's keys aren't known (the core keeps signing as before).
     */
    fun beforeRun(hdd: File, keys: HakuxConsoleKeys.Keys, saveKeys: Map<Long, ByteArray>) {
      if ((keys.cert == null && keys.hd == null) || saveKeys.isEmpty()) return
      val natives = nativeHdKeys(hdd)
      val toHd = keys.hd ?: natives.first()
      val fromHds = natives.filterNot { it.contentEquals(toHd) }
      val fromCerts = if (keys.cert != null) listOf(null, keys.cert) else listOf(null)
      val t0 = System.nanoTime()
      val n = runCatching {
        HakuxSaveResigner(fromCerts, fromHds, keys.cert, toHd).resign(File(hdd, "Partition1"), saveKeys)
      }.onFailure { Log.w(TAG, "re-signing failed", it) }.getOrDefault(0)
      Log.i(TAG, "${saveKeys.keys.joinToString { "%08X".format(it) }}: $n re-signed to xemu's keys, " +
        "${(System.nanoTime() - t0) / 1_000_000} ms")
    }
  }
}
