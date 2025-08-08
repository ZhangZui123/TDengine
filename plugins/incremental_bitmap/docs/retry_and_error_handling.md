# taosX 重试参数和异常处理机制

## 概述

本插件实现了taosX要求的重试参数和异常处理机制，确保备份操作的可靠性和稳定性。

## 重试参数

### 配置参数

插件支持以下重试相关配置参数：

```c
typedef struct {
    // taosX要求的重试参数
    uint32_t error_retry_max;       // 最大错误重试次数，默认10
    uint32_t error_retry_interval;  // 错误重试间隔(秒)，默认5s
    
    // 异常处理配置
    char* error_store_path;         // 出错数据存储路径
    bool enable_error_logging;      // 是否启用错误日志记录
    uint32_t error_buffer_size;     // 错误缓冲区大小
} SBackupCoordinatorConfig;
```

### 默认值

- `error.retry.max`: 10 (最大重试次数)
- `error.retry.interval`: 5 (重试间隔，单位：秒)

## 错误码定义

插件定义了完整的错误码体系：

```c
#define BACKUP_SUCCESS                   0
#define BACKUP_ERROR_INVALID_PARAM      -1
#define BACKUP_ERROR_INIT_FAILED        -2
#define BACKUP_ERROR_NOT_INITIALIZED    -3
#define BACKUP_ERROR_MEMORY_ALLOC       -4
#define BACKUP_ERROR_FILE_IO            -5
#define BACKUP_ERROR_NETWORK            -6
#define BACKUP_ERROR_TIMEOUT            -7
#define BACKUP_ERROR_DATA_CORRUPTION    -8
#define BACKUP_ERROR_PERMISSION_DENIED  -9
#define BACKUP_ERROR_DISK_FULL          -10
#define BACKUP_ERROR_CONNECTION_LOST    -11
#define BACKUP_ERROR_RETRY_EXHAUSTED    -12
#define BACKUP_ERROR_UNKNOWN            -99
```

## 可重试错误

以下错误类型支持自动重试：

- `BACKUP_ERROR_NETWORK`: 网络错误
- `BACKUP_ERROR_TIMEOUT`: 操作超时
- `BACKUP_ERROR_CONNECTION_LOST`: 连接丢失
- `BACKUP_ERROR_FILE_IO`: 文件I/O错误

## 重试机制

### 重试上下文

```c
typedef struct {
    uint32_t current_retry;     // 当前重试次数
    uint32_t max_retry;         // 最大重试次数
    uint32_t retry_interval;    // 重试间隔(秒)
    uint64_t last_retry_time;   // 上次重试时间
    ERetryState state;          // 重试状态
    int32_t last_error;         // 最后一次错误码
    char* error_message;        // 错误信息
} SRetryContext;
```

### 重试状态

```c
typedef enum {
    RETRY_STATE_IDLE = 0,      // 空闲状态
    RETRY_STATE_RETRYING,      // 重试中
    RETRY_STATE_SUCCESS,       // 重试成功
    RETRY_STATE_FAILED         // 重试失败
} ERetryState;
```

## 使用示例

### 1. 基本重试操作

```c
// 定义操作函数
static int32_t my_operation(void* user_data) {
    // 执行可能失败的操作
    if (operation_fails) {
        return BACKUP_ERROR_NETWORK;
    }
    return BACKUP_SUCCESS;
}

// 使用重试机制
SRetryContext retry_ctx;
backup_retry_context_init(&retry_ctx, 5, 2); // 最多重试5次，间隔2秒

int32_t result = backup_execute_with_retry(&retry_ctx, my_operation, NULL);
if (result == BACKUP_SUCCESS) {
    printf("操作成功\n");
} else {
    printf("操作失败: %s\n", get_error_message(result));
}
```

### 2. 带重试的文件写入

