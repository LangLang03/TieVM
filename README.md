# TieVM

TieVM 是一个面向部署场景的 64 位寄存器虚拟机与字节码工具链，使用 C++20 + xmake 构建。

## 当前能力（v0.x）
- 64 位寄存器解释执行。
- `.tbc` 字节码格式（固定 16 字节指令、可选调试段、FFI 元数据、类元数据）。
- `.tlb/.tlbs` 容器与打包（目录包与 zip 包，依赖 zlib）。
- OOP 与反射：多继承（C3 MRO）、`new_object/invoke`、字节码类元数据自动注册。
- 闭包与调用协议：`closure/get_upval/set_upval/call_closure/tail_call/tail_call_closure/vararg`。
- 快路径指令：立即数算术、组合跳转、字符串快指令、位运算快指令。
- C FFI：动态库绑定、调用约定校验、结构化错误栈。
- GC 控制接口（分代并发、Weak/Phantom/finalizer）。
- LLVM AOT 编译链路：`tiebc aot` 可将 `.tbc/.tlb/.tlbs` 编译为本地可执行文件。

## 构建与测试

### 依赖
- xmake
- C++20 编译器（Windows 推荐 LLVM/Clang）

### Windows
```powershell
$env:PATH = "C:\Program Files\LLVM\bin;$env:PATH"
xmake -y
```

### Linux/macOS
```bash
xmake -y
```

### 运行测试
```bash
xmake run tievm_tests
```

只跑冒烟：
```bash
xmake run tievm_tests --gtest_filter=Smoke.*
```

## 命令行工具

### tievm
```text
tievm run <file.tbc> [--validate] [--trusted] [--cache-dir <dir>]
tievm run <file.tlb> [module_name] [--validate] [--trusted] [--cache-dir <dir>]
tievm run <file.tlbs> [module_name] [--validate] [--trusted] [--cache-dir <dir>]
```

说明：
- `--validate` 启用运行时寄存器/跳转边界校验（默认关闭，性能优先）。
- `--trusted` 在反序列化阶段跳过 `Verifier`（仅建议用于可信产物）。
- `--cache-dir` 指定 `.tlbs` 解包缓存目录（默认使用用户缓存目录）。
- 执行 `.tbc` 时会尝试自动加载同目录 `stdlib.tlbs`。

### tiebc
```text
tiebc check <file.tbc>
tiebc disasm <file.tbc|file.tlb|file.tlbs>
tiebc map <file.tlb>
tiebc tlbs-check <file.tlbs>
tiebc tlbs-pack <input_dir.tlbs> <output.tlbs>
tiebc tlbs-unpack <input.tlbs> <output_dir.tlbs>
tiebc tlb-struct
tiebc tlbs-struct
tiebc tbc-struct
tiebc aot <input.{tbc|tlb|tlbs}> [module] -o <output_exe> [--target <triple>] [--cc <clang>] [--sysroot <path>] [--opt <O0|O1|O2|O3>] [--ldflag <flag>]... [--cflag <flag>]... [--emit-ir <file.ll>] [--emit-obj <file.o>]
tiebc build-stdlib <output.tlb>
tiebc build-stdlib-tlbs <output.tlbs>
tiebc emit-hello <output.tbc>
tiebc emit-opset <output.tbc>
tiebc emit-oop-ok <output.tbc>
tiebc emit-oop-error <output.tbc>
```

`tiebc aot` 说明：
- 严格 AOT：无解释器回退，无法 lowering 的 opcode 会在编译阶段失败并给出 `[AOT]` 定位信息。
- FFI 链接使用模块内库路径（`.tbc` 相对输入目录、`.tlb` 相对包目录、`.tlbs` 相对 bundle 根目录）。

## 字节码模型

### 函数头元数据
每个函数包含以下元信息：
- `reg_count`
- `param_count`
- `upvalue_count`
- `is_vararg`
- （可选）FFI 绑定头

### 类元数据（TBC v0.4+）
模块可直接携带类定义（不再只靠常量池字符串）：
- 类名
- 基类列表
- 方法列表（方法名、函数索引、访问级别、virtual）

`tievm run` 会按模块中的类元数据自动注册反射类；`invoke` 仍以方法名字符串作为查找键，但目标方法实现来自类元数据映射的函数索引。

### 格式兼容
- 当前写出格式：`TBC v0.4`
- 当前可读取旧格式：`v0.1 / v0.2 / v0.3 / v0.4`

## 指令集（摘要）

### 基础
- `nop/mov/loadk`
- `add/sub/mul/div/cmpeq`
- `jmp/jmp_if/jmp_if_zero/jmp_if_not_zero`
- `call/ret/throw/halt`
- `ffi_call`
- `new_object/invoke`

### 快路径与组合
- `add_imm/sub_imm/inc/dec`
- `dec_jnz/add_dec_jnz/sub_imm_jnz/add_imm_jnz`
- `str_len/str_concat`
- `bit_and/bit_or/bit_xor/bit_not/bit_shl/bit_shr`

### 闭包与调用协议
- `closure/get_upval/set_upval`
- `call_closure`
- `tail_call/tail_call_closure`
- `vararg`

## 标准库模块

### tie.std.io
- `print`
- `read_text`
- `write_text`
- `exists`
- `append_text`

### tie.std.collections
- `array_new/array_push/array_get/array_size/array_pop/array_clear/array_free`
- `map_new/map_set/map_get/map_has/map_size/map_remove/map_clear/map_free`

### tie.std.string
- `concat`
- `length`
- `utf8_validate`
- `codepoints`
- `slice`
- `starts_with`
- `ends_with`
- `find`
- `replace`
- `lower_ascii`
- `upper_ascii`

### tie.std.concurrent
- `sleep_ms`

### tie.std.net
- `is_ipv4`
- `is_ipv6`

### tie.std.math
- `abs`
- `min`
- `max`
- `clamp`
- `pow_i`
- `gcd`

### tie.std.time
- `now_ms`

## 快速示例

### 生成并运行可成功 OOP/反射样例
```bash
xmake run tiebc emit-oop-ok artifacts/samples/demo_oop_ok.tbc
xmake run tiebc disasm artifacts/samples/demo_oop_ok.tbc
xmake run tievm run artifacts/samples/demo_oop_ok.tbc
```

### 生成并运行故意错误样例
```bash
xmake run tiebc emit-oop-error artifacts/samples/demo_oop_error.tbc
xmake run tievm run artifacts/samples/demo_oop_error.tbc
```

## 基准
```bash
xmake run tievm_perf --n 1000000000 --warmup 0 --rounds 1
xmake run tievm_perf --n 1000000000 --warmup 0 --rounds 1 --runtime-validate
xmake run tievm_perf --n 1000000000 --warmup 0 --rounds 1 --load-per-run
xmake run tievm_perf --n 1000000000 --warmup 0 --rounds 1 --serialize-per-run
```

启动/加载基准（包含 `tievm run <.tbc>` 同目录 `stdlib.tlbs` 自动加载）：
```bash
python3 benchmarks/tievm_startup_bench.py
python3 benchmarks/tievm_startup_bench.py --trusted
```

## 目录结构
- `include/tie/...`：公共 API
- `src/...`：核心实现
- `tests/unit`：单元测试
- `tests/smoke`：冒烟测试
- `docs/adr`：架构决策记录
- `docs/dev-history.md`：开发里程碑
