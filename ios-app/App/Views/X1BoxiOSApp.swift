import SwiftUI

@main
struct X1BoxiOSApp: App {
  @StateObject private var model = AppModel()

  var body: some Scene {
    WindowGroup {
      RootView()
        .environmentObject(model)
        .environment(\.locale, appLocale)
    }
  }

  private var appLocale: Locale {
    let languageCode = model.settingsStore.settings.appLanguage
    if languageCode == "system" {
      return .autoupdatingCurrent
    }
    return Locale(identifier: languageCode)
  }
}
