// 最小可用的 Android 工程配置。真机/厂商 SDK 接入后按需改包名与 targetSdk。
plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "com.sublens.glasses"
    compileSdk = 34

    defaultConfig {
        applicationId = "com.sublens.glasses"
        minSdk = 26          // AAudio 从 Android 8.0 开始
        targetSdk = 34
        versionCode = 1
        versionName = "0.1.0"

        ndk {
            // 眼镜端几乎都是 arm64; 保留 x86_64 方便用模拟器看 UI
            abiFilters += listOf("arm64-v8a", "x86_64")
        }
        externalNativeBuild {
            cmake {
                cppFlags += "-std=c++20"
                arguments += "-DANDROID_STL=c++_shared"
            }
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"))
        }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions { jvmTarget = "17" }
}

dependencies {
    implementation("androidx.core:core-ktx:1.12.0")
}
