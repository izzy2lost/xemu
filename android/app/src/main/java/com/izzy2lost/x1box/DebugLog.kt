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
  private const val CRASH_TAIL_FILE_NAME = "crash-tail.log"
  private const val MAX_LOG_BYTES = 16L * 1024L * 1024L

  @Volatile private var appContext: Context? = null
  @PublishedApi
  @Volatile
  internal var enabled = false
  @Volatile private var logcatProcess: java.lang.Process? = null
  @Volatile private var logcatThread: Thread? = null
  @Volatile private var activeLogcatPath: String? = null
  @Volatile private var crashTailCaptured = false

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
    captureCrashTail(applicationContext)
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
      // initialize() above ran while `enabled` still held the old (possibly
      // false) value, so the crash drain would have been skipped on the
      // transition into enabled. Retry now that the flag is set.
      captureCrashTail(context.applicationContext)
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
      xemuLogcatFile(context).isFile ||
      crashTailFile(context).isFile
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
      // Leads, because this is the only section that can contain a fatal
      // signal or tombstone. Read fresh here rather than reusing the
      // launch-time snapshot: the emulator runs in its own process, so a crash
      // in it does not restart the UI process, and a snapshot taken at UI
      // launch would predate the very crash being reported.
      writer.appendLine("=== Crash Buffer (read at export) ===")
      writer.appendLine(readCrashBufferDump())

      // The buffer is small and rolls over, so also include the snapshot taken
      // at launch in case the crash of interest has since been evicted.
      val crashTail = crashTailFile(context)
      if (crashTail.isFile) {
        writer.appendLine("=== Crash Buffer (snapshot at launch) ===")
        crashTail.bufferedReader().useLines { lines ->
          lines.forEach(writer::appendLine)
        }
        writer.appendLine()
      }

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
    crashTailFile(context).delete()
    // Allow the next initialize()/setEnabled() to re-drain the crash buffer;
    // otherwise clearing logs would permanently suppress the crash section for
    // the life of the process.
    crashTailCaptured = false
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

  private fun crashTailFile(context: Context): File {
    return File(logDir(context), CRASH_TAIL_FILE_NAME)
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

  /**
   * Drain the crash buffer into a file at process start.
   *
   * The live capture in [startLogcatCapture] runs `logcat --pid=<self>` from a
   * thread inside this process, so it can never record this process dying: on a
   * fatal signal every thread stops before the reader can drain the pipe, and a
   * native tombstone is emitted by `crash_dump64` under a different pid anyway.
   * The crash buffer outlives the process, so the only place a crash can be
   * observed is here -- on the next launch, before anything else runs.
   *
   * logd only returns entries matching our own uid unless the app holds
   * READ_LOGS, which a normal build does not. The tombstone is written by
   * crash_dump64 running as the crashed process' uid, so it comes through; the
   * ActivityManager/lowmemorykiller lines (which is where an OOM kill would
   * show up, as opposed to a segfault) belong to system_server and will only
   * appear on a rooted or userdebug device. Absence of both is therefore not
   * evidence of a clean exit.
   */
  private fun captureCrashTail(context: Context) {
    if (!enabled || crashTailCaptured) {
      return
    }
    crashTailCaptured = true
    writerExecutor.execute {
      try {
        val file = crashTailFile(context)
        file.parentFile?.mkdirs()
        // Overwrite rather than append: the buffer is cumulative, so each launch
        // already re-reads everything still retained, including the last crash.
        file.writeText(readCrashBufferDump(), Charsets.UTF_8)
      } catch (_: Exception) {
      }
    }
  }

  /** Snapshot of the crash buffer, plus kill records where readable. */
  private fun readCrashBufferDump(): String {
    return buildString {
      appendLine("--- read at ${timestamp()} (pid ${android.os.Process.myPid()})")
      // Whole buffer, unfiltered: a tombstone's backtrace and register dump
      // lines do not mention the package name, so filtering would shred it.
      appendLine("--- logcat -b crash")
      append(dumpLogBuffer(listOf("-b", "crash", "-t", "500")))
      appendLine("--- logcat -b system (ActivityManager/lowmemorykiller; needs READ_LOGS)")
      append(
        dumpLogBuffer(
          listOf(
            "-b", "system", "-t", "300",
            "-s", "ActivityManager:I", "lowmemorykiller:I", "libc:I", "DEBUG:I",
          )
        )
      )
    }
  }

  private fun dumpLogBuffer(args: List<String>): String {
    return try {
      val process = ProcessBuilder(listOf("logcat", "-d", "-v", "threadtime") + args)
        .redirectErrorStream(true)
        .start()
      val output = process.inputStream.bufferedReader().use { it.readText() }
      process.waitFor()
      if (output.isBlank()) "(empty)\n" else output
    } catch (error: Exception) {
      "(failed: ${error.message})\n"
    }
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
