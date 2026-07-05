# Building the RoQR JNI library for Android (NDK)

The `roqr-jni` shared library and its dependencies (roqr-core, roqr-quic,
roqr-rtmp, roqr-gateway, roqr-ffi, picoquic, picotls, OpenSSL) are all
C/C++ and cross-compile for Android with the NDK toolchain. The Java
classes in `jni/java/org/red5/roqr` are platform-independent and go into
your Android app or an AAR unchanged.

## Prerequisites

- Android NDK r25+ (`ANDROID_NDK_HOME` set).
- OpenSSL built for the target ABI. This repo's sibling `openssl-android`
  tree provides prebuilt static libs; point `OPENSSL_ROOT_DIR` at the ABI
  you are building.
- picoquic/picotls source (fetched by `scripts/setup_picoquic_deps.sh`);
  they build under the NDK toolchain via the same source-tree mechanism
  `FindPicoquic.cmake` uses.

## Configure

```
export ANDROID_ABI=arm64-v8a          # or armeabi-v7a, x86_64
export ANDROID_PLATFORM=android-24

eval "$(scripts/setup_picoquic_deps.sh)"

cmake -S . -B build/android-$ANDROID_ABI \
  -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=$ANDROID_ABI \
  -DANDROID_PLATFORM=$ANDROID_PLATFORM \
  -DOPENSSL_ROOT_DIR=/path/to/openssl-android/$ANDROID_ABI \
  -DROQR_BUILD_QUIC=ON -DROQR_BUILD_RTMP=ON \
  -DROQR_BUILD_EXAMPLES=ON -DROQR_BUILD_FFI=ON \
  -DROQR_BUILD_JNI=ON -DROQR_BUILD_TESTS=OFF -DROQR_BUILD_TOOLS=OFF

cmake --build build/android-$ANDROID_ABI --target roqr-jni
```

`find_package(JNI)` is not used on Android — the NDK provides `jni.h` on the
default include path, so `roqr-jni` compiles against it. If your CMake
version's `FindJNI` misbehaves under the NDK, the `jni/CMakeLists.txt`
`target_include_directories(... ${JNI_INCLUDE_DIRS})` line is a no-op there
(the NDK sysroot already has `jni.h`); guard it with
`if(NOT ANDROID)` if configure complains.

## Package

Copy `build/android-<abi>/jni/libroqr-jni.so` into
`app/src/main/jniLibs/<abi>/`, add the `org.red5.roqr` Java sources (or the
`roqr.jar`) to your app, and call `System.loadLibrary("roqr-jni")` (already
done in the static initializers).

## ABI note

Build one `libroqr-jni.so` per ABI you ship. The Java API is identical
across ABIs.
