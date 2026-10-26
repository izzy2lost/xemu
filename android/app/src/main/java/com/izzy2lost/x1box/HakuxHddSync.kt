package com.izzy2lost.x1box

import android.app.ActivityManager
import android.content.Context
import android.os.SystemClock
import android.util.Log
import java.io.File
import java.util.concurrent.Executors

/**
 * One hard disk for both cores. xemu's image (the "hddPath" setting) is where the saves
 * live; the hakuX core works on a folder (files/hakux/hdd, a folder per partition).
 * Before a hakuX run E:'s UDATA and TDATA are copied into the folder's Partition1 with
 * a manifest of what was copied; after the run, what it added, changed or deleted goes
 * back into the image. A run that never came back through the library (killed, crashed)
 * is brought back the next time the library starts or a game is launched: the manifest
 * stays until then.
 *
 * Without an image (no hddPath), the hakuX core keeps its own folder as before.
 */
object HakuxHddSync {
  private const val TAG = "x1box-hakux-sync"
  private val DIRS = listOf("UDATA", "TDATA")
  private const val MANIFEST = ".x1box-sync"
  /* Saves the hakuX core made before there was a sync (or without an image) were merged
   * into the image once. */
  private const val MERGED = ".x1box-merged"
  private const val CORE_EXIT_WAIT_MS = 3000L

  /** Every sync runs here, one at a time. */
  private val worker = Executors.newSingleThreadExecutor { r -> Thread(r, "hakux-hdd-sync") }

  private fun hddRoot(context: Context) = File(context.filesDir, "hakux/hdd")
  private fun partition1(context: Context) = File(hddRoot(context), "Partition1")
  private fun manifestFile(context: Context) = File(hddRoot(context), MANIFEST)

  /** The hakuX core's E: (where its saves stay when there is no image), made if missing. */
  fun drive(context: Context): File = partition1(context).apply { mkdirs() }

  private fun hddFile(context: Context): File? =
    context.getSharedPreferences("x1box_prefs", Context.MODE_PRIVATE)
      .getString("hddPath", null)?.let(::File)?.takeIf { it.isFile }

  /** A hakuX run's changes not yet back in the image. */
  fun pending(context: Context): Boolean = manifestFile(context).isFile

  /** Runs [work] on the sync thread, then [done] (with whether it worked) on it too. */
  fun async(work: () -> Unit, done: (Boolean) -> Unit) {
    worker.execute {
      val ok = runCatching(work).onFailure { Log.e(TAG, "sync failed", it) }.isSuccess
      done(ok)
    }
  }

  /** Before a hakuX run: the image's saves into the core's folder. */
  @Synchronized
  fun exportBeforeRun(context: Context) {
    val hdd = hddFile(context) ?: return
    importAfterRun(context)             /* a previous run's changes go back first */
    mergeOnce(context, hdd)
    val root = partition1(context)
    root.mkdirs()
    val t0 = SystemClock.uptimeMillis()
    XboxSaveManager.exportDirs(hdd, DIRS.map { "/$it" }, root)
    writeManifest(manifestFile(context), scan(root))
    Log.i(TAG, "image -> hakuX: ${SystemClock.uptimeMillis() - t0} ms")
  }

  /** After a hakuX run (or the next time we can): its changes back into the image. */
  @Synchronized
  fun importAfterRun(context: Context) {
    val mf = manifestFile(context)
    if (!mf.isFile) return
    val hdd = hddFile(context)
    if (hdd == null) {
      mf.delete()                       /* the image went away: nothing to bring back to */
      return
    }
    waitForCoreExit(context)
    val root = partition1(context)
    val before = readManifest(mf)
    val now = scan(root)
    val changed = now.filter { (path, stamp) -> stamp != DIR && before[path] != stamp }.keys
    val gone = before.keys.filter { it !in now }
      .filter { path -> before.keys.none { it != path && path.startsWith("$it/") && it !in now } }
    if (changed.isEmpty() && gone.isEmpty()) {
      mf.delete()
      return
    }
    val t0 = SystemClock.uptimeMillis()
    /* Only what changed, staged as the folders it merges into: each game's folder
     * (UDATA/<title id>, TDATA/<title id>) */
    val staging = File(context.cacheDir, "hakux-sync-staging")
    staging.deleteRecursively()
    try {
      for (path in changed) {
        val dst = File(staging, path)
        dst.parentFile?.mkdirs()
        File(root, path).copyTo(dst, overwrite = true)
      }
      val imports = changed.map { it.split('/').take(2).joinToString("/") }.distinct()
        .map { top -> File(staging, top) to "/$top" }
        .filter { (dir, _) -> dir.exists() }
        .map { (dir, fatx) -> if (dir.isDirectory) dir to fatx else dir.parentFile!! to fatx.substringBeforeLast('/') }
        .distinct()
      XboxSaveManager.applyChanges(hdd, imports, gone.map { "/$it" })
    } finally {
      staging.deleteRecursively()
    }
    mf.delete()
    Log.i(TAG, "hakuX -> image: ${changed.size} changed, ${gone.size} removed, " +
      "${SystemClock.uptimeMillis() - t0} ms")
  }

  /**
   * The first sync on this device: saves the hakuX core already made (its folder,
   * before syncing existed) go into the image, deleting nothing (a file of the same
   * name there is replaced), so the export that follows doesn't lose them.
   */
  private fun mergeOnce(context: Context, hdd: File) {
    val marker = File(hddRoot(context), MERGED)
    if (marker.exists()) return
    val root = partition1(context)
    val imports = DIRS.flatMap { top ->
      File(root, top).listFiles()?.filter { it.isDirectory }?.map { it to "/$top/${it.name}" }.orEmpty()
    }
    if (imports.isNotEmpty()) {
      XboxSaveManager.applyChanges(hdd, imports, emptyList())
      Log.i(TAG, "hakuX's own saves merged into the image: ${imports.size} folders")
    }
    hddRoot(context).mkdirs()
    marker.createNewFile()
  }

  /** The core's process still writing would race the import: it goes first. */
  private fun waitForCoreExit(context: Context) {
    val am = context.getSystemService(Context.ACTIVITY_SERVICE) as ActivityManager
    val core = "${context.packageName}:hakux"
    val start = SystemClock.uptimeMillis()
    while (am.runningAppProcesses?.any { it.processName == core } == true &&
      SystemClock.uptimeMillis() - start < CORE_EXIT_WAIT_MS) {
      Thread.sleep(20)
    }
  }

  private const val DIR = "D"

  /** Path under Partition1 ("UDATA/4d530004/...") -> "size:mtime", or DIR for a folder. */
  private fun scan(root: File): Map<String, String> {
    val out = HashMap<String, String>()
    for (top in DIRS) {
      val base = File(root, top)
      if (!base.isDirectory) continue
      base.walkTopDown().forEach { f ->
        val rel = f.relativeTo(root).path
        out[rel] = if (f.isDirectory) DIR else "${f.length()}:${f.lastModified()}"
      }
    }
    return out
  }

  private fun writeManifest(file: File, entries: Map<String, String>) {
    val tmp = File(file.path + ".tmp")
    tmp.bufferedWriter().use { w ->
      for ((path, stamp) in entries) {
        w.write(stamp)
        w.write("\t")
        w.write(path)
        w.write("\n")
      }
    }
    tmp.renameTo(file)
  }

  private fun readManifest(file: File): Map<String, String> =
    file.readLines().mapNotNull { line ->
      val tab = line.indexOf('\t')
      if (tab < 0) null else line.substring(tab + 1) to line.substring(0, tab)
    }.toMap()
}
