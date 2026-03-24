import Combine
import Foundation

@MainActor
final class AppModel: ObservableObject {
  enum Route {
    case launcher
    case setup
    case library
    case emulator
  }

  @Published var route: Route = .launcher
  @Published var games: [GameEntry] = []
  @Published var isShowingSettings = false
  @Published var scanErrorMessage: String?
  @Published var emulatorErrorMessage: String?
  @Published var emulatorNoticeMessage: String?

  let setupStore: SetupAssetStore
  let settingsStore: SettingsStore
  let controllerMonitor: GameControllerMonitor
  let emulatorSession: EmulatorSession

  private let scanner = LibraryScanner()
  private var cancellables = Set<AnyCancellable>()

  init(
    setupStore: SetupAssetStore = SetupAssetStore(),
    settingsStore: SettingsStore = SettingsStore(),
    controllerMonitor: GameControllerMonitor = GameControllerMonitor(),
    emulatorSession: EmulatorSession = EmulatorSession()
  ) {
    self.setupStore = setupStore
    self.settingsStore = settingsStore
    self.controllerMonitor = controllerMonitor
    self.emulatorSession = emulatorSession
    bindChildObjects()
    refreshRoute()
  }

  func refreshRoute() {
    route = setupStore.summary.isCoreReady ? .library : .setup
  }

  func reloadLibrary() async {
    emulatorSession.reloadSnapshotSlots()
    do {
      let games = try setupStore.withGamesFolderURL { folderURL in
        try scanner.scanGames(in: folderURL)
      }
      self.games = games
      self.scanErrorMessage = nil
    } catch {
      self.games = []
      self.scanErrorMessage = error.localizedDescription
    }
  }

  func startDashboard() async {
    emulatorErrorMessage = nil
    emulatorNoticeMessage = nil
    await emulatorSession.launchDashboard(setup: setupStore.summary, settings: settingsStore.settings)
    syncLaunchOutcome()
  }

  func start(game: GameEntry) async {
    emulatorErrorMessage = nil
    emulatorNoticeMessage = nil
    await emulatorSession.launch(game: game, setup: setupStore.summary, settings: settingsStore.settings)
    syncLaunchOutcome()
  }

  func stopEmulation() {
    emulatorSession.stop()
    emulatorNoticeMessage = nil
    route = .library
  }

  func saveSnapshot(to slotNumber: Int) {
    do {
      emulatorErrorMessage = nil
      try emulatorSession.saveSnapshot(to: slotNumber)
      emulatorNoticeMessage = emulatorSession.snapshotActionMessage
    } catch {
      emulatorErrorMessage = error.localizedDescription
    }
  }

  func deleteSnapshot(_ slot: EmulatorSession.SnapshotSlot) {
    do {
      emulatorErrorMessage = nil
      try emulatorSession.deleteSnapshot(slotNumber: slot.slotNumber)
      emulatorNoticeMessage = emulatorSession.snapshotActionMessage
    } catch {
      emulatorErrorMessage = error.localizedDescription
    }
  }

  func resumeSnapshot(_ slot: EmulatorSession.SnapshotSlot) async {
    emulatorErrorMessage = nil
    emulatorNoticeMessage = nil

    switch slot.launchKind {
    case "dashboard":
      await emulatorSession.launchDashboard(setup: setupStore.summary, settings: settingsStore.settings)
    case "game":
      guard let relativePath = slot.relativePath,
            let game = games.first(where: { $0.relativePath == relativePath }) else {
        emulatorErrorMessage = "The original game file for snapshot slot \(slot.slotNumber) could not be found in the current library."
        return
      }
      await emulatorSession.launch(game: game, setup: setupStore.summary, settings: settingsStore.settings)
    default:
      emulatorErrorMessage = "Snapshot slot \(slot.slotNumber) is empty."
      return
    }

    syncLaunchOutcome()
    if emulatorErrorMessage == nil {
      emulatorNoticeMessage = "Resumed slot \(slot.slotNumber) by restoring the saved boot target. Full memory-state resume will be connected when the native snapshot API is available."
    }
  }

  private func bindChildObjects() {
    setupStore.objectWillChange
      .sink { [weak self] _ in
        self?.objectWillChange.send()
      }
      .store(in: &cancellables)

    settingsStore.objectWillChange
      .sink { [weak self] _ in
        self?.objectWillChange.send()
      }
      .store(in: &cancellables)

    controllerMonitor.objectWillChange
      .sink { [weak self] _ in
        self?.objectWillChange.send()
      }
      .store(in: &cancellables)

    emulatorSession.objectWillChange
      .sink { [weak self] _ in
        self?.objectWillChange.send()
      }
      .store(in: &cancellables)
  }

  private func syncLaunchOutcome() {
    emulatorNoticeMessage = emulatorSession.launchWarning

    switch emulatorSession.state {
    case .running, .preparing:
      emulatorErrorMessage = nil
      route = .emulator
    case .failed(let message):
      emulatorErrorMessage = message
      route = .library
    case .idle:
      route = .library
    }
  }
}
