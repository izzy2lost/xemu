package com.izzy2lost.x1box

import android.net.Uri
import android.os.Bundle
import android.text.format.DateFormat
import android.text.format.Formatter
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.ImageButton
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.PopupMenu
import android.widget.TextView
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import androidx.recyclerview.widget.LinearLayoutManager
import androidx.recyclerview.widget.RecyclerView
import com.google.android.material.button.MaterialButton
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import com.google.android.material.progressindicator.LinearProgressIndicator
import java.io.File
import java.io.IOException
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.concurrent.ExecutorService
import java.util.concurrent.Executors

/** Lists, exports, imports and deletes the game saves on the configured HDD image. */
class SaveManagerActivity : AppCompatActivity() {

  companion object {
    private const val PREFS_NAME = "x1box_prefs"
    private const val STATE_PENDING_TITLES = "pending_export_titles"
    private const val STATE_PENDING_SAVES = "pending_export_saves"
  }

  private val prefs by lazy { getSharedPreferences(PREFS_NAME, MODE_PRIVATE) }
  private val worker: ExecutorService = Executors.newSingleThreadExecutor()

  private lateinit var storageText: TextView
  private lateinit var storageBar: LinearProgressIndicator
  private lateinit var busyBar: LinearProgressIndicator
  private lateinit var emptyText: TextView
  private lateinit var importButton: MaterialButton
  private lateinit var exportAllButton: MaterialButton
  private val adapter = TitleAdapter()

  private var inventory: XboxSaveManager.Inventory? = null
  private var titleDialog: AlertDialog? = null
  private var busy = false

  /* What the export ZIP picker is choosing a destination for. */
  private var pendingExportTitles: ArrayList<String> = arrayListOf()
  private var pendingExportSaves: ArrayList<String> = arrayListOf()

  private val createExportZip =
    registerForActivityResult(ActivityResultContracts.CreateDocument("application/zip")) { uri ->
      if (uri != null) {
        exportTo(uri, pendingExportTitles.toList(), pendingExportSaves.toList())
      }
      pendingExportTitles = arrayListOf()
      pendingExportSaves = arrayListOf()
    }

