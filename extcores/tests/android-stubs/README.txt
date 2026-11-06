Minimal stand-ins for android/log.h and GLES2/gl2.h.

They exist for one reason: the Android-only translation units
(player_android.cpp, sphynx_extcore_jni.cpp, android_paths.cpp with
SPHYNX_ANDROID=1) cannot be compiled in a desktop sandbox, and a typo in them
used to be discovered only after a 10-minute cloud build. With these headers a
`g++ -fsyntax-only` pass catches it in under a second:

  g++ -std=c++17 -fsyntax-only -DSPHYNX_ANDROID=1 \
      -Iextcores/tests/android-stubs -I"$JAVA_HOME/include" -I"$JAVA_HOME/include/linux" \
      -Iextcores/include extcores/src/player_android.cpp extcores/jni/sphynx_extcore_jni.cpp

CI runs exactly that before it starts the real build.
