#!/bin/bash

# 本地CI模拟测试脚本
# 模拟GitHub Actions CI的运行环境，用于本地验证CI配置

echo "🚀 本地CI模拟测试开始"
echo "========================"

# 设置环境变量（模拟CI环境）
export BUILD_TYPE="Debug"
export PLUGIN_DIR="plugins/incremental_bitmap"
export IB_TEST_SCALE="small"
export RUN_REAL_TESTS="false"
export CC="gcc"

echo "📋 环境配置："
echo "   BUILD_TYPE: $BUILD_TYPE"
echo "   PLUGIN_DIR: $PLUGIN_DIR"
echo "   IB_TEST_SCALE: $IB_TEST_SCALE"
echo "   RUN_REAL_TESTS: $RUN_REAL_TESTS"
echo "   CC: $CC"

# 检查当前目录
if [ ! -f "CMakeLists.txt" ]; then
    echo "❌ 错误：请在插件根目录运行此脚本"
    echo "   正确路径：/home/hp/TDengine/plugins/incremental_bitmap"
    exit 1
fi

# 1. 安装依赖（模拟CI的依赖安装）
echo ""
echo "1️⃣ 安装依赖（模拟CI）..."
echo "   注意：这里只检查依赖是否已安装，不实际安装"

# 检查必要的依赖
deps=("gcc" "cmake" "make" "valgrind" "gdb" "libroaring-dev")
missing_deps=()

for dep in "${deps[@]}"; do
    if command -v "$dep" >/dev/null 2>&1; then
        echo "   ✅ $dep 已安装"
    else
        echo "   ❌ $dep 未安装"
        missing_deps+=("$dep")
    fi
done

