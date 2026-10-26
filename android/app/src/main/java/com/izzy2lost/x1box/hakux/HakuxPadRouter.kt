package com.izzy2lost.x1box.hakux

import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent
import com.izzy2lost.x1box.ControllerRemap
import com.izzy2lost.x1box.ControllerRemap.Source
import com.izzy2lost.x1box.ControllerRemap.XboxInput
import kotlin.math.abs
import kotlin.math.max

/**
 * Physical controllers for the hakuX core, each its own player, joined as it is first
 * used (a button, or a stick or trigger pushed well over): the first free player of
 * four, plugged into the game then. Controls Android calls built in (not external) are
 * always player 1, a handheld's possibly split over several devices; some handhelds
 * call theirs external, which is why a pad joins by use, not by connecting: whoever
 * plays first is player 1. A fifth shares player 1. A device's keys and axes go
 * through the app's controller remap (ControllerRemap: the same table xemu's input
 * takes) and are sent as its player's Duke (nativeSetPad). The core adds the on-screen
 * controller's to player 1.
 */
class HakuxPadRouter(
  private var remap: Map<XboxInput, Source>,
  /** A player's Duke: buttons (NC_PAD_* bits), analog bytes (A B X Y Black White LT RT), sticks (up positive). */
  private val send: (player: Int, buttons: Int, analog: Long, lx: Int, ly: Int, rx: Int, ry: Int) -> Unit,
  /** The players with a controller plugged in changed (bit n: player n + 1; player 1 always). */
  private val plugged: (mask: Int) -> Unit,
) {
  private class Device {
    val held = HashSet<Source>()
    var lt = 0f
    var rt = 0f
    var hatX = 0f
    var hatY = 0f
    var lx = 0f
    var ly = 0f
    var rx = 0f
    var ry = 0f
  }

  /** Held input by device. */
  private val devices = HashMap<Int, Device>()
  /** Each gamepad's player (0-3), by device. */
  private val players = HashMap<Int, Int>()
  private val last = arrayOfNulls<List<Number>>(PLAYERS)
  /** The device of each player's that moved last (its rumble goes to it). */
  private val lastDevice = IntArray(PLAYERS) { -1 }
  private var mask = 1

  /** Bit n: player n + 1 has a controller (player 1 always: the built-in and on-screen controls). */
  fun pluggedMask(): Int = players.values.fold(1) { m, p -> m or (1 shl p) }

  /** The device of a player's to rumble, or -1. */
  fun rumbleDevice(player: Int): Int = lastDevice[player].takeIf { it in players }
    ?: players.entries.firstOrNull { it.value == player }?.key ?: -1

  /** Each player's devices, for the menu (empty: nobody yet). */
  fun assignments(): List<List<InputDevice>> = (0 until PLAYERS).map { p ->
    players.filterValues { it == p }.keys.sorted().mapNotNull { InputDevice.getDevice(it) }
  }

  /** Every pad leaves its player: each joins again with its next press (the game sees them go). */
  fun resetPlayers() {
    players.clear()
    devices.clear()
    publishAll()
    updatePlugged()
  }

  /**
   * The device's player: the one it has, or, when join (it was used for real), the first
   * free one. Null: not a gamepad, or not joined yet.
   */
  private fun player(deviceId: Int, device: InputDevice?, join: Boolean): Int? {
    players[deviceId]?.let { return it }
    if (!isGamepad(device) || !join) return null
    val p = if (device?.isExternal != true) {
      0
    } else {
      val builtIn = players.any { (id, p) -> p == 0 && InputDevice.getDevice(id)?.isExternal != true }
      val taken = players.values.toSet() + (if (builtIn) setOf(0) else emptySet())
      (0 until PLAYERS).firstOrNull { it !in taken } ?: 0    /* a fifth shares player 1 */
    }
    players[deviceId] = p
    return p
  }

  private fun updatePlugged() {
    val m = pluggedMask()
    if (m != mask) {
      mask = m
      plugged(m)
    }
  }

  fun setRemap(map: Map<XboxInput, Source>) {
    remap = map
    publishAll()
  }

  /** Everything released (focus lost, a menu over the game). */
  fun reset() {
    devices.clear()
    publishAll()
  }

  /** A device went: its input let go, its player free again (the game sees it unplugged). */
  fun forget(deviceId: Int) {
    val p = players.remove(deviceId)
    if (devices.remove(deviceId) != null && p != null) publish(p)
    updatePlugged()
  }

  /** A gamepad's key: true when taken (it doesn't go on to SDL or the system). */
  fun onKey(event: KeyEvent): Boolean {
    if (!isGamepad(event.device)) return false
    val source = Source.fromKeyCode(event.keyCode) ?: return false
    if (source == Source.GUIDE) return false        /* the activity's: its menu */
    val p = player(event.deviceId, event.device, join = event.action == KeyEvent.ACTION_DOWN)
      ?: return isGamepad(event.device)              /* a release from before it joined */
    updatePlugged()
    val d = devices.getOrPut(event.deviceId, ::Device)
    when (event.action) {
      KeyEvent.ACTION_DOWN -> d.held.add(source)
      KeyEvent.ACTION_UP -> d.held.remove(source)
      else -> return true
    }
    lastDevice[p] = event.deviceId
    publish(p)
    return true
  }

  fun onMotion(event: MotionEvent): Boolean {
    val dev = event.device ?: return false
    if (!isGamepad(dev) || event.action != MotionEvent.ACTION_MOVE) return false
    // a resting stick's noise doesn't join a pad: only a real push does
    val pushed = listOf(
      MotionEvent.AXIS_X, MotionEvent.AXIS_Y, MotionEvent.AXIS_Z, MotionEvent.AXIS_RZ,
      MotionEvent.AXIS_HAT_X, MotionEvent.AXIS_HAT_Y, MotionEvent.AXIS_LTRIGGER, MotionEvent.AXIS_RTRIGGER,
      MotionEvent.AXIS_GAS, MotionEvent.AXIS_BRAKE,
    ).any { abs(event.getAxisValue(it)) > 0.5f }
    val p = player(event.deviceId, dev, join = pushed) ?: return true
    updatePlugged()
    val d = devices.getOrPut(event.deviceId, ::Device)
    fun axis(a: Int): Float {
      val range = dev.getMotionRange(a, event.source) ?: return 0f
      val v = event.getAxisValue(a)
      return if (abs(v) <= max(range.flat, 0.08f)) 0f else v
    }
    d.lx = axis(MotionEvent.AXIS_X)
    d.ly = axis(MotionEvent.AXIS_Y)
    d.rx = axis(MotionEvent.AXIS_Z)
    d.ry = axis(MotionEvent.AXIS_RZ)
    d.hatX = event.getAxisValue(MotionEvent.AXIS_HAT_X)
    d.hatY = event.getAxisValue(MotionEvent.AXIS_HAT_Y)
    d.lt = max(event.getAxisValue(MotionEvent.AXIS_LTRIGGER), event.getAxisValue(MotionEvent.AXIS_BRAKE))
    d.rt = max(event.getAxisValue(MotionEvent.AXIS_RTRIGGER), event.getAxisValue(MotionEvent.AXIS_GAS))
    lastDevice[p] = event.deviceId
    publish(p)
    return true
  }

  /** A player's devices' held input. */
  private fun inputsOf(player: Int): List<Device> =
    devices.filterKeys { players[it] == player }.values.toList()

  /** A source's value over a player's devices, 0..1. */
  private fun value(ds: List<Device>, source: Source): Float {
    var v = 0f
    for (d in ds) {
      val x = when (source) {
        Source.NONE -> 0f
        Source.LEFT_TRIGGER -> max(d.lt, if (Source.LEFT_TRIGGER in d.held) 1f else 0f)
        Source.RIGHT_TRIGGER -> max(d.rt, if (Source.RIGHT_TRIGGER in d.held) 1f else 0f)
        Source.DPAD_LEFT -> if (Source.DPAD_LEFT in d.held || d.hatX < -0.5f) 1f else 0f
        Source.DPAD_RIGHT -> if (Source.DPAD_RIGHT in d.held || d.hatX > 0.5f) 1f else 0f
        Source.DPAD_UP -> if (Source.DPAD_UP in d.held || d.hatY < -0.5f) 1f else 0f
        Source.DPAD_DOWN -> if (Source.DPAD_DOWN in d.held || d.hatY > 0.5f) 1f else 0f
        else -> if (source in d.held) 1f else 0f
      }
      v = max(v, x)
    }
    return v
  }

  private fun publishAll() {
    for (p in 0 until PLAYERS) publish(p)
  }

  private fun publish(player: Int) {
    val ds = inputsOf(player)
    fun input(i: XboxInput): Float = value(ds, remap[i] ?: i.defaultSource)
    var buttons = 0
    if (input(XboxInput.DPAD_UP) > 0.5f) buttons = buttons or PAD_UP
    if (input(XboxInput.DPAD_DOWN) > 0.5f) buttons = buttons or PAD_DOWN
    if (input(XboxInput.DPAD_LEFT) > 0.5f) buttons = buttons or PAD_LEFT
    if (input(XboxInput.DPAD_RIGHT) > 0.5f) buttons = buttons or PAD_RIGHT
    if (input(XboxInput.START) > 0.5f) buttons = buttons or PAD_START
    if (input(XboxInput.BACK) > 0.5f) buttons = buttons or PAD_BACK
    if (input(XboxInput.LEFT_STICK) > 0.5f) buttons = buttons or PAD_LTHUMB
    if (input(XboxInput.RIGHT_STICK) > 0.5f) buttons = buttons or PAD_RTHUMB
    val analogInputs = listOf(
      XboxInput.A, XboxInput.B, XboxInput.X, XboxInput.Y, XboxInput.BLACK, XboxInput.WHITE,
      XboxInput.LEFT_TRIGGER, XboxInput.RIGHT_TRIGGER,
    )
    var analog = 0L
    analogInputs.forEachIndexed { i, x ->
      analog = analog or ((input(x) * 255f).toLong().coerceIn(0L, 255L) shl (8 * i))
    }
    /* the stick pushed furthest across devices; the Duke's y is up-positive */
    var lx = 0f; var ly = 0f; var rx = 0f; var ry = 0f
    for (d in ds) {
      if (abs(d.lx) + abs(d.ly) > abs(lx) + abs(ly)) { lx = d.lx; ly = d.ly }
      if (abs(d.rx) + abs(d.ry) > abs(rx) + abs(ry)) { rx = d.rx; ry = d.ry }
    }
    val state = listOf(buttons, analog, s16(lx), s16(-ly), s16(rx), s16(-ry))
    if (state == last[player]) return
    last[player] = state
    send(player, buttons, analog, s16(lx), s16(-ly), s16(rx), s16(-ry))
  }

  private fun s16(v: Float) = (v.coerceIn(-1f, 1f) * 32767f).toInt()

  companion object {
    private const val PLAYERS = 4

    /* hakux/core/native/dev/usb.h NC_PAD_* */
    private const val PAD_UP = 0x01
    private const val PAD_DOWN = 0x02
    private const val PAD_LEFT = 0x04
    private const val PAD_RIGHT = 0x08
    private const val PAD_START = 0x10
    private const val PAD_BACK = 0x20
    private const val PAD_LTHUMB = 0x40
    private const val PAD_RTHUMB = 0x80

    fun isGamepad(device: InputDevice?): Boolean {
      val s = device?.sources ?: return false
      return !device.isVirtual && ((s and InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD ||
        (s and InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK)
    }
  }
}
