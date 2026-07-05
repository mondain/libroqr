import org.red5.roqr.IngestGateway;

/** Bridges an RTMP publisher into a RoQR server.
 *  Usage: PublishSample <rtmpPort> <roqrHost> <roqrPort> */
public final class PublishSample {
    public static void main(String[] args) throws Exception {
        if (args.length < 3) {
            System.err.println(
                "usage: PublishSample <rtmpPort> <roqrHost> <roqrPort>");
            System.exit(2);
        }
        int rtmpPort = Integer.parseInt(args[0]);
        String host = args[1];
        int roqrPort = Integer.parseInt(args[2]);

        try (IngestGateway ingest = new IngestGateway()) {
            if (!ingest.start(rtmpPort, host, roqrPort, true)) {
                System.err.println("failed to start ingest");
                System.exit(1);
            }
            System.out.printf(
                "PublishSample: publish RTMP to rtmp://127.0.0.1:%d/live/<name>%n",
                rtmpPort);
            System.out.println("Ctrl-C to stop.");
            Thread.currentThread().join();
        }
    }
}
