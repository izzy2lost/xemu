package com.izzy2lost.x1box

import android.content.Intent
import android.content.res.Configuration
import android.net.Uri
import android.os.Bundle
import android.text.SpannableStringBuilder
import android.text.Spanned
import android.text.method.LinkMovementMethod
import android.text.style.ClickableSpan
import android.view.LayoutInflater
import android.view.View
import android.widget.ImageButton
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.ProgressBar
import android.widget.Space
import android.widget.TextView
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.documentfile.provider.DocumentFile
import coil.load
import com.google.android.material.button.MaterialButton
import com.google.android.material.button.MaterialButtonToggleGroup
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import com.google.android.material.materialswitch.MaterialSwitch
import java.net.URLEncoder
import java.util.ArrayDeque
import java.util.Locale
import java.util.concurrent.ConcurrentHashMap

class GameLibraryActivity : AppCompatActivity() {
  private data class GameEntry(
    val title: String,
    val uri: Uri,
    val relativePath: String,
    val sizeBytes: Long
  )

  private data class CoverEntry(
    val collapsed: String,
    val tokens: Set<String>,
    val numericTokens: Set<String>,
    val url: String
  )

  private val prefs by lazy { getSharedPreferences("x1box_prefs", MODE_PRIVATE) }
  private val gameExts = setOf("iso", "xiso", "cso", "cci")
  private val titleStopWords = setOf("the", "a", "an", "and", "of", "for", "in", "on", "to")
  private val coverRepoBaseUrl = "https://raw.githubusercontent.com/izzy2lost/X1_Covers/main/"
  private val boxArtCache = ConcurrentHashMap<String, String>()
  private val boxArtMisses = ConcurrentHashMap.newKeySet<String>()
  private val coverIndex = ConcurrentHashMap<String, String>()
  private val coverCollapsedIndex = ConcurrentHashMap<String, String>()
  private val coverEntries = ArrayList<CoverEntry>()
  @Volatile private var coverIndexLoaded = false

  private lateinit var folderText: TextView
  private lateinit var loadingSpinner: ProgressBar
  private lateinit var loadingText: TextView
  private lateinit var emptyText: TextView
  private lateinit var gamesListContainer: LinearLayout
  private lateinit var gamesGridContainer: LinearLayout
  private lateinit var btnChangeFolder: MaterialButton
  private lateinit var btnAbout: ImageButton
  private lateinit var viewModeToggle: MaterialButtonToggleGroup
  private lateinit var switchBoxArtLookup: MaterialSwitch

  private var gamesFolderUri: Uri? = null
  private var scanGeneration = 0
  private var currentGames: List<GameEntry> = emptyList()
  private var useCoverGrid = false
  private var boxArtLookupEnabled = true

  private val pickGamesFolder =
    registerForActivityResult(ActivityResultContracts.OpenDocumentTree()) { uri ->
      if (uri != null) {
        persistUriPermission(uri)
        gamesFolderUri = uri
        prefs.edit().putString("gamesFolderUri", uri.toString()).apply()
        boxArtCache.clear()
        boxArtMisses.clear()
        loadGames()
      }
    }

