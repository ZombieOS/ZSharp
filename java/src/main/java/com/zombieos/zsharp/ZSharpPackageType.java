package com.zombieos.zsharp;

/** The kind of Z# package to create. */
public enum ZSharpPackageType {
    /** A window application stored with the {@code .zapp} extension. */
    APP("app"),

    /** A game stored with the {@code .zgame} extension. */
    GAME("game");

    private final String commandName;

    ZSharpPackageType(String commandName) {
        this.commandName = commandName;
    }

    String commandName() {
        return commandName;
    }
}
