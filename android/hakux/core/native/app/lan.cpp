/*
 * hakuX native core, app front end — System Link: the app's LAN relay
 * (android/app/src/main/cpp/xemu_lan.cpp, as xemu runs it) in the core's
 * process. The relay hosts or joins on the LAN; the core's Ethernet
 * controller (native/dev/nvnet.c) trades raw frames with it over a loopback
 * UDP pair, given to the core as "lan=<bind>,<remote>".
 */
#include "xemu_lan.h"

#include <jni.h>
#include <string>

#define EXPORT __attribute__((visibility("default")))

namespace {

std::string Str(JNIEnv* env, jstring s) {
  if (!s) return {};
  const char* c = env->GetStringUTFChars(s, nullptr);
  std::string out = c ? c : "";
  if (c) env->ReleaseStringUTFChars(s, c);
  return out;
}

}  // namespace

/* The relay started (mode "host" or "join", the host's address to join, this
 * device's and the game's names for the Find list): "<bind>,<remote>" for the
 * core, or null (off, or it couldn't start: an event says why). */
extern "C" EXPORT JNIEXPORT jstring JNICALL Java_com_izzy2lost_x1box_hakux_HakuxCoreActivity_nativeStartLan(
    JNIEnv* env, jclass, jstring mode, jstring host, jstring name, jstring game) {
  xemu_lan::Config cfg;
  cfg.mode = xemu_lan::ParseMode(Str(env, mode));
  cfg.host_addr = Str(env, host);
  cfg.name = Str(env, name);
  cfg.game = Str(env, game);
  std::string bind, remote;
  if (!xemu_lan::Start(cfg, &bind, &remote)) return nullptr;
  return env->NewStringUTF((bind + "," + remote).c_str());
}

/* The relay's next event for the UI as (event << 32) | arg, 0 when none
 * (SystemLink.eventMessage). */
extern "C" EXPORT JNIEXPORT jlong JNICALL Java_com_izzy2lost_x1box_hakux_HakuxCoreActivity_nativePollLanEvent(
    JNIEnv*, jclass) {
  xemu_lan::Event event;
  int arg = 0;
  if (!xemu_lan::PollEvent(&event, &arg)) return 0;
  return (static_cast<jlong>(event) << 32) | static_cast<uint32_t>(arg);
}
