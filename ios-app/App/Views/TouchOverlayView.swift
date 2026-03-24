import SwiftUI

struct TouchOverlayView: View {
  @EnvironmentObject private var model: AppModel

  var body: some View {
    GeometryReader { geometry in
      let settings = model.settingsStore.settings.touchOverlay
      ZStack {
        joystick(name: "left_stick")
          .position(x: geometry.size.width * 0.18, y: geometry.size.height * 0.72)

        joystick(name: "right_stick")
          .position(x: geometry.size.width * 0.72, y: geometry.size.height * 0.72)

        VStack(spacing: 18) {
          HStack(spacing: 18) {
            triggerButton("LB")
            triggerButton("LT")
            Spacer()
            triggerButton("RT")
            triggerButton("RB")
          }
          Spacer()
        }
        .padding(.horizontal, 26)
        .padding(.top, 24)

        VStack {
          Spacer()
          HStack {
            dpad
            Spacer()
            faceButtons
          }
          .padding(.horizontal, 22)
          .padding(.bottom, 34)
        }

        VStack {
          Spacer()
          HStack(spacing: 16) {
            controlButton("Back", width: 66, height: 40)
            controlButton("Start", width: 66, height: 40)
          }
          .padding(.bottom, 18)
        }
      }
      .opacity(settings.opacity)
      .scaleEffect(settings.scale)
      .offset(
        x: geometry.size.width * settings.offsetX,
        y: geometry.size.height * settings.offsetY
      )
    }
    .allowsHitTesting(true)
  }

  private var dpad: some View {
    VStack(spacing: 8) {
      controlButton("Up")
      HStack(spacing: 8) {
        controlButton("Left")
        controlButton("Right")
      }
      controlButton("Down")
    }
  }

  private var faceButtons: some View {
    VStack(spacing: 10) {
      controlButton("Y")
      HStack(spacing: 10) {
        controlButton("X")
        controlButton("B")
      }
      controlButton("A")
    }
  }

  private func triggerButton(_ name: String) -> some View {
    controlButton(name, width: 64, height: 44)
  }

  private func controlButton(_ name: String, width: CGFloat = 58, height: CGFloat = 58) -> some View {
    let mapping = buttonMapping(name)
    return Text(name)
      .font(.caption.bold())
      .foregroundStyle(.black)
      .frame(width: width, height: height)
      .background(XboxTheme.accent.opacity(0.88))
      .clipShape(RoundedRectangle(cornerRadius: 14, style: .continuous))
      .overlay(
        RoundedRectangle(cornerRadius: 14, style: .continuous)
          .stroke(XboxTheme.panelBorder.opacity(0.4), lineWidth: 1)
      )
      .gesture(
        DragGesture(minimumDistance: 0)
          .onChanged { _ in
            model.emulatorSession.sendButton(mapping, pressed: true)
          }
          .onEnded { _ in
            model.emulatorSession.sendButton(mapping, pressed: false)
          }
      )
  }

  private func joystick(name: String) -> some View {
    Circle()
      .stroke(XboxTheme.panelBorder.opacity(0.8), lineWidth: 3)
      .frame(width: 110, height: 110)
      .overlay(
        Circle()
          .fill(XboxTheme.accent.opacity(0.75))
          .frame(width: 42, height: 42)
      )
      .gesture(
        DragGesture(minimumDistance: 0)
          .onChanged { value in
            let normalizedX = Float(max(-1, min(1, value.translation.width / 42)))
            let normalizedY = Float(max(-1, min(1, value.translation.height / 42)))
            model.emulatorSession.sendAxis(name, x: normalizedX, y: normalizedY)
          }
          .onEnded { _ in
            model.emulatorSession.sendAxis(name, x: 0, y: 0)
          }
      )
  }

  private func buttonMapping(_ name: String) -> String {
    switch name {
    case "A", "B", "X", "Y":
      return name.lowercased()
    case "Up":
      return "dpad_up"
    case "Down":
      return "dpad_down"
    case "Left":
      return "dpad_left"
    case "Right":
      return "dpad_right"
    case "LB":
      return "left_bumper"
    case "LT":
      return "left_trigger"
    case "RB":
      return "right_bumper"
    case "RT":
      return "right_trigger"
    case "Start":
      return "start"
    case "Back":
      return "back"
    default:
      return name.lowercased()
    }
  }
}