  private val pickImportZip =
    registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
      if (uri != null) {
        confirmImport(uri)
      }
    }

  override fun onCreate(savedInstanceState: Bundle?) {
    super.onCreate(savedInstanceState)
    OrientationLocker(this).enable()
    setContentView(R.layout.activity_save_manager)
    EdgeToEdgeHelper.enable(this)
    EdgeToEdgeHelper.applySystemBarPadding(findViewById(R.id.save_manager_root))

    savedInstanceState?.let { state ->
      pendingExportTitles = state.getStringArrayList(STATE_PENDING_TITLES) ?: arrayListOf()
      pendingExportSaves = state.getStringArrayList(STATE_PENDING_SAVES) ?: arrayListOf()
    }

    storageText = findViewById(R.id.tv_save_manager_storage)
    storageBar = findViewById(R.id.progress_save_manager_storage)
    busyBar = findViewById(R.id.progress_save_manager_busy)
    emptyText = findViewById(R.id.tv_save_manager_empty)
    importButton = findViewById(R.id.btn_save_manager_import)
    exportAllButton = findViewById(R.id.btn_save_manager_export_all)

    findViewById<RecyclerView>(R.id.recycler_save_titles).apply {
      layoutManager = LinearLayoutManager(this@SaveManagerActivity)
      adapter = this@SaveManagerActivity.adapter
    }

    importButton.setOnClickListener {
      pickImportZip.launch(arrayOf("application/zip", "application/x-zip-compressed", "application/octet-stream"))
    }
    exportAllButton.setOnClickListener {
      val titles = inventory?.titles.orEmpty()
      if (titles.isEmpty()) {
        Toast.makeText(this, R.string.save_manager_nothing_to_export, Toast.LENGTH_SHORT).show()
      } else {
        val stamp = SimpleDateFormat("yyyyMMdd", Locale.US).format(Date())
        requestExport("xbox-saves-$stamp.zip", titles.map { it.titleId }, emptyList())
      }
    }

    refresh()
  }

  override fun onSaveInstanceState(outState: Bundle) {
    super.onSaveInstanceState(outState)
    outState.putStringArrayList(STATE_PENDING_TITLES, pendingExportTitles)
    outState.putStringArrayList(STATE_PENDING_SAVES, pendingExportSaves)
  }

  override fun onDestroy() {
    titleDialog?.dismiss()
    worker.shutdown()
    super.onDestroy()
  }

  /* xemu's HDD image, or without one the hakuX core's E: folder. */
  private fun resolveDrive(): File =
    prefs.getString("hddPath", null)?.let(::File)?.takeIf { it.isFile }
      ?: HakuxHddSync.drive(this)

  /* Runs [work] off the main thread with the controls disabled, then [done] on it. */
  private fun <T> runBusy(work: (File) -> T, done: (Result<T>) -> Unit) {
    val hddFile = resolveDrive()
    if (busy) {
      return
    }
    setBusy(true)
    worker.execute {
      val result = runCatching { work(hddFile) }
      runOnUiThread {
        if (isDestroyed) {
          return@runOnUiThread
        }
        setBusy(false)
        done(result)
      }
    }
  }

  private fun setBusy(value: Boolean) {
    busy = value
    busyBar.visibility = if (value) View.VISIBLE else View.INVISIBLE
    importButton.isEnabled = !value
    exportAllButton.isEnabled = !value
  }

  private fun refresh() {
    storageText.setText(R.string.save_manager_loading)
    runBusy(
      work = { hddFile ->
        HakuxHddSync.importAfterRun(this)   /* a hakuX run's saves are the image's too */
        XboxSaveManager.scan(hddFile, XbeMetadataCache(this).knownTitles())
      },
      done = { result ->
        result.onSuccess(::showInventory).onFailure { error ->
          inventory = null
          adapter.submit(emptyList())
          storageText.text = ""
          emptyText.text = getString(R.string.save_manager_read_failed, error.message ?: error.javaClass.simpleName)
          emptyText.visibility = View.VISIBLE
        }
      },
    )
  }

  private fun showInventory(value: XboxSaveManager.Inventory) {
    inventory = value
    storageText.text = getString(
      R.string.save_manager_storage,
      Formatter.formatShortFileSize(this, value.freeBytes),
      Formatter.formatShortFileSize(this, value.totalBytes),
      value.titles.size,
    )
    storageBar.visibility = View.VISIBLE
    storageBar.progress = if (value.totalBytes > 0) {
      ((value.totalBytes - value.freeBytes) * 1000 / value.totalBytes).toInt()
    } else {
      0
    }
    adapter.submit(value.titles)
    emptyText.setText(R.string.save_manager_empty)
    emptyText.visibility = if (value.titles.isEmpty()) View.VISIBLE else View.GONE
  }

  private fun formatSize(bytes: Long): String = Formatter.formatShortFileSize(this, bytes)

  private fun formatDate(millis: Long): String? = millis.takeIf { it > 0 }?.let {
    DateFormat.getMediumDateFormat(this).format(Date(it)) + " " +
      DateFormat.getTimeFormat(this).format(Date(it))
  }

  private fun titleDetails(title: XboxSaveManager.SaveTitle): String = getString(
    R.string.save_manager_title_details,
    title.titleId.uppercase(Locale.ROOT),
    resources.getQuantityString(R.plurals.save_manager_save_count, title.saves.size, title.saves.size),
    formatSize(title.sizeBytes),
  )

  private fun showTitle(title: XboxSaveManager.SaveTitle) {
    val view = LayoutInflater.from(this).inflate(R.layout.dialog_save_title, null)
    bindIcon(view.findViewById(R.id.img_save_dialog_icon), title.icon)
    view.findViewById<TextView>(R.id.tv_save_dialog_name).text = title.name
    view.findViewById<TextView>(R.id.tv_save_dialog_details).text = titleDetails(title)
    view.findViewById<View>(R.id.tv_save_dialog_empty).visibility =
      if (title.saves.isEmpty()) View.VISIBLE else View.GONE

    val list = view.findViewById<LinearLayout>(R.id.layout_save_dialog_saves)
    for (save in title.saves) {
      val row = LayoutInflater.from(this).inflate(R.layout.item_save_entry, list, false)
      bindIcon(row.findViewById(R.id.img_save_entry_icon), save.icon)
      row.findViewById<TextView>(R.id.tv_save_entry_name).text = save.name
      row.findViewById<TextView>(R.id.tv_save_entry_details).text =
        listOfNotNull(formatDate(save.modifiedMillis), formatSize(save.sizeBytes)).joinToString(" · ")
      val menuButton = row.findViewById<ImageButton>(R.id.btn_save_entry_menu)
      val showMenu = { showSaveMenu(menuButton, title, save) }
      menuButton.setOnClickListener { showMenu() }
      row.setOnClickListener { showMenu() }
      list.addView(row)
    }

    titleDialog?.dismiss()
    titleDialog = MaterialAlertDialogBuilder(this, R.style.ThemeOverlay_Xemu_RoundedDialog)
      .setView(view)
      .setPositiveButton(R.string.save_manager_export_game) { _, _ ->
        requestExport("${safeFileName(title.name)} saves.zip", listOf(title.titleId), emptyList())
      }
      .setNeutralButton(R.string.save_manager_delete_game) { _, _ -> confirmDeleteTitle(title) }
      .setNegativeButton(R.string.save_manager_close, null)
      .setOnDismissListener { titleDialog = null }
      .show()
  }

  private fun showSaveMenu(anchor: View, title: XboxSaveManager.SaveTitle, save: XboxSaveManager.SaveGame) {
    PopupMenu(this, anchor).apply {
      menu.add(0, 1, 0, R.string.save_manager_export)
      menu.add(0, 2, 1, R.string.save_manager_delete)
      setOnMenuItemClickListener { item ->
        when (item.itemId) {
          1 -> requestExport(
            "${safeFileName(title.name)} - ${safeFileName(save.name)}.zip",
            emptyList(),
            listOf(save.exportPath),
          )
          2 -> confirmDeleteSave(title, save)
        }
        true
      }
      show()
    }
  }

  private fun requestExport(suggestedName: String, titleIds: List<String>, savePaths: List<String>) {
    pendingExportTitles = ArrayList(titleIds)
    pendingExportSaves = ArrayList(savePaths)
    createExportZip.launch(suggestedName)
  }

  private fun exportTo(uri: Uri, titleIds: List<String>, savePaths: List<String>) {
    runBusy(
      work = { hddFile ->
        val output = contentResolver.openOutputStream(uri, "wt")
          ?: throw IOException("Could not open the destination file.")
        output.use {
          XboxSaveManager.exportZip(hddFile, titleIds, savePaths, it, File(cacheDir, "save-export"))
        }
      },
      done = { result ->
        result.onSuccess {
          Toast.makeText(this, R.string.save_manager_export_done, Toast.LENGTH_SHORT).show()
        }.onFailure { error ->
          Toast.makeText(
            this,
            getString(R.string.save_manager_export_failed, error.message ?: error.javaClass.simpleName),
            Toast.LENGTH_LONG,
          ).show()
        }
      },
    )
  }

  private fun confirmImport(uri: Uri) {
    MaterialAlertDialogBuilder(this, R.style.ThemeOverlay_Xemu_RoundedDialog)
      .setTitle(R.string.save_manager_import_title)
      .setMessage(R.string.save_manager_import_message)
      .setPositiveButton(R.string.save_manager_import) { _, _ -> importFrom(uri) }
      .setNegativeButton(android.R.string.cancel, null)
      .show()
  }

  private fun importFrom(uri: Uri) {
    runBusy(
      work = { hddFile ->
        val input = contentResolver.openInputStream(uri)
          ?: throw IOException("Could not open the selected file.")
        input.use { XboxSaveManager.importZip(hddFile, it, File(cacheDir, "save-import")) }
      },
      done = { result ->
        result.onSuccess { titleIds ->
          val names = titleIds.map { id ->
            inventory?.titles?.firstOrNull { it.titleId.equals(id, ignoreCase = true) }?.name
              ?: id.uppercase(Locale.ROOT)
          }
          Toast.makeText(
            this,
            getString(R.string.save_manager_import_done, names.joinToString(", ")),
            Toast.LENGTH_LONG,
          ).show()
        }.onFailure { error ->
          Toast.makeText(
            this,
            getString(R.string.save_manager_import_failed, error.message ?: error.javaClass.simpleName),
            Toast.LENGTH_LONG,
          ).show()
        }
        refresh()
      },
    )
  }

  private fun confirmDeleteSave(title: XboxSaveManager.SaveTitle, save: XboxSaveManager.SaveGame) {
    MaterialAlertDialogBuilder(this, R.style.ThemeOverlay_Xemu_RoundedDialog)
      .setTitle(R.string.save_manager_delete_save_title)
      .setMessage(getString(R.string.save_manager_delete_save_message, save.name, title.name))
      .setPositiveButton(R.string.save_manager_delete) { _, _ ->
        runDelete { hddFile -> XboxSaveManager.delete(hddFile, save) }
      }
      .setNegativeButton(android.R.string.cancel, null)
      .show()
  }

  private fun confirmDeleteTitle(title: XboxSaveManager.SaveTitle) {
    MaterialAlertDialogBuilder(this, R.style.ThemeOverlay_Xemu_RoundedDialog)
      .setTitle(R.string.save_manager_delete_game_title)
      .setMessage(getString(R.string.save_manager_delete_game_message, title.name))
      .setPositiveButton(R.string.save_manager_delete) { _, _ ->
        runDelete { hddFile -> XboxSaveManager.delete(hddFile, title) }
      }
      .setNegativeButton(android.R.string.cancel, null)
      .show()
  }

  private fun runDelete(work: (File) -> Unit) {
    titleDialog?.dismiss()
    runBusy(work) { result ->
      result.onSuccess {
        Toast.makeText(this, R.string.save_manager_delete_done, Toast.LENGTH_SHORT).show()
      }.onFailure { error ->
        Toast.makeText(
          this,
          getString(R.string.save_manager_delete_failed, error.message ?: error.javaClass.simpleName),
          Toast.LENGTH_LONG,
        ).show()
      }
      refresh()
    }
  }

  private fun bindIcon(view: ImageView, bitmap: android.graphics.Bitmap?) {
    if (bitmap != null) {
      view.setImageBitmap(bitmap)
      view.setPadding(0, 0, 0, 0)
    } else {
      view.setImageResource(R.drawable.ic_xemu_hard_drive)
      val pad = (view.resources.displayMetrics.density * 12).toInt()
      view.setPadding(pad, pad, pad, pad)
    }
  }

  private fun safeFileName(name: String): String =
    name.replace(Regex("[\\\\/:*?\"<>|\\p{Cntrl}]"), "_").trim().ifEmpty { "save" }.take(80)

  private inner class TitleAdapter : RecyclerView.Adapter<TitleAdapter.Holder>() {
    private var titles: List<XboxSaveManager.SaveTitle> = emptyList()

    inner class Holder(view: View) : RecyclerView.ViewHolder(view) {
      val icon: ImageView = view.findViewById(R.id.img_save_title_icon)
      val name: TextView = view.findViewById(R.id.tv_save_title_name)
      val details: TextView = view.findViewById(R.id.tv_save_title_details)
    }

    @android.annotation.SuppressLint("NotifyDataSetChanged")
    fun submit(value: List<XboxSaveManager.SaveTitle>) {
      titles = value
      notifyDataSetChanged()
    }

    override fun getItemCount(): Int = titles.size

    override fun onCreateViewHolder(parent: ViewGroup, viewType: Int): Holder =
      Holder(LayoutInflater.from(parent.context).inflate(R.layout.item_save_title, parent, false))

    override fun onBindViewHolder(holder: Holder, position: Int) {
      val title = titles[position]
      bindIcon(holder.icon, title.icon)
      holder.name.text = title.name
      holder.details.text = titleDetails(title)
      holder.itemView.setOnClickListener {
        if (!busy) {
          showTitle(title)
        }
      }
    }
  }
}
