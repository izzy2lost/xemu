package com.izzy2lost.x1box

import android.content.Context
import android.os.Bundle
import android.widget.Toast
import androidx.appcompat.app.AppCompatActivity
import com.google.android.material.button.MaterialButton
import com.google.android.material.button.MaterialButtonToggleGroup
import com.google.android.material.materialswitch.MaterialSwitch

class SettingsActivity : AppCompatActivity() {

  private val prefs by lazy { getSharedPreferences("x1box_prefs", Context.MODE_PRIVATE) }

  override fun onCreate(savedInstanceState: Bundle?) {
    super.onCreate(savedInstanceState)
    setContentView(R.layout.activity_settings)

    val toggleScale       = findViewById<MaterialButtonToggleGroup>(R.id.toggle_resolution_scale)
    val btn1x             = findViewById<MaterialButton>(R.id.btn_scale_1x)
    val btn2x             = findViewById<MaterialButton>(R.id.btn_scale_2x)
    val btn3x             = findViewById<MaterialButton>(R.id.btn_scale_3x)
    val toggleDisplayMode = findViewById<MaterialButtonToggleGroup>(R.id.toggle_display_mode)
    val toggleThread      = findViewById<MaterialButtonToggleGroup>(R.id.toggle_tcg_thread)
    val btnMulti          = findViewById<MaterialButton>(R.id.btn_thread_multi)
    val btnSingle         = findViewById<MaterialButton>(R.id.btn_thread_single)
    val switchDsp         = findViewById<MaterialSwitch>(R.id.switch_use_dsp)
    val switchHrtf        = findViewById<MaterialSwitch>(R.id.switch_hrtf)
    val switchShaders     = findViewById<MaterialSwitch>(R.id.switch_cache_shaders)
    val switchFpu         = findViewById<MaterialSwitch>(R.id.switch_hard_fpu)
    val toggleAudioDriver = findViewById<MaterialButtonToggleGroup>(R.id.toggle_audio_driver)
    val btnSave           = findViewById<MaterialButton>(R.id.btn_settings_save)

    // Load current values
    val scale = prefs.getInt("setting_surface_scale", 1)
    when (scale) {
      2    -> toggleScale.check(R.id.btn_scale_2x)
      3    -> toggleScale.check(R.id.btn_scale_3x)
      else -> toggleScale.check(R.id.btn_scale_1x)
    }

    val displayMode = prefs.getInt("setting_display_mode", 0)
    when (displayMode) {
      1    -> toggleDisplayMode.check(R.id.btn_display_4_3)
      2    -> toggleDisplayMode.check(R.id.btn_display_16_9)
      else -> toggleDisplayMode.check(R.id.btn_display_stretch)
    }

    val tcgThread = prefs.getString("setting_tcg_thread", "multi") ?: "multi"
    if (tcgThread == "single") {
      toggleThread.check(R.id.btn_thread_single)
    } else {
      toggleThread.check(R.id.btn_thread_multi)
    }

    switchDsp.isChecked     = prefs.getBoolean("setting_use_dsp", false)
    switchHrtf.isChecked    = prefs.getBoolean("setting_hrtf", true)
    switchShaders.isChecked = prefs.getBoolean("setting_cache_shaders", true)
    switchFpu.isChecked     = prefs.getBoolean("setting_hard_fpu", true)

    val audioDriver = prefs.getString("setting_audio_driver", "openslES") ?: "openslES"
    when (audioDriver) {
      "aaudio"  -> toggleAudioDriver.check(R.id.btn_audio_aaudio)
      "dummy"   -> toggleAudioDriver.check(R.id.btn_audio_disabled)
      else      -> toggleAudioDriver.check(R.id.btn_audio_opensles)
    }

    btnSave.setOnClickListener {
      val selectedDisplayMode = when (toggleDisplayMode.checkedButtonId) {
        R.id.btn_display_4_3  -> 1
        R.id.btn_display_16_9 -> 2
        else                   -> 0
      }
      val selectedScale = when (toggleScale.checkedButtonId) {
        R.id.btn_scale_2x -> 2
        R.id.btn_scale_3x -> 3
        else              -> 1
      }
      val selectedThread = when (toggleThread.checkedButtonId) {
        R.id.btn_thread_single -> "single"
        else                   -> "multi"
      }
      val selectedAudioDriver = when (toggleAudioDriver.checkedButtonId) {
        R.id.btn_audio_aaudio    -> "aaudio"
        R.id.btn_audio_disabled  -> "dummy"
        else                     -> "openslES"
      }

      prefs.edit()
        .putInt("setting_display_mode", selectedDisplayMode)
        .putInt("setting_surface_scale", selectedScale)
        .putString("setting_tcg_thread", selectedThread)
        .putBoolean("setting_use_dsp", switchDsp.isChecked)
        .putBoolean("setting_hrtf", switchHrtf.isChecked)
        .putBoolean("setting_cache_shaders", switchShaders.isChecked)
        .putBoolean("setting_hard_fpu", switchFpu.isChecked)
        .putString("setting_audio_driver", selectedAudioDriver)
        .apply()

      Toast.makeText(this, R.string.settings_saved, Toast.LENGTH_SHORT).show()
      finish()
    }
  }
}
