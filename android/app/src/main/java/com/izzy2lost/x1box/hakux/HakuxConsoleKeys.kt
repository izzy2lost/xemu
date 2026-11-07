package com.izzy2lost.x1box.hakux

import android.content.Context
import android.net.Uri
import java.io.File
import java.security.MessageDigest

/**
 * The keys xemu's console signs saves with, so the hakuX core signs as it does (one hard disk
 * for both cores, HakuxHddSync). Read from the user's own dumps at each launch, never kept
 * (after upstream hakuX's ConsoleKeys):
 *
 * - The certificate key, from which each title's save key comes: the BIOS's 2BL holds the
 *   EEPROM, certificate and kernel keys in a row; the MCPX 1.0 boot ROM decrypts it (RC4, the key
 *   at 0x1A5; the 2BL is the 24 KiB below the flash's last 512 bytes). The EEPROM key is told by
 *   its HMAC-SHA1 midstates, the certificate key is the 16 bytes after it.
 * - The HD key, for saves locked to the console: xemu's EEPROM's encrypted section (hash,
 *   confounder, HD key, region) is RC4 with HMAC(EEPROM key, hash) and checks by its hash.
 */
object HakuxConsoleKeys {
  class Keys(
    /** The BIOS's certificate key; null without MCPX and flash dumps it can be read from. */
    val cert: ByteArray?,
    /** xemu's EEPROM's HD key; null before xemu has made its EEPROM. */
    val hd: ByteArray?,
  ) {
    /** The core's launch argument (native/app/app_main.c keys=), null when neither is known. */
    fun arg(): String? =
      if (cert == null && hd == null) null else "keys=${cert?.let(::hex).orEmpty()}|${hd?.let(::hex).orEmpty()}"
  }

  fun load(context: Context): Keys {
    val prefs = context.getSharedPreferences("x1box_prefs", Context.MODE_PRIVATE)
    fun dump(pathKey: String, uriKey: String, max: Int): ByteArray? {
      prefs.getString(pathKey, null)?.let(::File)?.takeIf { it.isFile && it.length() <= max }?.let {
        return runCatching { it.readBytes() }.getOrNull()
      }
      val uri = prefs.getString(uriKey, null)?.let(Uri::parse) ?: return null
      return runCatching {
        context.contentResolver.openInputStream(uri)?.use { s ->
          val out = java.io.ByteArrayOutputStream()
          val buf = ByteArray(64 shl 10)
          while (out.size() <= max) {
            val n = s.read(buf)
            if (n < 0) break
            out.write(buf, 0, n)
          }
          out.toByteArray().takeIf { it.size <= max }
        }
      }.getOrNull()
    }
    val cert = run {
      val mcpx = dump("mcpxPath", "mcpxUri", MCPX_SIZE) ?: return@run null
      val flash = dump("flashPath", "flashUri", 16 shl 20) ?: return@run null
      certKey(mcpx, flash)
    }
    val hd = xemuEeprom(context)?.let { e -> runCatching { e.readBytes() }.getOrNull() }?.let(::hdKey)
    return Keys(cert, hd)
  }

  /** Where xemu keeps its EEPROM (cpp/xemu_android.cpp SyncSetupFiles: external storage first). */
  private fun xemuEeprom(context: Context): File? =
    listOfNotNull(context.getExternalFilesDir(null), context.filesDir)
      .map { File(File(it, "x1box"), "eeprom.bin") }
      .firstOrNull { it.isFile && it.length() >= 0x30 }

  /** An EEPROM key's HMAC-SHA1 midstates (inner, outer). */
  private class Midstates(val inner: IntArray, val outer: IntArray)

