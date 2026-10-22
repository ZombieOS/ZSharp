package ZSharp;

/** A live Z# object supplied by the VM; it is not a copy of the object's fields. */
public final class ZSharpObject {
    final String reference;
    final String type;

    ZSharpObject(String reference, String type) {
        this.reference = reference;
        this.type = type;
    }

    public Object get(String member) {
        return ZSharp.objectOperation("host.get", reference, member);
    }

    public void set(String member, Object value) {
        ZSharp.objectOperation("host.set", reference, member, value);
    }

    public Object invoke(String member, Object... arguments) {
        return ZSharp.objectOperation("host.invoke", reference, member, arguments);
    }

    @Override
    public String toString() {
        return type + " (Z# reference " + reference + ")";
    }
}
