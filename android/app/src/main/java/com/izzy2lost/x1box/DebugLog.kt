package com.izzy2lost.x1box

import android.content.Context
import android.util.Log
import java.io.File
import java.io.FileOutputStream
import java.io.OutputStream
import java.io.PrintWriter
import java.io.StringWriter
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.concurrent.Executors

object DebugLog {
  const val PREF_ENABLED = "setting_debug_logs_enabled"

  private const val TAG = "xemu-android"
  private const val LOG_DIR = "x1box/debug-logs"
  private const val UI_LOG_FILE_NAME = "ui-debug.log"
  private const val NATIVE_LOG_FILE_NAME = "xemu-debug.log"
  private const val UI_LOGCAT_FILE_NAME = "ui-logcat.log"
  private const val XEMU_LOGCAT_FILE_NAME = "xemu-logcat.log"
  private const val MAX_LOG_BYTES = 16L * 1024L * 1024L

  @Volatile private var appContext: Context? = null
  @PublishedApi
  @Volatile
  internal var enabled = false
  @Volatile private var logcatProcess: java.lang.Process? = null
  @Volatile private var logcatThread: Thread? = null
  @Volatile private var activeLogcatPath: String? = null

  private val writerExecutor = Executors.newSingleThreadExecutor { runnable ->
    Thread(runnable, "x1box-debug-log-writer").apply {
      isDaemon = true
    }
  }

  fun initialize(context: Context) {
    val applicationContext = context.applicationContext
    appContext = applicationContext
    enabled = applicationContext
      .getSharedPreferences("x1box_prefs", Context.MODE_PRIVATE)
      .getBoolean(PREF_ENABLED, false)
    ensureLogcatCaptureState(applicationContext)
  }

  fun setEnabled(context: Context, value: Boolean, resetLogs: Boolean = false) {
    initialize(context)
    if (!value) {
      stopLogcatCapture()
    }
    if (resetLogs) {
      clearLogs(context)
    }
    enabled = value
    if (value) {
      ensureLogcatCaptureState(context.applicationContext)
    }
    if (value) {
      i(TAG) { "Debug logging enabled" }
    } else {
      Log.i(TAG, "Debug logging disabled")
    }
  }

  fun hasAnyLog(context: Context): Boolean {
    return uiLogFile(context).isFile ||
      nativeLogFile(context).isFile ||
      uiLogcatFile(context).isFile ||
      xemuLogcatFile(context).isFile
  }

  fun exportDefaultFileName(): String {
    val stamp = SimpleDateFormat("yyyyMMdd-HHmmss", Locale.US).format(Date())
    return "x1box-debug-$stamp.log"
  }

  @Throws(Exception::class)
  fun exportCombined(context: Context, outputStream: OutputStream) {
    val uiLog = uiLogFile(context)
    val nativeLog = nativeLogFile(context)
    if (!uiLog.isFile && !nativeLog.isFile) {
      throw IllegalStateException("No debug log captured yet.")
    }

    outputStream.bufferedWriter().use { writer ->
      if (uiLog.isFile) {
        writer.appendLine("=== UI Debug Log ===")
        uiLog.bufferedReader().useLines { lines ->
          lines.forEach(writer::appendLine)
        }
      }

      if (nativeLog.isFile) {
        if (uiLog.isFile) {
          writer.appendLine()
        }
        writer.appendLine("=== xemu Native Debug Log ===")
        nativeLog.bufferedReader().useLines { lines ->
          lines.forEach(writer::appendLine)
        }
      }

      val uiLogcat = uiLogcatFile(context)
      if (uiLogcat.isFile) {
        writer.appendLine()
        writer.appendLine("=== UI Logcat Capture ===")
        uiLogcat.bufferedReader().useLines { lines ->
          lines.forEach(writer::appendLine)
        }
      }

      val xemuLogcat = xemuLogcatFile(context)
      if (xemuLogcat.isFile) {
        writer.appendLine()
        writer.appendLine("=== xemu Logcat Capture ===")
        xemuLogcat.bufferedReader().useLines { lines ->
          lines.forEach(writer::appendLine)
        }
      }
    }
  }

  fun clearLogs(context: Context? = appContext) {
    context ?: return
    stopLogcatCapture()
    uiLogFile(context).delete()
    nativeLogFile(context).delete()
    uiLogcatFile(context).delete()
    xemuLogcatFile(context).delete()
  }

  fun resetLogs(context: Context? = appContext) {
    context ?: return
    val shouldResumeCapture = enabled
    clearLogs(context)
    if (shouldResumeCapture) {
      ensureLogcatCaptureState(context.applicationContext)
    }
  }

