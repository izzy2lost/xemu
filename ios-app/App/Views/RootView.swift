import SwiftUI

struct RootView: View {
  @EnvironmentObject private var model: AppModel

  var body: some View {
    ZStack {
      XboxTheme.background.ignoresSafeArea()

      switch model.route {
      case .launcher:
        LauncherView()
      case .setup:
        SetupWizardView()
      case .library:
        GameLibraryView()
      case .emulator:
        EmulatorContainerView()
      }
    }
    .preferredColorScheme(.dark)
  }
}
