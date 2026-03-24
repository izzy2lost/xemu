import SwiftUI

struct LauncherView: View {
  var body: some View {
    VStack(spacing: 16) {
      Text("X1 BOX")
        .font(.system(size: 34, weight: .heavy, design: .rounded))
        .foregroundStyle(XboxTheme.accent)
      Text("Original Xbox inspired shell for iPhone and iPad")
        .font(.headline)
        .foregroundStyle(XboxTheme.muted)
      ProgressView()
        .tint(XboxTheme.accent)
        .scaleEffect(1.2)
    }
    .frame(maxWidth: 520)
    .xboxPanel()
    .padding(24)
  }
}
