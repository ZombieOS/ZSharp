package com.zombieos.zsharp;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.nio.file.Files;
import java.nio.file.Path;
import org.junit.jupiter.api.Assumptions;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.io.TempDir;

class MavenProjectLayoutTest {
    @TempDir Path project;

    private Path projectFile(String relative) throws Exception {
        Path file = project.resolve(relative);
        Files.createDirectories(file.getParent());
        Files.writeString(file, "");
        return file;
    }

    @Test
    void infersSharedPackageAcrossStandardLanguageSourceRoots() throws Exception {
        projectFile("project.zsettings");
        Path source = projectFile("src/main/zsharp/com/user/project/Main.zsharp");
        projectFile("src/main/java/com/user/project/Java.java");
        projectFile("src/main/kotlin/com/user/project/Kotlin.kt");
        var scope = MavenProjectLayout.scope(source, null).orElseThrow();
        assertEquals(project.toRealPath(), scope.projectRoot());
        for (String language : new String[] {"java", "kotlin", "zsharp"}) {
            assertEquals(project.resolve("src/main/" + language + "/com/user/project").toRealPath().toString(),
                    scope.environment().get("ZSHARP_MAVEN_" + language.toUpperCase() + "_ROOT"));
        }
    }

    @Test
    void ordinaryProjectsKeepTheirExistingImportBehavior() throws Exception {
        projectFile("project.zsettings");
        Path source = projectFile("Scripts/Main.zsharp");
        assertTrue(MavenProjectLayout.scope(source, null).isEmpty());
    }

    @Test
    void infersTheSamePackageWhenTheProjectIsOpenedThroughAnAlias() throws Exception {
        projectFile("project.zsettings");
        Path source = projectFile("src/main/zsharp/com/user/project/Main.zsharp");
        Path alias = project.resolve("alias");
        try {
            Files.createSymbolicLink(alias, project);
        } catch (UnsupportedOperationException | java.nio.file.FileSystemException exception) {
            Assumptions.assumeTrue(false, "Host cannot create the project alias fixture");
        }
        Path throughAlias = alias.resolve(project.relativize(source));
        var actual = MavenProjectLayout.scope(throughAlias, null).orElseThrow();
        var expected = MavenProjectLayout.scope(source, null).orElseThrow();
        assertEquals(expected, actual);
    }

    @Test
    void standaloneSourceWithoutProjectSettingsIsUnchanged() throws Exception {
        Path source = projectFile("src/main/zsharp/com/user/project/Main.zsharp");
        assertTrue(MavenProjectLayout.scope(source, null).isEmpty());
    }

    @Test
    void explicitBasePackageSupportsEntryFilesOutsideThePackageTree() throws Exception {
        projectFile("project.zsettings");
        Path source = projectFile("Main.zsharp");
        projectFile("src/main/java/com/user/project/Java.java");
        var scope = MavenProjectLayout.scope(source, "com.user.project").orElseThrow();
        assertEquals(project.resolve("src/main/zsharp/com/user/project").toString(),
                scope.environment().get("ZSHARP_MAVEN_ZSHARP_ROOT"));
    }

    @Test
    void includesCompiledMavenClassesInTheWorkerClasspath() throws Exception {
        projectFile("project.zsettings");
        Path source = projectFile("src/main/zsharp/com/user/project/Main.zsharp");
        Files.createDirectories(project.resolve("target/classes"));
        String classpath = MavenProjectLayout.scope(source, null).orElseThrow()
                .environment().get("ZSHARP_JVM_CLASSPATH");
        assertTrue(classpath.contains(project.resolve("target/classes").toString()));
    }

    @Test
    void rejectsInvalidAndTraversingPackageNames() {
        for (String invalid : new String[] {"..", "com..project", "com/project", "com.1project", "com.project-2"}) {
            assertThrows(IllegalArgumentException.class,
                    () -> MavenProjectLayout.validatePackage(invalid));
        }
        assertEquals("com.user.project", MavenProjectLayout.validatePackage("com.user.project"));
        assertEquals("", MavenProjectLayout.validatePackage(""));
    }

    @Test
    void packageConfigurationDoesNotModifyTheOriginalToolchain() {
        ZSharpToolchain original = new ZSharpToolchain(Path.of("zsharp"));
        ZSharpToolchain selected = original.withBasePackage("com.user.project");
        assertFalse(original == selected);
        assertEquals(original.executable(), selected.executable());
    }
}
