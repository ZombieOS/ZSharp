package ZSharp;

import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.io.PrintStream;
import java.lang.reflect.Array;
import java.lang.reflect.Constructor;
import java.lang.reflect.Executable;
import java.lang.reflect.Field;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.lang.reflect.Modifier;
import java.math.BigDecimal;
import java.math.BigInteger;
import java.net.URLClassLoader;
import java.nio.charset.StandardCharsets;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.IdentityHashMap;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.TreeSet;
import java.util.jar.JarFile;

/** Persistent JSON-lines transport used by the desktop ZVM, not an application entry point. */
public final class Worker {
    private static final int MAX_FRAME_BYTES = 1024 * 1024;
    private final BufferedReader input;
    private final PrintStream protocol;
    private final Map<String, Module> modules = new LinkedHashMap<>();
    private final Map<String, Object> objects = new LinkedHashMap<>();
    private final IdentityHashMap<Object, String> identities = new IdentityHashMap<>();
    private long nextObject = 1;
    private long nextCallback = 1;
    private final Thread workerThread = Thread.currentThread();

    private record Module(Path path, URLClassLoader loader, TreeSet<String> classes) {
    }

    private record Match(Executable executable, Object[] arguments, int score) {
    }

    private record Conversion(Object value, int score) {
    }

    public Worker(BufferedReader input, PrintStream protocol) {
        this.input = input;
        this.protocol = protocol;
        ZSharp.connect(this::callback);
    }

    public static void main(String[] arguments) throws Exception {
        PrintStream protocol = new PrintStream(System.out, true, StandardCharsets.UTF_8);
        System.setOut(new PrintStream(System.err, true, StandardCharsets.UTF_8));
        Worker worker = new Worker(new BufferedReader(new InputStreamReader(System.in, StandardCharsets.UTF_8)), protocol);
        try {
            String line;
            while ((line = worker.readFrame()) != null) {
                worker.accept(line);
            }
        } finally {
            worker.close();
        }
    }

    private void close() throws Exception {
        ZSharp.connect(null);
        objects.clear();
        identities.clear();
        for (Module module : modules.values()) {
            module.loader.close();
        }
        modules.clear();
    }

    @SuppressWarnings("unchecked")
    private static Map<String, Object> object(Object value) {
        if (!(value instanceof Map<?, ?>)) {
            throw new IllegalArgumentException("Bridge request must be a JSON object");
        }
        return (Map<String, Object>) value;
    }

    private static String text(Map<String, Object> request, String field) {
        Object value = request.get(field);
        return value == null ? "" : value.toString();
    }

    private static Map<String, Object> result(Object id, boolean ok, Object value) {
        Map<String, Object> response = new LinkedHashMap<>();
        response.put("id", id);
        response.put("op", "result");
        response.put("ok", ok);
        response.put(ok ? "value" : "error", value);
        return response;
    }

    private void send(Map<String, Object> message) {
        String json = Json.write(message);
        if (json.getBytes(StandardCharsets.UTF_8).length > MAX_FRAME_BYTES) {
            throw new IllegalArgumentException("Java bridge frame exceeds the 1 MiB limit");
        }
        protocol.println(json);
        if (protocol.checkError()) {
            throw new IllegalStateException("ZVM bridge connection was closed");
        }
    }

    private String readFrame() throws java.io.IOException {
        StringBuilder line = new StringBuilder();
        int character;
        while ((character = input.read()) != -1 && character != '\n') {
            if (line.length() == MAX_FRAME_BYTES) {
                throw new IllegalArgumentException("Java bridge frame exceeds the 1 MiB limit");
            }
            line.append((char) character);
        }
        if (character == -1 && line.isEmpty()) {
            return null;
        }
        String result = line.toString();
        if (result.getBytes(StandardCharsets.UTF_8).length > MAX_FRAME_BYTES) {
            throw new IllegalArgumentException("Java bridge frame exceeds the 1 MiB limit");
        }
        return result;
    }

