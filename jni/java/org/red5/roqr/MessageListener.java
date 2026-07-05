package org.red5.roqr;

/** Receives RoQR frames and close notifications. Callbacks fire on a
 * native (non-JVM-created) thread that the binding attaches for the call;
 * do not block in them. */
public interface MessageListener {
    void onMessage(Frame frame);

    void onClosed(long appErrorCode);
}
