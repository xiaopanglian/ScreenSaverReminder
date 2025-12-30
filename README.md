# ScreenSaverReminderCPP（C++/Win32）

一句话：Windows 护眼软件，定时弹出护眼遮罩，提醒休息保护眼睛。

目标：一款不依赖 .NET 运行时的 Windows 护眼软件，常驻托盘，按间隔弹出“全屏半透明护眼遮罩 + 实时时间 + 可选文字”，淡入结束后检测到任意键鼠活动才淡出；遮罩不抢焦点、不拦截底层输入。

## 功能对齐
- 托盘常驻：右键菜单【打开设置】【退出】；退出只能从托盘执行
- 默认启动：程序启动后直接进入托盘开始计时（不自动弹出设置窗口）
- 设置窗口：仅【保存】按钮；保存后隐藏窗口；点右上角 X 也只会隐藏
- 限制：间隔最小 1 分钟；淡入/淡出最小 1 秒；文字最多 500 字
- 遮罩：覆盖所有显示器（虚拟屏幕）；透明度作用于整个遮罩（含文字）
- 文本显示：超长自动换行；设置中的换行会原样显示

## 配置存储
- `%AppData%\\ScreenSaverReminderCPP\\config.ini`：间隔/透明度/淡入淡出/颜色
- `%AppData%\\ScreenSaverReminderCPP\\text.txt`：显示文字（UTF-8，保留换行）

## 开机自启
- 设置窗口勾选“开机自启”并保存后生效
- 实现方式：写入/删除 `HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Run\\ScreenSaverReminderCPP`

## 构建（MSVC，推荐 /MT 静态运行库）
本目录提供 `build-msvc.bat`，需要在 **Developer Command Prompt for VS** 中运行（确保 `cl.exe`/`rc.exe` 在 PATH）。

```bat
cd /d C:\Private\Learn\ScreenSaverReminderCPP
build-msvc.bat
```

输出：`build\\ScreenSaverReminderCPP.exe`

说明：
- 通过 `/MT` 静态链接 VC 运行库，避免额外安装 VC++ Redistributable（Windows 自带的系统组件仍然依赖）。

## 用 VS 打开
- 解决方案：`ScreenSaverReminderCPP.sln`
- 若 VS 提示“工具集/SDK 需要重定向”，直接按提示 Retarget 即可（例如从 `v143` 切到你机器上的默认工具集）。
