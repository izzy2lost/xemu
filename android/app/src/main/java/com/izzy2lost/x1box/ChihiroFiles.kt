package com.izzy2lost.x1box

import android.content.Context
import android.net.Uri
import android.provider.OpenableColumns
import java.io.File
import java.util.Locale

/**
 * The files the xemu core needs for a Chihiro game, beside its own core files
 * in x1box/ (where SyncSetupFiles looks for them): the Chihiro BIOS, which it
 * boots in place of the Xbox one, and the media board flash ROM (SEGABOOT),
 * which hw/xbox/chihiro.c looks for by name next to the BIOS. The hakuX core
 * needs neither.
 */
object ChihiroFiles {
  /** The name xemu_android.cpp boots a Chihiro game's BIOS from. */
  const val BIOS_NAME = "chihiro_bios.bin"

  /* Chihiro BIOS dumps are 512 KiB (1 MiB with a mirror); 256 KiB as for the Xbox. */
  private val BIOS_SIZES = setOf(262144L, 524288L, 1048576L)

  /** The names chihiro.c looks for, the first the one a renamed dump lands under. */
  private val MB_ROM_NAMES = listOf(
    "fpr21042_m29w160et.bin",
    "fpr-23887_29lv160te.ic4",
    "fpr-23887.bin",
  )

  /* chihiro_load_flash_rom() rejects anything over 4 MiB. */
  private const val MB_ROM_MAX = 4L * 1024 * 1024

  fun dir(context: Context): File = File(context.getExternalFilesDir(null) ?: context.filesDir, "x1box")

  fun installedBios(context: Context): File? =
    File(dir(context), BIOS_NAME).takeIf { it.isFile && it.length() in BIOS_SIZES }

  fun installedMbRom(context: Context): File? =
    MB_ROM_NAMES.map { File(dir(context), it) }.firstOrNull { it.isFile && it.length() > 0L }

  /** What a Chihiro game in the xemu core still lacks: string resources, in order. */
  fun missingForXemu(context: Context): List<Int> = listOfNotNull(
    R.string.chihiro_missing_bios.takeIf { installedBios(context) == null },
    R.string.chihiro_missing_mbrom.takeIf { installedMbRom(context) == null },
  )

  /** Copies a Chihiro BIOS in; false when it is not one (by size). */
  fun installBios(context: Context, uri: Uri): Boolean =
    copyIn(context, uri, BIOS_NAME) { it in BIOS_SIZES }

  /** Copies a media board ROM in, under the name chihiro.c looks for; false when it is not one. */
  fun installMbRom(context: Context, uri: Uri): Boolean {
    val source = displayName(context, uri)?.lowercase(Locale.ROOT)
    val name = MB_ROM_NAMES.firstOrNull { it.lowercase(Locale.ROOT) == source } ?: MB_ROM_NAMES.first()
    return copyIn(context, uri, name) { it in 1..MB_ROM_MAX }
  }

  private fun copyIn(context: Context, uri: Uri, name: String, sizeOk: (Long) -> Boolean): Boolean {
    val dir = dir(context)
    if (!dir.isDirectory && !dir.mkdirs()) return false
    val target = File(dir, name)
    val tmp = File(dir, "$name.tmp")
    val ok = runCatching {
      context.contentResolver.openInputStream(uri)?.use { input ->
        tmp.outputStream().use { input.copyTo(it) }
      } != null && sizeOk(tmp.length()) && tmp.renameTo(target)
    }.getOrDefault(false)
    tmp.delete()
    return ok
  }

  private fun displayName(context: Context, uri: Uri): String? = runCatching {
    context.contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)?.use {
      if (it.moveToFirst()) it.getString(0) else null
    }
  }.getOrNull() ?: uri.lastPathSegment?.substringAfterLast('/')
}
