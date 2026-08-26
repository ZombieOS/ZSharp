package com.zombieos.zsharp;

/**
 * The complete result of invoking a Z# toolchain command.
 *
 * @param exitCode native process exit code
 * @param standardOutput text written to standard output
 * @param standardError text written to standard error
 */
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
