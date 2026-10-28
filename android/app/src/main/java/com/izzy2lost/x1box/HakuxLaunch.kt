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

  /** The core a library game will run on, its own override first (before startGame applies it). */
  fun coreFor(context: Context, relativePath: String): String {
    val prefs = context.getSharedPreferences("x1box_prefs", Context.MODE_PRIVATE)
    return PerGameSettingsManager.loadOverrides(context, relativePath)[PREF_CORE] ?: globalCore(prefs)
  }

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
  /** The dashboard's XBE on the hakuX core's hard disk (C:, Partition2), as the kernel names it. */
  const val DASHBOARD_XBE = "\\Device\\Harddisk0\\Partition2\\xboxdash.xbe"

  /** The dashboard's file in the hakuX core's C: (SettingsActivity imports it there). */
  fun hakuxDashboard(context: Context): File = File(context.filesDir, "hakux/hdd/Partition2/xboxdash.xbe")

  /**
   * Starts the Xbox dashboard in the hakuX core from its hard disk. The core boots from a disc,
   * so it gets an empty one (emptyDisc); the dashboard sees no disc in the tray.
   */
  fun launchDashboard(activity: Activity) {
    val intent = Intent(activity, HakuxCoreActivity::class.java)
      .putExtra(HakuxCoreActivity.EXTRA_DVD_URI, Uri.fromFile(emptyDisc(activity)).toString())
      .putExtra(HakuxCoreActivity.EXTRA_BOOT_XBE, DASHBOARD_XBE)
    EmulationProcessHandoff.launch(activity, intent)
  }

  /**
   * An XDVDFS image with nothing in it (one empty file at its root): the volume descriptor at
   * sector 32, the root directory at 33.
   */
  private fun emptyDisc(context: Context): File {
    val file = File(context.filesDir, "hakux/empty-disc.iso")
    if (file.length() == 34L * 2048) return file
    val image = ByteArray(34 * 2048)
    val magic = "MICROSOFT*XBOX*MEDIA".toByteArray(Charsets.US_ASCII)
    val vd = 32 * 2048
    magic.copyInto(image, vd)
    fun le32(at: Int, v: Int) {
      for (i in 0 until 4) image[at + i] = (v ushr (8 * i)).toByte()
    }
    le32(vd + 20, 33)
    le32(vd + 24, 2048)
    magic.copyInto(image, vd + 0x7EC)
    val root = 33 * 2048
    image.fill(0xFF.toByte(), root, root + 2048)
    val name = "x1box.txt".toByteArray(Charsets.US_ASCII)
    image.fill(0, root, root + 14)              // no subtrees, sector 0, size 0
    image[root + 12] = 0x20                     // a normal file
    image[root + 13] = name.size.toByte()
    name.copyInto(image, root + 14)
    file.parentFile?.mkdirs()
    file.writeBytes(image)
    return file
  }

  /**
   * Starts an unpacked Chihiro game (ChihiroGameFolder.hakuxGameDir) in the hakuX core: its
   * XBE from the hard disk with an empty disc in the tray, and the Sega baseboard on the USB.
   * The baseboard's EEPROM dumps (ic10, ic11, pc20) are those beside the flash ROM set for
   * xemu, else files/chihiro-roms; the core has House of the Dead 3's built in otherwise.
   * Returns false when the folder holds no XBE.
   */
  fun launchChihiro(activity: Activity, dir: File): Boolean {
    val xbe = ChihiroGameFolder.bootExecutable(dir) ?: return false
    val partition1 = File(activity.filesDir, "hakux/hdd/Partition1")
    val rel = dir.relativeTo(partition1).path.replace('/', '\\')
    val intent = Intent(activity, HakuxCoreActivity::class.java)
      .putExtra(HakuxCoreActivity.EXTRA_DVD_URI, Uri.fromFile(emptyDisc(activity)).toString())
      .putExtra(HakuxCoreActivity.EXTRA_BOOT_XBE, "\\Device\\Harddisk0\\Partition1\\$rel\\$xbe")
      .putExtra(HakuxCoreActivity.EXTRA_CHIHIRO_ROMS, chihiroRoms(activity)?.absolutePath ?: "")
      .putExtra(
        HakuxCoreActivity.EXTRA_CHIHIRO_SAVE,
        File(activity.filesDir, "hakux/chihiro-saves/${dir.name}.sav").apply { parentFile?.mkdirs() }.absolutePath,
      )
    EmulationProcessHandoff.launch(activity, intent)
    return true
  }

  private fun chihiroRoms(context: Context): File? {
    val prefs = context.getSharedPreferences("x1box_prefs", Context.MODE_PRIVATE)
    val beside = prefs.getString("flashPath", null)?.let { File(it).parentFile }
    return listOfNotNull(beside, File(context.filesDir, "chihiro-roms"))
      .firstOrNull { File(it, "ic10_g24lc64.bin").isFile }
  }

  fun launch(activity: Activity, dvdUri: String, loadState: String? = null) {
    val intent = Intent(activity, HakuxCoreActivity::class.java)
      .putExtra(HakuxCoreActivity.EXTRA_DVD_URI, dvdUri)
    if (loadState != null) {
      intent.putExtra(HakuxCoreActivity.EXTRA_LOADSTATE, loadState)
    }
    EmulationProcessHandoff.launch(activity, intent)
  }
}
