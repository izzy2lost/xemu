package com.izzy2lost.x1box

import android.app.Activity
import android.content.Context
import android.content.Intent
import android.content.SharedPreferences
import android.net.Uri
import com.izzy2lost.x1box.hakux.HakuxCoreActivity
import java.io.File

/**
 * Which core runs a game: xemu (the whole console under QEMU, :xemu) or the
 * hakuX core (an HLE Xbox kernel on FEX with an HLE GPU, android/hakux, in
 * :hakux). The global choice is PREF_CORE, the hakuX core unless set; a game's
 * own override comes in as its runtime override, like every other per-game setting.
 */
object HakuxLaunch {
  const val PREF_CORE = "setting_core"
  const val CORE_XEMU = "xemu"
  const val CORE_HAKUX = "hakux"
  /** The library path (per-game settings key) of the game last launched, beside its dvdUri/dvdPath. */
  const val PREF_LAST_GAME = "last_game_relative_path"

  /** The global core choice, before any game's override. */
  fun globalCore(prefs: SharedPreferences): String =
    prefs.getString(PREF_CORE, CORE_HAKUX) ?: CORE_HAKUX

  /** The core for the game whose runtime overrides were just applied (startGame). */
  fun selectedCore(context: Context): String {
    val prefs = context.getSharedPreferences("x1box_prefs", Context.MODE_PRIVATE)
    return PerGameSettingsManager.getRuntimeOverride(context, PREF_CORE)
      ?: globalCore(prefs)
  }

  /**
   * Whether xemu's MCPX boot ROM, flash ROM and hard disk are all in place. Only
   * xemu boots them: the hakuX core has its own kernel and keeps its saves in a
   * folder of its own when there is no hard disk image (HakuxHddSync).
   */
  fun xemuFilesReady(context: Context): Boolean {
    val prefs = context.getSharedPreferences("x1box_prefs", Context.MODE_PRIVATE)
    val granted = context.contentResolver.persistedUriPermissions
      .filter { it.isReadPermission }
      .map { it.uri }
    fun ready(pathKey: String, uriKey: String): Boolean =
      prefs.getString(pathKey, null)?.let { File(it).isFile } == true ||
        prefs.getString(uriKey, null)?.let { Uri.parse(it) in granted } == true
    return ready("mcpxPath", "mcpxUri") && ready("flashPath", "flashUri") && ready("hddPath", "hddUri")
  }

  /** The URI the hakuX core opens a disc by: a local path as a file:// URI. */
  fun discUri(dvdUri: String?, dvdPath: String?): String? =
    dvdUri ?: dvdPath?.let { Uri.fromFile(File(it)).toString() }

  /**
   * Starts the game at dvdUri in the hakuX core (into the savestate at loadState, when
   * given), the library's process handed off as for xemu.
   */
  fun launch(activity: Activity, dvdUri: String, loadState: String? = null) {
    val intent = Intent(activity, HakuxCoreActivity::class.java)
      .putExtra(HakuxCoreActivity.EXTRA_DVD_URI, dvdUri)
    if (loadState != null) {
      intent.putExtra(HakuxCoreActivity.EXTRA_LOADSTATE, loadState)
    }
    EmulationProcessHandoff.launch(activity, intent)
  }
}
