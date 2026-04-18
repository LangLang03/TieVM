# TieVM 开发历史

## 2026-04-18 TBC v0.5 破坏性升级（函数参数类型 + AOT 共享库导出）
- 字节码函数签名升级：
  - 新增 `BytecodeValueType`（`any/null/int64/float64/bool/object/pointer/string/closure`）。
  - 所有函数支持 `param_types/return_type`，`Module::AddFunction` 增加类型签名入参。
- `.tbc` 格式升级到 `v0.6`：
  - 函数头新增 `param_type_count + param_types + return_type` 序列化字段。
  - 反序列化改为仅接受 `v0.6`，不再兼容旧 minor 版本。
- 校验与运行时增强：
  - `Verifier` 新增函数参数类型签名一致性校验（`param_types.size == param_count`）。
  - 解释器调用路径新增固定参数类型检查，不匹配时返回结构化 `InvalidArgument`。
  - AOT 生成函数入口同样加入参数 tag 检查，解释执行与 AOT 行为对齐。
- AOT 导出能力增强：
  - `AotCompileOptions` 新增 `output_kind` 与 `emit_header`。
  - `tiebc aot` 支持 `--shared` 输出 `.so/.dylib/.dll`，并可 `--emit-header` 自动生成导出头文件。
  - 导出头文件按 `param_types/return_type` 生成强类型函数签名，支持直接 `funcname(123, true)` 调用与真实类型返回值。
  - 导出函数保持原函数名（不重命名），共享库模式下会自动导出模块中 `is_exported=true` 的函数。
- CLI/文档同步：
  - `tiebc disasm` 输出函数 `param_types`。
  - `tbc-struct` 输出函数头字段顺序更新为 `param_type_count + param_types + ffi-header + ...`。
  - `README` 补充函数类型签名与 `v0.5` 兼容性说明。
- 测试补充：
  - 新增参数类型 round-trip、签名不一致拒绝、调用参数类型不匹配拒绝测试。
  - 新增 shared library 导出与自动生成 `.h` 的集成测试。

## 2026-04-13 LLVM AOT 首版落地（本次）
- 新增真实 AOT 编译 API：`AotCompiler + AotCompileOptions + AotCompileResult`。
- 新增 `tiebc aot` 命令：
  - 输入：`.tbc/.tlb/.tlbs`
  - 输出：本地可执行文件（可选 `--emit-ir/--emit-obj`）
  - 支持 `--target/--cc/--sysroot/--opt/--cflag/--ldflag`
- AOT 后端采用 LLVM 文本 IR + 外部 clang 编译/链接。
- FFI 静态链接输入来自模块内库路径，并在编译阶段执行路径解析与存在性校验。
- 保留占位 API 兼容壳：`AotPipeline/AotMetadataEmitter` 改为返回 `Unsupported` 并提示迁移。
- 测试更新：
  - 占位 AOT 测试改为兼容壳行为验证。
  - 新增 `.tbc/.tlbs` 到可执行产物的 AOT 编译与运行测试。

## 2026-04-09 M0-M1 工程与字节码基础
- 初始化工程结构、`xmake` 构建链路、`tievm/tiebc/tievm_tests` 目标。
- 完成 `.tbc` 指令集、构建器、序列化器、校验器。
- 完成 `tiebc` 基础命令（检查、反汇编、标准库构建、示例字节码生成）。

## 2026-04-09 M2-M7 运行时首版能力
- 完成 `VmInstance/VmThread` 解释执行主路径。
- 完成对象模型、多继承与反射基础能力。
- 完成 GC 控制器接口与并发收集基础实现。
- 完成热加载会话、模块加载器、AOT 占位管线。
- 建立单元测试与冒烟测试基线。

## 2026-04-09 CLI 模块化重构
- `tiebc` 按职责拆分为 `disasm/emit/struct/dispatch`。
- `tievm` 按职责拆分为 `dispatch/run_tbc/run_tlb`。
- 保持行为兼容并通过现有 smoke 回归。

## 2026-04-09 FFI/标准库重构（本次）
- 引入结构化错误模型：`VmError + StackFrame`，`Status` 可携带错误栈。
- `.tbc` 升级到 `0.2`：新增 FFI 扩展段（库路径、签名、绑定、结构体布局）与函数头 FFI 绑定元信息。
- 新增 FFI 元数据构建 API：`FfiMetadataBuilder`。
- `FfiBridge` 升级为“绑定驱动 + 动态库调用”：
  - 支持 `LoadLibrary/GetProcAddress` 与 `dlopen/dlsym`。
  - 支持运行时类型映射、指针与 `in/out/inout` 语义、结构体按值（<=8 字节）与指针传递。
  - 保留 `RegisterFunction` 兼容通道（非标准库路径）。
- 移除 VM 启动时内置标准库注册，`StdlibRegistry::RegisterCore` 改为显式 `Unsupported`。
- 新增 `.tlbs` 包规范与实现：
  - 同后缀双格式：目录包与 zip 包自动识别。
  - 固定布局：`manifest.toml + modules/*.tbc + libs/<platform>/<arch>/*`。
  - 新增 `TlbsBundle`（目录/zip 读写、manifest 解析）。
- `ModuleLoader` 新增 `.tlbs` 加载：
  - zip 自动物化到临时目录。
  - FFI 库相对路径改写为绝对路径。
  - 同名符号冲突拒绝加载。
- 新增原生标准库动态库 `tievm_std_native`，`stdlib` 模块改为 FFI 绑定驱动。
- `tiebc` 增强：
  - 新增 `build-stdlib-tlbs / tlbs-check / tlbs-pack / tlbs-unpack / tlbs-struct`。
  - `disasm` 可显示 FFI 函数头、签名表、绑定表、库映射。
- `tievm` 增强：
  - 支持运行 `.tlbs`。
  - `.tbc` 运行时可自动尝试加载同目录 `stdlib.tlbs`。
  - 运行时错误输出结构化栈。
- 新增/更新测试：
  - `.tbc` FFI 扩展段序列化回归。
  - `.tlbs` 目录/zip 往返测试与加载器测试。
  - 动态库 FFI 集成测试（直接调用 `add`、指针 `inout`、结构体按值、错误栈）。
  - OOP 与故意报错字节码回归。
  - HelloWorld + 中文字节码运行回归。

## 质量结果（本次）
- `xmake` 全量构建通过。
- `tievm_tests` 全量通过（当前 33/33）。
- 关键 CLI 冒烟通过：
  - `tiebc build-stdlib-tlbs`
  - `tiebc emit-hello`
  - `tiebc disasm`（`.tbc/.tlbs`）
  - `tievm run`（Hello + 中文输出）。