  override fun onCreate(savedInstanceState: Bundle?) {
    super.onCreate(savedInstanceState)
    setContentView(R.layout.activity_game_library)

    folderText = findViewById(R.id.library_folder_text)
    loadingSpinner = findViewById(R.id.library_loading)
    loadingText = findViewById(R.id.library_loading_text)
    emptyText = findViewById(R.id.library_empty_text)
    gamesListContainer = findViewById(R.id.library_games_container)
    gamesGridContainer = findViewById(R.id.library_games_grid_container)
    btnChangeFolder = findViewById(R.id.btn_change_games_folder)
    btnAbout = findViewById(R.id.btn_library_about)
    viewModeToggle = findViewById(R.id.library_view_mode_toggle)
    switchBoxArtLookup = findViewById(R.id.switch_box_art_lookup)

    gamesFolderUri = prefs.getString("gamesFolderUri", null)?.let(Uri::parse)
    useCoverGrid = prefs.getBoolean("library_cover_grid", false)
    boxArtLookupEnabled = prefs.getBoolean("library_box_art_lookup", true)

    switchBoxArtLookup.isChecked = boxArtLookupEnabled
    viewModeToggle.check(if (useCoverGrid) R.id.btn_view_grid else R.id.btn_view_list)
    syncDisplayModeUi()

    btnChangeFolder.setOnClickListener {
      pickGamesFolder.launch(gamesFolderUri)
    }
    btnAbout.setOnClickListener {
      showAboutDialog()
    }
    viewModeToggle.addOnButtonCheckedListener { _, checkedId, isChecked ->
      if (!isChecked) {
        return@addOnButtonCheckedListener
      }
      val nextGrid = checkedId == R.id.btn_view_grid
      if (nextGrid == useCoverGrid) {
        return@addOnButtonCheckedListener
      }
      useCoverGrid = nextGrid
      prefs.edit().putBoolean("library_cover_grid", useCoverGrid).apply()
      syncDisplayModeUi()
      renderGames()
    }
    switchBoxArtLookup.setOnCheckedChangeListener { _, checked ->
      boxArtLookupEnabled = checked
      prefs.edit().putBoolean("library_box_art_lookup", checked).apply()
      if (useCoverGrid) {
        renderGames()
      }
    }

    if (!isFolderReady(gamesFolderUri)) {
      folderText.text = getString(R.string.library_no_folder)
      Toast.makeText(this, getString(R.string.setup_pick_disc), Toast.LENGTH_SHORT).show()
      pickGamesFolder.launch(gamesFolderUri)
      return
    }

    loadGames()
  }

  private fun loadGames() {
    val folderUri = gamesFolderUri
    if (!isFolderReady(folderUri)) {
      setLoading(false)
      currentGames = emptyList()
      renderGames()
      folderText.text = getString(R.string.library_no_folder)
      return
    }

    folderText.text = getString(R.string.library_folder_value, formatTreeLabel(folderUri!!))
    setLoading(true)

    val generation = ++scanGeneration
    Thread {
      val games = scanFolderForGames(folderUri)
      runOnUiThread {
        if (generation != scanGeneration) {
          return@runOnUiThread
        }
        setLoading(false)
        currentGames = games
        renderGames()
      }
    }.start()
  }

  private fun syncDisplayModeUi() {
    switchBoxArtLookup.visibility = if (useCoverGrid) View.VISIBLE else View.GONE
    gamesListContainer.visibility = if (useCoverGrid) View.GONE else View.VISIBLE
    gamesGridContainer.visibility = if (useCoverGrid) View.VISIBLE else View.GONE
  }

  private fun setLoading(loading: Boolean) {
    loadingSpinner.visibility = if (loading) View.VISIBLE else View.GONE
    loadingText.visibility = if (loading) View.VISIBLE else View.GONE
  }

  private fun renderGames() {
    val games = currentGames
    syncDisplayModeUi()
    gamesListContainer.removeAllViews()
    gamesGridContainer.removeAllViews()
    emptyText.visibility = if (games.isEmpty()) View.VISIBLE else View.GONE
    if (games.isEmpty()) {
      return
    }

    if (useCoverGrid) {
      renderCoverGrid(games)
    } else {
      renderList(games)
    }
  }

  private fun renderList(games: List<GameEntry>) {
    val inflater = LayoutInflater.from(this)
    for (game in games) {
      val item = inflater.inflate(R.layout.item_game_entry, gamesListContainer, false)
      val nameText = item.findViewById<TextView>(R.id.game_name_text)
      val sizeText = item.findViewById<TextView>(R.id.game_size_text)
      val pathText = item.findViewById<TextView>(R.id.game_path_text)

      nameText.text = game.title
      sizeText.text = getString(R.string.library_game_size, formatSize(game.sizeBytes))
      pathText.text = getString(R.string.library_game_path, game.relativePath)

      item.setOnClickListener { launchGame(game) }
      gamesListContainer.addView(item)
    }
  }

