package com.izzy2lost.x1box

import android.content.Context
import android.content.Intent
import android.graphics.Bitmap
import android.hardware.input.InputManager
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.Process
import android.view.Gravity
import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.View
import android.view.ViewGroup
import android.view.WindowInsets
import android.view.WindowInsetsController
import android.view.WindowManager
import android.view.ViewConfiguration
import android.widget.BaseAdapter
import android.widget.FrameLayout
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.ListView
import android.widget.RelativeLayout
import android.widget.TextView
import android.widget.Toast
import androidx.core.widget.NestedScrollView
import com.google.android.material.button.MaterialButton
import androidx.appcompat.app.AlertDialog
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import org.libsdl.app.SDLActivity
import org.libsdl.app.SDLSurface
import java.io.File
import java.nio.ByteBuffer
import java.nio.ByteOrder
import kotlin.math.max

class MainActivity : SDLActivity(), InputManager.InputDeviceListener {
  companion object {
    const val EXTRA_AUTO_LOAD_SNAPSHOT_SLOT = "com.izzy2lost.x1box.extra.AUTO_LOAD_SNAPSHOT_SLOT"
    private const val SNAPSHOT_PREVIEW_HEADER_SIZE = 12
    private const val TOTAL_SNAPSHOT_SLOTS = 10
    private const val TAG = "MainActivity"
  }

  private data class SnapshotSlotPreview(
    val slot: Int,
    val slotLabel: String,
    val gameTitle: String,
    val thumbnail: Bitmap?,
  )

  private var onScreenController: OnScreenController? = null
  private var controllerBridge: ControllerInputBridge? = null
  private var isControllerVisible = false
  private var inputManager: InputManager? = null
  private var hasPhysicalController = false
  private var inGameMenuDialog: AlertDialog? = null
  private var startButtonDown = false
  private var selectButtonDown = false
  private var comboTriggered = false
  private var suspendedByLifecycle = false
  private var resumeEmulationOnMenuDismiss = false
  private var startupSnapshotSlot: Int? = null
  private var startupSnapshotLoadScheduled = false
  private lateinit var swipeUpGestureRecognizer: SwipeUpGestureRecognizer

  override fun loadLibraries() {
    super.loadLibraries()
    initializeGpuDriver()
  }

  private fun initializeGpuDriver() {
    GpuDriverHelper.init(this)
    if (GpuDriverHelper.supportsCustomDriverLoading()) {
      val driverLib = GpuDriverHelper.getInstalledDriverLibrary()
      if (driverLib != null) {
        android.util.Log.i(TAG, "GPU driver: loading custom driver=$driverLib")
        GpuDriverHelper.initializeDriver(driverLib)
      } else {
        android.util.Log.i(TAG, "GPU driver: no custom driver installed, initializing system driver via adrenotools")
        GpuDriverHelper.initializeDriver()
      }
    } else {
      android.util.Log.i(TAG, "GPU driver: custom loading not supported on this device")
    }
  }

  override fun createSDLSurface(context: Context): SDLSurface {
    return super.createSDLSurface(context).apply {
      layoutParams = RelativeLayout.LayoutParams(
        ViewGroup.LayoutParams.MATCH_PARENT,
        ViewGroup.LayoutParams.MATCH_PARENT
      )
      keepScreenOn = true
    }
  }

  override fun onCreate(savedInstanceState: Bundle?) {
    super.onCreate(savedInstanceState)
    DebugLog.initialize(this)
    OrientationLocker(this, landscapeOnly = true).enable()
    window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
    val requestedSlot = intent?.getIntExtra(EXTRA_AUTO_LOAD_SNAPSHOT_SLOT, 0) ?: 0
    if (requestedSlot in 1..TOTAL_SNAPSHOT_SLOTS) {
      startupSnapshotSlot = requestedSlot
    }
    initializeSwipeMenuGesture()
    setupOnScreenController()
    setupControllerDetection()
    hideSystemUI()
  }

