import SwiftUI
import X1BoxNativeCore

struct SettingsView: View {
  @ObservedObject var store: SettingsStore
  @ObservedObject var setupStore: SetupAssetStore
  let onDismiss: () -> Void

  @State private var draft: EmulatorSettings
  @State private var isImportingEEPROM = false
  @State private var isImportingEmbeddedCore = false
  @State private var assetErrorMessage: String?
  @State private var embeddedCoreStatus: String = X1BoxNativeBridge.shared().embeddedCoreStatusSummary()
  @State private var embeddedCorePath: String?

  init(store: SettingsStore, setupStore: SetupAssetStore, onDismiss: @escaping () -> Void) {
    self.store = store
    self.setupStore = setupStore
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

        Section("Native Core") {
          Text(embeddedCoreStatus)
            .font(.footnote)
            .foregroundStyle(XboxTheme.muted)

          Text(setupStore.summary.record(for: .embeddedCore)?.displayName ?? "No embedded core artifact imported into app storage.")
            .font(.footnote)
            .foregroundStyle(XboxTheme.muted)

          if let stagedCorePath = setupStore.localURL(for: .embeddedCore)?.path {
            Text(stagedCorePath)
              .font(.footnote.monospaced())
              .foregroundStyle(XboxTheme.text)
              .textSelection(.enabled)
          }

          if let embeddedCorePath,
             !embeddedCorePath.isEmpty {
            Text(embeddedCorePath)
              .font(.footnote.monospaced())
              .foregroundStyle(XboxTheme.accent)
              .textSelection(.enabled)
          }

          Button("Import Embedded Core") {
            isImportingEmbeddedCore = true
          }

          if setupStore.summary.record(for: .embeddedCore) != nil {
            Button("Clear Imported Embedded Core") {
              do {
                try setupStore.removeSelection(for: .embeddedCore)
                assetErrorMessage = nil
                refreshEmbeddedCoreStatus()
              } catch {
                assetErrorMessage = error.localizedDescription
              }
            }
            .foregroundStyle(.red)
          }

          Button("Refresh Embedded Core Detection") {
            refreshEmbeddedCoreStatus()
          }

          Text("Import a signed X1BoxEmbeddedCore.framework, X1BoxEmbeddedCore.xcframework, or libxemu-ios-core.dylib exported from the upstream iOS core build. If you replace a core that is already loaded, restart the app so iOS picks up the new image cleanly.")
            .font(.footnote)
            .foregroundStyle(XboxTheme.muted)
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
          Text(setupStore.summary.record(for: .eeprom)?.displayName ?? "No EEPROM imported yet.")
            .font(.footnote)
            .foregroundStyle(XboxTheme.muted)

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

          Button("Import EEPROM") {
            isImportingEEPROM = true
          }

          if setupStore.summary.record(for: .eeprom) != nil {
            Button("Clear EEPROM") {
              do {
                try setupStore.removeSelection(for: .eeprom)
                assetErrorMessage = nil
              } catch {
                assetErrorMessage = error.localizedDescription
              }
            }
            .foregroundStyle(.red)
          }

          if let assetErrorMessage {
            Text(assetErrorMessage)
              .font(.footnote)
              .foregroundStyle(.red)
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
    .onAppear {
      refreshEmbeddedCoreStatus()
    }
    .fileImporter(
      isPresented: $isImportingEmbeddedCore,
      allowedContentTypes: [.item, .folder],
      allowsMultipleSelection: false
    ) { result in
      switch result {
      case .success(let urls):
        guard let url = urls.first else { return }
        do {
          try setupStore.importEmbeddedCoreArtifact(from: url)
          assetErrorMessage = nil
          refreshEmbeddedCoreStatus()
        } catch {
          assetErrorMessage = error.localizedDescription
        }
      case .failure(let error):
        assetErrorMessage = error.localizedDescription
      }
    }
    .fileImporter(
      isPresented: $isImportingEEPROM,
      allowedContentTypes: [.data, .item],
      allowsMultipleSelection: false
    ) { result in
      switch result {
      case .success(let urls):
        guard let url = urls.first else { return }
        do {
          try setupStore.importSelection(from: url, kind: .eeprom)
          assetErrorMessage = nil
        } catch {
          assetErrorMessage = error.localizedDescription
        }
      case .failure(let error):
        assetErrorMessage = error.localizedDescription
      }
    }
  }

  private func refreshEmbeddedCoreStatus() {
    let bridge = X1BoxNativeBridge.shared()
    bridge.refreshEmbeddedCoreAvailability()
    embeddedCoreStatus = bridge.embeddedCoreStatusSummary()
    embeddedCorePath = bridge.resolvedEmbeddedCorePath()
  }
}
