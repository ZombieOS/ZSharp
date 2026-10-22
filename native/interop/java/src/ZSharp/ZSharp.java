package ZSharp;

/** Entry point for synchronous callbacks into the calling Z# VM. */
public final class ZSharp {
    @FunctionalInterface
    interface Callback {
        Object call(String operation, String target, String reference, String member, Object[] arguments) throws Exception;
    }

    private static volatile Callback callback;

    private ZSharp() {
    }

    static void connect(Callback value) {
        callback = value;
    }

    /** Calls an imported Z# brain, for example {@code Main:Main:Calculate}. */
    public static Object call(String target, Object... arguments) {
        if (target == null || target.isBlank()) {
            throw new IllegalArgumentException("Z# callback target must not be empty");
        }
        Callback connection = callback;
        if (connection == null) {
            throw new IllegalStateException("Z# callbacks are only available inside the ZVM worker");
        }
        try {
            return connection.call("host.call", target, null, null, arguments);
        } catch (RuntimeException error) {
            throw error;
        } catch (Exception error) {
            throw new IllegalStateException("Z# callback failed: " + error.getMessage(), error);
        }
    }

    static Object objectOperation(String operation, String reference, String member, Object... arguments) {
        Callback connection = callback;
        if (connection == null) {
            throw new IllegalStateException("Z# callbacks are only available inside the ZVM worker");
        }
        try {
            return connection.call(operation, null, reference, member, arguments);
        } catch (RuntimeException error) {
            throw error;
        } catch (Exception error) {
            throw new IllegalStateException("Z# object callback failed: " + error.getMessage(), error);
        }
    }
}