  override fun onWindowFocusChanged(hasFocus: Boolean) {
    super.onWindowFocusChanged(hasFocus)
    if (hasFocus) {
      hideSystemUI()
    } else {
      // Release all on-screen inputs when the window loses focus (e.g. a system
      // gesture panel, notification shade, or dialog appears). Without this,
      // triggers can stay "pressed" if the touch UP event is never delivered.
      onScreenController?.resetAllInputs()
      controllerBridge?.reset()
    }
  }

  override fun onBackPressed() {
    val currentDialog = inGameMenuDialog
    if (currentDialog?.isShowing == true) {
      currentDialog.dismiss()
      return
    }
    showInGameMenu()
  }

  override fun dispatchKeyEvent(event: KeyEvent): Boolean {
    if (event.keyCode == KeyEvent.KEYCODE_BACK && !isGamepadKeyEvent(event)) {
      if (event.action == KeyEvent.ACTION_UP && event.repeatCount == 0) {
        val currentDialog = inGameMenuDialog
        if (currentDialog?.isShowing == true) {
          currentDialog.dismiss()
        } else {
          showInGameMenu()
        }
      }
      return true
    }

    if (handleGamepadMenuCombo(event)) {
      return true
    }
    return super.dispatchKeyEvent(event)
  }

  override fun dispatchTouchEvent(event: MotionEvent): Boolean {
    if (shouldHandleActivitySwipeMenu() && swipeUpGestureRecognizer.onTouchEvent(event)) {
      return true
    }
    if (!shouldHandleActivitySwipeMenu()) {
      swipeUpGestureRecognizer.reset()
    }
    return super.dispatchTouchEvent(event)
  }

