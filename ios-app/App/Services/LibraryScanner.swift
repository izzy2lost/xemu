import Foundation

struct LibraryScanner {
  private let supportedExtensions: Set<String> = ["iso", "xiso", "cso", "cci"]

  func scanGames(in folderURL: URL) throws -> [GameEntry] {
    let keys: [URLResourceKey] = [.isDirectoryKey, .fileSizeKey, .nameKey]
    let enumerator = FileManager.default.enumerator(
      at: folderURL,
      includingPropertiesForKeys: keys,
      options: [.skipsHiddenFiles]
    )

    var games: [GameEntry] = []
    while let next = enumerator?.nextObject() as? URL {
      let values = try next.resourceValues(forKeys: Set(keys))
      if values.isDirectory == true {
        continue
      }
      let ext = next.pathExtension.lowercased()
      guard supportedExtensions.contains(ext) else {
        continue
      }

      let relative = next.path.replacingOccurrences(of: folderURL.path + "/", with: "")
      let title = next.deletingPathExtension().lastPathComponent.replacingOccurrences(of: "_", with: " ")
      games.append(
        GameEntry(
          title: title,
          url: next,
          relativePath: relative,
          sizeBytes: Int64(values.fileSize ?? 0)
        )
      )
    }

    return games.sorted { $0.title.localizedCaseInsensitiveCompare($1.title) == .orderedAscending }
  }
}
