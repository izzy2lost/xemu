package com.izzy2lost.x1box.hakux

import android.content.Context
import android.util.Log
import org.json.JSONArray
import java.io.File
import java.net.HttpURLConnection
import java.net.URL

/**
 * Insignia's DNS zones (insignia.live/dns_zones.json, as insigniaDNS loads them): the Xbox
 * Live names hakuX's network answers itself (x1box_nat.c), each with the host it stands for.
 * Fetched over HTTPS, which a network that rewrites DNS can't change, for the next start;
 * until a fetch has worked, the list known when this was written.
 */
object InsigniaZones {
  private const val TAG = "x1box-hakux"
  private const val URL_ZONES = "https://insignia.live/dns_zones.json"
  private val KNOWN = listOf(
    "macs.xboxlive.com" to "macs.insig.uk",
    "as.xboxlive.com" to "as.insig.uk",
    "tgs.xboxlive.com" to "tgs.insig.uk",
    "xds.xboxlive.com" to "xexds.xboxlive.com",
    "insignia.live" to "insignia.live",
  )

  /**
   * "name=host" lines at once: the last list fetched, or the one known. A fetch starts in
   * the background and keeps its list for the next start.
   */
  fun lines(context: Context): String {
    val cache = File(context.filesDir, "hakux/insignia_zones.txt")
    Thread {
      runCatching { fetch() }
        .onSuccess { list ->
          if (list.isNotEmpty()) {
            cache.parentFile?.mkdirs()
            cache.writeText(list.joinToString("") { (n, h) -> "$n=$h\n" })
            Log.i(TAG, "Insignia zones: ${list.size} fetched")
          }
        }
        .onFailure { Log.w(TAG, "Insignia zones: ${it.message}") }
    }.start()
    return cache.takeIf { it.isFile }?.readText()?.takeIf { it.isNotBlank() }
      ?: KNOWN.joinToString("") { (n, h) -> "$n=$h\n" }
  }

  private fun fetch(): List<Pair<String, String>> {
    val c = URL(URL_ZONES).openConnection() as HttpURLConnection
    c.connectTimeout = 3000
    c.readTimeout = 3000
    c.setRequestProperty("User-Agent", "x1box")
    try {
      val zones = JSONArray(c.inputStream.bufferedReader().readText())
      return (0 until zones.length()).mapNotNull { i ->
        val z = zones.getJSONObject(i)
        val name = z.optString("name")
        val value = z.optString("value")
        // "a": an address itself; "p": a host to look up (both resolve as a host name)
        if (name.isEmpty() || value.isEmpty() || z.optString("type") !in setOf("a", "p")) null
        else name to value
      }
    } finally {
      c.disconnect()
    }
  }
}
