plugins {
    id("com.android.application")
}

android {
    namespace = "org.korra.recomp"
    compileSdk = 36
    ndkVersion = "28.2.13676358"

    defaultConfig {
        applicationId = "org.korra.recomp"
        minSdk = 29          // ELF TLS for the lifted guest's thread-local state
        targetSdk = 36
        versionCode = 1
        versionName = "0.1.0-m3prep"

        // One ABI. The two target devices (Exynos 2400, Snapdragon 8 Elite) are
        // arm64; a second ABI would only hide 32/64-bit assumptions.
        ndk {
            abiFilters += listOf("arm64-v8a")
        }

        externalNativeBuild {
            cmake {
                arguments += listOf(
                    "-DANDROID_STL=c++_shared",
                    "-DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON",
                    // Flip with -PkorraLinkGuest=ON / -PkorraWithDxvk=ON, or
                    // pass -PkorraDxvkSource=<prepared tree>. See
                    // docs/ANDROID_BRINGUP.md sections 4.5 and 5.3.
                    "-DKORRA_LINK_GUEST=${project.findProperty("korraLinkGuest") ?: "OFF"}",
                    "-DKORRA_WITH_DXVK=${project.findProperty("korraWithDxvk") ?: "OFF"}",
                    "-DKORRA_DXVK_SOURCE=${project.findProperty("korraDxvkSource") ?: ""}",
                )
                cppFlags += listOf("-std=c++17", "-fno-exceptions", "-fno-rtti")
            }
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.31.5"
        }
    }

    // Exposes the game-activity AAR's prefab package to CMake
    // (find_package(game-activity CONFIG)).
    buildFeatures {
        prefab = true
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    buildTypes {
        debug {
            isJniDebuggable = true
        }
        release {
            isMinifyEnabled = false
            isShrinkResources = false
        }
    }

    // Uncompressed, page-aligned native libraries: required for 16 KiB page
    // devices (Android 15+) and for dlopen straight from the APK.
    packaging {
        jniLibs {
            useLegacyPackaging = false
        }
    }
}

dependencies {
    // GameActivity: activity + SurfaceView + native input/lifecycle glue.
    implementation("androidx.games:games-activity:4.4.2")
    // GameActivity extends AppCompatActivity, so the theme and runtime must be
    // present even though this app has no UI of its own.
    implementation("androidx.appcompat:appcompat:1.8.0")
}
