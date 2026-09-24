package com.zombieos.zsharp.porttest;

import android.app.Activity;
import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.TextView;

import java.io.DataInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.Arrays;
import java.util.HashSet;

public final class MainActivity extends Activity {
    private static final int PICK_PACKAGE = 1;
    private static final byte[] MAGIC = "ZSPKG1\r\n".getBytes(StandardCharsets.US_ASCII);
    private TextView details;

    static {
        System.loadLibrary("zsharp_port_test");
        System.loadLibrary("SDL3");
        System.loadLibrary("zsharp_android_vm");
    }

    private static native String nativeCoreStatus();
    private static native String nativeVmSmokeTest(String projectRoot);

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        LinearLayout layout = new LinearLayout(this);
        layout.setOrientation(LinearLayout.VERTICAL);
        int padding = (int) (24 * getResources().getDisplayMetrics().density);
        layout.setPadding(padding, padding, padding, padding);
        TextView title = new TextView(this);
        title.setText("Z# Android Port Test");
        title.setTextSize(23);
        layout.addView(title);
        details = new TextView(this);
        details.setText(nativeCoreStatus() + "\n" +
            nativeVmSmokeTest(getFilesDir().getAbsolutePath()) +
            "\n\nThis test build can inspect a Z# package, but it cannot run apps or games yet.");
        details.setTextSize(16);
        layout.addView(details);
        Button pick = new Button(this);
        pick.setText("Choose .zapp or .zgame");
        pick.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View view) {
                Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
                intent.setType("*/*");
                intent.addCategory(Intent.CATEGORY_OPENABLE);
                startActivityForResult(intent, PICK_PACKAGE);
            }
        });
        layout.addView(pick);
        Button renderer = new Button(this);
        renderer.setText("Test Android renderer (3 seconds)");
        renderer.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View view) {
                startActivity(new Intent(MainActivity.this, GameActivity.class));
            }
        });
        layout.addView(renderer);
        setContentView(layout);
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode != PICK_PACKAGE || resultCode != RESULT_OK || data == null) return;
        final Uri uri = data.getData();
        if (uri == null) return;
        details.setText("Checking package files and SHA-256 hashes…");
        new Thread(new Runnable() {
            @Override public void run() {
                String result;
                try (InputStream stream = getContentResolver().openInputStream(uri)) {
                    if (stream == null) throw new IOException("Could not open selected file");
                    result = readPackageInfo(stream);
                } catch (IOException error) {
                    result = "Package check failed: " + error.getMessage();
                }
                final String message = result;
                runOnUiThread(new Runnable() {
                    @Override public void run() { details.setText(message); }
                });
            }
        }, "zsharp-package-check").start();
    }

    private static long u32(DataInputStream input) throws IOException {
        long a = input.readUnsignedByte();
        long b = input.readUnsignedByte();
        long c = input.readUnsignedByte();
        long d = input.readUnsignedByte();
        return a | (b << 8) | (c << 16) | (d << 24);
    }

    private static String text(DataInputStream input) throws IOException {
        long length = u32(input);
        if (length > 1024 * 1024) throw new IOException("Invalid package text length");
        byte[] bytes = new byte[(int) length];
        input.readFully(bytes);
        return new String(bytes, StandardCharsets.UTF_8);
    }

    private static long u64(DataInputStream input) throws IOException {
        long value = 0;
        for (int i = 0; i < 8; i++) value |= ((long) input.readUnsignedByte()) << (i * 8);
        if (value < 0) throw new IOException("Invalid package entry size");
        return value;
    }

    private static boolean safePath(String path) {
        if (path.isEmpty() || path.startsWith("/") || path.startsWith("\\") ||
            path.contains("\\") || path.indexOf('\0') >= 0) return false;
        String[] parts = path.split("/", -1);
        for (String part : parts) {
            if (part.isEmpty() || part.equals(".") || part.equals("..") ||
                part.indexOf(':') >= 0) return false;
        }
        return true;
    }

    private static String readPackageInfo(InputStream stream) throws IOException {
        DataInputStream input = new DataInputStream(stream);
        byte[] magic = new byte[MAGIC.length];
        input.readFully(magic);
        if (!Arrays.equals(magic, MAGIC))
            throw new IOException("Not a bytecoded Z# package; source ZIPs are not supported by this test");
        if (u32(input) != 1) throw new IOException("Unsupported package format");
        long kind = u32(input);
        if (kind != 1 && kind != 2) throw new IOException("Invalid package kind");
        String version = u32(input) + "." + u32(input) + "." + u32(input) + "." + u32(input);
        String id = text(input);
        String name = text(input);
        long files = u32(input);
        if (files == 0 || files > 100000) throw new IOException("Invalid file count");
        HashSet<String> paths = new HashSet<>();
        byte[] buffer = new byte[64 * 1024];
        long total = 0;
        for (long entry = 0; entry < files; entry++) {
            String path = text(input);
            if (!safePath(path) || !paths.add(path))
                throw new IOException("Unsafe or duplicate package path");
            long size = u64(input);
            if (size > 32L * 1024 * 1024 * 1024 - total)
                throw new IOException("Package exceeds the size limit");
            total += size;
            byte[] expected = new byte[32];
            input.readFully(expected);
            MessageDigest digest;
            try {
                digest = MessageDigest.getInstance("SHA-256");
            } catch (NoSuchAlgorithmException error) {
                throw new IOException("SHA-256 unavailable", error);
            }
            long remaining = size;
            while (remaining > 0) {
                int chunk = (int) Math.min(remaining, buffer.length);
                input.readFully(buffer, 0, chunk);
                digest.update(buffer, 0, chunk);
                remaining -= chunk;
            }
            if (!Arrays.equals(expected, digest.digest()))
                throw new IOException("Package entry failed SHA-256: " + path);
        }
        if (input.read() != -1) throw new IOException("Unexpected bytes after package files");
        return nativeCoreStatus() + "\n\n" +
            (kind == 1 ? "App" : "Game") + ": " + name +
            "\nProject ID: " + id + "\nRequired Z#: " + version +
            "\nVerified files: " + files +
            "\n\nIntegrity verified. This Android build does not execute the package yet.";
    }
}
