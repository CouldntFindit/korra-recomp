package org.korra.recomp;

import android.content.Intent;
import android.content.SharedPreferences;
import android.graphics.Color;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.provider.DocumentsContract;
import android.util.Log;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.TextView;
import android.widget.Toast;

import com.google.androidgamesdk.GameActivity;

import java.io.File;
import java.util.List;
import java.util.Locale;

/**
 * GameActivity shell for the Korra recompilation.
 *
 * <p>Responsibilities, in order:
 * <ol>
 *   <li>let GameActivity create the SurfaceView and start the native thread
 *       (the library named by the android.app.lib_name meta-data, libkorra.so);</li>
 *   <li>on first run, detect the missing game root, open the system folder
 *       picker (Storage Access Framework), validate that the user picked the
 *       folder that contains both {@code LoK.exe} and {@code GameData\}, then
 *       copy it into {@code files/game/} with progress and timing;</li>
 *   <li>tell the native side when the data is in place.</li>
 * </ol>
 *
 * <p>No storage permission is requested: SAF hands the app a tree URI and the
 * copy is done by the app into its own internal files directory. Internal, not
 * external: Android 16+ refuses the app {@code fopen} on files that adb pushed
 * into its own external files dir (EACCES), so the one install path that works
 * for both the picker and the dev {@code run-as} flow is internal storage.
 *
 * <p>The native side never touches the SAF URI. It only sees a plain directory
 * path, which is the contract docs/ANDROID_BRINGUP.md describes for the file
 * layer.
 */
public class MainActivity extends GameActivity {
    private static final String TAG = "Korra";
    private static final int REQUEST_PICK_TREE = 0x4b4f; // 'KO'

    private static final String PREFS = "korra-install";
    private static final String PREF_TREE = "picked_tree";

    private LinearLayout overlay;
    private TextView status;
    private ProgressBar progress;
    private final Handler ui = new Handler(Looper.getMainLooper());
    private volatile boolean busy;
    private File gameRoot;
    private long copyStartMs;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        File files = getFilesDir();
        gameRoot = files == null ? null : new File(files, "game");
        buildOverlay();

        if (gameRoot == null) {
            showMessage(getString(R.string.copy_failed, "app files directory unavailable"), false);
            return;
        }

        if (GameDataInstaller.isInstalled(gameRoot)) {
            overlay.setVisibility(View.GONE);
            Log.i(TAG, "game data present at " + gameRoot.getAbsolutePath());
            nativeGameDataReady(new File(gameRoot, "GameData").getAbsolutePath());
            return;
        }

        showMessage(getString(R.string.pick_prompt) + "\n\n" + getString(R.string.pick_hint), true);

