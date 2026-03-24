package com.izzy2lost.x1box

import android.content.Context
import android.content.SharedPreferences
import android.content.pm.ActivityInfo

object OrientationPreferences {
  private const val PREFS_NAME = "x1box_prefs"

  const val PREF_UI_ORIENTATION = "setting_ui_orientation"
  const val PREF_GAME_ORIENTATION = "setting_game_orientation"

  enum class UiOrientation(
    val prefValue: String,
    val requestedOrientation: Int,
  ) {
    FOLLOW_DEVICE("follow_device", ActivityInfo.SCREEN_ORIENTATION_FULL_SENSOR),
    PORTRAIT("portrait", ActivityInfo.SCREEN_ORIENTATION_PORTRAIT),
    REVERSE_PORTRAIT("reverse_portrait", ActivityInfo.SCREEN_ORIENTATION_REVERSE_PORTRAIT),
    LANDSCAPE("landscape", ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE),
    REVERSE_LANDSCAPE("reverse_landscape", ActivityInfo.SCREEN_ORIENTATION_REVERSE_LANDSCAPE),
    ;

    companion object {
      fun fromPrefValue(value: String?): UiOrientation {
        return values().firstOrNull { it.prefValue == value } ?: FOLLOW_DEVICE
      }
    }
  }

  enum class GameOrientation(
    val prefValue: String,
    val requestedOrientation: Int,
  ) {
    FOLLOW_DEVICE(
      "follow_device",
      ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE,
    ),
    LANDSCAPE(
      "landscape",
      ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE,
    ),
    REVERSE_LANDSCAPE(
      "reverse_landscape",
      ActivityInfo.SCREEN_ORIENTATION_REVERSE_LANDSCAPE,
    ),
    ;

    companion object {
      fun fromPrefValue(value: String?): GameOrientation {
        return values().firstOrNull { it.prefValue == value } ?: FOLLOW_DEVICE
      }
    }
  }

  fun getUiOrientation(context: Context): UiOrientation {
    return UiOrientation.fromPrefValue(sharedPreferences(context).getString(PREF_UI_ORIENTATION, null))
  }

  fun getGameOrientation(context: Context): GameOrientation {
    return GameOrientation.fromPrefValue(sharedPreferences(context).getString(PREF_GAME_ORIENTATION, null))
  }

  fun getUiRequestedOrientation(context: Context): Int {
    return getUiOrientation(context).requestedOrientation
  }

  fun getGameRequestedOrientation(context: Context): Int {
    return getGameOrientation(context).requestedOrientation
  }

  private fun sharedPreferences(context: Context): SharedPreferences {
    return context.applicationContext.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
  }
}