  /** The retail and debug kernels' EEPROM keys, as xemu's hw/xbox/eeprom_generation.c gives them. */
  private val EEPROM_KEYS = listOf(
    Midstates( /* debug */
      intArrayOf(0x85F9E51A.toInt(), 0xE04613D2.toInt(), 0x6D86A50C, 0x77C32E3C, 0x4BD717A4),
      intArrayOf(0x5D7A9C6B, 0xE1922BEB.toInt(), 0xB82CCDBC.toInt(), 0x3137AB34, 0x486B52B3)),
    Midstates( /* 1.0 */
      intArrayOf(0x72127625, 0x336472B9, 0xBE609BEA.toInt(), 0xF55E226B.toInt(), 0x99958DAC.toInt()),
      intArrayOf(0x76441D41, 0x4DE82659, 0x2E8EF85E, 0xB256FACA.toInt(), 0xC4FE2DE8.toInt())),
    Midstates( /* 1.1-1.4 */
      intArrayOf(0x39B06E79, 0xC9BD25E8.toInt(), 0xDBC6B498.toInt(), 0x40B4389D, 0x86BBD7ED.toInt()),
      intArrayOf(0x9B49BED3.toInt(), 0x84B430FC.toInt(), 0x6B8749CD, 0xEBFE5FE5.toInt(), 0xD96E7393.toInt())),
    Midstates( /* 1.6 */
      intArrayOf(0x8058763A.toInt(), 0xF97D4E0E.toInt(), 0x865A9762.toInt(), 0x8A3D920D.toInt(), 0x08995B2C),
      intArrayOf(0x01075307, 0xA2F1E037.toInt(), 0x1186EEEA, 0x88DA9992.toInt(), 0x168A5609)),
  )

  private const val MCPX_SIZE = 512
  private const val MCPX_RC4 = 0x1A5
  private const val BL_SIZE = 0x6000
  private const val BL_END = 0x200

  /** The certificate key in a BIOS for the MCPX 1.0 boot ROM, or null (another ROM, not a BIOS). */
  fun certKey(mcpx: ByteArray, flash: ByteArray): ByteArray? {
    if (mcpx.size != MCPX_SIZE || flash.size < 0x40000) return null
    val bl = XCrypto.rc4(mcpx.copyOfRange(MCPX_RC4, MCPX_RC4 + 16), flash, flash.size - BL_END - BL_SIZE, BL_SIZE)
    for (off in 0..bl.size - 48) {
      val mid = XCrypto.innerMid(bl, off, 16)
      if (EEPROM_KEYS.any { it.inner.contentEquals(mid) }) return bl.copyOfRange(off + 16, off + 32)
    }
    return null
  }

  /** The HD key in an EEPROM dump, or null when its encrypted section checks with none of the keys. */
  fun hdKey(eeprom: ByteArray): ByteArray? {
    if (eeprom.size < 0x30) return null
    val hash = eeprom.copyOf(20)
    for (k in EEPROM_KEYS) {
      val plain = XCrypto.rc4(XCrypto.hmacMid(k.inner, k.outer, hash), eeprom, 20, 0x1C)
      if (XCrypto.hmacMid(k.inner, k.outer, plain).contentEquals(hash)) return plain.copyOfRange(8, 24)
    }
    return null
  }

  fun hex(b: ByteArray): String = b.joinToString("") { "%02x".format(it.toInt() and 0xFF) }
}

/** The Xbox's crypto as saves and dumps need it: HMAC-SHA1 (also from SHA-1 midstates), RC4. */
internal object XCrypto {
  private val SHA1_IV = intArrayOf(0x67452301, 0xEFCDAB89.toInt(), 0x98BADCFE.toInt(), 0x10325476, 0xC3D2E1F0.toInt())

  fun hmac(key: ByteArray, data: ByteArray, off: Int = 0, len: Int = data.size - off): ByteArray =
    Hmac(key).apply { update(data, off, len) }.finish()

  /** HMAC-SHA1, incremental: copy() forks it, for messages that share a start. */
  class Hmac private constructor(private val inner: MessageDigest, private val outerKey: ByteArray) {
    constructor(key: ByteArray) : this(MessageDigest.getInstance("SHA-1"), pad(key, 0x5C)) {
      inner.update(pad(key, 0x36))
    }

    fun update(b: ByteArray, off: Int, len: Int) {
      if (len > 0) inner.update(b, off, len)
    }

    fun copy() = Hmac(inner.clone() as MessageDigest, outerKey)

