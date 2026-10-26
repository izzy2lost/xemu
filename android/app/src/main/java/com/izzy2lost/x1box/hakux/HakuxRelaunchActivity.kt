/*
 * Adapted from hakuX's RelaunchActivity (https://github.com/rfandango/hakuX,
 * GPLv3 or later; see android/hakux/core/COPYING).
 */
package com.izzy2lost.x1box.hakux

import android.app.Activity
import android.app.ActivityManager
import android.app.ActivityOptions
import android.content.Context
import android.content.Intent
import android.graphics.BitmapFactory
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.util.Log
import android.view.WindowInsets
import android.view.WindowInsetsController
import android.widget.ImageView
import java.io.File

/**
 * A title launched another of its XBEs (XLaunchNewImage: Splinter Cell into
 * each level, Morrowind's New Game). The hakuX core lives for one run, so it
 * starts again in a new :hakux process with "relaunch", once the old process
 * has gone. This activity, in the main process, waits for that while showing
 * the frame the game left on screen, then starts the core with no transition.
 */
class HakuxRelaunchActivity : Activity() {
  private val handler = Handler(Looper.getMainLooper())

  override fun onCreate(savedInstanceState: Bundle?) {
    super.onCreate(savedInstanceState)
    setContentView(ImageView(this).apply {
      scaleType = ImageView.ScaleType.FIT_XY
      BitmapFactory.decodeFile(heldFrame(this@HakuxRelaunchActivity).path)?.let(::setImageBitmap)
    })
    hideSystemBars()
    val am = getSystemService(ACTIVITY_SERVICE) as ActivityManager
    val coreProcess = "$packageName:hakux"
    val start = SystemClock.uptimeMillis()
    handler.post(object : Runnable {
      override fun run() {
        val alive = am.runningAppProcesses?.any { it.processName == coreProcess } == true
        if (alive && SystemClock.uptimeMillis() - start < WAIT_MS) {
          handler.postDelayed(this, 20)
          return
        }
        Log.i(TAG, "relaunch: core process gone after ${SystemClock.uptimeMillis() - start} ms")
        startCore()
      }
    })
  }

  private fun startCore() {
    val core = Intent(this, HakuxCoreActivity::class.java)
      .putExtra(HakuxCoreActivity.EXTRA_DVD_URI, intent.getStringExtra(HakuxCoreActivity.EXTRA_DVD_URI))
      .apply {
        /* a savestate's load, or the title's own reboot */
        val state = intent.getStringExtra(HakuxCoreActivity.EXTRA_LOADSTATE)
        if (state != null) putExtra(HakuxCoreActivity.EXTRA_LOADSTATE, state)
        else if (!intent.getBooleanExtra(EXTRA_RESTART, false)) putExtra(HakuxCoreActivity.EXTRA_RELAUNCH, true)
      }
      .addFlags(Intent.FLAG_ACTIVITY_NO_ANIMATION)
    startActivity(core, noTransition(this).toBundle())
    finish()
    noCloseTransition(this)
  }

  private fun hideSystemBars() {
    if (Build.VERSION.SDK_INT >= 30) {
      window.insetsController?.let {
        it.hide(WindowInsets.Type.systemBars())
        it.systemBarsBehavior = WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
      }
    }
  }

  override fun onDestroy() {
    handler.removeCallbacksAndMessages(null)
    super.onDestroy()
  }

  companion object {
    private const val TAG = "x1box-hakux"
    private const val WAIT_MS = 3000L
    /** The game from its start (its mods changed): neither a savestate nor the title's own reboot. */
    const val EXTRA_RESTART = "restart"

    /** The frame on screen when the title rebooted, held across the core's restart. */
    fun heldFrame(context: Context) = File(context.cacheDir, "hakux_relaunch_frame.jpg")

    /** An activity start with no window animation and, on Android 13+, a splash without the icon. */
    fun noTransition(context: Context): ActivityOptions {
      val o = ActivityOptions.makeCustomAnimation(context, 0, 0)
      if (Build.VERSION.SDK_INT >= 33) {
        o.setSplashScreenStyle(android.window.SplashScreen.SPLASH_SCREEN_STYLE_SOLID_COLOR)
      }
      return o
    }

    @Suppress("DEPRECATION")
    fun noCloseTransition(activity: Activity) {
      if (Build.VERSION.SDK_INT >= 34) {
        activity.overrideActivityTransition(Activity.OVERRIDE_TRANSITION_CLOSE, 0, 0)
      } else {
        activity.overridePendingTransition(0, 0)
      }
    }
  }
}
