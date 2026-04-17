# 从 `todo-list` 开始的全部改动说明（P4 侧）

> 文档目的：按实际实施顺序，解释每一步“改了什么、为什么改、关键代码是什么”。
> 范围：仅 P4 工程，不含 C5 工程。

---

## Step 0：建立执行清单（todo-list）

### 原因
在正式改代码前，把需求拆解成可执行清单，避免漏项，并保证每一项有可验证结果。

### 修改内容
- 新建并维护：`todo-list.md`
- 逐项勾选：需求冻结、能力层、CLI、NVS、集成、编译验证、文档。

### 为什么这样做
- 你明确要求“逐项执行、逐项勾选”。
- 后续复盘和教学时，清单可直接作为索引。

---

## Step 1：扩展 `wifi_app.h` 的能力接口（能力层抽象）

### 原因
原有 `wifi_app` 只覆盖简单连接，不满足 `scan/connect/disconnect/status/reconnect/mac/cancel` 的完整 CLI 需求。

### 修改文件
- `main/wifi_app.h`

### 关键代码
```c
#define WIFI_APP_MAX_SCAN_RESULTS 32

typedef struct {
    char ssid[33];
    int8_t rssi;
    uint8_t channel;
    uint8_t bssid[6];
    wifi_app_security_t security;
} wifi_app_scan_result_t;

typedef struct {
    const char *ssid;
    const char *password;
    const uint8_t *bssid;
    bool use_bssid;
    uint32_t timeout_ms;
} wifi_app_connect_params_t;

esp_err_t wifi_app_scan(...);
esp_err_t wifi_app_connect(...);
esp_err_t wifi_app_cancel_connect(void);
esp_err_t wifi_app_disconnect(void);
esp_err_t wifi_app_reconnect(void);
esp_err_t wifi_app_get_status(...);
esp_err_t wifi_app_get_mac(...);
esp_err_t wifi_app_set_mac_temporary(...);
```

### 为什么这样做
- CLI 只做参数/交互；底层能力统一放在 `wifi_app`，职责更清晰。
- 后续如果替换底层链路（例如协议细节变化），CLI 基本不用改。

---

## Step 2：重构 `wifi_app.c`（核心 Wi-Fi 行为）

### 原因
需要把“连接成功标准=DHCP 成功”固化，并支持扫描、状态、MAC、取消连接等动作。

### 修改文件
- `main/wifi_app.c`

### 关键改动 A：连接成功判定为 DHCP
```c
EventBits_t bits = xEventGroupWaitBits(...WIFI_CONNECTED_BIT | WIFI_CONNECT_FAIL_BIT...);

if (bits & WIFI_CONNECTED_BIT) {
    return ESP_OK;   // 由 IP_EVENT_STA_GOT_IP 置位
}
```

### 关键改动 B：支持取消连接
```c
#define WIFI_CONNECT_CANCEL_BIT BIT2

esp_err_t wifi_app_cancel_connect(void) {
    s_connecting = false;
    xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECT_CANCEL_BIT);
    return esp_wifi_disconnect();
}
```

### 关键改动 C：扫描能力 + 过滤隐藏 SSID
```c
wifi_scan_config_t scan_cfg = {
    .show_hidden = false,
    .scan_type = WIFI_SCAN_TYPE_ACTIVE,
};
```

### 关键改动 D：中国信道域 + 首扫空结果重试
```c
wifi_country_t country = {.cc="CN", .schan=1, .nchan=13, .policy=WIFI_COUNTRY_POLICY_AUTO};
esp_wifi_set_country(&country);

for (int attempt = 0; attempt < 2; attempt++) {
    esp_wifi_scan_start(&scan_cfg, true);
    esp_wifi_scan_get_ap_num(&ap_count);
    if (ap_count > 0) break;
    vTaskDelay(pdMS_TO_TICKS(250));
}
```

### 关键改动 E：启动后 MAC ready 等待（缓解 hosted 时序）
```c
for (int i = 0; i < WIFI_APP_MAC_READY_RETRY; i++) {
    if (esp_wifi_get_mac(WIFI_IF_STA, mac) == ESP_OK) return ESP_OK;
    vTaskDelay(pdMS_TO_TICKS(WIFI_APP_MAC_READY_DELAY_MS));
}
```

### 为什么这样做
- 你要求“连接成功必须拿到 DHCP IP”。
- 现场日志显示 hosted 链路存在启动时序抖动，需容错。
- 国内场景中 12/13 信道常见，国家域设置可提升扫描命中率。

---

## Step 3：新增 `wifi_cmd` 模块（命令树）

### 原因
项目原先没有 `wifi` 顶级命令树；需求是 `wifi scan/connect/...` 子命令形式。

### 修改文件
- 新增 `main/wifi_cmd.h`
- 新增 `main/wifi_cmd.c`

### 初版关键能力
1. `wifi scan [-s|--sort <rssi|ssid>]`
2. `wifi connect <ssid> [<password>] [-b|--bssid <mac>]`
3. 同 SSID 多 AP 交互选序号
4. `wifi disconnect / status / reconnect / mac`
5. NVS 保存“最近成功连接配置”

