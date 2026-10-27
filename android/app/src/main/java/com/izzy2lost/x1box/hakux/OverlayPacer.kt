package com.izzy2lost.x1box.hakux

import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.view.View

/**
 * The window's overlays (the on-screen controls, the FPS line) and the game's
 * GPU work, from hakuX. With Turnip the core raises its kgsl draw contexts to
 * priority 7, above the window's GPU queue, which then only runs when the
 * game's work runs out; the compositor shows the game's frames behind the
 * window's (a process's frames in order), so a window frame drawn while the
 * game kept the GPU busy held the picture until the GPU had a break, over a
 * second at a time (PGR2 at 3x on an Adreno 650).
 *
 * So the overlays wait to redraw while the GPU has been busy BUSY_MS without
 * a break (mayDraw), CAP_MS at most; and a window frame drawn while the GPU
 * was busy, still without a break STUCK_MS later, gets one: the render
 * thread lets the GPU run out of work for a moment (gap). An overlay may come
 * late; the game's frames don't wait for it.
 *
 * Elsewhere (the system driver, Mali, ...) the window's frames take turns with
 * the game's and busyMs reports 0, so nothing here waits or asks for a gap.
 */
class OverlayPacer(private val busyMs: () -> Int, private val gap: () -> Unit) {
  private val handler = Handler(Looper.getMainLooper())
  /** The last window frame drawn while the GPU was busy (uptime ms), 0: none to check. */
  private var drawnAt = 0L

  /** An overlay redraw wanted since `since` (uptime ms) may happen now. */
  fun mayDraw(since: Long): Boolean =
    busyMs() < BUSY_MS || SystemClock.uptimeMillis() - since >= CAP_MS

  /** Watches the frames of the window `view` is in. */
  fun attach(view: View) {
    view.viewTreeObserver.addOnDrawListener {
      if (busyMs() > 0) {
        drawnAt = SystemClock.uptimeMillis()
        handler.removeCallbacks(check)
        handler.postDelayed(check, STUCK_MS)
      }
    }
  }

  /*
   * The frame drawn at drawnAt: a break for it unless the GPU ran out of the
   * game's work after its GPU work was queued (SUBMIT_MS after the draw, as
   * the render thread draws it). A busy span begins where the GPU last ran out.
   */
  private val check = Runnable {
    val busy = busyMs()
    if (busy > 0 && SystemClock.uptimeMillis() - busy <= drawnAt + SUBMIT_MS) {
      gap()
    }
    drawnAt = 0L
  }

  companion object {
    /** The GPU busy this long without a break: the overlays wait to redraw. */
    const val BUSY_MS = 40
    /** An overlay redraw waits this long at most. */
    const val CAP_MS = 200L
    /** A window frame still waiting this long after its draw gets a break. */
    const val STUCK_MS = 32L
    /** How long after a draw its GPU work is queued, at most. */
    private const val SUBMIT_MS = 8L
  }
}
