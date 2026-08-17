package com.izzy2lost.x1box

import android.app.Activity
import android.content.Intent
import android.os.Handler
import android.os.Looper
import android.os.Process

/**
 * MainActivity intentionally runs in :xemu so a native emulator shutdown does
 * not take the library with it. Once Android has resumed that activity, the
 * finished frontend process has no work left to do, but Android normally keeps
 * its large cover-art/UI heap cached. On 4 GB devices lowmemorykiller then
 * reclaims it during gameplay, creating a major swap/reclaim hitch.
 */
object EmulationProcessHandoff {
  private const val FRONTEND_EXIT_DELAY_MS = 1_000L

  fun launch(activity: Activity, intent: Intent) {
    activity.startActivity(intent)
    activity.finish()

    // startActivity() has crossed the system-server boundary before it
    // returns. The delay gives :xemu time to become resumed, then releases
    // only this now-finished frontend process and its cached bitmaps.
    Handler(Looper.getMainLooper()).postDelayed({
      Process.killProcess(Process.myPid())
    }, FRONTEND_EXIT_DELAY_MS)
  }
}
