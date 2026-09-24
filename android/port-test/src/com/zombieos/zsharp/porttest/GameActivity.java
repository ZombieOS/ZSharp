package com.zombieos.zsharp.porttest;

import org.libsdl.app.SDLActivity;

public final class GameActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] {"SDL3", "zsharp_android_vm"};
    }
}
