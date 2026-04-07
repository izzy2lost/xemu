package com.izzy2lost.x1box

import android.content.Context
import android.content.SharedPreferences
import java.security.MessageDigest
import java.util.Locale

object PerGameSettingsManager {
  private const val APP_PREFS_NAME = "x1box_prefs"
  private const val STORE_PREFS_NAME = "x1box_per_game_settings"
  private const val STORE_KEY_PREFIX = "game_override:"
  private const val RUNTIME_KEY_PREFIX = "runtime_override_"

  val overridablePreferenceKeys = listOf(
    "setting_gpu_driver",
    "setting_renderer",
    "setting_filtering",
    "setting_vsync",
    "setting_surface_scale",
    "setting_display_mode",
    OrientationPreferences.PREF_GAME_ORIENTATION,
    "setting_system_memory_mib",
    "setting_tcg_thread",
    "setting_use_dsp",
    "setting_hrtf",
    "setting_cache_shaders",
    "setting_hard_fpu",
    "setting_skip_boot_anim",
    "setting_audio_driver",
    "setting_network_enable",
  )

  fun hasOverrides(context: Context, relativePath: String): Boolean {
    val prefs = storePreferences(context)
    val gameId = gameId(relativePath)
    return overridablePreferenceKeys.any { key ->
      prefs.contains(storageKey(gameId, key))
    }
  }

  fun loadOverrides(context: Context, relativePath: String): Map<String, String> {
    val prefs = storePreferences(context)
    val gameId = gameId(relativePath)
    return buildMap {
      for (key in overridablePreferenceKeys) {
        prefs.getString(storageKey(gameId, key), null)
          ?.takeIf { value -> value.isNotEmpty() }
          ?.let { value -> put(key, value) }
      }
    }
  }

  fun saveOverrides(
    context: Context,
    relativePath: String,
    overrides: Map<String, String?>,
  ) {
    val prefs = storePreferences(context)
    val gameId = gameId(relativePath)
    val editor = prefs.edit()
    for (key in overridablePreferenceKeys) {
      val value = overrides[key]
      if (value.isNullOrEmpty()) {
        editor.remove(storageKey(gameId, key))
      } else {
        editor.putString(storageKey(gameId, key), value)
      }
    }
    editor.apply()
  }

  fun clearOverrides(context: Context, relativePath: String) {
    val prefs = storePreferences(context)
    val gameId = gameId(relativePath)
    val editor = prefs.edit()
    for (key in overridablePreferenceKeys) {
      editor.remove(storageKey(gameId, key))
    }
    editor.apply()
  }

  fun applyRuntimeOverridesToEditor(
    context: Context,
    editor: SharedPreferences.Editor,
    relativePath: String?,
  ) {
    val overrides = relativePath
      ?.takeIf { path -> path.isNotBlank() }
      ?.let { path -> loadOverrides(context, path) }
      .orEmpty()

    for (key in overridablePreferenceKeys) {
      val runtimeKey = runtimeKey(key)
      val value = overrides[key]
      if (value.isNullOrEmpty()) {
        editor.remove(runtimeKey)
      } else {
        editor.putString(runtimeKey, value)
      }
    }
  }

  fun getRuntimeOverride(context: Context, key: String): String? {
    return appPreferences(context).getString(runtimeKey(key), null)
      ?.takeIf { value -> value.isNotEmpty() }
  }

  fun runtimeKey(key: String): String = RUNTIME_KEY_PREFIX + key

  private fun gameId(relativePath: String): String {
    val normalized = relativePath.trim().lowercase(Locale.ROOT)
    val digest = MessageDigest.getInstance("SHA-256")
    val bytes = digest.digest(normalized.toByteArray(Charsets.UTF_8))
    return bytes.joinToString(separator = "") { byte -> "%02x".format(byte.toInt() and 0xFF) }
  }

  private fun storageKey(gameId: String, key: String): String =
    STORE_KEY_PREFIX + gameId + ":" + key

  private fun appPreferences(context: Context): SharedPreferences {
    return context.applicationContext.getSharedPreferences(APP_PREFS_NAME, Context.MODE_PRIVATE)
  }

  private fun storePreferences(context: Context): SharedPreferences {
    return context.applicationContext.getSharedPreferences(STORE_PREFS_NAME, Context.MODE_PRIVATE)
  }
}
