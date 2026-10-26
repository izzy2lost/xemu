package com.izzy2lost.x1box

import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.DashPathEffect
import android.graphics.Paint
import android.graphics.PointF
import android.util.AttributeSet
import android.view.MotionEvent
import android.view.View
import android.view.ViewConfiguration
import kotlin.math.hypot
import kotlin.math.max
import kotlin.math.pow
import kotlin.math.sqrt

class OnScreenController @JvmOverloads constructor(
  context: Context,
  attrs: AttributeSet? = null,
  defStyleAttr: Int = 0
) : View(context, attrs, defStyleAttr) {

  companion object {
    private const val MIN_SCALE = 0.5f
    private const val MAX_SCALE = 2.5f
  }

  private val paint = Paint(Paint.ANTI_ALIAS_FLAG)
  private val buttons = mutableMapOf<Button, ButtonState>()
  private val sticks = mutableMapOf<Stick, StickState>()

  private var menuButtonCenter = PointF(0f, 0f)
  private var menuButtonRadius = 0f
  private var menuButtonPressed = false
  private var menuButtonPointerId = -1
  private val swipeTouchSlop = ViewConfiguration.get(context).scaledTouchSlop.toFloat()
  private val swipeUpGestureRecognizer = SwipeUpGestureRecognizer(
    minDistancePx = {
      max(height * 0.14f, swipeTouchSlop * 8f)
    },
    touchSlopPx = { swipeTouchSlop },
    canStartAt = { x, y ->
      !editMode && y >= height * 0.35f && !isPointInInteractiveElement(x, y)
    },
    onTriggered = {
      handleCancel()
      onMenuButtonTapped?.invoke()
      invalidate()
    },
  )

  var onMenuButtonTapped: (() -> Unit)? = null

  /**
   * Whether the fast-forward button exists (the core supports it). xemu shares
   * this view and the saved layout, and has no such button.
   */
  var fastForwardAvailable: Boolean = false
    set(value) {
      if (field == value) return
      field = value
      if (width > 0 && height > 0) initializeControls(width, height)
      invalidate()
    }

  /** Fast-forward on: the button stays lit. Set by its tap or by the host (its menu). */
  var fastForward: Boolean = false
    set(value) {
      if (field == value) return
      field = value
      invalidate()
    }

  /** The fast-forward button was tapped: [fastForward] is the new state. */
  var onFastForwardChanged: ((Boolean) -> Unit)? = null

  private var controllerListener: ControllerListener? = null

  enum class Button {
    A, B, X, Y,
    DPAD_UP, DPAD_DOWN, DPAD_LEFT, DPAD_RIGHT,
    LEFT_TRIGGER, RIGHT_TRIGGER,
    START, BACK,
    LEFT_STICK_BUTTON, RIGHT_STICK_BUTTON,
    BLACK, WHITE,
    /** Not a pad button: toggles [fastForward]; only where [fastForwardAvailable]. */
    FAST_FORWARD
  }

  enum class Stick {
    LEFT, RIGHT
  }

  /** A group of controls the layout editor moves, resizes and hides as one. */
  enum class Element(val label: String) {
    FACE_BUTTONS("Face Buttons"),
    DPAD("D-Pad"),
    LEFT_TRIGGER("Left Trigger"),
    RIGHT_TRIGGER("Right Trigger"),
    LEFT_STICK("Left Stick"),
    RIGHT_STICK("Right Stick"),
    BACK("Back"),
    START("Start"),
    WHITE("White"),
    BLACK("Black"),
    LEFT_STICK_BUTTON("LS"),
    RIGHT_STICK_BUTTON("RS"),
    FAST_FORWARD("Fast Forward"),
  }

  /**
   * User placement of an [Element]: its centre as a fraction of the view, a
   * size relative to the default, and whether it is shown at all.
   */
  data class ElementLayout(
    val centerX: Float,
    val centerY: Float,
    val scale: Float = 1f,
    val visible: Boolean = true,
  )

  data class ButtonState(
    val center: PointF,
    val radius: Float,
    var isPressed: Boolean = false,
    var activePointerId: Int = -1
  )

  data class StickState(
    val center: PointF,
    val radius: Float,
    val deadZone: Float = 0.2f,
    var currentPos: PointF = PointF(0f, 0f),
    var isPressed: Boolean = false,
    var activePointerId: Int = -1
  )

  interface ControllerListener {
    fun onButtonPressed(button: Button)
    fun onButtonReleased(button: Button)
    fun onStickMoved(stick: Stick, x: Float, y: Float)
    fun onStickPressed(stick: Stick)
    fun onStickReleased(stick: Stick)

    /**
     * A touch that did not land on any control, in normalised view
     * coordinates. This view consumes every touch so it can drive the pads,
     * so light gun games would otherwise never see a pointer.
     */
    fun onPointerMoved(x: Float, y: Float, down: Boolean) {}
  }

  init {
    setBackgroundColor(Color.TRANSPARENT)
  }

  // Geometry from initializeControls(), before the user's layout is applied.
  private val defaultButtons = mutableMapOf<Button, Pair<PointF, Float>>()
  private val defaultSticks = mutableMapOf<Stick, Pair<PointF, Float>>()
  private val layoutOverrides = mutableMapOf<Element, ElementLayout>()

  /** While true, touches move controls instead of driving the pad. */
  var editMode: Boolean = false
    set(value) {
      if (field == value) return
      resetAllInputs()
      field = value
      selectedElement = null
      editDragPointerId = -1
      pinchStartDistance = 0f
      onSelectionChanged?.invoke(null)
      invalidate()
    }

  var selectedElement: Element? = null
    private set

  var onSelectionChanged: ((Element?) -> Unit)? = null

  /** Called after each finished edit with the full set of overrides. */
  var onLayoutEdited: ((Map<Element, ElementLayout>) -> Unit)? = null

  private var editDragPointerId = -1
  private val editDragOffset = PointF()
  private var pinchStartDistance = 0f
  private var pinchStartScale = 1f
  private val editSlop = ViewConfiguration.get(context).scaledTouchSlop.toFloat()
  private val dashEffect = DashPathEffect(floatArrayOf(14f, 10f), 0f)

  fun setControllerListener(listener: ControllerListener) {
    this.controllerListener = listener
  }

  override fun onSizeChanged(w: Int, h: Int, oldw: Int, oldh: Int) {
    super.onSizeChanged(w, h, oldw, oldh)
    initializeControls(w, h)
  }

  private fun initializeControls(width: Int, height: Int) {
    val w = width.toFloat()
    val h = height.toFloat()
    
    // Button sizes - made D-pad smaller
    val faceButtonRadius = w * 0.032f
    val dpadButtonRadius = w * 0.025f
    val shoulderButtonRadius = w * 0.034f
    val smallButtonRadius = w * 0.022f
    val stickRadius = w * 0.07f

    // Face buttons (right side) - A, B, X, Y in diamond formation
    val faceButtonCenterX = w * 0.88f
    val faceButtonCenterY = h * 0.55f
    val faceButtonSpacing = w * 0.064f

    buttons[Button.A] = ButtonState(
      PointF(faceButtonCenterX, faceButtonCenterY + faceButtonSpacing),
      faceButtonRadius
    )
    buttons[Button.B] = ButtonState(
      PointF(faceButtonCenterX + faceButtonSpacing, faceButtonCenterY),
      faceButtonRadius
    )
    buttons[Button.X] = ButtonState(
      PointF(faceButtonCenterX - faceButtonSpacing, faceButtonCenterY),
      faceButtonRadius
    )
    buttons[Button.Y] = ButtonState(
      PointF(faceButtonCenterX, faceButtonCenterY - faceButtonSpacing),
      faceButtonRadius
    )

    // Spread the left-side controls apart: D-pad farther left, left stick farther right.
    val leftUpperControlCenterX = w * 0.13f
    val leftUpperControlCenterY = h * 0.45f
    val leftLowerControlCenterX = w * 0.18f
    val leftLowerControlCenterY = h * 0.82f
    val dpadSpacing = w * 0.045f

    buttons[Button.DPAD_UP] = ButtonState(
      PointF(leftUpperControlCenterX, leftUpperControlCenterY - dpadSpacing),
      dpadButtonRadius
    )
    buttons[Button.DPAD_DOWN] = ButtonState(
      PointF(leftUpperControlCenterX, leftUpperControlCenterY + dpadSpacing),
      dpadButtonRadius
    )
    buttons[Button.DPAD_LEFT] = ButtonState(
      PointF(leftUpperControlCenterX - dpadSpacing, leftUpperControlCenterY),
      dpadButtonRadius
    )
    buttons[Button.DPAD_RIGHT] = ButtonState(
      PointF(leftUpperControlCenterX + dpadSpacing, leftUpperControlCenterY),
      dpadButtonRadius
    )

    // Trigger buttons - smaller and aligned to top corners
    val shoulderEdgeMargin = shoulderButtonRadius + (w * 0.02f)
    buttons[Button.LEFT_TRIGGER] = ButtonState(
      PointF(shoulderEdgeMargin, shoulderEdgeMargin),
      shoulderButtonRadius
    )
    buttons[Button.RIGHT_TRIGGER] = ButtonState(
      PointF(w - shoulderEdgeMargin, shoulderEdgeMargin),
      shoulderButtonRadius
    )

    // Analog sticks
    sticks[Stick.LEFT] = StickState(
      PointF(leftLowerControlCenterX, leftLowerControlCenterY),
      stickRadius
    )
    sticks[Stick.RIGHT] = StickState(
      PointF(w * 0.62f, h * 0.82f),
      stickRadius
    )

    // Center buttons - keep these near the lower controls and right stick.
    val centerButtonsBaseX = (leftLowerControlCenterX + sticks[Stick.RIGHT]!!.center.x) * 0.5f
    val centerButtonsY = h * 0.9f
    val centerButtonSpacing = smallButtonRadius * 3.4f
    buttons[Button.BACK] = ButtonState(
      PointF(centerButtonsBaseX - (centerButtonSpacing * 0.5f), centerButtonsY),
      smallButtonRadius
    )
    buttons[Button.START] = ButtonState(
      PointF(centerButtonsBaseX + (centerButtonSpacing * 0.5f), centerButtonsY),
      smallButtonRadius
    )

    // Black and White buttons - black to the right of white
    val whiteBlackY = h * 0.8f
    val whiteX = w * 0.75f
    val whiteBlackSpacing = smallButtonRadius * 2.6f
    buttons[Button.WHITE] = ButtonState(
      PointF(whiteX, whiteBlackY),
      smallButtonRadius
    )
    buttons[Button.BLACK] = ButtonState(
      PointF(whiteX + whiteBlackSpacing, whiteBlackY),
      smallButtonRadius
    )

    // LS/RS (thumbstick click) as dedicated buttons separate from the stick
    // circles so they can't be triggered accidentally by joystick movement.
    // LS follows the left stick into the lower-left cluster.
    buttons[Button.LEFT_STICK_BUTTON] = ButtonState(
      PointF(leftLowerControlCenterX - (w * 0.10f), leftLowerControlCenterY + (h * 0.08f)),
      smallButtonRadius
    )
    buttons[Button.RIGHT_STICK_BUTTON] = ButtonState(
      PointF(w * 0.51f, h * 0.9f),
      smallButtonRadius
    )

    // Fast forward - beside the right trigger, out of the way of the face buttons
    if (fastForwardAvailable) {
      buttons[Button.FAST_FORWARD] = ButtonState(
        PointF(w - shoulderEdgeMargin - shoulderButtonRadius * 3.2f, shoulderEdgeMargin),
        smallButtonRadius * 1.25f
      )
    } else {
      buttons.remove(Button.FAST_FORWARD)
    }

    // Menu button — bottom-right corner, opens in-game menu
    menuButtonRadius = smallButtonRadius * 1.25f
    menuButtonCenter = PointF(
      w - menuButtonRadius - w * 0.018f,
      h - menuButtonRadius - h * 0.022f
    )

    defaultButtons.clear()
    buttons.forEach { (b, st) -> defaultButtons[b] = PointF(st.center.x, st.center.y) to st.radius }
    defaultSticks.clear()
    sticks.forEach { (s, st) -> defaultSticks[s] = PointF(st.center.x, st.center.y) to st.radius }
    applyLayout()
  }

  // ---- Layout ----

  private fun elementOf(button: Button): Element = when (button) {
    Button.A, Button.B, Button.X, Button.Y -> Element.FACE_BUTTONS
    Button.DPAD_UP, Button.DPAD_DOWN, Button.DPAD_LEFT, Button.DPAD_RIGHT -> Element.DPAD
    Button.LEFT_TRIGGER -> Element.LEFT_TRIGGER
    Button.RIGHT_TRIGGER -> Element.RIGHT_TRIGGER
    Button.BACK -> Element.BACK
    Button.START -> Element.START
    Button.WHITE -> Element.WHITE
    Button.BLACK -> Element.BLACK
    Button.LEFT_STICK_BUTTON -> Element.LEFT_STICK_BUTTON
    Button.RIGHT_STICK_BUTTON -> Element.RIGHT_STICK_BUTTON
    Button.FAST_FORWARD -> Element.FAST_FORWARD
  }

  private fun elementOf(stick: Stick): Element = when (stick) {
    Stick.LEFT -> Element.LEFT_STICK
    Stick.RIGHT -> Element.RIGHT_STICK
  }

  private fun isVisible(element: Element) = layoutOverrides[element]?.visible ?: true

  /** The elements this view has controls for (the editor's): not fast-forward where it isn't available. */
  private fun editableElements(): List<Element> = Element.entries.filter { e ->
    defaultButtons.keys.any { elementOf(it) == e } || defaultSticks.keys.any { elementOf(it) == e }
  }

  private fun defaultCentroid(element: Element): PointF {
    val points = defaultButtons.filterKeys { elementOf(it) == element }.values.map { it.first } +
      defaultSticks.filterKeys { elementOf(it) == element }.values.map { it.first }
    if (points.isEmpty()) return PointF(0f, 0f)
    return PointF(points.map { it.x }.average().toFloat(), points.map { it.y }.average().toFloat())
  }

  private fun currentLayout(element: Element): ElementLayout {
    layoutOverrides[element]?.let { return it }
    val c = defaultCentroid(element)
    return ElementLayout(c.x / max(width, 1), c.y / max(height, 1))
  }

  /** Rebuild control geometry from the defaults plus the user's overrides. */
  private fun applyLayout() {
    if (width <= 0 || height <= 0) return
    val centroids = Element.entries.associateWith { defaultCentroid(it) }
    fun place(element: Element, p: PointF, r: Float): Pair<PointF, Float> {
      val o = layoutOverrides[element] ?: return PointF(p.x, p.y) to r
      val c0 = centroids.getValue(element)
      return PointF(
        o.centerX * width + (p.x - c0.x) * o.scale,
        o.centerY * height + (p.y - c0.y) * o.scale
      ) to r * o.scale
    }
    defaultButtons.forEach { (b, d) ->
      val (c, r) = place(elementOf(b), d.first, d.second)
      buttons[b] = ButtonState(c, r)
    }
    defaultSticks.forEach { (s, d) ->
      val (c, r) = place(elementOf(s), d.first, d.second)
      sticks[s] = StickState(c, r)
    }
  }

  fun setLayout(layout: Map<Element, ElementLayout>) {
    resetAllInputs()
    layoutOverrides.clear()
    layoutOverrides.putAll(layout)
    applyLayout()
    invalidate()
  }

  fun resetLayout() {
    setLayout(emptyMap())
    onLayoutEdited?.invoke(layoutOverrides.toMap())
  }

  fun scaleSelected(factor: Float) {
    val element = selectedElement ?: return
    val cur = currentLayout(element)
    layoutOverrides[element] = cur.copy(scale = (cur.scale * factor).coerceIn(MIN_SCALE, MAX_SCALE))
    applyLayout()
    invalidate()
    onLayoutEdited?.invoke(layoutOverrides.toMap())
  }

  fun toggleSelectedVisibility() {
    val element = selectedElement ?: return
    val cur = currentLayout(element)
    layoutOverrides[element] = cur.copy(visible = !cur.visible)
    invalidate()
    onSelectionChanged?.invoke(element)
    onLayoutEdited?.invoke(layoutOverrides.toMap())
  }

  fun isSelectedVisible(): Boolean = selectedElement?.let(::isVisible) ?: true

  private fun elementCenter(element: Element): PointF {
    val l = currentLayout(element)
    return PointF(l.centerX * width, l.centerY * height)
  }

  private fun elementBoundRadius(element: Element, center: PointF): Float {
    var r = 0f
    buttons.forEach { (b, st) ->
      if (elementOf(b) == element) {
        r = max(r, hypot(st.center.x - center.x, st.center.y - center.y) + st.radius)
      }
    }
    sticks.forEach { (s, st) ->
      if (elementOf(s) == element) {
        r = max(r, hypot(st.center.x - center.x, st.center.y - center.y) + st.radius)
      }
    }
    return r
  }

  private fun elementAt(x: Float, y: Float): Element? {
    return editableElements()
      .map { e -> val c = elementCenter(e); Triple(e, c, elementBoundRadius(e, c)) }
      .filter { (_, c, r) -> hypot(x - c.x, y - c.y) <= r + editSlop }
      .minByOrNull { (_, _, r) -> r }
      ?.first
  }

  private fun selectElement(element: Element?) {
    if (selectedElement == element) return
    selectedElement = element
    onSelectionChanged?.invoke(element)
  }

  private fun beginDrag(event: MotionEvent, index: Int) {
    val element = selectedElement ?: return
    val c = elementCenter(element)
    editDragPointerId = event.getPointerId(index)
    editDragOffset.set(c.x - event.getX(index), c.y - event.getY(index))
  }

  private fun handleEditTouch(event: MotionEvent): Boolean {
    when (event.actionMasked) {
      MotionEvent.ACTION_DOWN -> {
        selectElement(elementAt(event.x, event.y))
        if (selectedElement != null) beginDrag(event, 0)
      }
      MotionEvent.ACTION_POINTER_DOWN -> {
        val element = selectedElement
        if (element != null && event.pointerCount >= 2) {
          pinchStartDistance = hypot(event.getX(0) - event.getX(1), event.getY(0) - event.getY(1))
          pinchStartScale = currentLayout(element).scale
        }
      }
      MotionEvent.ACTION_MOVE -> {
        val element = selectedElement ?: return true
        val cur = currentLayout(element)
        if (pinchStartDistance > 0f && event.pointerCount >= 2) {
          val d = hypot(event.getX(0) - event.getX(1), event.getY(0) - event.getY(1))
          val scale = (pinchStartScale * d / pinchStartDistance).coerceIn(MIN_SCALE, MAX_SCALE)
          layoutOverrides[element] = cur.copy(scale = scale)
        } else {
          val index = event.findPointerIndex(editDragPointerId)
          if (index >= 0) {
            layoutOverrides[element] = cur.copy(
              centerX = ((event.getX(index) + editDragOffset.x) / width).coerceIn(0f, 1f),
              centerY = ((event.getY(index) + editDragOffset.y) / height).coerceIn(0f, 1f),
            )
          }
        }
        applyLayout()
      }
      MotionEvent.ACTION_POINTER_UP -> {
        // Hand the drag to a finger that is still down, from where it is
        // now, so ending a pinch does not make the element jump.
        pinchStartDistance = 0f
        val remaining = (0 until event.pointerCount).firstOrNull { it != event.actionIndex }
        if (remaining != null) beginDrag(event, remaining)
      }
      MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
        editDragPointerId = -1
        pinchStartDistance = 0f
        onLayoutEdited?.invoke(layoutOverrides.toMap())
      }
    }
    invalidate()
    return true
  }

  /** Set the colour, faded for controls the user hid (only drawn in the editor). */
  private fun setPaintColor(color: Int, hidden: Boolean) {
    paint.color = color
    if (hidden) paint.alpha = paint.alpha / 4
  }

  override fun onDraw(canvas: Canvas) {
    super.onDraw(canvas)

    if (editMode) {
      canvas.drawColor(Color.argb(90, 0, 0, 0))
    }

    // Draw analog sticks
    sticks.forEach { (stick, state) ->
      val hidden = !isVisible(elementOf(stick))
      if (hidden && !editMode) return@forEach
      // Outer circle
      paint.style = Paint.Style.STROKE
      paint.strokeWidth = 4f
      setPaintColor(Color.argb(100, 255, 255, 255), hidden)
      canvas.drawCircle(state.center.x, state.center.y, state.radius, paint)

      // Dead zone circle
      setPaintColor(Color.argb(50, 255, 255, 255), hidden)
      canvas.drawCircle(state.center.x, state.center.y, state.radius * state.deadZone, paint)

      // Stick position
      val stickX = state.center.x + state.currentPos.x * state.radius
      val stickY = state.center.y + state.currentPos.y * state.radius
      
      paint.style = Paint.Style.FILL
      setPaintColor(
        if (state.isPressed) Color.argb(200, 100, 150, 255) else Color.argb(150, 200, 200, 200),
        hidden
      )
      canvas.drawCircle(stickX, stickY, state.radius * 0.4f, paint)
    }

    // Draw buttons
    buttons.forEach { (button, state) ->
      val hidden = !isVisible(elementOf(button))
      if (hidden && !editMode) return@forEach

      paint.style = Paint.Style.FILL
      val lit = state.isPressed || (button == Button.FAST_FORWARD && fastForward)
      setPaintColor(
        if (lit) getButtonPressedColor(button) else getButtonColor(button),
        hidden
      )
      canvas.drawCircle(state.center.x, state.center.y, state.radius, paint)

      // Button outline
      paint.style = Paint.Style.STROKE
      paint.strokeWidth = 3f
      setPaintColor(Color.argb(150, 255, 255, 255), hidden)
      canvas.drawCircle(state.center.x, state.center.y, state.radius, paint)

      // Button labels
      paint.style = Paint.Style.FILL
      setPaintColor(Color.WHITE, hidden)
      paint.textSize = state.radius * 0.8f
      paint.textAlign = Paint.Align.CENTER
      val label = getButtonLabel(button)
      canvas.drawText(label, state.center.x, state.center.y + state.radius * 0.3f, paint)
    }

    // Draw menu button (bottom-right corner)
    paint.style = Paint.Style.FILL
    paint.color = if (menuButtonPressed) {
      Color.argb(200, 120, 120, 220)
    } else {
      Color.argb(120, 100, 100, 180)
    }
    canvas.drawCircle(menuButtonCenter.x, menuButtonCenter.y, menuButtonRadius, paint)

    paint.style = Paint.Style.STROKE
    paint.strokeWidth = 3f
    paint.color = Color.argb(150, 255, 255, 255)
    canvas.drawCircle(menuButtonCenter.x, menuButtonCenter.y, menuButtonRadius, paint)

    paint.style = Paint.Style.FILL
    paint.color = Color.WHITE
    paint.textSize = menuButtonRadius * 1.0f
    paint.textAlign = Paint.Align.CENTER
    canvas.drawText("☰", menuButtonCenter.x, menuButtonCenter.y + menuButtonRadius * 0.35f, paint)

    if (editMode) drawEditOverlay(canvas)
  }

  private fun drawEditOverlay(canvas: Canvas) {
    paint.style = Paint.Style.STROKE
    editableElements().forEach { element ->
      val c = elementCenter(element)
      val r = elementBoundRadius(element, c) + editSlop * 0.5f
      val selected = element == selectedElement
      paint.strokeWidth = if (selected) 6f else 3f
      paint.pathEffect = if (selected) null else dashEffect
      paint.color = if (selected) Color.argb(255, 120, 220, 60) else Color.argb(170, 255, 255, 255)
      canvas.drawCircle(c.x, c.y, r, paint)
    }
    paint.pathEffect = null

    paint.style = Paint.Style.FILL
    paint.color = Color.WHITE
    paint.textAlign = Paint.Align.CENTER
    paint.textSize = height * 0.04f
    val hint = selectedElement?.let {
      if (isVisible(it)) it.label else "${it.label} (hidden)"
    } ?: "Drag a control to move it, pinch to resize"
    canvas.drawText(hint, width * 0.5f, height * 0.32f, paint)
  }

  private fun getButtonColor(button: Button): Int {
    return when (button) {
      Button.A -> Color.argb(150, 100, 200, 100)
      Button.B -> Color.argb(150, 200, 100, 100)
      Button.X -> Color.argb(150, 100, 150, 255)
      Button.Y -> Color.argb(150, 255, 255, 100)
      Button.BLACK -> Color.argb(150, 50, 50, 50)
      Button.WHITE -> Color.argb(150, 220, 220, 220)
      else -> Color.argb(120, 150, 150, 150)
    }
  }

  private fun getButtonPressedColor(button: Button): Int {
    return when (button) {
      Button.A -> Color.argb(255, 100, 255, 100)
      Button.B -> Color.argb(255, 255, 100, 100)
      Button.X -> Color.argb(255, 100, 150, 255)
      Button.Y -> Color.argb(255, 255, 255, 100)
      Button.BLACK -> Color.argb(255, 80, 80, 80)
      Button.WHITE -> Color.argb(255, 255, 255, 255)
      Button.FAST_FORWARD -> Color.argb(230, 255, 170, 40)
      else -> Color.argb(200, 200, 200, 200)
    }
  }

  private fun getButtonLabel(button: Button): String {
    return when (button) {
      Button.A -> "A"
      Button.B -> "B"
      Button.X -> "X"
      Button.Y -> "Y"
      Button.DPAD_UP -> "↑"
      Button.DPAD_DOWN -> "↓"
      Button.DPAD_LEFT -> "←"
      Button.DPAD_RIGHT -> "→"
      Button.LEFT_TRIGGER -> "LT"
      Button.RIGHT_TRIGGER -> "RT"
      Button.START -> "▶"
      Button.BACK -> "◀"
      Button.BLACK -> "BK"
      Button.WHITE -> "WH"
      Button.LEFT_STICK_BUTTON -> "LS"
      Button.RIGHT_STICK_BUTTON -> "RS"
      Button.FAST_FORWARD -> "▶▶"
    }
  }

  override fun onTouchEvent(event: MotionEvent): Boolean {
    if (editMode) {
      return handleEditTouch(event)
    }
    if (swipeUpGestureRecognizer.onTouchEvent(event)) {
      invalidate()
      return true
    }

    val pointerIndex = event.actionIndex
    val pointerId = event.getPointerId(pointerIndex)
    val x = event.getX(pointerIndex)
    val y = event.getY(pointerIndex)

    when (event.actionMasked) {
      MotionEvent.ACTION_DOWN, MotionEvent.ACTION_POINTER_DOWN -> {
        handleTouchDown(x, y, pointerId)
      }
      MotionEvent.ACTION_MOVE -> {
        for (i in 0 until event.pointerCount) {
          handleTouchMove(
            event.getX(i),
            event.getY(i),
            event.getPointerId(i)
          )
        }
      }
      MotionEvent.ACTION_POINTER_UP -> {
        handleTouchUp(pointerId)
      }
      MotionEvent.ACTION_UP -> {
        handleTouchUp(pointerId)
        // Last finger lifted — release everything as a safety net in case
        // pointer ID tracking got confused by multi-touch or system gestures.
        handleCancel()
      }
      MotionEvent.ACTION_CANCEL -> {
        handleCancel()
      }
    }

    invalidate()
    return true
  }

  /** Pointer currently aiming, -1 when none. */
  private var aimPointerId: Int = -1
  private var aimX: Float = 0.5f
  private var aimY: Float = 0.5f

  private fun reportAim(x: Float, y: Float, down: Boolean) {
    if (width <= 0 || height <= 0) return
    aimX = (x / width).coerceIn(0f, 1f)
    aimY = (y / height).coerceIn(0f, 1f)
    controllerListener?.onPointerMoved(aimX, aimY, down)
  }

  /** Let the trigger go but keep the gun pointed where it was. */
  private fun releaseAim() {
    if (aimPointerId == -1) return
    aimPointerId = -1
    controllerListener?.onPointerMoved(aimX, aimY, false)
  }

  private fun handleTouchDown(x: Float, y: Float, pointerId: Int) {
    // Check menu button first (UI button, not a controller input)
    if (menuButtonPointerId == -1 && isPointInCircle(x, y, menuButtonCenter, menuButtonRadius)) {
      menuButtonPressed = true
      menuButtonPointerId = pointerId
      return
    }

    // Check sticks first
    sticks.forEach { (stick, state) ->
      if (state.activePointerId == -1 && isVisible(elementOf(stick)) &&
        isPointInCircle(x, y, state.center, state.radius)) {
        state.activePointerId = pointerId
        updateStickPosition(stick, state, x, y)
        return
      }
    }

    // Check buttons
    buttons.forEach { (button, state) ->
      if (state.activePointerId == -1 && isVisible(elementOf(button)) &&
        isPointInCircle(x, y, state.center, state.radius)) {
        pressButton(button, state, pointerId)
        return
      }
    }

    // Nothing else wanted it, so treat it as aiming and firing.
    if (aimPointerId == -1) {
      aimPointerId = pointerId
      reportAim(x, y, true)
    }
  }

  private fun handleTouchMove(x: Float, y: Float, pointerId: Int) {
    if (aimPointerId == pointerId) {
      reportAim(x, y, true)
      return
    }

    if (menuButtonPointerId == pointerId) {
      menuButtonPressed = isPointInCircle(x, y, menuButtonCenter, menuButtonRadius)
      return
    }

    sticks.forEach { (stick, state) ->
      if (state.activePointerId == pointerId) {
        updateStickPosition(stick, state, x, y)
        return
      }
    }

    val activeButton = buttons.entries.firstOrNull { it.value.activePointerId == pointerId }
    if (activeButton != null) {
      val (button, state) = activeButton
      if (isPointInCircle(x, y, state.center, state.radius)) {
        return
      }
      releaseButton(button, state)
      // Only slide to a new button when this pointer was already tracking one.
      // This prevents other pointers from stealing a just-released button in the
      // same ACTION_MOVE batch, which would cause triggers (LT/RT) to get stuck.
      // Never onto fast-forward: a slide must not toggle it.
      val hoveredButton = buttons.entries.firstOrNull { (b, s) ->
        b != Button.FAST_FORWARD && s.activePointerId == -1 && isVisible(elementOf(b)) &&
          isPointInCircle(x, y, s.center, s.radius)
      }
      if (hoveredButton != null) {
        val (newButton, newState) = hoveredButton
        pressButton(newButton, newState, pointerId)
      }
    }
  }

  private fun handleTouchUp(pointerId: Int) {
    if (aimPointerId == pointerId) {
      releaseAim()
    }

    // Release menu button (fire on up = tap semantics)
    if (menuButtonPointerId == pointerId) {
      if (menuButtonPressed) {
        onMenuButtonTapped?.invoke()
      }
      menuButtonPressed = false
      menuButtonPointerId = -1
    }

    // Release sticks
    sticks.forEach { (stick, state) ->
      if (state.activePointerId == pointerId) {
        state.activePointerId = -1
        state.currentPos = PointF(0f, 0f)
        controllerListener?.onStickMoved(stick, 0f, 0f)
        if (state.isPressed) {
          state.isPressed = false
          controllerListener?.onStickReleased(stick)
        }
      }
    }

    // Release buttons
    buttons.forEach { (button, state) ->
      if (state.activePointerId == pointerId) {
        releaseButton(button, state)
      }
    }
  }

  private fun handleCancel() {
    swipeUpGestureRecognizer.reset()
    releaseAim()

    if (menuButtonPointerId != -1) {
      menuButtonPressed = false
      menuButtonPointerId = -1
    }

    sticks.forEach { (stick, state) ->
      if (state.activePointerId != -1) {
        state.activePointerId = -1
        state.currentPos = PointF(0f, 0f)
        controllerListener?.onStickMoved(stick, 0f, 0f)
        if (state.isPressed) {
          state.isPressed = false
          controllerListener?.onStickReleased(stick)
        }
      }
    }

    buttons.forEach { (button, state) ->
      if (state.isPressed) {
        releaseButton(button, state)
      }
    }
  }

  fun resetAllInputs() {
    handleCancel()
    invalidate()
  }

  override fun onDetachedFromWindow() {
    resetAllInputs()
    super.onDetachedFromWindow()
  }

  override fun onVisibilityChanged(changedView: View, visibility: Int) {
    super.onVisibilityChanged(changedView, visibility)
    if (changedView === this && visibility != View.VISIBLE) {
      resetAllInputs()
    }
  }

  private fun pressButton(button: Button, state: ButtonState, pointerId: Int) {
    state.isPressed = true
    state.activePointerId = pointerId
    if (button == Button.FAST_FORWARD) {
      fastForward = !fastForward
      onFastForwardChanged?.invoke(fastForward)
      return
    }
    controllerListener?.onButtonPressed(button)
  }

  private fun releaseButton(button: Button, state: ButtonState) {
    state.isPressed = false
    state.activePointerId = -1
    if (button == Button.FAST_FORWARD) return
    controllerListener?.onButtonReleased(button)
  }

  private fun updateStickPosition(stick: Stick, state: StickState, x: Float, y: Float) {
    val dx = x - state.center.x
    val dy = y - state.center.y
    val distance = sqrt(dx.pow(2) + dy.pow(2))

    if (distance > state.radius) {
      // Clamp to circle boundary
      state.currentPos.x = (dx / distance)
      state.currentPos.y = (dy / distance)
    } else {
      // Normalize to -1..1 range
      state.currentPos.x = dx / state.radius
      state.currentPos.y = dy / state.radius
    }

    // Apply dead zone
    val magnitude = sqrt(state.currentPos.x.pow(2) + state.currentPos.y.pow(2))
    if (magnitude < state.deadZone) {
      state.currentPos.x = 0f
      state.currentPos.y = 0f
    }

    controllerListener?.onStickMoved(stick, state.currentPos.x, state.currentPos.y)
  }

  private fun isPointInCircle(x: Float, y: Float, center: PointF, radius: Float): Boolean {
    val dx = x - center.x
    val dy = y - center.y
    return sqrt(dx.pow(2) + dy.pow(2)) <= radius
  }

  private fun isPointInInteractiveElement(x: Float, y: Float): Boolean {
    if (isPointInCircle(x, y, menuButtonCenter, menuButtonRadius)) {
      return true
    }

    if (sticks.any { (s, st) -> isVisible(elementOf(s)) && isPointInCircle(x, y, st.center, st.radius) }) {
      return true
    }

    return buttons.any { (b, st) -> isVisible(elementOf(b)) && isPointInCircle(x, y, st.center, st.radius) }
  }

  fun setVisibility(visible: Boolean) {
    visibility = if (visible) View.VISIBLE else View.GONE
  }
}
