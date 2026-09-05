# Android Shell

This directory contains Dusklight's Android shell built on top of Borealis.

## Prerequisites

- Android SDK with Platform 37 installed (`ANDROID_HOME`)
- Android NDK version used by CMake presets (`ANDROID_NDK_VERSION`)
- JDK 17+

Example:

```bash
export ANDROID_HOME="$HOME/Android/Sdk"
export ANDROID_NDK_VERSION="29.0.14206865"
export JAVA_HOME="/usr/lib/jvm/java-17-openjdk"
```

## Build Native Libraries

```bash
cmake --preset android-arm64
cmake --build --preset android-arm64
```

This build produces `build/android-arm64/libmain.so`

## Build APK

```bash
cd platforms/android
./gradlew :app:assembleDebug
```

Output APK:

- `app/build/outputs/apk/debug/app-arm64-v8a-debug.apk`

The debug APK is signed with the standard debug key and can be installed
directly on an Android device. The release output is unsigned unless a
release keystore is configured, so do not distribute the `*-unsigned.apk`
file as an installable download.

Aurora needs a hardware-backed graphics adapter. If an AVD has GPU
acceleration disabled, launch it with `-gpu host`.

## Launch With Runtime Args (adb)

You can pass command-line args through the activity intent:

```bash
adb shell am start -n dev.twilitrealm.dusk/.DuskActivity \
  --es borealis_args "--backend vulkan"
```

Supported extras:

- `borealis_args`: single shell-like argument string
- `borealis_argv`: string-array argv

The legacy `dusk_args` and `dusk_argv` names remain accepted during the shell
transition.