    void accept(String line) {
        Object id = null;
        try {
            Map<String, Object> request = object(Json.parse(line));
            id = request.get("id");
            send(result(id, true, encode(dispatch(request), 0)));
        } catch (Throwable error) {
            while (error instanceof InvocationTargetException && error.getCause() != null) {
                error = error.getCause();
            }
            send(result(id, false, error.getClass().getSimpleName() + ": " + error.getMessage()));
        }
    }

    private Object callback(String operation, String target, String reference, String member, Object[] arguments) throws Exception {
        if (Thread.currentThread() != workerThread) {
            throw new IllegalStateException("Z# callbacks must run on the thread handling the current foreign call");
        }
        long id = -nextCallback++;
        Map<String, Object> request = new LinkedHashMap<>();
        request.put("id", id);
        request.put("op", operation);
        if (target != null) {
            request.put("target", target);
        }
        if (reference != null) {
            request.put("ref", reference);
            request.put("member", member);
        }
        List<Object> encoded = new ArrayList<>();
        for (Object argument : arguments) {
            encoded.add(encode(argument, 0));
        }
        request.put("args", encoded);
        send(request);
        String line;
        while ((line = readFrame()) != null) {
            Map<String, Object> response = object(Json.parse(line));
            if ("result".equals(text(response, "op")) && response.get("id") instanceof BigDecimal receivedId
                && receivedId.compareTo(BigDecimal.valueOf(id)) == 0) {
                if (!Boolean.TRUE.equals(response.get("ok"))) {
                    throw new IllegalStateException("Z# callback failed: " + text(response, "error"));
                }
                return decode(response.get("value"), 0);
            }
            if (!"result".equals(text(response, "op"))) {
                send(result(response.get("id"), false, "Reentrant call to the active Java worker is not supported"));
            } else {
                throw new IllegalStateException("Unexpected ZVM bridge callback response");
            }
        }
        throw new IllegalStateException("ZVM bridge connection closed during callback");
    }

    private Module module(String filename) throws Exception {
        if (filename.isBlank()) {
            throw new IllegalArgumentException("Java module path is required");
        }
        Path path = Path.of(filename).toAbsolutePath().normalize();
        Module existing = modules.get(path.toString());
        if (existing != null) {
            return existing;
        }
        TreeSet<String> classes = new TreeSet<>();
        try (JarFile jar = new JarFile(path.toFile())) {
            jar.stream().filter(entry -> !entry.isDirectory() && entry.getName().endsWith(".class"))
                .filter(entry -> !entry.getName().startsWith("META-INF/") && !entry.getName().equals("module-info.class"))
                .forEach(entry -> classes.add(entry.getName().substring(0, entry.getName().length() - 6).replace('/', '.')));
        }
        URLClassLoader loader = new URLClassLoader(new java.net.URL[] {path.toUri().toURL()}, Worker.class.getClassLoader());
        Module loaded = new Module(path, loader, classes);
        modules.put(path.toString(), loaded);
        return loaded;
    }

    private Class<?> exportedClass(Module module, String name) throws Exception {
        if (!module.classes.contains(name)) {
            List<String> candidates = new ArrayList<>();
            for (String candidate : module.classes) {
                if (candidate.endsWith("." + name) || candidate.endsWith("$" + name)) {
                    candidates.add(candidate);
                }
            }
            if (candidates.size() != 1) {
                throw new IllegalArgumentException(candidates.isEmpty()
                    ? "Class '" + name + "' is not part of the imported Java module"
                    : "Multiple Java classes are named '" + name + "'; use a qualified target");
            }
            name = candidates.get(0);
        }
        Class<?> type = Class.forName(name, false, module.loader);
        if (!Modifier.isPublic(type.getModifiers())) {
            throw new IllegalArgumentException("Java class '" + name + "' is not public");
        }
        return type;
    }

