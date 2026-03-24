import XCTest
@testable import X1BoxiOS

@MainActor
final class SetupAssetStoreTests: XCTestCase {
  func testGamesFolderBookmarkMetadataPersists() throws {
    let suiteName = "SetupAssetStoreTests.\(UUID().uuidString)"
    let defaults = UserDefaults(suiteName: suiteName)!
    defaults.removePersistentDomain(forName: suiteName)
    defer {
      defaults.removePersistentDomain(forName: suiteName)
    }

    let folder = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString, isDirectory: true)
    try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
    defer {
      try? FileManager.default.removeItem(at: folder)
    }

    let store = SetupAssetStore(defaults: defaults)
    try store.importSelection(from: folder, kind: .gamesFolder)

    let record = store.summary.record(for: .gamesFolder)
    XCTAssertEqual(record?.displayName, folder.lastPathComponent)
    XCTAssertEqual(record?.bookmarkKey, "ios.setup.bookmark.gamesFolder")
    XCTAssertNotNil(defaults.data(forKey: "ios.setup.bookmark.gamesFolder"))
  }
}
