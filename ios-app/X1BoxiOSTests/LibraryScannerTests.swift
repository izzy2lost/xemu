import XCTest
@testable import X1BoxiOS

final class LibraryScannerTests: XCTestCase {
  func testScannerFindsSupportedGamesRecursively() throws {
    let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString, isDirectory: true)
    try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
    defer {
      try? FileManager.default.removeItem(at: root)
    }

    let nested = root.appendingPathComponent("Racing", isDirectory: true)
    try FileManager.default.createDirectory(at: nested, withIntermediateDirectories: true)

    FileManager.default.createFile(atPath: root.appendingPathComponent("Halo_2.iso").path, contents: Data([0x00]))
    FileManager.default.createFile(atPath: nested.appendingPathComponent("Project_Gotham.xiso").path, contents: Data([0x00]))
    FileManager.default.createFile(atPath: root.appendingPathComponent("ignore.txt").path, contents: Data([0x00]))

    let games = try LibraryScanner().scanGames(in: root)

    XCTAssertEqual(games.count, 2)
    XCTAssertEqual(games.map(\.title), ["Halo 2", "Project Gotham"])
    XCTAssertTrue(games.allSatisfy { $0.relativePath.contains(".") })
  }
}
