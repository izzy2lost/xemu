-keep interface com.izzy2lost.x1box.XisoProgressCallback {
    void onProgress(int, int);
}

# SDL's native libraries register and invoke this Java bridge by exact JNI names.
# R8 cannot see those native-to-Java references and must not shrink or rename them.
-keep class org.libsdl.app.** {
    *;
}
