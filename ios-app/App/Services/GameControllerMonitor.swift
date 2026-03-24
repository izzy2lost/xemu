import Combine
import Foundation
import GameController

@MainActor
final class GameControllerMonitor: ObservableObject {
  @Published private(set) var hasConnectedController = false

  init() {
    refresh()
    NotificationCenter.default.addObserver(
      forName: .GCControllerDidConnect,
      object: nil,
      queue: .main
    ) { [weak self] _ in
      self?.refresh()
    }
    NotificationCenter.default.addObserver(
      forName: .GCControllerDidDisconnect,
      object: nil,
      queue: .main
    ) { [weak self] _ in
      self?.refresh()
    }
  }

  func refresh() {
    hasConnectedController = !GCController.controllers().isEmpty
  }
}
