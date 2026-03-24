import Foundation

struct XemuConfigWriter {
  func writeConfig(
    setup: SetupSummary,
    settings: EmulatorSettings,
    dvdURL: URL?
  ) throws -> URL {
    let mcpx = setup.record(for: .mcpx)?.localPath ?? ""
    let flash = setup.record(for: .flash)?.localPath ?? ""
    let hdd = setup.record(for: .hdd)?.localPath ?? ""
    let eeprom = setup.record(for: .eeprom)?.localPath ?? ""
    let netBackend = effectiveNetworkBackend(from: settings)

    let inputPorts = settings.inputPorts.enumerated().map { index, port in
      """
      [input.peripherals.port\(index + 1)]
      peripheral_type_0 = "\(port.slotA)"
      peripheral_param_0 = ""
      peripheral_type_1 = "\(port.slotB)"
      peripheral_param_1 = ""
      """
    }.joined(separator: "\n\n")

    let dvdBlock: String
    if let dvdURL {
      dvdBlock = "dvd_path = \"\(dvdURL.path)\""
    } else {
      dvdBlock = ""
    }

    let toml = """
    [general]
    show_welcome = false
    skip_boot_anim = \(settings.skipBootAnimation ? "true" : "false")

    [display]
    renderer = "\(settings.renderer)"
    filtering = "\(settings.filtering)"

    [display.quality]
    surface_scale = \(settings.surfaceScale)

    [display.window]
    vsync = \(settings.vsync ? "true" : "false")

    [audio]
    use_dsp = \(settings.useDSP ? "true" : "false")
    hrtf = \(settings.hrtf ? "true" : "false")

    [audio.vp]
    num_workers = 0

    [perf]
    cache_shaders = \(settings.cacheShaders ? "true" : "false")
    hard_fpu = \(settings.hardFPU ? "true" : "false")

    [sys]
    mem_limit = "\(settings.systemMemoryMiB)"

    [sys.files]
    bootrom_path = "\(mcpx)"
    flashrom_path = "\(flash)"
    hdd_path = "\(hdd)"
    eeprom_path = "\(eeprom)"
    \(dvdBlock)

    [net]
    enable = \(settings.netEnable ? "true" : "false")
    backend = "\(netBackend)"

    [net.udp]
    bind_addr = "\(settings.netUdpBind)"
    remote_addr = "\(settings.netUdpRemote)"

    [net.pcap]
    netif = "\(settings.netInterface)"

    [net.nat]
    forward_ports = []

    [input]
    auto_bind = \((settings.inputEnableControllers && settings.inputAutoBind) ? "true" : "false")
    allow_vibration = \(settings.inputAllowVibration ? "true" : "false")
    background_input_capture = \(settings.inputBackgroundCapture ? "true" : "false")

    [input.bindings]
    port1_driver = "\(settings.inputPorts[0].driver)"
    port1 = "\(settings.inputPorts[0].bindingMode)"
    port2_driver = "\(settings.inputPorts[1].driver)"
    port2 = "\(settings.inputPorts[1].bindingMode)"
    port3_driver = "\(settings.inputPorts[2].driver)"
    port3 = "\(settings.inputPorts[2].bindingMode)"
    port4_driver = "\(settings.inputPorts[3].driver)"
    port4 = "\(settings.inputPorts[3].bindingMode)"

    \(inputPorts)
    """

    let configURL = try AppPaths.configURL()
    try toml.write(to: configURL, atomically: true, encoding: .utf8)
    return configURL
  }

  private func effectiveNetworkBackend(from settings: EmulatorSettings) -> String {
    if settings.netEnable && settings.netBackend == "bridge" {
      return "nat"
    }
    return settings.netBackend
  }
}
