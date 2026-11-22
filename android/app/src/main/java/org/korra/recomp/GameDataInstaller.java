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
import java.util.Locale;

/**
 * Copies the user's game root out of a SAF folder into app storage.
 *
 * <p>The picked folder must be the <b>game root</b>: the one that contains
 * {@code LoK.exe} and {@code GameData\}. LoK.exe is not optional - the lifted
 * program reads its image's data sections (strings, vtables, resources) at
 * runtime - so a GameData-only selection is rejected before a byte is copied.
 *
 * <p>Why copy at all: the lifted code opens paths through the host file layer,
 * which is plain POSIX I/O. A content:// URI cannot be handed to that layer
 * without a proxy the engine does not know about. Copying once, on first run,
 * keeps the runtime boring.
 *
 * <p>The copy is resumable at file granularity: a file whose target already
 * exists with the source's exact size is skipped, so a copy killed by the
 * system (or a storage-full failure) continues where it stopped on the next
 * attempt. 2.8 GB is a few minutes of I/O, so progress is reported on a byte
 * clock (not a file count - the first files are 265 MB and 1.6 GB) and every
 * top-level item is timed in the log.
 */
final class GameDataInstaller {
    private static final String TAG = "Korra";

    /** Written only after a complete pass; its absence means "copy me again". */
    static final String INSTALL_MARKER = "install.complete";

    private static final int BUFFER = 1 << 20;

    /** How often the byte clock publishes progress. */
    private static final long PROGRESS_INTERVAL_NS = 400_000_000L;

    /** Headroom requested on top of the measured source size. */
    private static final long SPACE_SLACK = 128L << 20;

    interface Progress {
        void onProgress(long files, long filesTotal, long bytes, long bytesTotal, String item);
    }

    /** Why a picked folder cannot be installed, with a string for the user. */
    enum Problem {
        NO_GAMEDATA(R.string.pick_no_gamedata),
        NO_EXE(R.string.pick_no_exe),
        GAMEDATA_ITSELF(R.string.pick_gamedata_itself),
        INCOMPLETE(R.string.pick_incomplete);

        final int message;

        Problem(int message) {
            this.message = message;
        }
    }

    static final class PickProblem extends IOException {
        final Problem problem;

        PickProblem(Problem problem) {
            super(problem.name());
            this.problem = problem;
        }

        String message(Context context) {
            return context.getString(problem.message);
        }
    }

    static final class SpaceProblem extends IOException {
        final long needed;
        final long available;

        SpaceProblem(long needed, long available) {
            super("need " + needed + " bytes, have " + available);
            this.needed = needed;
            this.available = available;
        }
    }

    /** A validated selection: where the two required pieces live, and its size. */
    static final class Source {
        final String gameDataDocId;
        final String exeDocId;
        final long files;
        final long bytes;

        Source(String gameDataDocId, String exeDocId, long files, long bytes) {
            this.gameDataDocId = gameDataDocId;
            this.exeDocId = exeDocId;
            this.files = files;
            this.bytes = bytes;
        }
    }

    static final class Result {
        final long files;
        final long skipped;
        final long bytes;
        final long millis;

        Result(long files, long skipped, long bytes, long millis) {
            this.files = files;
            this.skipped = skipped;
            this.bytes = bytes;
            this.millis = millis;
        }
    }

    private GameDataInstaller() {
    }

    /** True when the marker, LoK.exe and both CPKs are present. */
    static boolean isInstalled(File gameRoot) {
        File data = new File(gameRoot, "GameData");
        return new File(gameRoot, INSTALL_MARKER).isFile()
                && new File(gameRoot, "LoK.exe").isFile()
                && new File(data, "data000.cpk").isFile()
                && new File(data, "data001.cpk").isFile();
    }

    /** True when a previous copy left something behind worth resuming. */
    static boolean hasPartialInstall(File gameRoot) {
        if (!gameRoot.isDirectory()) {
            return false;
        }
        String[] children = gameRoot.list();
        return children != null && children.length > 0;
    }

    static String human(long bytes) {
        if (bytes >= 1_000_000_000L) {
            return String.format(Locale.US, "%.2f GB", bytes / 1e9);
        }
        return String.format(Locale.US, "%.0f MB", bytes / 1e6);
    }

