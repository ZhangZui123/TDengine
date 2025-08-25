cmake_minimum_required(VERSION 3.16)
project(incremental_bitmap_plugin)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# 设置TDengine路径
set(TDENGINE_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
set(TDENGINE_BUILD_DIR ${TDENGINE_ROOT}/build)

# 包含目录
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${TDENGINE_ROOT}/include
    ${TDENGINE_BUILD_DIR}/include
    ${TDENGINE_ROOT}/deps/roaring/include
)

# 设置库目录
link_directories(
    ${TDENGINE_BUILD_DIR}/lib
    ${TDENGINE_BUILD_DIR}/build/lib
)

# 源文件
set(SOURCES
    src/bitmap_engine.c
    src/event_interceptor.c
    src/backup_coordinator.c
    src/skiplist.c
    src/simple_bitmap.c
    src/roaring_bitmap.c
    src/ring_buffer.c
    src/storage_engine_interface.c
    src/mock_storage_engine.c
)

# 创建动态库
add_library(incremental_bitmap_plugin SHARED ${SOURCES})

# 链接库
target_link_libraries(incremental_bitmap_plugin
    taos
    pthread
    roaring
)

# 设置输出目录
set_target_properties(incremental_bitmap_plugin PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/build
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/build
)

# 安装规则
install(TARGETS incremental_bitmap_plugin
    LIBRARY DESTINATION /usr/local/taos/plugins/backup
)

# 测试程序
if(BUILD_TESTING)
    # 环形队列测试
    add_executable(test_ring_buffer test/test_ring_buffer.c src/ring_buffer.c)
    target_link_libraries(test_ring_buffer pthread)
    
    # 位图引擎核心功能测试
    add_executable(test_bitmap_engine_core test/test_bitmap_engine_core.c src/bitmap_engine.c src/skiplist.c src/simple_bitmap.c src/roaring_bitmap.c)
    target_link_libraries(test_bitmap_engine_core pthread roaring)
    
    # 抽象层接口测试
    add_executable(test_abstraction_layer test/test_abstraction_layer.c src/simple_bitmap.c src/roaring_bitmap.c)
    target_link_libraries(test_abstraction_layer pthread roaring)
    
    # RoaringBitmap专用测试
    add_executable(test_roaring_bitmap_specific test/test_roaring_bitmap_specific.c src/simple_bitmap.c src/roaring_bitmap.c)
    target_link_libraries(test_roaring_bitmap_specific pthread roaring)
    
    # 事件拦截器测试
    add_executable(test_event_interceptor 
        test/test_event_interceptor.c 
        src/event_interceptor.c
        src/bitmap_engine.c
        src/ring_buffer.c
        src/skiplist.c
        src/simple_bitmap.c
        src/roaring_bitmap.c
    )
    target_link_libraries(test_event_interceptor pthread roaring)
    
    # 备份协同器测试
    add_executable(test_backup_coordinator 
        test/test_backup_coordinator.c 
        src/backup_coordinator.c
        src/event_interceptor.c
        src/bitmap_engine.c
        src/ring_buffer.c
        src/skiplist.c
        src/simple_bitmap.c
        src/roaring_bitmap.c
    )
    target_link_libraries(test_backup_coordinator pthread roaring)
    
    # 状态转换测试
    add_executable(test_state_transitions 
        test/test_state_transitions.c 
        src/bitmap_engine.c
        src/skiplist.c
        src/simple_bitmap.c
        src/roaring_bitmap.c
    )
    target_link_libraries(test_state_transitions pthread roaring)
    
    # 跳表测试
    add_executable(test_skiplist 
        test/test_skiplist.c 
        src/skiplist.c
    )
    target_link_libraries(test_skiplist pthread)
    
    # 重试机制测试
    add_executable(test_retry_mechanism 
        test/test_retry_mechanism.c 
        src/backup_coordinator.c
        src/event_interceptor.c
        src/bitmap_engine.c
        src/ring_buffer.c
        src/skiplist.c
        src/simple_bitmap.c
        src/roaring_bitmap.c
    )
    target_link_libraries(test_retry_mechanism pthread roaring)
    
    # 设置测试输出目录
    set_target_properties(test_ring_buffer test_bitmap_engine_core test_abstraction_layer test_roaring_bitmap_specific test_event_interceptor test_backup_coordinator test_state_transitions test_skiplist test_retry_mechanism
        PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/build
    )
endif() 