    private Class<?> defaultClass(Module module) throws Exception {
        String name = module.path.getFileName().toString().replaceFirst("(?i)(\\.zjava)?\\.jar$", "");
        List<Class<?>> candidates = new ArrayList<>();
        List<Class<?>> named = new ArrayList<>();
        for (String candidate : module.classes) {
            Class<?> type = Class.forName(candidate, false, module.loader);
            if (!Modifier.isPublic(type.getModifiers())) {
                continue;
            }
            if (type.getSimpleName().equals(name)) {
                named.add(type);
            }
            if (type.isAnnotationPresent(ZSharp_Export.class)) {
                candidates.add(type);
            }
        }
        if (named.size() == 1) {
            return named.get(0);
        }
        if (named.size() > 1) {
            throw new IllegalArgumentException("Java module contains multiple classes named '" + name + "'; use a qualified target");
        }
        if (candidates.size() == 1) {
            return candidates.get(0);
        }
        throw new IllegalArgumentException("Java target requires an explicit exported class name");
    }

    private static boolean exported(java.lang.reflect.Member member) {
        if (!Modifier.isPublic(member.getModifiers()) || !Modifier.isPublic(member.getDeclaringClass().getModifiers())) {
            return false;
        }
        return ((java.lang.reflect.AnnotatedElement) member).isAnnotationPresent(ZSharp_Export.class)
            || member.getDeclaringClass().isAnnotationPresent(ZSharp_Export.class);
    }

    Object dispatch(Map<String, Object> request) throws Exception {
        String operation = text(request, "op");
        String reference = text(request, "ref");
        Object instance = null;
        if (!reference.isEmpty()) {
            instance = objects.get(reference);
            if (instance == null) {
                throw new IllegalArgumentException("Java object reference is invalid or has been released: " + reference);
            }
        }
        if (operation.equals("release")) {
            if (instance == null) {
                throw new IllegalArgumentException("Java release requires a live object reference");
            }
            objects.remove(reference);
            identities.remove(instance);
            return null;
        }
        Module module = instance == null ? module(text(request, "module")) : null;
        String target = text(request, "target");
        String member = text(request, "member");
        Class<?> type;
        if (instance != null) {
            type = instance.getClass();
        } else if (operation.equals("new")) {
            type = target.isEmpty() ? defaultClass(module) : exportedClass(module, target);
        } else {
            if (member.isEmpty()) {
                int separator = target.lastIndexOf('.');
                if (separator >= 0) {
                    member = target.substring(separator + 1);
                    target = target.substring(0, separator);
                } else {
                    member = target;
                    target = "";
                }
            }
            type = target.isEmpty() ? defaultClass(module) : exportedClass(module, target);
        }
        Object rawArguments = request.get("args");
        if (rawArguments != null && !(rawArguments instanceof List<?>)) {
            throw new IllegalArgumentException("Java arguments must be an array");
        }
        List<?> raw = rawArguments == null ? List.of() : (List<?>) rawArguments;
        Object[] arguments = new Object[raw.size()];
        for (int i = 0; i < raw.size(); i++) {
            arguments[i] = decode(raw.get(i), 0);
        }
        if (operation.equals("new")) {
            List<Executable> candidates = new ArrayList<>();
            for (Constructor<?> constructor : type.getConstructors()) {
                if (exported(constructor)) {
                    candidates.add(constructor);
                }
            }
            Match match = match(candidates, arguments, type.getName() + " constructor");
            return ((Constructor<?>) match.executable).newInstance(match.arguments);
        }
        if (operation.equals("get") || operation.equals("set")) {
            Field field = type.getField(member);
            if (!exported(field) || (instance == null && !Modifier.isStatic(field.getModifiers()))) {
                throw new IllegalArgumentException("Java field is not exported: " + member);
            }
            if (operation.equals("get")) {
                if (arguments.length != 0) {
                    throw new IllegalArgumentException("Java field get takes no arguments");
                }
                return field.get(instance);
            }
            if (arguments.length != 1 || Modifier.isFinal(field.getModifiers())) {
                throw new IllegalArgumentException("Java field set requires one value and a writable exported field");
            }
            field.set(instance, convert(arguments[0], field.getType()).value);
            return null;
        }
        if (!operation.equals("call") && !operation.equals("invoke")) {
            throw new IllegalArgumentException("Unknown Java bridge operation: " + operation);
        }
        List<Executable> candidates = new ArrayList<>();
        for (Method method : type.getMethods()) {
            if (method.getName().equals(member) && exported(method) && !method.isBridge() && !method.isSynthetic()
                && (instance != null || Modifier.isStatic(method.getModifiers()))) {
                candidates.add(method);
            }
        }
        Match match = match(candidates, arguments, type.getName() + "." + member);
        return ((Method) match.executable).invoke(instance, match.arguments);
    }

