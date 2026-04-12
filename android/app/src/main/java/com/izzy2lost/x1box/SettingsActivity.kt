package com.izzy2lost.x1box

import android.content.Context
import android.content.Intent
import android.net.Uri
import android.os.Bundle
import android.provider.OpenableColumns
import android.text.format.Formatter
import android.view.View
import android.widget.ArrayAdapter
import android.widget.AutoCompleteTextView
import android.widget.LinearLayout
import android.widget.TextView
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.documentfile.provider.DocumentFile
import com.google.android.material.button.MaterialButton
import com.google.android.material.button.MaterialButtonToggleGroup
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import com.google.android.material.materialswitch.MaterialSwitch
import com.google.android.material.textfield.TextInputLayout
import java.io.BufferedOutputStream
import java.io.BufferedInputStream
import java.io.File
import java.io.FileOutputStream
import java.io.IOException
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.zip.Deflater
import java.util.zip.ZipEntry
import java.util.zip.ZipInputStream
import java.util.zip.ZipOutputStream

class SettingsActivity : AppCompatActivity() {
  companion object {
    private const val PREFS_NAME = "x1box_prefs"
    private const val PREF_ADVANCED_EXPERIMENTAL_EXPANDED = "settings_advanced_experimental_expanded"
    private const val PREF_HRTF = "setting_hrtf"
    private const val PREF_HRTF_DEFAULT_OFF_MIGRATED = "setting_hrtf_default_off_migrated_v1"
    private const val PREF_SETTINGS_MIGRATED_V2 = "settings_migrated_v2"
    private const val PREF_INSIGNIA_SETUP_URI = "setting_insignia_setup_assistant_uri"
    private const val PREF_INSIGNIA_SETUP_NAME = "setting_insignia_setup_assistant_name"
    private const val INSIGNIA_SIGN_UP_URL = "https://insignia.live/"
    private const val INSIGNIA_GUIDE_URL = "https://insignia.live/guide/connect"
    private const val MANAGED_FILES_ARCHIVE_PREFIX = "x1box-files-"
    private val MANAGED_EMULATOR_FILE_ORDER = listOf(
      "mcpx.bin",
      "flash.bin",
      "eeprom.bin",
      "hdd.img",
      "xemu.toml",
    )
    private val MANAGED_EMULATOR_FILE_NAMES = MANAGED_EMULATOR_FILE_ORDER.toSet()
  }

