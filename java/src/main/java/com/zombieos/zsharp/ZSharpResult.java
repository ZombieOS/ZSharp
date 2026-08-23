package com.zombieos.zsharp;

/** The complete result of invoking a Z# toolchain command. */
public record ZSharpResult(int exitCode, String standardOutput, String standardError) {
    /**
     * Reports whether the native command exited successfully.
     *
     * @return {@code true} when the exit code is zero
     */
    public boolean succeeded() {
        return exitCode == 0;
    }
}
