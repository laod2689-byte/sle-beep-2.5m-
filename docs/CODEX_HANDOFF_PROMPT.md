# 给下一个 Codex 的接手提示

把下面这段作为新会话第一条消息，能最大程度恢复上下文：

```text
请先阅读本仓库根目录的 AGENTS.md、PROJECT_MEMORY.md、docs/DEBUG_PLAYBOOK.md。
同时使用 c-skill 仓库里的 sle-ranging-bs21e skill。

这是 BearPi/HiSilicon BS21E 星闪 SLE 测距项目，当前重点是 server 侧测距、UART 阈值修改、LED/蜂鸣器报警。
不要从零猜业务逻辑，先确认实际编译目录、生成配置、当前参数和烧录文件。
```

## 当前最重要的结论

- 实际开发目录曾经是 `D:\DesktopProjects\bearpi-pico_h2821e-master\bearpi-pico_h2821e-master`。
- 本仓库只上传了测距相关代码，不是完整 SDK。
- 当前源码默认阈值是 `2.50m`，回差是 `15cm`，跳变过滤是 `300cm`。
- 蜂鸣器没有写死响几秒，它跟随最新有效测距结果。
- 测距刷新周期接近 `1s`，所以蜂鸣器停响不会是毫秒级。
- 正常直烧优先用 `application_sign.bin`，不是 `application.bin`。

## 必须先确认的事

1. 当前用户实际编译的是哪个目录。
2. `Kconfig` 默认值是否被 `build/config/target_config/bs21e/menuconfig/acore/*.config` 覆盖。
3. 用户说的“滤波”到底是：
   - vendor 算法平滑；
   - 应用层跳变过滤；
   - 报警回差；
   - 还是想要显示值平均。
4. 用户说“蜂鸣器延时”时，先判断是代码固定延时，还是测距周期加回差造成的体感延时。

## 不要重复踩的坑

- 不要再随手加 IIR/平均滤波；之前用户反馈更不稳定。
- 不要让远端 IQ 在算法忙时排队堆积；旧数据会让判断更差。
- 不要直接拿全局 local IQ buffer 做长时间算法计算；要先做快照。
- 不要只改源码默认值就认为固件默认阈值变了；生成配置可能覆盖它。
- 不要把 FOTA 包当成普通直烧文件。

