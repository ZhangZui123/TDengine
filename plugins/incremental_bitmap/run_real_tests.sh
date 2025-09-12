#!/bin/bash

# TDengine 增量位图插件 - 真实环境测试脚本
# 作者：章子渝
# 版本：1.0

echo "🚀 TDengine 增量位图插件真实环境测试开始"
echo "=========================================="

# 检查当前目录
if [ ! -f "CMakeLists.txt" ]; then
    echo "❌ 错误：请在插件根目录运行此脚本"
    echo "   正确路径：/home/hp/TDengine/plugins/incremental_bitmap"
    exit 1
fi

# 1. 检查TDengine服务状态
echo "1️⃣ 检查TDengine服务状态..."
if ! pgrep -f taosd > /dev/null; then
    echo "❌ 错误：TDengine服务未运行，请先启动taosd"
    echo "   启动命令：sudo systemctl start taosd"
    exit 1
fi
echo "   ✅ TDengine服务正在运行"

# 2. 构建真实环境测试
echo ""
echo "2️⃣ 构建真实环境测试..."
if [ -d "build" ]; then
    echo "   清理旧构建..."
    rm -rf build
fi

mkdir -p build
cd build

echo "   配置CMake（真实环境）..."
cmake -DUSE_MOCK=OFF -DBUILD_TESTING=ON -DE2E_TDENGINE_REAL_TESTS=ON .. > cmake.log 2>&1
if [ $? -ne 0 ]; then
    echo "   ❌ CMake配置失败！"
    echo "   错误日志："
    cat cmake.log
    exit 1
fi

echo "   编译真实环境测试..."
make -j$(nproc) > make.log 2>&1
if [ $? -ne 0 ]; then
    echo "   ❌ 编译失败！"
    echo "   错误日志："
    cat make.log
    exit 1
fi

echo "   ✅ 构建成功"

# 3. 运行真实环境测试
echo ""
echo "3️⃣ 运行真实环境测试..."

test_results=0
passed=0
failed=0

# 真实环境测试列表
real_tests=(
    "test_offset_semantics_realtime:实时偏移量语义测试"
    "test_taosdump_comparison:taosdump对比测试"
    "test_pitr_e2e:完整PITR端到端测试"
    "test_e2e_tdengine_real:真实TDengine端到端测试"
)

for test_info in "${real_tests[@]}"; do
    test_name=$(echo "$test_info" | cut -d: -f1)
    test_desc=$(echo "$test_info" | cut -d: -f2)
    
    echo "   📋 运行$test_desc..."
    if timeout 120s ./$test_name > /dev/null 2>&1; then
        echo "      ✅ 通过"
        ((passed++))
    else
        echo "      ❌ 失败"
        ((failed++))
        test_results=1
    fi
done

# 4. 测试结果总结
echo ""
echo "=========================================="
if [ $test_results -eq 0 ]; then
    echo "✅ 所有真实环境测试通过！"
    echo "   通过: $passed"
    echo "   失败: $failed"
    echo "   总计: $((passed + failed))"
else
    echo "❌ 部分真实环境测试失败"
    echo "   通过: $passed"
    echo "   失败: $failed"
    echo "   总计: $((passed + failed))"
fi

echo ""
echo "🔧 故障排除："
echo "   - 检查TDengine服务：systemctl status taosd"
echo "   - 检查编译日志：build/cmake.log, build/make.log"
echo "   - 检查测试日志：运行单个测试查看详细输出"

echo "=========================================="

exit $test_results
