#include <jni.h>

extern "C" {
#include "roqr/roqr.h"
}

#include <cstdint>
#include <cstring>
#include <vector>

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

namespace {

// Attaches the current thread to the JVM if needed. On destruction detaches
// only if this scope performed the attach.
struct AttachedEnv {
    JNIEnv* env = nullptr;
    bool attached = false;

    AttachedEnv() {
        if (g_vm == nullptr) return;
        const jint rc = g_vm->GetEnv(reinterpret_cast<void**>(&env),
                                     JNI_VERSION_1_6);
        if (rc == JNI_EDETACHED) {
            if (g_vm->AttachCurrentThread(reinterpret_cast<void**>(&env),
                                          nullptr) == JNI_OK) {
                attached = true;
            } else {
                env = nullptr;
            }
        } else if (rc != JNI_OK) {
            env = nullptr;
        }
    }
    ~AttachedEnv() {
        if (attached) g_vm->DetachCurrentThread();
    }
};

// Per-client JNI state: the roqr_client and a global ref to the listener
// plus cached method ids and the Frame constructor.
struct JniClient {
    roqr_client* handle = nullptr;
    jobject listener = nullptr;      // global ref
    jmethodID on_message = nullptr;  // MessageListener.onMessage(Frame)
    jmethodID on_closed = nullptr;   // MessageListener.onClosed(long)
    jclass frame_class = nullptr;    // global ref
    jmethodID frame_ctor = nullptr;
};

void message_trampoline(const roqr_frame* f, void* user) {
    auto* jc = static_cast<JniClient*>(user);
    if (jc->listener == nullptr) return;
    AttachedEnv ae;
    if (ae.env == nullptr) return;
    JNIEnv* env = ae.env;

    jbyteArray payload = env->NewByteArray(static_cast<jsize>(f->payload_len));
    env->SetByteArrayRegion(
        payload, 0, static_cast<jsize>(f->payload_len),
        reinterpret_cast<const jbyte*>(f->payload));
    jobject frame = env->NewObject(
        jc->frame_class, jc->frame_ctor, static_cast<jlong>(f->flow_id),
        static_cast<jlong>(f->timestamp), static_cast<jint>(f->message_type),
        static_cast<jlong>(f->message_stream_id),
        static_cast<jlong>(f->chunk_stream_id), payload);
    env->CallVoidMethod(jc->listener, jc->on_message, frame);
    if (env->ExceptionCheck()) env->ExceptionClear();
    env->DeleteLocalRef(frame);
    env->DeleteLocalRef(payload);
}

void closed_trampoline(uint64_t code, void* user) {
    auto* jc = static_cast<JniClient*>(user);
    if (jc->listener == nullptr) return;
    AttachedEnv ae;
    if (ae.env == nullptr) return;
    ae.env->CallVoidMethod(jc->listener, jc->on_closed,
                           static_cast<jlong>(code));
    if (ae.env->ExceptionCheck()) ae.env->ExceptionClear();
}

}  // namespace

