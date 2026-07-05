package org.red5.roqr;

/** Accepts an RTMP publisher and re-originates it over RoQR. */
public final class IngestGateway implements AutoCloseable {
    static {
        System.loadLibrary("roqr-jni");
    }

    private long nativeHandle;

    public IngestGateway() {
        nativeHandle = nativeCreate();
    }

    /** True if the RTMP listener started. Not a guarantee the RoQR server
     * leg is live; call waitPublishing to confirm the end-to-end path. */
    public boolean start(int rtmpPort, String roqrHost, int roqrPort,
                         boolean insecureSkipVerify) {
        return nativeStart(nativeHandle, rtmpPort, roqrHost, roqrPort,
                insecureSkipVerify);
    }

    public boolean waitPublishing(int timeoutMs) {
        return nativeWaitPublishing(nativeHandle, timeoutMs);
    }

    public void stop() { nativeStop(nativeHandle); }

    public void destroy() {
        if (nativeHandle != 0) {
            nativeDestroy(nativeHandle);
            nativeHandle = 0;
        }
    }

    @Override
    public void close() { destroy(); }

    private static native long nativeCreate();
    private static native boolean nativeStart(long h, int rtmpPort,
                                              String roqrHost, int roqrPort,
                                              boolean insecure);
    private static native boolean nativeWaitPublishing(long h, int timeoutMs);
    private static native void nativeStop(long h);
    private static native void nativeDestroy(long h);
}