if [ ${#missing_deps[@]} -gt 0 ]; then
    echo "   ⚠️  缺少依赖：${missing_deps[*]}"
    echo "   请运行：sudo apt-get install ${missing_deps[*]}"
    # 不退出，继续执行
fi

# 2. 配置构建（模拟CI的CMake配置）
echo ""
echo "2️⃣ 配置构建（模拟CI）..."

if [ -d "build" ]; then
    echo "   清理旧构建..."
    rm -rf build
fi

mkdir -p build
cd build

echo "   运行CMake配置..."
cmake .. \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DCMAKE_C_COMPILER="$CC" \
    -DENABLE_TESTS=ON \
    -DENABLE_COVERAGE=ON \
    -DENABLE_SANITIZERS=ON \
    -DBUILD_TAOSX_PLUGIN=ON \
    -DUSE_MOCK=ON \
    -DE2E_TDENGINE_REAL_TESTS=OFF \
    -DIB_TEST_SCALE="$IB_TEST_SCALE" \
    > cmake.log 2>&1

if [ $? -ne 0 ]; then
    echo "   ❌ CMake配置失败！"
    echo "   错误日志："
    cat cmake.log
    exit 1
fi

echo "   ✅ CMake配置成功"

# 3. 构建插件（模拟CI的构建步骤）
echo ""
echo "3️⃣ 构建插件（模拟CI）..."

echo "   编译项目..."
make -j$(nproc) VERBOSE=1 > make.log 2>&1

if [ $? -ne 0 ]; then
    echo "   ❌ 构建失败！"
    echo "   错误日志："
    cat make.log
    exit 1
fi

echo "   ✅ 构建成功"

# 4. 运行测试（模拟CI的测试步骤）
echo ""
echo "4️⃣ 运行测试（模拟CI）..."

test_failed=0
passed=0
failed=0

# 运行所有测试，添加错误处理（模拟CI逻辑）
for test in test_*; do
    if [ -x "$test" ]; then
        echo "   📋 运行测试: $test"
        
        # 跳过真实环境相关二进制（模拟CI逻辑）
        if [ "$RUN_REAL_TESTS" != "true" ]; then
            if echo "$test" | grep -Eq "(e2e_tdengine_real|offset_semantics_real|offset_semantics_realtime)"; then
                echo "      ⏭️  跳过真实环境测试: $test"
                continue
            fi
        fi
        
        # 为单个用例增加90秒超时，避免挂死拖住整步（模拟CI超时）
        if timeout 90s ./$test > /dev/null 2>&1; then
            echo "      ✅ 通过"
            ((passed++))
        else
            echo "      ❌ 失败或超时"
            ((failed++))
            test_failed=1
        fi
    fi
done

# 运行taosX插件测试（模拟CI逻辑）
if [ -x "test_taosx_plugin_interface" ]; then
    echo "   📋 运行taosX插件测试: test_taosx_plugin_interface"
    if ./test_taosx_plugin_interface > /dev/null 2>&1; then
        echo "      ✅ 通过"
        ((passed++))
    else
        echo "      ❌ 失败"
        ((failed++))
        test_failed=1
    fi
else
    echo "   ⚠️  taosX插件测试未找到"
fi

# 5. 运行Valgrind测试（模拟CI的Valgrind步骤）
echo ""
echo "5️⃣ 运行Valgrind测试（模拟CI）..."

valgrind_tests=("test_bitmap_engine_core" "test_abstraction_layer" "test_taosx_plugin_interface")
valgrind_passed=0
valgrind_failed=0

for test in "${valgrind_tests[@]}"; do
    if [ -x "$test" ]; then
        echo "   📋 运行Valgrind测试: $test"
        if timeout 120s valgrind --tool=memcheck --leak-check=full --show-leak-kinds=all \
                --error-exitcode=1 --track-origins=yes ./$test > /dev/null 2>&1; then
            echo "      ✅ Valgrind通过"
            ((valgrind_passed++))
        else
            echo "      ❌ Valgrind失败或超时"
            ((valgrind_failed++))
        fi
    fi
done

# 6. 收集测试结果（模拟CI的结果收集）
echo ""
echo "6️⃣ 收集测试结果（模拟CI）..."

echo "   === 测试结果 ==="
ls -la test_* 2>/dev/null || echo "   无测试文件"
echo "   === 构建产物 ==="
ls -la *.so *.a 2>/dev/null || echo "   无库文件"
echo "   === taosX插件产物 ==="
ls -la *taosx* 2>/dev/null || echo "   无taosX插件产物"

# 生成测试结果报告（模拟CI的报告生成）
echo ""
echo "   === 测试结果摘要 ==="
echo "   # 测试结果报告" > test_results_summary.md
echo "   ## 测试执行摘要" >> test_results_summary.md
echo "   - 总测试文件数: $(ls test_* 2>/dev/null | wc -l)" >> test_results_summary.md
echo "   - 可执行测试文件: $(ls -x test_* 2>/dev/null | wc -l)" >> test_results_summary.md
echo "   - 构建产物: $(ls *.so *.a 2>/dev/null | wc -l)" >> test_results_summary.md
echo "   - taosX插件产物: $(ls *taosx* 2>/dev/null | wc -l)" >> test_results_summary.md
echo "   - 通过测试: $passed" >> test_results_summary.md
echo "   - 失败测试: $failed" >> test_results_summary.md
echo "   - Valgrind通过: $valgrind_passed" >> test_results_summary.md
echo "   - Valgrind失败: $valgrind_failed" >> test_results_summary.md

cat test_results_summary.md

# 7. 测试结果总结
echo ""
echo "=========================================="
if [ $test_failed -eq 0 ]; then
    echo "✅ 本地CI模拟测试通过！"
    echo "   通过: $passed"
    echo "   失败: $failed"
    echo "   Valgrind通过: $valgrind_passed"
    echo "   Valgrind失败: $valgrind_failed"
    echo "   总计: $((passed + failed))"
else
    echo "❌ 本地CI模拟测试失败"
    echo "   通过: $passed"
    echo "   失败: $failed"
    echo "   Valgrind通过: $valgrind_passed"
    echo "   Valgrind失败: $valgrind_failed"
    echo "   总计: $((passed + failed))"
fi

echo ""
echo "🔧 故障排除："
echo "   - 检查编译日志：build/cmake.log, build/make.log"
echo "   - 检查测试日志：运行单个测试查看详细输出"
echo "   - 检查Valgrind日志：valgrind --tool=memcheck --leak-check=full ./test_name"

echo "=========================================="

exit $test_failed
