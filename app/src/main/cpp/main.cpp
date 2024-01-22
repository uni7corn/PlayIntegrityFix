#include <android/log.h>
#include <unistd.h>
#include <map>
#include <fstream>
#include "zygisk.hpp"
#include "shadowhook.h"
#include "json.hpp"

#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, "PIF", __VA_ARGS__)

#define CLASSES_DEX "/data/adb/modules/playintegrityfix/classes.dex"

#define PIF_JSON "/data/adb/pif.json"

static JavaVM *jvm = nullptr;

static jclass entryPointClass = nullptr;

static jmethodID spoofFieldsMethod = nullptr;

static void spoofFields() {

    JNIEnv *env;
    jvm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6);

    if (env != nullptr && entryPointClass != nullptr && spoofFieldsMethod != nullptr) {

        env->CallStaticVoidMethod(entryPointClass, spoofFieldsMethod);

        LOGD("[Zygisk] Call Java spoofFields!");
    }

    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
    }
}

static std::string FIRST_API_LEVEL, SECURITY_PATCH, BUILD_ID;

typedef void (*T_Callback)(void *, const char *, const char *, uint32_t);

static std::map<void *, T_Callback> callbacks;

static void modify_callback(void *cookie, const char *name, const char *value, uint32_t serial) {

    if (cookie == nullptr || name == nullptr || value == nullptr ||
        !callbacks.contains(cookie))
        return;

    std::string_view prop(name);

    if (prop.ends_with("security_patch")) {

        if (!SECURITY_PATCH.empty()) {

            value = SECURITY_PATCH.c_str();
        }

    } else if (prop.ends_with("api_level")) {

        if (!FIRST_API_LEVEL.empty()) {

            value = FIRST_API_LEVEL.c_str();
        }

    } else if (prop.ends_with("build.id")) {

        if (!BUILD_ID.empty()) {

            value = BUILD_ID.c_str();
        }

    } else if (prop == "sys.usb.state") {

        value = "none";
    }

    if (prop == "ro.product.first_api_level") spoofFields();

    if (!prop.starts_with("debug") && !prop.starts_with("cache") && !prop.starts_with("persist")) {

        LOGD("[%s] -> %s", name, value);
    }

    return callbacks[cookie](cookie, name, value, serial);
}

static void (*o_system_property_read_callback)(const void *, T_Callback, void *);

static void
my_system_property_read_callback(const void *pi, T_Callback callback, void *cookie) {
    if (pi == nullptr || callback == nullptr || cookie == nullptr) {
        return o_system_property_read_callback(pi, callback, cookie);
    }
    callbacks[cookie] = callback;
    return o_system_property_read_callback(pi, modify_callback, cookie);
}

static void doHook() {
    void *handle = shadowhook_hook_sym_name("libc.so", "__system_property_read_callback",
                                            reinterpret_cast<void *>(my_system_property_read_callback),
                                            reinterpret_cast<void **>(&o_system_property_read_callback));
    if (handle == nullptr) {
        LOGD("Couldn't find '__system_property_read_callback' handle. Report to @chiteroman");
        return;
    }
    LOGD("Found '__system_property_read_callback' handle at %p", handle);
}

class PlayIntegrityFix : public zygisk::ModuleBase {
public:
    void onLoad(zygisk::Api *api, JNIEnv *env) override {
        this->api = api;
        this->env = env;
        env->GetJavaVM(&jvm);
    }

    void preAppSpecialize(zygisk::AppSpecializeArgs *args) override {

        auto rawProcess = env->GetStringUTFChars(args->nice_name, nullptr);
        auto rawDir = env->GetStringUTFChars(args->app_data_dir, nullptr);

        std::string_view process(rawProcess);
        std::string_view dir(rawDir);

        bool isGms = dir.ends_with("/com.google.android.gms");
        bool isGmsUnstable = process == "com.google.android.gms.unstable";

        env->ReleaseStringUTFChars(args->nice_name, rawProcess);
        env->ReleaseStringUTFChars(args->app_data_dir, rawDir);

        if (!isGms) {
            api->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
            return;
        }

        api->setOption(zygisk::FORCE_DENYLIST_UNMOUNT);

        if (!isGmsUnstable) {
            api->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
            return;
        }

        long dexSize = 0, jsonSize = 0;

        int fd = api->connectCompanion();

        read(fd, &dexSize, sizeof(long));
        read(fd, &jsonSize, sizeof(long));

        LOGD("Dex file size: %ld", dexSize);
        LOGD("Json file size: %ld", jsonSize);

        if (dexSize > 0) {
            vector.resize(dexSize);
            read(fd, vector.data(), dexSize);
        } else {
            close(fd);
            LOGD("Dex file empty!");
            api->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
            return;
        }

        std::vector<char> jsonVector;
        if (jsonSize > 0) {
            jsonVector.resize(jsonSize);
            read(fd, jsonVector.data(), jsonSize);
        } else {
            close(fd);
            LOGD("Json file empty!");
            api->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
            return;
        }

        close(fd);

        std::string_view jsonStr(jsonVector.cbegin(), jsonVector.cend());
        json = nlohmann::json::parse(jsonStr, nullptr, false, true);

        parseJson();
    }