extern "C" {

JNIEXPORT jlong JNICALL
Java_org_red5_roqr_RoqrClient_nativeCreate(JNIEnv* env, jclass) {
    auto* jc = new JniClient();
    jc->handle = roqr_client_create();

    jclass fc = env->FindClass("org/red5/roqr/Frame");
    jc->frame_class = static_cast<jclass>(env->NewGlobalRef(fc));
    jc->frame_ctor = env->GetMethodID(jc->frame_class, "<init>", "(JJIJJ[B)V");

    return reinterpret_cast<jlong>(jc);
}

JNIEXPORT void JNICALL Java_org_red5_roqr_RoqrClient_nativeSetListener(
    JNIEnv* env, jclass, jlong h, jobject listener) {
    auto* jc = reinterpret_cast<JniClient*>(h);
    if (jc->listener != nullptr) env->DeleteGlobalRef(jc->listener);
    jc->listener = env->NewGlobalRef(listener);

    jclass lc = env->GetObjectClass(listener);
    jc->on_message =
        env->GetMethodID(lc, "onMessage", "(Lorg/red5/roqr/Frame;)V");
    jc->on_closed = env->GetMethodID(lc, "onClosed", "(J)V");

    roqr_client_set_on_message(jc->handle, message_trampoline, jc);
    roqr_client_set_on_closed(jc->handle, closed_trampoline, jc);
}

JNIEXPORT jboolean JNICALL Java_org_red5_roqr_RoqrClient_nativeConnect(
    JNIEnv* env, jclass, jlong h, jstring host, jint port, jboolean insecure) {
    auto* jc = reinterpret_cast<JniClient*>(h);
    if (host == nullptr) return JNI_FALSE;
    const char* chost = env->GetStringUTFChars(host, nullptr);
    const roqr_error rc = roqr_client_connect(
        jc->handle, chost, static_cast<uint16_t>(port), insecure ? 1 : 0);
    env->ReleaseStringUTFChars(host, chost);
    return rc == ROQR_OK ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL Java_org_red5_roqr_RoqrClient_nativeWaitConnected(
    JNIEnv*, jclass, jlong h, jint timeout_ms) {
    auto* jc = reinterpret_cast<JniClient*>(h);
    return roqr_client_wait_connected(jc->handle, timeout_ms) ? JNI_TRUE
                                                              : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_org_red5_roqr_RoqrClient_nativeDatagramsNegotiated(JNIEnv*, jclass,
                                                        jlong h) {
    auto* jc = reinterpret_cast<JniClient*>(h);
    return roqr_client_datagrams_negotiated(jc->handle) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL Java_org_red5_roqr_RoqrClient_nativeSend(
    JNIEnv* env, jclass, jlong h, jlong flow_id, jlong ts, jint type,
    jlong msid, jlong csid, jbyteArray payload, jint mode) {
    auto* jc = reinterpret_cast<JniClient*>(h);
    if (payload == nullptr) return JNI_FALSE;
    const jsize len = env->GetArrayLength(payload);
    std::vector<uint8_t> bytes(static_cast<size_t>(len));
    env->GetByteArrayRegion(payload, 0, len,
                            reinterpret_cast<jbyte*>(bytes.data()));
    roqr_frame f{};
    f.flow_id = static_cast<uint64_t>(flow_id);
    f.timestamp = static_cast<uint64_t>(ts);
    f.message_type = static_cast<uint8_t>(type);
    f.message_stream_id = static_cast<uint64_t>(msid);
    f.chunk_stream_id = static_cast<uint64_t>(csid);
    f.payload = bytes.data();
    f.payload_len = bytes.size();
    return roqr_client_send(jc->handle, &f,
                            static_cast<roqr_delivery_mode>(mode)) == ROQR_OK
               ? JNI_TRUE
               : JNI_FALSE;
}

JNIEXPORT void JNICALL Java_org_red5_roqr_RoqrClient_nativeBindFlow(
    JNIEnv*, jclass, jlong h, jlong flow_id) {
    roqr_client_bind_flow(reinterpret_cast<JniClient*>(h)->handle,
                          static_cast<uint64_t>(flow_id));
}

JNIEXPORT void JNICALL Java_org_red5_roqr_RoqrClient_nativeRetireFlow(
    JNIEnv*, jclass, jlong h, jlong flow_id) {
    roqr_client_retire_flow(reinterpret_cast<JniClient*>(h)->handle,
                            static_cast<uint64_t>(flow_id));
}

JNIEXPORT void JNICALL Java_org_red5_roqr_RoqrClient_nativeClose(
    JNIEnv*, jclass, jlong h, jlong code) {
    roqr_client_close(reinterpret_cast<JniClient*>(h)->handle,
                      static_cast<uint64_t>(code));
}

JNIEXPORT jboolean JNICALL Java_org_red5_roqr_RoqrClient_nativeWaitClosed(
    JNIEnv*, jclass, jlong h, jint timeout_ms) {
    return roqr_client_wait_closed(reinterpret_cast<JniClient*>(h)->handle,
                                   timeout_ms)
               ? JNI_TRUE
               : JNI_FALSE;
}

JNIEXPORT void JNICALL Java_org_red5_roqr_RoqrClient_nativeDestroy(
    JNIEnv* env, jclass, jlong h) {
    auto* jc = reinterpret_cast<JniClient*>(h);
    roqr_client_destroy(jc->handle);  // joins the network thread first
    if (jc->listener != nullptr) env->DeleteGlobalRef(jc->listener);
    if (jc->frame_class != nullptr) env->DeleteGlobalRef(jc->frame_class);
    delete jc;
}

}  // extern "C"
