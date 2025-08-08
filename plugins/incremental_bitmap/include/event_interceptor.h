/*
 * Copyright (c) 2024 TAOS Data, Inc. <jhtao@taosdata.com>
 *
 * This program is free software: you can use, redistribute, and/or modify
 * it under the terms of the GNU Affero General Public License, version 3
 * or later ("AGPL"), as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef TDENGINE_EVENT_INTERCEPTOR_H
#define TDENGINE_EVENT_INTERCEPTOR_H

#include <stdint.h>
#include <stdbool.h>
#include "bitmap_engine.h"
#include "../src/ring_buffer.h"
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

// 事件类型枚举
typedef enum {
    EVENT_BLOCK_CREATE = 0,   // 块创建事件
    EVENT_BLOCK_UPDATE = 1,   // 块修改事件
    EVENT_BLOCK_FLUSH = 2,    // 块刷盘事件
    EVENT_BLOCK_DELETE = 3    // 块删除事件
} EEventType;

// 事件数据结构
typedef struct {
    EEventType event_type;    // 事件类型
    uint64_t block_id;        // 块ID
    uint64_t wal_offset;      // WAL偏移量
    int64_t timestamp;        // 时间戳
    void* user_data;          // 用户数据
} SBlockEvent;

// 事件回调函数类型
typedef void (*FBlockEventCallback)(const SBlockEvent* event, void* user_data);

// 事件拦截器配置
typedef struct {
    bool enable_interception;     // 是否启用拦截
    uint32_t event_buffer_size;   // 事件缓冲区大小
    uint32_t callback_threads;    // 回调线程数
    FBlockEventCallback callback; // 事件回调函数
    void* callback_user_data;     // 回调用户数据
} SEventInterceptorConfig;

// 事件拦截器实例
typedef struct {
    SEventInterceptorConfig config;
    SBitmapEngine* bitmap_engine;
    
    // 事件缓冲区（环形队列）
    SRingBuffer* event_buffer;
    uint32_t buffer_size;
    uint32_t head;
    uint32_t tail;
    uint32_t count;
    
    // 线程同步
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    
    // 回调线程
    pthread_t* callback_threads;
    uint32_t thread_count;
    bool stop_threads;
    
    // 统计信息
    uint64_t events_processed;
    uint64_t events_dropped;
} SEventInterceptor;

// 事件拦截器API

/**
 * 初始化事件拦截器
 * @param config 拦截器配置
 * @param bitmap_engine 位图引擎实例
 * @return 拦截器实例，失败返回NULL
 */
SEventInterceptor* event_interceptor_init(const SEventInterceptorConfig* config,
                                         SBitmapEngine* bitmap_engine);

/**
 * 销毁事件拦截器
 * @param interceptor 拦截器实例
 */
void event_interceptor_destroy(SEventInterceptor* interceptor);

/**
 * 启动事件拦截器
 * @param interceptor 拦截器实例
 * @return 0成功，非0失败
 */
int32_t event_interceptor_start(SEventInterceptor* interceptor);

/**
 * 停止事件拦截器
 * @param interceptor 拦截器实例
 * @return 0成功，非0失败
 */
int32_t event_interceptor_stop(SEventInterceptor* interceptor);

/**
 * 处理块创建事件
 * @param interceptor 拦截器实例
 * @param block_id 块ID
 * @param wal_offset WAL偏移量
 * @param timestamp 时间戳
 * @return 0成功，非0失败
 */
int32_t event_interceptor_on_block_create(SEventInterceptor* interceptor,
                                         uint64_t block_id, uint64_t wal_offset, int64_t timestamp);

/**
 * 处理块修改事件
 * @param interceptor 拦截器实例
 * @param block_id 块ID
 * @param wal_offset WAL偏移量
 * @param timestamp 时间戳
 * @return 0成功，非0失败
 */
int32_t event_interceptor_on_block_update(SEventInterceptor* interceptor,
                                         uint64_t block_id, uint64_t wal_offset, int64_t timestamp);

/**
 * 处理块刷盘事件
 * @param interceptor 拦截器实例
 * @param block_id 块ID
 * @param wal_offset WAL偏移量
 * @param timestamp 时间戳
 * @return 0成功，非0失败
 */
int32_t event_interceptor_on_block_flush(SEventInterceptor* interceptor,
                                        uint64_t block_id, uint64_t wal_offset, int64_t timestamp);

/**
 * 处理块删除事件
 * @param interceptor 拦截器实例
 * @param block_id 块ID
 * @param wal_offset WAL偏移量
 * @param timestamp 时间戳
 * @return 0成功，非0失败
 */
int32_t event_interceptor_on_block_delete(SEventInterceptor* interceptor,
                                         uint64_t block_id, uint64_t wal_offset, int64_t timestamp);

/**
 * 获取统计信息
 * @param interceptor 拦截器实例
 * @param events_processed 已处理事件数
 * @param events_dropped 丢弃事件数
 */
void event_interceptor_get_stats(SEventInterceptor* interceptor,
                                uint64_t* events_processed, uint64_t* events_dropped);

// 存储引擎接口拦截函数

/**
 * 拦截taosWriteBlock函数
 * @param taos 数据库连接
 * @param rows 行数
 * @param pData 数据指针
 * @param tbname 表名
 * @return 原始返回值
 */
int taos_write_block_intercepted(void* taos, int rows, char* pData, const char* tbname);

/**
 * 拦截taosWriteBlockWithReqid函数
 * @param taos 数据库连接
 * @param rows 行数
 * @param pData 数据指针
 * @param tbname 表名
 * @param reqid 请求ID
 * @return 原始返回值
 */
int taos_write_block_with_reqid_intercepted(void* taos, int rows, char* pData, 
                                           const char* tbname, int64_t reqid);

/**
 * 安装存储引擎接口拦截
 * @param interceptor 拦截器实例
 * @return 0成功，非0失败
 */
int32_t install_storage_engine_interception(SEventInterceptor* interceptor);

/**
 * 卸载存储引擎接口拦截
 * @param interceptor 拦截器实例
 * @return 0成功，非0失败
 */
int32_t uninstall_storage_engine_interception(SEventInterceptor* interceptor);

#ifdef __cplusplus
}
#endif

#endif // TDENGINE_EVENT_INTERCEPTOR_H 