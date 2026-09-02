package com.zombieos.zsharp;

import java.nio.file.Path;
import java.util.Locale;
import java.util.Objects;

/** Entry point for embedding the Z# toolchain in a Java application. */
public final class ZSharp {
    /** Version of the Java integration and matching Z# release. */
    public static final String VERSION = "1.0.2.0";

    /** Required filename extension for Z# source files. */
    public static final String SOURCE_EXTENSION = ".zsharp";

    /** Required filename extension for Z# application packages. */
    public static final String APP_EXTENSION = ".zapp";

    /** Required filename extension for Z# game packages. */
    public static final String GAME_EXTENSION = ".zgame";

    private ZSharp() {
    }

    /**
     * Locates the Z# executable from an environment override, the bundled
     * runtime, or {@code PATH}.
     *
     * @return the discovered toolchain
     */
    public static ZSharpToolchain toolchain() {
        return ZSharpToolchain.discover();
    }

    /**
     * Creates a toolchain backed by a specific Z# executable.
     *
     * @param executable path or command used to launch Z#
     * @return a toolchain backed by {@code executable}
     */
    public static ZSharpToolchain toolchain(Path executable) {
        return new ZSharpToolchain(executable);
    }

    /**
     * Checks whether a path has the Z# source extension.
     *
     * @param path path to inspect
     * @return {@code true} when the filename ends in {@code .zsharp}
     */
    public static boolean isSourceFile(Path path) {
        Objects.requireNonNull(path, "path");
        return path.getFileName().toString().toLowerCase(Locale.ROOT)
                .endsWith(SOURCE_EXTENSION);
    }

    /**
     * Checks whether a path has a Z# application or game package extension.
     *
     * @param path path to inspect
     * @return {@code true} when the filename ends in {@code .zapp} or
     *         {@code .zgame}
     */
    public static boolean isPackageFile(Path path) {
        Objects.requireNonNull(path, "path");
        String name = path.getFileName().toString().toLowerCase(Locale.ROOT);
        return name.endsWith(APP_EXTENSION) || name.endsWith(GAME_EXTENSION);
    }
}
