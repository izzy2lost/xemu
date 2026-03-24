import Foundation

enum SetupAssetKind: String, CaseIterable, Codable, Identifiable {
  case mcpx
  case flash
  case hdd
  case eeprom
  case embeddedCore
  case gamesFolder

  var id: String { rawValue }

  var displayName: String {
    switch self {
    case .mcpx:
      return "MCPX Boot ROM"
    case .flash:
      return "Flash ROM / BIOS"
    case .hdd:
      return "Hard Disk"
    case .eeprom:
      return "EEPROM"
    case .embeddedCore:
      return "Embedded iOS Core"
    case .gamesFolder:
      return "Games Folder"
    }
  }

  var stagingFilename: String? {
    switch self {
    case .mcpx:
      return "mcpx.bin"
    case .flash:
      return "flash.bin"
    case .hdd:
      return "hdd.img"
    case .eeprom:
      return "eeprom.bin"
    case .embeddedCore:
      return nil
    case .gamesFolder:
      return nil
    }
  }

  var isRequired: Bool {
    switch self {
    case .mcpx, .flash, .hdd, .gamesFolder:
      return true
    case .eeprom, .embeddedCore:
      return false
    }
  }

  var allowsFolderSelection: Bool {
    self == .gamesFolder
  }
}

struct ImportedAssetRecord: Codable, Equatable {
  var kind: SetupAssetKind
  var displayName: String
  var localPath: String?
  var bookmarkKey: String?
}

struct SetupSummary: Equatable {
  var assets: [SetupAssetKind: ImportedAssetRecord]

  func record(for kind: SetupAssetKind) -> ImportedAssetRecord? {
    assets[kind]
  }

  var isCoreReady: Bool {
    SetupAssetKind.allCases.filter(\.isRequired).allSatisfy { assets[$0] != nil }
  }
}
