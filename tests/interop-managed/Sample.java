import ZSharp.ZSharp;
import ZSharp.ZSharpObject;
import ZSharp.ZSharp_Export;
import java.math.BigDecimal;
import java.util.Map;

@ZSharp_Export
public class Sample {
    public int value;
    public final String readOnly = "fixed";
    private final String secret = "must not escape";
    private static int calls;

    public Sample(int initial) { value = initial; }
    public int increase(int amount) { value += amount; return value; }
    public Sample self() { return this; }
    private String hidden() { return secret; }
    public static int add(int left, int right) { return left + right; }
    public static int counted() { return ++calls; }
    public static BigDecimal exact(BigDecimal value) { return value; }
    public static int[] array(int[] value) { return value; }
    public static Map<String, Object> record() { return Map.of("name", "bridge", "value", 42); }
    public static String printed() { System.out.println("user-output-stays-on-stderr"); return "printed"; }
    public static Object callback(int value) { return ZSharp.call("Tests:Bridge:Double", value); }
    public static Object proxy(ZSharpObject value) {
        Object old = value.get("value");
        value.set("value", 21);
        return value.invoke("increase", old);
    }
    public static String ambiguous(String value) { return "text"; }
    public static String ambiguous(Sample value) { return "object"; }
    public static String overload(int value) { return "int"; }
    public static String overload(String value) { return "text"; }
}

class NotPublic {
    @ZSharp_Export public static String leak() { return "no"; }
}
