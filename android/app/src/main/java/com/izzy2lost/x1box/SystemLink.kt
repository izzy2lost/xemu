package com.izzy2lost.x1box

import android.content.Context
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.Inet4Address
import java.net.InetAddress
import java.net.NetworkInterface
import java.net.SocketTimeoutException

/**
 * System Link (LAN play) settings and helpers. The relay itself is native
 * (xemu_lan.cpp); this side picks the mode, finds hosts and words the relay's
 * events for the player.
 */
internal object SystemLink {
  const val PREF_MODE = "setting_lan_mode"
  const val PREF_HOST = "setting_lan_host"

  const val MODE_OFF = "off"
  /** Join a host the relay finds on the LAN, else host (xemu_lan.cpp ResolveAuto). */
  const val MODE_AUTO = "auto"
  const val MODE_HOST = "host"
  const val MODE_JOIN = "join"

  // Must match xemu_lan.h.
  const val PORT = 9368
  private const val DISCOVERY_PORT = 9369
  private const val DISCOVER_REQUEST = "X1LAN-DISCOVER"
  private const val DISCOVER_REPLY_PREFIX = "X1LAN-HOST\n"

  // xemu_lan::Event values.
  private const val EVENT_START_FAILED = 1
  private const val EVENT_HOST_UNRESOLVED = 2
  private const val EVENT_CONNECTED = 3
  private const val EVENT_CONNECTION_LOST = 4
  private const val EVENT_PEER_JOINED = 5
  private const val EVENT_PEER_LEFT = 6
  private const val EVENT_DUPLICATE_MAC = 7
  private const val EVENT_HOSTING = 8

  data class Host(
    val address: String,
    val port: Int,
    val name: String,
    val game: String,
    val consoles: Int,
  ) {
    /** What goes in the "Host address" field. */
    val joinAddress: String
      get() = if (port == PORT) address else "$address:$port"
  }

  /** IPv4 addresses other devices on the LAN can reach this one at. */
  fun localAddresses(): List<String> {
    val interfaces = runCatching { NetworkInterface.getNetworkInterfaces()?.toList() }
      .getOrNull().orEmpty()
    return interfaces
      .filter { runCatching { it.isUp && !it.isLoopback }.getOrDefault(false) }
      // Mobile data is never on the same LAN as another player.
      .filterNot { it.name.startsWith("rmnet") || it.name.startsWith("ccmni") }
      .flatMap { it.inetAddresses.toList() }
      .filterIsInstance<Inet4Address>()
      .filterNot { it.isLoopbackAddress || it.isLinkLocalAddress }
      .mapNotNull { it.hostAddress }
      .distinct()
  }

  private fun broadcastAddresses(): List<InetAddress> {
    val directed = runCatching {
      NetworkInterface.getNetworkInterfaces()?.toList().orEmpty()
        .filter { it.isUp && !it.isLoopback }
        .flatMap { it.interfaceAddresses }
        .mapNotNull { it.broadcast }
    }.getOrDefault(emptyList())
    // Directed broadcasts reach hotspot clients that the limited broadcast
    // address does not on some devices.
    return (directed + InetAddress.getByName("255.255.255.255")).distinct()
  }

  /**
   * Broadcasts a discovery probe and collects the hosts that answer. Blocks
   * for about [timeoutMs]; call it off the main thread.
   */
  fun discover(timeoutMs: Int = 1500): List<Host> {
    val hosts = LinkedHashMap<String, Host>()
    DatagramSocket().use { socket ->
      socket.broadcast = true
      val request = DISCOVER_REQUEST.toByteArray()
      for (target in broadcastAddresses()) {
        runCatching {
          socket.send(DatagramPacket(request, request.size, target, DISCOVERY_PORT))
        }
      }
      val deadline = System.nanoTime() + timeoutMs * 1_000_000L
      val buffer = ByteArray(1024)
      while (true) {
        val remainingMs = ((deadline - System.nanoTime()) / 1_000_000L).toInt()
        if (remainingMs <= 0) break
        socket.soTimeout = remainingMs
        val packet = DatagramPacket(buffer, buffer.size)
        try {
          socket.receive(packet)
        } catch (_: SocketTimeoutException) {
          break
        }
        val host = parseReply(packet) ?: continue
        hosts[host.joinAddress] = host
      }
    }
    return hosts.values.toList()
  }

  private fun parseReply(packet: DatagramPacket): Host? {
    val text = String(packet.data, packet.offset, packet.length, Charsets.UTF_8)
    if (!text.startsWith(DISCOVER_REPLY_PREFIX)) return null
    val fields = text.removePrefix(DISCOVER_REPLY_PREFIX).split('\n')
    val port = fields.getOrNull(0)?.toIntOrNull() ?: return null
    val address = packet.address?.hostAddress ?: return null
    return Host(
      address = address,
      port = port,
      name = fields.getOrNull(1).orEmpty(),
      game = fields.getOrNull(2).orEmpty(),
      consoles = fields.getOrNull(3)?.toIntOrNull() ?: 1,
    )
  }

  /** Words a relay event from MainActivity.nativePollLanEvent for a toast. */
  fun eventMessage(context: Context, event: Int, arg: Int, hostAddress: String): String? =
    when (event) {
      EVENT_START_FAILED ->
        if (arg > 0) context.getString(R.string.lan_event_start_failed_port, arg)
        else context.getString(R.string.lan_event_start_failed)
      EVENT_HOST_UNRESOLVED -> context.getString(R.string.lan_event_host_unresolved, hostAddress)
      EVENT_CONNECTED -> context.getString(R.string.lan_event_connected)
      EVENT_CONNECTION_LOST -> context.getString(R.string.lan_event_connection_lost)
      // The relay counts remote consoles; players think in totals.
      EVENT_PEER_JOINED -> context.getString(R.string.lan_event_peer_joined, arg + 1)
      EVENT_PEER_LEFT -> context.getString(R.string.lan_event_peer_left, arg + 1)
      EVENT_DUPLICATE_MAC -> context.getString(R.string.lan_event_duplicate_mac)
      EVENT_HOSTING -> {
        val addresses = localAddresses()
        if (addresses.isEmpty()) context.getString(R.string.lan_event_hosting_no_ip)
        else context.getString(R.string.lan_event_hosting, addresses.joinToString(", "))
      }
      else -> null
    }
}