```c
SBackupCoordinator* coordinator = backup_coordinator_init(&config, ...);

const char* data = "Hello, World!";
int32_t result = backup_write_file_with_retry(coordinator, 
                                             "/path/to/file.txt", 
                                             data, 
                                             strlen(data));

if (result != BACKUP_SUCCESS) {
    const char* error_msg = backup_get_last_error(coordinator);
    printf("文件写入失败: %s\n", error_msg);
}
```

### 3. 错误记录和统计

```c
// 记录错误
backup_record_error(coordinator, BACKUP_ERROR_NETWORK, "网络连接失败");

// 获取错误信息
const char* error_msg = backup_get_last_error(coordinator);
printf("最后一次错误: %s\n", error_msg);

// 获取错误统计
uint64_t error_count, retry_count;
backup_get_error_stats(coordinator, &error_count, &retry_count);
printf("错误次数: %lu, 重试次数: %lu\n", error_count, retry_count);

// 清除错误信息
backup_clear_error(coordinator);
```

## 插件接口

### 错误信息接口

```c
// 获取最后一次错误信息
const char* backup_plugin_get_last_error(void);

// 获取错误统计信息
void backup_plugin_get_error_stats(uint64_t* error_count, uint64_t* retry_count);

// 清除错误信息
void backup_plugin_clear_error(void);
```

### 使用示例

```c
// 初始化插件
backup_plugin_init("config_string", config_len);

// 执行备份操作
// ...

// 检查错误
const char* error_msg = backup_plugin_get_last_error();
if (strcmp(error_msg, "Success") != 0) {
    printf("备份过程中出现错误: %s\n", error_msg);
}

// 获取统计信息
uint64_t error_count, retry_count;
backup_plugin_get_error_stats(&error_count, &retry_count);
printf("总错误次数: %lu, 总重试次数: %lu\n", error_count, retry_count);
```

## 配置示例

### taosX配置文件

```toml
[plugins.incremental_bitmap]
enable = true
error_retry_max = 10
error_retry_interval = 5
error_store_path = "/var/log/taosx/backup_errors"
enable_error_logging = true
error_buffer_size = 1000
```

### 程序配置

```c
SBackupCoordinatorConfig config = {
    .max_blocks_per_batch = 1000,
    .batch_timeout_ms = 5000,
    .enable_compression = true,
    .enable_encryption = false,
    .encryption_key = NULL,
    .error_retry_max = 10,           // 最大重试次数
    .error_retry_interval = 5,        // 重试间隔5秒
    .error_store_path = "/tmp/errors", // 错误存储路径
    .enable_error_logging = true,     // 启用错误日志
    .error_buffer_size = 1000,        // 错误缓冲区大小
    .backup_path = "/backup",         // 备份路径
    .backup_max_size = 1024 * 1024 * 1024, // 1GB
    .compression_level = 1            // fastest
};
```

## 错误日志

当启用错误日志记录时，错误信息会写入到指定的错误存储路径：

```
/var/log/taosx/backup_errors/backup_error_1640995200000.log
```

日志格式：
```
[1640995200000] Error -6: Network connection failed
[1640995201000] Error -5: File I/O error
```

## 测试

运行测试以验证重试和异常处理机制：

```bash
cd plugins/incremental_bitmap
make test_retry_mechanism
./test_retry_mechanism
```

测试包括：
- 重试机制测试
- 错误记录测试
- 插件接口测试
- 带重试的文件写入测试

## 最佳实践

1. **合理设置重试参数**：
   - 网络操作：重试次数5-10次，间隔2-5秒
   - 文件操作：重试次数3-5次，间隔1-2秒

2. **监控错误统计**：
   - 定期检查错误次数和重试次数
   - 根据错误模式调整重试策略

3. **错误日志管理**：
   - 定期清理错误日志文件
   - 设置日志轮转策略

4. **资源清理**：
   - 及时清理错误信息和重试上下文
   - 避免内存泄漏

## 兼容性

本实现完全兼容taosX的重试参数要求：
- `error.retry.max`: 支持配置最大重试次数
- `error.retry.interval`: 支持配置重试间隔

同时提供了扩展的错误处理功能，确保备份操作的可靠性和可观测性。 