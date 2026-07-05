package org.red5.roqr;

/** Plays a RoQR stream and serves it to an RTMP player. */
public final class EgressGateway implements AutoCloseable {
    static {
        System.loadLibrary("roqr-jni");
    }

    private long nativeHandle;

    public EgressGateway() {
        nativeHandle = nativeCreate();
    }

    public boolean start(int rtmpPort, String roqrHost, int roqrPort,
                         String streamName, boolean insecureSkipVerify) {
        return nativeStart(nativeHandle, rtmpPort, roqrHost, roqrPort,
                streamName, insecureSkipVerify);
    }

    public boolean waitPlaying(int timeoutMs) {
        return nativeWaitPlaying(nativeHandle, timeoutMs);
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
                                              String streamName,
                                              boolean insecure);
    private static native boolean nativeWaitPlaying(long h, int timeoutMs);
    private static native void nativeStop(long h);
    private static native void nativeDestroy(long h);
}
