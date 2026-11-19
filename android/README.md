# Android shell (M3 scaffold)

This directory is the Gradle project for the arm64-v8a APK. It builds and runs
today as a **shell only**: the recompiled guest is not linked, so the app shows
a black screen and logs `guest: STUB - nothing linked`.

Read `docs/ANDROID_BRINGUP.md` before changing anything here; it explains the
GameActivity choice, the NDK build of the lifted C, the DXVK path, and the
`pcmpistri` arm64 problem that must be fixed before the first device frame.

## Build

```powershell
powershell -ExecutionPolicy Bypass -File ..\scripts\android_build.ps1
# APK: android\app\build\outputs\apk\debug\app-debug.apk
```

The script sets `JAVA_HOME` (this machine's default is JDK 8), writes
`local.properties`, and checks the NDK pin. Direct `gradlew` use works once
those are set. Toolchain versions: `docs/TOOLCHAIN.md`.

## Run

```powershell
adb install -r app\build\outputs\apk\debug\app-debug.apk
adb logcat -s Korra
adb pull /sdcard/Android/data/org.korra.recomp/files/log.txt
```

First launch asks for the folder that contains `GameData` and copies it to
`Android/data/org.korra.recomp/files/game/GameData/` (about 2.2 GB; no storage
permission is involved). The copy is repeated only if the `install.complete`
marker or either CPK is missing.

## Layout

| Path | Role |
|---|---|
| `app/src/main/java/org/korra/recomp/MainActivity.java` | GameActivity shell, first-run picker, overlay |
| `app/src/main/java/org/korra/recomp/GameDataInstaller.java` | SAF tree walk + copy |
| `app/src/main/cpp/jni_bridge.cpp` | `android_main`, frame loop, the guest stubs to replace |
| `app/src/main/cpp/log_android.c` | `log.txt` + logcat pump, stdout/stderr redirect |
| `app/src/main/cpp/install_android.c` | install hand-off from Java (JNI) |
| `app/src/main/cpp/perf_android.c` | ADPF + `ANativeWindow_setFrameRate` via `dlsym` |
| `app/src/main/cpp/CMakeLists.txt` | `KORRA_LINK_GUEST`, `KORRA_WITH_DXVK` (both OFF) |

`src/host/`, `work/` and `src/recomp/gen/` are owned by the Windows bring-up
session. The Android host will be a separate tree, `src/host_android/`; do not
edit the Win32 host to make it compile here.
