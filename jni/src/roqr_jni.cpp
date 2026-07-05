#include <jni.h>

extern "C" {
#include "roqr/roqr.h"
}

namespace {
JavaVM* g_vm = nullptr;
}

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* /*reserved*/) {
    g_vm = vm;
    return JNI_VERSION_1_6;
}

extern "C" JNIEXPORT jstring JNICALL
Java_org_red5_roqr_RoqrNative_version(JNIEnv* env, jclass /*clazz*/) {
    return env->NewStringUTF(roqr_version());
}
