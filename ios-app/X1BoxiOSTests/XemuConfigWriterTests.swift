import XCTest
@testable import X1BoxiOS

final class XemuConfigWriterTests: XCTestCase {
  func testGeneratedConfigIncludesSystemAndNetworkSections() throws {
    let writer = XemuConfigWriter()
    let summary = SetupSummary(assets: [
      .mcpx: ImportedAssetRecord(kind: .mcpx, displayName: "mcpx.bin", localPath: "/tmp/mcpx.bin", bookmarkKey: nil),
      .flash: ImportedAssetRecord(kind: .flash, displayName: "flash.bin", localPath: "/tmp/flash.bin", bookmarkKey: nil),
      .hdd: ImportedAssetRecord(kind: .hdd, displayName: "hdd.img", localPath: "/tmp/hdd.img", bookmarkKey: nil),
      .eeprom: ImportedAssetRecord(kind: .eeprom, displayName: "eeprom.bin", localPath: "/tmp/eeprom.bin", bookmarkKey: nil)
    ])

    var settings = EmulatorSettings()
    settings.netEnable = true
    settings.netBackend = "nat"
    settings.touchOverlay.scale = 1.1

    let configURL = try writer.writeConfig(setup: summary, settings: settings, dvdURL: nil)
    let config = try String(contentsOf: configURL, encoding: .utf8)

    XCTAssertTrue(config.contains("[sys.files]"))
    XCTAssertTrue(config.contains("bootrom_path = \"/tmp/mcpx.bin\""))
    XCTAssertTrue(config.contains("[net]"))
    XCTAssertTrue(config.contains("backend = \"nat\""))
    XCTAssertFalse(config.contains("dvd_path ="))
  }

  func testBridgeNetworkFallsBackToNatInGeneratedConfig() throws {
    let writer = XemuConfigWriter()
    let summary = SetupSummary(assets: [
      .mcpx: ImportedAssetRecord(kind: .mcpx, displayName: "mcpx.bin", localPath: "/tmp/mcpx.bin", bookmarkKey: nil),
      .flash: ImportedAssetRecord(kind: .flash, displayName: "flash.bin", localPath: "/tmp/flash.bin", bookmarkKey: nil),
      .hdd: ImportedAssetRecord(kind: .hdd, displayName: "hdd.img", localPath: "/tmp/hdd.img", bookmarkKey: nil)
    ])

    var settings = EmulatorSettings()
    settings.netEnable = true
    settings.netBackend = "bridge"

    let configURL = try writer.writeConfig(setup: summary, settings: settings, dvdURL: nil)
    let config = try String(contentsOf: configURL, encoding: .utf8)

    XCTAssertTrue(config.contains("backend = \"nat\""))
    XCTAssertFalse(config.contains("backend = \"bridge\""))
  }
}
