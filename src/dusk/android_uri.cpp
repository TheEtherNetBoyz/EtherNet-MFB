#include "dusk/android_uri.hpp"

#if defined(__ANDROID__) || defined(ANDROID)
#include <SDL3/SDL_system.h>

#include <jni.h>
#endif

namespace dusk::android {

std::string materialize_content_uri(std::string_view location) {
#if defined(__ANDROID__) || defined(ANDROID)
    if (!location.starts_with("content://")) {
        return std::string{location};
    }

    auto* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
    if (env == nullptr || activity == nullptr) {
        return {};
    }

    jclass activityClass = env->GetObjectClass(activity);
    if (activityClass == nullptr || env->ExceptionCheck()) {
        env->ExceptionClear();
        return {};
    }

    jmethodID copyMethod = env->GetMethodID(activityClass, "copyContentUriToCache",
        "(Ljava/lang/String;)Ljava/lang/String;");
    if (copyMethod == nullptr || env->ExceptionCheck()) {
        env->ExceptionClear();
        env->DeleteLocalRef(activityClass);
        return {};
    }

    const std::string uri{location};
    jstring uriString = env->NewStringUTF(uri.c_str());
    if (uriString == nullptr || env->ExceptionCheck()) {
        env->ExceptionClear();
        env->DeleteLocalRef(activityClass);
        return {};
    }

    auto result = static_cast<jstring>(env->CallObjectMethod(activity, copyMethod, uriString));
    const bool failed = env->ExceptionCheck();
    env->ExceptionClear();

    std::string path;
    if (!failed && result != nullptr) {
        const char* utf8Path = env->GetStringUTFChars(result, nullptr);
        if (utf8Path != nullptr) {
            path = utf8Path;
            env->ReleaseStringUTFChars(result, utf8Path);
        } else {
            env->ExceptionClear();
        }
    }

    if (result != nullptr) {
        env->DeleteLocalRef(result);
    }
    env->DeleteLocalRef(uriString);
    env->DeleteLocalRef(activityClass);
    return path;
#else
    return std::string{location};
#endif
}

}  // namespace dusk::android
