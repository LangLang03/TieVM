# TieVM

TieVM 是一个面向部署场景的 64 位寄存器虚拟机与字节码工具链项目，采用 C++20 与 xmake 构建。

## 当前范围（v0.x）

- 64 位统一寄存器解释执行
- `.tbc` 字节码格式（固定长度指令、固定小端、可剥离调试段）
- `tlb` 动态容器（字节码 + 可选本地插件元数据）
- OOP（多继承 + 接口）与反射（元信息 + 动态调用）
- 分代并发 GC（受控调优接口、Weak/Phantom、受限 finalizer）
- C FFI（显式所有权、显式线程 attach/detach）
- AOT 管线占位接口（不含机器码后端）

## 目录结构

- `include/tie/...`：公共 API
- `src/...`：核心实现
- `tests/unit`：单元测试
- `tests/smoke`：冒烟测试
- `docs/adr`：架构决策记录
- `docs/dev-history.md`：开发里程碑历史

## 构建

### Windows（Clang 官方）

```powershell
$env:PATH = "C:\Program Files\LLVM\bin;$env:PATH"
xmake f -m debug
xmake
```

### Linux/macOS

```bash
xmake f -m debug --toolchain=clang
xmake
```

## 测试

```bash
xmake run tievm_tests
```

仅运行冒烟：

```bash
xmake run tievm_tests --gtest_filter=Smoke.*
```

## 工具

- `tievm`：运行 `.tbc` 或 `.tlb` 中入口函数
- `tiebc`：字节码校验/反汇编/构建标准库容器