    /**
     * Check the picked tree before copying anything: LoK.exe, GameData and both
     * CPKs must be there, and the size is measured for the space check and the
     * progress bar. Throws a {@link PickProblem} naming what is missing.
     */
    static Source validate(Context context, Uri tree) throws IOException {
        ContentResolver resolver = context.getContentResolver();
        String rootDocId = DocumentsContract.getTreeDocumentId(tree);

        String gameDataDocId = null;
        String exeDocId = null;
        long exeBytes = -1;
        boolean cpkAtRoot = false;

        try (Cursor c = query(resolver, childUri(tree, rootDocId))) {
            if (c == null) {
                throw new IOException("cannot list the selected folder");
            }
            while (c.moveToNext()) {
                String name = c.getString(1);
                if (name == null) {
                    continue;
                }
                boolean dir = isDir(c.getString(2));
                if (dir && name.equalsIgnoreCase("GameData")) {
                    gameDataDocId = c.getString(0);
                } else if (!dir && name.equalsIgnoreCase("LoK.exe")) {
                    exeDocId = c.getString(0);
                    exeBytes = sizeOf(c);
                } else if (!dir && (name.equalsIgnoreCase("data000.cpk")
                        || name.equalsIgnoreCase("data001.cpk"))) {
                    cpkAtRoot = true;
                }
            }
        }

        if (gameDataDocId == null) {
            throw new PickProblem(cpkAtRoot ? Problem.GAMEDATA_ITSELF : Problem.NO_GAMEDATA);
        }
        if (exeDocId == null) {
            throw new PickProblem(Problem.NO_EXE);
        }

        boolean cpk0 = false;
        boolean cpk1 = false;
        try (Cursor c = query(resolver, childUri(tree, gameDataDocId))) {
            while (c != null && c.moveToNext()) {
                String name = c.getString(1);
                if (name == null) {
                    continue;
                }
                if (name.equalsIgnoreCase("data000.cpk")) {
                    cpk0 = true;
                } else if (name.equalsIgnoreCase("data001.cpk")) {
                    cpk1 = true;
                }
            }
        }
        if (!cpk0 || !cpk1) {
            throw new PickProblem(Problem.INCOMPLETE);
        }

        long[] totals = new long[2];
        measure(resolver, tree, gameDataDocId, totals);
        long bytes = totals[1] + Math.max(0, exeBytes);
        Log.i(TAG, "install: validated " + (totals[0] + 1) + " files, " + bytes + " bytes");
        return new Source(gameDataDocId, exeDocId, totals[0] + 1, bytes);
    }

    /**
     * Copy the validated tree into {@code gameRoot}. Existing files whose size
     * matches the source are skipped, so this is safe to call again after an
     * interrupted copy.
     */
    static Result copy(Context context, Uri tree, Source source, File gameRoot, Progress progress)
            throws IOException {
        ContentResolver resolver = context.getContentResolver();

        File target = new File(gameRoot, "GameData");
        if (!target.isDirectory() && !target.mkdirs()) {
            throw new IOException("cannot create " + target);
        }

        long existing = existingBytes(target) + existingBytes(new File(gameRoot, "LoK.exe"));
        long need = Math.max(0, source.bytes - existing);
        long free = gameRoot.getUsableSpace();
        if (free > 0 && free < need + SPACE_SLACK) {
            throw new SpaceProblem(need + SPACE_SLACK, free);
        }

        long started = System.currentTimeMillis();
        Meter meter = new Meter(progress, source.files, source.bytes, "GameData");

        Log.i(TAG, "install: GameData start: " + source.files + " files, "
                + source.bytes + " bytes (" + GameDataInstaller.human(existing) + " already present)");
        copyDir(resolver, tree, source.gameDataDocId, target, meter, "");
        long gameDataMs = System.currentTimeMillis() - started;
        Log.i(TAG, "install: GameData done in " + gameDataMs + " ms, "
                + GameDataInstaller.human(meter.bytes) + " at "
                + GameDataInstaller.human(rate(meter.bytes, gameDataMs)) + "/s");

        long exeStarted = System.currentTimeMillis();
        Log.i(TAG, "install: LoK.exe start");
        copyOne(resolver, tree, source.exeDocId, new File(gameRoot, "LoK.exe"),
                -1, meter);
        long exeMs = System.currentTimeMillis() - exeStarted;
        Log.i(TAG, "install: LoK.exe done in " + exeMs + " ms");
        meter.tick(true);

        if (meter.bytes == 0) {
            throw new IOException("the selection was empty");
        }

        // Only after a complete pass, so a killed copy is retried next launch.
        try (OutputStream marker = new FileOutputStream(new File(gameRoot, INSTALL_MARKER))) {
            marker.write(("files=" + meter.files + " skipped=" + meter.skipped
                    + " bytes=" + meter.bytes + " total=" + source.bytes
                    + " seconds=" + ((System.currentTimeMillis() - started) / 1000) + "\n")
                    .getBytes());
        }
        return new Result(meter.files, meter.skipped, meter.bytes,
                System.currentTimeMillis() - started);
    }

