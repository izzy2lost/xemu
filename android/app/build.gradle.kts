import java.util.Properties
import org.jetbrains.kotlin.gradle.dsl.JvmTarget

plugins {
  id("com.android.application")
}

val keystorePropertiesFile = rootProject.file("key.properties")
val keystoreProperties = Properties()
val hasKeystoreProperties = keystorePropertiesFile.exists()

if (hasKeystoreProperties) {
  keystorePropertiesFile.inputStream().use { keystoreProperties.load(it) }
}

val hasReleaseKeystore = hasKeystoreProperties &&
  listOf("storeFile", "storePassword", "keyAlias", "keyPassword").all {
    !keystoreProperties.getProperty(it).isNullOrBlank()
  }

val localPropertiesFile = rootProject.file("local.properties")
val localProperties = Properties()
if (localPropertiesFile.exists()) {
  localPropertiesFile.inputStream().use { localProperties.load(it) }
}

val localCmakeArguments = listOf(
  "xemu.cargoExecutable" to "CARGO_EXECUTABLE",
  "xemu.mesonExecutable" to "MESON_EXECUTABLE",
  "xemu.cargoHome" to "XEMU_CARGO_HOME",
  "xemu.rustupHome" to "XEMU_RUSTUP_HOME",
  "xemu.hostRustLinker" to "XEMU_HOST_RUST_LINKER",
).mapNotNull { (propertyName, cmakeName) ->
  localProperties.getProperty(propertyName)
    ?.trim()
    ?.takeIf(String::isNotEmpty)
    ?.let { "-D$cmakeName=$it" }
}

/*
 * Diagnostic profile builds may package Khronos' validation layer without
 * checking the large binary into the source tree. The directory must contain
 * ABI subdirectories, for example arm64-v8a/libVkLayer_khronos_validation.so.
 */
val vulkanValidationJniLibs =
  providers.environmentVariable("XEMU_VVL_JNILIBS").orNull

android {
  namespace = "com.izzy2lost.x1box"
  compileSdk = 36
  buildToolsVersion = "36.1.0"
  ndkVersion = "30.0.15729638"

  defaultConfig {
    applicationId = "com.izzy2lost.x1box"
    /*
     * 29 (Android 10), not lower: below that the NDK toolchain falls back to
     * *emulated* TLS, which put __emutls_get_address in the hot path of every
     * emulator thread. Native ELF TLS needs API 29+.
     */
    minSdk = 29
    targetSdk = 36

    versionCode = 26
    versionName = "1.2.5"

    ndk {
      abiFilters += listOf("arm64-v8a")
    }

    externalNativeBuild {
      cmake {
        arguments += listOf(
          "-DXEMU_ANDROID_BUILD_ID=3",
          "-DXEMU_ENABLE_XISO_CONVERTER=ON",
          "-DCMAKE_C_FLAGS_DEBUG=-O2 -g0",
          "-DCMAKE_CXX_FLAGS_DEBUG=-O2 -g0",
          "-DCMAKE_C_FLAGS_RELWITHDEBINFO=-O2 -g0 -fvisibility=hidden",
          "-DCMAKE_CXX_FLAGS_RELWITHDEBINFO=-O2 -g0 -fvisibility=hidden",
          "-DCMAKE_C_FLAGS_RELEASE=-O2 -g0 -fvisibility=hidden",
          "-DCMAKE_CXX_FLAGS_RELEASE=-O2 -g0 -fvisibility=hidden",
        ) + localCmakeArguments
        cppFlags += listOf("-std=c++17", "-fexceptions", "-frtti")
      }
    }
  }

  signingConfigs {
    if (hasReleaseKeystore) {
      create("release") {
        storeFile = file(keystoreProperties.getProperty("storeFile"))
        storePassword = keystoreProperties.getProperty("storePassword")
        keyAlias = keystoreProperties.getProperty("keyAlias")
        keyPassword = keystoreProperties.getProperty("keyPassword")
      }
    }
  }

  buildTypes {
    debug {
      ndk {
        debugSymbolLevel = "NONE"
      }
    }
    release {
      externalNativeBuild {
        cmake {
          arguments += listOf("-DXEMU_ENABLE_LTO=ON")
        }
      }
      isMinifyEnabled = true
      isShrinkResources = true
      proguardFiles(
        getDefaultProguardFile("proguard-android-optimize.txt"),
        "proguard-rules.pro"
      )
      if (hasReleaseKeystore) {
        signingConfig = signingConfigs.getByName("release")
      }
    }
    create("profile") {
      initWith(getByName("release"))
      isDebuggable = false
      isProfileable = true
      signingConfig = signingConfigs.getByName("debug")
      matchingFallbacks += listOf("release")
      ndk {
        debugSymbolLevel = "FULL"
      }
    }
  }

  if (!vulkanValidationJniLibs.isNullOrBlank()) {
    /* AGP 9 deprecated srcDir() in favour of the mutable `directories` set. */
    sourceSets.getByName("profile").jniLibs.directories += vulkanValidationJniLibs
  }

  externalNativeBuild {
    cmake {
      path = file("src/main/cpp/CMakeLists.txt")
      version = "3.22.1"
    }
  }

  packaging {
    resources.excludes += setOf(
      "**/*.md",
      "META-INF/LICENSE*",
      "META-INF/NOTICE*"
    )
    /* Extract .so to disk (nativeLibraryDir); required for adrenotools hooks / custom GPU drivers. */
    jniLibs.useLegacyPackaging = true
    jniLibs.keepDebugSymbols += setOf("**/*.so")
  }

  compileOptions {
    sourceCompatibility = JavaVersion.VERSION_21
    targetCompatibility = JavaVersion.VERSION_21
  }

}

dependencies {
  implementation("androidx.core:core-ktx:1.15.0")
  implementation("androidx.appcompat:appcompat:1.7.0")
  implementation("androidx.constraintlayout:constraintlayout:2.1.4")
  implementation("androidx.documentfile:documentfile:1.0.1")
  implementation("io.coil-kt:coil:2.7.0")
  implementation("com.google.android.material:material:1.12.0")
}

kotlin {
  compilerOptions {
    jvmTarget.set(JvmTarget.JVM_21)
  }
}
