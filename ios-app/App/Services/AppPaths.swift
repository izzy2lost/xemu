import Foundation

enum AppPaths {
  static func appSupportRoot() throws -> URL {
    let base = try FileManager.default.url(
      for: .applicationSupportDirectory,
      in: .userDomainMask,
      appropriateFor: nil,
      create: true
    )
    let root = base.appendingPathComponent("X1Box", isDirectory: true)
    try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
    return root
  }

  static func stagedAssetURL(for kind: SetupAssetKind) throws -> URL {
    guard let name = kind.stagingFilename else {
      throw NSError(domain: "X1Box.AppPaths", code: 1, userInfo: [
        NSLocalizedDescriptionKey: "This asset kind does not stage to local storage."
      ])
    }
    return try appSupportRoot().appendingPathComponent(name)
  }

  static func stagedDVDURL() throws -> URL {
    try appSupportRoot().appendingPathComponent("dvd.iso")
  }

  static func embeddedCoreDirectory() throws -> URL {
    let url = try appSupportRoot().appendingPathComponent("EmbeddedCore", isDirectory: true)
    try FileManager.default.createDirectory(at: url, withIntermediateDirectories: true)
    return url
  }

  static func embeddedCoreFrameworkURL() throws -> URL {
    try embeddedCoreDirectory().appendingPathComponent("X1BoxEmbeddedCore.framework", isDirectory: true)
  }

  static func embeddedCoreFrameworkBinaryURL() throws -> URL {
    try embeddedCoreFrameworkURL().appendingPathComponent("X1BoxEmbeddedCore")
  }

  static func embeddedCoreDylibURL() throws -> URL {
    try embeddedCoreDirectory().appendingPathComponent("libxemu-ios-core.dylib")
  }

  static func configURL() throws -> URL {
    try appSupportRoot().appendingPathComponent("xemu.toml")
  }

  static func snapshotsDirectory() throws -> URL {
    let url = try appSupportRoot().appendingPathComponent("snapshots", isDirectory: true)
    try FileManager.default.createDirectory(at: url, withIntermediateDirectories: true)
    return url
  }

  static func snapshotManifestURL() throws -> URL {
    try snapshotsDirectory().appendingPathComponent("slots.json")
  }

  static func snapshotConfigURL(slotNumber: Int) throws -> URL {
    try snapshotsDirectory().appendingPathComponent("slot\(slotNumber)-xemu.toml")
  }
}
