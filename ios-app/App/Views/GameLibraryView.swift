import SwiftUI

struct GameLibraryView: View {
  @EnvironmentObject private var model: AppModel
  @State private var useGrid = false

  private let boxArt = BoxArtResolver()
  private let gridColumns = [GridItem(.adaptive(minimum: 150, maximum: 180), spacing: 16)]

  var body: some View {
    NavigationStack {
      ScrollView {
        VStack(alignment: .leading, spacing: 18) {
          header
          actions
          snapshotsPanel
          content
        }
        .padding(24)
      }
      .navigationBarHidden(true)
      .sheet(isPresented: $model.isShowingSettings) {
        SettingsView(store: model.settingsStore) {
          model.isShowingSettings = false
        }
      }
      .task {
        await model.reloadLibrary()
      }
    }
  }

  private var header: some View {
    VStack(alignment: .leading, spacing: 8) {
      Text("Game Library")
        .font(.system(size: 30, weight: .bold, design: .rounded))
        .foregroundStyle(XboxTheme.text)
      Text("Boot to dashboard, scan your games, and jump straight into emulation.")
        .foregroundStyle(XboxTheme.muted)
    }
    .xboxPanel()
  }

  private var actions: some View {
    VStack(alignment: .leading, spacing: 12) {
      HStack(spacing: 12) {
        Button("Start Console") {
          Task { await model.startDashboard() }
        }
        .buttonStyle(.borderedProminent)
        .tint(XboxTheme.accent)

        if model.emulatorSession.snapshotSlots.contains(where: { $0.isOccupied }) {
          Menu("Resume") {
            ForEach(model.emulatorSession.snapshotSlots.filter { $0.isOccupied }) { slot in
              Button("Slot \(slot.slotNumber): \(slot.title)") {
                Task { await model.resumeSnapshot(slot) }
              }
            }
          }
          .buttonStyle(.bordered)
          .tint(XboxTheme.accent)
        }

        Button(useGrid ? "List" : "Grid") {
          useGrid.toggle()
        }
        .buttonStyle(.bordered)
        .tint(XboxTheme.accent)

        Button("Settings") {
          model.isShowingSettings = true
        }
        .buttonStyle(.bordered)
        .tint(XboxTheme.accent)
      }

      if let emulatorErrorMessage = model.emulatorErrorMessage {
        Text(emulatorErrorMessage)
          .font(.footnote)
          .foregroundStyle(.red)
      }

      if let emulatorNoticeMessage = model.emulatorNoticeMessage {
        Text(emulatorNoticeMessage)
          .font(.footnote)
          .foregroundStyle(.yellow)
      }

      if let snapshotActionMessage = model.emulatorSession.snapshotActionMessage {
        Text(snapshotActionMessage)
          .font(.footnote)
          .foregroundStyle(XboxTheme.accent)
      }

      if let snapshotActionError = model.emulatorSession.snapshotActionError {
        Text(snapshotActionError)
          .font(.footnote)
          .foregroundStyle(.red)
      }

      if let scanErrorMessage = model.scanErrorMessage {
        Text(scanErrorMessage)
          .font(.footnote)
          .foregroundStyle(.red)
      }
    }
    .xboxPanel()
  }

