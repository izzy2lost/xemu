/*
 * Adapted from hakuX's ModCatalog and ModRun (https://github.com/rfandango/hakuX,
 * GPLv3 or later; see android/hakux/core/COPYING).
 */
package com.izzy2lost.x1box.hakux

import android.content.Context
import android.content.res.AssetManager
import android.util.Log
import java.io.File
import java.util.Locale

/**
 * A mod (android/hakux/mods/README.md): its folder name, what it says about itself,
 * its XBE patches (file, line) and the vblank rate it asks for (0: the video mode's).
 */
data class HakuxMod(
  val id: String,
  val name: String,
  val description: String,
  val patches: List<Pair<String, String>>,
  val refresh: Int = 0,
)

/**
 * hakuX's mods, packed as the APK's assets/mods (android/hakux/mods, the Gradle task
 * syncHakuxMods): a folder per disc release (4D530004.9), a folder per mod in it with
 * its mod.txt and, maybe, files/ that replace or add disc files.
 */
object HakuxModCatalog {
  private const val ROOT = "mods"
  private const val TAG = "x1box-hakux-mods"
  /** Game key (upper case) -> its folder's name as packed. */
  @Volatile private var folders: Map<String, String>? = null

  private fun folders(context: Context): Map<String, String> = folders ?: run {
    val map = context.assets.list(ROOT).orEmpty().associateBy { it.uppercase(Locale.ROOT) }
    folders = map
    map
  }

  private fun folder(context: Context, gameKey: String) = folders(context)[gameKey.uppercase(Locale.ROOT)]

  /** The disc's mods, by folder name. */
  fun load(context: Context, gameKey: String): List<HakuxMod> {
    val dir = folder(context, gameKey) ?: return emptyList()
    val am = context.assets
    return am.list("$ROOT/$dir").orEmpty().sorted().mapNotNull { id ->
      text(am, "$ROOT/$dir/$id/mod.txt")?.let { parse(id, it) }
    }
  }

  private fun text(am: AssetManager, path: String): String? =
    runCatching { am.open(path).use { it.readBytes().toString(Charsets.UTF_8) } }.getOrNull()

  private fun parse(id: String, text: String): HakuxMod {
    val fields = mutableMapOf<String, String>()
    val patches = mutableListOf<Pair<String, String>>()
    var xbe: String? = null
    for (raw in text.lines()) {
      val line = raw.trim().takeUnless { it.isEmpty() || it.startsWith("#") } ?: continue
      if (line.startsWith("[") && line.endsWith("]")) {
        xbe = line.substring(1, line.length - 1).trim()
        continue
      }
      val x = xbe
      if (x != null) {
        patches += x to line
        continue
      }
      val colon = line.indexOf(':')
      if (colon > 0) {
        val key = line.substring(0, colon).trim().lowercase(Locale.ROOT)
        if (key.all { it.isLetter() }) fields[key] = line.substring(colon + 1).trim()
      }
    }
    return HakuxMod(
      id = id,
      name = fields["name"]?.takeIf { it.isNotEmpty() } ?: id,
      description = fields["description"].orEmpty(),
      patches = patches,
      refresh = fields["refresh"]?.toIntOrNull() ?: 0,
    )
  }

  /**
   * A mod's files (assets mods/<game>/<mod>/files/...) in the app's storage for the core
   * to read, extracted again after the app changed: their disc paths and host files.
   */
  fun extract(context: Context, gameKey: String, mod: String): List<Pair<String, File>> {
    val dir = folder(context, gameKey) ?: return emptyList()
    val am = context.assets
    val src = "$ROOT/$dir/$mod/files"
    val dst = File(context.filesDir, "hakux/mods/${gameKey.uppercase(Locale.ROOT)}/$mod")
    val files = File(dst, "files")
    val stamp = File(dst, ".stamp")
    val app = context.packageManager.getPackageInfo(context.packageName, 0).lastUpdateTime.toString()
    if (stamp.isFile && stamp.readText() == app) {
      return files.walkTopDown().filter { it.isFile }.map { it.relativeTo(files).path to it }.toList()
    }
    dst.deleteRecursively()
    val out = mutableListOf<Pair<String, File>>()
    fun copy(asset: String, rel: String) {
      val children = am.list(asset).orEmpty()
      if (children.isEmpty()) {
        val target = File(files, rel)
        target.parentFile?.mkdirs()
        runCatching { am.open(asset).use { i -> target.outputStream().use { i.copyTo(it) } } }
          .onSuccess { out += rel to target }
          .onFailure { Log.w(TAG, "$asset: ${it.message}") }
        return
      }
      for (c in children) copy("$asset/$c", if (rel.isEmpty()) c else "$rel/$c")
    }
    if (am.list(src).orEmpty().isNotEmpty()) copy(src, "")
    dst.mkdirs()
    stamp.writeText(app)
    return out
  }
}

/** Which of a game's mods are on: files/hakux/mods/<game key>.on, a mod's folder name a line. */
class HakuxModChoices(context: Context, gameKey: String) {
  private val file = File(context.filesDir, "hakux/mods/$gameKey.on")

  fun enabled(): Set<String> =
    if (file.isFile) file.readLines().filter { it.isNotEmpty() }.toSet() else emptySet()

  fun set(id: String, on: Boolean) {
    val names = if (on) enabled() + id else enabled() - id
    file.parentFile?.mkdirs()
    file.writeText(names.joinToString("") { "$it\n" })
  }
}

/**
 * The run's manifest for the core (hakux/core/native/core/mods.h): the mods that are on
 * (their files, XBE patches and vblank rate), every mod's patches as "patchoff" when off
 * (a savestate saved with it on gets the disc's bytes back), then every cheat with
 * whether it is on, numbered in this order (nativeSetCheat's index).
 */
object HakuxModRun {
  private const val TAG = "x1box-hakux-mods"

  /** The manifest written, and the run's cheats in its order; null when the game has neither. */
  fun write(context: Context, gameKey: String, target: File): List<HakuxCheat>? {
    val mods = HakuxModCatalog.load(context, gameKey)
    val cheatStore = HakuxCheatStore(context, gameKey)
    val cheats = cheatStore.load()
    if (mods.isEmpty() && cheats.isEmpty()) return null
    val lines = mutableListOf<String>()
    val modsOn = HakuxModChoices(context, gameKey).enabled()
    val on = mods.filter { it.id in modsOn }
    for (mod in on) {
      for ((disc, file) in HakuxModCatalog.extract(context, gameKey, mod.id)) {
        lines += "file\t${disc.replace('/', '\\')}\t${file.absolutePath}"
      }
    }
    for (mod in mods) {
      val kind = if (mod in on) "patch" else "patchoff"
      for ((xbe, line) in mod.patches) lines += "$kind\t$xbe\t${mod.id}\t$line"
    }
    on.maxOfOrNull { it.refresh }?.takeIf { it > 0 }?.let { lines += "refresh\t$it" }
    lines += cheatStore.manifestLines(cheats)
    target.parentFile?.mkdirs()
    target.writeText(lines.joinToString("\n", postfix = "\n"))
    Log.i(TAG, "$gameKey: ${on.size} of ${mods.size} mods on, ${cheats.size} cheats")
    return cheats
  }
}
