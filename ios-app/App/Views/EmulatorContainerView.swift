import SwiftUI

struct EmulatorContainerView: View {
  @EnvironmentObject private var model: AppModel

  var body: some View {
    ZStack(alignment: .topLeading) {
      EmulatorHostView(controller: model.emulatorSession.makeViewController())
        .ignoresSafeArea()

      if model.settingsStore.settings.touchOverlay.showOverlay && !model.controllerMonitor.hasConnectedController {
        TouchOverlayView()
          .environmentObject(model)
          .ignoresSafeArea()
      }

      VStack(alignment: .leading, spacing: 10) {
        HStack(spacing: 10) {
          Button {
            model.stopEmulation()
          } label: {
            Label("Back to Library", systemImage: "chevron.backward")
          }
          .buttonStyle(.borderedProminent)
          .tint(XboxTheme.accent)

          Menu("Snapshots") {
            ForEach(model.emulatorSession.snapshotSlots) { slot in
              Button("Save to Slot \(slot.slotNumber)") {
                model.saveSnapshot(to: slot.slotNumber)
              }
            }
          }
          .buttonStyle(.bordered)
          .tint(XboxTheme.accent)
        }

        switch model.emulatorSession.state {
        case .preparing:
          Text("Preparing session...")
            .font(.caption)
            .foregroundStyle(XboxTheme.text)
            .padding(10)
            .background(.black.opacity(0.45))
            .clipShape(RoundedRectangle(cornerRadius: 12, style: .continuous))
        case .failed(let message):
          Text(message)
            .font(.caption)
            .foregroundStyle(.white)
            .padding(10)
            .background(.red.opacity(0.75))
            .clipShape(RoundedRectangle(cornerRadius: 12, style: .continuous))
        case .idle, .running:
          EmptyView()
        }

        if let snapshotActionMessage = model.emulatorSession.snapshotActionMessage {
          Text(snapshotActionMessage)
            .font(.caption)
            .foregroundStyle(XboxTheme.text)
            .padding(10)
            .background(.black.opacity(0.45))
            .clipShape(RoundedRectangle(cornerRadius: 12, style: .continuous))
        }

        if let snapshotActionError = model.emulatorSession.snapshotActionError {
          Text(snapshotActionError)
            .font(.caption)
            .foregroundStyle(.white)
            .padding(10)
            .background(.red.opacity(0.75))
            .clipShape(RoundedRectangle(cornerRadius: 12, style: .continuous))
        }
      }
      .padding()
    }
  }
}

struct EmulatorHostView: UIViewControllerRepresentable {
  let controller: UIViewController

  func makeUIViewController(context _: Context) -> UIViewController {
    controller
  }

  func updateUIViewController(_: UIViewController, context _: Context) {}
}
