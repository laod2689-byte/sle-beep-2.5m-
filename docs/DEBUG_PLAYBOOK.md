# BS21E 星闪测距调试手册

## 1. 业务逻辑

Server 侧流程：

1. SLE 连接建立。
2. 本地 IQ 回调保存 local IQ。
3. 收到对端 remote IQ。
4. local IQ 和 remote IQ 时间戳匹配后进入 SLEM 算法。
5. 算法输出距离。
6. 应用层把距离转成厘米。
7. 距离进入跳变过滤和阈值回差判断。
8. LED/蜂鸣器按 `far_state` 输出。

当前默认参数：

- `CONFIG_MEASURE_DIS_DEFAULT_THRESHOLD_CM = 250`
- `MEASURE_DIS_THRESHOLD_HYSTERESIS_CM = 15`
- `MEASURE_DIS_MAX_JUMP_CM = 300`
- `MEASURE_DIS_VALID_TIMEOUT_MS = 1200`

默认阈值 `2.50m` 时：

- `>= 2.65m` 报警。
- `<= 2.35m` 停止报警。
- `2.35m ~ 2.65m` 保持上一次报警状态。

## 2. 日志怎么读

正常测距链路：

```text
RECEIVE LOCAL IQ. timestamp_sn:...
store local iq data complete.
RECEIVE REMOTE IQ. timestamp_sn:...
slem recv remote iq.
local ts = ..., remote ts = ..., local complete:1.
SLEM get distance done. distance:x.xxx, time = xxx ms.
measure_dis distance:x.xxm threshold:y.yym far:0/1 buzzer:0/1
```

异常但不一定崩溃：

```text
slem_alg_calc_smoothed_dis failed. ret:0x8000a453
REMOTE IQ CALC FAIL
```

含义：这一帧算法失败。只要后面还能继续出距离，就不是整机崩溃。

队列积压：

```text
remote iq queue backlog:1, drop.
```

含义：算法处理慢于 IQ 到达。当前项目对报警只需要最新结果，所以忙时丢远端 IQ 是合理的。

等待态超时：

```text
measure_dis distance timeout, fallback to waiting state.
```

含义：太久没有有效距离。注意 timeout 不应该在算法忙时触发。

## 3. 蜂鸣器问题判断

用户说“回到阈值内还响几秒”时，不要先假设有固定响铃定时器。

先看三件事：

1. 当前回差是多少。
2. 最新一笔有效距离是否真的已经低于停止阈值。
3. 测距结果是否还没刷新。

当前代码里蜂鸣器 GPIO 跟随 `g_measure_dis_buzzer_on`，没有固定持续几秒的逻辑。

## 4. 距离不稳怎么处理

先区分来源：

- 人没动但距离变化：SLE 测距算法、环境反射、天线方向、RSSI/IQ 质量都会影响。
- 日志出现时间戳错配：优先查 IQ 快照和 busy 丢包逻辑。
- 显示值太跳：可考虑应用层显示滤波，但会降低响应速度。
- 报警来回抖：调回差，不是调显示滤波。

本项目已试过应用层额外平均/IIR 平滑，用户反馈更不稳定，默认不要恢复。

## 5. 构建和烧录

常见产物：

- `application_sign.bin`：正常直烧优先选择。
- `application_std.hex`：烧录工具只接受 hex 时用。
- `fota.fwpkg`：OTA/FOTA 用。
- `application.bin`：原始未签名 app，通常不是首选。

如果构建最后报：

```text
PermissionError: [WinError 32]
application.bin -> unpatch.bin
```

这通常是 Windows 文件锁，不是 C 源码问题。关闭占用该文件的工具后重编译。

## 6. 改参数时的检查表

改默认阈值：

- 改 `Kconfig`。
- 改源码 fallback 宏。
- 查生成配置 `standard_bs21e_1100e.config`。
- 重编译后看日志里的 `threshold:x.xxm`。

改蜂鸣器灵敏度：

- 改 `MEASURE_DIS_THRESHOLD_HYSTERESIS_CM`。
- 说明进入阈值和退出阈值都会变。

改“滤波”：

- 先问清楚是跳变过滤还是平均平滑。
- 跳变过滤是 `MEASURE_DIS_MAX_JUMP_CM`。
- 平均平滑会让响应变慢。

