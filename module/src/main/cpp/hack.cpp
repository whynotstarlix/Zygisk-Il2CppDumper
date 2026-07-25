//
// Runtime memory dumper for protected/obfuscated Unity IL2CPP games
// (Standoff 2 and similar)
//

#include "hack.h"
#include "log.h"
#include "dobby.h"

#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <dlfcn.h>
#include <jni.h>
#include <thread>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <cinttypes>
#include <string>
#include <vector>
#include <mutex>
#include <algorithm>

static char g_game_data_dir[512] = {0};
static std::mutex g_dump_mutex;

// ===================== helpers =====================

static void ensure_dir(const char *path) {
    mkdir(path, 0777);
}

static bool write_file(const char *path, const void *data, size_t size) {
    int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0666);
    if (fd < 0) {
        LOGE("[DUMP] open(%s) failed: %s", path, strerror(errno));
        return false;
    }
    ssize_t w = write(fd, data, size);
    close(fd);
    if (w != (ssize_t)size) {
        LOGE("[DUMP] write incomplete %zd / %zu → %s", w, size, path);
        return false;
    }
    LOGI("[DUMP] ✓ %zu bytes → %s", size, path);
    return true;
}

static void dump_region(void *addr, size_t len, const char *tag) {
    if (!addr || len < 256 * 1024) return;
    if (len > 512 * 1024 * 1024) return; // защита от совсем бешеного

    std::lock_guard<std::mutex> lock(g_dump_mutex);

    char name[128];
    snprintf(name, sizeof(name), "%s_%p_%zu.bin", tag, addr, len);

    // 1) в files/ игры
    if (g_game_data_dir[0]) {
        char dir[600], path[700];
        snprintf(dir, sizeof(dir), "%s/files", g_game_data_dir);
        ensure_dir(dir);
        snprintf(path, sizeof(path), "%s/%s", dir, name);
        write_file(path, addr, len);
    }

    // 2) дубль на /sdcard
    {
        ensure_dir("/sdcard/Download");
        ensure_dir("/sdcard/Download/SO2_dump");
        char path[512];
        snprintf(path, sizeof(path), "/sdcard/Download/SO2_dump/%s", name);
        write_file(path, addr, len);
    }
}

// ===================== mprotect =====================

static int (*orig_mprotect)(void *addr, size_t len, int prot) = nullptr;

static int fake_mprotect(void *addr, size_t len, int prot) {
    if ((prot & PROT_EXEC) && len >= 256 * 1024) {
        LOGI("[DUMP] mprotect(%p, %zu, 0x%x)", addr, len, prot);
        dump_region(addr, len, "mprotect");
    }
    return orig_mprotect(addr, len, prot);
}

// ===================== mmap / mmap64 =====================

static void *(*orig_mmap)(void *, size_t, int, int, int, off_t) = nullptr;
static void *(*orig_mmap64)(void *, size_t, int, int, int, off64_t) = nullptr;

static void *fake_mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset) {
    void *res = orig_mmap(addr, length, prot, flags, fd, offset);
    if (res != MAP_FAILED && (prot & PROT_EXEC) && length >= 256 * 1024) {
        LOGI("[DUMP] mmap(%p, %zu, 0x%x) → %p", addr, length, prot, res);
        dump_region(res, length, "mmap");
    }
    return res;
}

static void *fake_mmap64(void *addr, size_t length, int prot, int flags, int fd, off64_t offset) {
    void *res = orig_mmap64(addr, length, prot, flags, fd, offset);
    if (res != MAP_FAILED && (prot & PROT_EXEC) && length >= 256 * 1024) {
        LOGI("[DUMP] mmap64(%p, %zu, 0x%x) → %p", addr, length, prot, res);
        dump_region(res, length, "mmap64");
    }
    return res;
}

// ===================== dump from /proc/self/maps =====================

struct MapEntry {
    uintptr_t start;
    uintptr_t end;
    char perms[8];
    char path[256];
};

static std::vector<MapEntry> parse_maps() {
    std::vector<MapEntry> maps;
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return maps;

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        MapEntry e{};
        e.path[0] = 0;
        if (sscanf(line, "%" SCNxPTR "-%" SCNxPTR " %7s %*s %*s %*s %255s",
                   &e.start, &e.end, e.perms, e.path) >= 3) {
            maps.push_back(e);
        }
    }
    fclose(f);
    return maps;
}

