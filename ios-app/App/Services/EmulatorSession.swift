import Combine
import Foundation
import UIKit
import X1BoxNativeCore

@MainActor
final class EmulatorSession: ObservableObject {
  struct SnapshotSlot: Identifiable, Codable, Equatable {
    let slotNumber: Int
    var title: String
    var detail: String
    var createdAt: Date?
    var launchKind: String?
    var relativePath: String?
    var storedConfigPath: String?
    var nativeSnapshotName: String?

    var id: Int { slotNumber }
    var isOccupied: Bool { launchKind != nil }

    static func emptySlots(count: Int = 4) -> [SnapshotSlot] {
      (1...count).map { slotNumber in
        SnapshotSlot(
          slotNumber: slotNumber,
          title: "Empty Slot",
          detail: "No saved shell snapshot yet.",
          createdAt: nil,
          launchKind: nil,
          relativePath: nil,
          storedConfigPath: nil,
          nativeSnapshotName: nil
        )
      }
    }
  }

  enum State: Equatable {
    case idle
    case preparing
    case running
    case failed(String)
  }

  private enum LaunchTarget: Equatable {
    case dashboard
    case game(title: String, relativePath: String)
  }

  @Published private(set) var state: State = .idle
  @Published private(set) var currentGame: GameEntry?
  @Published private(set) var lastPreparedConfigURL: URL?
  @Published private(set) var launchWarning: String?
  @Published private(set) var snapshotSlots: [SnapshotSlot] = SnapshotSlot.emptySlots()
  @Published private(set) var snapshotActionMessage: String?
  @Published private(set) var snapshotActionError: String?

  private let bridge = X1BoxNativeBridge.shared()
  private let configWriter = XemuConfigWriter()
  private var currentLaunchTarget: LaunchTarget?

  init() {
    reloadSnapshotSlots()
  }

  func makeViewController() -> UIViewController {
    bridge.makeEmulatorViewController()
  }

  func launchDashboard(setup: SetupSummary, settings: EmulatorSettings) async {
    await prepareAndStart(setup: setup, settings: settings, dvdURL: nil, game: nil)
  }

  func launch(game: GameEntry, setup: SetupSummary, settings: EmulatorSettings) async {
    do {
      let stagedDVD = try AppPaths.stagedDVDURL()
      if FileManager.default.fileExists(atPath: stagedDVD.path) {
        try FileManager.default.removeItem(at: stagedDVD)
      }
      try FileManager.default.copyItem(at: game.url, to: stagedDVD)
      await prepareAndStart(setup: setup, settings: settings, dvdURL: stagedDVD, game: game)
    } catch {
      state = .failed(error.localizedDescription)
    }
  }

  func stop() {
    bridge.stopSession()
    currentGame = nil
    currentLaunchTarget = nil
    launchWarning = nil
    state = .idle
  }

  func sendButton(_ name: String, pressed: Bool) {
    bridge.updateVirtualButton(name, pressed: pressed)
  }

  func sendAxis(_ name: String, x: Float, y: Float) {
    bridge.updateVirtualAxis(name, x: x, y: y)
  }

  func reloadSnapshotSlots() {
    do {
      let manifestURL = try AppPaths.snapshotManifestURL()
      guard FileManager.default.fileExists(atPath: manifestURL.path) else {
        snapshotSlots = SnapshotSlot.emptySlots()
        snapshotActionError = nil
        return
      }

      let data = try Data(contentsOf: manifestURL)
      let decoder = JSONDecoder()
      decoder.dateDecodingStrategy = .iso8601
      let decoded = try decoder.decode([SnapshotSlot].self, from: data)
      snapshotSlots = normalizedSnapshotSlots(from: decoded)
      snapshotActionError = nil
    } catch {
      snapshotSlots = SnapshotSlot.emptySlots()
      snapshotActionError = error.localizedDescription
    }
  }

  func saveSnapshot(to slotNumber: Int) throws {
    guard case .running = state else {
      throw NSError(domain: "X1Box.EmulatorSession", code: 20, userInfo: [
        NSLocalizedDescriptionKey: "Start the console before saving a snapshot slot."
      ])
    }

    guard let target = currentLaunchTarget else {
      throw NSError(domain: "X1Box.EmulatorSession", code: 21, userInfo: [
        NSLocalizedDescriptionKey: "There is no active launch target to save yet."
      ])
    }

    var slots = normalizedSnapshotSlots(from: snapshotSlots)
    guard let slotIndex = slots.firstIndex(where: { $0.slotNumber == slotNumber }) else {
      throw NSError(domain: "X1Box.EmulatorSession", code: 22, userInfo: [
        NSLocalizedDescriptionKey: "The selected snapshot slot does not exist."
      ])
    }

    let configCopyPath = try captureSnapshotConfig(slotNumber: slotNumber)
    var slot = slots[slotIndex]
    slot.createdAt = Date()
    slot.storedConfigPath = configCopyPath
    let nativeSnapshotName = "ios-slot-\(slotNumber)"

    switch target {
    case .dashboard:
      slot.title = "Dashboard"
      slot.detail = "Boots the console shell again with the saved profile."
      slot.launchKind = "dashboard"
      slot.relativePath = nil
    case .game(let title, let relativePath):
      slot.title = title
      slot.detail = "Relaunches \(title) from the last saved shell slot."
      slot.launchKind = "game"
      slot.relativePath = relativePath
    }

    if bridge.canUseNativeSnapshots() {
      var snapshotError: NSError?
      if bridge.saveNativeSnapshotNamed(nativeSnapshotName, error: &snapshotError) {
        slot.nativeSnapshotName = nativeSnapshotName
        slot.detail += " Native memory-state snapshot is available for this slot."
      } else if let snapshotError {
        throw snapshotError
      }
    } else {
      slot.nativeSnapshotName = nil
    }

    slots[slotIndex] = slot
    snapshotSlots = slots
    try persistSnapshotSlots()
    snapshotActionError = nil
    snapshotActionMessage = "Saved snapshot slot \(slotNumber). Full memory-state resume will plug into this slot when the native snapshot API is linked."
  }

