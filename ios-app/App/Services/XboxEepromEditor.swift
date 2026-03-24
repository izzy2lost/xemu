import Foundation

enum XboxEepromEditor {
  private static let eepromSize = 256
  private static let factoryChecksumOffset = 0x30
  private static let factoryChecksumStart = 0x34
  private static let factoryChecksumLength = 0x2C
  private static let userChecksumOffset = 0x60
  private static let userChecksumStart = 0x64
  private static let userChecksumLength = 0x5C
  private static let videoStandardOffset = 0x58
  private static let languageOffset = 0x90

  enum Language: String, CaseIterable, Codable {
    case english
    case japanese
    case german
    case french
    case spanish
    case italian
    case korean
    case chinese
    case portuguese

    var id: UInt32 {
      switch self {
      case .english:
        return 0x0000_0001
      case .japanese:
        return 0x0000_0002
      case .german:
        return 0x0000_0003
      case .french:
        return 0x0000_0004
      case .spanish:
        return 0x0000_0005
      case .italian:
        return 0x0000_0006
      case .korean:
        return 0x0000_0007
      case .chinese:
        return 0x0000_0008
      case .portuguese:
        return 0x0000_0009
      }
    }

    init(rawID: UInt32) {
      self = Self.allCases.first(where: { $0.id == rawID }) ?? .english
    }
  }

  enum VideoStandard: String, CaseIterable, Codable {
    case ntscM = "ntsc-m"
    case ntscJ = "ntsc-j"
    case palI = "pal-i"
    case palM = "pal-m"

    var id: UInt32 {
      switch self {
      case .ntscM:
        return 0x0040_0100
      case .ntscJ:
        return 0x0040_0200
      case .palI:
        return 0x0080_0300
      case .palM:
        return 0x0040_0400
      }
    }

    init(rawID: UInt32) {
      self = Self.allCases.first(where: { $0.id == rawID }) ?? .ntscM
    }
  }

  struct Snapshot {
    var language: Language
    var videoStandard: VideoStandard
    var rawLanguage: UInt32
    var rawVideoStandard: UInt32
  }

  static func load(from url: URL) throws -> Snapshot {
    let data = try Data(contentsOf: url)
    try ensureValidSize(data, url: url)

    let rawLanguage = readLEUInt32(data, offset: languageOffset)
    let rawVideoStandard = readLEUInt32(data, offset: videoStandardOffset)
    return Snapshot(
      language: Language(rawID: rawLanguage),
      videoStandard: VideoStandard(rawID: rawVideoStandard),
      rawLanguage: rawLanguage,
      rawVideoStandard: rawVideoStandard
    )
  }

  static func apply(to url: URL, language: Language, videoStandard: VideoStandard) throws -> Bool {
    var data = try Data(contentsOf: url)
    try ensureValidSize(data, url: url)

    let currentLanguage = readLEUInt32(data, offset: languageOffset)
    let currentVideoStandard = readLEUInt32(data, offset: videoStandardOffset)
    if currentLanguage == language.id, currentVideoStandard == videoStandard.id {
      return false
    }

    writeLEUInt32(&data, offset: languageOffset, value: language.id)
    writeLEUInt32(&data, offset: videoStandardOffset, value: videoStandard.id)

    let factoryChecksum = xboxChecksum(data, offset: factoryChecksumStart, length: factoryChecksumLength)
    writeLEUInt32(&data, offset: factoryChecksumOffset, value: factoryChecksum)

    let userChecksum = xboxChecksum(data, offset: userChecksumStart, length: userChecksumLength)
    writeLEUInt32(&data, offset: userChecksumOffset, value: userChecksum)

    try data.write(to: url, options: .atomic)
    return true
  }

  private static func ensureValidSize(_ data: Data, url: URL) throws {
    guard data.count == eepromSize else {
      throw NSError(domain: "X1Box.XboxEepromEditor", code: 1, userInfo: [
        NSLocalizedDescriptionKey: "Invalid EEPROM size \(data.count), expected \(eepromSize) (\(url.path))."
      ])
    }
  }

  private static func readLEUInt32(_ data: Data, offset: Int) -> UInt32 {
    let bytes = Array(data[offset..<(offset + 4)])
    return bytes.enumerated().reduce(UInt32(0)) { partialResult, item in
      partialResult | (UInt32(item.element) << UInt32(item.offset * 8))
    }
  }

  private static func writeLEUInt32(_ data: inout Data, offset: Int, value: UInt32) {
    let bytes: [UInt8] = [
      UInt8(value & 0xFF),
      UInt8((value >> 8) & 0xFF),
      UInt8((value >> 16) & 0xFF),
      UInt8((value >> 24) & 0xFF)
    ]
    data.replaceSubrange(offset..<(offset + 4), with: bytes)
  }

  private static func xboxChecksum(_ data: Data, offset: Int, length: Int) -> UInt32 {
    precondition(length.isMultiple(of: 4), "Checksum length must be 32-bit aligned")

    var high: UInt32 = 0
    var low: UInt32 = 0
    var position = offset
    let end = offset + length

    while position < end {
      let value = readLEUInt32(data, offset: position)
      let sum = (UInt64(high) << 32) | UInt64(low)
      let next = sum + UInt64(value)

      high = UInt32((next >> 32) & 0xFFFF_FFFF)
      low = low &+ value
      position += 4
    }

    return ~(high &+ low)
  }
}