  private val prefs by lazy { getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE) }

  private data class EepromLanguageOption(
    val value: XboxEepromEditor.Language,
    val labelRes: Int,
  )

  private data class EepromVideoOption(
    val value: XboxEepromEditor.VideoStandard,
    val labelRes: Int,
  )

  private data class EepromAspectRatioOption(
    val value: XboxEepromEditor.AspectRatio,
    val labelRes: Int,
  )

  private data class EepromRefreshRateOption(
    val value: XboxEepromEditor.RefreshRate,
    val labelRes: Int,
  )

  private data class UiOrientationOption(
    val value: OrientationPreferences.UiOrientation,
    val labelRes: Int,
  )

  private data class GameOrientationOption(
    val value: OrientationPreferences.GameOrientation,
    val labelRes: Int,
  )

  private data class CacheClearResult(
    val deletedEntries: Int,
    val hadFailures: Boolean,
  )


  private data class DashboardImportPlan(
    val hddFile: File,
    val workingDir: File,
    val sourceDir: File,
    val backupDir: File,
    val summary: String,
    val bootNote: String?,
    val bootAliasCreated: Boolean,
    val retailBootReady: Boolean,
  )

  private data class DashboardBootPreparation(
    val note: String?,
    val aliasCreated: Boolean,
    val retailBootReady: Boolean,
  )

  private data class InsigniaStatusSnapshot(
    val hasLocalHdd: Boolean,
    val hasEeprom: Boolean,
    val dashboardStatus: XboxInsigniaHelper.DashboardStatus?,
    val dashboardError: String?,
    val setupAssistantName: String?,
    val setupAssistantReady: Boolean,
  )

  private val eepromLanguageOptions = listOf(
    EepromLanguageOption(XboxEepromEditor.Language.ENGLISH, R.string.settings_eeprom_language_english),
    EepromLanguageOption(XboxEepromEditor.Language.JAPANESE, R.string.settings_eeprom_language_japanese),
    EepromLanguageOption(XboxEepromEditor.Language.GERMAN, R.string.settings_eeprom_language_german),
    EepromLanguageOption(XboxEepromEditor.Language.FRENCH, R.string.settings_eeprom_language_french),
    EepromLanguageOption(XboxEepromEditor.Language.SPANISH, R.string.settings_eeprom_language_spanish),
    EepromLanguageOption(XboxEepromEditor.Language.ITALIAN, R.string.settings_eeprom_language_italian),
    EepromLanguageOption(XboxEepromEditor.Language.KOREAN, R.string.settings_eeprom_language_korean),
    EepromLanguageOption(XboxEepromEditor.Language.CHINESE, R.string.settings_eeprom_language_chinese),
    EepromLanguageOption(XboxEepromEditor.Language.PORTUGUESE, R.string.settings_eeprom_language_portuguese),
  )

  private val eepromVideoOptions = listOf(
    EepromVideoOption(XboxEepromEditor.VideoStandard.NTSC_M, R.string.settings_eeprom_video_standard_ntsc_m),
    EepromVideoOption(XboxEepromEditor.VideoStandard.NTSC_J, R.string.settings_eeprom_video_standard_ntsc_j),
    EepromVideoOption(XboxEepromEditor.VideoStandard.PAL_I, R.string.settings_eeprom_video_standard_pal_i),
    EepromVideoOption(XboxEepromEditor.VideoStandard.PAL_M, R.string.settings_eeprom_video_standard_pal_m),
  )

  private val eepromAspectRatioOptions = listOf(
    EepromAspectRatioOption(XboxEepromEditor.AspectRatio.NORMAL, R.string.settings_eeprom_aspect_ratio_normal),
    EepromAspectRatioOption(XboxEepromEditor.AspectRatio.WIDESCREEN, R.string.settings_eeprom_aspect_ratio_widescreen),
    EepromAspectRatioOption(XboxEepromEditor.AspectRatio.LETTERBOX, R.string.settings_eeprom_aspect_ratio_letterbox),
  )

  private val eepromRefreshRateOptions = listOf(
    EepromRefreshRateOption(XboxEepromEditor.RefreshRate.DEFAULT, R.string.settings_eeprom_refresh_rate_default),
    EepromRefreshRateOption(XboxEepromEditor.RefreshRate.HZ_60, R.string.settings_eeprom_refresh_rate_60),
    EepromRefreshRateOption(XboxEepromEditor.RefreshRate.HZ_50, R.string.settings_eeprom_refresh_rate_50),
  )

  private val uiOrientationOptions = listOf(
    UiOrientationOption(OrientationPreferences.UiOrientation.FOLLOW_DEVICE, R.string.settings_orientation_follow_device),
    UiOrientationOption(OrientationPreferences.UiOrientation.PORTRAIT, R.string.settings_orientation_portrait),
    UiOrientationOption(OrientationPreferences.UiOrientation.REVERSE_PORTRAIT, R.string.settings_orientation_reverse_portrait),
    UiOrientationOption(OrientationPreferences.UiOrientation.LANDSCAPE, R.string.settings_orientation_landscape),
    UiOrientationOption(OrientationPreferences.UiOrientation.REVERSE_LANDSCAPE, R.string.settings_orientation_reverse_landscape),
  )

  private val gameOrientationOptions = listOf(
    GameOrientationOption(OrientationPreferences.GameOrientation.FOLLOW_DEVICE, R.string.settings_orientation_follow_device),
    GameOrientationOption(OrientationPreferences.GameOrientation.LANDSCAPE, R.string.settings_orientation_landscape),
    GameOrientationOption(OrientationPreferences.GameOrientation.REVERSE_LANDSCAPE, R.string.settings_orientation_reverse_landscape),
  )

  private var isInitializingHdd = false
  private var isImportingDashboard = false
  private var isImportingEmulatorFiles = false
  private var isExportingEmulatorFiles = false
  private var isPreparingInsignia = false

  private lateinit var btnImportEmulatorFiles: MaterialButton
  private lateinit var btnExportEmulatorFiles: MaterialButton
  private lateinit var switchDebugLogs: MaterialSwitch
  private lateinit var switchNetworkEnable: MaterialSwitch
  private lateinit var driverStatusText: TextView
  private lateinit var gpuNotSupportedText: TextView
  private lateinit var btnInstallDriver: MaterialButton
  private lateinit var btnSelectDriver: MaterialButton
  private lateinit var btnResetDriver: MaterialButton
  private lateinit var tvInsigniaStatus: TextView
  private lateinit var tvEepromStatus: TextView
  private lateinit var tvHddToolsStatus: TextView
  private lateinit var btnToggleAdvancedExperimental: MaterialButton
  private lateinit var btnInsigniaGuide: MaterialButton
  private lateinit var btnInsigniaSignUp: MaterialButton
  private lateinit var btnPrepareInsignia: MaterialButton
  private lateinit var btnRegisterInsignia: MaterialButton
  private lateinit var btnImportDashboard: MaterialButton
  private lateinit var layoutAdvancedExperimentalContent: LinearLayout
  private lateinit var dropdownUiOrientation: AutoCompleteTextView
  private lateinit var dropdownGameOrientation: AutoCompleteTextView
  private lateinit var inputEepromLanguage: TextInputLayout
  private lateinit var inputEepromVideoStandard: TextInputLayout
  private lateinit var inputEepromAspectRatio: TextInputLayout
  private lateinit var inputEepromRefreshRate: TextInputLayout
  private lateinit var dropdownEepromLanguage: AutoCompleteTextView
  private lateinit var dropdownEepromVideoStandard: AutoCompleteTextView
  private lateinit var dropdownEepromAspectRatio: AutoCompleteTextView
  private lateinit var dropdownEepromRefreshRate: AutoCompleteTextView
  private lateinit var switchEeprom480p: MaterialSwitch
  private lateinit var switchEeprom720p: MaterialSwitch
  private lateinit var switchEeprom1080i: MaterialSwitch

  private var selectedEepromLanguage = XboxEepromEditor.Language.ENGLISH
  private var selectedEepromVideoStandard = XboxEepromEditor.VideoStandard.NTSC_M
  private var selectedEepromAspectRatio = XboxEepromEditor.AspectRatio.NORMAL
  private var selectedEepromRefreshRate = XboxEepromEditor.RefreshRate.DEFAULT
  private var selectedUiOrientation = OrientationPreferences.UiOrientation.FOLLOW_DEVICE
  private var selectedGameOrientation = OrientationPreferences.GameOrientation.FOLLOW_DEVICE
  private var eepromEditable = false
  private var eepromMissing = false
  private var eepromError = false

  private val pickDriverZip =
    registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri: Uri? ->
      if (uri != null) installDriverFromUri(uri)
    }

  private val pickDashboardZip =
    registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri: Uri? ->
      uri ?: return@registerForActivityResult
      persistUriPermission(uri)
      if (!isZipSelection(uri)) {
        Toast.makeText(this, R.string.settings_dashboard_import_pick_zip_error, Toast.LENGTH_LONG).show()
        return@registerForActivityResult
      }
      prepareDashboardImportFromZip(uri)
    }

  private val pickDashboardFolder =
    registerForActivityResult(ActivityResultContracts.OpenDocumentTree()) { uri: Uri? ->
      uri ?: return@registerForActivityResult
      persistUriPermission(uri)
      prepareDashboardImportFromFolder(uri)
    }

  private val pickInsigniaSetupAssistant =
    registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri: Uri? ->
      uri ?: return@registerForActivityResult
      persistUriPermission(uri)

      val name = getFileName(uri)
        ?: uri.lastPathSegment
        ?: getString(R.string.settings_insignia_setup_source_unknown)
      prefs.edit()
        .putString(PREF_INSIGNIA_SETUP_URI, uri.toString())
        .putString(PREF_INSIGNIA_SETUP_NAME, name)
        .apply()

      refreshInsigniaStatus()
      launchInsigniaSetupAssistant(uri)
    }

  private val exportDebugLogDocument =
    registerForActivityResult(ActivityResultContracts.CreateDocument("text/plain")) { uri: Uri? ->
      uri ?: return@registerForActivityResult
      exportDebugLog(uri)
    }

  private val importEmulatorFilesZip =
    registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri: Uri? ->
      uri ?: return@registerForActivityResult
      persistUriPermission(uri)
      if (!isZipSelection(uri)) {
        Toast.makeText(this, R.string.settings_import_emulator_files_pick_zip_error, Toast.LENGTH_LONG).show()
        return@registerForActivityResult
      }
      importManagedFilesFromZip(uri)
    }

  private val importEmulatorFilesDocuments =
    registerForActivityResult(ActivityResultContracts.OpenMultipleDocuments()) { uris: List<Uri> ->
      if (uris.isEmpty()) {
        return@registerForActivityResult
      }
      uris.forEach(::persistUriPermission)
      importManagedFilesFromDocuments(uris)
    }

  private val exportEmulatorFilesDocument =
    registerForActivityResult(ActivityResultContracts.CreateDocument("application/zip")) { uri: Uri? ->
      uri ?: return@registerForActivityResult
      exportManagedFiles(uri)
    }

  override fun onCreate(savedInstanceState: Bundle?) {
    super.onCreate(savedInstanceState)
    OrientationLocker(this).enable()
    DebugLog.initialize(this)
    applyHrtfDefaultOffMigration()
    applySettingsMigrationV2()
    setContentView(R.layout.activity_settings)
    EdgeToEdgeHelper.enable(this)
    EdgeToEdgeHelper.applySystemBarPadding(findViewById(R.id.settings_scroll))

    val toggleGraphicsApi = findViewById<MaterialButtonToggleGroup>(R.id.toggle_graphics_api)
    val toggleFiltering   = findViewById<MaterialButtonToggleGroup>(R.id.toggle_filtering)
    val toggleScale       = findViewById<MaterialButtonToggleGroup>(R.id.toggle_resolution_scale)
    val btn1x             = findViewById<MaterialButton>(R.id.btn_scale_1x)
    val btn2x             = findViewById<MaterialButton>(R.id.btn_scale_2x)
    val btn3x             = findViewById<MaterialButton>(R.id.btn_scale_3x)
    val toggleDisplayMode = findViewById<MaterialButtonToggleGroup>(R.id.toggle_display_mode)
    val toggleSystemMemory = findViewById<MaterialButtonToggleGroup>(R.id.toggle_system_memory)
    val toggleThread      = findViewById<MaterialButtonToggleGroup>(R.id.toggle_tcg_thread)
    val btnMulti          = findViewById<MaterialButton>(R.id.btn_thread_multi)
    val btnSingle         = findViewById<MaterialButton>(R.id.btn_thread_single)
    val switchDsp         = findViewById<MaterialSwitch>(R.id.switch_use_dsp)
    val switchHrtf        = findViewById<MaterialSwitch>(R.id.switch_hrtf)
    val switchShaders     = findViewById<MaterialSwitch>(R.id.switch_cache_shaders)
    val switchFpu         = findViewById<MaterialSwitch>(R.id.switch_hard_fpu)
    val switchVsync       = findViewById<MaterialSwitch>(R.id.switch_vsync)
    val switchSkipBootAnim = findViewById<MaterialSwitch>(R.id.switch_skip_boot_anim)
    val switchDrawReorder  = findViewById<MaterialSwitch>(R.id.switch_draw_reorder)
    val switchDrawMerge    = findViewById<MaterialSwitch>(R.id.switch_draw_merge)
    val switchAsyncCompile = findViewById<MaterialSwitch>(R.id.switch_async_compile)
    val switchShowFps      = findViewById<MaterialSwitch>(R.id.switch_show_fps)
    switchDebugLogs      = findViewById(R.id.switch_debug_logs)
    val toggleAudioDriver = findViewById<MaterialButtonToggleGroup>(R.id.toggle_audio_driver)
    val btnSave           = findViewById<MaterialButton>(R.id.btn_settings_save)
    val btnRedoSetup      = findViewById<MaterialButton>(R.id.btn_redo_setup_wizard)
    val btnClearCache     = findViewById<MaterialButton>(R.id.btn_clear_system_cache)
    val btnExportDebugLog = findViewById<MaterialButton>(R.id.btn_export_debug_log)
    val btnClearDebugLog  = findViewById<MaterialButton>(R.id.btn_clear_debug_log)
    btnImportEmulatorFiles = findViewById(R.id.btn_import_emulator_files)
    btnExportEmulatorFiles = findViewById(R.id.btn_export_emulator_files)
    val btnInitializeRetailHdd = findViewById<MaterialButton>(R.id.btn_initialize_retail_hdd)
    switchNetworkEnable  = findViewById(R.id.switch_network_enable)
    tvInsigniaStatus     = findViewById(R.id.tv_insignia_status)
    btnToggleAdvancedExperimental = findViewById(R.id.btn_toggle_advanced_experimental)
    btnInsigniaGuide     = findViewById(R.id.btn_insignia_guide)
    btnInsigniaSignUp    = findViewById(R.id.btn_insignia_sign_up)
    btnPrepareInsignia   = findViewById(R.id.btn_prepare_insignia)
    btnRegisterInsignia  = findViewById(R.id.btn_register_insignia)
    btnImportDashboard   = findViewById(R.id.btn_import_dashboard)
    layoutAdvancedExperimentalContent = findViewById(R.id.layout_advanced_experimental_content)
    dropdownUiOrientation = findViewById(R.id.dropdown_app_orientation)
    dropdownGameOrientation = findViewById(R.id.dropdown_game_orientation)
    driverStatusText      = findViewById(R.id.settings_gpu_driver_status)
    gpuNotSupportedText   = findViewById(R.id.settings_gpu_not_supported)
    btnInstallDriver      = findViewById(R.id.btn_install_driver)
    btnSelectDriver       = findViewById(R.id.btn_select_driver)
    btnResetDriver        = findViewById(R.id.btn_reset_driver)
    tvEepromStatus        = findViewById(R.id.tv_eeprom_status)
    tvHddToolsStatus      = findViewById(R.id.tv_hdd_tools_status)
    inputEepromLanguage   = findViewById(R.id.input_eeprom_language)
    inputEepromVideoStandard = findViewById(R.id.input_eeprom_video_standard)
    inputEepromAspectRatio = findViewById(R.id.input_eeprom_aspect_ratio)
    inputEepromRefreshRate = findViewById(R.id.input_eeprom_refresh_rate)
    dropdownEepromLanguage = findViewById(R.id.dropdown_eeprom_language)
    dropdownEepromVideoStandard = findViewById(R.id.dropdown_eeprom_video_standard)
    dropdownEepromAspectRatio = findViewById(R.id.dropdown_eeprom_aspect_ratio)
    dropdownEepromRefreshRate = findViewById(R.id.dropdown_eeprom_refresh_rate)
    switchEeprom480p = findViewById(R.id.switch_eeprom_480p)
    switchEeprom720p = findViewById(R.id.switch_eeprom_720p)
    switchEeprom1080i = findViewById(R.id.switch_eeprom_1080i)

    updateEmulatorFilesActionState()

    // Load current values
    val renderer = prefs.getString("setting_renderer", "vulkan") ?: "vulkan"
    if (renderer == "opengl") {
      toggleGraphicsApi.check(R.id.btn_renderer_opengl)
    } else {
      toggleGraphicsApi.check(R.id.btn_renderer_vulkan)
    }

    val filtering = prefs.getString("setting_filtering", "linear") ?: "linear"
    if (filtering == "nearest") {
      toggleFiltering.check(R.id.btn_filtering_nearest)
    } else {
      toggleFiltering.check(R.id.btn_filtering_linear)
    }

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

    setupOrientationControls()
    setUiOrientationSelection(OrientationPreferences.getUiOrientation(this))
    setGameOrientationSelection(OrientationPreferences.getGameOrientation(this))

    if (prefs.getInt("setting_frame_rate_limit", 60) != 60) {
      prefs.edit().putInt("setting_frame_rate_limit", 60).apply()
    }

    val systemMemoryMiB = prefs.getInt("setting_system_memory_mib", 64)
    when (systemMemoryMiB) {
      128  -> toggleSystemMemory.check(R.id.btn_memory_128)
      else -> toggleSystemMemory.check(R.id.btn_memory_64)
    }

    GpuDriverHelper.init(this)
    val supportsCustomDriver = GpuDriverHelper.supportsCustomDriverLoading()
    if (!supportsCustomDriver) {
      gpuNotSupportedText.visibility = View.VISIBLE
      btnInstallDriver.isEnabled = false
      btnSelectDriver.isEnabled = false
      btnResetDriver.isEnabled = false
    }
    refreshDriverStatus()

    btnInstallDriver.setOnClickListener {
      pickDriverZip.launch(arrayOf("application/zip", "application/octet-stream"))
    }
    btnSelectDriver.setOnClickListener { showDriverSelectionDialog() }
    btnResetDriver.setOnClickListener { confirmResetDriver() }

    val tcgThread = prefs.getString("setting_tcg_thread", "multi") ?: "multi"
    if (tcgThread == "single") {
      toggleThread.check(R.id.btn_thread_single)
    } else {
      toggleThread.check(R.id.btn_thread_multi)
    }

    switchDsp.isChecked     = prefs.getBoolean("setting_use_dsp", false)
    switchHrtf.isChecked    = prefs.getBoolean(PREF_HRTF, false)
    switchShaders.isChecked = prefs.getBoolean("setting_cache_shaders", true)
    switchFpu.isChecked     = prefs.getBoolean("setting_hard_fpu", true)
    switchVsync.isChecked   = prefs.getBoolean("setting_vsync", false)
    switchSkipBootAnim.isChecked =
      prefs.getBoolean("setting_skip_boot_anim", true)
    switchDrawReorder.isChecked  = prefs.getBoolean("draw_reorder", true)
    switchDrawMerge.isChecked    = prefs.getBoolean("draw_merge", true)
    switchAsyncCompile.isChecked = prefs.getBoolean("async_compile", false)
    switchShowFps.isChecked      = prefs.getBoolean("show_fps", false)
    switchDebugLogs.isChecked =
      prefs.getBoolean(DebugLog.PREF_ENABLED, false)
    switchNetworkEnable.isChecked =
      prefs.getBoolean("setting_network_enable", false)

    val audioDriver = prefs.getString("setting_audio_driver", "openslES") ?: "openslES"
    when (audioDriver) {
      "aaudio"  -> toggleAudioDriver.check(R.id.btn_audio_aaudio)
      "dummy"   -> toggleAudioDriver.check(R.id.btn_audio_disabled)
      else      -> toggleAudioDriver.check(R.id.btn_audio_opensles)
    }

    btnRedoSetup.setOnClickListener {
      prefs.edit().putBoolean("setup_complete", false).apply()
      startActivity(Intent(this, SetupWizardActivity::class.java))
      finish()
    }

    setupEepromEditor()
    refreshInsigniaStatus()
    refreshHddToolsPreview(btnInitializeRetailHdd)
    setAdvancedExperimentalExpanded(
      prefs.getBoolean(PREF_ADVANCED_EXPERIMENTAL_EXPANDED, false)
    )
    btnToggleAdvancedExperimental.setOnClickListener {
      setAdvancedExperimentalExpanded(layoutAdvancedExperimentalContent.visibility != View.VISIBLE)
    }
    btnInsigniaGuide.setOnClickListener {
      openExternalLink(INSIGNIA_GUIDE_URL)
    }
    btnInsigniaSignUp.setOnClickListener {
      openExternalLink(INSIGNIA_SIGN_UP_URL)
    }
    btnPrepareInsignia.setOnClickListener {
      prepareInsigniaNetworking()
    }
    btnRegisterInsignia.setOnClickListener {
      showInsigniaSetupAssistantPrompt()
    }
    btnImportDashboard.setOnClickListener {
      showDashboardImportSourcePicker()
    }

    fun persistSettings(): Pair<Int, Int> {
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
      val selectedSystemMemoryMiB = when (toggleSystemMemory.checkedButtonId) {
        R.id.btn_memory_128 -> 128
        else                -> 64
      }
      val selectedAudioDriver = when (toggleAudioDriver.checkedButtonId) {
        R.id.btn_audio_aaudio    -> "aaudio"
        R.id.btn_audio_disabled  -> "dummy"
        else                     -> "openslES"
      }
      val selectedRenderer = when (toggleGraphicsApi.checkedButtonId) {
        R.id.btn_renderer_opengl -> "opengl"
        else                     -> "vulkan"
      }
      val selectedFiltering = when (toggleFiltering.checkedButtonId) {
        R.id.btn_filtering_nearest -> "nearest"
        else                       -> "linear"
      }
      val wasDebugLoggingEnabled = prefs.getBoolean(DebugLog.PREF_ENABLED, false)
      val enableDebugLogs = switchDebugLogs.isChecked

      val edit = prefs.edit()
        .putInt("setting_display_mode", selectedDisplayMode)
        .putInt("setting_surface_scale", selectedScale)
        .putInt("setting_frame_rate_limit", 60)
        .putInt("setting_system_memory_mib", selectedSystemMemoryMiB)
        .putString(OrientationPreferences.PREF_UI_ORIENTATION, selectedUiOrientation.prefValue)
        .putString(OrientationPreferences.PREF_GAME_ORIENTATION, selectedGameOrientation.prefValue)
        .putString("setting_tcg_thread", selectedThread)
        .putBoolean("setting_use_dsp", switchDsp.isChecked)
        .putBoolean(PREF_HRTF, switchHrtf.isChecked)
        .putBoolean("setting_cache_shaders", switchShaders.isChecked)
        .putBoolean("setting_hard_fpu", switchFpu.isChecked)
        .putBoolean("setting_vsync", switchVsync.isChecked)
        .putBoolean("setting_skip_boot_anim", switchSkipBootAnim.isChecked)
        .putBoolean("draw_reorder", switchDrawReorder.isChecked)
        .putBoolean("draw_merge", switchDrawMerge.isChecked)
        .putBoolean("async_compile", switchAsyncCompile.isChecked)
        .putBoolean("show_fps", switchShowFps.isChecked)
        .putBoolean(DebugLog.PREF_ENABLED, enableDebugLogs)
        .putBoolean("setting_network_enable", switchNetworkEnable.isChecked)
        .putString("setting_audio_driver", selectedAudioDriver)
        .putString("setting_filtering", selectedFiltering)
        .putString("setting_renderer", selectedRenderer)

      edit.apply()
      DebugLog.setEnabled(
        context = this@SettingsActivity,
        value = enableDebugLogs,
        resetLogs = enableDebugLogs != wasDebugLoggingEnabled
      )

      return applyEepromEdits()
    }

    btnClearCache.setOnClickListener {
      showClearCacheConfirmation()
    }
    btnExportDebugLog.setOnClickListener {
      if (!DebugLog.hasAnyLog(this)) {
        Toast.makeText(this, R.string.settings_export_debug_log_empty, Toast.LENGTH_LONG).show()
        return@setOnClickListener
      }
      exportDebugLogDocument.launch(DebugLog.exportDefaultFileName())
    }
    btnClearDebugLog.setOnClickListener {
      if (!DebugLog.hasAnyLog(this)) {
        Toast.makeText(this, R.string.settings_clear_debug_log_empty, Toast.LENGTH_SHORT).show()
        return@setOnClickListener
      }
      DebugLog.resetLogs(this)
      Toast.makeText(this, R.string.settings_clear_debug_log_success, Toast.LENGTH_SHORT).show()
    }
    btnImportEmulatorFiles.setOnClickListener {
      if (isImportingEmulatorFiles || isExportingEmulatorFiles) {
        return@setOnClickListener
      }
      showManagedFilesImportWarning()
    }
    btnExportEmulatorFiles.setOnClickListener {
      if (isImportingEmulatorFiles || isExportingEmulatorFiles) {
        return@setOnClickListener
      }
      if (!hasAnyManagedFilesToExport()) {
        Toast.makeText(this, R.string.settings_export_emulator_files_empty, Toast.LENGTH_LONG).show()
        return@setOnClickListener
      }
      exportEmulatorFilesDocument.launch(defaultManagedFilesArchiveName())
    }

    btnInitializeRetailHdd.setOnClickListener {
      showInitializeHddLayoutPicker(btnInitializeRetailHdd)
    }

    btnSave.setOnClickListener {
      try {
        val toastResult = persistSettings()
        Toast.makeText(this, toastResult.first, toastResult.second).show()
        finish()
      } catch (error: Exception) {
        Toast.makeText(
          this,
          "Failed to save settings: ${error.message ?: error.javaClass.simpleName}",
          Toast.LENGTH_LONG
        ).show()
      }
    }
  }

  private fun applyHrtfDefaultOffMigration() {
    if (prefs.getBoolean(PREF_HRTF_DEFAULT_OFF_MIGRATED, false)) {
      return
    }

    prefs.edit()
      .putBoolean(PREF_HRTF, false)
      .putBoolean(PREF_HRTF_DEFAULT_OFF_MIGRATED, true)
      .apply()
  }

  private fun applySettingsMigrationV2() {
    if (prefs.getBoolean(PREF_SETTINGS_MIGRATED_V2, false)) {
      return
    }

    val editor = prefs.edit()

    if (!prefs.contains("setting_skip_boot_anim")) editor.putBoolean("setting_skip_boot_anim", true)
    if (!prefs.contains("draw_reorder")) editor.putBoolean("draw_reorder", true)
    if (!prefs.contains("draw_merge")) editor.putBoolean("draw_merge", true)
    if (!prefs.contains("async_compile")) editor.putBoolean("async_compile", false)
    if (!prefs.contains("setting_cache_shaders")) editor.putBoolean("setting_cache_shaders", true)
    if (!prefs.contains("setting_hard_fpu")) editor.putBoolean("setting_hard_fpu", true)
    if (!prefs.contains("setting_vsync")) editor.putBoolean("setting_vsync", false)
    if (!prefs.contains("setting_use_dsp")) editor.putBoolean("setting_use_dsp", false)
    if (!prefs.contains("setting_hrtf")) editor.putBoolean("setting_hrtf", false)
    if (!prefs.contains("setting_network_enable")) editor.putBoolean("setting_network_enable", false)
    if (!prefs.contains("setting_renderer")) editor.putString("setting_renderer", "vulkan")
    if (!prefs.contains("setting_filtering")) editor.putString("setting_filtering", "nearest")
    if (!prefs.contains("setting_tcg_thread")) editor.putString("setting_tcg_thread", "multi")
    if (!prefs.contains("setting_audio_driver")) editor.putString("setting_audio_driver", "openslES")
    if (!prefs.contains("setting_surface_scale")) editor.putInt("setting_surface_scale", 1)
    if (!prefs.contains("setting_display_mode")) editor.putInt("setting_display_mode", 0)
    if (!prefs.contains("setting_system_memory_mib")) editor.putInt("setting_system_memory_mib", 64)
    if (!prefs.contains("tcg_tb_size")) editor.putInt("tcg_tb_size", 256)

    editor.putBoolean(PREF_SETTINGS_MIGRATED_V2, true).apply()
  }

  private fun installDriverFromUri(uri: Uri) {
    Thread {
      val success = GpuDriverHelper.installDriverFromUri(this, uri)
      runOnUiThread {
        if (success) {
          Toast.makeText(this, getString(R.string.settings_gpu_driver_installed), Toast.LENGTH_SHORT).show()
          refreshDriverStatus()
        } else {
          Toast.makeText(this, getString(R.string.settings_gpu_driver_install_failed), Toast.LENGTH_SHORT).show()
        }
      }
    }.start()
  }

  private fun showDriverSelectionDialog() {
    val drivers = GpuDriverHelper.getAvailableDrivers()
    if (drivers.isEmpty()) {
      Toast.makeText(this, getString(R.string.settings_gpu_driver_none_available), Toast.LENGTH_SHORT).show()
      return
    }
    val labels = drivers.map { driver ->
      buildString {
        append(driver.name ?: "Unknown")
        if (!driver.description.isNullOrBlank()) { append("\n"); append(driver.description) }
        if (!driver.author.isNullOrBlank()) { append("\nby "); append(driver.author) }
      }
    }.toTypedArray()
    MaterialAlertDialogBuilder(this)
      .setTitle(R.string.settings_gpu_driver_select_title)
      .setItems(labels) { _, which ->
        val selected = drivers[which]
        if (selected.path != null) {
          val zipFile = File(selected.path)
          val success = GpuDriverHelper.installDriver(zipFile)
          if (success) {
            Toast.makeText(this, getString(R.string.settings_gpu_driver_installed), Toast.LENGTH_SHORT).show()
            refreshDriverStatus()
          } else {
            Toast.makeText(this, getString(R.string.settings_gpu_driver_install_failed), Toast.LENGTH_SHORT).show()
          }
        }
      }
      .setNegativeButton(android.R.string.cancel, null)
      .show()
  }

  private fun confirmResetDriver() {
    MaterialAlertDialogBuilder(this)
      .setTitle(R.string.settings_gpu_driver_reset_title)
      .setMessage(R.string.settings_gpu_driver_reset_message)
      .setPositiveButton(R.string.settings_gpu_driver_reset) { _, _ ->
        GpuDriverHelper.installDefaultDriver()
        Toast.makeText(this, getString(R.string.settings_gpu_driver_reset_done), Toast.LENGTH_SHORT).show()
        refreshDriverStatus()
      }
      .setNegativeButton(android.R.string.cancel, null)
      .show()
  }

  private fun refreshDriverStatus() {
    val name = GpuDriverHelper.getInstalledDriverName()
    driverStatusText.text = if (name != null) {
      getString(R.string.settings_gpu_driver_active, name)
    } else {
      getString(R.string.settings_gpu_driver_system)
    }
  }

  private fun updateEmulatorFilesActionState() {
    if (!::btnImportEmulatorFiles.isInitialized || !::btnExportEmulatorFiles.isInitialized) {
      return
    }
    val enabled = !isImportingEmulatorFiles && !isExportingEmulatorFiles
    btnImportEmulatorFiles.isEnabled = enabled
    btnExportEmulatorFiles.isEnabled = enabled
  }

  private fun showManagedFilesImportWarning() {
    MaterialAlertDialogBuilder(this, R.style.ThemeOverlay_Xemu_RoundedDialog)
      .setTitle(R.string.settings_import_emulator_files_action)
      .setMessage(R.string.settings_import_emulator_files_message)
      .setPositiveButton(R.string.settings_import_emulator_files_continue) { _, _ ->
        showManagedFilesImportSourcePicker()
      }
      .setNegativeButton(android.R.string.cancel, null)
      .show()
  }

  private fun showManagedFilesImportSourcePicker() {
    val labels = arrayOf(
      getString(R.string.settings_import_emulator_files_source_zip),
      getString(R.string.settings_import_emulator_files_source_files),
    )
    val dp = resources.displayMetrics.density
    lateinit var importDialog: androidx.appcompat.app.AlertDialog

    val buttonList = LinearLayout(this).apply {
      orientation = LinearLayout.VERTICAL
      setPadding((20 * dp).toInt(), (12 * dp).toInt(), (20 * dp).toInt(), 0)
      labels.forEachIndexed { index, label ->
        addView(
          MaterialButton(
            this@SettingsActivity,
            null,
            com.google.android.material.R.attr.materialButtonOutlinedStyle
          ).apply {
            text = label
            layoutParams = LinearLayout.LayoutParams(
              LinearLayout.LayoutParams.MATCH_PARENT,
              LinearLayout.LayoutParams.WRAP_CONTENT,
            ).also { lp ->
              lp.bottomMargin = (8 * dp).toInt()
            }
            setOnClickListener {
              importDialog.dismiss()
              when (index) {
                0 -> importEmulatorFilesZip.launch(arrayOf("application/zip", "application/octet-stream"))
                else -> importEmulatorFilesDocuments.launch(arrayOf("*/*"))
              }
            }
          }
        )
      }
    }

    importDialog = MaterialAlertDialogBuilder(this, R.style.ThemeOverlay_Xemu_RoundedDialog)
      .setTitle(R.string.settings_import_emulator_files_source_title)
      .setView(buttonList)
      .setNegativeButton(android.R.string.cancel, null)
      .create()
    importDialog.show()
  }

  private fun defaultManagedFilesArchiveName(): String {
    val stamp = SimpleDateFormat("yyyyMMdd-HHmmss", Locale.US).format(Date())
    return "$MANAGED_FILES_ARCHIVE_PREFIX$stamp.zip"
  }

  private fun hasAnyManagedFilesToExport(): Boolean {
    return MANAGED_EMULATOR_FILE_ORDER.any { resolveManagedExportSource(it)?.isFile == true }
  }

  private fun exportManagedFiles(uri: Uri) {
    if (isImportingEmulatorFiles || isExportingEmulatorFiles) {
      return
    }

    val filesToExport = resolveManagedFilesForExport()
    if (filesToExport.isEmpty()) {
      Toast.makeText(this, R.string.settings_export_emulator_files_empty, Toast.LENGTH_LONG).show()
      return
    }

    isExportingEmulatorFiles = true
    updateEmulatorFilesActionState()
    Toast.makeText(this, R.string.settings_export_emulator_files_working, Toast.LENGTH_SHORT).show()

    Thread {
      val result = runCatching {
        exportManagedFilesInternal(uri, filesToExport)
      }

      runOnUiThread {
        isExportingEmulatorFiles = false
        updateEmulatorFilesActionState()
        result.onSuccess { exportedNames ->
          Toast.makeText(
            this,
            getString(R.string.settings_export_emulator_files_success, exportedNames.size),
            Toast.LENGTH_LONG,
          ).show()
        }.onFailure { error ->
          Toast.makeText(
            this,
            getString(
              R.string.settings_export_emulator_files_failed,
              error.message ?: getString(R.string.settings_import_emulator_files_unknown_error),
            ),
            Toast.LENGTH_LONG,
          ).show()
        }
      }
    }.start()
  }

  private fun exportManagedFilesInternal(
    uri: Uri,
    filesToExport: List<Pair<String, File>>,
  ): List<String> {
    contentResolver.openOutputStream(uri, "w")?.use { rawOutput ->
      ZipOutputStream(BufferedOutputStream(rawOutput)).use { zip ->
        zip.setLevel(Deflater.NO_COMPRESSION)
        for ((name, file) in filesToExport) {
          val entry = ZipEntry(name)
          if (file.lastModified() > 0L) {
            entry.time = file.lastModified()
          }
          zip.putNextEntry(entry)
          file.inputStream().use { input ->
            input.copyTo(zip)
          }
          zip.closeEntry()
        }
        zip.finish()
      }
    } ?: throw IOException(getString(R.string.settings_export_emulator_files_open_failed))

    return filesToExport.map { it.first }
  }

  private fun resolveManagedFilesForExport(): List<Pair<String, File>> {
    return MANAGED_EMULATOR_FILE_ORDER.mapNotNull { fileName ->
      resolveManagedExportSource(fileName)
        ?.takeIf { it.isFile }
        ?.let { fileName to it }
    }
  }

  private fun importManagedFilesFromZip(uri: Uri) {
    if (isImportingEmulatorFiles || isExportingEmulatorFiles) {
      return
    }

    isImportingEmulatorFiles = true
    updateEmulatorFilesActionState()
    Toast.makeText(this, R.string.settings_import_emulator_files_working, Toast.LENGTH_SHORT).show()

    Thread {
      val result = runCatching {
        importManagedFilesFromZipInternal(uri)
      }

      runOnUiThread {
        isImportingEmulatorFiles = false
        updateEmulatorFilesActionState()
        result.onSuccess(::finishManagedFilesImport)
          .onFailure { error ->
            Toast.makeText(
              this,
              getString(
                R.string.settings_import_emulator_files_failed,
                error.message ?: getString(R.string.settings_import_emulator_files_unknown_error),
              ),
              Toast.LENGTH_LONG,
            ).show()
          }
      }
    }.start()
  }

  private fun importManagedFilesFromDocuments(uris: List<Uri>) {
    if (isImportingEmulatorFiles || isExportingEmulatorFiles) {
      return
    }

    isImportingEmulatorFiles = true
    updateEmulatorFilesActionState()
    Toast.makeText(this, R.string.settings_import_emulator_files_working, Toast.LENGTH_SHORT).show()

    Thread {
      val result = runCatching {
        importManagedFilesFromDocumentsInternal(uris)
      }

      runOnUiThread {
        isImportingEmulatorFiles = false
        updateEmulatorFilesActionState()
        result.onSuccess(::finishManagedFilesImport)
          .onFailure { error ->
            Toast.makeText(
              this,
              getString(
                R.string.settings_import_emulator_files_failed,
                error.message ?: getString(R.string.settings_import_emulator_files_unknown_error),
              ),
              Toast.LENGTH_LONG,
            ).show()
          }
      }
    }.start()
  }

  private fun importManagedFilesFromZipInternal(uri: Uri): List<String> {
    ensureManagedFilesRoot()
    val editor = prefs.edit()
    val importedNames = linkedSetOf<String>()

    contentResolver.openInputStream(uri)?.use { rawInput ->
      ZipInputStream(BufferedInputStream(rawInput)).use { zip ->
        while (true) {
          val entry = zip.nextEntry ?: break
          if (!entry.isDirectory) {
            val normalizedName = normalizeManagedFileName(entry.name)
            if (normalizedName != null && importedNames.add(normalizedName)) {
              val target = resolveManagedImportTarget(normalizedName)
              copyZipEntryToFile(zip, target)
            }
          }
          zip.closeEntry()
        }
      }
    } ?: throw IOException(getString(R.string.settings_import_emulator_files_open_failed))

    if (importedNames.isEmpty()) {
      throw IOException(getString(R.string.settings_import_emulator_files_zip_empty))
    }

    for (fileName in sortManagedFileNames(importedNames)) {
      applyImportedManagedFile(fileName, resolveManagedImportTarget(fileName), editor)
    }
    editor.apply()
    return sortManagedFileNames(importedNames)
  }

  private fun importManagedFilesFromDocumentsInternal(uris: List<Uri>): List<String> {
    ensureManagedFilesRoot()
    val selectedFiles = linkedMapOf<String, Uri>()

    for (uri in uris) {
      val rawName = getFileName(uri) ?: uri.lastPathSegment ?: continue
      val normalizedName = normalizeManagedFileName(rawName) ?: continue
      if (selectedFiles.containsKey(normalizedName)) {
        throw IOException(getString(R.string.settings_import_emulator_files_duplicate, normalizedName))
      }
      selectedFiles[normalizedName] = uri
    }

    if (selectedFiles.isEmpty()) {
      throw IOException(getString(R.string.settings_import_emulator_files_pick_files_error))
    }

    val editor = prefs.edit()
    for ((fileName, uri) in selectedFiles) {
      val target = resolveManagedImportTarget(fileName)
      copyUriToFile(uri, target)
    }
    for (fileName in sortManagedFileNames(selectedFiles.keys)) {
      applyImportedManagedFile(fileName, resolveManagedImportTarget(fileName), editor)
    }
    editor.apply()

    return sortManagedFileNames(selectedFiles.keys)
  }

  private fun finishManagedFilesImport(importedNames: List<String>) {
    val summary = importedNames.joinToString(", ")
    Toast.makeText(
      this,
      getString(R.string.settings_import_emulator_files_success, summary),
      Toast.LENGTH_LONG,
    ).show()
    recreate()
  }

  private fun copyZipEntryToFile(zip: ZipInputStream, target: File) {
    val parent = target.parentFile
    if (parent != null && !parent.exists() && !parent.mkdirs()) {
      throw IOException("Failed to prepare ${parent.absolutePath}.")
    }
    FileOutputStream(target).use { output ->
      zip.copyTo(output)
    }
  }

  private fun resolveManagedFilesRoot(): File {
    val base = getExternalFilesDir(null) ?: filesDir
    return File(base, "x1box")
  }

  private fun ensureManagedFilesRoot(): File {
    val dir = resolveManagedFilesRoot()
    if (!dir.exists() && !dir.mkdirs()) {
      throw IOException("Failed to prepare the emulator files folder.")
    }
    return dir
  }

  private fun resolveManagedImportTarget(fileName: String): File {
    return File(resolveManagedFilesRoot(), fileName)
  }

  private fun resolveManagedExportSource(fileName: String): File? {
    return when (fileName) {
      "mcpx.bin" -> resolveConfiguredFileOrLocalFallback("mcpxPath", "mcpx.bin")
      "flash.bin" -> resolveConfiguredFileOrLocalFallback("flashPath", "flash.bin")
      "hdd.img" -> resolveConfiguredFileOrLocalFallback("hddPath", "hdd.img")
      "eeprom.bin" -> resolveEepromFile().takeIf { it.isFile }
      "xemu.toml" -> resolveManagedImportTarget("xemu.toml").takeIf { it.isFile }
      else -> null
    }
  }

  private fun resolveConfiguredFileOrLocalFallback(pathKey: String, fallbackName: String): File? {
    val configuredFile = prefs.getString(pathKey, null)
      ?.let(::File)
      ?.takeIf { it.isFile }
    if (configuredFile != null) {
      return configuredFile
    }

    val localFile = File(resolveManagedFilesRoot(), fallbackName)
    return localFile.takeIf { it.isFile }
  }

  private fun normalizeManagedFileName(rawName: String): String? {
    val trimmed = rawName.replace('\\', '/').substringAfterLast('/').trim()
    if (trimmed.isBlank()) {
      return null
    }
    val normalized = trimmed.lowercase(Locale.US)
    return normalized.takeIf { it in MANAGED_EMULATOR_FILE_NAMES }
  }

  private fun sortManagedFileNames(names: Iterable<String>): List<String> {
    return names.sortedBy { MANAGED_EMULATOR_FILE_ORDER.indexOf(it) }
  }

  private fun applyImportedManagedFile(
    fileName: String,
    target: File,
    editor: android.content.SharedPreferences.Editor,
  ) {
    when (fileName) {
      "mcpx.bin" -> editor.putString("mcpxPath", target.absolutePath).remove("mcpxUri")
      "flash.bin" -> editor.putString("flashPath", target.absolutePath).remove("flashUri")
      "hdd.img" -> editor.putString("hddPath", target.absolutePath).remove("hddUri")
      "xemu.toml" -> applyImportedConfigToml(target, editor)
    }
  }

  private fun applyImportedConfigToml(
    file: File,
    editor: android.content.SharedPreferences.Editor,
  ) {
    val sections = parseSimpleTomlSections(file)

    resolveImportedConfigFileReference(
      rawPath = parseTomlString(sections, "sys.files", "bootrom_path"),
      managedFileName = "mcpx.bin",
    )?.let { resolved ->
      editor.putString("mcpxPath", resolved.absolutePath).remove("mcpxUri")
    }
    resolveImportedConfigFileReference(
      rawPath = parseTomlString(sections, "sys.files", "flashrom_path"),
      managedFileName = "flash.bin",
    )?.let { resolved ->
      editor.putString("flashPath", resolved.absolutePath).remove("flashUri")
    }
    resolveImportedConfigFileReference(
      rawPath = parseTomlString(sections, "sys.files", "hdd_path"),
      managedFileName = "hdd.img",
    )?.let { resolved ->
      editor.putString("hddPath", resolved.absolutePath).remove("hddUri")
    }

    parseTomlBoolean(sections, "general", "skip_boot_anim")
      ?.let { editor.putBoolean("setting_skip_boot_anim", it) }
    parseTomlString(sections, "display", "renderer")
      ?.lowercase(Locale.US)
      ?.takeIf { it == "opengl" || it == "vulkan" }
      ?.let { editor.putString("setting_renderer", it) }
    parseTomlString(sections, "display", "filtering")
      ?.lowercase(Locale.US)
      ?.takeIf { it == "linear" || it == "nearest" }
      ?.let { editor.putString("setting_filtering", it) }
    parseTomlBoolean(sections, "display.window", "vsync")
      ?.let { editor.putBoolean("setting_vsync", it) }
    parseTomlInt(sections, "display.quality", "surface_scale")
      ?.coerceIn(1, 3)
      ?.let { editor.putInt("setting_surface_scale", it) }
    parseTomlBoolean(sections, "audio", "use_dsp")
      ?.let { editor.putBoolean("setting_use_dsp", it) }
    parseTomlBoolean(sections, "audio", "hrtf")
      ?.let { editor.putBoolean(PREF_HRTF, it) }
    parseTomlBoolean(sections, "perf", "cache_shaders")
      ?.let { editor.putBoolean("setting_cache_shaders", it) }
    (parseTomlBoolean(sections, "perf", "fp_jit")
      ?: parseTomlBoolean(sections, "perf", "hard_fpu"))
      ?.let { editor.putBoolean("setting_hard_fpu", it) }
    parseTomlString(sections, "android", "tcg_thread")
      ?.lowercase(Locale.US)
      ?.takeIf { it == "single" || it == "multi" }
      ?.let { editor.putString("setting_tcg_thread", it) }
    parseTomlString(sections, "android", "audio_driver")
      ?.let(::normalizeImportedAudioDriver)
      ?.let { editor.putString("setting_audio_driver", it) }
    parseTomlBoolean(sections, "net", "enable")
      ?.let { editor.putBoolean("setting_network_enable", it) }
    parseTomlInt(sections, "sys", "mem_limit")
      ?.takeIf { it == 64 || it == 128 }
      ?.let { editor.putInt("setting_system_memory_mib", it) }
  }

  private fun parseSimpleTomlSections(file: File): Map<String, Map<String, String>> {
    val sections = linkedMapOf<String, MutableMap<String, String>>()
    var currentSection = ""

    file.forEachLine { rawLine ->
      val trimmed = stripTomlComment(rawLine).trim()
      if (trimmed.isBlank()) {
        return@forEachLine
      }
      if (trimmed.startsWith('[') && trimmed.endsWith(']')) {
        currentSection = trimmed.substring(1, trimmed.length - 1).trim()
        return@forEachLine
      }

      val separator = trimmed.indexOf('=')
      if (separator <= 0) {
        return@forEachLine
      }

      val key = trimmed.substring(0, separator).trim()
      val value = trimmed.substring(separator + 1).trim()
      if (key.isEmpty() || value.isEmpty()) {
        return@forEachLine
      }

      sections.getOrPut(currentSection) { linkedMapOf() }[key] = value
    }

    return sections
  }

  private fun stripTomlComment(line: String): String {
    var inString = false
    var escaping = false

    for ((index, char) in line.withIndex()) {
      when {
        char == '"' && !escaping -> inString = !inString
        char == '#' && !inString -> return line.substring(0, index)
      }

      escaping = if (char == '\\' && inString) {
        !escaping
      } else {
        false
      }
    }

    return line
  }

  private fun parseTomlBoolean(
    sections: Map<String, Map<String, String>>,
    section: String,
    key: String,
  ): Boolean? {
    return when (sections[section]?.get(key)?.trim()?.lowercase(Locale.US)) {
      "true" -> true
      "false" -> false
      else -> null
    }
  }

  private fun parseTomlInt(
    sections: Map<String, Map<String, String>>,
    section: String,
    key: String,
  ): Int? {
    val rawValue = sections[section]?.get(key)?.trim() ?: return null
    return rawValue.toIntOrNull() ?: decodeTomlString(rawValue)?.toIntOrNull()
  }

  private fun parseTomlString(
    sections: Map<String, Map<String, String>>,
    section: String,
    key: String,
  ): String? {
    val rawValue = sections[section]?.get(key)?.trim() ?: return null
    return decodeTomlString(rawValue) ?: rawValue
  }

  private fun decodeTomlString(rawValue: String): String? {
    if (rawValue.length < 2 || rawValue.first() != '"' || rawValue.last() != '"') {
      return null
    }

    val inner = rawValue.substring(1, rawValue.length - 1)
    return inner
      .replace("\\\\", "\\")
      .replace("\\\"", "\"")
  }

  private fun normalizeImportedAudioDriver(rawValue: String): String? {
    return when (rawValue.trim().lowercase(Locale.US)) {
      "android",
      "audiotrack",
      "opensl",
      "opensles",
      "opensl_es",
      "opensl-es",
      "openslesaudio",
      "openslesbackend" -> "openslES"
      "aaudio" -> "aaudio"
      "dummy", "disabled" -> "dummy"
      else -> null
    }
  }

  private fun resolveImportedConfigFileReference(
    rawPath: String?,
    managedFileName: String,
  ): File? {
    val trimmed = rawPath?.trim().orEmpty()
    if (trimmed.isEmpty()) {
      return null
    }

    val directFile = File(trimmed)
    if (directFile.isFile) {
      return directFile
    }

    return if (directFile.name.lowercase(Locale.US) == managedFileName.lowercase(Locale.US)) {
      resolveManagedImportTarget(managedFileName).takeIf { it.isFile }
    } else {
      null
    }
  }

  private fun setAdvancedExperimentalExpanded(expanded: Boolean) {
    layoutAdvancedExperimentalContent.visibility = if (expanded) View.VISIBLE else View.GONE
    btnToggleAdvancedExperimental.text = getString(
      if (expanded) {
        R.string.settings_advanced_experimental_hide
      } else {
        R.string.settings_advanced_experimental_show
      }
    )
    prefs.edit().putBoolean(PREF_ADVANCED_EXPERIMENTAL_EXPANDED, expanded).apply()
  }

  private fun exportDebugLog(uri: Uri) {
    try {
      contentResolver.openOutputStream(uri, "w")?.use { stream ->
        DebugLog.exportCombined(this, stream)
      } ?: throw IOException("Could not open the selected export location.")
      Toast.makeText(this, R.string.settings_export_debug_log_success, Toast.LENGTH_LONG).show()
    } catch (error: Exception) {
      Toast.makeText(
        this,
        getString(R.string.settings_export_debug_log_failed, error.message ?: "unknown error"),
        Toast.LENGTH_LONG
      ).show()
    }
  }

  private fun openExternalLink(url: String) {
    val intent = Intent(Intent.ACTION_VIEW, Uri.parse(url)).apply {
      addCategory(Intent.CATEGORY_BROWSABLE)
    }
    try {
      startActivity(intent)
    } catch (_: Exception) {
      Toast.makeText(this, getString(R.string.library_about_open_failed), Toast.LENGTH_SHORT).show()
    }
  }

  private fun refreshInsigniaStatus() {
    val hddFile = resolveHddFile()
    val eepromFile = resolveEepromFile()
    val setupUri = resolveInsigniaSetupAssistantUri()
    val setupName = resolveInsigniaSetupAssistantName()
    val setupReady = setupUri != null && hasPersistedReadPermission(setupUri)

    tvInsigniaStatus.text = getString(R.string.settings_insignia_status_checking)

    Thread {
      var dashboardStatus: XboxInsigniaHelper.DashboardStatus? = null
      var dashboardError: String? = null

      if (hddFile != null) {
        try {
          dashboardStatus = XboxInsigniaHelper.inspectDashboard(hddFile)
        } catch (error: Exception) {
          dashboardError = error.message ?: error.javaClass.simpleName
        }
      }

      val snapshot = InsigniaStatusSnapshot(
        hasLocalHdd = hddFile != null,
        hasEeprom = eepromFile.isFile,
        dashboardStatus = dashboardStatus,
        dashboardError = dashboardError,
        setupAssistantName = setupName,
        setupAssistantReady = setupReady,
      )

      runOnUiThread {
        if (isFinishing || isDestroyed) {
          return@runOnUiThread
        }
        tvInsigniaStatus.text = buildInsigniaStatusText(snapshot)
      }
    }.start()
  }

  private fun buildInsigniaStatusText(snapshot: InsigniaStatusSnapshot): String {
    val dashboardLine = when {
      !snapshot.hasLocalHdd ->
        getString(R.string.settings_insignia_status_dashboard_unavailable)
      snapshot.dashboardError != null ->
        getString(R.string.settings_insignia_status_dashboard_error, snapshot.dashboardError)
      snapshot.dashboardStatus?.looksRetailDashboardInstalled == true ->
        getString(R.string.settings_insignia_status_dashboard_ready)
      snapshot.dashboardStatus?.hasAnyRetailDashboardFiles == true ->
        getString(R.string.settings_insignia_status_dashboard_partial)
      else ->
        getString(R.string.settings_insignia_status_dashboard_missing)
    }

    val eepromLine = if (snapshot.hasEeprom) {
      getString(R.string.settings_insignia_status_eeprom_ready)
    } else {
      getString(R.string.settings_insignia_status_eeprom_missing)
    }

    val setupLine = when {
      snapshot.setupAssistantReady ->
        getString(
          R.string.settings_insignia_status_setup_selected,
          snapshot.setupAssistantName ?: getString(R.string.settings_insignia_setup_source_unknown),
        )
      snapshot.setupAssistantName != null ->
        getString(R.string.settings_insignia_status_setup_inaccessible, snapshot.setupAssistantName)
      else ->
        getString(R.string.settings_insignia_status_setup_missing)
    }

    return listOf(
      getString(
        R.string.settings_insignia_status_dns,
        XboxInsigniaHelper.PRIMARY_DNS,
        XboxInsigniaHelper.SECONDARY_DNS,
      ),
      dashboardLine,
      eepromLine,
      setupLine,
    ).joinToString("\n")
  }

  private fun prepareInsigniaNetworking() {
    if (isPreparingInsignia || isInitializingHdd || isImportingDashboard) {
      return
    }

    val hddFile = resolveHddFile()
    if (hddFile == null) {
      Toast.makeText(this, R.string.settings_insignia_prepare_no_hdd, Toast.LENGTH_LONG).show()
      refreshInsigniaStatus()
      return
    }

    val eepromFile = resolveEepromFile()
    if (!eepromFile.isFile) {
      Toast.makeText(this, R.string.settings_insignia_prepare_no_eeprom, Toast.LENGTH_LONG).show()
      refreshInsigniaStatus()
      return
    }

    switchNetworkEnable.isChecked = true
    prefs.edit().putBoolean("setting_network_enable", true).apply()

    isPreparingInsignia = true
    Toast.makeText(this, R.string.settings_insignia_prepare_working, Toast.LENGTH_SHORT).show()

    Thread {
      val result = runCatching {
        XboxInsigniaHelper.applyConfigSectorDns(hddFile)
        XboxEepromEditor.applyXboxLiveDns(eepromFile, XboxInsigniaHelper.primaryDnsBytes())
        runCatching { XboxInsigniaHelper.inspectDashboard(hddFile) }.getOrNull()
      }

      runOnUiThread {
        isPreparingInsignia = false
        if (isFinishing || isDestroyed) {
          return@runOnUiThread
        }

        result.onSuccess { dashboardStatus ->
          refreshInsigniaStatus()
          val messageRes = if (dashboardStatus?.looksRetailDashboardInstalled == false) {
            R.string.settings_insignia_prepare_success_missing_dashboard
          } else {
            R.string.settings_insignia_prepare_success
          }
          Toast.makeText(this, messageRes, Toast.LENGTH_LONG).show()
        }.onFailure { error ->
          Toast.makeText(
            this,
            getString(
              R.string.settings_insignia_prepare_failed,
              error.message ?: error.javaClass.simpleName,
            ),
            Toast.LENGTH_LONG,
          ).show()
          refreshInsigniaStatus()
        }
      }
    }.start()
  }

  private fun showInsigniaSetupAssistantPrompt() {
    val setupUri = resolveInsigniaSetupAssistantUri()
    if (setupUri == null || !hasPersistedReadPermission(setupUri)) {
      if (setupUri != null) {
        prefs.edit()
          .remove(PREF_INSIGNIA_SETUP_URI)
          .remove(PREF_INSIGNIA_SETUP_NAME)
          .apply()
      }
      refreshInsigniaStatus()
      Toast.makeText(this, R.string.settings_insignia_register_pick_prompt, Toast.LENGTH_SHORT).show()
      pickInsigniaSetupAssistant.launch(arrayOf("*/*"))
      return
    }

    val setupName = resolveInsigniaSetupAssistantName()
      ?: getString(R.string.settings_insignia_setup_source_unknown)
    MaterialAlertDialogBuilder(this)
      .setTitle(R.string.settings_insignia_register_title)
      .setMessage(getString(R.string.settings_insignia_register_message, setupName))
      .setPositiveButton(R.string.settings_insignia_register_boot_action) { _, _ ->
        launchInsigniaSetupAssistant(setupUri)
      }
      .setNeutralButton(R.string.settings_insignia_register_choose_new) { _, _ ->
        pickInsigniaSetupAssistant.launch(arrayOf("*/*"))
      }
      .setNegativeButton(android.R.string.cancel, null)
      .show()
  }

  private fun launchInsigniaSetupAssistant(uri: Uri) {
    persistUriPermission(uri)
    switchNetworkEnable.isChecked = true
    val launchEditor = prefs.edit()
    PerGameSettingsManager.applyRuntimeOverridesToEditor(
      context = this,
      editor = launchEditor,
      relativePath = null,
    )
    launchEditor
      .putBoolean("setting_network_enable", true)
      .putString("dvdUri", uri.toString())
      .remove("dvdPath")
      .putBoolean("skip_game_picker", false)
      .commit()

    startActivity(Intent(this, MainActivity::class.java))
  }

  private fun resolveInsigniaSetupAssistantUri(): Uri? {
    return prefs.getString(PREF_INSIGNIA_SETUP_URI, null)?.let(Uri::parse)
  }

  private fun resolveInsigniaSetupAssistantName(): String? {
    return prefs.getString(PREF_INSIGNIA_SETUP_NAME, null)
  }

  private fun setupEepromEditor() {
    val languageLabels = eepromLanguageOptions.map { getString(it.labelRes) }
    val videoLabels = eepromVideoOptions.map { getString(it.labelRes) }
    val aspectRatioLabels = eepromAspectRatioOptions.map { getString(it.labelRes) }
    val refreshRateLabels = eepromRefreshRateOptions.map { getString(it.labelRes) }

    dropdownEepromLanguage.setAdapter(
      ArrayAdapter(this, android.R.layout.simple_list_item_1, languageLabels)
    )
    dropdownEepromVideoStandard.setAdapter(
      ArrayAdapter(this, android.R.layout.simple_list_item_1, videoLabels)
    )
    dropdownEepromAspectRatio.setAdapter(
      ArrayAdapter(this, android.R.layout.simple_list_item_1, aspectRatioLabels)
    )
    dropdownEepromRefreshRate.setAdapter(
      ArrayAdapter(this, android.R.layout.simple_list_item_1, refreshRateLabels)
    )

    dropdownEepromLanguage.setOnItemClickListener { _, _, position, _ ->
      selectedEepromLanguage = eepromLanguageOptions[position].value
    }
    dropdownEepromVideoStandard.setOnItemClickListener { _, _, position, _ ->
      selectedEepromVideoStandard = eepromVideoOptions[position].value
    }
    dropdownEepromAspectRatio.setOnItemClickListener { _, _, position, _ ->
      selectedEepromAspectRatio = eepromAspectRatioOptions[position].value
    }
    dropdownEepromRefreshRate.setOnItemClickListener { _, _, position, _ ->
      selectedEepromRefreshRate = eepromRefreshRateOptions[position].value
    }

    val eepromFile = resolveEepromFile()
    if (!eepromFile.isFile) {
      eepromEditable = false
      eepromMissing = true
      eepromError = false
      setEepromEditorEnabled(false)
      setEepromLanguageSelection(selectedEepromLanguage)
      setEepromVideoSelection(selectedEepromVideoStandard)
      setEepromVideoSettingsSelection(
        XboxEepromEditor.VideoSettings(
          allow480p = false,
          allow720p = false,
          allow1080i = false,
          aspectRatio = selectedEepromAspectRatio,
          refreshRate = selectedEepromRefreshRate,
        )
      )
      tvEepromStatus.text = getString(
        R.string.settings_eeprom_status_missing,
        eepromFile.absolutePath,
      )
      return
    }

    try {
      val snapshot = XboxEepromEditor.load(eepromFile)
      eepromEditable = true
      eepromMissing = false
      eepromError = false
      setEepromEditorEnabled(true)
      setEepromLanguageSelection(snapshot.language)
      setEepromVideoSelection(snapshot.videoStandard)
      setEepromVideoSettingsSelection(snapshot.videoSettings)

      val hasUnknownValues =
        snapshot.rawLanguage != snapshot.language.id ||
        snapshot.rawVideoStandard != snapshot.videoStandard.id ||
        snapshot.hasManagedVideoSettingsMismatch
      tvEepromStatus.text = if (hasUnknownValues) {
        getString(R.string.settings_eeprom_status_unknown, eepromFile.absolutePath)
      } else {
        getString(R.string.settings_eeprom_status_ready, eepromFile.absolutePath)
      }
    } catch (_: IllegalArgumentException) {
      eepromEditable = false
      eepromMissing = false
      eepromError = true
      setEepromEditorEnabled(false)
      setEepromLanguageSelection(selectedEepromLanguage)
      setEepromVideoSelection(selectedEepromVideoStandard)
      setEepromVideoSettingsSelection(
        XboxEepromEditor.VideoSettings(
          allow480p = false,
          allow720p = false,
          allow1080i = false,
          aspectRatio = selectedEepromAspectRatio,
          refreshRate = selectedEepromRefreshRate,
        )
      )
      tvEepromStatus.text = getString(
        R.string.settings_eeprom_status_invalid,
        eepromFile.absolutePath,
      )
    } catch (_: Exception) {
      eepromEditable = false
      eepromMissing = false
      eepromError = true
      setEepromEditorEnabled(false)
      setEepromLanguageSelection(selectedEepromLanguage)
      setEepromVideoSelection(selectedEepromVideoStandard)
      setEepromVideoSettingsSelection(
        XboxEepromEditor.VideoSettings(
          allow480p = false,
          allow720p = false,
          allow1080i = false,
          aspectRatio = selectedEepromAspectRatio,
          refreshRate = selectedEepromRefreshRate,
        )
      )
      tvEepromStatus.text = getString(
        R.string.settings_eeprom_status_error,
        eepromFile.absolutePath,
      )
    }
  }

  private fun setupOrientationControls() {
    val uiOrientationLabels = uiOrientationOptions.map { getString(it.labelRes) }
    val gameOrientationLabels = gameOrientationOptions.map { getString(it.labelRes) }

    dropdownUiOrientation.setAdapter(
      ArrayAdapter(this, android.R.layout.simple_list_item_1, uiOrientationLabels)
    )
    dropdownGameOrientation.setAdapter(
      ArrayAdapter(this, android.R.layout.simple_list_item_1, gameOrientationLabels)
    )

    dropdownUiOrientation.setOnItemClickListener { _, _, position, _ ->
      selectedUiOrientation = uiOrientationOptions[position].value
    }
    dropdownGameOrientation.setOnItemClickListener { _, _, position, _ ->
      selectedGameOrientation = gameOrientationOptions[position].value
    }
  }

  private fun setUiOrientationSelection(orientation: OrientationPreferences.UiOrientation) {
    selectedUiOrientation = orientation
    val option = uiOrientationOptions.firstOrNull { it.value == orientation }
      ?: uiOrientationOptions.first()
    dropdownUiOrientation.setText(getString(option.labelRes), false)
  }

  private fun setGameOrientationSelection(orientation: OrientationPreferences.GameOrientation) {
    selectedGameOrientation = orientation
    val option = gameOrientationOptions.firstOrNull { it.value == orientation }
      ?: gameOrientationOptions.first()
    dropdownGameOrientation.setText(getString(option.labelRes), false)
  }

  private fun applyEepromEdits(): Pair<Int, Int> {
    if (eepromMissing) {
      return Pair(R.string.settings_saved_eeprom_missing, Toast.LENGTH_LONG)
    }
    if (eepromError || !eepromEditable) {
      return Pair(R.string.settings_saved_eeprom_failed, Toast.LENGTH_LONG)
    }

    return try {
      val changed = XboxEepromEditor.apply(
        resolveEepromFile(),
        selectedEepromLanguage,
        selectedEepromVideoStandard,
        XboxEepromEditor.VideoSettings(
          allow480p = switchEeprom480p.isChecked,
          allow720p = switchEeprom720p.isChecked,
          allow1080i = switchEeprom1080i.isChecked,
          aspectRatio = selectedEepromAspectRatio,
          refreshRate = selectedEepromRefreshRate,
        ),
      )
      if (changed) {
        Pair(R.string.settings_saved_with_eeprom, Toast.LENGTH_SHORT)
      } else {
        Pair(R.string.settings_saved, Toast.LENGTH_SHORT)
      }
    } catch (_: Exception) {
      Pair(R.string.settings_saved_eeprom_failed, Toast.LENGTH_LONG)
    }
  }

  private fun setEepromEditorEnabled(enabled: Boolean) {
    inputEepromLanguage.isEnabled = enabled
    inputEepromVideoStandard.isEnabled = enabled
    inputEepromAspectRatio.isEnabled = enabled
    inputEepromRefreshRate.isEnabled = enabled
    dropdownEepromLanguage.isEnabled = enabled
    dropdownEepromVideoStandard.isEnabled = enabled
    dropdownEepromAspectRatio.isEnabled = enabled
    dropdownEepromRefreshRate.isEnabled = enabled
    switchEeprom480p.isEnabled = enabled
    switchEeprom720p.isEnabled = enabled
    switchEeprom1080i.isEnabled = enabled
  }

  private fun setEepromLanguageSelection(language: XboxEepromEditor.Language) {
    selectedEepromLanguage = language
    val option = eepromLanguageOptions.firstOrNull { it.value == language }
      ?: eepromLanguageOptions.first()
    dropdownEepromLanguage.setText(getString(option.labelRes), false)
  }

  private fun setEepromVideoSelection(video: XboxEepromEditor.VideoStandard) {
    selectedEepromVideoStandard = video
    val option = eepromVideoOptions.firstOrNull { it.value == video }
      ?: eepromVideoOptions.first()
    dropdownEepromVideoStandard.setText(getString(option.labelRes), false)
  }

  private fun setEepromVideoSettingsSelection(videoSettings: XboxEepromEditor.VideoSettings) {
    switchEeprom480p.isChecked = videoSettings.allow480p
    switchEeprom720p.isChecked = videoSettings.allow720p
    switchEeprom1080i.isChecked = videoSettings.allow1080i
    setEepromAspectRatioSelection(videoSettings.aspectRatio)
    setEepromRefreshRateSelection(videoSettings.refreshRate)
  }

  private fun setEepromAspectRatioSelection(aspectRatio: XboxEepromEditor.AspectRatio) {
    selectedEepromAspectRatio = aspectRatio
    val option = eepromAspectRatioOptions.firstOrNull { it.value == aspectRatio }
      ?: eepromAspectRatioOptions.first()
    dropdownEepromAspectRatio.setText(getString(option.labelRes), false)
  }

  private fun setEepromRefreshRateSelection(refreshRate: XboxEepromEditor.RefreshRate) {
    selectedEepromRefreshRate = refreshRate
    val option = eepromRefreshRateOptions.firstOrNull { it.value == refreshRate }
      ?: eepromRefreshRateOptions.first()
    dropdownEepromRefreshRate.setText(getString(option.labelRes), false)
  }

  private fun showClearCacheConfirmation() {
    MaterialAlertDialogBuilder(this, R.style.ThemeOverlay_Xemu_RoundedDialog)
      .setTitle(R.string.settings_clear_cache_title)
      .setMessage(R.string.settings_clear_cache_message)
      .setPositiveButton(R.string.settings_clear_cache_action) { _, _ ->
        val result = clearSystemCache()
        val messageRes = when {
          result.hadFailures -> R.string.settings_clear_cache_partial
          result.deletedEntries > 0 -> R.string.settings_clear_cache_success
          else -> R.string.settings_clear_cache_empty
        }
        Toast.makeText(this, getString(messageRes), Toast.LENGTH_SHORT).show()
      }
      .setNegativeButton(android.R.string.cancel, null)
      .show()
  }

  private fun showInitializeHddLayoutPicker(button: MaterialButton) {
    val hddFile = resolveHddFile()
    if (hddFile == null) {
      refreshHddToolsPreview(button)
      return
    }

    val inspection = runCatching { XboxHddFormatter.inspect(hddFile) }.getOrElse { error ->
      tvHddToolsStatus.text = getString(
        R.string.settings_hdd_status_error,
        error.message ?: hddFile.absolutePath,
      )
      button.isEnabled = false
      return
    }

    if (!inspection.supportsRetailFormat) {
      refreshHddToolsState(button)
      return
    }

    val supportedLayouts = XboxHddFormatter.supportedLayouts(inspection).toSet()
    if (supportedLayouts.isEmpty()) {
      refreshHddToolsState(button)
      return
    }

    val allLayouts = XboxHddFormatter.Layout.entries
    val labels = allLayouts
      .map { layout ->
        val label = getString(hddLayoutLabelRes(layout))
        val availability = XboxHddFormatter.availabilityFor(inspection, layout)
        if (availability == XboxHddFormatter.LayoutAvailability.AVAILABLE) {
          label
        } else {
          getString(
            R.string.settings_hdd_layout_unavailable_format,
            label,
            getString(hddLayoutUnavailableReasonRes(availability)),
          )
        }
      }
      .toTypedArray()
    val dp = resources.displayMetrics.density
    lateinit var hddDialog: androidx.appcompat.app.AlertDialog

    val buttonList = LinearLayout(this).apply {
      orientation = LinearLayout.VERTICAL
      setPadding((20 * dp).toInt(), (12 * dp).toInt(), (20 * dp).toInt(), 0)
      labels.forEachIndexed { i, label ->
        addView(MaterialButton(this@SettingsActivity, null,
          com.google.android.material.R.attr.materialButtonOutlinedStyle).apply {
          text = label
          isAllCaps = false
          layoutParams = LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT,
            LinearLayout.LayoutParams.WRAP_CONTENT
          ).also { lp -> lp.bottomMargin = (8 * dp).toInt() }
          setOnClickListener {
            hddDialog.dismiss()
            val layout = allLayouts[i]
            val availability = XboxHddFormatter.availabilityFor(inspection, layout)
            if (availability == XboxHddFormatter.LayoutAvailability.AVAILABLE) {
              showInitializeHddConfirmation(hddFile, layout, button)
            } else {
              MaterialAlertDialogBuilder(this@SettingsActivity, R.style.ThemeOverlay_Xemu_RoundedDialog)
                .setTitle(R.string.settings_hdd_layout_unavailable_title)
                .setMessage(getString(hddLayoutUnavailableReasonRes(availability)))
                .setPositiveButton(android.R.string.ok, null)
                .show()
            }
          }
        })
      }
    }

    hddDialog = MaterialAlertDialogBuilder(this, R.style.ThemeOverlay_Xemu_RoundedDialog)
      .setTitle(R.string.settings_hdd_layout_pick_title)
      .setView(buttonList)
      .setNegativeButton(android.R.string.cancel, null)
      .create()
    hddDialog.show()
  }

  private fun showInitializeHddConfirmation(
    hddFile: File,
    layout: XboxHddFormatter.Layout,
    button: MaterialButton,
  ) {
    MaterialAlertDialogBuilder(this, R.style.ThemeOverlay_Xemu_RoundedDialog)
      .setTitle(R.string.settings_hdd_init_title)
      .setMessage(
        getString(
          R.string.settings_hdd_init_message,
          getString(hddLayoutLabelRes(layout)),
          getString(hddLayoutSummaryRes(layout)),
          hddFile.absolutePath,
        )
      )
      .setPositiveButton(R.string.settings_hdd_init_action) { _, _ ->
        initializeHddLayout(hddFile, layout, button)
      }
      .setNegativeButton(android.R.string.cancel, null)
      .show()
  }

  private fun initializeHddLayout(
    hddFile: File,
    layout: XboxHddFormatter.Layout,
    button: MaterialButton,
  ) {
    if (isInitializingHdd) {
      return
    }

    isInitializingHdd = true
    button.isEnabled = false
    Toast.makeText(this, R.string.settings_hdd_init_working, Toast.LENGTH_SHORT).show()

    Thread {
      val result = runCatching {
        XboxHddFormatter.initialize(hddFile, layout)
      }

      runOnUiThread {
        isInitializingHdd = false
        refreshHddToolsState(button)
        refreshInsigniaStatus()
        result.onSuccess {
          Toast.makeText(this, R.string.settings_hdd_init_success, Toast.LENGTH_SHORT).show()
        }.onFailure { error ->
          Toast.makeText(
            this,
            getString(
              R.string.settings_hdd_init_failed,
              error.message ?: hddFile.absolutePath,
            ),
            Toast.LENGTH_LONG,
          ).show()
        }
      }
    }.start()
  }

  private fun refreshHddToolsState(button: MaterialButton) {
    val hddFile = resolveHddFile()
    if (hddFile == null) {
      tvHddToolsStatus.text = getString(R.string.settings_hdd_status_missing)
      button.isEnabled = false
      return
    }

    val inspection = runCatching { XboxHddFormatter.inspect(hddFile) }.getOrElse { error ->
      tvHddToolsStatus.text = getString(
        R.string.settings_hdd_status_error,
        error.message ?: hddFile.absolutePath,
      )
      button.isEnabled = false
      return
    }

    val sizeLabel = Formatter.formatFileSize(this, inspection.totalBytes)
    val formatLabel = getString(hddFormatLabelRes(inspection.format))
    tvHddToolsStatus.text = when {
      inspection.totalBytes < XboxHddFormatter.MINIMUM_RETAIL_DISK_BYTES -> getString(
        R.string.settings_hdd_status_too_small,
        formatLabel,
        sizeLabel,
        hddFile.absolutePath,
      )
      else -> getString(
        R.string.settings_hdd_status_ready,
        formatLabel,
        sizeLabel,
        hddFile.absolutePath,
      )
    }
    button.isEnabled = !isInitializingHdd && XboxHddFormatter.supportedLayouts(inspection).isNotEmpty()
  }

  private fun refreshHddToolsPreview(button: MaterialButton) {
    val hddFile = resolveHddFile()
    if (hddFile == null || !hddFile.isFile) {
      tvHddToolsStatus.text = getString(R.string.settings_hdd_status_missing)
      button.isEnabled = false
      return
    }

    tvHddToolsStatus.text = getString(
      R.string.settings_hdd_status_configured,
      hddFile.absolutePath,
    )
    button.isEnabled = !isInitializingHdd
  }

  private fun showDashboardImportSourcePicker() {
    val hddFile = resolveHddFile()
    if (hddFile == null) {
      Toast.makeText(this, R.string.settings_hdd_status_missing, Toast.LENGTH_LONG).show()
      return
    }
    if (isImportingDashboard) {
      return
    }

    val labels = arrayOf(
      getString(R.string.settings_dashboard_import_source_zip),
      getString(R.string.settings_dashboard_import_source_folder),
    )
    val dp = resources.displayMetrics.density
    lateinit var importDialog: androidx.appcompat.app.AlertDialog

    val buttonList = LinearLayout(this).apply {
      orientation = LinearLayout.VERTICAL
      setPadding((20 * dp).toInt(), (12 * dp).toInt(), (20 * dp).toInt(), 0)
      labels.forEachIndexed { i, label ->
        addView(MaterialButton(this@SettingsActivity, null,
          com.google.android.material.R.attr.materialButtonOutlinedStyle).apply {
          text = label
          layoutParams = LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT,
            LinearLayout.LayoutParams.WRAP_CONTENT
          ).also { lp -> lp.bottomMargin = (8 * dp).toInt() }
          setOnClickListener {
            importDialog.dismiss()
            when (i) {
              0 -> pickDashboardZip.launch(arrayOf("application/zip", "application/octet-stream"))
              else -> pickDashboardFolder.launch(null)
            }
          }
        })
      }
    }

    importDialog = MaterialAlertDialogBuilder(this, R.style.ThemeOverlay_Xemu_RoundedDialog)
      .setTitle(R.string.settings_dashboard_import_source_title)
      .setView(buttonList)
      .setNegativeButton(android.R.string.cancel, null)
      .create()
    importDialog.show()
  }

  private fun prepareDashboardImportFromZip(uri: Uri) {
    val hddFile = resolveHddFile()
    if (hddFile == null) {
      Toast.makeText(this, R.string.settings_hdd_status_missing, Toast.LENGTH_LONG).show()
      return
    }

    startDashboardImportPreparation(hddFile) { workingDir ->
      extractDashboardZipToDirectory(uri, workingDir)
    }
  }

  private fun prepareDashboardImportFromFolder(uri: Uri) {
    val hddFile = resolveHddFile()
    if (hddFile == null) {
      Toast.makeText(this, R.string.settings_hdd_status_missing, Toast.LENGTH_LONG).show()
      return
    }

    startDashboardImportPreparation(hddFile) { workingDir ->
      copyDashboardTreeToDirectory(uri, workingDir)
    }
  }

  private fun startDashboardImportPreparation(
    hddFile: File,
    prepareSource: (File) -> File,
  ) {
    if (isImportingDashboard) {
      return
    }

    isImportingDashboard = true
    btnImportDashboard.isEnabled = false
    Toast.makeText(this, R.string.settings_dashboard_import_preparing, Toast.LENGTH_SHORT).show()

    Thread {
      var workingDir: File? = null
      val result = runCatching {
        workingDir = createDashboardWorkingDirectory()
        val preparedRoot = prepareSource(workingDir!!)
        val sourceRoot = normalizeDashboardSourceRoot(preparedRoot)
        if (!dashboardSourceHasFiles(sourceRoot)) {
          throw IOException(getString(R.string.settings_dashboard_import_empty))
        }
        val importLayoutRoot = buildDashboardImportLayout(sourceRoot, workingDir!!)
        val bootPreparation = prepareDashboardBootFiles(importLayoutRoot)

        DashboardImportPlan(
          hddFile = hddFile,
          workingDir = workingDir!!,
          sourceDir = importLayoutRoot,
          backupDir = createDashboardBackupDirectory(),
          summary = describeDashboardSource(importLayoutRoot),
          bootNote = bootPreparation.note,
          bootAliasCreated = bootPreparation.aliasCreated,
          retailBootReady = bootPreparation.retailBootReady,
        )
      }

      runOnUiThread {
        result.onSuccess { plan ->
          showDashboardImportConfirmation(plan)
        }.onFailure { error ->
          workingDir?.deleteRecursively()
          isImportingDashboard = false
          btnImportDashboard.isEnabled = true
          Toast.makeText(
            this,
            getString(
              R.string.settings_dashboard_import_failed,
              error.message ?: getString(R.string.settings_dashboard_import_empty),
            ),
            Toast.LENGTH_LONG,
          ).show()
        }
      }
    }.start()
  }

  private fun showDashboardImportConfirmation(plan: DashboardImportPlan) {
    MaterialAlertDialogBuilder(this, R.style.ThemeOverlay_Xemu_RoundedDialog)
      .setTitle(R.string.settings_dashboard_import_title)
      .setMessage(
        buildString {
          append(
            getString(
              R.string.settings_dashboard_import_message,
              plan.summary,
              plan.backupDir.absolutePath,
            )
          )
          if (!plan.bootNote.isNullOrBlank()) {
            append("\n\n")
            append(plan.bootNote)
          }
        }
      )
      .setPositiveButton(R.string.settings_dashboard_import_action) { _, _ ->
        importDashboard(plan)
      }
      .setNegativeButton(android.R.string.cancel) { _, _ ->
        plan.workingDir.deleteRecursively()
        isImportingDashboard = false
        btnImportDashboard.isEnabled = true
      }
      .setOnCancelListener {
        plan.workingDir.deleteRecursively()
        isImportingDashboard = false
        btnImportDashboard.isEnabled = true
      }
      .show()
  }

  private fun importDashboard(plan: DashboardImportPlan) {
    Toast.makeText(this, R.string.settings_dashboard_import_working, Toast.LENGTH_SHORT).show()

    Thread {
      val result = runCatching {
        XboxDashboardImporter.importDashboard(
          hddFile = plan.hddFile,
          sourceRoot = plan.sourceDir,
          backupRoot = plan.backupDir,
        )
      }

      runOnUiThread {
        plan.workingDir.deleteRecursively()
        isImportingDashboard = false
        btnImportDashboard.isEnabled = true
        refreshInsigniaStatus()
        result.onSuccess {
          val messageRes = when {
            plan.bootAliasCreated -> R.string.settings_dashboard_import_success_with_alias
            !plan.retailBootReady -> R.string.settings_dashboard_import_success_without_retail_boot
            else -> R.string.settings_dashboard_import_success
          }
          Toast.makeText(this, getString(messageRes, plan.backupDir.absolutePath), Toast.LENGTH_LONG).show()
        }.onFailure { error ->
          Toast.makeText(
            this,
            getString(
              R.string.settings_dashboard_import_failed,
              error.message ?: plan.hddFile.absolutePath,
            ),
            Toast.LENGTH_LONG,
          ).show()
        }
      }
    }.start()
  }

  private fun createDashboardWorkingDirectory(): File {
    val dir = File(cacheDir, "dashboard-import-${System.currentTimeMillis()}")
    if (!dir.mkdirs()) {
      throw IOException("Failed to prepare a temporary dashboard import folder.")
    }
    return dir
  }

  private fun createDashboardBackupDirectory(): File {
    val base = getExternalFilesDir(null) ?: filesDir
    val root = File(File(base, "x1box"), "dashboard-backups")
    val stamp = SimpleDateFormat("yyyyMMdd-HHmmss", Locale.US).format(Date())
    val dir = File(root, "dashboard-$stamp")
    if (!dir.mkdirs()) {
      throw IOException("Failed to prepare the dashboard backup folder.")
    }
    return dir
  }

  private fun extractDashboardZipToDirectory(uri: Uri, targetDir: File): File {
    val canonicalRoot = targetDir.canonicalFile
    contentResolver.openInputStream(uri)?.use { rawInput ->
      ZipInputStream(BufferedInputStream(rawInput)).use { zip ->
        while (true) {
          val entry = zip.nextEntry ?: break
          if (entry.name.isBlank()) {
            continue
          }
          val outFile = File(targetDir, entry.name).canonicalFile
          val rootPath = canonicalRoot.path + File.separator
          if (outFile.path != canonicalRoot.path && !outFile.path.startsWith(rootPath)) {
            throw IOException("The selected ZIP contains an invalid path.")
          }
          if (entry.isDirectory) {
            if (!outFile.exists() && !outFile.mkdirs()) {
              throw IOException("Failed to create ${outFile.name} from the ZIP.")
            }
            continue
          }

          outFile.parentFile?.let { parent ->
            if (!parent.exists() && !parent.mkdirs()) {
              throw IOException("Failed to create ${parent.name} from the ZIP.")
            }
          }
          FileOutputStream(outFile).use { output ->
            zip.copyTo(output)
          }
          zip.closeEntry()
        }
      }
    } ?: throw IOException("Failed to open the selected dashboard ZIP.")

    return targetDir
  }

  private fun copyDashboardTreeToDirectory(uri: Uri, targetDir: File): File {
    val root = DocumentFile.fromTreeUri(this, uri)
      ?: throw IOException("Failed to open the selected dashboard folder.")
    copyDocumentFileRecursively(root, targetDir)
    return targetDir
  }

  private fun copyDocumentFileRecursively(source: DocumentFile, target: File) {
    if (source.isDirectory) {
      val children = source.listFiles()
      for (child in children) {
        val name = child.name ?: continue
        val childTarget = File(target, name)
        if (child.isDirectory) {
          if (!childTarget.exists() && !childTarget.mkdirs()) {
            throw IOException("Failed to create ${childTarget.name}.")
          }
          copyDocumentFileRecursively(child, childTarget)
        } else if (child.isFile) {
          childTarget.parentFile?.mkdirs()
          contentResolver.openInputStream(child.uri)?.use { input ->
            FileOutputStream(childTarget).use { output ->
              input.copyTo(output)
            }
          } ?: throw IOException("Failed to copy ${child.name}.")
        }
      }
      return
    }

    if (source.isFile) {
      contentResolver.openInputStream(source.uri)?.use { input ->
        FileOutputStream(target).use { output ->
          input.copyTo(output)
        }
      } ?: throw IOException("Failed to copy ${source.name}.")
    }
  }

  private fun normalizeDashboardSourceRoot(root: File): File {
    var current = root

    while (true) {
      val children = dashboardSourceEntries(current)
      if (children.size != 1 || !children.first().isDirectory) {
        break
      }
      current = children.first()
    }

    if (looksLikeDashboardSourceRoot(current)) {
      return current
    }

    return findNestedDashboardSourceRoot(current) ?: current
  }

  private fun buildDashboardImportLayout(sourceRoot: File, workingDir: File): File {
    val entries = sourceRoot.listFiles()
      ?.filterNot { shouldSkipDashboardSourceEntry(it.name) }
      .orEmpty()
    val layoutRoot = File(workingDir, "dashboard-layout")
    if (layoutRoot.exists()) {
      layoutRoot.deleteRecursively()
    }
    if (!layoutRoot.mkdirs()) {
      throw IOException("Failed to prepare the dashboard import layout.")
    }

    val sourceC = entries.firstOrNull { it.isDirectory && it.name.equals("C", ignoreCase = true) }
      ?.let(::normalizeDashboardPartitionRoot)
    val sourceE = entries.firstOrNull { it.isDirectory && it.name.equals("E", ignoreCase = true) }
      ?.let(::normalizeDashboardPartitionRoot)
    val rootEntriesForC = if (sourceC == null) entries.filterNot { entry ->
      entry.isDirectory && (entry.name.equals("C", ignoreCase = true) || entry.name.equals("E", ignoreCase = true))
    } else {
      emptyList()
    }

    sourceC?.let { copyLocalDirectoryContents(it, File(layoutRoot, "C")) }
    if (rootEntriesForC.isNotEmpty()) {
      val targetC = File(layoutRoot, "C")
      for (entry in rootEntriesForC) {
        copyLocalEntry(entry, File(targetC, entry.name))
      }
    }

    sourceE?.let { copyLocalDirectoryContents(it, File(layoutRoot, "E")) }

    return layoutRoot
  }

  private fun normalizeDashboardPartitionRoot(partitionDir: File): File {
    if (!partitionDir.isDirectory) {
      return partitionDir
    }

    var current = partitionDir
    while (true) {
      if (looksLikeDashboardSourceRoot(current)) {
        return current
      }

      val children = dashboardSourceEntries(current).filter { it.isDirectory }
      if (children.size != 1) {
        break
      }
      current = children.first()
    }

    return findNestedDashboardSourceRoot(current) ?: current
  }

  private fun copyLocalDirectoryContents(sourceDir: File, targetDir: File) {
    val children = sourceDir.listFiles().orEmpty()
    if (!targetDir.exists() && !targetDir.mkdirs()) {
      throw IOException("Failed to create ${targetDir.name}.")
    }
    for (child in children) {
      if (shouldSkipDashboardSourceEntry(child.name)) {
        continue
      }
      copyLocalEntry(child, File(targetDir, child.name))
    }
  }

  private fun copyLocalEntry(source: File, target: File) {
    if (source.isDirectory) {
      if (!target.exists() && !target.mkdirs()) {
        throw IOException("Failed to create ${target.name}.")
      }
      for (child in source.listFiles().orEmpty()) {
        if (shouldSkipDashboardSourceEntry(child.name)) {
          continue
        }
        copyLocalEntry(child, File(target, child.name))
      }
      return
    }

    target.parentFile?.let { parent ->
      if (!parent.exists() && !parent.mkdirs()) {
        throw IOException("Failed to create ${parent.name}.")
      }
    }
    source.copyTo(target, overwrite = true)
  }

  private fun prepareDashboardBootFiles(layoutRoot: File): DashboardBootPreparation {
    val cDir = File(layoutRoot, "C")
    if (!cDir.isDirectory || !cDir.exists()) {
      return DashboardBootPreparation(
        note = getString(R.string.settings_dashboard_import_boot_missing_note),
        aliasCreated = false,
        retailBootReady = false,
      )
    }

    val topLevelFiles = cDir.listFiles()
      ?.filter { it.isFile }
      .orEmpty()
    val xboxdash = topLevelFiles.firstOrNull { it.name.equals("xboxdash.xbe", ignoreCase = true) }
    if (xboxdash != null) {
      return DashboardBootPreparation(
        note = null,
        aliasCreated = false,
        retailBootReady = true,
      )
    }

    val candidate = findDashboardBootCandidate(cDir)

    if (candidate != null) {
      val aliasFile = File(cDir, "xboxdash.xbe")
      candidate.copyTo(aliasFile, overwrite = true)
      val relativePath = candidate.relativeTo(cDir).invariantSeparatorsPath
      return DashboardBootPreparation(
        note = getString(R.string.settings_dashboard_import_boot_alias_note, relativePath),
        aliasCreated = true,
        retailBootReady = true,
      )
    }

    return DashboardBootPreparation(
      note = getString(R.string.settings_dashboard_import_boot_missing_note),
      aliasCreated = false,
      retailBootReady = false,
    )
  }

  private fun findDashboardBootCandidate(cDir: File): File? {
    var bestFile: File? = null
    var bestScore = Int.MIN_VALUE

    cDir.walkTopDown().forEach { file ->
      if (!file.isFile || !file.extension.equals("xbe", ignoreCase = true)) {
        return@forEach
      }

      val score = scoreDashboardBootCandidate(cDir, file)
      if (score > bestScore) {
        bestScore = score
        bestFile = file
      }
    }

    return bestFile
  }

  private fun scoreDashboardBootCandidate(cDir: File, candidate: File): Int {
    val relativePath = candidate.relativeTo(cDir).invariantSeparatorsPath.lowercase(Locale.US)
    val fileName = candidate.name.lowercase(Locale.US)
    val baseName = candidate.nameWithoutExtension.lowercase(Locale.US)
    val depth = relativePath.count { it == '/' }
    var score = 0

    score += when (fileName) {
      "xboxdash.xbe" -> 12_000
      "default.xbe" -> 10_000
      "evoxdash.xbe" -> 9_500
      "avalaunch.xbe" -> 9_400
      "unleashx.xbe" -> 9_300
      "xbmc.xbe" -> 9_200
      "nexgen.xbe" -> 9_100
      else -> 0
    }

    if (baseName.contains("dash")) {
      score += 800
    }
    if (relativePath.contains("/dashboard/") || relativePath.contains("/dash/")) {
      score += 500
    }
    if (relativePath.startsWith("dashboard/") || relativePath.startsWith("dash/")) {
      score += 400
    }
    if (relativePath.contains("/apps/") || relativePath.contains("/games/")) {
      score -= 1_000
    }
    if (baseName.contains("installer") || baseName.contains("uninstall") || baseName.contains("config")) {
      score -= 2_000
    }

    score += 300 - (depth * 40)
    return score
  }

  private fun dashboardSourceHasFiles(root: File): Boolean {
    return looksLikeDashboardSourceRoot(root) || findNestedDashboardSourceRoot(root) != null
  }

  private fun dashboardSourceEntries(root: File): List<File> {
    return root.listFiles()
      ?.filterNot { shouldSkipDashboardSourceEntry(it.name) }
      .orEmpty()
  }

  private fun looksLikeDashboardSourceRoot(root: File): Boolean {
    val entries = dashboardSourceEntries(root)
    if (entries.isEmpty()) {
      return false
    }

    val hasPartitionDir = entries.any { entry ->
      entry.isDirectory &&
        (entry.name.equals("C", ignoreCase = true) || entry.name.equals("E", ignoreCase = true)) &&
        dashboardSourceEntries(entry).isNotEmpty()
    }
    if (hasPartitionDir) {
      return true
    }

    return scoreDashboardSourceRoot(root, root) > 0
  }

  private fun findNestedDashboardSourceRoot(root: File): File? {
    var bestDir: File? = null
    var bestScore = Int.MIN_VALUE

    root.walkTopDown()
      .maxDepth(8)
      .forEach { candidate ->
        if (!candidate.isDirectory || candidate == root) {
          return@forEach
        }

        val score = scoreDashboardSourceRoot(root, candidate)
        if (score > bestScore) {
          bestScore = score
          bestDir = candidate
        }
      }

    return bestDir?.takeIf { bestScore > 0 }
  }

  private fun scoreDashboardSourceRoot(searchRoot: File, candidate: File): Int {
    val entries = dashboardSourceEntries(candidate)
    if (entries.isEmpty()) {
      return Int.MIN_VALUE
    }

    val partitionDirs = entries.filter { entry ->
      entry.isDirectory &&
        (entry.name.equals("C", ignoreCase = true) || entry.name.equals("E", ignoreCase = true)) &&
        dashboardSourceEntries(entry).isNotEmpty()
    }
    val directFiles = entries.filter { it.isFile }
    val directDirs = entries.filter { it.isDirectory }

    var score = 0
    if (partitionDirs.isNotEmpty()) {
      score += 10_000
    }
    if (directFiles.any { it.name.equals("xboxdash.xbe", ignoreCase = true) }) {
      score += 9_000
    }
    if (directFiles.any { it.name.equals("msdash.xbe", ignoreCase = true) }) {
      score += 7_000
    }
    if (directFiles.any { it.name.equals("xbox.xtf", ignoreCase = true) }) {
      score += 3_000
    }
    if (directDirs.any { it.name.equals("xodash", ignoreCase = true) }) {
      score += 3_000
    }
    if (directDirs.any { it.name.equals("audio", ignoreCase = true) }) {
      score += 1_500
    }
    if (directDirs.any { it.name.equals("fonts", ignoreCase = true) }) {
      score += 1_500
    }
    if (directFiles.any { it.extension.equals("xbe", ignoreCase = true) }) {
      score += 1_000
    }

    if (score <= 0) {
      return score
    }

    val depth = candidate.relativeTo(searchRoot)
      .invariantSeparatorsPath
      .count { it == '/' } + 1
    return score - (depth * 120)
  }

  private fun describeDashboardSource(root: File): String {
    val sourceC = File(root, "C")
    val sourceE = File(root, "E")
    val hasC = sourceC.isDirectory && sourceC.walkTopDown().any { it.isFile }
    val hasE = sourceE.isDirectory && sourceE.walkTopDown().any { it.isFile }

    return when {
      hasC && hasE -> getString(R.string.settings_dashboard_import_summary_c_e)
      hasE -> getString(R.string.settings_dashboard_import_summary_e)
      else -> getString(R.string.settings_dashboard_import_summary_c)
    }
  }

  private fun shouldSkipDashboardSourceEntry(name: String): Boolean {
    return name == ".DS_Store" || name == "__MACOSX"
  }

  private fun isZipSelection(uri: Uri): Boolean {
    val name = getFileName(uri) ?: uri.lastPathSegment ?: return false
    return name.lowercase(Locale.US).endsWith(".zip")
  }

  private fun copyUriToFile(
    uri: Uri,
    target: File,
    openError: String = "Failed to open the selected file.",
  ) {
    val parent = target.parentFile
    if (parent != null && !parent.exists() && !parent.mkdirs()) {
      throw IOException("Failed to prepare ${parent.absolutePath}.")
    }
    contentResolver.openInputStream(uri)?.use { input ->
      FileOutputStream(target).use { output ->
        input.copyTo(output)
      }
    } ?: throw IOException(openError)
  }

  private fun persistUriPermission(uri: Uri) {
    val flags = Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_GRANT_WRITE_URI_PERMISSION
    try {
      contentResolver.takePersistableUriPermission(uri, flags)
    } catch (_: SecurityException) {
    }
  }

  private fun hasPersistedReadPermission(uri: Uri): Boolean {
    return contentResolver.persistedUriPermissions.any { permission ->
      permission.isReadPermission && permission.uri == uri
    }
  }

  private fun hddFormatLabelRes(format: XboxHddFormatter.ImageFormat): Int {
    return when (format) {
      XboxHddFormatter.ImageFormat.RAW -> R.string.settings_hdd_format_raw
      XboxHddFormatter.ImageFormat.QCOW2 -> R.string.settings_hdd_format_qcow2
    }
  }

  private fun hddLayoutLabelRes(layout: XboxHddFormatter.Layout): Int {
    return when (layout) {
      XboxHddFormatter.Layout.RETAIL -> R.string.settings_hdd_layout_retail
      XboxHddFormatter.Layout.RETAIL_PLUS_F -> R.string.settings_hdd_layout_retail_f
      XboxHddFormatter.Layout.RETAIL_PLUS_F_G -> R.string.settings_hdd_layout_retail_f_g
    }
  }

  private fun hddLayoutSummaryRes(layout: XboxHddFormatter.Layout): Int {
    return when (layout) {
      XboxHddFormatter.Layout.RETAIL -> R.string.settings_hdd_layout_summary_retail
      XboxHddFormatter.Layout.RETAIL_PLUS_F -> R.string.settings_hdd_layout_summary_retail_f
      XboxHddFormatter.Layout.RETAIL_PLUS_F_G -> R.string.settings_hdd_layout_summary_retail_f_g
    }
  }

  private fun hddLayoutUnavailableReasonRes(
    availability: XboxHddFormatter.LayoutAvailability,
  ): Int {
    return when (availability) {
      XboxHddFormatter.LayoutAvailability.AVAILABLE ->
        R.string.settings_hdd_layout_unavailable_not_enough_space
      XboxHddFormatter.LayoutAvailability.NO_EXTENDED_SPACE ->
        R.string.settings_hdd_layout_unavailable_no_extended_space
      XboxHddFormatter.LayoutAvailability.NEEDS_STANDARD_G_BOUNDARY ->
        R.string.settings_hdd_layout_unavailable_needs_standard_g_boundary
      XboxHddFormatter.LayoutAvailability.NOT_ENOUGH_SPACE ->
        R.string.settings_hdd_layout_unavailable_not_enough_space
    }
  }

  private fun clearSystemCache(): CacheClearResult {
    var result = CacheClearResult(0, false)

    val cacheRoots = buildList {
      add(cacheDir)
      add(codeCacheDir)
      externalCacheDir?.let { add(it) }
    }
    for (root in cacheRoots.distinctBy { it.absolutePath }) {
      result = mergeCacheClearResults(result, clearDirectoryChildren(root))
    }

    val persistentRoots = buildList {
      add(filesDir)
      getExternalFilesDir(null)?.let { add(it) }
    }
    for (root in persistentRoots.distinctBy { it.absolutePath }) {
      result = mergeCacheClearResults(result, clearPersistentCacheEntries(root))
    }

    return result
  }

  private fun clearDirectoryChildren(dir: File?): CacheClearResult {
    if (dir == null || !dir.exists()) {
      return CacheClearResult(0, false)
    }

    val children = dir.listFiles() ?: return CacheClearResult(0, false)
    var deletedEntries = 0
    var hadFailures = false
    for (child in children) {
      val deleted = runCatching { child.deleteRecursively() }.getOrDefault(false)
      if (deleted) {
        deletedEntries++
      } else {
        hadFailures = true
      }
    }
    return CacheClearResult(deletedEntries, hadFailures)
  }

  private fun clearPersistentCacheEntries(root: File): CacheClearResult {
    if (!root.exists() || !root.isDirectory) {
      return CacheClearResult(0, false)
    }

    val children = root.listFiles() ?: return CacheClearResult(0, false)
    var deletedEntries = 0
    var hadFailures = false

    for (child in children) {
      if (isPersistentCacheEntry(child.name)) {
        val deleted = runCatching { child.deleteRecursively() }.getOrDefault(false)
        if (deleted) {
          deletedEntries++
        } else {
          hadFailures = true
        }
        continue
      }

      if (child.isDirectory) {
        val nested = clearPersistentCacheEntries(child)
        deletedEntries += nested.deletedEntries
        hadFailures = hadFailures || nested.hadFailures
      }
    }

    return CacheClearResult(deletedEntries, hadFailures)
  }

  private fun isPersistentCacheEntry(name: String): Boolean {
    return name == "shaders" ||
      name == "tb_cache.bin" ||
      name == "shader_cache_list" ||
      name.startsWith("scache-") ||
      name.startsWith("vk_pipeline_cache_")
  }

  private fun mergeCacheClearResults(
    first: CacheClearResult,
    second: CacheClearResult,
  ): CacheClearResult {
    return CacheClearResult(
      deletedEntries = first.deletedEntries + second.deletedEntries,
      hadFailures = first.hadFailures || second.hadFailures,
    )
  }

  private fun resolveEepromFile(): File {
    val base = getExternalFilesDir(null) ?: filesDir
    return File(File(base, "x1box"), "eeprom.bin")
  }

  private fun resolveHddFile(): File? {
    val path = prefs.getString("hddPath", null) ?: return null
    val file = File(path)
    return file.takeIf { it.isFile }
  }

  private fun getFileName(uri: Uri): String? {
    return contentResolver.query(uri, null, null, null, null)?.use { cursor ->
      val col = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME)
      if (col >= 0 && cursor.moveToFirst()) cursor.getString(col) else null
    }
  }
}
