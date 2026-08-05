// Top-level build file.
plugins {
  /*
   * AGP 9 has built-in Kotlin support, so the separate
   * org.jetbrains.kotlin.android plugin must NOT be applied -- AGP fails the
   * build if it is. See https://issuetracker.google.com/438678642
   */
  id("com.android.application") version "9.2.1" apply false
}