  private fun renderCoverGrid(games: List<GameEntry>) {
    val inflater = LayoutInflater.from(this)
    var row: LinearLayout? = null
    val columns = resolveCoverGridColumns()
    val spacingPx = dp(8)
    val halfSpacingPx = spacingPx / 2

    for ((index, game) in games.withIndex()) {
      val columnIndex = index % columns
      if (columnIndex == 0) {
        row = LinearLayout(this).apply {
          orientation = LinearLayout.HORIZONTAL
        }
        val rowLp = LinearLayout.LayoutParams(
          LinearLayout.LayoutParams.MATCH_PARENT,
          LinearLayout.LayoutParams.WRAP_CONTENT
        )
        if (index >= columns) {
          rowLp.topMargin = dp(12)
        }
        gamesGridContainer.addView(row, rowLp)
      }

      val item = inflater.inflate(R.layout.item_game_cover, row, false)
      val itemLp = LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f)
      itemLp.marginStart = if (columnIndex == 0) 0 else halfSpacingPx
      itemLp.marginEnd = if (columnIndex == columns - 1) 0 else halfSpacingPx
      row!!.addView(item, itemLp)

      val nameText = item.findViewById<TextView>(R.id.game_cover_name_text)
      val sizeText = item.findViewById<TextView>(R.id.game_cover_size_text)
      val coverImage = item.findViewById<ImageView>(R.id.game_cover_image)

      nameText.text = game.title
      sizeText.text = getString(R.string.library_game_size, formatSize(game.sizeBytes))
      item.setOnClickListener { launchGame(game) }
      bindCoverArt(coverImage, game)
    }

