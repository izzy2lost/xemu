package com.izzy2lost.x1box

import android.content.Context
import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent
import com.izzy2lost.x1box.ControllerRemap.Source

/**
 * Emulator hotkeys on the physical controller: each action is a combination
 * of up to three pad buttons, matched before the press reaches the game. They
 * match the pad's own buttons, not the remapped Xbox inputs, and are shared by
 * both cores; an activity offers only the actions it can carry out.
 */
object ControllerHotkeys {

  enum class Action(val key: String, val label: Int) {
    MENU("menu", R.string.hotkey_menu),
    SAVE_STATE("save_state", R.string.hotkey_save_state),
    LOAD_STATE("load_state", R.string.hotkey_load_state),
    NEXT_SLOT("next_slot", R.string.hotkey_next_slot),
    PREVIOUS_SLOT("previous_slot", R.string.hotkey_previous_slot),
    FAST_FORWARD("fast_forward", R.string.hotkey_fast_forward),
    EXIT_GAME("exit_game", R.string.hotkey_exit_game),
  }

  const val MAX_BUTTONS = 3
  const val SLOTS = 10

  private const val PREFS = "controller_remap"
  private const val KEY_HOTKEYS = "hotkeys"
  private const val KEY_SLOT = "hotkey_slot"

  /** Stored as "action=code+code;…", the codes being Source.sdlCode. */
  fun load(context: Context): MutableMap<Action, List<Source>> {
    val map = mutableMapOf<Action, List<Source>>()
    context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
      .getString(KEY_HOTKEYS, null)
      ?.split(';')
      ?.forEach { entry ->
        val action = Action.entries.firstOrNull { it.key == entry.substringBefore('=') } ?: return@forEach
        val combo = entry.substringAfter('=', "").split('+')
          .mapNotNull { it.toIntOrNull()?.let(Source::fromSdlCode) }
          .filter { it != Source.NONE }
        if (combo.isNotEmpty()) map[action] = combo
      }
    return map
  }

  fun save(context: Context, map: Map<Action, List<Source>>) {
    val value = map.filterValues { it.isNotEmpty() }.entries
      .joinToString(";") { (action, combo) -> action.key + "=" + combo.joinToString("+") { it.sdlCode.toString() } }
    context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit()
      .apply { if (value.isEmpty()) remove(KEY_HOTKEYS) else putString(KEY_HOTKEYS, value) }
      .apply()
  }

  fun label(combo: List<Source>?): String =
    if (combo.isNullOrEmpty()) Source.NONE.label else combo.joinToString(" + ") { it.label }

  /** The savestate slot Save and Load state use, 1..[SLOTS]. */
  fun slot(context: Context): Int =
    context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).getInt(KEY_SLOT, 1).coerceIn(1, SLOTS)

  /** Steps the slot by [delta], wrapping, and returns the new one. */
  fun stepSlot(context: Context, delta: Int): Int {
    val next = Math.floorMod(slot(context) - 1 + delta, SLOTS) + 1
    context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit().putInt(KEY_SLOT, next).apply()
    return next
  }

  /**
   * Watches the pad and runs [onAction] when a combination completes. The
   * press that completes it (and its release) never reaches the game; the
   * buttons held before it already did, as with Start + Select. Analog
   * triggers that send no L2/R2 keys count as held, but can't be withheld.
   */
  class Matcher(
    private var bindings: Map<Action, List<Source>>,
    private val onAction: (Action) -> Unit,
  ) {
    private val held = HashSet<Source>()
    private val swallowed = HashSet<Source>()

    fun setBindings(map: Map<Action, List<Source>>) {
      bindings = map.toMap()
      reset()
    }

    /** Forget what is held: the window lost focus, so the releases go elsewhere. */
    fun reset() {
      held.clear()
      swallowed.clear()
    }

    /** True when the event belongs to a hotkey and must not reach the game. */
    fun onKey(event: KeyEvent): Boolean {
      if (bindings.isEmpty()) return false
      val source = Source.fromKeyCode(event.keyCode) ?: return false
      return when (event.action) {
        KeyEvent.ACTION_DOWN -> {
          if (source in swallowed) return true
          if (event.repeatCount != 0) return false
          if (!press(source)) return false
          swallowed += source
          true
        }
        KeyEvent.ACTION_UP -> {
          held -= source
          swallowed.remove(source)
        }
        else -> false
      }
    }

    /** Analog triggers; always passed on to the game. */
    fun onMotion(event: MotionEvent) {
      if (bindings.isEmpty()) return
      if ((event.source and InputDevice.SOURCE_JOYSTICK) != InputDevice.SOURCE_JOYSTICK) return
      trigger(Source.LEFT_TRIGGER, maxOf(event.getAxisValue(MotionEvent.AXIS_LTRIGGER), event.getAxisValue(MotionEvent.AXIS_BRAKE)))
      trigger(Source.RIGHT_TRIGGER, maxOf(event.getAxisValue(MotionEvent.AXIS_RTRIGGER), event.getAxisValue(MotionEvent.AXIS_GAS)))
    }

    private fun trigger(source: Source, value: Float) {
      if (value > TRIGGER_PRESS) {
        if (source !in held) press(source)
      } else if (value < TRIGGER_RELEASE && source !in swallowed) {
        held -= source
      }
    }

    /** Adds [source] to the held set; true when that completed a combination. */
    private fun press(source: Source): Boolean {
      held += source
      // The longest match wins, so Select + R1 beats a lone R1.
      val hit = bindings.entries
        .filter { (_, combo) -> source in combo && held.containsAll(combo) }
        .maxByOrNull { it.value.size }
        ?: return false
      onAction(hit.key)
      return true
    }
  }

  const val TRIGGER_PRESS = 0.6f
  const val TRIGGER_RELEASE = 0.3f
}
