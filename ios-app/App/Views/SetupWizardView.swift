import SwiftUI
import UniformTypeIdentifiers

struct SetupWizardView: View {
  @EnvironmentObject private var model: AppModel
  @State private var pickingKind: SetupAssetKind?
  @State private var isImportingFile = false
  @State private var isImportingFolder = false
  @State private var errorMessage: String?

  private let orderedKinds: [SetupAssetKind] = [.mcpx, .flash, .hdd, .eeprom, .gamesFolder]

  var body: some View {
    NavigationStack {
      ScrollView {
        VStack(alignment: .leading, spacing: 18) {
          Text("Setup Wizard")
            .font(.system(size: 30, weight: .bold, design: .rounded))
            .foregroundStyle(XboxTheme.text)

          Text("Import your original Xbox files and choose the games folder to unlock the iOS shell.")
            .foregroundStyle(XboxTheme.muted)

          ForEach(orderedKinds) { kind in
            VStack(alignment: .leading, spacing: 10) {
              Text(kind.displayName)
                .font(.headline)
                .foregroundStyle(XboxTheme.text)

              Text(statusText(for: kind))
                .font(.subheadline)
                .foregroundStyle(XboxTheme.muted)

              Button(kind.allowsFolderSelection ? "Choose Folder" : "Import File") {
                pickingKind = kind
                if kind.allowsFolderSelection {
                  isImportingFolder = true
                } else {
                  isImportingFile = true
                }
              }
              .buttonStyle(.borderedProminent)
              .tint(XboxTheme.accent)
            }
            .xboxPanel()
          }

          if let errorMessage {
            Text(errorMessage)
              .foregroundStyle(.red)
              .font(.footnote)
          }

          Button("Finish Setup") {
            model.refreshRoute()
          }
          .buttonStyle(.borderedProminent)
          .tint(XboxTheme.accent)
          .disabled(!model.setupStore.summary.isCoreReady)
        }
        .padding(24)
      }
      .navigationBarHidden(true)
    }
    .fileImporter(
      isPresented: $isImportingFile,
      allowedContentTypes: [.data, .item],
      allowsMultipleSelection: false
    ) { result in
      handleImport(result)
    }
    .fileImporter(
      isPresented: $isImportingFolder,
      allowedContentTypes: [.folder],
      allowsMultipleSelection: false
    ) { result in
      handleImport(result)
    }
  }

  private func statusText(for kind: SetupAssetKind) -> String {
    if let record = model.setupStore.summary.record(for: kind) {
      return record.displayName
    }
    return kind.isRequired ? "Required" : "Optional"
  }

  private func handleImport(_ result: Result<[URL], Error>) {
    guard let kind = pickingKind else {
      return
    }
    switch result {
    case .success(let urls):
      guard let url = urls.first else { return }
      do {
        try model.setupStore.importSelection(from: url, kind: kind)
        model.refreshRoute()
        errorMessage = nil
      } catch {
        errorMessage = error.localizedDescription
      }
    case .failure(let error):
      errorMessage = error.localizedDescription
    }
  }
}
