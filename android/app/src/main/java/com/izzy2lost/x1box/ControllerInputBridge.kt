package com.izzy2lost.x1box

import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.view.KeyEvent
import org.libsdl.app.SDLControllerManager

/**
 * Bridge between on-screen controller and SDL input system
 */
class ControllerInputBridge : OnScreenController.ControllerListener {

  private data class ButtonDispatchState(
    var isDown: Boolean = false,
    var pressedAtMs: Long = 0L,
    var pendingRelease: Runnable? = null,
  )

  companion object {
    // Virtual device ID for on-screen controller
    const val VIRTUAL_DEVICE_ID = -2
    private const val MIN_TAP_HOLD_MS = 50L

    // Axis indices for SDL
    const val AXIS_LEFT_X = 0
    const val AXIS_LEFT_Y = 1
    const val AXIS_RIGHT_X = 2
    const val AXIS_RIGHT_Y = 3
    const val AXIS_LEFT_TRIGGER = 4
    const val AXIS_RIGHT_TRIGGER = 5
  }

  private val mainHandler = Handler(Looper.getMainLooper())
  private val buttonStates = mutableMapOf<OnScreenController.Button, ButtonDispatchState>()

  override fun onButtonPressed(button: OnScreenController.Button) {
    val state = buttonStates.getOrPut(button) { ButtonDispatchState() }
    state.pendingRelease?.let(mainHandler::removeCallbacks)
    state.pendingRelease = null
    state.pressedAtMs = SystemClock.uptimeMillis()
    if (state.isDown) {
      return
    }
    state.isDown = true

    try {
      dispatchButtonState(button, pressed = true)
    } catch (e: Exception) {
      DebugLog.e("ControllerBridge", e) { "Error on button press: ${e.message}" }
    }
  }

  override fun onButtonReleased(button: OnScreenController.Button) {
    val state = buttonStates.getOrPut(button) { ButtonDispatchState() }
    state.pendingRelease?.let(mainHandler::removeCallbacks)
    state.pendingRelease = null
    if (!state.isDown) {
      return
    }

    val elapsed = SystemClock.uptimeMillis() - state.pressedAtMs
    val remaining = MIN_TAP_HOLD_MS - elapsed
    if (remaining > 0L) {
      val releaseTask = Runnable {
        state.pendingRelease = null
        if (!state.isDown) {
          return@Runnable
        }
        state.isDown = false
        try {
          dispatchButtonState(button, pressed = false)
        } catch (e: Exception) {
          DebugLog.e("ControllerBridge", e) { "Error on delayed button release: ${e.message}" }
        }
      }
      state.pendingRelease = releaseTask
      mainHandler.postDelayed(releaseTask, remaining)
      return
    }

    state.isDown = false
    try {
      dispatchButtonState(button, pressed = false)
    } catch (e: Exception) {
      DebugLog.e("ControllerBridge", e) { "Error on button release: ${e.message}" }
    }
  }

  override fun onStickMoved(stick: OnScreenController.Stick, x: Float, y: Float) {
    try {
      when (stick) {
        OnScreenController.Stick.LEFT -> {
          SDLControllerManager.onNativeJoy(VIRTUAL_DEVICE_ID, AXIS_LEFT_X, x)
          SDLControllerManager.onNativeJoy(VIRTUAL_DEVICE_ID, AXIS_LEFT_Y, y)
        }
        OnScreenController.Stick.RIGHT -> {
          SDLControllerManager.onNativeJoy(VIRTUAL_DEVICE_ID, AXIS_RIGHT_X, x)
          SDLControllerManager.onNativeJoy(VIRTUAL_DEVICE_ID, AXIS_RIGHT_Y, y)
        }
      }
    } catch (e: Exception) {
      DebugLog.e("ControllerBridge", e) { "Error on stick move: ${e.message}" }
    }
  }

  override fun onStickPressed(stick: OnScreenController.Stick) {
    try {
      val keyCode = when (stick) {
        OnScreenController.Stick.LEFT -> KeyEvent.KEYCODE_BUTTON_THUMBL
        OnScreenController.Stick.RIGHT -> KeyEvent.KEYCODE_BUTTON_THUMBR
      }
      SDLControllerManager.onNativePadDown(VIRTUAL_DEVICE_ID, keyCode)
    } catch (e: Exception) {
      DebugLog.e("ControllerBridge", e) { "Error on stick press: ${e.message}" }
    }
  }

