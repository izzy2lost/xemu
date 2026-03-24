package com.izzy2lost.x1box

import android.app.Activity

class OrientationLocker(private val activity: Activity, private val landscapeOnly: Boolean = false) {
  fun enable() {
    val target = if (landscapeOnly) {
      OrientationPreferences.getGameRequestedOrientation(activity)
    } else {
      OrientationPreferences.getUiRequestedOrientation(activity)
    }

    if (activity.requestedOrientation != target) {
      activity.requestedOrientation = target
    }
  }

  fun disable() {
  }
}
