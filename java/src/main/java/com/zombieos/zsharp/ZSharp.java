package com.zombieos.zsharp;

import java.nio.file.Path;
import java.util.Locale;
import java.util.Objects;

/** Entry point for embedding the Z# toolchain in a Java application. */
public final class ZSharp {
    public static final String VERSION = "1.0.0.0";
    public static final String SOURCE_EXTENSION = ".zsharp";

    private ZSharp() {
    }

    /** Locates the Z# executable from ZSHARP_BIN, ZSHARP_HOME, or PATH. */
    public static ZSharpToolchain toolchain() {
        return ZSharpToolchain.discover();
    }

    /** Creates a toolchain backed by a specific Z# executable. */
    public static ZSharpToolchain toolchain(Path executable) {
        return new ZSharpToolchain(executable);
    }

    public static boolean isSourceFile(Path path) {
        Objects.requireNonNull(path, "path");
        return path.getFileName().toString().toLowerCase(Locale.ROOT)
                .endsWith(SOURCE_EXTENSION);
    }
}
