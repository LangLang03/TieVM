# ADR-0004: 结构化错误栈（VmError）

## 状态
Accepted

## 背景
原 `Status` 仅保留字符串消息，无法满足部署级故障定位，尤其是 VM 与 FFI 交界问题。

## 决策
- 在 `Status` 中增加可选 `VmError`。
- `VmError` 包含：
  - `message`
  - `frames[]`（`StackFrame`）
- 帧类型：
  - VM 帧：`module/function/pc/line/column`
  - FFI 帧：`library/symbol`（并可带模块信息）
- `VmThread` 在调用链回传错误时追加 VM 帧；
  FFI 调用失败时追加 FFI 帧；
  CLI 优先打印 `VmError::Format()`。

## 影响
- 错误信息可程序化消费（API）与可读化展示（CLI）。
- 回归测试可验证帧完整性，减少“只有一行报错文本”的排障成本。
