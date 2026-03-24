import Combine
import Foundation

@MainActor
final class SettingsStore: ObservableObject {
  private let defaults: UserDefaults
  private let key = "ios.settings.v1"

  @Published private(set) var settings: EmulatorSettings

  init(defaults: UserDefaults = .standard) {
    self.defaults = defaults
    if let data = defaults.data(forKey: key),
       let decoded = try? JSONDecoder().decode(EmulatorSettings.self, from: data) {
      self.settings = Self.normalized(decoded)
    } else {
      self.settings = Self.normalized(EmulatorSettings())
    }
  }

  func update(_ newSettings: EmulatorSettings) {
    settings = Self.normalized(newSettings)
    save()
  }

  func save() {
    if let data = try? JSONEncoder().encode(settings) {
      defaults.set(data, forKey: key)
    }
  }

  private static func normalized(_ settings: EmulatorSettings) -> EmulatorSettings {
    var normalized = settings

    if normalized.audioDriver == "openslES" || normalized.audioDriver == "aaudio" {
      normalized.audioDriver = "coreaudio"
    }

    if normalized.inputPorts.count < 4 {
      normalized.inputPorts += Array(repeating: InputPortSettings(), count: 4 - normalized.inputPorts.count)
    } else if normalized.inputPorts.count > 4 {
      normalized.inputPorts = Array(normalized.inputPorts.prefix(4))
    }

    return normalized
  }
}
