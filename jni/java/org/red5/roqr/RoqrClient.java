package org.red5.roqr;

/** A RoQR client: connect to a RoQR server, send and receive frames. */
public final class RoqrClient implements AutoCloseable {
    static {
        System.loadLibrary("roqr-jni");
    }

    private long nativeHandle;
    private MessageListener listener;

    public RoqrClient() {
        nativeHandle = nativeCreate();
    }

    /** Set before connect(). */
    public void setListener(MessageListener listener) {
        this.listener = listener;
        nativeSetListener(nativeHandle, listener);
    }

    public boolean connect(String host, int port, boolean insecureSkipVerify) {
        return nativeConnect(nativeHandle, host, port, insecureSkipVerify);
    }

    public boolean waitConnected(int timeoutMs) {
        return nativeWaitConnected(nativeHandle, timeoutMs);
    }

    public boolean datagramsNegotiated() {
        return nativeDatagramsNegotiated(nativeHandle);
    }

    public boolean send(Frame frame, DeliveryMode mode) {
        return nativeSend(nativeHandle, frame.flowId(), frame.timestamp(),
                frame.messageType(), frame.messageStreamId(),
                frame.chunkStreamId(), frame.payload(), mode.nativeValue());
    }

    public void bindFlow(long flowId) { nativeBindFlow(nativeHandle, flowId); }
    public void retireFlow(long flowId) {
        nativeRetireFlow(nativeHandle, flowId);
    }

    public void close(long appErrorCode) {
        nativeClose(nativeHandle, appErrorCode);
    }

    public boolean waitClosed(int timeoutMs) {
        return nativeWaitClosed(nativeHandle, timeoutMs);
    }

    public void destroy() {
        if (nativeHandle != 0) {
            nativeDestroy(nativeHandle);
            nativeHandle = 0;
        }
    }

    @Override
    public void close() {
        destroy();
    }

    private static native long nativeCreate();
    private static native void nativeSetListener(long h, MessageListener l);
    private static native boolean nativeConnect(long h, String host, int port,
                                                boolean insecure);
    private static native boolean nativeWaitConnected(long h, int timeoutMs);
    private static native boolean nativeDatagramsNegotiated(long h);
    private static native boolean nativeSend(long h, long flowId, long ts,
                                             int type, long msid, long csid,
                                             byte[] payload, int mode);
    private static native void nativeBindFlow(long h, long flowId);
    private static native void nativeRetireFlow(long h, long flowId);
    private static native void nativeClose(long h, long appErrorCode);
    private static native boolean nativeWaitClosed(long h, int timeoutMs);
    private static native void nativeDestroy(long h);
}
