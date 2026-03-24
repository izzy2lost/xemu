import SwiftUI

struct SettingsView: View {
  @ObservedObject var store: SettingsStore
  let onDismiss: () -> Void

  @State private var draft: EmulatorSettings

  init(store: SettingsStore, onDismiss: @escaping () -> Void) {
    self.store = store
    self.onDismiss = onDismiss
    _draft = State(initialValue: store.settings)
  }

  var body: some View {
    NavigationStack {
      Form {
        Section("Language") {
          Picker("App Language", selection: $draft.appLanguage) {
            Text("System").tag("system")
            Text("English").tag("en")
            Text("Spanish").tag("es")
          }
        }

        Section("Display") {
          Picker("Renderer", selection: $draft.renderer) {
            Text("OpenGL").tag("opengl")
            Text("Vulkan").tag("vulkan")
          }
          Picker("Filtering", selection: $draft.filtering) {
            Text("Linear").tag("linear")
            Text("Nearest").tag("nearest")
          }
          Picker("Aspect Ratio", selection: $draft.displayMode) {
            ForEach(EmulatorDisplayMode.allCases) { mode in
              Text(mode.title).tag(mode.rawValue)
            }
          }
          Stepper("Resolution Scale: \(draft.surfaceScale)x", value: $draft.surfaceScale, in: 1...3)
          Toggle("VSync", isOn: $draft.vsync)
        }

        Section("Performance") {
          Picker("CPU Threading", selection: $draft.tcgThread) {
            Text("Multi").tag("multi")
            Text("Single").tag("single")
          }
          Stepper("Frame Limit: \(draft.frameRateLimit) FPS", value: $draft.frameRateLimit, in: 30...60, step: 30)
          Picker("System Memory", selection: $draft.systemMemoryMiB) {
            Text("64 MiB").tag(64)
            Text("128 MiB").tag(128)
          }
          Toggle("Shader Cache", isOn: $draft.cacheShaders)
          Toggle("Hardware FPU", isOn: $draft.hardFPU)
          Toggle("Skip Boot Animation", isOn: $draft.skipBootAnimation)
        }

        Section("Input") {
          Toggle("Enable Bluetooth and USB controllers", isOn: $draft.inputEnableControllers)
          Toggle("Auto-bind controllers", isOn: $draft.inputAutoBind)
          Toggle("Allow vibration", isOn: $draft.inputAllowVibration)
          Toggle("Background input capture", isOn: $draft.inputBackgroundCapture)
        }

        Section("Input Ports") {
          ForEach(Array(draft.inputPorts.indices), id: \.self) { index in
            VStack(alignment: .leading, spacing: 10) {
              Text("Port \(index + 1)")
                .font(.headline)

              Picker("Driver", selection: $draft.inputPorts[index].driver) {
                ForEach(InputDriverOption.allCases) { option in
                  Text(option.title).tag(option.rawValue)
                }
              }

              Picker("Binding", selection: $draft.inputPorts[index].bindingMode) {
                ForEach(InputBindingMode.allCases) { option in
                  Text(option.title).tag(option.rawValue)
                }
              }

              if draft.inputPorts[index].bindingMode == InputBindingMode.manual.rawValue {
                TextField("Manual mapping GUID", text: $draft.inputPorts[index].bindingGUID)
                  .textInputAutocapitalization(.never)
                  .autocorrectionDisabled()
              }

              Picker("Expansion Slot A", selection: $draft.inputPorts[index].slotA) {
                ForEach(InputExpansionOption.allCases) { option in
                  Text(option.title).tag(option.rawValue)
                }
              }

              Picker("Expansion Slot B", selection: $draft.inputPorts[index].slotB) {
                ForEach(InputExpansionOption.allCases) { option in
                  Text(option.title).tag(option.rawValue)
                }
              }
            }
            .padding(.vertical, 6)
          }
        }

        Section("Touch Overlay") {
          Toggle("Show on-screen controls", isOn: $draft.touchOverlay.showOverlay)
          VStack(alignment: .leading) {
            Text("Opacity: \(Int(draft.touchOverlay.opacity * 100))%")
            Slider(value: $draft.touchOverlay.opacity, in: 0.2...1.0)
          }
          VStack(alignment: .leading) {
            Text("Scale: \(Int(draft.touchOverlay.scale * 100))%")
            Slider(value: $draft.touchOverlay.scale, in: 0.75...1.35)
          }
          VStack(alignment: .leading) {
            Text("Horizontal Position: \(Int(draft.touchOverlay.offsetX * 100))%")
            Slider(value: $draft.touchOverlay.offsetX, in: -0.2...0.2)
          }
          VStack(alignment: .leading) {
            Text("Vertical Position: \(Int(draft.touchOverlay.offsetY * 100))%")
            Slider(value: $draft.touchOverlay.offsetY, in: -0.2...0.2)
          }
        }

        Section("Library") {
          Toggle("Download cover art when available", isOn: $draft.boxArtLookup)
        }

        Section("Network") {
          Toggle("Enable networking", isOn: $draft.netEnable)
          Picker("Backend", selection: $draft.netBackend) {
            Text("NAT").tag("nat")
            Text("UDP").tag("udp")
            Text("Bridge").tag("bridge")
          }
          if draft.netBackend == "bridge" {
            Text("Bridge mode is kept in Settings for parity, but the iOS shell currently falls back to NAT at launch time.")
              .font(.footnote)
              .foregroundStyle(.yellow)
          }
          TextField("UDP bind address", text: $draft.netUdpBind)
          TextField("UDP remote address", text: $draft.netUdpRemote)
          TextField("Interface", text: $draft.netInterface)
        }

        Section("Audio") {
          Toggle("Use DSP", isOn: $draft.useDSP)
          Toggle("HRTF", isOn: $draft.hrtf)
          Picker("Audio Driver", selection: $draft.audioDriver) {
            Text("Core Audio").tag("coreaudio")
            Text("SDL").tag("sdl")
            Text("Disabled").tag("dummy")
          }
        }

        Section("EEPROM") {
          Picker("Language", selection: $draft.eeprom.language) {
            ForEach(XboxEepromEditor.Language.allCases, id: \.rawValue) { language in
              Text(language.rawValue.capitalized).tag(language.rawValue)
            }
          }
          Picker("Video Standard", selection: $draft.eeprom.videoStandard) {
            ForEach(XboxEepromEditor.VideoStandard.allCases, id: \.rawValue) { standard in
              Text(standard.rawValue.uppercased()).tag(standard.rawValue)
            }
          }
        }
      }
      .scrollContentBackground(.hidden)
      .background(XboxTheme.background.ignoresSafeArea())
      .navigationTitle("Settings")
      .toolbar {
        ToolbarItem(placement: .cancellationAction) {
          Button("Close") {
            onDismiss()
          }
        }
        ToolbarItem(placement: .confirmationAction) {
          Button("Save") {
            store.update(draft)
            onDismiss()
          }
        }
      }
    }
    .preferredColorScheme(.dark)
  }
}
