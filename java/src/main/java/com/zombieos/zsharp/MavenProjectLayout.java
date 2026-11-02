package com.zombieos.zsharp;

import java.io.File;
import java.io.IOException;
import java.net.URISyntaxException;
import java.net.URL;
import java.net.URLClassLoader;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.Set;

/** Selects package-relative source roots for a JVM-hosted Maven project. */
final class MavenProjectLayout {
    static final List<String> ENVIRONMENT_KEYS = List.of(
            "ZSHARP_MAVEN_PROJECT_ROOT", "ZSHARP_MAVEN_JAVA_ROOT",
            "ZSHARP_MAVEN_KOTLIN_ROOT", "ZSHARP_MAVEN_ZSHARP_ROOT",
            "ZSHARP_JVM_CLASSPATH");

    record Scope(Path projectRoot, Map<String, String> environment) {
    }

    private MavenProjectLayout() {
    }

    static String validatePackage(String name) {
        if (name == null) {
            throw new NullPointerException("basePackage");
        }
        if (name.isEmpty()) {
            return name;
        }
        for (String part : name.split("\\.", -1)) {
            if (part.isEmpty() || !Character.isJavaIdentifierStart(part.charAt(0))) {
                throw new IllegalArgumentException("Invalid Java base package: " + name);
            }
            for (int index = 1; index < part.length(); index++) {
                if (!Character.isJavaIdentifierPart(part.charAt(index))) {
                    throw new IllegalArgumentException("Invalid Java base package: " + name);
                }
            }
        }
        return name;
    }

    static Optional<Scope> scope(Path context, String configuredPackage)
            throws IOException {
        if (context == null) {
            return Optional.empty();
        }
        Path absolute = context.toAbsolutePath().normalize();
        Path cursor = Files.isDirectory(absolute) ? absolute : absolute.getParent();
        while (cursor != null && !Files.isRegularFile(cursor.resolve("project.zsettings"))) {
            cursor = cursor.getParent();
        }
        if (cursor == null || !Files.isDirectory(cursor.resolve("src/main"))) {
            return Optional.empty();
        }
        Path project = cursor.toRealPath();
        Path zsharp = project.resolve("src/main/zsharp");
        String basePackage = configuredPackage;
        if (basePackage == null) {
            if (!ZSharp.isSourceFile(absolute)
                    || !absolute.startsWith(cursor.resolve("src/main/zsharp"))) {
                return Optional.empty();
            }
            Path physicalSource = absolute.toRealPath();
            if (!physicalSource.startsWith(zsharp)) {
                throw new IOException("Maven Z# source escapes " + zsharp);
            }
            Path packagePath = zsharp.relativize(physicalSource.getParent());
            StringBuilder inferred = new StringBuilder();
            for (Path part : packagePath) {
                if (!part.toString().isEmpty()) {
                    if (!inferred.isEmpty()) inferred.append('.');
                    inferred.append(part);
                }
            }
            basePackage = validatePackage(inferred.toString());
        }
        Path javaRoot = packageRoot(project, "java", basePackage);
        Path kotlinRoot = packageRoot(project, "kotlin", basePackage);
        Path zsharpRoot = packageRoot(project, "zsharp", basePackage);
        return Optional.of(new Scope(project, Map.of(
                "ZSHARP_MAVEN_PROJECT_ROOT", project.toString(),
                "ZSHARP_MAVEN_JAVA_ROOT", javaRoot.toString(),
                "ZSHARP_MAVEN_KOTLIN_ROOT", kotlinRoot.toString(),
                "ZSHARP_MAVEN_ZSHARP_ROOT", zsharpRoot.toString(),
                "ZSHARP_JVM_CLASSPATH", classpath(project))));
    }

    private static Path packageRoot(Path project, String language, String basePackage)
            throws IOException {
        Path languageRoot = project.resolve("src/main").resolve(language);
        Path selected = languageRoot;
        if (!basePackage.isEmpty()) {
            for (String part : basePackage.split("\\.")) selected = selected.resolve(part);
        }
        if (Files.exists(selected)) {
            Path real = selected.toRealPath();
            if (!real.startsWith(languageRoot.toAbsolutePath().normalize())) {
                throw new IOException("Maven source package escapes " + languageRoot);
            }
            return real;
        }
        return selected;
    }

    private static String classpath(Path project) {
        Set<Path> entries = new LinkedHashSet<>();
        addClasspath(entries, System.getProperty("java.class.path", ""));
        for (ClassLoader loader = Thread.currentThread().getContextClassLoader();
             loader != null; loader = loader.getParent()) {
            if (loader instanceof URLClassLoader urls) {
                for (URL url : urls.getURLs()) {
                    if (url.getProtocol().equals("file")) {
                        try {
                            addEntry(entries, Path.of(url.toURI()));
                        } catch (URISyntaxException exception) {
                            throw new IllegalArgumentException("Invalid JVM classpath URL", exception);
                        }
                    }
                }
            }
        }
        addEntry(entries, project.resolve("target/classes"));
        return String.join(File.pathSeparator,
                entries.stream().map(Path::toString).toList());
    }

    private static void addClasspath(Set<Path> entries, String classpath) {
        for (String entry : classpath.split(java.util.regex.Pattern.quote(File.pathSeparator), -1)) {
            addEntry(entries, entry.isEmpty() ? Path.of(".") : Path.of(entry));
        }
    }

    private static void addEntry(Set<Path> entries, Path path) {
        Path absolute = path.toAbsolutePath().normalize();
        if (Files.isDirectory(absolute) || Files.isRegularFile(absolute)) entries.add(absolute);
    }
}
