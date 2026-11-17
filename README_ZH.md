<div align="center">
  <h1>BaeStim 2.0.1</h1>

  <p>
    <a href="README.md">English</a> &nbsp;|&nbsp;
    <a href="README_ES.md">Español</a> &nbsp;|&nbsp;
    <a href="README_ZH.md">中文</a>
  </p>
</div>

## 说明

本文件仅为中文简要说明。完整的功能介绍、安装步骤、配置、Lua 用法、签名回退
机制与构建说明请参阅 [README.md](README.md)（英文）或
[README_ES.md](README_ES.md)（西班牙文）。

BaeStim 是一款注入 Steam 的 Windows DLL：通过 Lua 脚本解锁 app ID，并在每次
启动时加载与 `steamclient64.dll` / `steamui.dll` 的 SHA-256 匹配的签名模式文件；
当没有精确匹配的 TOML 时，会扫描最多 3 个较新的相邻 TOML，并把最佳结果保存为
非规范后缀的 `<sha>.fallback.toml` 以供下次复用。安装时将 `BaeStim.dll`、
`dwmapi.dll`、`xinput1_4.dll` 三个 DLL 复制到 Steam 根目录，配置文件为
`baestim.toml`，Lua 脚本放在 `<Steam>\config\baestim\`。

`baestim.toml` 中的 `[supabase]` 段（`url`、`anon_key`）用于启用远程 appid 列表；
`url` 或 `anon_key` 为空时该功能完全关闭。远程 appid 与同一
`<Steam>\config\baestim\` 目录下的本地 Lua **叠加**（不替换本地内容）：生成的
`baestim-remote.lua` 每次刷新整体重写，最后一次有效列表缓存在
`<Steam>\baestim\remote\<steamid>.json`，服务中断也不会移除已有解锁。同一 appid
在两处都带 depot key 时，后加载的**非空** key 生效——请勿在本地文件与远程列表中
重复同一 appid。详见 [README.md](README.md)。

## 许可与归属

BaeStim 基于 OpenSteamTool（OpenSteam001）及其分支 BetterSteamTools（madoiscool），
两者均采用 GPL-3.0 许可。来源：https://github.com/OpenSteam001/OpenSteamTool
和 https://github.com/madoiscool/BetterSteamTools 。BaeStim 以相同许可分发。

完整许可文本见 [LICENSE](LICENSE)；上游项目的许可与归属均予保留。
