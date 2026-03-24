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

  func testEmbeddedCoreDylibStagesIntoApplicationSupport() throws {
    let suiteName = "SetupAssetStoreEmbeddedCoreTests.\(UUID().uuidString)"
    let defaults = UserDefaults(suiteName: suiteName)!
    defaults.removePersistentDomain(forName: suiteName)
    defer {
      defaults.removePersistentDomain(forName: suiteName)
    }

    let sourceRoot = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString, isDirectory: true)
    let sourceURL = sourceRoot.appendingPathComponent("libxemu-ios-core.dylib")
    try FileManager.default.createDirectory(at: sourceRoot, withIntermediateDirectories: true)
    FileManager.default.createFile(atPath: sourceURL.path, contents: Data([0xCA, 0xFE, 0xBA, 0xBE]))
    defer {
      try? FileManager.default.removeItem(at: sourceRoot)
    }

    let store = SetupAssetStore(defaults: defaults)
    try store.importEmbeddedCoreArtifact(from: sourceURL)
    defer {
      try? store.removeSelection(for: .embeddedCore)
    }

    let record = store.summary.record(for: .embeddedCore)
    XCTAssertEqual(record?.displayName, "libxemu-ios-core.dylib")
    XCTAssertEqual(record?.localPath, try AppPaths.embeddedCoreDylibURL().path)
    XCTAssertTrue(FileManager.default.fileExists(atPath: try AppPaths.embeddedCoreDylibURL().path))
  }

  func testEmbeddedCoreXCFrameworkStagesIntoApplicationSupport() throws {
    let suiteName = "SetupAssetStoreEmbeddedCoreXCFrameworkTests.\(UUID().uuidString)"
    let defaults = UserDefaults(suiteName: suiteName)!
    defaults.removePersistentDomain(forName: suiteName)
    defer {
      defaults.removePersistentDomain(forName: suiteName)
    }

    let sourceRoot = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString, isDirectory: true)
    let xcframeworkURL = sourceRoot.appendingPathComponent("X1BoxEmbeddedCore.xcframework", isDirectory: true)
    let plistURL = xcframeworkURL.appendingPathComponent("Info.plist")
    try FileManager.default.createDirectory(at: xcframeworkURL, withIntermediateDirectories: true)
    try Data("test".utf8).write(to: plistURL)
    defer {
      try? FileManager.default.removeItem(at: sourceRoot)
    }

    let store = SetupAssetStore(defaults: defaults)
    try store.importEmbeddedCoreArtifact(from: xcframeworkURL)
    defer {
      try? store.removeSelection(for: .embeddedCore)
    }

    let record = store.summary.record(for: .embeddedCore)
    XCTAssertEqual(record?.displayName, "X1BoxEmbeddedCore.xcframework")
    XCTAssertEqual(record?.localPath, try AppPaths.embeddedCoreXCFrameworkURL().path)
    XCTAssertTrue(FileManager.default.fileExists(atPath: try AppPaths.embeddedCoreXCFrameworkURL().path))
  }
}