### 排序规则代码
```c
// RSSI降序，次关键字SSID升序
static int compare_scan_rssi(...)

// SSID升序，次关键字RSSI降序
static int compare_scan_ssid(...)
```

### NVS 关键代码
```c
nvs_open("wifi_cli", NVS_READWRITE, &nvs);
nvs_set_str(nvs, "ssid", ...);
nvs_set_str(nvs, "pass", ...);
nvs_set_u8(nvs, "use_bssid", ...);
nvs_set_blob(nvs, "bssid", ...);
nvs_set_u8(nvs, "valid", 1);
```

### 为什么这样做
- 满足你定义的 CLI 语法和行为。
- 保证 `wifi reconnect` 可跨掉电使用（P4 持久化）。

---

## Step 4：控制台注册接入（替换旧入口）

### 原因
新命令要被 REPL 识别；旧 `wifi_set` 与新命令体系重复，需清理。

### 修改文件
- `main/console_app.c`
- `main/console_app.h`
- `main/CMakeLists.txt`

### 关键代码
```c
ESP_ERROR_CHECK(wifi_cmd_register());
```

```cmake
"wifi_cmd.c"
```

### 为什么这样做
- 统一入口为 `wifi` 子命令树。
- 避免同功能多个命令并存导致使用混乱。

---

## Step 5：帮助输出排版优化

### 原因
你反馈 `wifi` 帮助显示过于拥挤。

### 修改文件
- `main/wifi_cmd.c`

### 关键代码
```c
static void print_wifi_help(void) {
    printf("wifi scan ...\n  ...\n\n");
    printf("wifi connect ...\n  ...\n\n");
    ...
}
```

### 为什么这样做
- 对齐你给的 `uart_help` 风格，命令和说明分段展示，可读性明显提升。

---

## Step 6：从“阻塞 connect”升级为“异步 connect + cancel”

### 原因
你要求运行中可中断连接；阻塞式 `wifi connect` 无法在同一 CLI 会话中再输入取消命令。

### 修改文件
- `main/wifi_cmd.c`
- `main/wifi_app.h`
- `main/wifi_app.c`

### 关键改动 A：后台任务执行 connect
```c
static void connect_task_entry(void *arg) {
    esp_err_t err = wifi_app_connect(&params);
    ...
    vTaskDelete(NULL);
}

xTaskCreate(connect_task_entry, "wifi_connect", 6144, job, 5, &task);
```

### 关键改动 B：新增 `wifi cancel`
```c
static int cmd_wifi_cancel(...) {
    esp_err_t err = wifi_app_cancel_connect();
}
```

### 关键改动 C：状态里显示是否有连接任务在跑
```c
printf("Connect Pending: %s\n", is_connect_running() ? "yes" : "no");
```

### 为什么这样做
- 不改 monitor 工具前提下，最可靠的中断方式是 CLI 命令级中断（`wifi cancel`）。
- 异步后 CLI 不会被 connect 卡住，体验更稳定。

---

## Step 7：README 同步更新

### 原因
命令体系已变更，文档必须与实际一致，避免误操作。

### 修改文件
- `README.md`

### 关键更新
- 替换 `wifi_set` 为 `wifi` 子命令集合
- 增加示例和行为说明（DHCP 成功判定、BSSID、MAC 临时性）

### 为什么这样做
- 文档是团队交接入口，需与固件行为一致。

---

## Step 8：构建与检查

### 执行过的验证
- `idf.py build`
- `idf.py fullclean build`
- 代码 maxline 检查（改动文件）

### 说明
- 构建中遇到过一次 bootloader cache 路径冲突（`C:/esp/...` vs `C:/Users/...`），通过 `fullclean` 解决。
- 当前版本已能通过完整构建。

---

## 额外说明：为什么没有直接支持“Ctrl+C 终止 connect”

### 原因
`Ctrl+C` 在 `idf.py monitor` 默认由主机端 monitor 捕获，不会直接当成板端 CLI 输入。

### 结论
- 纯固件侧无法稳定接住“键盘 Ctrl+C”。
- 当前方案用 `wifi cancel` 实现可控中断，是在不改 monitor 工具下最可靠方案。

---

## 你接下来逐行学习建议顺序

1. `main/wifi_app.h`：先看能力接口定义。
2. `main/wifi_app.c`：看事件位和 connect/cancel 的状态流。
3. `main/wifi_cmd.c`：看参数解析 -> 任务启动 -> NVS -> cancel。
4. `main/console_app.c`：看命令注册入口。
5. `README.md`：对照最终外部行为。

---

## 变更文件清单（从 todo-list 开始到当前）

- `todo-list.md`
- `main/wifi_app.h`
- `main/wifi_app.c`
- `main/wifi_cmd.h`
- `main/wifi_cmd.c`
- `main/console_app.c`
- `main/console_app.h`
- `main/CMakeLists.txt`
- `README.md`