static void dump_library_from_maps(const char *libname) {
    auto maps = parse_maps();
    uintptr_t start = 0, end = 0;
    bool found = false;

    for (auto &m : maps) {
        if (strstr(m.path, libname)) {
            if (!found) {
                start = m.start;
                end = m.end;
                found = true;
            } else {
                // склеиваем непрерывные сегменты одной библиотеки
                if (m.start <= end + 0x1000) {
                    end = std::max(end, m.end);
                }
            }
        }
    }

    if (!found) {
        LOGI("[DUMP] %s not found in maps yet", libname);
        return;
    }

    size_t size = end - start;
    LOGI("[DUMP] %s in memory: %p - %p (%zu bytes)", libname, (void *)start, (void *)end, size);

    if (size >= 1024 * 1024) {
        dump_region((void *)start, size, libname);
    }
}

// ===================== background scanner =====================

static void scanner_thread() {
    LOGI("[DUMP] Scanner thread started");

    const char *targets[] = {
        "libunity.so",
        "libil2cpp.so",
        "libmain.so",
        nullptr
    };

    // несколько проходов — библиотеки подгружаются не сразу
    for (int pass = 0; pass < 40; ++pass) {
        for (int t = 0; targets[t]; ++t) {
            dump_library_from_maps(targets[t]);
        }
        sleep(3);
    }
    LOGI("[DUMP] Scanner finished");
}

// ===================== install hooks =====================

static void setup_hooks() {
    LOGI("[DUMP] Installing hooks...");

    void *p = dlsym(RTLD_DEFAULT, "mprotect");
    if (p) {
        DobbyHook(p,
                  reinterpret_cast<dobby_dummy_func_t>(fake_mprotect),
                  reinterpret_cast<dobby_dummy_func_t *>(&orig_mprotect));
        LOGI("[DUMP] mprotect hooked @ %p", p);
    }

    p = dlsym(RTLD_DEFAULT, "mmap");
    if (p) {
        DobbyHook(p,
                  reinterpret_cast<dobby_dummy_func_t>(fake_mmap),
                  reinterpret_cast<dobby_dummy_func_t *>(&orig_mmap));
        LOGI("[DUMP] mmap hooked @ %p", p);
    }

    p = dlsym(RTLD_DEFAULT, "mmap64");
    if (p) {
        DobbyHook(p,
                  reinterpret_cast<dobby_dummy_func_t>(fake_mmap64),
                  reinterpret_cast<dobby_dummy_func_t *>(&orig_mmap64));
        LOGI("[DUMP] mmap64 hooked @ %p", p);
    }
}

// ===================== entry =====================

void hack_start(const char *game_data_dir) {
    if (game_data_dir && game_data_dir[0]) {
        snprintf(g_game_data_dir, sizeof(g_game_data_dir), "%s", game_data_dir);
        LOGI("[DUMP] game_data_dir = %s", g_game_data_dir);
    } else {
        LOGI("[DUMP] game_data_dir empty → only /sdcard/Download/SO2_dump");
    }

    setup_hooks();

    // фоновый сканер maps
    std::thread(scanner_thread).detach();

    LOGI("[DUMP] Runtime dumper ready (tid %d)", gettid());
}

// ===================== остальной код (NativeBridge и т.д.) =====================

#include <sys/system_properties.h>
#include <array>
#include <linux/unistd.h>

std::string GetLibDir(JavaVM *vms) {
    JNIEnv *env = nullptr;
    vms->AttachCurrentThread(&env, nullptr);
    jclass activity_thread_clz = env->FindClass("android/app/ActivityThread");
    if (activity_thread_clz != nullptr) {
        jmethodID currentApplicationId = env->GetStaticMethodID(activity_thread_clz,
                                                                "currentApplication",
                                                                "()Landroid/app/Application;");
        if (currentApplicationId) {
            jobject application = env->CallStaticObjectMethod(activity_thread_clz, currentApplicationId);
            jclass application_clazz = env->GetObjectClass(application);
            if (application_clazz) {
                jmethodID get_application_info = env->GetMethodID(application_clazz,
                                                                  "getApplicationInfo",
                                                                  "()Landroid/content/pm/ApplicationInfo;");
                if (get_application_info) {
                    jobject application_info = env->CallObjectMethod(application, get_application_info);
                    jfieldID native_library_dir_id = env->GetFieldID(
                            env->GetObjectClass(application_info), "nativeLibraryDir", "Ljava/lang/String;");
                    if (native_library_dir_id) {
                        auto native_library_dir_jstring = (jstring) env->GetObjectField(application_info, native_library_dir_id);
                        auto path = env->GetStringUTFChars(native_library_dir_jstring, nullptr);
                        LOGI("lib dir %s", path);
                        std::string lib_dir(path);
                        env->ReleaseStringUTFChars(native_library_dir_jstring, path);
                        return lib_dir;
                    }
                }
            }
        }
    }
    return {};
}

