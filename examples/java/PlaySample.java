import org.red5.roqr.EgressGateway;

/** Serves a RoQR stream to an RTMP player.
 *  Usage: PlaySample <rtmpPort> <roqrHost> <roqrPort> <stream> */
public final class PlaySample {
    public static void main(String[] args) throws Exception {
        if (args.length < 4) {
            System.err.println(
                "usage: PlaySample <rtmpPort> <roqrHost> <roqrPort> <stream>");
            System.exit(2);
        }
        int rtmpPort = Integer.parseInt(args[0]);
        String host = args[1];
        int roqrPort = Integer.parseInt(args[2]);
        String stream = args[3];

        try (EgressGateway egress = new EgressGateway()) {
            if (!egress.start(rtmpPort, host, roqrPort, stream, true)) {
                System.err.println("failed to start egress");
                System.exit(1);
            }
            if (!egress.waitPlaying(5000)) {
                System.err.println("warning: RoQR play not confirmed");
            }
            System.out.printf(
                "PlaySample: play with ffplay rtmp://127.0.0.1:%d/live/%s%n",
                rtmpPort, stream);
            System.out.println("Ctrl-C to stop.");
            Thread.currentThread().join();
        }
    }
}
