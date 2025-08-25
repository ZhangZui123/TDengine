# TDengine增量位图插件集成指南

## 集成方案

### 方案一：作为独立工具集成（推荐）

将位图插件作为独立的增量备份工具，与taosdump配合使用：

```bash
# 1. 使用位图插件进行增量检测
./incremental_bitmap_tool --config bitmap_config.json --output incremental_blocks.json

# 2. 使用taosdump进行数据导出
taosdump -h localhost -P 6030 -D dbname -o /backup/full/

# 3. 使用位图插件进行增量数据导出
./incremental_bitmap_tool --export --blocks incremental_blocks.json --output /backup/incremental/
```

**优势：**
- 无需修改TDengine核心代码
- 可以独立开发和维护
- 支持多种备份策略

**实现步骤：**
1. 编译位图插件为独立工具
2. 创建配置文件接口
3. 实现与taosdump的协作机制

### 方案二：扩展taosdump

在taosdump中添加增量备份功能：

```bash
# 新增的增量备份命令
taosdump --incremental --bitmap-plugin /path/to/libincremental_bitmap_plugin.so \
         -h localhost -P 6030 -D dbname -o /backup/incremental/
```

**优势：**
- 统一的备份工具
- 用户使用简单

**实现步骤：**
1. 修改taosdump源码
2. 添加插件加载机制
3. 实现增量备份逻辑

### 方案三：创建新的备份工具

开发专门的增量备份工具：

```bash
# 新的增量备份工具
taosbackup --engine bitmap --config backup_config.json \
           --source localhost:6030 --database dbname \
           --output /backup/incremental/
```

**优势：**
- 专门为增量备份设计
- 功能完整且灵活

## 技术实现细节

### 1. 插件接口设计

```c
// 备份插件接口
typedef struct {
    // 插件信息
    const char* (*get_plugin_name)(void);
    const char* (*get_plugin_version)(void);
    
    // 初始化
    int32_t (*init)(const char* config);
    void (*destroy)(void);
    
    // 增量检测
    int32_t (*detect_incremental_blocks)(const char* database, 
                                        uint64_t since_timestamp,
                                        SIncrementalBlock** blocks,
                                        uint32_t* block_count);
    
    // 数据导出
    int32_t (*export_blocks)(const SIncrementalBlock* blocks,
                             uint32_t block_count,
                             const char* output_path);
    
    // 数据恢复
    int32_t (*restore_blocks)(const char* backup_path,
                              const char* target_database);
} SBackupPluginInterface;
```

### 2. 配置文件格式

```json
{
    "plugin": {
        "name": "incremental_bitmap",
        "version": "1.0.0",
        "library_path": "/usr/local/taos/plugins/backup/libincremental_bitmap_plugin.so"
    },
    "bitmap_engine": {
        "type": "roaring",
        "memory_limit_mb": 1024,
        "persistence_path": "/var/lib/taos/bitmap_cache"
    },
    "event_interceptor": {
        "enable": true,
        "buffer_size": 10000,
        "callback_threads": 4
    },
    "backup": {
        "batch_size": 1000,
        "compression": true,
        "encryption": false,
        "retry_count": 3,
        "retry_interval_ms": 1000
    }
}
```

### 3. 与TDengine的集成点

```c
// 1. 存储引擎事件监听
// 通过storage_engine_interface监听块变更事件
SStorageEngineInterface* interface = get_storage_engine_interface("tdengine");
interface->install_interception();

// 2. WAL文件监控
// 监控WAL文件变化，获取增量信息
int32_t monitor_wal_changes(const char* wal_path, 
                           WALChangeCallback callback);

// 3. 数据块访问
// 通过TDengine API访问数据块
int32_t read_data_block(uint64_t block_id, 
                        void** data, 
                        uint32_t* size);
```

## 部署和配置

### 1. 编译和安装

```bash
# 编译插件
cd plugins/incremental_bitmap
mkdir build && cd build
cmake .. -DBUILD_TESTING=ON
make -j$(nproc)

# 安装到TDengine插件目录
sudo cp build/libincremental_bitmap_plugin.so /usr/local/taos/plugins/backup/
```

### 2. 配置文件

```bash
# 创建配置文件
sudo mkdir -p /etc/taos/plugins
sudo cp config/bitmap_backup.json /etc/taos/plugins/
```

### 3. 服务集成

```bash
# 创建systemd服务
sudo tee /etc/systemd/system/taos-bitmap-backup.service << EOF
[Unit]
Description=TDengine Bitmap Backup Service
After=taosd.service

[Service]
Type=simple
User=taos
ExecStart=/usr/local/bin/taos-bitmap-backup --config /etc/taos/plugins/bitmap_backup.json
Restart=always

[Install]
WantedBy=multi-user.target
EOF

# 启动服务
sudo systemctl enable taos-bitmap-backup
sudo systemctl start taos-bitmap-backup
```

## 性能优化

### 1. 内存管理

```c
// 位图引擎内存限制
SBitmapEngineConfig config = {
    .max_memory_mb = 1024,
    .persistence_enabled = true,
    .persistence_path = "/var/lib/taos/bitmap_cache"
};
```

### 2. 并发处理

```c
// 多线程事件处理
SEventInterceptorConfig interceptor_config = {
    .callback_threads = 8,
    .event_buffer_size = 50000
};
```

### 3. 批量操作

```c
// 批量块处理
SBackupConfig backup_config = {
    .batch_size = 2000,
    .enable_compression = true,
    .enable_parallel_processing = true
};
```

## 监控和运维

### 1. 性能监控

```c
// 获取插件统计信息
SBackupStats stats;
backup_plugin->get_stats(&stats);

printf("处理块数: %lu, 失败块数: %lu, 总大小: %lu bytes\n",
       stats.processed_blocks, stats.failed_blocks, stats.total_size);
```

### 2. 日志管理

```c
// 配置日志级别
log_set_level(LOG_LEVEL_INFO);
log_set_output_file("/var/log/taos/bitmap_backup.log");
```

### 3. 健康检查

```bash
# 检查插件状态
curl -X GET http://localhost:8080/health

# 检查备份进度
curl -X GET http://localhost:8080/backup/status
```

## 故障排除

### 1. 常见问题

```bash
# 插件加载失败
ERROR: Failed to load plugin library
解决方案: 检查库文件路径和依赖

# 内存不足
ERROR: Bitmap engine memory limit exceeded
解决方案: 增加内存限制或启用持久化

# 事件丢失
ERROR: Event buffer overflow
解决方案: 增加缓冲区大小或回调线程数
```

### 2. 调试模式

```bash
# 启用调试模式
export TAOS_BITMAP_DEBUG=1
./taos-bitmap-backup --debug --config bitmap_backup.json
```

## 总结

通过以上集成方案，位图插件可以有效地集成到TDengine的备份系统中，提供高性能的增量备份功能。推荐使用方案一（独立工具集成），因为它具有最大的灵活性和可维护性。

