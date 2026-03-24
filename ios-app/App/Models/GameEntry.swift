import Foundation

struct GameEntry: Identifiable, Hashable {
  let id: UUID
  let title: String
  let url: URL
  let relativePath: String
  let sizeBytes: Int64

  init(title: String, url: URL, relativePath: String, sizeBytes: Int64) {
    self.id = UUID()
    self.title = title
    self.url = url
    self.relativePath = relativePath
    self.sizeBytes = sizeBytes
  }
}
