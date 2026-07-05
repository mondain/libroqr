import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import org.red5.roqr.DeliveryMode;
import org.red5.roqr.Frame;
import org.red5.roqr.MessageListener;
import org.red5.roqr.RoqrClient;

public class RoqrClientTest {
    public static void main(String[] args) throws Exception {
        // args: <relayd-binary> <cert-dir>
        String relayd = args[0];
        String certDir = args[1];
        int port = 45610;

        Process relay = new ProcessBuilder(relayd,
                "--cert", certDir + "/cert.pem",
                "--key", certDir + "/key.pem",
                "--port", Integer.toString(port),
                "--mode", "echo").inheritIO().start();
        Thread.sleep(700);

        try {
            final CountDownLatch got = new CountDownLatch(1);
            final byte[][] received = new byte[1][];
            RoqrClient client = new RoqrClient();
            client.setListener(new MessageListener() {
                public void onMessage(Frame f) {
                    received[0] = f.payload();
                    got.countDown();
                }
                public void onClosed(long code) {}
            });
            if (!client.connect("127.0.0.1", port, true)) fail("connect");
            if (!client.waitConnected(5000)) fail("waitConnected");

            byte[] payload = {0x17, 0x01, (byte) 0xAB};
            Frame f = new Frame(0, 100, 9, 1, 6, payload);
            if (!client.send(f, DeliveryMode.STREAM)) fail("send");

            if (!got.await(5, TimeUnit.SECONDS)) fail("no echo");
            if (received[0].length != 3 || received[0][2] != (byte) 0xAB) {
                fail("payload mismatch");
            }
            client.close(0);
            client.waitClosed(5000);
            client.destroy();
            System.out.println("PASS: RoqrClient echo round-trip");
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
