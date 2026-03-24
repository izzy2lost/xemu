import Foundation

struct BoxArtResolver {
  private let baseURL = URL(string: "https://raw.githubusercontent.com/izzy2lost/X1_Covers/main/")!

  func url(for game: GameEntry, enabled: Bool) -> URL? {
    guard enabled else {
      return nil
    }

    let slug = game.title
      .lowercased()
      .replacingOccurrences(of: "'", with: "")
      .replacingOccurrences(of: ":", with: "")
      .replacingOccurrences(of: "/", with: "-")
      .replacingOccurrences(of: " ", with: "%20")

    return baseURL.appendingPathComponent("\(slug).png")
  }
}
