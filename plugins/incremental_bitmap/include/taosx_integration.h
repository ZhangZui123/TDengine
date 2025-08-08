#ifndef TAOSX_INTEGRATION_H
#define TAOSX_INTEGRATION_H

#include <stdint.h>
#include <stdbool.h>

// taosX集成接口
// 用于替代插件中的重复功能

// 内存管理接口
typedef struct {
    uint64_t current_memory_mb;
    uint64_t peak_memory_mb;
    double usage_percent;
} STaosXMemoryStats;

// 日志级别
typedef enum {
    TAOSX_LOG_DEBUG = 0,
    TAOSX_LOG_INFO,
    TAOSX_LOG_WARN,
    TAOSX_LOG_ERROR
} ETaosXLogLevel;

// 内存管理API
int32_t taosx_memory_get_stats(STaosXMemoryStats* stats);
int32_t taosx_memory_cleanup(uint32_t target_memory_mb);
int32_t taosx_memory_set_limit(uint32_t memory_limit_mb);

// 日志API
void taosx_log(ETaosXLogLevel level, const char* format, ...);

// 配置API
int32_t taosx_config_get_int(const char* key, int32_t default_value);
const char* taosx_config_get_string(const char* key, const char* default_value);
bool taosx_config_get_bool(const char* key, bool default_value);

// 持久化API
int32_t taosx_persist_save(const char* path, const void* data, size_t size);
int32_t taosx_persist_load(const char* path, void* data, size_t size);

#endif // TAOSX_INTEGRATION_H