    void postAppSpecialize(const zygisk::AppSpecializeArgs *args) override {
        if (vector.empty() || json.empty()) return;

        shadowhook_init(SHADOWHOOK_MODE_UNIQUE, true);

        LOGD("JSON keys: %d", static_cast<int>(json.size()));

        if (!json.contains("PRODUCT") ||
            !json.contains("DEVICE") ||
            !json.contains("MANUFACTURER") ||
            !json.contains("BRAND") ||
            !json.contains("MODEL") ||
            !json.contains("FINGERPRINT")) {
            LOGD("JSON doesn't contain important fields to spoof!");
            return;
        }

        injectDex();

        doHook();

        vector.clear();
        json.clear();
    }

    void preServerSpecialize(zygisk::ServerSpecializeArgs *args) override {
        api->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
    }

private:
    zygisk::Api *api = nullptr;
    JNIEnv *env = nullptr;
    std::vector<char> vector;
    nlohmann::json json;

    void injectDex() {
        LOGD("get system classloader");
        auto clClass = env->FindClass("java/lang/ClassLoader");
        auto getSystemClassLoader = env->GetStaticMethodID(clClass, "getSystemClassLoader",
                                                           "()Ljava/lang/ClassLoader;");
        auto systemClassLoader = env->CallStaticObjectMethod(clClass, getSystemClassLoader);

        LOGD("create class loader");
        auto dexClClass = env->FindClass("dalvik/system/InMemoryDexClassLoader");
        auto dexClInit = env->GetMethodID(dexClClass, "<init>",
                                          "(Ljava/nio/ByteBuffer;Ljava/lang/ClassLoader;)V");
        auto buffer = env->NewDirectByteBuffer(vector.data(), vector.size());
        auto dexCl = env->NewObject(dexClClass, dexClInit, buffer, systemClassLoader);

        LOGD("load class");
        auto loadClass = env->GetMethodID(clClass, "loadClass",
                                          "(Ljava/lang/String;)Ljava/lang/Class;");
        auto entryClassName = env->NewStringUTF("es.chiteroman.playintegrityfix.EntryPoint");
        auto entryClassObj = env->CallObjectMethod(dexCl, loadClass, entryClassName);

        entryPointClass = (jclass) entryClassObj;

        LOGD("call init");
        auto entryInit = env->GetStaticMethodID(entryPointClass, "init", "(Ljava/lang/String;)V");
        auto str = env->NewStringUTF(json.dump().c_str());
        env->CallStaticVoidMethod(entryPointClass, entryInit, str);

        spoofFieldsMethod = env->GetStaticMethodID(entryPointClass, "spoofFields", "()V");
    }

    void parseJson() {
        if (json.contains("FIRST_API_LEVEL")) {

            if (json["FIRST_API_LEVEL"].is_number_integer()) {

                FIRST_API_LEVEL = std::to_string(json["FIRST_API_LEVEL"].get<int>());

            } else if (json["FIRST_API_LEVEL"].is_string()) {

                FIRST_API_LEVEL = json["FIRST_API_LEVEL"].get<std::string>();
            }

            json.erase("FIRST_API_LEVEL");

        } else {

            LOGD("JSON file doesn't contain FIRST_API_LEVEL key :(");
        }

        if (json.contains("SECURITY_PATCH")) {

            if (json["SECURITY_PATCH"].is_string()) {

                SECURITY_PATCH = json["SECURITY_PATCH"].get<std::string>();
            }

        } else {

            LOGD("JSON file doesn't contain SECURITY_PATCH key :(");
        }

        if (json.contains("ID")) {

            if (json["ID"].is_string()) {

                BUILD_ID = json["ID"].get<std::string>();
            }

        } else if (json.contains("BUILD_ID")) {

            if (json["BUILD_ID"].is_string()) {

                BUILD_ID = json["BUILD_ID"].get<std::string>();
            }

        } else {

            LOGD("JSON file doesn't contain ID/BUILD_ID keys :(");
        }
    }
};

static void companion(int fd) {

    std::ifstream dexFile(CLASSES_DEX, std::ios::binary);
    std::ifstream jsonFile(PIF_JSON);

    std::vector<char> dexVector((std::istreambuf_iterator<char>(dexFile)),
                                std::istreambuf_iterator<char>());
    std::vector<char> jsonVector((std::istreambuf_iterator<char>(jsonFile)),
                                 std::istreambuf_iterator<char>());

    long dexSize = dexVector.size();
    long jsonSize = jsonVector.size();

    write(fd, &dexSize, sizeof(long));
    write(fd, &jsonSize, sizeof(long));

    write(fd, dexVector.data(), dexSize);
    write(fd, jsonVector.data(), jsonSize);
}

REGISTER_ZYGISK_MODULE(PlayIntegrityFix)

REGISTER_ZYGISK_COMPANION(companion)