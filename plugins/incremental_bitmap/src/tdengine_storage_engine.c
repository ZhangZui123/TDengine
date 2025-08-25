#include "storage_engine_interface.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>

// TDengine存储引擎状态
typedef struct {
    bool initialized;
    bool interception_installed;
    uint64_t events_processed;
    uint64_t events_dropped;
    pthread_mutex_t mutex;
    
    // TDengine相关
    char* data_dir;
    char* wal_dir;
    void* taos_connection;
    
    // 事件回调
    StorageEventCallback event_callback;
    void* callback_user_data;
} STdengineStorageEngine;

static STdengineStorageEngine g_tdengine_engine = {
    .initialized = false,
    .interception_installed = false,
    .events_processed = 0,
    .events_dropped = 0,
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .data_dir = NULL,
    .wal_dir = NULL,
    .taos_connection = NULL,
    .event_callback = NULL,
    .callback_user_data = NULL
};

// TDengine存储引擎实现函数
static int32_t tdengine_init(const SStorageEngineConfig* config) {
    if (!config) {
        return -1;
    }
    
    pthread_mutex_lock(&g_tdengine_engine.mutex);
    
    // 保存回调函数
    g_tdengine_engine.event_callback = config->event_callback;
    g_tdengine_engine.callback_user_data = config->callback_user_data;
    
    // 获取TDengine数据目录
    const char* data_dir = getenv("TDENGINE_DATA_DIR");
    if (!data_dir) {
        data_dir = "/var/lib/taos";
    }
    
    g_tdengine_engine.data_dir = strdup(data_dir);
    g_tdengine_engine.wal_dir = strdup(data_dir); // WAL通常在同一目录
    
    g_tdengine_engine.initialized = true;
    g_tdengine_engine.events_processed = 0;
    g_tdengine_engine.events_dropped = 0;
    
    pthread_mutex_unlock(&g_tdengine_engine.mutex);
    
    printf("[TDengine] 存储引擎初始化成功，数据目录: %s\n", data_dir);
    return 0;
}

static void tdengine_destroy(void) {
    pthread_mutex_lock(&g_tdengine_engine.mutex);
    
    if (g_tdengine_engine.data_dir) {
        free(g_tdengine_engine.data_dir);
        g_tdengine_engine.data_dir = NULL;
    }
    
    if (g_tdengine_engine.wal_dir) {
        free(g_tdengine_engine.wal_dir);
        g_tdengine_engine.wal_dir = NULL;
    }
    
    g_tdengine_engine.initialized = false;
    g_tdengine_engine.interception_installed = false;
    
    pthread_mutex_unlock(&g_tdengine_engine.mutex);
    
    printf("[TDengine] 存储引擎销毁完成\n");
}

// 监控WAL文件变化
static int32_t monitor_wal_changes(const char* wal_dir, StorageEventCallback callback, void* user_data) {
    DIR* dir = opendir(wal_dir);
    if (!dir) {
        printf("[TDengine] 无法打开WAL目录: %s\n", wal_dir);
        return -1;
    }
    
    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type == DT_REG && strstr(entry->d_name, ".wal") != NULL) {
            char filepath[PATH_MAX];
            snprintf(filepath, sizeof(filepath), "%s/%s", wal_dir, entry->d_name);
            
            struct stat st;
            if (stat(filepath, &st) == 0) {
                // 检测文件变化
                SStorageEvent event = {
                    .event_type = STORAGE_EVENT_BLOCK_UPDATE,
                    .block_id = (uint64_t)st.st_ino,
                    .wal_offset = (uint64_t)st.st_size,
                    .timestamp = (int64_t)st.st_mtime * 1000000000LL,
                    .user_data = NULL
                };
                
                if (callback) {
                    callback(&event, user_data);
                }
            }
        }
    }
    
    closedir(dir);
    return 0;
}

static int32_t tdengine_install_interception(void) {
    pthread_mutex_lock(&g_tdengine_engine.mutex);
    if (!g_tdengine_engine.initialized) {
        pthread_mutex_unlock(&g_tdengine_engine.mutex);
        return -1;
    }
    
    g_tdengine_engine.interception_installed = true;
    pthread_mutex_unlock(&g_tdengine_engine.mutex);
    
    // 启动WAL监控线程
    pthread_t monitor_thread;
    pthread_create(&monitor_thread, NULL, (void* (*)(void*))monitor_wal_changes, 
                   g_tdengine_engine.wal_dir);
    
    printf("[TDengine] 事件拦截安装成功，开始监控WAL变化\n");
    return 0;
}

