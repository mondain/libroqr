package org.red5.roqr;

/** Low-level JNI entry points. Loads the native roqr-jni library. */
public final class RoqrNative {
    static {
        System.loadLibrary("roqr-jni");
    }

    private RoqrNative() {}

    /** Native library version, "major.minor.patch". */
    public static native String version();
}