  inline fun d(tag: String, message: () -> String) {
    if (!enabled) {
      return
    }
    val text = message()
    Log.d(tag, text)
    appendUiLine("D", tag, text)
  }

  inline fun i(tag: String, message: () -> String) {
    if (!enabled) {
      return
    }
    val text = message()
    Log.i(tag, text)
    appendUiLine("I", tag, text)
  }

  inline fun w(tag: String, message: () -> String) {
    if (!enabled) {
      return
    }
    val text = message()
    Log.w(tag, text)
    appendUiLine("W", tag, text)
  }

  inline fun e(tag: String, throwable: Throwable? = null, message: () -> String) {
    val text = message()
    if (throwable != null) {
      Log.e(tag, text, throwable)
    } else {
      Log.e(tag, text)
    }
    if (enabled) {
      appendUiLine("E", tag, text, throwable)
    }
  }

  fun nativeLogFile(context: Context): File {
    return File(logDir(context), NATIVE_LOG_FILE_NAME)
  }

  @PublishedApi
  internal fun appendUiLine(
    level: String,
    tag: String,
    message: String,
    throwable: Throwable? = null,
  ) {
    val context = appContext ?: return
    writerExecutor.execute {
      try {
        val file = uiLogFile(context)
        file.parentFile?.mkdirs()
        trimFileIfNeeded(file)
        file.appendText(
          buildString {
            append(timestamp())
            append(' ')
            append(level)
            append('/')
            append(tag)
            append(": ")
            appendLine(message)
            if (throwable != null) {
              appendLine(stackTraceFor(throwable))
            }
          },
          Charsets.UTF_8
        )
      } catch (_: Exception) {
      }
    }
  }

  private fun uiLogFile(context: Context): File {
    return File(logDir(context), UI_LOG_FILE_NAME)
  }

  private fun uiLogcatFile(context: Context): File {
    return File(logDir(context), UI_LOGCAT_FILE_NAME)
  }

  private fun xemuLogcatFile(context: Context): File {
    return File(logDir(context), XEMU_LOGCAT_FILE_NAME)
  }

  private fun currentProcessLogcatFile(context: Context): File {
    val processName = runCatching {
      File("/proc/self/cmdline").readText().trim('\u0000', ' ', '\n')
    }.getOrDefault("")
    return if (processName.endsWith(":xemu")) {
      xemuLogcatFile(context)
    } else {
      uiLogcatFile(context)
    }
  }

  private fun logDir(context: Context): File {
    return File(context.filesDir, LOG_DIR)
  }

  private fun trimFileIfNeeded(file: File) {
    if (file.isFile && file.length() > MAX_LOG_BYTES) {
      file.writeText("")
    }
  }

  private fun timestamp(): String {
    return SimpleDateFormat("yyyy-MM-dd HH:mm:ss.SSS", Locale.US).format(Date())
  }

  private fun stackTraceFor(throwable: Throwable): String {
    return StringWriter().also { writer ->
      PrintWriter(writer).use { printer ->
        throwable.printStackTrace(printer)
      }
    }.toString().trimEnd()
  }

  private fun ensureLogcatCaptureState(context: Context) {
    if (!enabled) {
      stopLogcatCapture()
      return
    }

    val targetFile = currentProcessLogcatFile(context)
    if (activeLogcatPath == targetFile.absolutePath && logcatProcess != null) {
      return
    }

    stopLogcatCapture()
    startLogcatCapture(targetFile)
  }

  private fun startLogcatCapture(targetFile: File) {
    try {
      targetFile.parentFile?.mkdirs()
      trimFileIfNeeded(targetFile)
      val process = ProcessBuilder(
        "logcat",
        "-T",
        "1",
        "-v",
        "threadtime",
        "--pid=${android.os.Process.myPid()}",
      )
        .redirectErrorStream(true)
        .start()

      val thread = Thread({
        try {
          process.inputStream.bufferedReader().use { reader ->
            FileOutputStream(targetFile, true).bufferedWriter(Charsets.UTF_8).use { writer ->
              while (true) {
                val line = reader.readLine() ?: break
                writer.appendLine(line)
                writer.flush()
              }
            }
          }
        } catch (_: Exception) {
        }
      }, "x1box-logcat-capture").apply {
        isDaemon = true
        start()
      }

      logcatProcess = process
      logcatThread = thread
      activeLogcatPath = targetFile.absolutePath
    } catch (error: Exception) {
      Log.w(TAG, "Failed to start logcat capture", error)
    }
  }

  private fun stopLogcatCapture() {
    logcatProcess?.destroy()
    logcatProcess = null
    logcatThread?.interrupt()
    logcatThread = null
    activeLogcatPath = null
  }
}
