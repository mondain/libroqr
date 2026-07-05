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

The root `CMakeLists.txt` still calls `find_package(JNI)` whenever
`ROQR_BUILD_JNI=ON`, including on Android, but the `jni` subdirectory is
gated with `if(JNI_FOUND OR ANDROID)` so it is added regardless of whether
`find_package(JNI)` succeeds under the NDK toolchain (`FindJNI` can be
unreliable there). Inside `jni/CMakeLists.txt`, the
`target_include_directories(... ${JNI_INCLUDE_DIRS})` and
`target_link_libraries(... ${JNI_LIBRARIES})` lines are further guarded with
`if(NOT ANDROID)`, since the NDK sysroot already provides `jni.h` on the
default include path and needs no separate JNI libraries to link against.

Note that a HOST JDK is still required even for the NDK configure: the NDK
only supplies `jni.h`, not a `javac`. `jni/CMakeLists.txt` calls
`find_package(Java REQUIRED COMPONENTS Development)` and `add_jar` to build
`roqr.jar` from the platform-independent Java sources, and both need a host
JDK's `javac` on `PATH` (or discoverable via `JAVA_HOME`) regardless of the
Android target ABI.

## Package

Copy `build/android-<abi>/jni/libroqr-jni.so` into
`app/src/main/jniLibs/<abi>/`, add the `org.red5.roqr` Java sources (or the
`roqr.jar`) to your app, and call `System.loadLibrary("roqr-jni")` (already
done in the static initializers).

## ABI note

Build one `libroqr-jni.so` per ABI you ship. The Java API is identical
across ABIs.
