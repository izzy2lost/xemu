package com.izzy2lost.x1box

import java.io.File
import java.io.IOException

internal object XboxDashboardImporter {

  @Throws(IOException::class)
  fun importDashboard(
    hddFile: File,
    sourceRoot: File,
    backupRoot: File,
  ) {
    require(hddFile.isFile) { "No local HDD image is configured." }
    require(sourceRoot.isDirectory) { "The selected dashboard source is not available." }
    if (!backupRoot.exists() && !backupRoot.mkdirs()) {
      throw IOException("Failed to create the dashboard backup folder.")
    }

    NativeBridge.nativeImportDashboard(
      hddFile.absolutePath,
      sourceRoot.absolutePath,
      backupRoot.absolutePath,
    )
  }

  private object NativeBridge {
    init {
      System.loadLibrary("SDL3")
      System.loadLibrary("xemu")
    }

    external fun nativeImportDashboard(
      hddPath: String,
      sourceRoot: String,
      backupRoot: String,
    )
  }
}