    private static void copyDir(ContentResolver resolver, Uri tree, String docId, File target,
                                Meter meter, String prefix) throws IOException {
        try (Cursor c = query(resolver, childUri(tree, docId))) {
            if (c == null) {
                return;
            }
            while (c.moveToNext()) {
                String id = c.getString(0);
                String name = c.getString(1);
                if (name == null) {
                    continue;
                }
                File child = new File(target, name);
                if (isDir(c.getString(2))) {
                    if (!child.isDirectory() && !child.mkdirs()) {
                        throw new IOException("cannot create " + child);
                    }
                    copyDir(resolver, tree, id, child, meter, prefix + name + "/");
                } else {
                    copyOne(resolver, tree, id, child, sizeOf(c), meter);
                }
            }
        }
    }

    /**
     * Copy one document. A target that already has the source's exact size is
     * counted and skipped (resume). {@code size < 0} means the provider did not
     * report one: no skip, no length assertion.
     */
    private static void copyOne(ContentResolver resolver, Uri tree, String docId, File target,
                                long size, Meter meter) throws IOException {
        if (size > 0 && target.isFile() && target.length() == size) {
            meter.skipped++;
            meter.file(size);
            meter.tick(false);
            return;
        }
        Uri document = DocumentsContract.buildDocumentUriUsingTree(tree, docId);
        long written = 0;
        try (InputStream in = new BufferedInputStream(resolver.openInputStream(document), BUFFER);
             OutputStream out = new BufferedOutputStream(new FileOutputStream(target), BUFFER)) {
            if (in == null) {
                throw new IOException("cannot open " + docId);
            }
            byte[] buffer = new byte[BUFFER];
            int n;
            while ((n = in.read(buffer)) > 0) {
                out.write(buffer, 0, n);
                written += n;
                meter.bytes(n);
                meter.tick(false);
            }
        }
        if (size > 0 && written != size) {
            throw new IOException("short copy of " + target.getName() + ": "
                    + written + " of " + size + " bytes");
        }
        meter.files++;
        meter.bytes(0);
        meter.tick(false);
    }

    private static long existingBytes(File file) {
        if (file.isFile()) {
            return file.length();
        }
        if (!file.isDirectory()) {
            return 0;
        }
        long total = 0;
        File[] children = file.listFiles();
        if (children != null) {
            for (File child : children) {
                total += existingBytes(child);
            }
        }
        return total;
    }

    private static long rate(long bytes, long millis) {
        if (millis <= 0) {
            return bytes;
        }
        return bytes * 1000 / millis;
    }

    private static void measure(ContentResolver resolver, Uri tree, String docId, long[] totals) {
        try (Cursor c = query(resolver, childUri(tree, docId))) {
            if (c == null) {
                return;
            }
            while (c.moveToNext()) {
                long size = sizeOf(c);
                if (isDir(c.getString(2))) {
                    measure(resolver, tree, c.getString(0), totals);
                } else {
                    totals[0]++;
                    if (size > 0) {
                        totals[1] += size;
                    }
                }
            }
        } catch (RuntimeException e) {
            Log.w(TAG, "install: measure failed below " + docId, e);
        }
    }

    /** Byte-clock progress: a 1.6 GB CPK must not look frozen. */
    private static final class Meter {
        final Progress progress;
        final long totalFiles;
        final long totalBytes;
        final String item;
        long files;
        long skipped;
        long bytes;
        long lastNs;

        Meter(Progress progress, long totalFiles, long totalBytes, String item) {
            this.progress = progress;
            this.totalFiles = totalFiles;
            this.totalBytes = totalBytes;
            this.item = item;
            this.lastNs = System.nanoTime();
        }

        void file(long size) {
            files++;
            bytes += size;
        }

        void bytes(long n) {
            bytes += n;
        }

        void tick(boolean force) {
            long now = System.nanoTime();
            if (!force && now - lastNs < PROGRESS_INTERVAL_NS) {
                return;
            }
            lastNs = now;
            if (progress != null) {
                progress.onProgress(files, totalFiles, bytes, totalBytes, item);
            }
        }
    }

    private static boolean isDir(String mime) {
        return DocumentsContract.Document.MIME_TYPE_DIR.equals(mime);
    }

    private static long sizeOf(Cursor c) {
        return c.isNull(3) ? -1 : c.getLong(3);
    }

    private static Uri childUri(Uri tree, String docId) {
        return DocumentsContract.buildChildDocumentsUriUsingTree(tree, docId);
    }

    private static Cursor query(ContentResolver resolver, Uri uri) {
        return resolver.query(uri, new String[]{
                DocumentsContract.Document.COLUMN_DOCUMENT_ID,
                DocumentsContract.Document.COLUMN_DISPLAY_NAME,
                DocumentsContract.Document.COLUMN_MIME_TYPE,
                DocumentsContract.Document.COLUMN_SIZE,
        }, null, null, null);
    }
}
