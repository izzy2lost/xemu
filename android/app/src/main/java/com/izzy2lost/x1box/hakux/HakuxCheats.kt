/*
 * The cheats format and run manifest follow hakuX's mods/README.md and ModRun
 * (https://github.com/rfandango/hakuX, GPLv3 or later; see android/hakux/core/COPYING).
 */
package com.izzy2lost.x1box.hakux

import android.content.Context
import java.io.File
import java.util.Locale

/** A cheat: its name, the note shown under it, once (written once when turned on), its code lines. */
data class HakuxCheat(val name: String, val note: String, val once: Boolean, val code: List<String>)

/**
 * A game's cheats, kept per disc release (its title ID and certificate version,
 * 4D530004.9: a cheat's addresses hold for one release): files/hakux/cheats/<key>.txt
 * in hakuX's cheats.txt format, so a file from hakuX or written by hand imports as
 * is, and <key>.on, the names of those turned on, one a line.
 */
class HakuxCheatStore(context: Context, gameKey: String) {
  private val dir = File(context.filesDir, "hakux/cheats")
  private val file = File(dir, "$gameKey.txt")
  private val onFile = File(dir, "$gameKey.on")

  fun load(): List<HakuxCheat> = if (file.isFile) parse(file.readText()) else emptyList()

  fun save(cheats: List<HakuxCheat>) {
    dir.mkdirs()
    file.writeText(format(cheats))
    setEnabled(enabled() intersect cheats.map { it.name }.toSet())
  }

  fun enabled(): Set<String> =
    if (onFile.isFile) onFile.readLines().filter { it.isNotEmpty() }.toSet() else emptySet()

  fun setEnabled(name: String, on: Boolean) = setEnabled(if (on) enabled() + name else enabled() - name)

  private fun setEnabled(names: Set<String>) {
    dir.mkdirs()
    onFile.writeText(names.joinToString("") { "$it\n" })
  }

  /**
   * The run's manifest for the core (hakux/core/native/core/mods.h): every cheat with
   * whether it is on. Null when the game has none. The core numbers the cheats in this
   * order (nativeSetCheat's index).
   */
  fun writeManifest(target: File): List<HakuxCheat>? {
    val cheats = load().takeIf { it.isNotEmpty() } ?: return null
    val on = enabled()
    val lines = mutableListOf<String>()
    for (c in cheats) {
      lines += "cheat\t${if (c.name in on) 1 else 0}\t${if (c.once) 1 else 0}\t${c.name.tabless()}"
      for (l in c.code) lines += "code\t${l.tabless()}"
    }
    target.parentFile?.mkdirs()
    target.writeText(lines.joinToString("\n", postfix = "\n"))
    return cheats
  }

  companion object {
    private fun String.tabless() = replace('\t', ' ')

    private fun content(line: String) = line.trim().takeUnless { it.isEmpty() || it.startsWith("#") }

    /** hakuX's cheats.txt: "[name]", then "note: ...", "once" and code lines, comments with #. */
    fun parse(text: String): List<HakuxCheat> {
      val out = mutableListOf<HakuxCheat>()
      var name: String? = null
      var note = ""
      var once = false
      val code = mutableListOf<String>()
      fun flush() {
        name?.let { out += HakuxCheat(it, note, once, code.toList()) }
        note = ""
        once = false
        code.clear()
      }
      for (raw in text.lines()) {
        val line = content(raw) ?: continue
        if (line.startsWith("[") && line.endsWith("]")) {
          flush()
          name = line.substring(1, line.length - 1).trim()
          continue
        }
        if (name == null) continue
        when {
          line.lowercase(Locale.ROOT).startsWith("note:") -> note = line.substringAfter(':').trim()
          line.equals("once", ignoreCase = true) -> once = true
          else -> code += line
        }
      }
      flush()
      return out
    }

    fun format(cheats: List<HakuxCheat>): String = buildString {
      for (c in cheats) {
        append('[').append(c.name).append("]\n")
        if (c.note.isNotEmpty()) append("note: ").append(c.note).append('\n')
        if (c.once) append("once\n")
        for (l in c.code) append(l).append('\n')
        append('\n')
      }
    }
  }
}
