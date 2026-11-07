package com.izzy2lost.x1box.hakux

import android.content.Context
import android.media.AudioAttributes
import android.os.Build
import android.os.CombinedVibration
import android.os.SystemClock
import android.os.VibrationAttributes
import android.os.VibrationEffect
import android.os.Vibrator
import android.os.VibratorManager
import android.view.InputDevice

/**
 * The titles' rumble on the controllers playing (upstream hakuX's PadRouter rumble, fed by
 * HakuxPadRouter's device per player). Polled: each change of level is sent, and every
 * 900 ms while it lasts (a vibration is asked for a second at a time, as SDL's rumble was).
 * A controller with two motors gets the Duke's left (heavy) and right (light) on each; one
 * motor gets the stronger. A controller without motors (a handheld's own controls, which
 * Android may report as external, or a clip-on) rumbles the device itself instead: the
 * lowest player's such controller only, so that several don't share it.
 */
class HakuxRumble(
  private val context: Context,
  /** The device each player's rumble goes to, or -1. */
  private val deviceOf: (player: Int) -> Int,
  /** Bit n: player n + 1 plays. */
  private val playing: () -> Int,
  /** The rumble the title last sent to a player: left << 16 | right (0..65535 each). */
  private val rumbleOf: (player: Int) -> Int,
) {
  /** device id -> the levels last sent (left << 8 | right) and when */
  private val sent = HashMap<Int, Int>()
  private val sentAt = HashMap<Int, Long>()
  /** device id -> whether it has motors of its own (asked once) */
  private val motors = HashMap<Int, Boolean>()

  fun poll() {
    val now = SystemClock.uptimeMillis()
    val mask = playing()
    val devices = LinkedHashMap<Int, Int>()          /* device -> its (lowest) player */
    for (p in 0 until PLAYERS) {
      if (mask shr p and 1 == 0) continue
      val id = deviceOf(p)
      if (id >= 0 && id !in devices) devices[id] = p
    }
    // a device that stopped being some player's rumble target goes still
    for (id in sent.keys.toList()) {
      if (id !in devices && sent[id] != 0) {
        InputDevice.getDevice(id)?.let { vibrate(it, 0, 0, true) }
        sent[id] = 0
      }
    }
    val body = devices.keys.firstOrNull { id -> InputDevice.getDevice(id)?.let { !hasMotors(it) } == true }
    for ((id, p) in devices) {
      val raw = rumbleOf(p)
      val r = level(raw ushr 16) shl 8 or level(raw and 0xFFFF)
      val last = sent[id] ?: 0
      if (r == last && (r == 0 || now - (sentAt[id] ?: 0) < REFRESH_MS)) continue
      sent[id] = r
      sentAt[id] = now
      InputDevice.getDevice(id)?.let { vibrate(it, r ushr 8, r and 0xFF, id == body) }
    }
  }

  /** Every controller still (the game paused or gone). */
  fun stop() {
    for ((id, r) in sent) if (r != 0) InputDevice.getDevice(id)?.let { vibrate(it, 0, 0, true) }
    sent.clear()
    sentAt.clear()
  }

  /** Controllers came or went: ask their motors again. */
  fun forget(deviceId: Int) {
    motors.remove(deviceId)
    sent.remove(deviceId)
    sentAt.remove(deviceId)
  }

  private fun hasMotors(d: InputDevice): Boolean = motors.getOrPut(d.id) {
    try {
      if (Build.VERSION.SDK_INT >= 31) {
        d.vibratorManager.vibratorIds.isNotEmpty()
      } else {
        @Suppress("DEPRECATION")
        d.vibrator.hasVibrator()
      }
    } catch (_: Exception) {
      false
    }
  }

  /** A strength (0..65535) as a motor's amplitude, 0..255 (0 is off, as SDL's). */
  private fun level(v: Int) = (v * 255 + 32767) / 65535

  /*
   * A motor without amplitude control is on or off: on from a quarter of the strength
   * (else a title's faint engine rumble would be the motor's all).
   */
  private fun amplitude(v: Int, control: Boolean): Int = when {
    control -> v
    v >= ONOFF_MIN -> VibrationEffect.DEFAULT_AMPLITUDE
    else -> 0
  }

  /** One motor: the stronger of the two. */
  private fun vibrateOne(vib: Vibrator, left: Int, right: Int) {
    val a = amplitude(maxOf(left, right), vib.hasAmplitudeControl())
    if (a == 0) {
      vib.cancel()
      return
    }
    val e = VibrationEffect.createOneShot(1000, a)
    if (Build.VERSION.SDK_INT >= 33) {
      vib.vibrate(e, VibrationAttributes.createForUsage(VibrationAttributes.USAGE_MEDIA))
    } else {
      @Suppress("DEPRECATION")
      vib.vibrate(e, AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_GAME).build())
    }
  }

  /** The device's own motor. */
  private fun bodyVibrator(): Vibrator? {
    val v = if (Build.VERSION.SDK_INT >= 31) {
      context.getSystemService(VibratorManager::class.java)?.defaultVibrator
    } else {
      @Suppress("DEPRECATION")
      context.getSystemService(Vibrator::class.java)
    }
    return v?.takeIf { it.hasVibrator() }
  }

  /** A controller's rumble (levels): on its motors, or (body) on the device's when it has none. */
  private fun vibrate(d: InputDevice, left: Int, right: Int, body: Boolean) {
    try {
      if (!hasMotors(d)) {
        if (body) bodyVibrator()?.let { vibrateOne(it, left, right) }
        return
      }
      if (Build.VERSION.SDK_INT < 31) {
        @Suppress("DEPRECATION")
        vibrateOne(d.vibrator, left, right)
        return
      }
      val vm = d.vibratorManager
      val ids = vm.vibratorIds
      val c = CombinedVibration.startParallel()
      var any = false
      /* two motors: the left (heavy) and the right (light), as the Duke's */
      for ((i, vid) in ids.withIndex()) {
        val v = if (ids.size >= 2) (if (i == 0) left else right) else maxOf(left, right)
        val a = amplitude(v, vm.getVibrator(vid).hasAmplitudeControl())
        if (a == 0) continue
        c.addVibrator(vid, VibrationEffect.createOneShot(1000, a))
        any = true
      }
      when {
        !any -> vm.cancel()
        Build.VERSION.SDK_INT >= 33 ->
          vm.vibrate(c.combine(), VibrationAttributes.createForUsage(VibrationAttributes.USAGE_MEDIA))
        else -> vm.vibrate(c.combine())
      }
    } catch (_: Exception) {
      /* a controller gone meanwhile, or one that refuses: no rumble */
    }
  }

  companion object {
    private const val PLAYERS = 4
    /** A vibration lasts a second; it is asked again this often while it lasts. */
    private const val REFRESH_MS = 900L
    /** The level (of 255) from which an on/off motor is on. */
    private const val ONOFF_MIN = 64
  }
}
