# ADR-0003: 绑定驱动的本地 FFI 调用

## 状态
Accepted

## 背景
历史实现以 VM 内部 `RegisterFunction` 为主，不满足“无需手写绑定代码、直接调用本地符号”的目标。

## 决策
- 将 FFI 主路径升级为“字节码绑定表驱动”：
  - `ffi_call` 使用 VM 符号名
  - 符号名在 `.tbc` FFI 扩展段映射到 `library + native_symbol + signature`
- 调用约定放在绑定函数头（并在签名中保留一致性信息）。
- 动态库解析使用平台原生接口：
  - Windows: `LoadLibrary/GetProcAddress`
  - POSIX: `dlopen/dlsym`
- 保留 `RegisterFunction` 仅用于兼容测试与特殊宿主注入，不作为标准库路径。
- 类型系统支持扩展标量、指针、引用方向（`in/out/inout`）、结构体按值（当前 <=8 字节）与按指针。

## 影响
- 标准库和第三方库可以通过 `.tlbs + .tbc` 自描述绑定运行。
- 调用路径可缓存（按模块+符号）并避免重复解析。
- 结构体按值当前仍有大小限制（>8 字节待后续 ABI 后端扩展）。
