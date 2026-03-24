import Combine
import Foundation
import UniformTypeIdentifiers

@MainActor
final class SetupAssetStore: ObservableObject {
  private let defaults: UserDefaults
  private let metadataKey = "ios.setup.asset.metadata.v1"
  private let bookmarkPrefix = "ios.setup.bookmark."

  @Published private(set) var summary: SetupSummary

  init(defaults: UserDefaults = .standard) {
    self.defaults = defaults
    if let data = defaults.data(forKey: metadataKey),
       let decoded = try? JSONDecoder().decode([SetupAssetKind: ImportedAssetRecord].self, from: data) {
      self.summary = SetupSummary(assets: decoded)
    } else {
      self.summary = SetupSummary(assets: [:])
    }
  }

  func importSelection(from sourceURL: URL, kind: SetupAssetKind) throws {
    switch kind {
    case .gamesFolder:
      try persistFolderBookmark(sourceURL, kind: kind)
    case .embeddedCore:
      try stageEmbeddedCoreArtifact(sourceURL)
    case .mcpx, .flash, .hdd, .eeprom:
      try stageLocalCopy(sourceURL, kind: kind)
    }
  }

  func importEmbeddedCoreArtifact(from sourceURL: URL) throws {
    try importSelection(from: sourceURL, kind: .embeddedCore)
  }

  func removeSelection(for kind: SetupAssetKind) throws {
    guard let record = summary.record(for: kind) else {
      return
    }

    if let bookmarkKey = record.bookmarkKey {
      defaults.removeObject(forKey: bookmarkKey)
    }

    if let localPath = record.localPath {
      let url = URL(fileURLWithPath: localPath)
      if FileManager.default.fileExists(atPath: url.path) {
        try FileManager.default.removeItem(at: url)
      }
    }

    summary.assets.removeValue(forKey: kind)
    saveMetadata()
  }

  func localURL(for kind: SetupAssetKind) -> URL? {
    guard let path = summary.record(for: kind)?.localPath else {
      return nil
    }
    let url = URL(fileURLWithPath: path)
    return FileManager.default.fileExists(atPath: url.path) ? url : nil
  }

  func withGamesFolderURL<T>(_ body: (URL) throws -> T) throws -> T {
    let bookmarkKey = bookmarkStorageKey(for: .gamesFolder)
    guard let bookmarkData = defaults.data(forKey: bookmarkKey) else {
      throw NSError(domain: "X1Box.SetupAssetStore", code: 1, userInfo: [
        NSLocalizedDescriptionKey: "Games folder bookmark is missing."
      ])
    }

    var stale = false
    let folderURL = try URL(
      resolvingBookmarkData: bookmarkData,
      options: [],
      relativeTo: nil,
      bookmarkDataIsStale: &stale
    )
    let started = folderURL.startAccessingSecurityScopedResource()
    defer {
      if started {
        folderURL.stopAccessingSecurityScopedResource()
      }
    }

    if stale {
      try persistFolderBookmark(folderURL, kind: .gamesFolder)
    }

    return try body(folderURL)
  }

  private func stageLocalCopy(_ sourceURL: URL, kind: SetupAssetKind) throws {
    let started = sourceURL.startAccessingSecurityScopedResource()
    defer {
      if started {
        sourceURL.stopAccessingSecurityScopedResource()
      }
    }

    let destinationURL = try AppPaths.stagedAssetURL(for: kind)
    try FileManager.default.createDirectory(
      at: destinationURL.deletingLastPathComponent(),
      withIntermediateDirectories: true
    )
    if FileManager.default.fileExists(atPath: destinationURL.path) {
      try FileManager.default.removeItem(at: destinationURL)
    }
    try FileManager.default.copyItem(at: sourceURL, to: destinationURL)

    let record = ImportedAssetRecord(
      kind: kind,
      displayName: sourceURL.lastPathComponent,
      localPath: destinationURL.path,
      bookmarkKey: nil
    )
    summary.assets[kind] = record
    saveMetadata()
  }

