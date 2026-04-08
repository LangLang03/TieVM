# TieVM 开发历史

## 2026-04-09 M0 初始化

- 初始化仓库结构与 xmake 构建。
- 建立 CI（Clang 主线 + GCC 兼容）与 clang-tidy 静态检查。
- 固化编码规范（clang-format/clang-tidy）与 GPL-3.0-only 许可声明。

## 2026-04-09 M1-M7 首版能力落地

- 完成 `.tbc` 字节码 API（Module/Function/BasicBlock/InstructionBuilder/Verifier/Serializer）。
- 完成运行时（VmInstance/VmThread/ExceptionBridge/ModuleLoader/HotReloadSession）。
- 完成 OOP + 反射核心（多继承、C3 线性化、动态调用）。
- 完成 GC 控制器（分代、并发收集入口、Pin/Weak/Phantom/finalizer）。
- 完成 FFI 桥接（调用约定元数据、显式所有权、线程 attach/detach）。
- 完成 `tlb` 容器格式与热加载原子切换机制。
- 完成 AOT 占位管线接口及元数据导出。
- 完成单元测试与冒烟测试集。

