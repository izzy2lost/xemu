package com.izzy2lost.x1box

import android.content.Context
import android.util.TypedValue
import android.view.Gravity
import android.view.View
import android.widget.LinearLayout
import android.widget.Space
import androidx.annotation.DrawableRes
import com.google.android.material.button.MaterialButton

/**
 * The in-game menus' buttons (MainActivity's for xemu, HakuxCoreActivity's): a tile per
 * item, its icon over its label, three to a row; a short last row keeps the tiles' width.
 */
object InGameMenuGrid {
  class Item(val label: String, @DrawableRes val icon: Int, val action: () -> Unit)

  private const val COLUMNS = 3

  /** The grid; a tap calls onPick with the item (the caller closes its menu and runs it). */
  fun build(context: Context, items: List<Item>, onPick: (Item) -> Unit): LinearLayout {
    val dp = context.resources.displayMetrics.density
    val spacing = (8 * dp).toInt()
    return LinearLayout(context).apply {
      orientation = LinearLayout.VERTICAL
      setPadding((20 * dp).toInt(), (12 * dp).toInt(), (20 * dp).toInt(), 0)
      for (rowItems in items.chunked(COLUMNS)) {
        addView(LinearLayout(context).apply {
          orientation = LinearLayout.HORIZONTAL
          // tiles stay top-aligned when one label wraps to two lines
          isBaselineAligned = false
          layoutParams = LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT,
          ).also { it.bottomMargin = spacing }
          for (i in 0 until COLUMNS) {
            val item = rowItems.getOrNull(i)
            val cell: View = if (item != null) tile(context, item, onPick) else Space(context)
            addView(cell, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.MATCH_PARENT, 1f).also { lp ->
              if (i < COLUMNS - 1) lp.marginEnd = spacing
            })
          }
        })
      }
    }
  }

  private fun tile(context: Context, item: Item, onPick: (Item) -> Unit): MaterialButton {
    val dp = context.resources.displayMetrics.density
    return MaterialButton(context, null, com.google.android.material.R.attr.materialButtonOutlinedStyle).apply {
      text = item.label
      setIconResource(item.icon)
      iconGravity = MaterialButton.ICON_GRAVITY_TOP
      iconSize = (26 * dp).toInt()
      iconPadding = (4 * dp).toInt()
      gravity = Gravity.CENTER
      textAlignment = View.TEXT_ALIGNMENT_CENTER
      setTextSize(TypedValue.COMPLEX_UNIT_SP, 13f)
      isSingleLine = false
      maxLines = 2
      insetTop = 0
      insetBottom = 0
      setPadding((6 * dp).toInt(), (10 * dp).toInt(), (6 * dp).toInt(), (10 * dp).toInt())
      setOnClickListener { onPick(item) }
    }
  }
}