    val remainder = games.size % columns
    if (remainder != 0) {
      for (columnIndex in remainder until columns) {
        val filler = Space(this)
        val fillerLp = LinearLayout.LayoutParams(0, 0, 1f)
        fillerLp.marginStart = if (columnIndex == 0) 0 else halfSpacingPx
        fillerLp.marginEnd = if (columnIndex == columns - 1) 0 else halfSpacingPx
        row?.addView(filler, fillerLp)
      }
    }
  }

  private fun resolveCoverGridColumns(): Int {
    val widthDp = resources.configuration.screenWidthDp
    val suggested = (widthDp / 180).coerceIn(2, 4)
    return if (resources.configuration.orientation == Configuration.ORIENTATION_LANDSCAPE) {
      maxOf(3, suggested)
    } else {
      suggested
    }
  }

  private fun bindCoverArt(coverView: ImageView, game: GameEntry) {
    coverView.tag = game.uri.toString()
    coverView.setImageResource(android.R.drawable.ic_menu_report_image)

    if (!boxArtLookupEnabled) {
      return
    }

    val key = normalizeCoverKey(game.title)
    val cachedUrl = boxArtCache[key]
    if (cachedUrl != null) {
      applyBoxArtToView(coverView, cachedUrl)
      return
    }

    val url = lookupBoxArtUrl(game.title) ?: return
    boxArtCache[key] = url
    if (coverView.tag == game.uri.toString()) {
      applyBoxArtToView(coverView, url)
    }
  }

  private fun applyBoxArtToView(coverView: ImageView, url: String) {
    coverView.load(url) {
      crossfade(true)
      placeholder(android.R.drawable.ic_menu_report_image)
      error(android.R.drawable.ic_menu_report_image)
    }
  }

  private fun lookupBoxArtUrl(title: String): String? {
    ensureCoverIndexLoaded()
    val candidates = linkedSetOf<String>()
    val cleanTitle = normalizeLookupTitle(title)
    val normalizedTitle = normalizeCoverKey(cleanTitle)
    if (normalizedTitle.isBlank()) {
      return null
    }
    if (boxArtMisses.contains(normalizedTitle)) {
      return null
    }
    addCoverLookupCandidates(candidates, title)
    addCoverLookupCandidates(candidates, cleanTitle)
    addCoverLookupCandidates(candidates, cleanTitle.replace(":", ""))
    addCoverLookupCandidates(candidates, cleanTitle.substringBefore(" - ").trim())
    addCoverLookupCandidates(candidates, cleanTitle.substringBefore(":").trim())

    for (key in candidates) {
      val found = coverIndex[key]
      if (!found.isNullOrBlank()) {
        return found
      }
    }

    for (key in candidates) {
      val collapsed = collapseCoverKey(key)
      if (collapsed.isBlank()) {
        continue
      }
      val found = coverCollapsedIndex[collapsed]
      if (!found.isNullOrBlank()) {
        coverIndex.putIfAbsent(key, found)
        return found
      }
    }

    val fuzzyMatch = findClosestCoverUrl(candidates)
    if (!fuzzyMatch.isNullOrBlank()) {
      for (key in candidates) {
        coverIndex.putIfAbsent(key, fuzzyMatch)
        val collapsed = collapseCoverKey(key)
        if (collapsed.isNotBlank()) {
          coverCollapsedIndex.putIfAbsent(collapsed, fuzzyMatch)
        }
      }
      return fuzzyMatch
    }

    boxArtMisses.add(normalizedTitle)
    return null
  }

  private fun addCoverLookupCandidates(out: MutableSet<String>, raw: String) {
    if (raw.isBlank()) {
      return
    }
    val normalized = normalizeCoverKey(raw)
    if (normalized.isBlank()) {
      return
    }
    out.add(normalized)
    out.add(stripTrailingRegion(normalized))
  }

  private fun ensureCoverIndexLoaded() {
    if (coverIndexLoaded) {
      return
    }
    try {
      val lines = assets.open("X1_Covers.txt").bufferedReader().use { it.readLines() }
      val seenEntries = HashSet<String>()
      for (line in lines) {
        val fileName = line.trim()
        if (fileName.isEmpty() || !fileName.endsWith(".png", ignoreCase = true)) {
          continue
        }
        val gameName = fileName.removeSuffix(".png").trim()
        val encoded = URLEncoder.encode(fileName, "UTF-8").replace("+", "%20")
        val url = coverRepoBaseUrl + encoded

        val exactKey = normalizeCoverKey(gameName)
        val strippedKey = stripTrailingRegion(exactKey)
        if (exactKey.isNotEmpty()) {
          coverIndex.putIfAbsent(exactKey, url)
        }
        if (strippedKey.isNotEmpty()) {
          coverIndex.putIfAbsent(strippedKey, url)
        }
        val canonical = if (strippedKey.isNotEmpty()) strippedKey else exactKey
        val collapsed = collapseCoverKey(canonical)
        if (collapsed.isNotEmpty()) {
          coverCollapsedIndex.putIfAbsent(collapsed, url)
        }
        if (canonical.isNotEmpty() && seenEntries.add("$canonical|$url")) {
          val tokens = tokenizeCoverKey(canonical)
          coverEntries.add(
            CoverEntry(
              collapsed = collapsed,
              tokens = tokens,
              numericTokens = tokens.filterTo(HashSet()) { token -> token.any(Char::isDigit) },
              url = url
            )
          )
        }
      }
    } catch (_: Exception) {
      // Keep empty index; grid will show placeholders if the asset is unavailable.
    }
    coverIndexLoaded = true
  }

  private fun normalizeLookupTitle(input: String): String {
    var title = input.trim()
    title = title.replace('_', ' ')
    title = title.replace(Regex("\\[[^\\]]*\\]"), " ")
    title = title.replace(Regex("\\([^\\)]*\\)"), " ")
    title = title.replace(Regex("\\s+"), " ").trim()
    return title
  }

  private fun normalizeCoverKey(input: String): String {
    var title = input.lowercase(Locale.ROOT).trim()
    title = title.replace('_', ' ')
    title = title.replace('\u2019', '\'')
    title = title.replace("’", "'")
    title = title.replace(Regex("\\s+"), " ")
    return title
  }

  private fun stripTrailingRegion(input: String): String {
    return input.replace(Regex("\\s*\\([^\\)]*\\)\\s*$"), "").trim()
  }

  private fun collapseCoverKey(input: String): String {
    return normalizeCoverKey(input).replace(Regex("[^a-z0-9]+"), "")
  }

  private fun tokenizeCoverKey(input: String): Set<String> {
    return normalizeCoverKey(input)
      .replace(Regex("[^a-z0-9]+"), " ")
      .split(' ')
      .asSequence()
      .map { it.trim() }
      .filter { it.length >= 2 }
      .filter { it !in titleStopWords }
      .toSet()
  }

  private fun findClosestCoverUrl(candidates: Set<String>): String? {
    var bestUrl: String? = null
    var bestScore = 0
    for (candidate in candidates) {
      val collapsed = collapseCoverKey(candidate)
      val tokens = tokenizeCoverKey(candidate)
      if (collapsed.isBlank() || tokens.isEmpty()) {
        continue
      }
      val numericTokens = tokens.filterTo(HashSet()) { token -> token.any(Char::isDigit) }
      for (entry in coverEntries) {
        val score = scoreCoverMatch(collapsed, tokens, numericTokens, entry)
        if (score > bestScore) {
          bestScore = score
          bestUrl = entry.url
        }
      }
    }
    return if (bestScore >= 55) bestUrl else null
  }

  private fun scoreCoverMatch(
    candidateCollapsed: String,
    candidateTokens: Set<String>,
    candidateNumericTokens: Set<String>,
    entry: CoverEntry
  ): Int {
    if (candidateCollapsed == entry.collapsed) {
      return 100
    }

    if (candidateNumericTokens.isNotEmpty() &&
      entry.numericTokens.isNotEmpty() &&
      candidateNumericTokens != entry.numericTokens
    ) {
      return 0
    }

    val overlapCount = candidateTokens.count { token -> entry.tokens.contains(token) }
    if (overlapCount == 0) {
      return 0
    }

    val maxTokenCount = maxOf(candidateTokens.size, entry.tokens.size)
    var score = (overlapCount * 70) / maxTokenCount

    if (candidateCollapsed.contains(entry.collapsed) || entry.collapsed.contains(candidateCollapsed)) {
      score += 20
    }

    val lengthDelta = kotlin.math.abs(candidateCollapsed.length - entry.collapsed.length)
    if (lengthDelta <= 4) {
      score += 10
    } else if (lengthDelta <= 10) {
      score += 5
    }

    return score
  }

  private fun launchGame(game: GameEntry) {
    persistUriPermission(game.uri)
    prefs.edit()
      .putString("dvdUri", game.uri.toString())
      .remove("dvdPath")
      .putBoolean("skip_game_picker", false)
      .apply()

    startActivity(Intent(this, MainActivity::class.java))
    finish()
  }

  private fun scanFolderForGames(folderUri: Uri): List<GameEntry> {
    val root = DocumentFile.fromTreeUri(this, folderUri) ?: return emptyList()
    val stack = ArrayDeque<Pair<DocumentFile, String>>()
    stack.add(root to "")

    val games = ArrayList<GameEntry>()
    while (stack.isNotEmpty()) {
      val (node, prefix) = stack.removeLast()
      val files = try {
        node.listFiles()
      } catch (_: Exception) {
        emptyArray()
      }
      for (child in files) {
        val name = child.name ?: continue
        if (child.isDirectory) {
          stack.add(child to (prefix + name + "/"))
          continue
        }
        if (!child.isFile || !isSupportedGame(name)) {
          continue
        }
        games.add(
          GameEntry(
            title = toGameTitle(name),
            uri = child.uri,
            relativePath = prefix + name,
            sizeBytes = child.length()
          )
        )
      }
    }

    games.sortBy { it.title.lowercase(Locale.ROOT) }
    return games
  }

  private fun isSupportedGame(name: String): Boolean {
    val lower = name.lowercase(Locale.ROOT)
    if (lower.endsWith(".xiso.iso")) {
      return true
    }
    val ext = lower.substringAfterLast('.', "")
    return ext.isNotEmpty() && gameExts.contains(ext)
  }

  private fun toGameTitle(fileName: String): String {
    val lower = fileName.lowercase(Locale.ROOT)
    return when {
      lower.endsWith(".xiso.iso") -> fileName.dropLast(".xiso.iso".length)
      fileName.contains('.') -> fileName.substringBeforeLast('.')
      else -> fileName
    }
  }

  private fun formatSize(bytes: Long): String {
    if (bytes <= 0L) {
      return "Unknown"
    }
    val units = arrayOf("B", "KB", "MB", "GB", "TB")
    var value = bytes.toDouble()
    var unitIndex = 0
    while (value >= 1024.0 && unitIndex < units.lastIndex) {
      value /= 1024.0
      unitIndex++
    }
    return String.format(Locale.US, "%.1f %s", value, units[unitIndex])
  }

  private fun isFolderReady(uri: Uri?): Boolean {
    if (uri == null || !hasPersistedReadPermission(uri)) {
      return false
    }
    val root = DocumentFile.fromTreeUri(this, uri) ?: return false
    return root.exists() && root.isDirectory
  }

  private fun persistUriPermission(uri: Uri) {
    val flags = Intent.FLAG_GRANT_READ_URI_PERMISSION
    try {
      contentResolver.takePersistableUriPermission(uri, flags)
    } catch (_: SecurityException) {
    }
  }

  private fun hasPersistedReadPermission(uri: Uri): Boolean {
    return contentResolver.persistedUriPermissions.any { perm ->
      perm.uri == uri && perm.isReadPermission
    }
  }

  private fun formatTreeLabel(uri: Uri): String {
    val name = DocumentFile.fromTreeUri(this, uri)?.name
    if (!name.isNullOrBlank()) {
      return name
    }
    return uri.toString()
  }

  private fun dp(value: Int): Int {
    return (value * resources.displayMetrics.density).toInt()
  }

  private fun showAboutDialog() {
    val links = listOf(
      getString(R.string.library_about_link_source) to getString(R.string.library_about_url_source),
      getString(R.string.library_about_link_privacy) to getString(R.string.library_about_url_privacy),
      getString(R.string.library_about_link_fork) to getString(R.string.library_about_url_fork),
      getString(R.string.library_about_link_license) to getString(R.string.library_about_url_license),
      getString(R.string.library_about_link_disclaimer) to getString(R.string.library_about_url_disclaimer)
    )

    val message = SpannableStringBuilder().apply {
      append(getString(R.string.library_about_message))
      append("\n\n")
      links.forEachIndexed { index, pair ->
        val label = pair.first
        val url = pair.second
        append(label)
        append('\n')
        val start = length
        append(url)
        setSpan(
          object : ClickableSpan() {
            override fun onClick(widget: View) {
              openExternalLink(url)
            }
          },
          start,
          length,
          Spanned.SPAN_EXCLUSIVE_EXCLUSIVE
        )
        if (index != links.lastIndex) {
          append("\n\n")
        }
      }
    }

    val dialog = MaterialAlertDialogBuilder(this)
      .setTitle(R.string.library_about_title)
      .setMessage(message)
      .setPositiveButton(android.R.string.ok, null)
      .show()

    dialog.findViewById<TextView>(android.R.id.message)?.apply {
      movementMethod = LinkMovementMethod.getInstance()
      linksClickable = true
    }
  }

  private fun openExternalLink(url: String) {
    val intent = Intent(Intent.ACTION_VIEW, Uri.parse(url)).apply {
      addCategory(Intent.CATEGORY_BROWSABLE)
    }
    try {
      startActivity(intent)
    } catch (_: Exception) {
      Toast.makeText(this, getString(R.string.library_about_open_failed), Toast.LENGTH_SHORT).show()
    }
  }
}