    private static Match match(List<Executable> candidates, Object[] arguments, String target) {
        Match best = null;
        boolean ambiguous = false;
        for (Executable candidate : candidates) {
            if (candidate.isVarArgs() || candidate.getParameterCount() != arguments.length) {
                continue;
            }
            try {
                Object[] converted = new Object[arguments.length];
                int score = 0;
                Class<?>[] types = candidate.getParameterTypes();
                for (int i = 0; i < arguments.length; i++) {
                    Conversion conversion = convert(arguments[i], types[i]);
                    converted[i] = conversion.value;
                    score += conversion.score;
                }
                if (best == null || score < best.score) {
                    best = new Match(candidate, converted, score);
                    ambiguous = false;
                } else if (score == best.score) {
                    ambiguous = true;
                }
            } catch (IllegalArgumentException ignored) {
                // Another public overload may accept the supplied values.
            }
        }
        if (best == null) {
            throw new IllegalArgumentException("No exported Java overload accepts these arguments: " + target);
        }
        if (ambiguous) {
            throw new IllegalArgumentException("Ambiguous exported Java overload: " + target);
        }
        return best;
    }

    private static Conversion convert(Object value, Class<?> type) {
        if (value == null) {
            if (type.isPrimitive()) {
                throw new IllegalArgumentException("Null cannot be passed to a primitive Java parameter");
            }
            return new Conversion(null, 1);
        }
        if (type == Object.class) {
            return new Conversion(value, 20);
        }
        if (value instanceof BigDecimal number) {
            try {
                if (type == BigDecimal.class) return new Conversion(number, 0);
                if (type == BigInteger.class) return new Conversion(number.toBigIntegerExact(), 1);
                if (type == int.class || type == Integer.class) return new Conversion(number.intValueExact(), 1);
                if (type == long.class || type == Long.class) return new Conversion(number.longValueExact(), 2);
                if (type == short.class || type == Short.class) return new Conversion(number.shortValueExact(), 3);
                if (type == byte.class || type == Byte.class) return new Conversion(number.byteValueExact(), 4);
                if ((type == double.class || type == Double.class) && Double.isFinite(number.doubleValue())) return new Conversion(number.doubleValue(), 5);
                if ((type == float.class || type == Float.class) && Float.isFinite(number.floatValue())) return new Conversion(number.floatValue(), 6);
            } catch (ArithmeticException invalid) {
                throw new IllegalArgumentException("Number is outside the Java parameter's range", invalid);
            }
        }
        if ((type == boolean.class || type == Boolean.class) && value instanceof Boolean) {
            return new Conversion(value, 0);
        }
        if ((type == char.class || type == Character.class) && value instanceof String text && text.length() == 1) {
            return new Conversion(text.charAt(0), 1);
        }
        if (type.isInstance(value)) {
            return new Conversion(value, 0);
        }
        if (type.isArray() && value instanceof List<?> list) {
            Object array = Array.newInstance(type.componentType(), list.size());
            int score = 1;
            for (int i = 0; i < list.size(); i++) {
                Conversion element = convert(list.get(i), type.componentType());
                Array.set(array, i, element.value);
                score += element.score;
            }
            return new Conversion(array, score);
        }
        throw new IllegalArgumentException("Bridge value cannot be converted to " + type.getName());
    }

