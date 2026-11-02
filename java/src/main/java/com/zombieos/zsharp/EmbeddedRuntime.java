package com.zombieos.zsharp;

import java.io.IOException;
import java.io.InputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.nio.file.AtomicMoveNotSupportedException;
import java.nio.file.Files;
import java.nio.file.LinkOption;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;
import java.nio.file.StandardOpenOption;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.ArrayList;
import java.util.Comparator;
import java.util.HashSet;
import java.util.HexFormat;
import java.util.List;
import java.util.Locale;
import java.util.Optional;
import java.util.Set;
import java.util.zip.CRC32;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

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
                prepareRuntimeSupport(platform, destination.getParent());
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
                prepareRuntimeSupport(platform, destination.getParent());
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

    private static void prepareRuntimeSupport(String platform, Path directory)
            throws IOException {
        prepareSupportFile(platform, directory, "python-runtime.tar.gz");
        Path interop = prepareSupportFile(platform, directory,
                "interop-runtime.zip");
        extractInteropArchive(interop, directory);
        if (platform.startsWith("macos-")) {
            prepareSupportFile(platform, directory, "libMoltenVK.dylib");
        }
    }

    private static Path prepareSupportFile(String platform, Path directory,
                                          String name) throws IOException {
        String resource = RESOURCE_ROOT + platform + "/" + name;
        String checksumResource = resource + ".sha256";
        String expectedHash;
        try (InputStream checksumInput = open(checksumResource)) {
            if (checksumInput == null) {
                throw new IOException("embedded " + name + " checksum is missing");
            }
            expectedHash = new String(checksumInput.readAllBytes(),
                    StandardCharsets.UTF_8).trim().toLowerCase(Locale.ROOT);
        }
        if (!expectedHash.matches("[0-9a-f]{64}")) {
            throw new IOException("invalid embedded " + name + " checksum");
        }
        Path destination = directory.resolve(name);
        if (Files.isRegularFile(destination, LinkOption.NOFOLLOW_LINKS)
                && expectedHash.equals(sha256(destination))) {
            return destination;
        }
        if (Files.exists(destination, LinkOption.NOFOLLOW_LINKS)
                && !Files.isRegularFile(destination, LinkOption.NOFOLLOW_LINKS)) {
            throw new IOException("embedded support destination is not a regular file: "
                    + name);
        }
        Path temporary = Files.createTempFile(directory, name + ".", ".tmp");
        try {
            try (InputStream supportInput = open(resource)) {
                if (supportInput == null) {
                    throw new IOException("embedded " + name + " is missing");
                }
                Files.copy(supportInput, temporary,
                        StandardCopyOption.REPLACE_EXISTING);
            }
            if (!expectedHash.equals(sha256(temporary))) {
                throw new IOException("embedded " + name + " checksum mismatch");
            }
            moveIntoPlace(temporary, destination);
        } finally {
            Files.deleteIfExists(temporary);
        }
        return destination;
    }

    private record InteropMember(String name, long size, long crc) {
    }

    /** Validates the complete ZIP index before creating any helper files. */
    static void extractInteropArchive(Path archive, Path directory)
            throws IOException {
        if (Files.size(archive) > 64L * 1024 * 1024) {
            throw new IOException("embedded interop archive exceeds its size limit");
        }
        List<InteropMember> members = interopMembers(Files.readAllBytes(archive));
        Path stage = Files.createTempDirectory(directory, ".interop-stage-");
        try {
            try (ZipFile zip = new ZipFile(archive.toFile())) {
                byte[] buffer = new byte[8192];
                for (InteropMember member : members) {
                    ZipEntry entry = zip.getEntry(member.name());
                    if (entry == null || entry.getMethod() != ZipEntry.STORED
                            || entry.getSize() != member.size()) {
                        throw new IOException("invalid embedded interop member");
                    }
                    Path target = stage.resolve(member.name());
                    Files.createDirectories(target.getParent());
                    CRC32 crc = new CRC32();
                    long total = 0;
                    try (InputStream input = zip.getInputStream(entry);
                         var output = Files.newOutputStream(target,
                                 StandardOpenOption.CREATE_NEW)) {
                        int read;
                        while ((read = input.read(buffer)) != -1) {
                            total += read;
                            if (total > member.size()) {
                                throw new IOException("embedded interop member exceeds its size");
                            }
                            output.write(buffer, 0, read);
                            crc.update(buffer, 0, read);
                        }
                    }
                    if (total != member.size() || crc.getValue() != member.crc()) {
                        throw new IOException("embedded interop member checksum mismatch");
                    }
                }
            }
            for (InteropMember member : members) {
                Path source = stage.resolve(member.name());
                Path destination = directory.resolve(member.name());
                createHelperDirectories(directory, destination.getParent());
                if (Files.exists(destination, LinkOption.NOFOLLOW_LINKS)
                        && !Files.isRegularFile(destination, LinkOption.NOFOLLOW_LINKS)) {
                    throw new IOException("embedded helper destination is not a regular file");
                }
                if (!Files.isRegularFile(destination, LinkOption.NOFOLLOW_LINKS)
                        || !sha256(source).equals(sha256(destination))) {
                    moveIntoPlace(source, destination);
                }
            }
        } finally {
            try (var paths = Files.walk(stage)) {
                for (Path path : paths.sorted(Comparator.reverseOrder()).toList()) {
                    Files.deleteIfExists(path);
                }
            }
        }
    }

    private static void createHelperDirectories(Path root, Path directory)
            throws IOException {
        if (!Files.isDirectory(root, LinkOption.NOFOLLOW_LINKS)) {
            throw new IOException("embedded runtime cache is not a directory");
        }
        Path current = root;
        for (Path part : root.relativize(directory)) {
            current = current.resolve(part);
            if (!Files.exists(current, LinkOption.NOFOLLOW_LINKS)) {
                try {
                    Files.createDirectory(current);
                } catch (java.nio.file.FileAlreadyExistsException ignored) {
                    // Another JVM may have created the same cache directory.
                }
            }
            if (!Files.isDirectory(current, LinkOption.NOFOLLOW_LINKS)) {
                throw new IOException("embedded helper parent is not a directory");
            }
        }
    }

    private static List<InteropMember> interopMembers(byte[] bytes)
            throws IOException {
        ByteBuffer zip = ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN);
        int end = bytes.length - 22;
        for (; end >= Math.max(0, bytes.length - 65557); end--) {
            if (zip.getInt(end) == 0x06054b50
                    && end + 22 + unsignedShort(zip, end + 20) == bytes.length) {
                break;
            }
        }
        if (end < Math.max(0, bytes.length - 65557)
                || unsignedShort(zip, end + 4) != 0
                || unsignedShort(zip, end + 6) != 0
                || unsignedShort(zip, end + 8) != unsignedShort(zip, end + 10)) {
            throw new IOException("invalid embedded interop ZIP index");
        }
        int count = unsignedShort(zip, end + 10);
        long centralSize = unsignedInt(zip, end + 12);
        long centralOffset = unsignedInt(zip, end + 16);
        if (count == 0 || count > 4096 || centralOffset + centralSize != end) {
            throw new IOException("invalid embedded interop ZIP bounds");
        }
        List<InteropMember> members = new ArrayList<>();
        Set<String> names = new HashSet<>();
        Set<String> parents = new HashSet<>();
        int cursor = (int) centralOffset;
        long total = 0;
        for (int index = 0; index < count; index++) {
            if (cursor < 0 || cursor > end - 46 || zip.getInt(cursor) != 0x02014b50) {
                throw new IOException("invalid embedded interop ZIP entry");
            }
            int flags = unsignedShort(zip, cursor + 8);
            int nameSize = unsignedShort(zip, cursor + 28);
            int extra = unsignedShort(zip, cursor + 30);
            int comment = unsignedShort(zip, cursor + 32);
            long size = unsignedInt(zip, cursor + 24);
            long attributes = unsignedInt(zip, cursor + 38);
            int mode = (int) (attributes >> 16) & 0170000;
            long localOffset = unsignedInt(zip, cursor + 42);
            if ((flags & ~0x0808) != 0 || unsignedShort(zip, cursor + 10) != 0
                    || unsignedInt(zip, cursor + 20) != size
                    || nameSize == 0 || nameSize > 512
                    || cursor + 46L + nameSize + extra + comment > end
                    || size > 16L * 1024 * 1024
                    || (mode != 0 && mode != 0100000)
                    || (attributes & 0x10) != 0
                    || unsignedShort(zip, cursor + 34) != 0
                    || localOffset >= centralOffset
                    || (total += size) > 64L * 1024 * 1024) {
                throw new IOException("unsupported embedded interop ZIP member");
            }
            String name = new String(bytes, cursor + 46, nameSize,
                    StandardCharsets.US_ASCII);
            validateInteropName(name);
            String folded = name.toLowerCase(Locale.ROOT);
            if (!names.add(folded) || parents.contains(folded)) {
                throw new IOException("colliding embedded interop ZIP members");
            }
            for (int slash = folded.indexOf('/'); slash >= 0;
                 slash = folded.indexOf('/', slash + 1)) {
                String parent = folded.substring(0, slash);
                if (names.contains(parent)) {
                    throw new IOException("colliding embedded interop ZIP paths");
                }
                parents.add(parent);
            }
            members.add(new InteropMember(name, size,
                    unsignedInt(zip, cursor + 16)));
            cursor += 46 + nameSize + extra + comment;
        }
        if (cursor != end) {
            throw new IOException("invalid embedded interop ZIP directory size");
        }
        return members;
    }

    private static void validateInteropName(String name) throws IOException {
        if (!name.startsWith("interop/") || name.length() <= 8) {
            throw new IOException("embedded helper must be under interop/");
        }
        for (String part : name.split("/", -1)) {
            String stem = part.split("\\.", 2)[0].toUpperCase(Locale.ROOT);
            if (!part.matches("[A-Za-z0-9_.-]+") || part.equals(".")
                    || part.equals("..") || part.endsWith(".")
                    || stem.matches("CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9]")) {
                throw new IOException("unsafe embedded interop ZIP path");
            }
        }
    }

    private static int unsignedShort(ByteBuffer buffer, int offset) {
        return Short.toUnsignedInt(buffer.getShort(offset));
    }

    private static long unsignedInt(ByteBuffer buffer, int offset) {
        return Integer.toUnsignedLong(buffer.getInt(offset));
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
