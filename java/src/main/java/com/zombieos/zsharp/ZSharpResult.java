package com.zombieos.zsharp;

/** The complete result of invoking a Z# toolchain command. */
public record ZSharpResult(int exitCode, String standardOutput, String standardError) {
    public boolean succeeded() {
        return exitCode == 0;
    }
}
