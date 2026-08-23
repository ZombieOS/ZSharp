package com.zombieos.zsharp;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertNotNull;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.io.InputStream;
import java.nio.file.Files;
import java.nio.file.Path;
import java.security.MessageDigest;
import java.util.HexFormat;
import java.util.List;
import org.junit.jupiter.api.Test;

class EmbeddedRuntimeTest {
    @Test
    void recognizesSupportedOperatingSystemsAndArchitectures() {
        assertEquals("windows-x86_64",
                EmbeddedRuntime.platformId("Windows 11", "amd64"));
        assertEquals("windows-aarch64",
                EmbeddedRuntime.platformId("Windows 11", "aarch64"));
        assertEquals("linux-x86_64",
                EmbeddedRuntime.platformId("Linux", "x86_64"));
        assertEquals("linux-aarch64",
                EmbeddedRuntime.platformId("Linux", "arm64"));
        assertEquals("macos-x86_64",
                EmbeddedRuntime.platformId("Mac OS X", "x86_64"));
        assertEquals("macos-aarch64",
                EmbeddedRuntime.platformId("Darwin", "aarch64"));
    }

    @Test
    void rejectsUnsupportedOperatingSystemsAndArchitectures() {
        assertNull(EmbeddedRuntime.platformId("Linux", "x86"));
        assertNull(EmbeddedRuntime.platformId("FreeBSD", "amd64"));
    }

    @Test
    void packagesValidChecksummedRuntimesForEverySupportedPlatform()
            throws Exception {
        List<String> platforms = List.of(
                "windows-x86_64", "windows-aarch64",
                "linux-x86_64", "linux-aarch64",
                "macos-x86_64", "macos-aarch64");

        for (String platform : platforms) {
            String executableName = platform.startsWith("windows-")
                    ? "zsharp.exe"
                    : "zsharp";
            String resource = "/META-INF/zsharp/runtime/" + platform
                    + "/" + executableName;
            try (InputStream executable = EmbeddedRuntimeTest.class
                         .getResourceAsStream(resource);
                 InputStream checksum = EmbeddedRuntimeTest.class
                         .getResourceAsStream(resource + ".sha256")) {
                assertNotNull(executable, resource);
                assertNotNull(checksum, resource + ".sha256");
                String expected = new String(checksum.readAllBytes()).trim();
                String actual = HexFormat.of().formatHex(
                        MessageDigest.getInstance("SHA-256")
                                .digest(executable.readAllBytes()));
                assertEquals(expected, actual, platform);
            }
        }
    }

    @Test
    void extractsAndRunsTheCurrentPlatformsBundledRuntime() throws Exception {
        Path executable = EmbeddedRuntime.discover().orElseThrow();
        assertTrue(Files.isRegularFile(executable));

        ZSharpResult result = new ZSharpToolchain(executable).version();
        assertEquals(0, result.exitCode(), result.standardError());
        assertEquals("Z# " + ZSharp.VERSION, result.standardOutput().trim());
        assertFalse(result.standardError().contains("error"));
    }
}
