/*
 * Adapted from hakuX's NativeCoreActivity (https://github.com/rfandango/hakuX,
 * GPLv3 or later; see android/hakux/core/COPYING).
 */
package com.izzy2lost.x1box.hakux

import androidx.appcompat.app.AlertDialog
import android.hardware.input.InputManager
import android.view.Gravity
import android.view.MotionEvent
import android.widget.HorizontalScrollView
import android.widget.LinearLayout
import com.google.android.material.button.MaterialButton
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import com.izzy2lost.x1box.ControllerRemap
import kotlin.math.abs
import kotlin.math.roundToInt
import android.content.Context
import android.content.Intent
import android.graphics.Bitmap
import android.graphics.Color
import android.graphics.Typeface
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.util.Log
import android.util.TypedValue
import android.view.InputDevice
import android.view.KeyEvent
import android.view.PixelCopy
import android.view.View
import android.view.ViewGroup
import android.view.WindowInsets
import android.view.WindowInsetsController
import android.widget.RelativeLayout
import android.widget.TextView
import android.window.OnBackInvokedDispatcher
import com.izzy2lost.x1box.ControllerSettings
import com.izzy2lost.x1box.GameLibraryActivity
import com.izzy2lost.x1box.GpuDriverHelper
import com.izzy2lost.x1box.HakuxLaunch
import com.izzy2lost.x1box.OnScreenController
import com.izzy2lost.x1box.OrientationLocker
import com.izzy2lost.x1box.PerGameSettingsManager
import com.izzy2lost.x1box.R
import java.io.File
import java.util.TimeZone
import org.libsdl.app.SDLActivity

/**
 * The hakuX core (libhakuxcore.so: FEX, an HLE Xbox kernel, the HLE GPU) in
 * its own process, :hakux, for one run. The core takes everything as
 * arguments (getArguments) and is polled for what it reports: the frame
 * rate, a fatal error, the title leaving or rebooting into another XBE.
 */
class HakuxCoreActivity : SDLActivity(), InputManager.InputDeviceListener {
  private val handler = Handler(Looper.getMainLooper())
  private val prefs by lazy { getSharedPreferences("x1box_prefs", Context.MODE_PRIVATE) }

  private var discUri: Uri? = null
  private var discArg = ""
  private var onScreenController: OnScreenController? = null
  private var controlsShown: Boolean? = null
  private var fpsView: TextView? = null
  private var shaderLine: TextView? = null

  override fun getLibraries(): Array<String> = arrayOf("SDL3", "hakuxcore")

  /** The setting for this game: its per-game override, else the global one. */
  private fun setting(key: String): String? =
    PerGameSettingsManager.getRuntimeOverride(this, key) ?: prefs.all[key]?.toString()

  override fun getArguments(): Array<String> {
    val args = mutableListOf(discArg, File(filesDir, "hakux/hdd").absolutePath)
    val state = intent.getStringExtra(EXTRA_LOADSTATE)
    if (state != null) {
      args += "loadstate=$state"        /* boot into a savestate (HakuxSaveStates) */
    } else if (intent.getBooleanExtra(EXTRA_RELAUNCH, false)) {
      args += "relaunch"
    }
    runCatching { packageManager.getPackageInfo(packageName, 0).versionName }.getOrNull()?.let {
      args += "version=$it"
    }
    /* The app's own display settings: the resolution scale (1-3x) and the
     * picture's shape (0 stretch, 1 4:3, 2 16:9). */
    val scale = setting("setting_surface_scale")?.toIntOrNull()?.coerceIn(1, 4) ?: 1
    args += "scale=$scale"
    args += "aspect=" + when (setting("setting_display_mode")?.toIntOrNull() ?: 0) {
      1 -> "4:3"
      2 -> "16:9"
      else -> "stretch"
    }
    args += "tz=${TimeZone.getDefault().getOffset(System.currentTimeMillis()) / 60_000}"
    /* Player 1: every physical controller (HakuxPadRouter) plus the on-screen controls. */
    args += "pads=1"
    vkDriverArg()?.let { args += it }
    return args.toTypedArray()
  }

  /**
   * The custom Vulkan driver (Turnip...) the core loads through adrenotools,
   * chosen as libxemu's is: the game's override ("system" or a driver ZIP),
   * else the installed one. Null: the system driver.
   */
  private fun vkDriverArg(): String? {
    GpuDriverHelper.init(this)
    if (!GpuDriverHelper.supportsCustomDriverLoading()) return null
    val override = PerGameSettingsManager.getRuntimeOverride(this, "setting_gpu_driver")
    if (override == "system") return null
    if (override != null) {
      val zip = File(GpuDriverHelper.driverStorageDir, override)
      if (!zip.exists() || !GpuDriverHelper.installDriver(zip)) {
        Log.w(TAG, "GPU driver: per-game $override not found or install failed, using the system driver")
        return null
      }
    }
    val lib = GpuDriverHelper.getInstalledDriverLibrary() ?: return null
    return "vkdriver=${GpuDriverHelper.driverInstallDir}|$lib|${GpuDriverHelper.hookLibDir}"
  }