static std::string GetNativeBridgeLibrary() {
    auto value = std::array<char, PROP_VALUE_MAX>();
    __system_property_get("ro.dalvik.vm.native.bridge", value.data());
    return {value.data()};
}

struct NativeBridgeCallbacks {
    uint32_t version;
    void *initialize;
    void *(*loadLibrary)(const char *libpath, int flag);
    void *(*getTrampoline)(void *handle, const char *name, const char *shorty, uint32_t len);
    void *isSupported;
    void *getAppEnv;
    void *isCompatibleWith;
    void *getSignalHandler;
    void *unloadLibrary;
    void *getError;
    void *isPathSupported;
    void *initAnonymousNamespace;
    void *createNamespace;
    void *linkNamespaces;
    void *(*loadLibraryExt)(const char *libpath, int flag, void *ns);
};

bool NativeBridgeLoad(const char *game_data_dir, int api_level, void *data, size_t length) {
    sleep(5);
    auto libart = dlopen("libart.so", RTLD_NOW);
    auto JNI_GetCreatedJavaVMs = (jint (*)(JavaVM **, jsize, jsize *)) dlsym(libart, "JNI_GetCreatedJavaVMs");
    JavaVM *vms_buf[1];
    JavaVM *vms;
    jsize num_vms;
    if (JNI_GetCreatedJavaVMs(vms_buf, 1, &num_vms) != JNI_OK || num_vms <= 0) return false;
    vms = vms_buf[0];

    auto lib_dir = GetLibDir(vms);
    if (lib_dir.empty() || lib_dir.find("/lib/x86") != std::string::npos) {
        munmap(data, length);
        return false;
    }

    auto nb = dlopen("libhoudini.so", RTLD_NOW);
    if (!nb) {
        auto native_bridge = GetNativeBridgeLibrary();
        nb = dlopen(native_bridge.data(), RTLD_NOW);
    }
    if (!nb) return false;

    auto callbacks = (NativeBridgeCallbacks *) dlsym(nb, "NativeBridgeItf");
    if (!callbacks) return false;

    int fd = syscall(__NR_memfd_create, "anon", MFD_CLOEXEC);
    ftruncate(fd, (off_t) length);
    void *mem = mmap(nullptr, length, PROT_WRITE, MAP_SHARED, fd, 0);
    memcpy(mem, data, length);
    munmap(mem, length);
    munmap(data, length);

    char path[PATH_MAX];
    snprintf(path, PATH_MAX, "/proc/self/fd/%d", fd);

    void *arm_handle = (api_level >= 26)
                       ? callbacks->loadLibraryExt(path, RTLD_NOW, (void *) 3)
                       : callbacks->loadLibrary(path, RTLD_NOW);

    if (arm_handle) {
        auto init = (void (*)(JavaVM *, void *)) callbacks->getTrampoline(arm_handle, "JNI_OnLoad", nullptr, 0);
        if (init) init(vms, (void *) game_data_dir);
        return true;
    }
    close(fd);
    return false;
}

void hack_prepare(const char *game_data_dir, void *data, size_t length) {
    LOGI("hack thread: %d", gettid());
    int api_level = android_get_device_api_level();
    LOGI("api level: %d", api_level);

#if defined(__i386__) || defined(__x86_64__)
    if (!NativeBridgeLoad(game_data_dir, api_level, data, length)) {
#endif
    hack_start(game_data_dir);
#if defined(__i386__) || defined(__x86_64__)
    }
#endif
}

#if defined(__arm__) || defined(__aarch64__)
JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved) {
    auto game_data_dir = (const char *) reserved;
    std::thread(hack_start, game_data_dir).detach();
    return JNI_VERSION_1_6;
}
#endif