  func deleteSnapshot(slotNumber: Int) throws {
    var slots = normalizedSnapshotSlots(from: snapshotSlots)
    guard let slotIndex = slots.firstIndex(where: { $0.slotNumber == slotNumber }) else {
      throw NSError(domain: "X1Box.EmulatorSession", code: 23, userInfo: [
        NSLocalizedDescriptionKey: "The selected snapshot slot does not exist."
      ])
    }

    let nativeSnapshotName = slots[slotIndex].nativeSnapshotName
    slots[slotIndex] = SnapshotSlot.emptySlots()[slotNumber - 1]
    snapshotSlots = slots

    if let nativeSnapshotName,
       bridge.canUseNativeSnapshots() {
      var snapshotError: NSError?
      if !bridge.deleteNativeSnapshotNamed(nativeSnapshotName, error: &snapshotError),
         let snapshotError {
        throw snapshotError
      }
    }

    let configURL = try AppPaths.snapshotConfigURL(slotNumber: slotNumber)
    if FileManager.default.fileExists(atPath: configURL.path) {
      try FileManager.default.removeItem(at: configURL)
    }

    try persistSnapshotSlots()
    snapshotActionError = nil
    snapshotActionMessage = "Removed snapshot slot \(slotNumber)."
  }

  private func prepareAndStart(
    setup: SetupSummary,
    settings: EmulatorSettings,
    dvdURL: URL?,
    game: GameEntry?
  ) async {
    state = .preparing
    launchWarning = nil
    snapshotActionMessage = nil
    snapshotActionError = nil
    do {
      let sessionSettings = normalizedSettings(from: settings)
      try applyEEPROMOverridesIfNeeded(setup: setup, settings: sessionSettings)
      let configURL = try configWriter.writeConfig(setup: setup, settings: sessionSettings, dvdURL: dvdURL)
      lastPreparedConfigURL = configURL
      currentGame = game

      var bridgeError: NSError?
      if bridge.startSession(withConfigPath: configURL.path, error: &bridgeError) {
        state = .running
        currentLaunchTarget = launchTarget(for: game)
      } else {
        let message = bridgeError?.localizedDescription ?? "The native bridge could not start the session."
        state = .failed(message)
      }
    } catch {
      state = .failed(error.localizedDescription)
    }
  }

  private func normalizedSettings(from settings: EmulatorSettings) -> EmulatorSettings {
    var normalized = settings
    if normalized.netEnable && normalized.netBackend == "bridge" {
      normalized.netBackend = "nat"
      launchWarning = "Bridge networking is not available in the iOS shell yet. This session is using NAT instead."
    }
    return normalized
  }

  private func launchTarget(for game: GameEntry?) -> LaunchTarget {
    if let game {
      return .game(title: game.title, relativePath: game.relativePath)
    }
    return .dashboard
  }

  private func normalizedSnapshotSlots(from slots: [SnapshotSlot]) -> [SnapshotSlot] {
    var normalized = SnapshotSlot.emptySlots()
    for slot in slots {
      guard (1...normalized.count).contains(slot.slotNumber) else {
        continue
      }
      normalized[slot.slotNumber - 1] = slot
    }
    return normalized
  }

  private func captureSnapshotConfig(slotNumber: Int) throws -> String? {
    guard let configURL = lastPreparedConfigURL,
          FileManager.default.fileExists(atPath: configURL.path) else {
      return nil
    }

    let destination = try AppPaths.snapshotConfigURL(slotNumber: slotNumber)
    if FileManager.default.fileExists(atPath: destination.path) {
      try FileManager.default.removeItem(at: destination)
    }
    try FileManager.default.copyItem(at: configURL, to: destination)
    return destination.path
  }

  private func persistSnapshotSlots() throws {
    let manifestURL = try AppPaths.snapshotManifestURL()
    let encoder = JSONEncoder()
    encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
    encoder.dateEncodingStrategy = .iso8601
    let data = try encoder.encode(snapshotSlots)
    try data.write(to: manifestURL, options: .atomic)
  }

  func restoreNativeSnapshotIfAvailable(for slot: SnapshotSlot) throws -> Bool {
    guard let nativeSnapshotName = slot.nativeSnapshotName else {
      return false
    }

    guard bridge.canUseNativeSnapshots() else {
      return false
    }

    var snapshotError: NSError?
    if bridge.loadNativeSnapshotNamed(nativeSnapshotName, error: &snapshotError) {
      snapshotActionMessage = "Loaded native snapshot for slot \(slot.slotNumber)."
      snapshotActionError = nil
      return true
    }

    if let snapshotError {
      throw snapshotError
    }

    return false
  }

  private func applyEEPROMOverridesIfNeeded(setup: SetupSummary, settings: EmulatorSettings) throws {
    guard let eepromPath = setup.record(for: .eeprom)?.localPath else {
      return
    }

    let language = XboxEepromEditor.Language(rawValue: settings.eeprom.language) ?? .english
    let videoStandard = XboxEepromEditor.VideoStandard(rawValue: settings.eeprom.videoStandard) ?? .ntscM
    _ = try XboxEepromEditor.apply(
      to: URL(fileURLWithPath: eepromPath),
      language: language,
      videoStandard: videoStandard
    )
  }
}