  override fun onCreate(savedInstanceState: Bundle?) {
    discArg = openDisc()
    super.onCreate(savedInstanceState)
    OrientationLocker(this, landscapeOnly = true).enable()
    setupOnScreenController()
    setupOverlayText()
    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
      onBackInvokedDispatcher.registerOnBackInvokedCallback(OnBackInvokedDispatcher.PRIORITY_DEFAULT) { onBack() }
    }
  }

  /** The disc image's name without its extension. */
  private fun gameName(): String =
    discUri?.let { u -> runCatching { androidx.documentfile.provider.DocumentFile.fromSingleUri(this, u)?.name }.getOrNull() ?: u.lastPathSegment }
      ?.substringAfterLast('/')
      ?.replace(Regex("(\\.xiso)?\\.iso$", RegexOption.IGNORE_CASE), "") ?: "Game"

  /** The disc, opened in this process: the core reads it by descriptor. */
  private fun openDisc(): String {
    val uri = intent.getStringExtra(EXTRA_DVD_URI)
    discUri = uri?.takeIf { it.isNotBlank() }?.let(Uri::parse)
    val u = discUri ?: run {
      Log.e(TAG, "no game to run")
      return ""
    }
    return try {
      "fd:${contentResolver.openFileDescriptor(u, "r")!!.detachFd()}"
    } catch (e: Exception) {
      Log.e(TAG, "can't open $u: ${e.message}")
      "" /* the core reports NC_FATAL_DISC and the dialog goes back */
    }
  }

  // ---------------------------------------------------------------- pause

  private var activityPaused = false
  private var menuDialogs = 0
  private var nativePaused: Boolean? = null

  /** The emulation stops while the activity is paused or a menu dialog is open. */
  private fun syncPause() {
    val want = activityPaused || menuDialogs > 0
    if (nativePaused != want) {
      nativePaused = want
      nativeSetPaused(want)
    }
  }

  private fun showMenuDialog(b: AlertDialog.Builder): AlertDialog {
    menuDialogs++
    syncPause()
    b.setOnDismissListener {
      menuDialogs--
      syncPause()
    }
    return b.show()
  }

  override fun onResume() {
    super.onResume()
    activityPaused = false
    syncPause()
    hideSystemBars()
    fpsView?.visibility = if (prefs.getBoolean("show_fps", false)) View.VISIBLE else View.GONE
    handler.removeCallbacks(poll)
    handler.postDelayed(poll, POLL_MS)
    handler.removeCallbacks(rumble)
    handler.postDelayed(rumble, RUMBLE_MS)
  }

  override fun onPause() {
    handler.removeCallbacks(poll)
    handler.removeCallbacks(rumble)
    router?.reset()
    activityPaused = true
    syncPause()
    super.onPause()
  }

  override fun onDestroy() {
    handler.removeCallbacksAndMessages(null)
    (getSystemService(Context.INPUT_SERVICE) as InputManager).unregisterInputDeviceListener(this)
    super.onDestroy()
  }

  override fun onWindowFocusChanged(hasFocus: Boolean) {
    super.onWindowFocusChanged(hasFocus)
    if (hasFocus) hideSystemBars()
  }

  private fun hideSystemBars() {
    if (Build.VERSION.SDK_INT >= 30) {
      window.insetsController?.let {
        it.hide(WindowInsets.Type.systemBars())
        it.systemBarsBehavior = WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
      }
    }
  }

  // ---------------------------------------------------------------- what the core reports

  private var fatalShown = false

  private val poll = object : Runnable {
    override fun run() {
      if (fpsView?.visibility == View.VISIBLE) {
        val s = nativeGetFrameStats()
        if (s != null && s.size >= 3) {
          fpsView?.text = "hakuX  FPS: %.0f  |  %.1fms avg  /  %.1fms last".format(s[0], s[1], s[2])
        }
      }
      val why = nativeGetFatal()
      if (why != null && !fatalShown) {
        fatalShown = true
        showFatal(why, nativeGetFatalKind())
        return
      }
      /* The title rebooted into another of its XBEs: a new core process. */
      if (nativeGetRelaunch()) {
        relaunchCore()
        return
      }
      /* The title left (to the dashboard, a reboot, a shutdown): the game closes, as on a console. */
      if (nativeGetQuit()) {
        Log.i(TAG, "the title left: closing the game")
        exitGame()
        return
      }
      handler.postDelayed(this, POLL_MS)
    }
  }

  private fun showFatal(why: String, kind: Int) {
    val (title, message) = when (kind) {
      FATAL_DRIVER -> R.string.hakux_fatal_driver_title to getString(R.string.hakux_fatal_driver, why)
      FATAL_GPU_LOST -> R.string.hakux_fatal_gpu_lost_title to getString(R.string.hakux_fatal_gpu_lost, why)
      FATAL_BOOT, FATAL_DISC -> R.string.hakux_fatal_boot_title to getString(R.string.hakux_fatal_boot, why)
      else -> R.string.hakux_fatal_incompatible_title to getString(R.string.hakux_fatal_incompatible)
    }
    if (kind == FATAL_DISC) {
      // the file itself is bad: xemu would get the same one
      MaterialAlertDialogBuilder(this, R.style.ThemeOverlay_Xemu_RoundedDialog)
        .setTitle(title)
        .setMessage(message)
        .setCancelable(false)
        .setPositiveButton(R.string.in_game_menu_exit_to_library) { _, _ -> exitGame() }
        .show()
      return
    }
    // xemu emulates the whole console, so it may run what this core can't. Only
    // this run moves to xemu unless "always" is ticked, which the game's
    // per-game settings can undo.
    val always = com.google.android.material.checkbox.MaterialCheckBox(this).apply {
      setText(R.string.hakux_always_xemu)
    }
    val pad = (24 * resources.displayMetrics.density).roundToInt()
    val alwaysRow = android.widget.FrameLayout(this).apply {
      setPadding(pad - pad / 4, pad / 3, pad, 0)
      addView(always)
    }
    MaterialAlertDialogBuilder(this, R.style.ThemeOverlay_Xemu_RoundedDialog)
      .setTitle(title)
      .setMessage(message + "\n\n" + getString(R.string.hakux_try_xemu_hint))
      .apply { if (lastGamePath() != null) setView(alwaysRow) }
      .setCancelable(false)
      .setPositiveButton(R.string.hakux_try_xemu) { _, _ -> retryInXemu(always.isChecked) }
      .setNegativeButton(R.string.in_game_menu_exit_to_library) { _, _ -> exitGame() }
      .show()
  }

  /** The library's game this disc is (its per-game settings key), when the library launched it. */
  private fun lastGamePath(): String? {
    val prefs = getSharedPreferences("x1box_prefs", MODE_PRIVATE)
    val disc = discUri?.toString() ?: return null
    val launched = HakuxLaunch.discUri(prefs.getString("dvdUri", null), prefs.getString("dvdPath", null))
    return prefs.getString(HakuxLaunch.PREF_LAST_GAME, null)?.takeIf { launched == disc }
  }

  /** Back to the library, which starts this disc in xemu (GameLibraryActivity.EXTRA_RETRY_IN_XEMU). */
  private fun retryInXemu(always: Boolean) {
    val disc = discUri?.toString() ?: intent.getStringExtra(EXTRA_DVD_URI)
    startActivity(
      Intent(applicationContext, GameLibraryActivity::class.java)
        .putExtra(GameLibraryActivity.EXTRA_RETRY_IN_XEMU, disc)
        .apply { if (always) putExtra(GameLibraryActivity.EXTRA_RETRY_GAME_PATH, lastGamePath()) }
        .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_CLEAR_TOP)
    )
    finish()
    android.os.Process.killProcess(android.os.Process.myPid())
  }

  /** "Building shaders n / N" while the core builds the game's cached shaders before it starts. */
  private fun setupOverlayText() {
    val fps = TextView(this).apply {
      text = "hakuX  FPS: --"
      setTextColor(Color.WHITE)
      setTextSize(TypedValue.COMPLEX_UNIT_SP, 11f)
      typeface = Typeface.MONOSPACE
      setShadowLayer(2f, 1f, 1f, Color.BLACK)
      setPadding(16, 8, 16, 8)
      setBackgroundColor(Color.argb(100, 0, 0, 0))
      maxLines = 1
      visibility = View.GONE
    }
    mLayout?.addView(fps, RelativeLayout.LayoutParams(
      RelativeLayout.LayoutParams.WRAP_CONTENT, RelativeLayout.LayoutParams.WRAP_CONTENT,
    ).apply {
      addRule(RelativeLayout.ALIGN_PARENT_TOP)
      addRule(RelativeLayout.ALIGN_PARENT_START)
    })
    fpsView = fps

    val line = TextView(this).apply {
      visibility = View.GONE
      setTextColor(Color.WHITE)
      setTextSize(TypedValue.COMPLEX_UNIT_SP, 14f)
      setShadowLayer(2f, 1f, 1f, Color.BLACK)
      setPadding(24, 12, 24, 12)
      setBackgroundColor(Color.argb(140, 0, 0, 0))
    }
    mLayout?.addView(line, RelativeLayout.LayoutParams(
      RelativeLayout.LayoutParams.WRAP_CONTENT, RelativeLayout.LayoutParams.WRAP_CONTENT,
    ).apply { addRule(RelativeLayout.CENTER_IN_PARENT) })
    shaderLine = line
    val started = SystemClock.uptimeMillis()
    var seen = false
    handler.post(object : Runnable {
      override fun run() {
        val p = nativeGetShaderBuild()
        val total = p?.getOrNull(1) ?: 0
        if (total > 0) {
          seen = true
          line.text = getString(R.string.hakux_shader_build, p!![0], total)
          line.visibility = View.VISIBLE
        } else {
          line.visibility = View.GONE
          if (seen || SystemClock.uptimeMillis() - started > 30_000) return
        }
        handler.postDelayed(this, 200)
      }
    })
  }

  // ---------------------------------------------------------------- controls and menu

  private val controllerSettings by lazy { ControllerSettings(this) }
  /** Physical controllers to player 1 through the app's remap (HakuxPadRouter). */
  private var router: HakuxPadRouter? = null
  private var touchLayoutToolbar: View? = null
  private var menuDialog: androidx.appcompat.app.AlertDialog? = null

  private fun setupOnScreenController() {
    val settings = controllerSettings
    val overlay = OnScreenController(this).apply {
      layoutParams = ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT)
      alpha = settings.controllerOpacity
      setLayout(settings.touchLayout)
      onLayoutEdited = { settings.touchLayout = it }
    }
    overlay.setControllerListener(OverlayBridge())
    overlay.onMenuButtonTapped = { showMenu() }
    mLayout?.addView(overlay)
    onScreenController = overlay
    router = HakuxPadRouter(ControllerRemap.load(this)) { b, a, lx, ly, rx, ry -> nativeSetPad(0, b, a, lx, ly, rx, ry) }
    (getSystemService(Context.INPUT_SERVICE) as InputManager).registerInputDeviceListener(this, null)
    updateControllerVisibility()
  }

  /** The controls show while no gamepad is attached, unless the menu said otherwise. */
  private fun updateControllerVisibility() {
    val shown = controlsShown ?: !hasGamepad()
    onScreenController?.visibility = if (shown) View.VISIBLE else View.GONE
  }

  private fun hasGamepad(): Boolean =
    InputDevice.getDeviceIds().any { HakuxPadRouter.isGamepad(InputDevice.getDevice(it)) }

  override fun onInputDeviceAdded(deviceId: Int) = updateControllerVisibility()
  override fun onInputDeviceChanged(deviceId: Int) = updateControllerVisibility()
  override fun onInputDeviceRemoved(deviceId: Int) {
    router?.forget(deviceId)
    updateControllerVisibility()
  }

  /** The rumble the title asks for, on the controller used last. */
  private var rumbleOn = false
  private val rumble = object : Runnable {
    override fun run() {
      val v = nativeGetRumble(0)
      val amp = (maxOf(v ushr 16, v and 0xFFFF) ushr 8).coerceIn(0, 255)
      val dev = router?.lastDeviceId?.let { InputDevice.getDevice(it) }
      if (dev != null && Build.VERSION.SDK_INT >= 31) {
        val vib = dev.vibratorManager.defaultVibrator
        if (amp > 0 && vib.hasVibrator()) {
          vib.vibrate(android.os.VibrationEffect.createOneShot(RUMBLE_MS + 20, amp.coerceAtLeast(1)))
          rumbleOn = true
        } else if (rumbleOn) {
          vib.cancel()
          rumbleOn = false
        }
      }
      handler.postDelayed(this, RUMBLE_MS)
    }
  }

  /** The game's savestates, kept by its disc. */
  private val saveStates by lazy {
    HakuxSaveStates(
      this,
      gameKey = discUri?.toString() ?: intent.getStringExtra(EXTRA_DVD_URI) ?: "",
      gameName = gameName(),
      showDialog = ::showMenuDialog,
      captureFrame = ::captureFrame,
      restartInto = ::restartInto,
    )
  }

  /** The frame on screen, copied from the game's surface (null: it couldn't be). */
  private fun captureFrame(done: (Bitmap?) -> Unit) {
    val surface = mSurface
    if (surface == null || surface.width <= 0 || surface.height <= 0) {
      done(null)
      return
    }
    val frame = Bitmap.createBitmap(surface.width, surface.height, Bitmap.Config.ARGB_8888)
    try {
      PixelCopy.request(surface, frame, { result -> done(if (result == PixelCopy.SUCCESS) frame else null) }, handler)
    } catch (e: IllegalArgumentException) {
      done(null)
    }
  }

  /** The game stops while [what] is up (a dialog, the layout editor); [over] lets it go on. */
  private fun holdPause(): () -> Unit {
    menuDialogs++
    syncPause()
    var released = false
    return {
      if (!released) {
        released = true
        menuDialogs--
        syncPause()
      }
    }
  }

  /** The in-game menu, laid out as xemu's (MainActivity.showInGameMenu). */
  private fun showMenu() {
    if (menuDialog?.isShowing == true || touchLayoutToolbar != null) return
    onScreenController?.resetAllInputs()
    router?.reset()
    val release = holdPause()

    val dp = resources.displayMetrics.density
    val spacing = (8 * dp).toInt()
    lateinit var dialog: androidx.appcompat.app.AlertDialog
    class Spec(val label: String, val action: () -> Unit)

    fun button(spec: Spec) = MaterialButton(this, null, com.google.android.material.R.attr.materialButtonOutlinedStyle).apply {
      text = spec.label
      gravity = Gravity.CENTER
      textAlignment = View.TEXT_ALIGNMENT_CENTER
      isSingleLine = false
      maxLines = 2
      setOnClickListener {
        spec.action()                   /* a dialog it opens holds the pause before the menu lets go */
        dialog.dismiss()
      }
    }

    fun row(parent: LinearLayout, vararg specs: Spec) {
      parent.addView(LinearLayout(this).apply {
        orientation = LinearLayout.HORIZONTAL
        isBaselineAligned = false
        layoutParams = LinearLayout.LayoutParams(
          LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT,
        ).also { it.bottomMargin = spacing }
        specs.forEachIndexed { i, spec ->
          addView(button(spec).apply {
            layoutParams = LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f).also { lp ->
              if (i < specs.lastIndex) lp.marginEnd = spacing
            }
          })
        }
      })
    }

    val shown = onScreenController?.visibility == View.VISIBLE
    val resume = Spec(getString(R.string.in_game_menu_resume)) {}
    val touch = Spec(getString(if (shown) R.string.in_game_menu_hide_touch_controls else R.string.in_game_menu_show_touch_controls)) {
      controlsShown = !shown
      updateControllerVisibility()
    }
    val remap = Spec(getString(R.string.in_game_menu_remap_controller)) {
      val done = holdPause()
      ControllerRemap.showEditor(
        this,
        onChanged = { router?.setRemap(it) },
        onDismiss = {
          hideSystemBars()
          done()
        },
      )
    }
    val layout = Spec(getString(R.string.in_game_menu_edit_touch_layout)) { startTouchLayoutEditor() }
    val save = Spec(getString(R.string.in_game_menu_save_state)) { saveStates.showSlots(save = true) }
    val load = Spec(getString(R.string.in_game_menu_load_state)) { saveStates.showSlots(save = false) }
    val exit = Spec(getString(R.string.in_game_menu_exit_to_library)) { exitGame() }
    val quit = Spec(getString(R.string.in_game_menu_quit_app)) { quitApp() }

    val list = LinearLayout(this).apply {
      orientation = LinearLayout.VERTICAL
      setPadding((20 * dp).toInt(), (12 * dp).toInt(), (20 * dp).toInt(), 0)
      row(this, resume, touch)
      row(this, remap, layout)
      /* savestates: the core has them for push-buffer titles */
      if (nativeStatesAvailable()) row(this, save, load)
      row(this, exit, quit)
    }
    val scroll = androidx.core.widget.NestedScrollView(this).apply {
      isFillViewport = true
      overScrollMode = View.OVER_SCROLL_IF_CONTENT_SCROLLS
      addView(list, ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT))
    }
    dialog = MaterialAlertDialogBuilder(this, R.style.ThemeOverlay_Xemu_RoundedDialog)
      .setTitle(getString(R.string.in_game_menu_title))
      .setView(scroll)
      .setOnDismissListener {
        menuDialog = null
        hideSystemBars()
        release()
      }
      .create()
    menuDialog = dialog
    dialog.show()
  }

  /** The layout editor over the game, paused meanwhile (MainActivity's). */
  private var editorRelease: (() -> Unit)? = null

  private fun startTouchLayoutEditor() {
    val controller = onScreenController ?: return
    val host = mLayout ?: return
    editorRelease = holdPause()
    controller.visibility = View.VISIBLE
    controller.editMode = true

    val dp = resources.displayMetrics.density
    fun toolbarButton(label: String, onClick: () -> Unit) = MaterialButton(this).apply {
      text = label
      isAllCaps = false
      minWidth = 0
      minimumWidth = 0
      setPadding((14 * dp).toInt(), 0, (14 * dp).toInt(), 0)
      setOnClickListener { onClick() }
      layoutParams = LinearLayout.LayoutParams(
        LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT,
      ).also { it.marginEnd = (6 * dp).toInt() }
    }
    val opacitySteps = floatArrayOf(1.0f, 0.85f, 0.7f, 0.55f, 0.4f, 0.25f)
    fun opacityLabel() = getString(R.string.touch_layout_opacity, (controller.alpha * 100).roundToInt())

    val smaller = toolbarButton(getString(R.string.touch_layout_smaller)) { controller.scaleSelected(1f / 1.1f) }
    val larger = toolbarButton(getString(R.string.touch_layout_larger)) { controller.scaleSelected(1.1f) }
    val hide = toolbarButton(getString(R.string.touch_layout_hide)) { controller.toggleSelectedVisibility() }
    lateinit var opacity: MaterialButton
    opacity = toolbarButton(opacityLabel()) {
      val i = opacitySteps.indexOfFirst { abs(it - controller.alpha) < 0.01f }
      controller.alpha = opacitySteps[(i + 1) % opacitySteps.size]
      controllerSettings.controllerOpacity = controller.alpha
      opacity.text = opacityLabel()
    }
    val reset = toolbarButton(getString(R.string.touch_layout_reset)) {
      MaterialAlertDialogBuilder(this, R.style.ThemeOverlay_Xemu_RoundedDialog)
        .setMessage(R.string.touch_layout_reset_message)
        .setPositiveButton(R.string.touch_layout_reset) { _, _ ->
          controller.resetLayout()
          controller.alpha = 1f
          controllerSettings.controllerOpacity = 1f
          opacity.text = opacityLabel()
        }
        .setNegativeButton(android.R.string.cancel, null)
        .setOnDismissListener { hideSystemBars() }
        .show()
    }
    val done = toolbarButton(getString(R.string.touch_layout_done)) { finishTouchLayoutEditor() }

    fun updateSelectionButtons(element: OnScreenController.Element?) {
      val has = element != null
      smaller.isEnabled = has
      larger.isEnabled = has
      hide.isEnabled = has
      hide.text = getString(if (controller.isSelectedVisible()) R.string.touch_layout_hide else R.string.touch_layout_show)
    }
    controller.onSelectionChanged = ::updateSelectionButtons
    updateSelectionButtons(null)

    val row = LinearLayout(this).apply {
      orientation = LinearLayout.HORIZONTAL
      setPadding((8 * dp).toInt(), (6 * dp).toInt(), (8 * dp).toInt(), (6 * dp).toInt())
      setBackgroundColor(Color.argb(200, 20, 20, 20))
      listOf(smaller, larger, hide, opacity, reset, done).forEach(::addView)
    }
    val toolbar = HorizontalScrollView(this).apply {
      isHorizontalScrollBarEnabled = false
      addView(row)
    }
    host.addView(toolbar, RelativeLayout.LayoutParams(
      RelativeLayout.LayoutParams.WRAP_CONTENT, RelativeLayout.LayoutParams.WRAP_CONTENT,
    ).apply {
      addRule(RelativeLayout.ALIGN_PARENT_TOP)
      addRule(RelativeLayout.CENTER_HORIZONTAL)
      topMargin = (8 * dp).toInt()
    })
    touchLayoutToolbar = toolbar
  }

  private fun finishTouchLayoutEditor() {
    val toolbar = touchLayoutToolbar ?: return
    (toolbar.parent as? ViewGroup)?.removeView(toolbar)
    touchLayoutToolbar = null
    onScreenController?.apply {
      onSelectionChanged = null
      editMode = false
    }
    updateControllerVisibility()
    hideSystemBars()
    editorRelease?.invoke()
    editorRelease = null
  }

  @Deprecated("Android 12 and older; newer ones call the OnBackInvokedCallback.")
  override fun onBackPressed() = onBack()

  /** Back: leaves the layout editor, or opens (closes) the menu. */
  private fun onBack() {
    when {
      touchLayoutToolbar != null -> finishTouchLayoutEditor()
      menuDialog?.isShowing == true -> menuDialog?.dismiss()
      else -> showMenu()
    }
  }

  /* Start + Select together open the menu, as in xemu (MainActivity.handleGamepadMenuCombo). */
  private var startDown = false
  private var selectDown = false
  private var comboTriggered = false

  private fun menuCombo(event: KeyEvent): Boolean {
    val start = event.keyCode == KeyEvent.KEYCODE_BUTTON_START
    val select = event.keyCode == KeyEvent.KEYCODE_BUTTON_SELECT || event.keyCode == KeyEvent.KEYCODE_BACK
    if (!start && !select) return false
    when (event.action) {
      KeyEvent.ACTION_DOWN -> {
        if (start) startDown = true
        if (select) selectDown = true
        if (!comboTriggered && event.repeatCount == 0 && startDown && selectDown) {
          comboTriggered = true
          showMenu()
          return true
        }
      }
      KeyEvent.ACTION_UP -> {
        if (start) startDown = false
        if (select) selectDown = false
        if (!startDown || !selectDown) comboTriggered = false
      }
    }
    return comboTriggered
  }

  /*
   * The system's back (navigation button, back swipe, a keyboard) opens the
   * menu before SDL takes it and closes the game; a gamepad's keys go to the
   * game through the router, its guide button and Start + Select to the menu.
   */
  override fun dispatchKeyEvent(event: KeyEvent): Boolean {
    val fromPad = HakuxPadRouter.isGamepad(event.device)
    if (event.keyCode == KeyEvent.KEYCODE_BACK && !fromPad) {
      if (event.action == KeyEvent.ACTION_UP && !event.isCanceled) onBack()
      return true
    }
    if (fromPad) {
      if (event.keyCode == KeyEvent.KEYCODE_BUTTON_MODE) {
        if (event.action == KeyEvent.ACTION_UP && !event.isCanceled) showMenu()
        return true
      }
      if (menuCombo(event)) {
        router?.reset()
        return true
      }
      if (touchLayoutToolbar == null && router?.onKey(event) == true) {
        return true
      }
    }
    return super.dispatchKeyEvent(event)
  }

  override fun dispatchGenericMotionEvent(event: MotionEvent): Boolean {
    if (touchLayoutToolbar == null && router?.onMotion(event) == true) {
      return true
    }
    return super.dispatchGenericMotionEvent(event)
  }

  // ---------------------------------------------------------------- leaving

  /** Back to the library; the core keeps its state in this process for one run, so the process goes too. */
  private fun exitGame() {
    startActivity(
      Intent(applicationContext, GameLibraryActivity::class.java)
        .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_CLEAR_TOP or Intent.FLAG_ACTIVITY_SINGLE_TOP)
    )
    finish()
    android.os.Process.killProcess(android.os.Process.myPid())
  }

  /** Out of the app: the game's process ends (its saves go back to the disk the next time the library opens). */
  private fun quitApp() {
    finishAndRemoveTask()
    android.os.Process.killProcess(android.os.Process.myPid())
  }

  private var relaunchStarted = false

  /**
   * The same disc again in a new core process, booting the XBE the title
   * launched (the core saved its launch data page). HakuxRelaunchActivity, in
   * the main process, starts it once this one is gone, showing the frame on
   * screen now meanwhile.
   */
  private fun relaunchCore() {
    if (relaunchStarted) return
    Log.i(TAG, "relaunch: the title launched another XBE; starting the core again")
    val surface = mSurface
    if (surface != null && surface.width > 0 && surface.height > 0) {
      val frame = Bitmap.createBitmap(surface.width, surface.height, Bitmap.Config.ARGB_8888)
      val timeout = Runnable { startRelaunch() }
      try {
        PixelCopy.request(surface, frame, { result ->
          handler.removeCallbacks(timeout)
          if (result != PixelCopy.SUCCESS) {
            startRelaunch()
          } else {
            Thread {
              val file = HakuxRelaunchActivity.heldFrame(this)
              runCatching { file.outputStream().use { frame.compress(Bitmap.CompressFormat.JPEG, 92, it) } }
              runOnUiThread { startRelaunch() }
            }.start()
          }
        }, handler)
        handler.postDelayed(timeout, RELAUNCH_COPY_MS)
        return
      } catch (e: IllegalArgumentException) {
        Log.w(TAG, "relaunch: the frame can't be copied: ${e.message}")
      }
    }
    startRelaunch()
  }

  /** Starts the core again into the savestate at path (HakuxSaveStates' Load state). */
  private fun restartInto(path: String) {
    Log.i(TAG, "savestate: starting the core again into $path")
    startRelaunch(path)
  }

  private fun startRelaunch(state: String? = null) {
    if (relaunchStarted) return
    relaunchStarted = true
    startActivity(
      Intent(this, HakuxRelaunchActivity::class.java)
        .putExtra(EXTRA_DVD_URI, discUri?.toString() ?: intent.getStringExtra(EXTRA_DVD_URI))
        .apply { if (state != null) putExtra(EXTRA_LOADSTATE, state) }
        .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_CLEAR_TASK or Intent.FLAG_ACTIVITY_NO_ANIMATION),
      HakuxRelaunchActivity.noTransition(this).toBundle(),
    )
    finish()
    HakuxRelaunchActivity.noCloseTransition(this)
    android.os.Process.killProcess(android.os.Process.myPid())
  }

  // ---------------------------------------------------------------- the on-screen controller's pad

  /** The on-screen controller's whole state, sent to the core (player 1) on every change. */
  private class OverlayBridge : OnScreenController.ControllerListener {
    private var buttons = 0
    private var analog = 0
    private var lt = 0f
    private var rt = 0f
    private var lx = 0f
    private var ly = 0f
    private var rx = 0f
    private var ry = 0f

    override fun onButtonPressed(button: OnScreenController.Button) = set(button, true)
    override fun onButtonReleased(button: OnScreenController.Button) = set(button, false)

    override fun onStickMoved(stick: OnScreenController.Stick, x: Float, y: Float) {
      if (stick == OnScreenController.Stick.LEFT) { lx = x; ly = y } else { rx = x; ry = y }
      send()
    }

    override fun onStickPressed(stick: OnScreenController.Stick) =
      bit(if (stick == OnScreenController.Stick.LEFT) PAD_LTHUMB else PAD_RTHUMB, true)

    override fun onStickReleased(stick: OnScreenController.Stick) =
      bit(if (stick == OnScreenController.Stick.LEFT) PAD_LTHUMB else PAD_RTHUMB, false)

    private fun set(button: OnScreenController.Button, down: Boolean) {
      when (button) {
        OnScreenController.Button.A -> analogBit(0, down)
        OnScreenController.Button.B -> analogBit(1, down)
        OnScreenController.Button.X -> analogBit(2, down)
        OnScreenController.Button.Y -> analogBit(3, down)
        OnScreenController.Button.BLACK -> analogBit(4, down)
        OnScreenController.Button.WHITE -> analogBit(5, down)
        OnScreenController.Button.LEFT_TRIGGER -> { lt = if (down) 1f else 0f; send() }
        OnScreenController.Button.RIGHT_TRIGGER -> { rt = if (down) 1f else 0f; send() }
        OnScreenController.Button.DPAD_UP -> bit(PAD_UP, down)
        OnScreenController.Button.DPAD_DOWN -> bit(PAD_DOWN, down)
        OnScreenController.Button.DPAD_LEFT -> bit(PAD_LEFT, down)
        OnScreenController.Button.DPAD_RIGHT -> bit(PAD_RIGHT, down)
        OnScreenController.Button.START -> bit(PAD_START, down)
        OnScreenController.Button.BACK -> bit(PAD_BACK, down)
        OnScreenController.Button.LEFT_STICK_BUTTON -> bit(PAD_LTHUMB, down)
        OnScreenController.Button.RIGHT_STICK_BUTTON -> bit(PAD_RTHUMB, down)
      }
    }

    private fun analogBit(i: Int, down: Boolean) {
      analog = if (down) analog or (1 shl i) else analog and (1 shl i).inv()
      send()
    }

    private fun bit(mask: Int, down: Boolean) {
      buttons = if (down) buttons or mask else buttons and mask.inv()
      send()
    }

    private fun send() = nativeSetOverlayPad(buttons, analog, lx, ly, rx, ry, lt, rt)
  }

  companion object {
    private const val TAG = "x1box-hakux"
    const val EXTRA_DVD_URI = "dvdUri"
    /** Boot the XBE of the saved launch data page (a title's XLaunchNewImage). */
    const val EXTRA_RELAUNCH = "relaunch"
    /** Boot into the savestate at this path (HakuxSaveStates' Load state). */
    const val EXTRA_LOADSTATE = "loadstate"

    /* hakux/core/native/dev/usb.h NC_PAD_* */
    private const val PAD_UP = 0x01
    private const val PAD_DOWN = 0x02
    private const val PAD_LEFT = 0x04
    private const val PAD_RIGHT = 0x08
    private const val PAD_START = 0x10
    private const val PAD_BACK = 0x20
    private const val PAD_LTHUMB = 0x40
    private const val PAD_RTHUMB = 0x80

    /* hakux/core/native/hle/hle_native.h NC_FATAL_* */
    private const val FATAL_DRIVER = 1
    private const val FATAL_GPU_LOST = 4
    private const val FATAL_BOOT = 5
    private const val FATAL_DISC = 6

    private const val POLL_MS = 500L
    private const val RUMBLE_MS = 50L
    private const val RELAUNCH_COPY_MS = 1000L

    /** fps, average frame time (ms), last frame time (ms), 1 if push-buffer decoding renders. */
    @JvmStatic external fun nativeGetFrameStats(): FloatArray?
    /** Why the game can't run here (the GPU driver, or the game), or null. */
    @JvmStatic external fun nativeGetFatal(): String?
    @JvmStatic external fun nativeGetFatalKind(): Int
    /** The core stopped to boot another of the title's XBEs. */
    @JvmStatic external fun nativeGetRelaunch(): Boolean
    /** The core stopped as the title left (to the dashboard, a reboot, a shutdown). */
    @JvmStatic external fun nativeGetQuit(): Boolean
    /** The cached shaders' build before the game: {built, to build}, {0, 0} when none runs. */
    @JvmStatic external fun nativeGetShaderBuild(): IntArray?
    @JvmStatic external fun nativeSetOverlayPad(
      buttons: Int, analog: Int, lx: Float, ly: Float, rx: Float, ry: Float, lt: Float, rt: Float,
    )
    /** A player's Duke from its physical controllers (HakuxPadRouter): NC_PAD_* buttons, analog bytes, sticks. */
    @JvmStatic external fun nativeSetPad(player: Int, buttons: Int, analog: Long, lx: Int, ly: Int, rx: Int, ry: Int)
    /** The rumble the title last sent to a player: left << 16 | right. */
    @JvmStatic external fun nativeGetRumble(player: Int): Int
    /** The emulation paused (onPause, a menu) or resumed. */
    @JvmStatic external fun nativeSetPaused(paused: Boolean)
    /** Saves the running game to path (not on the UI thread): null, or why it couldn't. */
    @JvmStatic external fun nativeSaveState(path: String): String?
    /** Whether the savestate at path can load into this game: null, or why not. */
    @JvmStatic external fun nativeStateCheck(path: String): String?
    /** Savestates work for the running game (push-buffer mode). */
    @JvmStatic external fun nativeStatesAvailable(): Boolean
  }
}
