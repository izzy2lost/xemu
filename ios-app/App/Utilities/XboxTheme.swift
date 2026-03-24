import SwiftUI

enum XboxTheme {
  static let background = LinearGradient(
    colors: [
      Color(red: 0.02, green: 0.05, blue: 0.02),
      Color(red: 0.04, green: 0.09, blue: 0.05),
      Color(red: 0.01, green: 0.03, blue: 0.02)
    ],
    startPoint: .topLeading,
    endPoint: .bottomTrailing
  )

  static let panel = Color(red: 0.06, green: 0.12, blue: 0.07)
  static let panelBorder = Color(red: 0.52, green: 0.90, blue: 0.36)
  static let accent = Color(red: 0.72, green: 1.0, blue: 0.48)
  static let text = Color(red: 0.92, green: 0.98, blue: 0.88)
  static let muted = Color(red: 0.64, green: 0.78, blue: 0.62)
}

struct XboxPanelModifier: ViewModifier {
  func body(content: Content) -> some View {
    content
      .padding(20)
      .background(XboxTheme.panel.opacity(0.92))
      .overlay(
        RoundedRectangle(cornerRadius: 24)
          .stroke(XboxTheme.panelBorder.opacity(0.55), lineWidth: 1)
      )
      .clipShape(RoundedRectangle(cornerRadius: 24, style: .continuous))
      .shadow(color: XboxTheme.accent.opacity(0.12), radius: 18, x: 0, y: 8)
  }
}

extension View {
  func xboxPanel() -> some View {
    modifier(XboxPanelModifier())
  }
}