  private fun hideSystemUI() {
    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
      // Android 11 (API 30) and above
      @Suppress("DEPRECATION")
      window.setDecorFitsSystemWindows(false)
      window.insetsController?.let { controller ->
        controller.hide(WindowInsets.Type.systemBars())
        controller.systemBarsBehavior = WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
      }
    } else {
      // Android 10 and below
      @Suppress("DEPRECATION")
      window.decorView.systemUiVisibility = (
        View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
        or View.SYSTEM_UI_FLAG_FULLSCREEN
        or View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
        or View.SYSTEM_UI_FLAG_LAYOUT_STABLE
        or View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
        or View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
      )
    }
  }

  private fun initializeSwipeMenuGesture() {
    val swipeTouchSlop = ViewConfiguration.get(this).scaledTouchSlop.toFloat()
    swipeUpGestureRecognizer = SwipeUpGestureRecognizer(
      minDistancePx = {
        max(currentGestureHostHeight() * 0.14f, swipeTouchSlop * 8f)
      },
      touchSlopPx = { swipeTouchSlop },
      canStartAt = { _, y ->
        y >= currentGestureHostHeight() * 0.35f
      },
      onTriggered = {
        onScreenController?.resetAllInputs()
        showInGameMenu()
      },
    )
  }

  private fun setupOnScreenController() {
    // Create on-screen controller
    onScreenController = OnScreenController(this).apply {
      layoutParams = FrameLayout.LayoutParams(
        FrameLayout.LayoutParams.MATCH_PARENT,
        FrameLayout.LayoutParams.MATCH_PARENT
      )
    }

    // Create input bridge
    controllerBridge = ControllerInputBridge()
    onScreenController?.setControllerListener(controllerBridge!!)
    onScreenController?.onMenuButtonTapped = { showInGameMenu() }

    // Add to layout
    mLayout?.addView(onScreenController)

    // Check for existing controllers and show/hide accordingly
    updateControllerVisibility()
  }

  override fun onResume() {
    super.onResume()
    if (suspendedByLifecycle) {
      nativeResumeEmulation()
      suspendedByLifecycle = false
    }
    OrientationLocker(this, landscapeOnly = true).enable()
    window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
    mLayout?.keepScreenOn = true
    
    // Register virtual controller after SDL is initialized
    // Use a delay to ensure SDL is fully ready
    mLayout?.postDelayed({
      registerVirtualController()
    }, 1000)

    scheduleStartupSnapshotLoadIfRequested()
  }

  override fun onPause() {
    swipeUpGestureRecognizer.reset()
    onScreenController?.resetAllInputs()
    controllerBridge?.reset()
    resumeEmulationOnMenuDismiss = false
    suspendedByLifecycle = true
    nativePauseEmulation()
    super.onPause()
  }

  private fun resumeEmulationIfSafe() {
    val destroyed =
      Build.VERSION.SDK_INT >= Build.VERSION_CODES.JELLY_BEAN_MR1 && isDestroyed
    if (!suspendedByLifecycle && !isFinishing && !destroyed) {
      nativeResumeEmulation()
    }
  }

  private fun scheduleStartupSnapshotLoadIfRequested() {
    val slot = startupSnapshotSlot ?: return
    if (startupSnapshotLoadScheduled) {
      return
    }
    startupSnapshotLoadScheduled = true

    val hostView = mLayout ?: window.decorView
    hostView.postDelayed({
      Thread {
        val ok = nativeLoadSnapshot(slotName(slot))
        runOnUiThread {
          if (ok) {
            writeSnapshotTitleFallback(slot)
          }
          val msg = if (ok) {
            getString(R.string.snapshot_loaded, slot)
          } else {
            getString(R.string.snapshot_load_failed, slot)
          }
          Toast.makeText(this, msg, Toast.LENGTH_SHORT).show()
        }
      }.start()
      startupSnapshotSlot = null
    }, 2500)
  }

  private fun registerVirtualController() {
    try {
      // Register the virtual on-screen controller as a joystick device
      // Device ID: -2, Name: "On-Screen Controller"
      org.libsdl.app.SDLControllerManager.nativeAddJoystick(
        -2, // device_id
        "On-Screen Controller", // name
        "Virtual touchscreen controller", // desc
        0x045e, // vendor_id (Microsoft)
        0x028e, // product_id (Xbox 360 Controller)
        false, // is_accelerometer
        0xFFFF, // button_mask (all buttons)
        6, // naxes (left X/Y, right X/Y, left trigger, right trigger)
        0x3F, // axis_mask (6 axes)
        0, // nhats
        0  // nballs
      )
      DebugLog.d("MainActivity") { "Virtual controller registered successfully" }
    } catch (e: Exception) {
      DebugLog.e("MainActivity", e) { "Failed to register virtual controller: ${e.message}" }
    }
  }

  private fun setupControllerDetection() {
    inputManager = getSystemService(Context.INPUT_SERVICE) as InputManager
    inputManager?.registerInputDeviceListener(this, null)
    
    // Check for already connected controllers
    checkForPhysicalControllers()
  }

  private fun checkForPhysicalControllers() {
    val deviceIds = inputManager?.inputDeviceIds ?: return
    hasPhysicalController = deviceIds.any { deviceId ->
      val device = inputManager?.getInputDevice(deviceId)
      isGameController(device)
    }
    updateControllerVisibility()
  }

  private fun isGameController(device: InputDevice?): Boolean {
    if (device == null) return false
    
    val sources = device.sources
    
    // Check if device is a gamepad or joystick
    return ((sources and InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD) ||
           ((sources and InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK)
  }

  private fun updateControllerVisibility() {
    // Show on-screen controller only if no physical controller is connected
    val shouldShow = !hasPhysicalController
    
    if (shouldShow != isControllerVisible) {
      isControllerVisible = shouldShow
      swipeUpGestureRecognizer.reset()
      onScreenController?.visibility = if (shouldShow) View.VISIBLE else View.GONE
    }
  }

  // InputDeviceListener callbacks
  override fun onInputDeviceAdded(deviceId: Int) {
    val device = inputManager?.getInputDevice(deviceId)
    if (isGameController(device)) {
      hasPhysicalController = true
      updateControllerVisibility()
    }
  }

  override fun onInputDeviceRemoved(deviceId: Int) {
    // Recheck all devices to see if any controllers remain
    checkForPhysicalControllers()
  }

  override fun onInputDeviceChanged(deviceId: Int) {
    // Recheck all devices in case configuration changed
    checkForPhysicalControllers()
  }

  override fun onDestroy() {
    DebugLog.i(TAG) { "onDestroy()" }
    swipeUpGestureRecognizer.reset()
    resumeEmulationOnMenuDismiss = false
    inGameMenuDialog?.dismiss()
    inGameMenuDialog = null

    // Unregister virtual controller
    try {
      org.libsdl.app.SDLControllerManager.nativeRemoveJoystick(-2)
    } catch (e: Exception) {
      DebugLog.e("MainActivity", e) { "Failed to unregister virtual controller: ${e.message}" }
    }
    
    inputManager?.unregisterInputDeviceListener(this)
    window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
    super.onDestroy()
  }

  override fun onUserLeaveHint() {
    DebugLog.i(TAG) { "onUserLeaveHint()" }
    super.onUserLeaveHint()
  }

  override fun onTrimMemory(level: Int) {
    DebugLog.i(TAG) { "onTrimMemory(level=$level)" }
    super.onTrimMemory(level)
  }

  override fun onLowMemory() {
    DebugLog.w(TAG) { "onLowMemory()" }
    super.onLowMemory()
  }

  // Manual control methods (for settings/preferences)
  fun toggleOnScreenController() {
    isControllerVisible = !isControllerVisible
    onScreenController?.visibility = if (isControllerVisible) View.VISIBLE else View.GONE
  }

  fun showOnScreenController() {
    isControllerVisible = true
    onScreenController?.visibility = View.VISIBLE
  }

  fun hideOnScreenController() {
    isControllerVisible = false
    onScreenController?.visibility = View.GONE
  }

  fun forceUpdateControllerVisibility() {
    checkForPhysicalControllers()
  }

  private fun handleGamepadMenuCombo(event: KeyEvent): Boolean {
    if (!isGamepadKeyEvent(event)) {
      return false
    }

    val isStartKey = event.keyCode == KeyEvent.KEYCODE_BUTTON_START
    val isSelectKey = event.keyCode == KeyEvent.KEYCODE_BUTTON_SELECT ||
      event.keyCode == KeyEvent.KEYCODE_BACK
    if (!isStartKey && !isSelectKey) {
      return false
    }

    when (event.action) {
      KeyEvent.ACTION_DOWN -> {
        if (isStartKey) {
          startButtonDown = true
        }
        if (isSelectKey) {
          selectButtonDown = true
        }

        if (!comboTriggered && event.repeatCount == 0 &&
          startButtonDown && selectButtonDown) {
          comboTriggered = true
          showInGameMenu()
          return true
        }
      }
      KeyEvent.ACTION_UP -> {
        if (isStartKey) {
          startButtonDown = false
        }
        if (isSelectKey) {
          selectButtonDown = false
        }
        if (!startButtonDown || !selectButtonDown) {
          comboTriggered = false
        }
      }
    }

    return comboTriggered
  }

  private fun isGamepadKeyEvent(event: KeyEvent): Boolean {
    val source = event.source
    return ((source and InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD) ||
      ((source and InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK)
  }

  private external fun nativeSaveSnapshot(name: String): Boolean
  private external fun nativeLoadSnapshot(name: String): Boolean
  private external fun nativeRebootSystem()
  private external fun nativePauseEmulation()
  private external fun nativeResumeEmulation()
  private external fun nativeExitEmulation()

  private fun slotName(slot: Int) = "android_slot_$slot"

  private fun snapshotPreviewDir(): File = File(filesDir, "x1box/snapshots")

  private fun snapshotPreviewDirs(): List<File> {
    val dirs = ArrayList<File>(2)
    dirs.add(snapshotPreviewDir())
    getExternalFilesDir(null)?.let { dirs.add(File(it, "x1box/snapshots")) }
    return dirs.distinctBy { it.absolutePath }
  }

  private fun slotNameAliases(slot: Int): List<String> {
    val aliases = linkedSetOf(
      slotName(slot),
      "slot_$slot",
      "slot$slot",
      "snapshot_$slot",
    )
    return aliases.toList()
  }

  private fun resolveSnapshotPreviewFile(slot: Int, extension: String): File? {
    for (dir in snapshotPreviewDirs()) {
      for (name in slotNameAliases(slot)) {
        val file = File(dir, "$name.$extension")
        if (file.isFile) {
          return file
        }
      }
    }
    return null
  }

  private fun snapshotPreviewTitleFile(slot: Int): File =
    File(snapshotPreviewDir(), "${slotName(slot)}.title")

  private fun extractDisplayName(rawName: String?): String? {
    if (rawName.isNullOrBlank()) {
      return null
    }
    val decoded = Uri.decode(rawName)
    val leaf = decoded.substringAfterLast('/').substringAfterLast(':')
    if (leaf.isBlank()) {
      return null
    }
    val stem = leaf.substringBeforeLast('.', leaf).trim()
    return stem.takeIf { it.isNotEmpty() }
  }

  private fun fallbackCurrentGameName(): String {
    val prefs = getSharedPreferences("x1box_prefs", MODE_PRIVATE)
    val pathName = extractDisplayName(prefs.getString("dvdPath", null)?.let { File(it).name })
    if (!pathName.isNullOrEmpty()) {
      return pathName
    }
    val uriName = extractDisplayName(prefs.getString("dvdUri", null))
    if (!uriName.isNullOrEmpty()) {
      return uriName
    }
    return getString(R.string.snapshot_unknown_game)
  }

  private fun writeSnapshotTitleFallback(slot: Int) {
    val file = snapshotPreviewTitleFile(slot)
    if (runCatching { file.exists() && file.readText(Charsets.UTF_8).trim().isNotEmpty() }.getOrDefault(false)) {
      return
    }

    val title = fallbackCurrentGameName().trim()
    if (title.isEmpty() || title == getString(R.string.snapshot_unknown_game)) {
      return
    }

    runCatching {
      val dir = snapshotPreviewDir()
      if (!dir.exists()) {
        dir.mkdirs()
      }
      file.writeText(title, Charsets.UTF_8)
    }
  }

  private fun readSnapshotGameTitle(slot: Int): String {
    val title = runCatching {
      val file = resolveSnapshotPreviewFile(slot, "title")
      if (file != null && file.exists()) {
        file.readText(Charsets.UTF_8).trim()
      } else {
        ""
      }
    }.getOrDefault("")

    if (title.isNotEmpty()) {
      return title
    }

    return if (resolveSnapshotPreviewFile(slot, "thm") != null) {
      fallbackCurrentGameName()
    } else {
      getString(R.string.snapshot_empty_slot)
    }
  }

  private fun decodeSnapshotThumbnail(slot: Int): Bitmap? {
    val sourceFile = resolveSnapshotPreviewFile(slot, "thm") ?: return null
    val bytes = runCatching { sourceFile.readBytes() }.getOrNull() ?: return null
    if (bytes.size < SNAPSHOT_PREVIEW_HEADER_SIZE) {
      return null
    }

    if (bytes[0] != 'X'.code.toByte() ||
      bytes[1] != '1'.code.toByte() ||
      bytes[2] != 'T'.code.toByte() ||
      bytes[3] != 'H'.code.toByte()) {
      return null
    }

    val header = ByteBuffer.wrap(bytes, 4, 8).order(ByteOrder.LITTLE_ENDIAN)
    val version = header.short.toInt() and 0xFFFF
    val width = header.short.toInt() and 0xFFFF
    val height = header.short.toInt() and 0xFFFF
    val channels = header.short.toInt() and 0xFFFF

    if (version != 1 || channels != 4 || width <= 0 || height <= 0) {
      return null
    }

    val pixelBytesLong = width.toLong() * height.toLong() * channels.toLong()
    if (pixelBytesLong <= 0 || pixelBytesLong > Int.MAX_VALUE) {
      return null
    }

    val pixelBytes = pixelBytesLong.toInt()
    if (bytes.size < SNAPSHOT_PREVIEW_HEADER_SIZE + pixelBytes) {
      return null
    }

    val pixels = IntArray(width * height)
    var src = SNAPSHOT_PREVIEW_HEADER_SIZE
    for (y in 0 until height) {
      val dstRow = (height - 1 - y) * width
      for (x in 0 until width) {
        val r = bytes[src].toInt() and 0xFF
        val g = bytes[src + 1].toInt() and 0xFF
        val b = bytes[src + 2].toInt() and 0xFF
        pixels[dstRow + x] = (0xFF shl 24) or (r shl 16) or (g shl 8) or b
        src += 4
      }
    }

    return Bitmap.createBitmap(pixels, width, height, Bitmap.Config.ARGB_8888)
  }

  private fun loadSnapshotSlotPreviews(): List<SnapshotSlotPreview> {
    return (1..TOTAL_SNAPSHOT_SLOTS).map { slot ->
      SnapshotSlotPreview(
        slot = slot,
        slotLabel = getString(R.string.snapshot_slot_label, slot),
        gameTitle = readSnapshotGameTitle(slot),
        thumbnail = decodeSnapshotThumbnail(slot),
      )
    }
  }

  private fun showSnapshotPreviewDialog(preview: SnapshotSlotPreview) {
    val bitmap = preview.thumbnail ?: return
    val image = ImageView(this).apply {
      setImageBitmap(bitmap)
      adjustViewBounds = true
      scaleType = ImageView.ScaleType.FIT_CENTER
      setPadding(16, 16, 16, 16)
    }

    MaterialAlertDialogBuilder(this, R.style.ThemeOverlay_Xemu_RoundedDialog)
      .setTitle(getString(R.string.snapshot_preview_title, preview.slot, preview.gameTitle))
      .setView(image)
      .setPositiveButton(android.R.string.ok, null)
      .show()
  }

  private fun runSnapshotOperation(slot: Int, save: Boolean) {
    Thread {
      val ok = if (save) nativeSaveSnapshot(slotName(slot)) else nativeLoadSnapshot(slotName(slot))
      runOnUiThread {
        if (ok) {
          writeSnapshotTitleFallback(slot)
        }
        val msg = if (save) {
          if (ok) getString(R.string.snapshot_saved, slot) else getString(R.string.snapshot_save_failed)
        } else {
          if (ok) getString(R.string.snapshot_loaded, slot) else getString(R.string.snapshot_load_failed, slot)
        }
        Toast.makeText(this, msg, Toast.LENGTH_SHORT).show()
        hideSystemUI()
        resumeEmulationIfSafe()
      }
    }.start()
  }

  private fun showSnapshotSlotDialog(save: Boolean) {
    val previews = loadSnapshotSlotPreviews()
    val listView = ListView(this)
    lateinit var dialog: AlertDialog
    var operationStarted = false

    val adapter = object : BaseAdapter() {
      override fun getCount(): Int = previews.size
      override fun getItem(position: Int): SnapshotSlotPreview = previews[position]
      override fun getItemId(position: Int): Long = previews[position].slot.toLong()

      override fun getView(position: Int, convertView: View?, parent: ViewGroup): View {
        val view = convertView ?: layoutInflater.inflate(R.layout.item_snapshot_slot, parent, false)
        val preview = getItem(position)

        val slotLabel = view.findViewById<TextView>(R.id.snapshot_slot_label)
        val gameTitle = view.findViewById<TextView>(R.id.snapshot_game_title)
        val previewHint = view.findViewById<TextView>(R.id.snapshot_preview_hint)
        val thumbnail = view.findViewById<ImageView>(R.id.snapshot_thumbnail)

        slotLabel.text = preview.slotLabel
        gameTitle.text = preview.gameTitle

        if (preview.thumbnail != null) {
          thumbnail.setImageBitmap(preview.thumbnail)
          previewHint.text = getString(R.string.snapshot_preview_tap_hint)
          previewHint.visibility = View.VISIBLE
          thumbnail.setOnClickListener {
            showSnapshotPreviewDialog(preview)
          }
        } else {
          thumbnail.setImageResource(R.drawable.ic_xemu_image_placeholder)
          previewHint.text = getString(R.string.snapshot_preview_unavailable)
          previewHint.visibility = View.VISIBLE
          thumbnail.setOnClickListener(null)
        }

        return view
      }
    }

    listView.adapter = adapter
    listView.setOnItemClickListener { _, _, position, _ ->
      val slot = previews[position].slot
      operationStarted = true
      dialog.dismiss()
      runSnapshotOperation(slot, save)
    }

    dialog = MaterialAlertDialogBuilder(this, R.style.ThemeOverlay_Xemu_RoundedDialog)
      .setTitle(
        if (save) getString(R.string.snapshot_select_save_slot)
        else getString(R.string.snapshot_select_load_slot)
      )
      .setView(listView)
      .setNegativeButton(android.R.string.cancel, null)
      .setOnDismissListener {
        hideSystemUI()
        if (!operationStarted) {
          resumeEmulationIfSafe()
        }
      }
      .create()

    dialog.show()
  }

  private fun showSaveStateDialog() {
    showSnapshotSlotDialog(save = true)
  }

  private fun showLoadStateDialog() {
    showSnapshotSlotDialog(save = false)
  }

  private fun showRebootSystemConfirmation() {
    var confirmed = false
    MaterialAlertDialogBuilder(this, R.style.ThemeOverlay_Xemu_RoundedDialog)
      .setTitle(R.string.in_game_menu_reboot_title)
      .setMessage(R.string.in_game_menu_reboot_message)
      .setPositiveButton(R.string.in_game_menu_reboot_action) { _, _ ->
        confirmed = true
        onScreenController?.resetAllInputs()
        nativeRebootSystem()
      }
      .setNegativeButton(android.R.string.cancel, null)
      .setOnDismissListener {
        hideSystemUI()
        if (!confirmed) {
          resumeEmulationIfSafe()
        }
      }
      .show()
  }

  private fun showInGameMenu() {
    swipeUpGestureRecognizer.reset()
    if (inGameMenuDialog?.isShowing == true) {
      return
    }
    nativePauseEmulation()
    resumeEmulationOnMenuDismiss = true

    val dp = resources.displayMetrics.density
    val verticalButtonSpacing = (8 * dp).toInt()
    val horizontalButtonSpacing = (8 * dp).toInt()
    lateinit var dialog: androidx.appcompat.app.AlertDialog
    data class MenuButtonSpec(
      val label: String,
      val resumeAfterDismiss: Boolean = true,
      val action: () -> Unit,
    )

    fun createMenuButton(spec: MenuButtonSpec): MaterialButton {
      return MaterialButton(
        this@MainActivity,
        null,
        com.google.android.material.R.attr.materialButtonOutlinedStyle
      ).apply {
        text = spec.label
        gravity = Gravity.CENTER
        textAlignment = View.TEXT_ALIGNMENT_CENTER
        isSingleLine = false
        maxLines = 2
        setOnClickListener {
          resumeEmulationOnMenuDismiss = spec.resumeAfterDismiss
          dialog.dismiss()
          spec.action()
        }
      }
    }

    fun addSingleButton(parent: LinearLayout, spec: MenuButtonSpec) {
      parent.addView(createMenuButton(spec).apply {
        layoutParams = LinearLayout.LayoutParams(
          LinearLayout.LayoutParams.MATCH_PARENT,
          LinearLayout.LayoutParams.WRAP_CONTENT
        ).also { lp ->
          lp.bottomMargin = verticalButtonSpacing
        }
      })
    }

    fun addButtonRow(parent: LinearLayout, left: MenuButtonSpec, right: MenuButtonSpec) {
      parent.addView(
        LinearLayout(this).apply {
          orientation = LinearLayout.HORIZONTAL
          layoutParams = LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT,
            LinearLayout.LayoutParams.WRAP_CONTENT
          ).also { lp ->
            lp.bottomMargin = verticalButtonSpacing
          }
          addView(createMenuButton(left).apply {
            layoutParams = LinearLayout.LayoutParams(
              0,
              LinearLayout.LayoutParams.WRAP_CONTENT,
              1f
            ).also { lp ->
              lp.marginEnd = horizontalButtonSpacing
            }
          })
          addView(createMenuButton(right).apply {
            layoutParams = LinearLayout.LayoutParams(
              0,
              LinearLayout.LayoutParams.WRAP_CONTENT,
              1f
            )
          })
        }
      )
    }

    val resumeButton = MenuButtonSpec(getString(R.string.in_game_menu_resume)) { /* Resume */ }
    val touchControlsButton = MenuButtonSpec(
      if (isControllerVisible) {
        getString(R.string.in_game_menu_hide_touch_controls)
      } else {
        getString(R.string.in_game_menu_show_touch_controls)
      }
    ) {
      toggleOnScreenController()
    }
    val saveStateButton = MenuButtonSpec(
      getString(R.string.in_game_menu_save_state),
      resumeAfterDismiss = false
    ) {
      showSaveStateDialog()
    }
    val loadStateButton = MenuButtonSpec(
      getString(R.string.in_game_menu_load_state),
      resumeAfterDismiss = false
    ) {
      showLoadStateDialog()
    }
    val rebootButton = MenuButtonSpec(
      getString(R.string.in_game_menu_reboot_system),
      resumeAfterDismiss = false
    ) {
      showRebootSystemConfirmation()
    }
    val exitToLibraryButton = MenuButtonSpec(
      getString(R.string.in_game_menu_exit_to_library),
      resumeAfterDismiss = false
    ) {
      exitToGameLibrary()
    }
    val quitAppButton = MenuButtonSpec(
      getString(R.string.in_game_menu_quit_app),
      resumeAfterDismiss = false
    ) {
      quitApp()
    }

    val buttonList = LinearLayout(this).apply {
      orientation = LinearLayout.VERTICAL
      setPadding((20 * dp).toInt(), (12 * dp).toInt(), (20 * dp).toInt(), 0)
      addSingleButton(this, resumeButton)
      addSingleButton(this, touchControlsButton)
      addButtonRow(this, saveStateButton, loadStateButton)
      addSingleButton(this, rebootButton)
      addButtonRow(this, exitToLibraryButton, quitAppButton)
    }

    val scrollContainer = NestedScrollView(this).apply {
      isFillViewport = true
      overScrollMode = View.OVER_SCROLL_IF_CONTENT_SCROLLS
      addView(
        buttonList,
        ViewGroup.LayoutParams(
          ViewGroup.LayoutParams.MATCH_PARENT,
          ViewGroup.LayoutParams.WRAP_CONTENT
        )
      )
    }

    dialog = MaterialAlertDialogBuilder(this, R.style.ThemeOverlay_Xemu_RoundedDialog)
      .setTitle(getString(R.string.in_game_menu_title))
      .setView(scrollContainer)
      .setOnDismissListener {
        inGameMenuDialog = null
        hideSystemUI()
        if (resumeEmulationOnMenuDismiss) {
          resumeEmulationIfSafe()
        }
        resumeEmulationOnMenuDismiss = false
      }
      .create()

    inGameMenuDialog = dialog
    dialog.show()
  }

  private fun shouldHandleActivitySwipeMenu(): Boolean {
    return !isControllerVisible && inGameMenuDialog?.isShowing != true
  }

  private fun currentGestureHostHeight(): Float {
    val hostHeight = mLayout?.height ?: window.decorView.height
    return max(hostHeight, 1).toFloat()
  }

  private fun exitToGameLibrary() {
    nativeExitEmulation()
    val intent = Intent(this, GameLibraryActivity::class.java).apply {
      addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_CLEAR_TOP)
    }
    startActivity(intent)
    finish()
  }

  private fun quitApp() {
    nativeExitEmulation()
    finishAffinity()
  }

  override fun getLibraries(): Array<String> = arrayOf(
    "SDL2",
    "xemu",
  )
}
