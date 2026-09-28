# v2.3.2：远程桌面连接期间的本机点击解锁

## 原因与修复

之前 `HandleClickUnlock` 收到锁屏的点击请求后，会先判断后台进程是否锁定。如果后台仍在已解锁的 RDP 会话中，直接返回，不进行解锁，也不返回失败提示。`RequestUnlock` 又用同样的会话状态推断请求是否完成，而所有 LogonUI 共用一个请求文件，无法明确目标。

现在：

- 锁屏组件用自己的 Windows Session ID 生成 `unlock.<session>.<kind>` 文件；后台只读取当前物理控制台会话的点击，提交前再次核对目标仍为控制台。
- 本机点击不依赖后台 RDP 会话是否已解锁，但仍要求绑定设备有新鲜 RSSI 或实际连接。未绑定设备、信号过期等均返回“设备不在附近”。
- 同一会话同时只发一个请求，等待同一令牌的 `ReportResult` 认证结果。文件被删除、收到其他会话结果、仅提交凭据，都不代表成功。
- 请求最多等待约 20 秒，超时撤回；锁屏组件等待后台超过约 25 秒时提示检查监控和组件版本。已提交给 Windows 的认证由系统处理，超时无法撤销系统正在进行的认证。
- 显式本机点击不回退到不支持 Session ID 的旧远程解锁服务，避免作用于错误会话。
- 自动锁定/解锁仍作用于后台进程所在会话，不因本机出现蓝牙而自动接管一个正在使用的远程会话。此修复处理的是回到物理电脑后的主动点击。
- 区分远程/控制台连接变化与人工锁定/解锁；连接变化时刷新状态与扫描，不误触发三分钟暂停规则。

Windows 行为依据：[OpenInputDesktop](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-openinputdesktop)、[WTSGetActiveConsoleSessionId](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-wtsgetactiveconsolesessionid)、[ReportResult](https://learn.microsoft.com/en-us/windows/win32/api/credentialprovider/nf-credentialprovider-icredentialprovidercredential-reportresult)。

## 升级

1. 在 **Windows x64 实机**退出旧程序，备份程序目录与配置，解压新包并沿用原配置。
2. 同时替换 `UnlockServer.exe` 与 `UnlockServer.Provider.dll`，以管理员权限运行新版本，使用原有的本机解锁安装/更新流程。自动更新已安装组件时也需要管理员权限。
3. **更新成功后重启 Windows**，让 LogonUI 释放旧 DLL 并加载新组件。如果更新时报 DLL 正在使用，先重启、再更新，然后再次重启。
4. 新旧组件使用不同的会话协议，不要只替换 EXE。没有响应时会失败/超时，不能继续依赖旧版共享请求文件。

不需要删除已保存的凭据或重新配置蓝牙。测试包包含 x64 Provider，不适用于把组件安装到 Windows ARM64 的 LogonUI。

## 自动验证

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build-wpf.ps1 -WithProvider
dotnet run --project tests/UnlockServer.RegressionTests
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/test-windows-service.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/test-provider-protocol.ps1
```

2026-09-28 验证：

- Windows 11 ARM64 虚拟机内使用 VS 2022 构建 WPF Release 与 x64 Provider 成功。
- 17 个 .NET 回归场景通过，包括原有蓝牙/锁屏回归，以及 RDP 未锁定下本机点击、过期蓝牙拒绝、失败不回退远程服务、错误/过期会话令牌拒绝、并发请求隔离。
- Windows 服务测试通过，并只读检查实际进程 Session ID 与无效会话处理。
- 原生协议测试直接编译实际 Provider 源码，验证会话路径、控制台点击检查、没有请求不能序列化凭据、过期请求拒绝、认证结果按令牌返回。测试将文件和事件重定向到独立临时位置，只使用合成凭据，**不安装组件、不执行真实 Windows 登录**。

仍需下面的 x64 实机验证。编译和模拟回调不等于已经证明真实 RDP、LogonUI 和蓝牙驱动的组合行为。

## 实机验证

1. 启动监控，确认设备在附近；从另一台机器通过 Windows 远程桌面连接，保持窗口连接。
2. 回到物理电脑，在锁屏选“蓝牙解锁 → 点击解锁”。有新鲜信号时应提交到本机锁屏并由 Windows 完成登录，不应因远程已登录而一直“正在确认设备”。远程会话如何切换由 Windows 决定。
3. 设备关机/离开后重试，必须提示不在附近，不能因为 RDP 活跃而绕过蓝牙检查。
4. 停止后台监控后点击，约 25 秒应出现等待超时提示。恢复监控后重新点击可再尝试。
5. 测试错误密码的失败反馈，以及远程断开、重连、回本机后的普通自动锁定和解锁。
6. 多次点击并检查日志：WPF `Logs` 应出现“进程会话 …，目标会话 …”；`C:\ProgramData\UnlockServer\provider.log` 应出现 `GetSerialization: submitted` 与 `ReportResult` 的结果。

若仍无法解锁，请提供具体时间及上述两处日志对应片段，以区分“设备未发现”“请求没到目标 LogonUI”“Windows 拒绝认证”。
