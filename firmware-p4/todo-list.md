# Wi-Fi CLI 实现待办清单（P4 侧）

## 1. 需求冻结
- [x] 确认以下命令语法与返回行为：
  - `wifi scan [-s|--sort <rssi|ssid>]`
  - `wifi connect <ssid> [<password>] [-b|--bssid <mac>]`
  - `wifi disconnect`
  - `wifi status`
  - `wifi reconnect`
  - `wifi mac [<mac>]`
- [x] 确认“拿到 DHCP IP 才算连接成功”规则与超时值（建议：25 秒）。
- [x] 确认同 SSID 多 AP 时的交互选择行为。

## 2. P4 Wi-Fi 能力层（wifi_app）
- [x] 增加扫描 API，返回可见 AP 信息（SSID/RSSI/加密方式/信道/BSSID）。
- [x] 增加连接 API，支持可选密码与可选 BSSID 锁定。
- [x] 确保连接成功判定为“已获取 DHCP IP”。
- [x] 增加主动断开 API。
- [x] 增加状态查询 API（模式/连接状态/IP/子网掩码/网关/SSID/RSSI/BSSID/MAC）。
- [x] 增加重连 API。
- [x] 增加 MAC 获取/设置 API（仅临时伪装，本次上电有效）。

## 3. CLI 层（wifi 子命令树）
- [x] 增加顶层 `wifi` 命令与子命令分发。
- [x] 实现 `wifi scan` 排序：
  - 默认：按 RSSI 降序
  - `--sort ssid`：按 SSID 升序
  - 二级排序规则：
    - RSSI 相同时按 SSID 升序
    - SSID 相同时按 RSSI 降序
- [x] 实现 `wifi connect` 参数解析。
- [x] 实现同 SSID 交互选择流程（按序号），若无输入通道则提示使用 `-b`。
- [x] 实现 `wifi disconnect`。
- [x] 实现 `wifi status` 完整输出。
- [x] 实现 `wifi reconnect`（使用最近一次成功配置）。
- [x] 实现 `wifi mac` 获取/设置行为与输出。

## 4. 持久化（P4 NVS）
- [x] 定义“最近一次成功连接配置”的 NVS 命名空间与键名。
- [x] 仅在连接成功后写入持久化。
- [x] `wifi reconnect` 从 NVS 读取并重连。
- [x] MAC 伪装不持久化（重启恢复默认）。

## 5. 控制台集成
- [x] 在 console 初始化流程中注册新的 `wifi` 命令。
- [x] 确认旧命令 `wifi_set` 兼容策略（保留/移除/别名）。
- [x] 按需更新命令计数与帮助文本。

## 6. 验证
- [x] 编译检查：`idf.py build`。
- [ ] 手工 CLI 测试矩阵：
  - scan 两种排序
  - 连接开放网络
  - 连接加密网络
  - 同 SSID 多 AP + 序号选择
  - 使用指定 BSSID 连接
  - disconnect/status/reconnect
  - 断开状态下 mac 获取/设置
  - 已连接状态下 mac 设置（断开/重连路径）
- [ ] 掉电重启测试：验证 NVS 中重连配置保留。
- [ ] 掉电重启测试：验证 MAC 伪装恢复默认。

## 7. 文档
- [x] 更新 README 的命令章节为 `wifi` 子命令体系。
- [x] 增加命令示例与期望输出。
- [x] 说明 MAC 伪装作用范围与限制。

