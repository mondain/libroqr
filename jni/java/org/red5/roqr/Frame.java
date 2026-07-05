package org.red5.roqr;

/** One RoQR frame: RTMP message metadata plus one message payload. */
public final class Frame {
    private final long flowId;
    private final long timestamp;
    private final int messageType;
    private final long messageStreamId;
    private final long chunkStreamId;
    private final byte[] payload;

    public Frame(long flowId, long timestamp, int messageType,
                 long messageStreamId, long chunkStreamId, byte[] payload) {
        this.flowId = flowId;
        this.timestamp = timestamp;
        this.messageType = messageType;
        this.messageStreamId = messageStreamId;
        this.chunkStreamId = chunkStreamId;
        this.payload = payload;
    }

    public long flowId() { return flowId; }
    public long timestamp() { return timestamp; }
    public int messageType() { return messageType; }
    public long messageStreamId() { return messageStreamId; }
    public long chunkStreamId() { return chunkStreamId; }
    public byte[] payload() { return payload; }
}
