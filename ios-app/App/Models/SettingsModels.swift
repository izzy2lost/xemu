import Foundation

struct TouchOverlaySettings: Codable, Equatable {
  var showOverlay: Bool = true
  var opacity: Double = 0.7
  var scale: Double = 1.0
  var offsetX: Double = 0.0
  var offsetY: Double = 0.0
}

struct InputPortSettings: Codable, Equatable {
  var driver: String = "usb-xbox-gamepad"
  var bindingMode: String = "auto"
  var bindingGUID: String = ""
  var slotA: String = "none"
  var slotB: String = "none"
}

struct EEPROMSettings: Codable, Equatable {
  var language: String = "english"
  var videoStandard: String = "ntsc-m"
}

struct EmulatorSettings: Codable, Equatable {
  var renderer: String = "opengl"
  var filtering: String = "linear"
  var vsync: Bool = true
  var surfaceScale: Int = 1
  var displayMode: Int = 0
  var frameRateLimit: Int = 60
  var systemMemoryMiB: Int = 64
  var tcgThread: String = "multi"
  var useDSP: Bool = false
  var hrtf: Bool = true
  var cacheShaders: Bool = true
  var hardFPU: Bool = true
  var skipBootAnimation: Bool = false
  var audioDriver: String = "coreaudio"
  var netEnable: Bool = true
  var netBackend: String = "nat"
  var netInterface: String = ""
  var netUdpBind: String = "0.0.0.0:9368"
  var netUdpRemote: String = "1.2.3.4:9368"
  var inputEnableControllers: Bool = true
  var inputAutoBind: Bool = true
  var inputAllowVibration: Bool = true
  var inputBackgroundCapture: Bool = false
  var inputPorts: [InputPortSettings] = Array(repeating: InputPortSettings(), count: 4)
  var touchOverlay: TouchOverlaySettings = TouchOverlaySettings()
  var eeprom: EEPROMSettings = EEPROMSettings()
  var appLanguage: String = "system"
  var boxArtLookup: Bool = true
}

enum EmulatorDisplayMode: Int, CaseIterable, Identifiable {
  case stretch = 0
  case fourThree = 1
  case sixteenNine = 2

  var id: Int { rawValue }

  var title: String {
    switch self {
    case .stretch:
      return "Stretch"
    case .fourThree:
      return "4:3"
    case .sixteenNine:
      return "16:9"
    }
  }
}

enum InputDriverOption: String, CaseIterable, Identifiable {
  case usbXboxGamepad = "usb-xbox-gamepad"
  case auto
  case none

  var id: String { rawValue }

  var title: String {
    switch self {
    case .usbXboxGamepad:
      return "Xbox Gamepad"
    case .auto:
      return "Auto"
    case .none:
      return "Disabled"
    }
  }
}

enum InputBindingMode: String, CaseIterable, Identifiable {
  case auto
  case manual
  case touch

  var id: String { rawValue }

  var title: String {
    switch self {
    case .auto:
      return "Auto"
    case .manual:
      return "Manual"
    case .touch:
      return "Touch Overlay"
    }
  }
}

enum InputExpansionOption: String, CaseIterable, Identifiable {
  case none
  case memoryUnit = "memory-unit"
  case controllerPack = "controller-pack"
  case headset

  var id: String { rawValue }

  var title: String {
    switch self {
    case .none:
      return "None"
    case .memoryUnit:
      return "Memory Unit"
    case .controllerPack:
      return "Controller Pack"
    case .headset:
      return "Headset"
    }
  }
}