  private func stageEmbeddedCoreArtifact(_ sourceURL: URL) throws {
    let started = sourceURL.startAccessingSecurityScopedResource()
    defer {
      if started {
        sourceURL.stopAccessingSecurityScopedResource()
      }
    }

    let destinationURL = try embeddedCoreDestination(for: sourceURL)
    try clearExistingEmbeddedCoreArtifacts()

    try FileManager.default.createDirectory(
      at: destinationURL.deletingLastPathComponent(),
      withIntermediateDirectories: true
    )
    try FileManager.default.copyItem(at: sourceURL, to: destinationURL)

    let record = ImportedAssetRecord(
      kind: .embeddedCore,
      displayName: sourceURL.lastPathComponent,
      localPath: destinationURL.path,
      bookmarkKey: nil
    )
    summary.assets[.embeddedCore] = record
    saveMetadata()
  }

  private func persistFolderBookmark(_ sourceURL: URL, kind: SetupAssetKind) throws {
    let started = sourceURL.startAccessingSecurityScopedResource()
    defer {
      if started {
        sourceURL.stopAccessingSecurityScopedResource()
      }
    }

    let bookmarkData = try sourceURL.bookmarkData(
      options: [],
      includingResourceValuesForKeys: nil,
      relativeTo: nil
    )
    let key = bookmarkStorageKey(for: kind)
    defaults.set(bookmarkData, forKey: key)

    let record = ImportedAssetRecord(
      kind: kind,
      displayName: sourceURL.lastPathComponent.isEmpty ? sourceURL.path : sourceURL.lastPathComponent,
      localPath: nil,
      bookmarkKey: key
    )
    summary.assets[kind] = record
    saveMetadata()
  }

  private func bookmarkStorageKey(for kind: SetupAssetKind) -> String {
    bookmarkPrefix + kind.rawValue
  }

  private func clearExistingEmbeddedCoreArtifacts() throws {
    let fileManager = FileManager.default
    let frameworkURL = try AppPaths.embeddedCoreFrameworkURL()
    let dylibURL = try AppPaths.embeddedCoreDylibURL()

    if fileManager.fileExists(atPath: frameworkURL.path) {
      try fileManager.removeItem(at: frameworkURL)
    }

    if fileManager.fileExists(atPath: dylibURL.path) {
      try fileManager.removeItem(at: dylibURL)
    }
  }

  private func embeddedCoreDestination(for sourceURL: URL) throws -> URL {
    var isDirectory: ObjCBool = false
    let fileManager = FileManager.default
    guard fileManager.fileExists(atPath: sourceURL.path, isDirectory: &isDirectory) else {
      throw NSError(domain: "X1Box.SetupAssetStore", code: 2, userInfo: [
        NSLocalizedDescriptionKey: "The selected embedded core artifact no longer exists."
      ])
    }

    if isDirectory.boolValue || sourceURL.pathExtension.caseInsensitiveCompare("framework") == .orderedSame {
      let expectedName = "X1BoxEmbeddedCore.framework"
      guard sourceURL.lastPathComponent.caseInsensitiveCompare(expectedName) == .orderedSame else {
        throw NSError(domain: "X1Box.SetupAssetStore", code: 3, userInfo: [
          NSLocalizedDescriptionKey: "Select the full X1BoxEmbeddedCore.framework bundle."
        ])
      }
      return try AppPaths.embeddedCoreFrameworkURL()
    }

    let fileName = sourceURL.lastPathComponent.lowercased()
    if fileName == "libxemu-ios-core.dylib" || sourceURL.pathExtension.caseInsensitiveCompare("dylib") == .orderedSame {
      return try AppPaths.embeddedCoreDylibURL()
    }

    throw NSError(domain: "X1Box.SetupAssetStore", code: 4, userInfo: [
      NSLocalizedDescriptionKey: "Select X1BoxEmbeddedCore.framework or libxemu-ios-core.dylib."
    ])
  }

  private func saveMetadata() {
    if let data = try? JSONEncoder().encode(summary.assets) {
      defaults.set(data, forKey: metadataKey)
    }
  }
}
