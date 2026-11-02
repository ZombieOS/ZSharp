package com.zombieos.zsharp;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertNotEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import javax.tools.ToolProvider;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.condition.EnabledIfSystemProperty;
import org.junit.jupiter.api.io.TempDir;

class MavenProjectInteropTest {
    @TempDir Path project;

    private Path write(String relative, String source) throws Exception {
        Path file = project.resolve(relative);
        Files.createDirectories(file.getParent());
        Files.writeString(file, source);
        return file;
    }

    private Path setup(String imports, String body) throws Exception {
        write("project.zsettings", """
                zsharp = type.settings
                Project: "Maven imports":
                PID: "maven_import_fixture":
                Version: [1.0.0.0]:
                Authors: ["Validation"]:
                Description: "Maven package import validation":
                ZSharp: [1.4.0.0]:
                Dependencies (
                ):
                """);
        write("src/main/zsharp/com/user/project/ZSharp.zsharp", """
                zsharp = type.script
                noticed room Shared[] (
                    noticed text Reply[text Value] (
                        feed(Value + " callback"):
                    )
                )
                """);
        // Generated resource copies must not become a second source declaration.
        write("target/classes/com/user/project/ZSharp.zsharp", "not Z# source");
        return write("src/main/zsharp/com/user/project/Main.zsharp", """
                zsharp = type.script
                noticed room Main[] (
                    import maven_import_fixture.ZSharp():
                """ + imports + "\nnoticed brain Start[] (\n" + body + "\n)\n)\n");
    }

    private ZSharpToolchain toolchain() {
        String override = System.getProperty("zsharp.test.executable");
        if (override != null && !override.isBlank()) {
            return new ZSharpToolchain(Path.of(override));
        }
        return new ZSharpToolchain(EmbeddedRuntime.discover().orElseThrow());
    }

    @Test
    void shortJavaImportLoadsMavenClassesAndCallsBackIntoZSharp() throws Exception {
        Path script = setup("import java:maven_import_fixture.Java():", """
                Print(Function.call(java:Java:greet ["Ada"])):
                Print(Function.call(java:Java:callback ["Ada"])):
                """);
        Path source = write("src/main/java/com/user/project/Java.java", """
                package com.user.project;
                import ZSharp.ZSharp;
                import ZSharp.ZSharp_Export;
                @ZSharp_Export
                public class Java {
                    public static String greet(String name) { return "Hello " + name; }
                    public static Object callback(String name) {
                        return ZSharp.call("ZSharp:Shared:Reply", name);
                    }
                }
                """);
        Path classes = project.resolve("target/classes");
        int compiled = ToolProvider.getSystemJavaCompiler().run(null, null, null,
                "--release", "17", "-classpath", System.getProperty("java.class.path"),
                "-d", classes.toString(), source.toString());
        assertEquals(0, compiled);
        ZSharpResult result = toolchain().run(script);
        assertEquals(0, result.exitCode(), result.standardError());
        assertEquals("Hello Ada\nAda callback", result.standardOutput().trim().replace("\r\n", "\n"));
        assertTrue(Files.notExists(project.resolve("Java.zjava.jar")));
        ZSharpToolchain selected = toolchain().withBasePackage("com.user.project");
        Path bytecode = project.resolve("target/Main.zbc");
        ZSharpResult compiledResult = selected.compile(script, bytecode);
        assertEquals(0, compiledResult.exitCode(), compiledResult.standardError());
        ZSharpResult bytecodeResult = selected.runBytecode(bytecode);
        assertEquals(0, bytecodeResult.exitCode(), bytecodeResult.standardError());
        assertEquals(result.standardOutput().trim(), bytecodeResult.standardOutput().trim());
    }

    @Test
    void rawJavaSourceWithoutCompiledClassesFailsClearly() throws Exception {
        Path script = setup("import java:maven_import_fixture.Java():",
                "Print(Function.call(java:Java:greet [])):");
        write("src/main/java/com/user/project/Java.java", """
                package com.user.project;
                public class Java { public static String greet() { return "uncompiled"; } }
                """);
        ZSharpResult result = toolchain().run(script);
        assertNotEquals(0, result.exitCode());
        assertTrue(result.standardError().contains("compiled")
                        || result.standardError().contains("Maven"), result.standardError());
    }

    @Test
    @EnabledIfSystemProperty(named = "zsharp.test.kotlin.compiler.classpath", matches = ".+")
    void shortKotlinImportUsesJvmFacadeAndRetainsZSharpCallbacks() throws Exception {
        Path script = setup("import kt:maven_import_fixture.Kotlin():", """
                Print(Function.call(kt:Kotlin:greet ["Ada"])):
                Print(Function.call(kt:Kotlin:callback ["Ada"])):
                """);
        Path source = write("src/main/kotlin/com/user/project/Kotlin.kt", """
                @file:JvmName("CustomKotlinFacade")
                package com.user.project
                import ZSharp.ZSharp
                import ZSharp.ZSharp_Export
                @ZSharp_Export
                fun greet(name: String): String = "Kotlin " + name
                @ZSharp_Export
                fun callback(name: String): Any? = ZSharp.call("ZSharp:Shared:Reply", name)
                """);
        String stdlib = System.getProperty("zsharp.test.kotlin.stdlib");
        String classpath = System.getProperty("java.class.path") + File.pathSeparator + stdlib;
        Path executable = Path.of(System.getProperty("java.home"), "bin",
                System.getProperty("os.name").startsWith("Windows") ? "java.exe" : "java");
        Process compiler = new ProcessBuilder(executable.toString(), "-cp",
                System.getProperty("zsharp.test.kotlin.compiler.classpath"),
                "org.jetbrains.kotlin.cli.jvm.K2JVMCompiler", "-no-stdlib", "-no-reflect",
                "-jvm-target", "17", "-classpath", classpath,
                "-d", project.resolve("target/classes").toString(), source.toString())
                .redirectErrorStream(true).start();
        String output = new String(compiler.getInputStream().readAllBytes(), StandardCharsets.UTF_8);
        assertEquals(0, compiler.waitFor(), output);
        // The invoking application's classpath carries Kotlin's ordinary runtime.
        String previous = System.getProperty("java.class.path");
        try {
            System.setProperty("java.class.path", previous + File.pathSeparator + stdlib);
            ZSharpResult result = toolchain().run(script);
            assertEquals(0, result.exitCode(), result.standardError());
            assertEquals("Kotlin Ada\nAda callback", result.standardOutput().trim().replace("\r\n", "\n"));
        } finally {
            System.setProperty("java.class.path", previous);
        }
    }
}
