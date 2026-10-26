package com.izzy2lost.x1box

import android.content.Context
import android.content.res.ColorStateList
import android.graphics.drawable.GradientDrawable
import android.graphics.drawable.RippleDrawable
import android.graphics.drawable.StateListDrawable
import android.util.TypedValue
import android.view.Gravity
import android.view.View
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.Space
import android.widget.TextView
import androidx.annotation.DrawableRes
import androidx.core.content.ContextCompat
import androidx.core.graphics.ColorUtils
import com.google.android.material.color.MaterialColors

/**
 * The in-game menus' buttons (MainActivity's for xemu, HakuxCoreActivity's): a tile per
 * item, three to a row; a short last row keeps the tiles' width and is centred. On a short screen (a
 * phone in landscape) the icon sits beside its label, so four rows fit; on a tall one
 * it stands above it. Tiles size to their content, so the menu scrolls rather than
 * squeezing them (Material buttons with a top icon clipped their labels on a Pixel).
 */
object InGameMenuGrid {
  class Item(val label: String, @DrawableRes val icon: Int, val action: () -> Unit)

  private const val COLUMNS = 3
  /* below this screen height (dp) the icon goes beside the label */
  private const val COMPACT_BELOW_DP = 480

  /** The grid; a tap calls onPick with the item (the caller closes its menu and runs it). */
  fun build(context: Context, items: List<Item>, onPick: (Item) -> Unit): LinearLayout {
    val dp = context.resources.displayMetrics.density
    val spacing = (8 * dp).toInt()
    val compact = context.resources.configuration.screenHeightDp < COMPACT_BELOW_DP
    return LinearLayout(context).apply {
      orientation = LinearLayout.VERTICAL
      setPadding((20 * dp).toInt(), (8 * dp).toInt(), (20 * dp).toInt(), 0)
      for (rowItems in items.chunked(COLUMNS)) {
        addView(LinearLayout(context).apply {
          orientation = LinearLayout.HORIZONTAL
          layoutParams = LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT,
          ).also { it.bottomMargin = spacing }
          // a short last row is centred: half the missing tiles' room (and gaps) either side
          val missing = COLUMNS - rowItems.size
          fun pad(start: Boolean) = addView(Space(context), LinearLayout.LayoutParams(0, 0, missing / 2f).also { lp ->
            if (start) lp.marginEnd = missing * spacing / 2 else lp.marginStart = missing * spacing / 2
          })
          if (missing > 0) pad(start = true)
          rowItems.forEachIndexed { i, item ->
            // a row's tiles share its height: the tallest label sets it
            addView(tile(context, item, compact, onPick),
              LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.MATCH_PARENT, 1f).also { lp ->
                if (i < rowItems.size - 1) lp.marginEnd = spacing
              })
          }
          if (missing > 0) pad(start = false)
        })
      }
    }
  }

  private fun tile(context: Context, item: Item, compact: Boolean, onPick: (Item) -> Unit): View {
    val dp = context.resources.displayMetrics.density
    val accent = MaterialColors.getColor(
      context, com.google.android.material.R.attr.colorPrimary,
      ContextCompat.getColor(context, R.color.xemu_green),
    )
    val icon = ImageView(context).apply {
      setImageResource(item.icon)
      imageTintList = ColorStateList.valueOf(accent)
    }
    val label = FitLabel(context).apply {
      text = item.label
      setTextColor(accent)
      setTextSize(TypedValue.COMPLEX_UNIT_SP, if (compact) 14f else 13f)
      maxLines = 2
      gravity = if (compact) Gravity.CENTER_VERTICAL or Gravity.START else Gravity.CENTER
    }
    val iconSize = ((if (compact) 22 else 26) * dp).toInt()
    return LinearLayout(context).apply {
      orientation = if (compact) LinearLayout.HORIZONTAL else LinearLayout.VERTICAL
      gravity = if (compact) Gravity.CENTER_VERTICAL or Gravity.START else Gravity.CENTER
      minimumHeight = (48 * dp).toInt()
      val padH = ((if (compact) 12 else 8) * dp).toInt()
      val padV = ((if (compact) 8 else 10) * dp).toInt()
      setPadding(padH, padV, padH, padV)
      background = tileBackground(accent, dp)
      isClickable = true
      isFocusable = true
      contentDescription = item.label
      addView(icon, LinearLayout.LayoutParams(iconSize, iconSize))
      addView(label, LinearLayout.LayoutParams(
        if (compact) 0 else LinearLayout.LayoutParams.WRAP_CONTENT,
        LinearLayout.LayoutParams.WRAP_CONTENT,
        if (compact) 1f else 0f,
      ).also { lp ->
        if (compact) lp.marginStart = (10 * dp).toInt() else lp.topMargin = (4 * dp).toInt()
      })
      setOnClickListener { onPick(item) }
    }
  }

  /* A label that shrinks its text (to 10sp at least) until its longest word fits on a
   * line, so a narrow tile wraps "Remap Controller" between words, never inside one. */
  private class FitLabel(context: Context) : TextView(context) {
    private var fullSize = 0f

    override fun onMeasure(widthMeasureSpec: Int, heightMeasureSpec: Int) {
      if (fullSize == 0f) fullSize = textSize
      val width = MeasureSpec.getSize(widthMeasureSpec) - totalPaddingLeft - totalPaddingRight
      if (MeasureSpec.getMode(widthMeasureSpec) != MeasureSpec.UNSPECIFIED && width > 0) {
        val min = 10 * resources.displayMetrics.scaledDensity
        var size = fullSize
        paint.textSize = size
        val words = text.split(' ')
        while (size > min && words.any { paint.measureText(it) > width }) {
          size -= resources.displayMetrics.density
          paint.textSize = size
        }
        if (size != textSize) setTextSize(TypedValue.COMPLEX_UNIT_PX, size)
      }
      super.onMeasure(widthMeasureSpec, heightMeasureSpec)
    }
  }

  /* An outlined rounded tile, filled faintly while focused (a gamepad's selection) and
   * with a ripple when pressed. */
  private fun tileBackground(accent: Int, dp: Float): RippleDrawable {
    fun shape(fill: Int) = GradientDrawable().apply {
      cornerRadius = 18 * dp
      setStroke((2 * dp).toInt(), accent)
      setColor(fill)
    }
    val states = StateListDrawable().apply {
      addState(intArrayOf(android.R.attr.state_focused), shape(ColorUtils.setAlphaComponent(accent, 0x40)))
      addState(intArrayOf(), shape(0))
    }
    val mask = GradientDrawable().apply {
      cornerRadius = 18 * dp
      setColor(0xFFFFFFFF.toInt())
    }
    return RippleDrawable(ColorStateList.valueOf(ColorUtils.setAlphaComponent(accent, 0x55)), states, mask)
  }
}
