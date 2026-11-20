package org.korra.recomp;

import android.content.ContentResolver;
import android.content.Context;
import android.database.Cursor;
import android.net.Uri;
import android.provider.DocumentsContract;
import android.util.Log;

import java.io.BufferedInputStream;
import java.io.BufferedOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;

/**
 * Copies the user's GameData tree out of a SAF folder into app storage.
 *
 * <p>The picker can return either the game folder (the one that contains
 * {@code GameData}) or the {@code GameData} folder itself; both are accepted.
 * Everything below {@code GameData} is copied, because the engine reads the
 * two CPKs plus the loose {@code sound/} and {@code movie/} folders at
 * runtime, and the CPK layer is not allowed to reach outside its root.
 *
 * <p>Why copy at all: the lifted code opens paths through the host file layer,
 * which is plain POSIX I/O. A content:// URI cannot be handed to that layer
 * without a proxy the engine does not know about. Copying once, on first run,
 * keeps the runtime boring.
 */
final class GameDataInstaller {
    private static final String TAG = "Korra";

    /** Rough footprint of the shipped GameData tree, with slack for a patch. */
    static final long REQUIRED_BYTES = 2_600_000_000L;

    private static final int BUFFER = 1 << 20;

    interface Progress {
        void onProgress(long files, long bytes);
    }

    static final class Result {
        final long files;
        final long bytes;

        Result(long files, long bytes) {
            this.files = files;
            this.bytes = bytes;
        }
    }

    private GameDataInstaller() {
    }

    /** True when the marker, both CPKs and the image the lift needs are present. */
    static boolean isInstalled(File gameRoot) {
        File data = new File(gameRoot, "GameData");
        return new File(gameRoot, MainActivity.INSTALL_MARKER).isFile()
                && new File(gameRoot, "LoK.exe").isFile()
                && new File(data, "data000.cpk").isFile()
                && new File(data, "data001.cpk").isFile();
    }

    static String human(long bytes) {
        if (bytes >= 1_000_000_000L) {
            return String.format(java.util.Locale.US, "%.2f GB", bytes / 1e9);
        }
        return String.format(java.util.Locale.US, "%.0f MB", bytes / 1e6);
    }

    static Result copy(Context context, Uri tree, File gameRoot, Progress progress)
            throws IOException {
        ContentResolver resolver = context.getContentResolver();
        String rootDocId = DocumentsContract.getTreeDocumentId(tree);

        // Case 1: the picked tree is the game root and contains a GameData child.
        String gameDataDocId = null;
        try (Cursor c = query(resolver, childUri(tree, rootDocId))) {
            while (c != null && c.moveToNext()) {
                String name = c.getString(1);
                if ("GameData".equalsIgnoreCase(name)) {
                    gameDataDocId = c.getString(0);
                    break;
                }
            }
        }
        if (gameDataDocId == null) {
            // Case 2: the picked tree is GameData itself. Guard against a wrong
            // pick by requiring one of the two CPKs at this level.
            try (Cursor c = query(resolver, childUri(tree, rootDocId))) {
                boolean looksRight = false;
                while (c != null && c.moveToNext()) {
                    String name = c.getString(1).toLowerCase(java.util.Locale.ROOT);
                    if (name.equals("data000.cpk") || name.equals("data001.cpk")) {
                        looksRight = true;
                    }
                }
                if (!looksRight) {
                    throw new IOException("no GameData folder and no data000.cpk in the selection");
                }
            }
            gameDataDocId = rootDocId;
        }

        File target = new File(gameRoot, "GameData");
        if (!target.isDirectory() && !target.mkdirs()) {
            throw new IOException("cannot create " + target);
        }
        Log.i(TAG, "copying tree " + gameDataDocId + " -> " + target);

        long[] totals = new long[2];
        copyDir(resolver, tree, gameDataDocId, target, totals, progress);

        /* The lifted program still reads its image's data sections (strings,
         * vtables, resources), so LoK.exe is part of the install - 6.8 MB next
         * to the 2 GB of GameData. It lives in the game root, which the picker
         * may have been (case 1) or not (case 2). */
        if (gameDataDocId.equals(rootDocId)) {
            throw new IOException("pick the game folder that contains GameData, "
                    + "not GameData itself: LoK.exe must be copied too");
        }
        copyRootFile(resolver, tree, rootDocId, "LoK.exe", new File(gameRoot, "LoK.exe"),
                totals, progress);

        if (totals[0] == 0) {
            throw new IOException("the selection was empty");
        }
        // Only after a complete pass, so a killed copy is retried next launch.
        try (OutputStream marker = new FileOutputStream(
                new File(gameRoot, MainActivity.INSTALL_MARKER))) {
            marker.write(("files=" + totals[0] + " bytes=" + totals[1] + "\n").getBytes());
        }
        return new Result(totals[0], totals[1]);
    }

    private static void copyDir(ContentResolver resolver, Uri tree, String docId, File target,
                                long[] totals, Progress progress) throws IOException {
        try (Cursor c = query(resolver, childUri(tree, docId))) {
            if (c == null) {
                return;
            }
            while (c.moveToNext()) {
                String id = c.getString(0);
                String name = c.getString(1);
                String mime = c.getString(2);
                File child = new File(target, name);
                if (DocumentsContract.Document.MIME_TYPE_DIR.equals(mime)) {
                    if (!child.isDirectory() && !child.mkdirs()) {
                        throw new IOException("cannot create " + child);
                    }
                    copyDir(resolver, tree, id, child, totals, progress);
                } else {
                    copyFile(resolver, tree, id, child, totals, progress);
                }
            }
        }
    }

    private static void copyFile(ContentResolver resolver, Uri tree, String docId, File target,
                                 long[] totals, Progress progress) throws IOException {        Uri document = DocumentsContract.buildDocumentUriUsingTree(tree, docId);
        try (InputStream in = new BufferedInputStream(resolver.openInputStream(document), BUFFER);
             OutputStream out = new BufferedOutputStream(new FileOutputStream(target), BUFFER)) {
            if (in == null) {
                throw new IOException("cannot open " + docId);
            }
            byte[] buffer = new byte[BUFFER];
            int n;
            while ((n = in.read(buffer)) > 0) {
                out.write(buffer, 0, n);
                totals[1] += n;
            }
        }
        totals[0]++;
        if (progress != null && (totals[0] % 32) == 0) {
            progress.onProgress(totals[0], totals[1]);
        }
    }

    /** Copy one named file from the picked tree's root, if it exists. */
    private static void copyRootFile(ContentResolver resolver, Uri tree, String rootDocId,
                                     String name, File target, long[] totals,
                                     Progress progress) throws IOException {
        try (Cursor c = query(resolver, childUri(tree, rootDocId))) {
            if (c == null) {
                throw new IOException("cannot list the picked folder");
            }
            while (c.moveToNext()) {
                if (name.equalsIgnoreCase(c.getString(1))) {
                    copyFile(resolver, tree, c.getString(0), target, totals, progress);
                    return;
                }
            }
        }
        throw new IOException("LoK.exe is not in the picked folder; pick the game root");
    }

    private static Uri childUri(Uri tree, String docId) {
        return DocumentsContract.buildChildDocumentsUriUsingTree(tree, docId);
    }

    private static Cursor query(ContentResolver resolver, Uri uri) {
        return resolver.query(uri, new String[]{
                DocumentsContract.Document.COLUMN_DOCUMENT_ID,
                DocumentsContract.Document.COLUMN_DISPLAY_NAME,
                DocumentsContract.Document.COLUMN_MIME_TYPE,
        }, null, null, null);
    }
}
