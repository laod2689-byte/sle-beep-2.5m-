# sle-beep-2.5m

BearPi/HiSilicon BS21E 星闪 SLE 测距蜂鸣器报警示例。

## 当前功能
- SLE 测距 server/client 示例代码位于 `application/samples/products/sle_measure_dis`。
- Server 根据测距结果控制本地 LED 和蜂鸣器。
- UART2 支持运行时修改报警阈值。
- 当前源码默认阈值为 `2.50m`，回差为 `15cm`，跳变过滤为 `300cm`。

## 报警逻辑
默认阈值 `2.50m` 时：
- 距离 `>= 2.65m` 开始报警。
- 距离 `<= 2.35m` 停止报警。

蜂鸣器没有写死响几秒；它跟随最新有效测距结果切换。实际响应速度受测距结果刷新周期限制。

## 烧录文件
正常直烧优先使用构建输出中的：

`output/bs21e/acore/standard-bs21e-1100e/application_sign.bin`

其他产物：
- `application_std.hex`：仅烧录工具要求 hex 时使用。
- `fota.fwpkg`：OTA/FOTA 升级包。
- `application.bin`：原始未签名 app，一般不是首选。

## 接手前先读
请先读：

- `PROJECT_MEMORY.md`：项目状态、决策历史、当前参数和已知问题。
- `docs/DEBUG_PLAYBOOK.md`：日志判断、蜂鸣器、滤波、构建烧录排查流程。
- `docs/CODEX_HANDOFF_PROMPT.md`：给另一个 Codex 接手时的首条提示。

本仓库不是完整 SDK，只是当前测距相关代码和交接资料。完整编译仍需要原 BearPi/HiSilicon 工程树。
