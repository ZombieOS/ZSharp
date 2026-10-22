package ZSharp;

import java.math.BigDecimal;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/** Small, strict JSON codec; the SDK has no third-party dependencies. */
final class Json {
    private final String source;
    private int offset;

    private Json(String source) {
        this.source = source;
    }

    static Object parse(String source) {
        Json reader = new Json(source);
        Object value = reader.value(0);
        reader.space();
        if (reader.offset != source.length()) {
            throw reader.error("trailing data");
        }
        return value;
    }

    private IllegalArgumentException error(String message) {
        return new IllegalArgumentException("Invalid bridge JSON at " + offset + ": " + message);
    }

    private void space() {
        while (offset < source.length() && " \t\r\n".indexOf(source.charAt(offset)) >= 0) {
            offset++;
        }
    }

    private boolean take(char expected) {
        space();
        if (offset < source.length() && source.charAt(offset) == expected) {
            offset++;
            return true;
        }
        return false;
    }

    private Object value(int depth) {
        if (depth > 128) {
            throw error("nesting limit exceeded");
        }
        space();
        if (offset >= source.length()) {
            throw error("expected value");
        }
        char next = source.charAt(offset);
        if (next == '"') {
            return string();
        }
        if (take('{')) {
            Map<String, Object> result = new LinkedHashMap<>();
            if (take('}')) {
                return result;
            }
            do {
                space();
                if (offset >= source.length() || source.charAt(offset) != '"') {
                    throw error("expected object key");
                }
                String key = string();
                if (!take(':')) {
                    throw error("expected ':'");
                }
                if (result.containsKey(key)) {
                    throw error("duplicate object key");
                }
                result.put(key, value(depth + 1));
            } while (take(','));
            if (!take('}')) {
                throw error("expected '}'");
            }
            return result;
        }
        if (take('[')) {
            List<Object> result = new ArrayList<>();
            if (take(']')) {
                return result;
            }
            do {
                result.add(value(depth + 1));
            } while (take(','));
            if (!take(']')) {
                throw error("expected ']' ");
            }
            return result;
        }
        for (String literal : new String[] {"true", "false", "null"}) {
            if (source.startsWith(literal, offset)) {
                offset += literal.length();
                return literal.equals("null") ? null : literal.equals("true");
            }
        }
        int begin = offset;
        if (source.charAt(offset) == '-') {
            offset++;
        }
        if (offset >= source.length()) {
            throw error("expected number");
        }
        if (source.charAt(offset) == '0') {
            offset++;
        } else {
            int digits = offset;
            while (offset < source.length() && source.charAt(offset) >= '0' && source.charAt(offset) <= '9') {
                offset++;
            }
            if (digits == offset) {
                throw error("expected number");
            }
        }
        if (offset < source.length() && source.charAt(offset) == '.') {
            offset++;
            int digits = offset;
            while (offset < source.length() && Character.isDigit(source.charAt(offset))) {
                offset++;
            }
            if (digits == offset) {
                throw error("expected fraction digits");
            }
        }
        if (offset < source.length() && "eE".indexOf(source.charAt(offset)) >= 0) {
            offset++;
            if (offset < source.length() && "+-".indexOf(source.charAt(offset)) >= 0) {
                offset++;
            }
            int digits = offset;
            while (offset < source.length() && Character.isDigit(source.charAt(offset))) {
                offset++;
            }
            if (digits == offset) {
                throw error("expected exponent digits");
            }
        }
        try {
            return new BigDecimal(source.substring(begin, offset));
        } catch (NumberFormatException invalid) {
            throw error("invalid number");
        }
    }

    private String string() {
        offset++;
        StringBuilder result = new StringBuilder();
        while (offset < source.length()) {
            char next = source.charAt(offset++);
            if (next == '"') {
                return result.toString();
            }
            if (next < 0x20) {
                throw error("control character in string");
            }
            if (next != '\\') {
                result.append(next);
                continue;
            }
            if (offset >= source.length()) {
                throw error("unterminated escape");
            }
            char escape = source.charAt(offset++);
            switch (escape) {
                case '"', '\\', '/' -> result.append(escape);
                case 'b' -> result.append('\b');
                case 'f' -> result.append('\f');
                case 'n' -> result.append('\n');
                case 'r' -> result.append('\r');
                case 't' -> result.append('\t');
                case 'u' -> {
                    if (offset + 4 > source.length()) {
                        throw error("incomplete Unicode escape");
                    }
                    try {
                        result.append((char) Integer.parseInt(source.substring(offset, offset + 4), 16));
                    } catch (NumberFormatException invalid) {
                        throw error("invalid Unicode escape");
                    }
                    offset += 4;
                }
                default -> throw error("invalid escape");
            }
        }
        throw error("unterminated string");
    }

    static String write(Object value) {
        StringBuilder output = new StringBuilder();
        write(value, output, 0);
        return output.toString();
    }

    private static void write(Object value, StringBuilder output, int depth) {
        if (depth > 128) {
            throw new IllegalArgumentException("Bridge JSON nesting limit exceeded");
        }
        if (value == null || value instanceof Boolean || value instanceof Number) {
            output.append(value);
        } else if (value instanceof String text) {
            output.append('"');
            for (int i = 0; i < text.length(); i++) {
                char character = text.charAt(i);
                switch (character) {
                    case '"' -> output.append("\\\"");
                    case '\\' -> output.append("\\\\");
                    case '\n' -> output.append("\\n");
                    case '\r' -> output.append("\\r");
                    case '\t' -> output.append("\\t");
                    default -> {
                        if (character < 0x20) {
                            output.append(String.format("\\u%04x", (int) character));
                        } else {
                            output.append(character);
                        }
                    }
                }
            }
            output.append('"');
        } else if (value instanceof Map<?, ?> map) {
            output.append('{');
            boolean comma = false;
            for (Map.Entry<?, ?> entry : map.entrySet()) {
                if (!(entry.getKey() instanceof String)) {
                    throw new IllegalArgumentException("Bridge JSON object keys must be strings");
                }
                if (comma) {
                    output.append(',');
                }
                write(entry.getKey(), output, depth + 1);
                output.append(':');
                write(entry.getValue(), output, depth + 1);
                comma = true;
            }
            output.append('}');
        } else if (value instanceof Iterable<?> list) {
            output.append('[');
            boolean comma = false;
            for (Object element : list) {
                if (comma) {
                    output.append(',');
                }
                write(element, output, depth + 1);
                comma = true;
            }
            output.append(']');
        } else {
            throw new IllegalArgumentException("Value is not JSON serializable: " + value.getClass().getName());
        }
    }
}
