package dev.twilitrealm.dusk;

import android.content.Intent;
import android.os.Bundle;
import android.util.Log;

import dev.encounter.borealis.BorealisActivity;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;

public class DuskActivity extends BorealisActivity {
    private static final String TAG = "DuskActivity";
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        extractBundledMods();
        extractPracticeSaves();
        super.onCreate(savedInstanceState);
    }

    // Practice-save metadata and binaries are packaged as APK assets, but the native
    // practice menu reads them through ordinary filesystem I/O.
    private void extractPracticeSaves() {
        File outDir = new File(getFilesDir(), "res/gz");
        File staging = new File(getFilesDir(), "res/gz.staging");
        File backup = new File(getFilesDir(), "res/gz.backup");
        try {
            // Recover if the process stopped between moving the old tree and installing the new one.
            if (!outDir.exists() && backup.exists() && !backup.renameTo(outDir)) {
                throw new IOException("Unable to restore practice saves from " + backup);
            }
            String assetRoot = findPracticeAssetRoot();
            if (assetRoot == null) {
                throw new IOException("Practice-save metadata is missing from APK assets");
            }
            deleteRecursively(staging);
            if (staging.exists()) {
                throw new IOException("Unable to clear " + staging);
            }
            extractAssetTree(assetRoot, staging);
            File metadata = new File(staging, "any_saves/any.bin");
            if (!metadata.isFile() || metadata.length() < 32) {
                throw new IOException("Practice-save extraction did not produce any_saves/any.bin");
            }
            long metadataLength = metadata.length();
            deleteRecursively(backup);
            if (backup.exists()) {
                throw new IOException("Unable to clear " + backup);
            }
            if (outDir.exists() && !outDir.renameTo(backup)) {
                throw new IOException("Unable to preserve practice saves in " + backup);
            }
            if (!staging.renameTo(outDir)) {
                if (backup.exists() && !backup.renameTo(outDir)) {
                    throw new IOException("Unable to install practice saves; previous copy remains in " + backup);
                }
                throw new IOException("Unable to install practice saves; previous copy restored");
            }
            deleteRecursively(backup);
            Log.i(TAG, "Extracted practice saves from " + assetRoot + " to " + outDir +
                    " (" + metadataLength + "-byte metadata)");
        } catch (IOException | SecurityException e) {
            Log.w(TAG, "Failed to extract practice saves", e);
        }
    }

    private String findPracticeAssetRoot() {
        String[] roots = {"res/gz", "res/res/gz", "gz"};
        for (String root : roots) {
            try (InputStream in = getAssets().open(root + "/any_saves/any.bin")) {
                return root;
            } catch (IOException ignored) {
                // Try the next layout. This keeps older/generated APK asset layouts usable.
            }
        }
        return null;
    }

    private void extractAssetTree(String assetPath, File output) throws IOException {
        String[] children = getAssets().list(assetPath);
        if (children == null || children.length == 0) {
            File parent = output.getParentFile();
            if (parent != null && !parent.exists() && !parent.mkdirs()) {
                throw new IOException("Unable to create " + parent);
            }
            byte[] buffer = new byte[65536];
            try (InputStream in = getAssets().open(assetPath);
                 OutputStream out = new FileOutputStream(output)) {
                int count;
                while ((count = in.read(buffer)) > 0) {
                    out.write(buffer, 0, count);
                }
            }
            return;
        }

        if (!output.exists() && !output.mkdirs()) {
            throw new IOException("Unable to create " + output);
        }
        for (String child : children) {
            extractAssetTree(assetPath + "/" + child, new File(output, child));
        }
    }

    // Bundled mod packages ship as APK assets, which the native loader cannot read directly;
    // mirror them into internal storage (the loader's CachePath/bundled_mods search dir)
    // before SDL_main starts.
    private void extractBundledMods() {
        File outDir = new File(getFilesDir(), "bundled_mods");
        try {
            deleteRecursively(outDir); // drop packages removed by an app update
            String[] names = getAssets().list("mods");
            if (names == null || names.length == 0) {
                return;
            }
            if (!outDir.mkdirs()) {
                Log.w(TAG, "Unable to create " + outDir);
                return;
            }
            byte[] buffer = new byte[65536];
            for (String name : names) {
                if (!name.endsWith(".dusk")) {
                    continue;
                }
                try (InputStream in = getAssets().open("mods/" + name);
                     OutputStream out = new FileOutputStream(new File(outDir, name)))
                {
                    int count;
                    while ((count = in.read(buffer)) > 0) {
                        out.write(buffer, 0, count);
                    }
                }
            }
        } catch (IOException e) {
            Log.w(TAG, "Failed to extract bundled mods", e);
        }
    }

    private static void deleteRecursively(File file) {
        File[] children = file.listFiles();
        if (children != null) {
            for (File child : children) {
                deleteRecursively(child);
            }
        }
        file.delete();
    }

    @Override
    protected String[] getArguments() {
        String[] arguments = super.getArguments();
        if (arguments.length > 0) {
            return arguments;
        }

        Intent intent = getIntent();
        if (intent == null) {
            return arguments;
        }
        String[] argv = intent.getStringArrayExtra("dusk_argv");
        if (argv != null && argv.length > 0) {
            return argv;
        }
        String rawArgs = intent.getStringExtra("dusk_args");
        return rawArgs == null ? arguments : splitArguments(rawArgs.trim());
    }

}