static int32_t tdengine_uninstall_interception(void) {
    pthread_mutex_lock(&g_tdengine_engine.mutex);
    g_tdengine_engine.interception_installed = false;
    pthread_mutex_unlock(&g_tdengine_engine.mutex);
    
    printf("[TDengine] 事件拦截卸载成功\n");
    return 0;
}

static int32_t tdengine_trigger_event(const SStorageEvent* event) {
    if (!event) {
        return -1;
    }
    
    pthread_mutex_lock(&g_tdengine_engine.mutex);
    if (!g_tdengine_engine.interception_installed) {
        pthread_mutex_unlock(&g_tdengine_engine.mutex);
        return -1;
    }
    
    g_tdengine_engine.events_processed++;
    pthread_mutex_unlock(&g_tdengine_engine.mutex);
    
    printf("[TDengine] 触发事件: 类型=%d, 块ID=%lu, WAL偏移量=%lu, 时间戳=%ld\n",
           event->event_type, event->block_id, event->wal_offset, event->timestamp);
    
    // 调用回调函数
    if (g_tdengine_engine.event_callback) {
        g_tdengine_engine.event_callback(event, g_tdengine_engine.callback_user_data);
    }
    
    return 0;
}

static int32_t tdengine_get_stats(uint64_t* events_processed, uint64_t* events_dropped) {
    pthread_mutex_lock(&g_tdengine_engine.mutex);
    
    if (events_processed) {
        *events_processed = g_tdengine_engine.events_processed;
    }
    if (events_dropped) {
        *events_dropped = g_tdengine_engine.events_dropped;
    }
    
    pthread_mutex_unlock(&g_tdengine_engine.mutex);
    return 0;
}

static bool tdengine_is_supported(void) {
    // 检查TDengine是否运行
    const char* data_dir = getenv("TDENGINE_DATA_DIR");
    if (!data_dir) {
        data_dir = "/var/lib/taos";
    }
    
    DIR* dir = opendir(data_dir);
    if (dir) {
        closedir(dir);
        return true;
    }
    
    return false;
}

static const char* tdengine_get_engine_name(void) {
    return "tdengine";
}

// TDengine存储引擎接口
static SStorageEngineInterface g_tdengine_interface = {
    .init = tdengine_init,
    .destroy = tdengine_destroy,
    .install_interception = tdengine_install_interception,
    .uninstall_interception = tdengine_uninstall_interception,
    .trigger_event = tdengine_trigger_event,
    .get_stats = tdengine_get_stats,
    .is_supported = tdengine_is_supported,
    .get_engine_name = tdengine_get_engine_name
};

// TDengine存储引擎工厂函数
SStorageEngineInterface* tdengine_storage_engine_create(void) {
    return &g_tdengine_interface;
}

// 便捷函数：注册TDengine存储引擎
int32_t register_tdengine_storage_engine(void) {
    extern int32_t register_storage_engine_interface(const char* name, StorageEngineInterfaceFactory factory);
    return register_storage_engine_interface("tdengine", tdengine_storage_engine_create);
}

// TDengine特定的数据块读取函数
int32_t tdengine_read_data_block(uint64_t block_id, void** data, uint32_t* size) {
    // 这里需要实现从TDengine读取数据块的逻辑
    // 由于需要访问TDengine内部API，这里只是框架
    
    printf("[TDengine] 读取数据块: ID=%lu\n", block_id);
    
    // 模拟数据读取
    *size = 1024;
    *data = malloc(*size);
    if (*data) {
        memset(*data, 0, *size);
        return 0;
    }
    
    return -1;
}

// TDengine特定的WAL监控函数
int32_t tdengine_monitor_wal_changes(const char* wal_path, 
                                    void (*callback)(const char* filename, uint64_t offset, int64_t timestamp)) {
    DIR* dir = opendir(wal_path);
    if (!dir) {
        return -1;
    }
    
    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type == DT_REG && strstr(entry->d_name, ".wal") != NULL) {
            char filepath[PATH_MAX];
            snprintf(filepath, sizeof(filepath), "%s/%s", wal_path, entry->d_name);
            
            struct stat st;
            if (stat(filepath, &st) == 0) {
                if (callback) {
                    callback(entry->d_name, (uint64_t)st.st_size, 
                            (int64_t)st.st_mtime * 1000000000LL);
                }
            }
        }
    }
    
    closedir(dir);
    return 0;
}