  override fun onStickReleased(stick: OnScreenController.Stick) {
    try {
      val keyCode = when (stick) {
        OnScreenController.Stick.LEFT -> KeyEvent.KEYCODE_BUTTON_THUMBL
        OnScreenController.Stick.RIGHT -> KeyEvent.KEYCODE_BUTTON_THUMBR
      }
      SDLControllerManager.onNativePadUp(VIRTUAL_DEVICE_ID, keyCode)
    } catch (e: Exception) {
      DebugLog.e("ControllerBridge", e) { "Error on stick release: ${e.message}" }
    }
  }

  fun reset() {
    buttonStates.forEach { (button, state) ->
      state.pendingRelease?.let(mainHandler::removeCallbacks)
      state.pendingRelease = null
      if (!state.isDown) {
        return@forEach
      }
      state.isDown = false
      try {
        dispatchButtonState(button, pressed = false)
      } catch (e: Exception) {
        DebugLog.e("ControllerBridge", e) { "Error resetting $button: ${e.message}" }
      }
    }
  }

  private fun dispatchButtonState(button: OnScreenController.Button, pressed: Boolean) {
    when (button) {
      OnScreenController.Button.LEFT_TRIGGER,
      OnScreenController.Button.RIGHT_TRIGGER ->
        setTriggerState(button, pressed)
      else -> {
        val keyCode = getKeyCodeForButton(button)
        if (pressed) {
          SDLControllerManager.onNativePadDown(VIRTUAL_DEVICE_ID, keyCode)
        } else {
          SDLControllerManager.onNativePadUp(VIRTUAL_DEVICE_ID, keyCode)
        }
      }
    }
  }

  private fun setTriggerState(button: OnScreenController.Button, pressed: Boolean) {
    val keyCode = getKeyCodeForButton(button)
    val axis = when (button) {
      OnScreenController.Button.LEFT_TRIGGER -> AXIS_LEFT_TRIGGER
      OnScreenController.Button.RIGHT_TRIGGER -> AXIS_RIGHT_TRIGGER
      else -> return
    }
    // SDL maps the raw joystick axis (-1..1) to the game controller trigger
    // axis (0..1) as: output = (raw + 1) / 2. So 0.0f → 50% pressed, not 0%.
    // Use -1.0f for release so SDL computes output = 0 (fully released).
    val axisValue = if (pressed) 1.0f else -1.0f

    try {
      if (pressed) {
        SDLControllerManager.onNativePadDown(VIRTUAL_DEVICE_ID, keyCode)
      } else {
        SDLControllerManager.onNativePadUp(VIRTUAL_DEVICE_ID, keyCode)
      }
    } catch (e: Exception) {
      DebugLog.e("ControllerBridge", e) { "SDL pad event failed for $button: ${e.message}" }
    }
    try {
      SDLControllerManager.onNativeJoy(VIRTUAL_DEVICE_ID, axis, axisValue)
    } catch (e: Exception) {
      DebugLog.e("ControllerBridge", e) { "SDL joy event failed for $button: ${e.message}" }
    }
  }

  private fun getKeyCodeForButton(button: OnScreenController.Button): Int {
    return when (button) {
      OnScreenController.Button.A -> KeyEvent.KEYCODE_BUTTON_A
      OnScreenController.Button.B -> KeyEvent.KEYCODE_BUTTON_B
      OnScreenController.Button.X -> KeyEvent.KEYCODE_BUTTON_X
      OnScreenController.Button.Y -> KeyEvent.KEYCODE_BUTTON_Y
      OnScreenController.Button.DPAD_UP -> KeyEvent.KEYCODE_DPAD_UP
      OnScreenController.Button.DPAD_DOWN -> KeyEvent.KEYCODE_DPAD_DOWN
      OnScreenController.Button.DPAD_LEFT -> KeyEvent.KEYCODE_DPAD_LEFT
      OnScreenController.Button.DPAD_RIGHT -> KeyEvent.KEYCODE_DPAD_RIGHT
      OnScreenController.Button.LEFT_TRIGGER -> KeyEvent.KEYCODE_BUTTON_L2
      OnScreenController.Button.RIGHT_TRIGGER -> KeyEvent.KEYCODE_BUTTON_R2
      OnScreenController.Button.START -> KeyEvent.KEYCODE_BUTTON_START
      OnScreenController.Button.BACK -> KeyEvent.KEYCODE_BUTTON_SELECT
      OnScreenController.Button.LEFT_STICK_BUTTON -> KeyEvent.KEYCODE_BUTTON_THUMBL
      OnScreenController.Button.RIGHT_STICK_BUTTON -> KeyEvent.KEYCODE_BUTTON_THUMBR
      OnScreenController.Button.WHITE -> KeyEvent.KEYCODE_BUTTON_L1
      OnScreenController.Button.BLACK -> KeyEvent.KEYCODE_BUTTON_R1
    }
  }
}