  private var snapshotsPanel: some View {
    VStack(alignment: .leading, spacing: 12) {
      Text("Resume Slots")
        .font(.headline)
        .foregroundStyle(XboxTheme.text)

      Text("These slots already restore the saved boot target and shell profile. Full memory-state resume will connect here when the native snapshot API is linked.")
        .font(.footnote)
        .foregroundStyle(XboxTheme.muted)

      ForEach(model.emulatorSession.snapshotSlots) { slot in
        HStack(alignment: .top, spacing: 14) {
          VStack(alignment: .leading, spacing: 4) {
            Text("Slot \(slot.slotNumber)")
              .font(.caption.weight(.bold))
              .foregroundStyle(XboxTheme.accent)
            Text(slot.title)
              .font(.headline)
              .foregroundStyle(XboxTheme.text)
            Text(slot.detail)
              .font(.footnote)
              .foregroundStyle(XboxTheme.muted)
            if let createdAt = slot.createdAt {
              Text(createdAt.formatted(date: .abbreviated, time: .shortened))
                .font(.caption)
                .foregroundStyle(XboxTheme.muted)
            }
          }

          Spacer()

          VStack(spacing: 8) {
            Button("Resume") {
              Task { await model.resumeSnapshot(slot) }
            }
            .buttonStyle(.borderedProminent)
            .tint(XboxTheme.accent)
            .disabled(!slot.isOccupied)

            if slot.isOccupied {
              Button("Delete") {
                model.deleteSnapshot(slot)
              }
              .buttonStyle(.bordered)
              .tint(.red)
            }
          }
        }
        .padding(14)
        .background(XboxTheme.panel.opacity(0.55))
        .clipShape(RoundedRectangle(cornerRadius: 18, style: .continuous))
      }
    }
    .xboxPanel()
  }

  @ViewBuilder
  private var content: some View {
    if model.games.isEmpty {
      Text("No supported games were found in the selected folder.")
        .foregroundStyle(XboxTheme.muted)
        .frame(maxWidth: .infinity, alignment: .leading)
        .xboxPanel()
    } else if useGrid {
      LazyVGrid(columns: gridColumns, spacing: 16) {
        ForEach(model.games) { game in
          gameGridCard(game)
        }
      }
    } else {
      LazyVStack(spacing: 14) {
        ForEach(model.games) { game in
          gameListCard(game)
        }
      }
    }
  }

  private func gameListCard(_ game: GameEntry) -> some View {
    Button {
      Task { await model.start(game: game) }
    } label: {
      HStack(spacing: 14) {
        AsyncImage(url: boxArt.url(for: game, enabled: model.settingsStore.settings.boxArtLookup)) { image in
          image.resizable().scaledToFill()
        } placeholder: {
          RoundedRectangle(cornerRadius: 12).fill(XboxTheme.panelBorder.opacity(0.12))
            .overlay(Text("XBOX").foregroundStyle(XboxTheme.accent).font(.caption.bold()))
        }
        .frame(width: 72, height: 96)
        .clipShape(RoundedRectangle(cornerRadius: 12))

        VStack(alignment: .leading, spacing: 6) {
          Text(game.title)
            .font(.headline)
            .foregroundStyle(XboxTheme.text)
          Text(game.relativePath)
            .font(.footnote)
            .foregroundStyle(XboxTheme.muted)
          Text(ByteCountFormatter.string(fromByteCount: game.sizeBytes, countStyle: .file))
            .font(.caption)
            .foregroundStyle(XboxTheme.muted)
        }
        Spacer()
      }
      .frame(maxWidth: .infinity, alignment: .leading)
    }
    .buttonStyle(.plain)
    .xboxPanel()
  }

  private func gameGridCard(_ game: GameEntry) -> some View {
    Button {
      Task { await model.start(game: game) }
    } label: {
      VStack(alignment: .leading, spacing: 10) {
        AsyncImage(url: boxArt.url(for: game, enabled: model.settingsStore.settings.boxArtLookup)) { image in
          image.resizable().scaledToFill()
        } placeholder: {
          RoundedRectangle(cornerRadius: 14).fill(XboxTheme.panelBorder.opacity(0.12))
            .overlay(Text("XBOX").foregroundStyle(XboxTheme.accent).font(.caption.bold()))
        }
        .frame(height: 180)
        .clipShape(RoundedRectangle(cornerRadius: 14))

        Text(game.title)
          .font(.headline)
          .foregroundStyle(XboxTheme.text)
          .lineLimit(2)

        Text(ByteCountFormatter.string(fromByteCount: game.sizeBytes, countStyle: .file))
          .font(.caption)
          .foregroundStyle(XboxTheme.muted)
      }
      .frame(maxWidth: .infinity, alignment: .leading)
    }
    .buttonStyle(.plain)
    .xboxPanel()
  }
}
