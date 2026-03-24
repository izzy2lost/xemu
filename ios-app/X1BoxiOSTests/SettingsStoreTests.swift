import XCTest
@testable import X1BoxiOS

@MainActor
final class SettingsStoreTests: XCTestCase {
  func testSettingsRoundTripPersistsOverlayAndLanguage() {
    let suiteName = "SettingsStoreTests.\(UUID().uuidString)"
    let defaults = UserDefaults(suiteName: suiteName)!
    defaults.removePersistentDomain(forName: suiteName)
    defer {
      defaults.removePersistentDomain(forName: suiteName)
    }

    let store = SettingsStore(defaults: defaults)
    var updated = store.settings
    updated.netBackend = "udp"
    updated.touchOverlay.scale = 1.24
    updated.touchOverlay.offsetX = 0.1
    updated.appLanguage = "es"

    store.update(updated)

    let reloaded = SettingsStore(defaults: defaults)
    XCTAssertEqual(reloaded.settings.netBackend, "udp")
    XCTAssertEqual(reloaded.settings.touchOverlay.scale, 1.24, accuracy: 0.001)
    XCTAssertEqual(reloaded.settings.touchOverlay.offsetX, 0.1, accuracy: 0.001)
    XCTAssertEqual(reloaded.settings.appLanguage, "es")
  }

  func testSettingsStoreNormalizesLegacyAudioDriverAndPortCount() throws {
    let suiteName = "SettingsStoreLegacyTests.\(UUID().uuidString)"
    let defaults = UserDefaults(suiteName: suiteName)!
    defaults.removePersistentDomain(forName: suiteName)
    defer {
      defaults.removePersistentDomain(forName: suiteName)
    }

    var legacy = EmulatorSettings()
    legacy.audioDriver = "openslES"
    legacy.inputPorts = [InputPortSettings()]
    let data = try JSONEncoder().encode(legacy)
    defaults.set(data, forKey: "ios.settings.v1")

    let store = SettingsStore(defaults: defaults)

    XCTAssertEqual(store.settings.audioDriver, "coreaudio")
    XCTAssertEqual(store.settings.inputPorts.count, 4)
  }
}
