import org.red5.roqr.RoqrNative;

public class RoqrSmokeTest {
    public static void main(String[] args) {
        String v = RoqrNative.version();
        if (!"0.1.0".equals(v)) {
            System.err.println("FAIL: version=" + v + " expected 0.1.0");
            System.exit(1);
        }
        System.out.println("PASS: RoqrNative.version()=" + v);
    }
}