    Object decode(Object value, int depth) {
        if (depth > 128) {
            throw new IllegalArgumentException("Bridge value nesting limit exceeded");
        }
        if (value instanceof Map<?, ?> map) {
            if (map.containsKey("$number")) {
                Object number = map.get("$number");
                if (!(number instanceof String)) {
                    throw new IllegalArgumentException("$number must contain decimal text");
                }
                if (!((String) number).matches("-?(?:0|[1-9][0-9]*)(?:\\.[0-9]+)?(?:[eE][+-]?[0-9]+)?")) {
                    throw new IllegalArgumentException("$number must contain valid decimal text");
                }
                return new BigDecimal((String) number);
            }
            if (map.containsKey("$ref")) {
                Object result = objects.get(map.get("$ref"));
                if (result == null) {
                    throw new IllegalArgumentException("Java object reference is invalid or has been released");
                }
                return result;
            }
            if (map.containsKey("$zref")) {
                return new ZSharpObject(map.get("$zref").toString(), String.valueOf(map.get("$type")));
            }
            if (map.containsKey("$record")) {
                Map<String, Object> result = new LinkedHashMap<>();
                for (Map.Entry<String, Object> entry : object(map.get("$record")).entrySet()) {
                    result.put(entry.getKey(), decode(entry.getValue(), depth + 1));
                }
                return result;
            }
            throw new IllegalArgumentException("Unknown Java bridge value tag");
        }
        if (value instanceof List<?> list) {
            List<Object> result = new ArrayList<>();
            for (Object element : list) {
                result.add(decode(element, depth + 1));
            }
            return result;
        }
        return value;
    }

    Object encode(Object value, int depth) {
        if (depth > 128) {
            throw new IllegalArgumentException("Bridge value nesting limit exceeded");
        }
        if (value == null || value instanceof String || value instanceof Boolean) {
            return value;
        }
        if (value instanceof Character character) {
            return character.toString();
        }
        if (value instanceof Number number) {
            if ((number instanceof Double && !Double.isFinite(number.doubleValue()))
                || (number instanceof Float && !Float.isFinite(number.floatValue()))) {
                throw new IllegalArgumentException("Non-finite Java numbers cannot cross the bridge");
            }
            return Map.of("$number", number.toString());
        }
        if (value instanceof ZSharpObject reference) {
            return Map.of("$zref", reference.reference, "$type", reference.type);
        }
        if (value.getClass().isArray()) {
            List<Object> result = new ArrayList<>();
            for (int i = 0; i < Array.getLength(value); i++) {
                result.add(encode(Array.get(value, i), depth + 1));
            }
            return result;
        }
        if (value instanceof List<?> list) {
            List<Object> result = new ArrayList<>();
            for (Object element : list) {
                result.add(encode(element, depth + 1));
            }
            return result;
        }
        if (value instanceof Map<?, ?> map) {
            Map<String, Object> result = new LinkedHashMap<>();
            for (Map.Entry<?, ?> entry : map.entrySet()) {
                if (!(entry.getKey() instanceof String)) {
                    throw new IllegalArgumentException("Java records require text keys");
                }
                result.put((String) entry.getKey(), encode(entry.getValue(), depth + 1));
            }
            return Map.of("$record", result);
        }
        String reference = identities.get(value);
        if (reference == null) {
            reference = Long.toString(nextObject++);
            identities.put(value, reference);
            objects.put(reference, value);
        }
        return Map.of("$ref", reference, "$type", value.getClass().getName());
    }
}
