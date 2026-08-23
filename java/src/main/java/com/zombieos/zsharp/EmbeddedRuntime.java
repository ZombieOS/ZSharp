package com.zombieos.zsharp;

import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.AtomicMoveNotSupportedException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.HexFormat;
import java.util.Locale;
import java.util.Optional;

/** Extracts a packaged native runtime when one exists for the current system. */
final class EmbeddedRuntime {
    private static final String RESOURCE_ROOT = "/META-INF/zsharp/runtime/";

    private EmbeddedRuntime() {
    }

    static synchronized Optional<Path> discover() {
        String platform = platformId();
        if (platform == null) {
            return Optional.empty();
        }

        String executableName = platform.startsWith("windows-")
                ? "zsharp.exe"
                : "zsharp";
        String resource = RESOURCE_ROOT + platform + "/" + executableName;
        String checksumResource = resource + ".sha256";

        try (InputStream checksumInput = open(checksumResource)) {
            if (checksumInput == null) {
                return Optional.empty();
            }

            String expectedHash = new String(
                    checksumInput.readAllBytes(), StandardCharsets.UTF_8)
                    .trim()
                    .toLowerCase(Locale.ROOT);
            if (!expectedHash.matches("[0-9a-f]{64}")) {
                throw new IOException("invalid embedded runtime checksum");
            }

            Path destination = cacheRoot(platform)
                    .resolve(ZSharp.VERSION)
                    .resolve(platform)
                    .resolve(expectedHash.substring(0, 16))
                    .resolve(executableName);
            if (Files.isRegularFile(destination)
                    && expectedHash.equals(sha256(destination))) {
                return Optional.of(destination);
            }

            Files.createDirectories(destination.getParent());
            Path temporary = Files.createTempFile(
                    destination.getParent(), executableName + ".", ".tmp");
            try {
                try (InputStream executableInput = open(resource)) {
                    if (executableInput == null) {
                        throw new IOException("embedded runtime is missing");
                    }
                    Files.copy(executableInput, temporary,
                            StandardCopyOption.REPLACE_EXISTING);
                }

                if (!expectedHash.equals(sha256(temporary))) {
                    throw new IOException("embedded runtime checksum mismatch");
                }
                if (!platform.startsWith("windows-")
                        && !temporary.toFile().setExecutable(true, true)) {
                    throw new IOException("could not make embedded runtime executable");
                }

                moveIntoPlace(temporary, destination);
                return Optional.of(destination);
            } finally {
                Files.deleteIfExists(temporary);
            }
        } catch (IOException exception) {
            throw new IllegalStateException(
                    "Unable to prepare the bundled Z# runtime for " + platform,
                    exception);
        }
    }

    private static InputStream open(String resource) {
        return EmbeddedRuntime.class.getResourceAsStream(resource);
    }

    private static Path cacheRoot(String platform) {
        if (platform.startsWith("windows-")) {
            String localAppData = System.getenv("LOCALAPPDATA");
            if (localAppData != null && !localAppData.isBlank()) {
                return Path.of(localAppData, "ZombieOS", "ZSharp", "runtime");
            }
        }

        String userHome = System.getProperty("user.home");
        if (userHome == null || userHome.isBlank()) {
            throw new IllegalStateException(
                    "Cannot locate a user cache for the bundled Z# runtime");
        }
        if (platform.startsWith("macos-")) {
            return Path.of(userHome, "Library", "Caches", "com.zombieos.zsharp",
                    "runtime");
        }

        String xdgCache = System.getenv("XDG_CACHE_HOME");
        if (xdgCache != null && !xdgCache.isBlank()) {
            return Path.of(xdgCache, "zsharp", "runtime");
        }
        return Path.of(userHome, ".cache", "zsharp", "runtime");
    }

    private static String platformId() {
        return platformId(System.getProperty("os.name", ""),
                System.getProperty("os.arch", ""));
    }

    static String platformId(String osName, String architectureName) {
        String os = osName.toLowerCase(Locale.ROOT);
        String architecture = architectureName.toLowerCase(Locale.ROOT);
        if (os.startsWith("windows")
                && (architecture.equals("amd64")
                || architecture.equals("x86_64"))) {
            return "windows-x86_64";
        }
        if (os.startsWith("windows") && isArm64(architecture)) {
            return "windows-aarch64";
        }
        if (os.contains("linux")
                && (architecture.equals("amd64")
                || architecture.equals("x86_64"))) {
            return "linux-x86_64";
        }
        if (os.contains("linux") && isArm64(architecture)) {
            return "linux-aarch64";
        }
        if ((os.contains("mac") || os.contains("darwin"))
                && (architecture.equals("amd64")
                || architecture.equals("x86_64"))) {
            return "macos-x86_64";
        }
        if ((os.contains("mac") || os.contains("darwin"))
                && isArm64(architecture)) {
            return "macos-aarch64";
        }
        return null;
    }

    private static boolean isArm64(String architecture) {
        return architecture.equals("aarch64") || architecture.equals("arm64");
    }

    private static String sha256(Path path) throws IOException {
        MessageDigest digest;
        try {
            digest = MessageDigest.getInstance("SHA-256");
        } catch (NoSuchAlgorithmException exception) {
            throw new IllegalStateException("SHA-256 is unavailable", exception);
        }

        try (InputStream input = Files.newInputStream(path)) {
            byte[] buffer = new byte[8192];
            int read;
            while ((read = input.read(buffer)) != -1) {
                digest.update(buffer, 0, read);
            }
        }
        return HexFormat.of().formatHex(digest.digest());
    }

    private static void moveIntoPlace(Path source, Path destination)
            throws IOException {
        try {
            Files.move(source, destination,
                    StandardCopyOption.ATOMIC_MOVE,
                    StandardCopyOption.REPLACE_EXISTING);
        } catch (AtomicMoveNotSupportedException exception) {
            Files.move(source, destination,
                    StandardCopyOption.REPLACE_EXISTING);
        }
    }
}
