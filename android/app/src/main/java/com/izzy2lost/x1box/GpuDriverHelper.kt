package com.izzy2lost.x1box

import android.content.Context
import android.net.Uri
import android.os.Build
import android.util.Log
import org.json.JSONObject
import java.io.File
import java.io.FileOutputStream
import java.io.IOException
import java.util.zip.ZipFile

object GpuDriverHelper {
  private const val TAG = "GpuDriverHelper"
  private const val META_JSON = "meta.json"

  private lateinit var appContext: Context

  val driverInstallDir: String get() = appContext.filesDir.absolutePath + "/gpu_driver/"
  val driverStorageDir: String
    get() {
      val storageRoot = appContext.getExternalFilesDir(null) ?: appContext.filesDir
      return File(storageRoot, "gpu_drivers").absolutePath + File.separator
    }
  val hookLibDir: String get() = appContext.applicationInfo.nativeLibraryDir + "/"

  fun init(context: Context) {
    appContext = context.applicationContext
    File(driverInstallDir).mkdirs()
    File(driverStorageDir).mkdirs()
  }

  fun supportsCustomDriverLoading(): Boolean {
    return File("/dev/kgsl-3d0").exists()
  }

  fun initializeDriver(customDriverName: String? = null) {
    nativeInitializeDriver(hookLibDir, driverInstallDir, customDriverName)
  }

  fun installDriverFromUri(context: Context, uri: Uri): Boolean {
    init(context)

    val tmpFile = File(driverStorageDir, "driver_tmp.zip")
    try {
      context.contentResolver.openInputStream(uri)?.use { input ->
        FileOutputStream(tmpFile).use { output ->
          input.copyTo(output)
        }
      } ?: return false
    } catch (e: IOException) {
      Log.e(TAG, "Failed to copy driver URI", e)
      tmpFile.delete()
      return false
    }

    val metadata = readMetadata(tmpFile)
    if (metadata == null) {
      Log.e(TAG, "Invalid driver ZIP: no meta.json found")
      tmpFile.delete()
      return false
    }

    if (metadata.minApi > Build.VERSION.SDK_INT) {
      Log.e(TAG, "Driver requires API ${metadata.minApi}, device is ${Build.VERSION.SDK_INT}")
      tmpFile.delete()
      return false
    }

    val safeName = metadata.name
      ?.trim()
      ?.replace(Regex("[^A-Za-z0-9._-]+"), "_")
      ?.trim('.', '_')
      ?.takeIf { it.isNotEmpty() }
      ?: "custom_driver"
    val namedFile = File(driverStorageDir, "$safeName.zip")
    if (!tmpFile.renameTo(namedFile)) {
      try {
        tmpFile.copyTo(namedFile, overwrite = true)
        tmpFile.delete()
      } catch (e: IOException) {
        Log.e(TAG, "Failed to save driver ZIP", e)
        tmpFile.delete()
        return false
      }
    }

    return installDriver(namedFile)
  }

  fun installDriver(driverZip: File): Boolean {
    val installDir = File(driverInstallDir)
    installDir.deleteRecursively()
    if (!installDir.mkdirs() && !installDir.isDirectory) {
      Log.e(TAG, "Failed to create driver install directory")
      return false
    }
    val installRoot = installDir.canonicalFile
    val installRootPrefix = installRoot.path + File.separator

    try {
      ZipFile(driverZip).use { zip ->
        zip.entries().asSequence().forEach { entry ->
          val outFile = File(installRoot, entry.name).canonicalFile
          if (outFile != installRoot && !outFile.path.startsWith(installRootPrefix)) {
            throw IOException("Driver ZIP entry escapes install directory: ${entry.name}")
          }
          if (entry.isDirectory) {
            if (!outFile.mkdirs() && !outFile.isDirectory) {
              throw IOException("Failed to create driver directory: ${entry.name}")
            }
          } else {
            val parent = outFile.parentFile
            if (parent != null && !parent.mkdirs() && !parent.isDirectory) {
              throw IOException("Failed to create driver directory: ${entry.name}")
            }
            zip.getInputStream(entry).use { input ->
              FileOutputStream(outFile).use { output ->
                input.copyTo(output)
              }
            }
          }
        }
      }
    } catch (e: Exception) {
      Log.e(TAG, "Failed to extract driver", e)
      return false
    }

    return true
  }

  fun installDefaultDriver() {
    File(driverInstallDir).deleteRecursively()
    File(driverInstallDir).mkdirs()
  }

  fun getInstalledDriverName(): String? {
    val metaFile = File(driverInstallDir, META_JSON)
    if (!metaFile.exists()) return null
    return try {
      val json = JSONObject(metaFile.readText())
      json.optionalString("name")
    } catch (e: Exception) {
      null
    }
  }

  fun getInstalledDriverLibrary(): String? {
    val metaFile = File(driverInstallDir, META_JSON)
    if (!metaFile.exists()) return null
    return try {
      val json = JSONObject(metaFile.readText())
      json.optionalString("libraryName")
    } catch (e: Exception) {
      null
    }
  }

  fun getAvailableDrivers(): List<DriverMetadata> {
    val dir = File(driverStorageDir)
    if (!dir.exists()) return emptyList()
    return dir.listFiles()
      ?.filter { it.extension == "zip" }
      ?.mapNotNull { readMetadata(it)?.copy(path = it.absolutePath) }
      ?.sortedBy { it.name }
      ?: emptyList()
  }

  fun readMetadata(zipFile: File): DriverMetadata? {
    if (!zipFile.exists()) return null
    try {
      ZipFile(zipFile).use { zip ->
        val entries = zip.entries()
        while (entries.hasMoreElements()) {
          val entry = entries.nextElement()
          if (!entry.isDirectory && entry.name.equals(META_JSON, ignoreCase = true)) {
            zip.getInputStream(entry).use { input ->
              val text = input.bufferedReader().readText()
              val json = JSONObject(text)
              return DriverMetadata(
                name = json.optionalString("name"),
                description = json.optionalString("description"),
                author = json.optionalString("author"),
                libraryName = json.optionalString("libraryName"),
                minApi = json.optInt("minApi", 0),
                path = zipFile.absolutePath
              )
            }
          }
        }
      }
    } catch (e: Exception) {
      Log.e(TAG, "Failed to read driver metadata from ${zipFile.name}", e)
    }
    return null
  }

  private fun JSONObject.optionalString(key: String): String? {
    return optString(key)
      .trim()
      .takeIf { it.isNotEmpty() && !it.equals("null", ignoreCase = true) }
  }

  private external fun nativeInitializeDriver(
    hookLibDir: String?,
    customDriverDir: String?,
    customDriverName: String?
  )

  data class DriverMetadata(
    val name: String? = null,
    val description: String? = null,
    val author: String? = null,
    val libraryName: String? = null,
    val minApi: Int = 0,
    val path: String? = null
  )
}