    fun finish(): ByteArray {
      val i = inner.digest()
      return MessageDigest.getInstance("SHA-1").run {
        update(outerKey)
        update(i)
        digest()
      }
    }

    private companion object {
      fun pad(key: ByteArray, x: Int): ByteArray {
        val k = if (key.size > 64) MessageDigest.getInstance("SHA-1").digest(key) else key
        return ByteArray(64) { ((if (it < k.size) k[it].toInt() else 0) xor x).toByte() }
      }
    }
  }

  fun rc4(key: ByteArray, data: ByteArray, off: Int = 0, len: Int = data.size - off): ByteArray {
    val s = IntArray(256) { it }
    var j = 0
    for (i in 0 until 256) {
      j = (j + s[i] + (key[i % key.size].toInt() and 0xFF)) and 0xFF
      val t = s[i]; s[i] = s[j]; s[j] = t
    }
    val out = ByteArray(len)
    var i = 0
    j = 0
    for (n in 0 until len) {
      i = (i + 1) and 0xFF
      j = (j + s[i]) and 0xFF
      val t = s[i]; s[i] = s[j]; s[j] = t
      out[n] = (data[off + n].toInt() xor s[(s[i] + s[j]) and 0xFF]).toByte()
    }
    return out
  }

  /** SHA-1's compression of the 64-byte block at off, from state h. */
  private fun compress(h: IntArray, block: ByteArray, off: Int = 0): IntArray {
    val w = IntArray(80)
    for (t in 0 until 16) {
      val p = off + t * 4
      w[t] = ((block[p].toInt() and 0xFF) shl 24) or ((block[p + 1].toInt() and 0xFF) shl 16) or
        ((block[p + 2].toInt() and 0xFF) shl 8) or (block[p + 3].toInt() and 0xFF)
    }
    for (t in 16 until 80) w[t] = Integer.rotateLeft(w[t - 3] xor w[t - 8] xor w[t - 14] xor w[t - 16], 1)
    var a = h[0]; var b = h[1]; var c = h[2]; var d = h[3]; var e = h[4]
    for (t in 0 until 80) {
      val f: Int
      val k: Int
      when {
        t < 20 -> { f = (b and c) or (b.inv() and d); k = 0x5A827999 }
        t < 40 -> { f = b xor c xor d; k = 0x6ED9EBA1 }
        t < 60 -> { f = (b and c) or (b and d) or (c and d); k = 0x8F1BBCDC.toInt() }
        else -> { f = b xor c xor d; k = 0xCA62C1D6.toInt() }
      }
      val tmp = Integer.rotateLeft(a, 5) + f + e + k + w[t]
      e = d; d = c; c = Integer.rotateLeft(b, 30); b = a; a = tmp
    }
    return intArrayOf(h[0] + a, h[1] + b, h[2] + c, h[3] + d, h[4] + e)
  }

  /** SHA-1 of msg after one block already hashed into mid (an HMAC key's pad). */
  private fun sha1From(mid: IntArray, msg: ByteArray): ByteArray {
    val total = 64L + msg.size
    val padded = ByteArray(((msg.size + 9 + 63) / 64) * 64)
    msg.copyInto(padded)
    padded[msg.size] = 0x80.toByte()
    val bits = total * 8
    for (i in 0 until 8) padded[padded.size - 1 - i] = (bits ushr (8 * i)).toByte()
    var h = mid
    for (o in padded.indices step 64) h = compress(h, padded, o)
    return ByteArray(20) { (h[it / 4] ushr (24 - 8 * (it % 4))).toByte() }
  }

  /** HMAC-SHA1 from the key's inner and outer midstates. */
  fun hmacMid(inner: IntArray, outer: IntArray, msg: ByteArray) = sha1From(outer, sha1From(inner, msg))

  /** A key's HMAC inner midstate: SHA-1's compression of its ipad block. */
  fun innerMid(key: ByteArray, off: Int = 0, len: Int = key.size): IntArray =
    compress(SHA1_IV, ByteArray(64) { ((if (it < len) key[off + it].toInt() else 0) xor 0x36).toByte() })
}
