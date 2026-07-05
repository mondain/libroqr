import java.util.concurrent.TimeUnit;
import org.red5.roqr.EgressGateway;
import org.red5.roqr.IngestGateway;

public class RoqrGatewayTest {
    public static void main(String[] args) throws Exception {
        String relayd = args[0];
        String certDir = args[1];
        int relayPort = 45612;

        Process relay = new ProcessBuilder(relayd,
                "--cert", certDir + "/cert.pem",
                "--key", certDir + "/key.pem",
                "--port", Integer.toString(relayPort),
                "--mode", "media").inheritIO().start();
        Thread.sleep(700);
        try {
            IngestGateway ingest = new IngestGateway();
            if (!ingest.start(45613, "127.0.0.1", relayPort, true)) {
                fail("ingest start");
            }
            // No RTMP publisher connected -> not publishing yet.
            if (ingest.waitPublishing(500)) fail("unexpected publishing");

            EgressGateway egress = new EgressGateway();
            if (!egress.start(45614, "127.0.0.1", relayPort, "cam", true)) {
                fail("egress start");
            }
            // Egress connects+plays at start -> playing becomes true.
            if (!egress.waitPlaying(5000)) fail("egress not playing");

            ingest.stop();
            ingest.destroy();
            egress.stop();
            egress.destroy();

            // Null host/stream must be rejected, not crash the JVM.
            IngestGateway ni = new IngestGateway();
            if (ni.start(45699, null, relayPort, true)) fail("null ingest host accepted");
            ni.destroy();
            EgressGateway ne = new EgressGateway();
            if (ne.start(45699, "127.0.0.1", relayPort, null, true)) {
                fail("null egress stream accepted");
            }
            if (ne.start(45699, null, relayPort, "cam", true)) {
                fail("null egress host accepted");
            }
            ne.destroy();
            System.out.println("PASS: JNI gateway lifecycle");
        } finally {
            relay.destroy();
            relay.waitFor(5, TimeUnit.SECONDS);
        }
    }

    private static void fail(String why) {
        System.err.println("FAIL: " + why);
        System.exit(1);
    }
}
