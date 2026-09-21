# fx

`fx` 是一个 C++17 对象基础库，提供引用计数对象、强弱智能指针、接口查询、
内存分配和基础线程同步能力。本库由 Donut 的 core/object 模块迁移而来，公开
命名空间、头文件路径和 CMake 目标均使用 `fx`。

## 功能

- `AutoPtr<T>`：侵入式强引用智能指针。
- `WeakPtr<T>`：可安全解析为强引用的弱引用智能指针。
- `MonoPtr<T>`：支持自定义删除器的独占智能指针。
- `IObject`、`IWeakable` 和 `IWeakReference`：引用计数对象接口。
- `ObjectImpl`、`WeakableImpl` 和 `DelegatingObjectImpl`：对象实现基类。
- UUID 与接口表：支持 `QueryInterface` 风格的接口查询。
- `IMemoryAllocator` 和 `DefaultMemoryAllocator`：可替换的内存分配接口。
- `SpinLock`、`SharedSpinLock`、`Signal` 和 `LFStack`：基础并发组件。
- `IDataBlob`：二进制及字符串数据容器接口。

公开头文件位于 `include/fx/core/object/`，使用方式例如：

```cpp
#include <fx/core/object/AutoPtr.h>
#include <fx/core/object/Foundation.h>
```

## 构建要求

- 支持 C++17 的编译器
- CMake 3.16 或更高版本
- 支持的线程库；非 Windows 平台由 CMake 自动查找
- 构建单元测试时需要 Git 和网络访问，以便首次拉取 GoogleTest

## 构建

仅构建 `fx` 静态库：

```bash
cmake -S . -B build -DBUILD_TESTING=OFF
cmake --build build -j
```

构建完成后生成 CMake 目标 `fx`，同时提供别名目标 `fx::fx`。其他 CMake
工程可链接别名目标：

```cmake
add_subdirectory(path/to/fx)
target_link_libraries(your_target PRIVATE fx::fx)
```

## 单元测试

配置并构建测试：

```bash
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build -j
```

若系统没有安装 GoogleTest，CMake 会通过 `FetchContent` 从 GoogleTest 官方
仓库拉取固定版本。源码、子构建和构建文件保存在本工程的 `thirdparty/`
目录下，后续配置可复用已下载内容。

运行全部测试：

```bash
ctest --test-dir build --output-on-failure
```

也可以直接运行测试程序，或使用 GoogleTest 过滤器执行指定用例：

```bash
./build/fx_object_tests
./build/fx_object_tests --gtest_filter=Common_RefCntAutoPtr.*
```

测试覆盖强弱引用生命周期、智能指针操作、异常构造、委托对象以及并发引用
计数。并发测试耗时通常高于其他用例。

### 离线构建测试

离线环境需预先安装可由 `find_package(GTest)` 找到的 GoogleTest，或者保留一次
成功配置后生成的 `thirdparty/googletest-src`。只构建库本身不需要 GoogleTest，
可使用 `-DBUILD_TESTING=OFF` 完全关闭测试依赖。

## 目录结构

```text
include/fx/core/object/  公开头文件
src/core/object/         库实现
tests/src/core/          GoogleTest 单元测试
thirdparty/              FetchContent 依赖目录
```
