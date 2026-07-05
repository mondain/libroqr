package org.red5.roqr;

/** RoQR delivery mode (draft s10). */
public enum DeliveryMode {
    STREAM(0),
    DATAGRAM(1),
    AUTO(2);

    private final int nativeValue;

    DeliveryMode(int nativeValue) {
        this.nativeValue = nativeValue;
    }

    public int nativeValue() {
        return nativeValue;
    }
}
