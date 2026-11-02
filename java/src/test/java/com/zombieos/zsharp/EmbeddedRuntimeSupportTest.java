package com.zombieos.zsharp;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertThrows;

import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.zip.CRC32;
import java.util.zip.ZipEntry;
import java.util.zip.ZipOutputStream;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.io.TempDir;

class EmbeddedRuntimeSupportTest {
    @TempDir
    Path temporary;

    private Path archive(Map<String, String> contents) throws IOException {
        Path archive = temporary.resolve("helpers.zip");
        try (ZipOutputStream output = new ZipOutputStream(Files.newOutputStream(archive))) {
            for (var item : contents.entrySet()) {
                byte[] data = item.getValue().getBytes(StandardCharsets.UTF_8);
                CRC32 crc = new CRC32();
                crc.update(data);
                ZipEntry entry = new ZipEntry(item.getKey());
                entry.setMethod(ZipEntry.STORED);
                entry.setSize(data.length);
                entry.setCompressedSize(data.length);
                entry.setCrc(crc.getValue());
                output.putNextEntry(entry);
                output.write(data);
                output.closeEntry();
            }
        }
        return archive;
    }

    private Path destination() throws IOException {
        return Files.createDirectory(temporary.resolve("cache"));
    }

    @Test
    void extractsAndRepairsHelpersWithoutReplacingUnchangedFiles() throws Exception {
        Path archive = archive(Map.of("interop/python/zsharp_worker.py", "worker"));
        Path destination = destination();
        EmbeddedRuntime.extractInteropArchive(archive, destination);
        Path helper = destination.resolve("interop/python/zsharp_worker.py");
        assertEquals("worker", Files.readString(helper));
        var modified = Files.getLastModifiedTime(helper);
        EmbeddedRuntime.extractInteropArchive(archive, destination);
        assertEquals(modified, Files.getLastModifiedTime(helper));
        Files.writeString(helper, "tampered");
        EmbeddedRuntime.extractInteropArchive(archive, destination);
        assertEquals("worker", Files.readString(helper));
    }

    @Test
    void rejectsTraversalAbsoluteReservedAndDirectoryNames() throws Exception {
        Path destination = destination();
        for (String name : new String[]{"../escape", "interop/../escape",
                "interop/./escape", "/interop/escape", "other/escape",
                "interop\\escape", "interop/C:/escape", "interop/CON.txt",
                "interop/parent./escape", "interop/folder/"}) {
            Path archive = archive(Map.of(name, "data"));
            assertThrows(IOException.class,
                    () -> EmbeddedRuntime.extractInteropArchive(archive, destination), name);
        }
        assertFalse(Files.exists(destination.resolve("interop")));
        assertFalse(Files.exists(temporary.resolve("escape")));
    }

    @Test
    void rejectsCaseAndFileDirectoryCollisionsBeforeWriting() throws Exception {
        Path destination = destination();
        Map<String, String> entries = new LinkedHashMap<>();
        entries.put("interop/Sample.py", "one");
        entries.put("interop/sample.py", "two");
        Path archive = archive(entries);
        assertThrows(IOException.class,
                () -> EmbeddedRuntime.extractInteropArchive(archive, destination));
        entries.clear();
        entries.put("interop/parent", "one");
        entries.put("interop/parent/child.py", "two");
        Path collision = archive(entries);
        assertThrows(IOException.class,
                () -> EmbeddedRuntime.extractInteropArchive(collision, destination));
        assertFalse(Files.exists(destination.resolve("interop")));
    }

    @Test
    void rejectsSymlinkAndOversizedMetadataBeforeWriting() throws Exception {
        Path destination = destination();
        Path archive = archive(Map.of("interop/worker.py", "data"));
        byte[] data = Files.readAllBytes(archive);
        ByteBuffer bytes = ByteBuffer.wrap(data).order(ByteOrder.LITTLE_ENDIAN);
        int central = bytes.getInt(data.length - 6);
        bytes.putInt(central + 38, 0120000 << 16);
        Files.write(archive, data);
        assertThrows(IOException.class,
                () -> EmbeddedRuntime.extractInteropArchive(archive, destination));
        bytes.putInt(central + 38, 0);
        bytes.putInt(central + 20, 16 * 1024 * 1024 + 1);
        bytes.putInt(central + 24, 16 * 1024 * 1024 + 1);
        Files.write(archive, data);
        assertThrows(IOException.class,
                () -> EmbeddedRuntime.extractInteropArchive(archive, destination));
        assertFalse(Files.exists(destination.resolve("interop")));
    }

    @Test
    void rejectsCorruptContentsBeforeReplacingExistingHelpers() throws Exception {
        Path destination = destination();
        Path helper = destination.resolve("interop/worker.py");
        Files.createDirectories(helper.getParent());
        Files.writeString(helper, "previous");
        Path archive = archive(Map.of("interop/worker.py", "new"));
        byte[] data = Files.readAllBytes(archive);
        // Alter the stored payload without updating its CRC.
        ByteBuffer bytes = ByteBuffer.wrap(data).order(ByteOrder.LITTLE_ENDIAN);
        int nameLength = Short.toUnsignedInt(bytes.getShort(26));
        int extraLength = Short.toUnsignedInt(bytes.getShort(28));
        data[30 + nameLength + extraLength] ^= 1;
        Files.write(archive, data);
        assertThrows(IOException.class,
                () -> EmbeddedRuntime.extractInteropArchive(archive, destination));
        assertEquals("previous", Files.readString(helper));
    }

    @Test
    void refusesAnExistingHelperParentThatIsAFile() throws Exception {
        Path destination = destination();
        Files.writeString(destination.resolve("interop"), "user file");
        Path archive = archive(Map.of("interop/worker.py", "new"));
        assertThrows(IOException.class,
                () -> EmbeddedRuntime.extractInteropArchive(archive, destination));
        assertEquals("user file", Files.readString(destination.resolve("interop")));
    }

    @Test
    void rejectsTruncatedArchive() throws Exception {
        Path destination = destination();
        Path archive = temporary.resolve("bad.zip");
        Files.write(archive, new byte[10]);
        assertThrows(IOException.class,
                () -> EmbeddedRuntime.extractInteropArchive(archive, destination));
    }
}