        Uri saved = savedTree();
        if (saved != null && GameDataInstaller.hasPartialInstall(gameRoot)) {
            /* A copy was interrupted (kill, storage full, reboot). The tree
             * grant is persisted, so continue it without asking the user to
             * walk the picker again. Resumable at file granularity. */
            Log.i(TAG, "install: resuming an interrupted copy from " + saved);
            startInstall(saved, true);
        } else {
            /* First launch: do not make the user find the tap. Open the picker
             * once the activity has settled. */
            ui.postDelayed(this::pickGameFolder, 400);
        }
    }

    private void buildOverlay() {
        overlay = new LinearLayout(this);
        overlay.setOrientation(LinearLayout.VERTICAL);
        overlay.setGravity(Gravity.CENTER);
        overlay.setBackgroundColor(0xCC000000);
        overlay.setPadding(dp(48), dp(32), dp(48), dp(32));

        status = new TextView(this);
        status.setTextColor(Color.WHITE);
        status.setTextSize(20f);
        status.setGravity(Gravity.CENTER);
        overlay.addView(status, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        progress = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        progress.setMax(1000);
        progress.setIndeterminate(false);
        progress.setVisibility(View.GONE);
        LinearLayout.LayoutParams barParams = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, dp(10));
        barParams.topMargin = dp(28);
        overlay.addView(progress, barParams);

        addContentView(overlay, new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
    }

    private int dp(int value) {
        return (int) (value * getResources().getDisplayMetrics().density);
    }

    private void showMessage(String text, boolean clickable) {
        status.setText(text);
        progress.setVisibility(View.GONE);
        overlay.setVisibility(View.VISIBLE);
        overlay.setClickable(clickable);
        overlay.setOnClickListener(clickable ? v -> pickGameFolder() : null);
    }

    private void showProgress(String text) {
        status.setText(text);
        progress.setIndeterminate(true);
        progress.setVisibility(View.VISIBLE);
        overlay.setVisibility(View.VISIBLE);
        overlay.setClickable(false);
        overlay.setOnClickListener(null);
    }

    private SharedPreferences prefs() {
        return getSharedPreferences(PREFS, MODE_PRIVATE);
    }

    /** The last picked tree, if its persistable grant still exists. */
    private Uri savedTree() {
        String value = prefs().getString(PREF_TREE, null);
        if (value == null) {
            return null;
        }
        Uri uri;
        try {
            uri = Uri.parse(value);
        } catch (RuntimeException e) {
            return null;
        }
        List<android.content.UriPermission> grants =
                getContentResolver().getPersistedUriPermissions();
        for (android.content.UriPermission grant : grants) {
            if (grant.isReadPermission() && uri.equals(grant.getUri())) {
                return uri;
            }
        }
        Log.i(TAG, "install: saved tree grant is gone; picking again");
        return null;
    }

    private void pickGameFolder() {
        if (busy) {
            return;
        }
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
        intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION
                | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
        Uri initial = savedTree();
        if (initial == null) {
            /* Most users put a game folder in Download; start them there. */
            initial = DocumentsContract.buildDocumentUri(
                    "com.android.externalstorage.documents", "primary:Download");
        }
        intent.putExtra("android.provider.extra.INITIAL_URI", initial);
        try {
            startActivityForResult(intent, REQUEST_PICK_TREE);
            Log.i(TAG, "install: picker opened");
        } catch (RuntimeException e) {
            // Some vendors ship a device without a documents provider.
            showMessage(getString(R.string.copy_failed, e.toString()), true);
        }
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode != REQUEST_PICK_TREE) {
            return;
        }
        if (resultCode != RESULT_OK || data == null || data.getData() == null) {
            showMessage(getString(R.string.pick_cancelled) + "\n\n"
                    + getString(R.string.pick_hint), true);
            return;
        }
        Uri tree = data.getData();
        try {
            getContentResolver().takePersistableUriPermission(tree,
                    Intent.FLAG_GRANT_READ_URI_PERMISSION);
        } catch (SecurityException e) {
            // Not fatal: the copy starts now while the grant is live, and a
            // failed copy simply asks for the folder again next launch.
            Log.w(TAG, "install: persistable grant refused", e);
        }
        prefs().edit().putString(PREF_TREE, tree.toString()).apply();
        startInstall(tree, false);
    }

    private void startInstall(Uri tree, boolean resumed) {
        if (busy) {
            return;
        }
        busy = true;
        copyStartMs = SystemClock.elapsedRealtime();
        showProgress(getString(resumed ? R.string.resuming : R.string.validating));

        new Thread(() -> {
            try {
                GameDataInstaller.Source source = GameDataInstaller.validate(this, tree);
                Log.i(TAG, "install: validated " + source.files + " files, "
                        + source.bytes + " bytes");
                ui.post(() -> {
                    progress.setIndeterminate(false);
                    progress.setProgress(0);
                    status.setText(getString(R.string.copying_progress,
                            GameDataInstaller.human(0), GameDataInstaller.human(source.bytes)));
                });

                GameDataInstaller.Result result = GameDataInstaller.copy(this, tree, source,
                        gameRoot, (files, filesTotal, bytes, total, item) -> ui.post(() -> {
                            if (total <= 0) {
                                return;
                            }
                            progress.setProgress((int) Math.min(1000, bytes * 1000 / total));
                            status.setText(getString(R.string.copying_progress_rate,
                                    GameDataInstaller.human(bytes),
                                    GameDataInstaller.human(total),
                                    GameDataInstaller.human(rate(bytes)) + "/s"));
                        }));

                long elapsed = (SystemClock.elapsedRealtime() - copyStartMs) / 1000;
                ui.post(() -> {
                    busy = false;
                    overlay.setVisibility(View.GONE);
                    Toast.makeText(this, getString(R.string.copy_done_files,
                            GameDataInstaller.human(result.bytes),
                            String.format(Locale.US, "%d s", elapsed),
                            result.files, result.skipped), Toast.LENGTH_LONG).show();
                    Log.i(TAG, "installed " + result.files + " files (" + result.skipped
                            + " already present), " + result.bytes + " bytes in "
                            + result.millis + " ms");
                    nativeGameDataReady(new File(gameRoot, "GameData").getAbsolutePath());
                });
            } catch (GameDataInstaller.PickProblem e) {
                String message = e.message(this);
                Log.w(TAG, "install: rejected selection: " + message);
                ui.post(() -> installFailed(message));
            } catch (GameDataInstaller.SpaceProblem e) {
                String message = getString(R.string.no_space,
                        GameDataInstaller.human(e.needed),
                        GameDataInstaller.human(e.available));
                Log.w(TAG, "install: " + message);
                ui.post(() -> installFailed(message));
            } catch (Exception e) {
                String message = e.getMessage() == null ? e.toString() : e.getMessage();
                Log.e(TAG, "install failed", e);
                ui.post(() -> installFailed(getString(R.string.copy_failed, message)));
            }
        }, "korra-install").start();
    }

    private long rate(long bytes) {
        long seconds = Math.max(1, (SystemClock.elapsedRealtime() - copyStartMs) / 1000);
        return bytes / seconds;
    }

    /** Show why the install stopped and let a tap start over. */
    private void installFailed(String message) {
        busy = false;
        showMessage(message + "\n\n" + getString(R.string.pick_hint), true);
        nativeInstallFailed(message);
    }

    // Implemented in android/app/src/main/cpp/install_android.c.
    private native void nativeGameDataReady(String gameDataPath);

    private native void nativeInstallFailed(String reason);
}
