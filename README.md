# dsh-proxy

用 C 写的 DSH（DeepSeek Harness）反向代理：自动拉起 `dsh web`，读取它启动时打印的 token URL，完成 token → 会话 cookie 交换，然后把指定 `IP:PORT` 上收到的 HTTP / WebSocket 请求带上认证转发给 DSH。

## 为什么需要它

DSH web 每次启动都会生成一个随机 token，只打印一次：

```text
dsh web: http://127.0.0.1:3080/?token=<随机串>
```

这个 token 只在 `GET /?token=...` 和 `/connect` 上被接受；`/api` 等请求要求同源签名的 HttpOnly cookie（`dsh-auth-...`）。所以不能简单地把 `?token=` 加到每个请求后面。dsh-proxy 会：

1. 启动 `dsh web --no-open`；
2. 从它的 stdout 里解析 token URL；
3. 用该 token 请求 `GET /?token=...`，拿到 `Set-Cookie: dsh-auth-...`；
4. 对每个转发请求注入这个 `Cookie`；
5. 把 `Host`、`Origin`、`Referer` 改写成 DSH 本地地址，以满足 `/api` 的 Host/Origin 同源检查；
6. 原样转发响应（包括 SSE、WebSocket）。

## 功能

- 自动拉起 dsh；dsh 退出后从 `RESTART_DELAY`（默认 5s）开始指数退避重启，上限 30s。
- **dsh 端口随机**：默认给 dsh 传 `--port 0`，由系统分配空闲高端口；代理从 dsh 打印的 token URL 自动发现实际端口，不需要手工挑端口。
- **dsh 绝不以 root 运行**：代理启动时检查 `geteuid() == 0`，是 root 就直接退出；systemd --user 单元让代理和 dsh 都以当前用户运行。
- 自动回答安装/更新提示：`Ok to proceed? (y)`、`(y/N)`、`[Y/n]`、`Continue?`、`Do you want to...` 等，自动向子进程 stdin 写 `y`，然后继续等待。等待 token 超时只告警，不会杀掉正在更新的 dsh。
- 从 dsh 输出解析 token；dsh 重启/更新后自动重新获取并刷新 cookie。
- 定时重新交换 cookie；上游返回 401 时也会自动刷新。
- HTTP/1.1 流式代理：支持 `Content-Length`、`Transfer-Encoding: chunked`、`Expect: 100-continue`。
- WebSocket / HTTP Upgrade 双向隧道。
- SSE（`text/event-stream`）流式转发。
- 改写 `Host` / `Origin` / `Referer` / `Sec-Fetch-Site` / `Location`；从 query 中移除 `token`，避免旧 token 干扰。
- 监听 IPv4/IPv6；`LISTEN=0.0.0.0:8080`、`127.0.0.1:8080`、`[::]:8080` 均可。
- systemd unit + 安装脚本，开机自启。

## 构建

依赖：Linux、支持 C11 的编译器、make、pthread。测试额外需要 python3。

```bash
make
./dsh-proxy --help
```

产物是当前目录下的 `dsh-proxy`，没有运行时依赖（除 libc/pthread）。

## 手动运行

假设 dsh 在 PATH 中，把本机 8080 暴露到局域网：

```bash
./dsh-proxy \
  --listen 0.0.0.0:8080 \
  --dsh-command "dsh web --no-open" \
  --verbose
```

然后访问 `http://<本机IP>:8080/`。

首次启动时代理会先监听端口；dsh 尚未打印 token 时请求会得到 `503`，日志出现 `session cookie acquired` 后即可正常使用。

如果 `dsh` 不在 PATH 中，用绝对路径或 npx：

```bash
./dsh-proxy -l 0.0.0.0:8080 \
  -d "/home/lbh/.npm/_npx/1e7f6d9597241db0/node_modules/.bin/dsh web --no-open"
```

```bash
./dsh-proxy -l 0.0.0.0:8080 \
  -d "npx @deepseek-ai/dsh web --no-open"
```

默认 `DSH_PORT=0`：每次启动由操作系统分配一个空闲高端口，代理从 dsh 打印的 URL 读取实际端口，所以不需要手动选端口，也不会和现有 3080 冲突。想固定端口时设置 `DSH_PORT=3080`，或者直接在 `DSH_CMD` 里写 `--port 3080`（代理检测到已有 `--port` 就不会再追加）。

代理必须以普通用户运行；`sudo ./dsh-proxy` 会被直接拒绝。dsh 会继承代理进程的用户，因此 systemd --user 是最省心的方式。也不要在 `DSH_CMD` 里写 `sudo` / `su` / `doas`。

### 配置

命令行参数优先于配置文件。默认读取 `/etc/dsh-proxy/dsh-proxy.conf`（不存在则使用内置默认值）；`-c FILE` 指定其他文件。

```bash
./dsh-proxy -c ./dsh-proxy.conf -l 192.168.1.10:8080 -v
```

配置文件格式为 `KEY=VALUE`，示例见 [`packaging/dsh-proxy.conf`](packaging/dsh-proxy.conf)：

| 键 | 默认值 | 说明 |
| --- | --- | --- |
| `LISTEN` | `127.0.0.1:8080` | 监听地址；代理入口 |
| `DSH_CMD` | `dsh web --no-open` | 启动 dsh 的命令；由 `/bin/sh -c` 执行。若命令中没有 `--port`，代理会追加 `--port $DSH_PORT` |
| `DSH_PORT` | `0` | dsh 监听端口；`0` = 由系统分配空闲高端口（推荐） |
| `DSH_HOME` | 继承环境 | 作为 `DSH_HOME` 传给 dsh；空则继承 |
| `UPSTREAM` | 空 | 强制上游 `HOST:PORT`；仅在 dsh 打印 `--public-url` 时使用 |
| `TOKEN_TIMEOUT` | `180` | 等待 token 的告警秒数（不会杀进程） |
| `RESTART_DELAY` | `5` | dsh 退出后的初始重启延迟（秒） |
| `COOKIE_REFRESH` | `21600` | 重新交换 cookie 的间隔（秒） |
| `AUTO_YES` | `1` | 是否自动回答 y/N 提示 |
| `LOG_LEVEL` | `info` | `error` / `warn` / `info` / `debug` |

`UPSTREAM` 的典型用法：dsh 启动时带了 `--public-url https://example.com/ui/`，打印的是公网地址；此时设置 `UPSTREAM=127.0.0.1:3080`，代理会连接本地 HTTP 端口，token 仍从打印的 URL 中读取。

## systemd 开机启动

### 推荐：systemd --user（当前用户）

这是最适合“dsh 必须以当前用户运行”的方式：代理和 dsh 子进程都由你的 user manager 启动，始终是你的 uid，不会变成 root，也不会在 systemd unit 里硬编码 `User=`。

```bash
cd /home/lbh/Develop/dsh-proxy
./scripts/install-user.sh --listen 0.0.0.0:8080
sudo loginctl enable-linger "$USER"
```

`install-user.sh` **不要用 sudo 运行**。它会：

1. 以当前用户构建；
2. 安装到 `~/.local/bin/dsh-proxy`；
3. 生成 `~/.config/dsh-proxy/dsh-proxy.conf`（含探测到的 dsh 绝对路径、`DSH_PORT=0`）；
4. 生成 `~/.config/systemd/user/dsh-proxy.service`；
5. `systemctl --user daemon-reload && systemctl --user enable --now dsh-proxy`。

`loginctl enable-linger "$USER"` 需要 root，只做一次。它的作用是让 user manager 在开机时就启动，而不是等你登录后才启动；没有 linger 的话，服务只会在你登录后运行。

验证：

```bash
loginctl show-user "$USER" -p Linger         # 期望 Linger=yes
systemctl --user status dsh-proxy
journalctl --user -u dsh-proxy -f
```

停止 / 重启 / 取消开机启动：

```bash
systemctl --user stop dsh-proxy
systemctl --user restart dsh-proxy
systemctl --user disable --now dsh-proxy
sudo loginctl disable-linger "$USER"   # 如果不再需要开机启动
```

如果 `systemctl --user` 报 “Failed to connect to bus”，说明当前会话没有 user manager。先正常登录一次，或确认 `XDG_RUNTIME_DIR=/run/user/$(id -u)` 存在；在 linger 已开启的情况下重新登录通常即可。

### 备选：系统级 unit（仍然以 User=... 运行）

如果希望由 root 管理开机服务，也可以用系统级 unit；关键是 unit 里必须设置 `User=<你的用户名>`，代理和 dsh 会以该用户运行，`geteuid()` 不是 root。

```bash
sudo ./scripts/install.sh --user lbh --listen 0.0.0.0:8080
```

脚本会安装到 `/usr/local/bin/dsh-proxy`、生成 `/etc/dsh-proxy/dsh-proxy.conf` 和 `/etc/systemd/system/dsh-proxy.service`（`User=lbh`），然后 `enable --now`。`--dsh-port` 默认 0；一般不用传。

```bash
systemctl status dsh-proxy
journalctl -u dsh-proxy -f
sudo systemctl restart dsh-proxy
```

> 不要在这个 unit 里删掉 `User=`，否则 systemd 会以 root 启动代理，而代理会拒绝运行。也不要 `sudo dsh-proxy`。

### 手动安装 unit

仓库里有四个 unit 文件：

- [`packaging/dsh-proxy.user.service`](packaging/dsh-proxy.user.service)：systemd --user 现成示例；
- [`packaging/dsh-proxy.user.service.in`](packaging/dsh-proxy.user.service.in)：用户级安装脚本的模板；
- [`packaging/dsh-proxy.service`](packaging/dsh-proxy.service)：系统级现成示例（`User=lbh`）；
- [`packaging/dsh-proxy.service.in`](packaging/dsh-proxy.service.in)：系统级安装脚本的模板。

手工安装用户级服务：

```bash
cd /home/lbh/Develop/dsh-proxy
make
install -Dm0755 dsh-proxy ~/.local/bin/dsh-proxy
mkdir -p ~/.config/dsh-proxy ~/.config/systemd/user
cp packaging/dsh-proxy.conf ~/.config/dsh-proxy/dsh-proxy.conf
cp packaging/dsh-proxy.user.service ~/.config/systemd/user/dsh-proxy.service
# 编辑 ~/.config/dsh-proxy/dsh-proxy.conf 和 unit 里的 PATH，确保能找到 dsh
systemctl --user daemon-reload
systemctl --user enable --now dsh-proxy
sudo loginctl enable-linger "$USER"
```

手工安装系统级服务：

```bash
sudo make install
sudo install -m0644 packaging/dsh-proxy.service /etc/systemd/system/dsh-proxy.service
# 编辑 unit 的 User/Group/WorkingDirectory/HOME/DSH_HOME/PATH
sudo systemctl daemon-reload
sudo systemctl enable --now dsh-proxy
```

`Restart=always` 负责代理自身崩溃后的重启；dsh 的崩溃/更新退出由代理内部 supervisor 负责重启。`KillMode=control-group` 保证停止服务时 dsh 子进程组一起退出。

> 不要给 unit 加 `ProtectHome=yes` / `ProtectSystem=strict` 之类的限制：DSH 需要真实的 `~/.dsh`、自身子进程沙箱和网络。确有需要请逐项验证。

## 更新提示是怎么处理的

代理持续读取 dsh 的 stdout/stderr（合并到同一个管道）。一旦在最近的输出里发现常见交互提示，并且 `AUTO_YES=1`，就向子进程 stdin 写 `y\n`，同一提示 2 秒内不会重复回答：

- `Ok to proceed? (y)`
- `(y/N)` / `[Y/n]` / `yes/no`
- `Continue?` / `Proceed?`
- `Do you want to ...`
- `Install now?` / `Update now?`
- `Overwrite?` / `Are you sure?`

然后代理继续等 `dsh web: ...?token=...`，不会因为等待超时杀掉进程。`TOKEN_TIMEOUT` 只控制“还没看到 token”的告警。

## 安全警告（务必阅读）

代理端口等价于 DSH 的完全访问权限：任何能连上 `LISTEN` 的人，代理都会替他注入 DSH 的会话 cookie，因此可以调用 DSH API、执行 shell 命令、读写文件。

- 只绑定内网 / VPN 地址，或用防火墙限制来源；
- **不要**把代理端口暴露到公网；
- 公网访问请在代理前面加 TLS 和身份认证（如 Authelia、oauth2-proxy、Cloudflare Access），并且只允许受信网络访问；
- 代理本身不做客户端认证，也不终止 TLS；
- 上游 DSH 使用明文 HTTP（默认只监听 `127.0.0.1` 的随机高端口）；
- 不要用 `sudo` 启动代理，也不要在 `DSH_CMD` 里使用 `sudo` / `su` / `doas`。代理会拒绝以 root 启动，但无法阻止你在命令里主动提权。

## 限制

- 上游只支持 HTTP。`dsh web` 使用 `--tls-cert` / `--tls-key` 时打印 `https://`，代理会报错；这种情况请在前面用 TLS 反向代理，或设置 `UPSTREAM` 指向本地 HTTP 端口。
- 只支持 HTTP/1.1；不支持 HTTP/2/3。
- 每个客户端 TCP 连接只处理一个请求（响应带 `Connection: close`），不做 keep-alive 连接池；浏览器会自动重连，功能不受影响。
- WebSocket 隧道在一端关闭时直接关闭双向连接，不保证完整的 WebSocket close handshake。
- 不实现客户端认证、限流、访问控制、TLS。
- 依赖 dsh 打印 `dsh web: http://...?token=...` 这一行（默认 web profile 会打印）。如果用了 `--public-url`，请配合 `UPSTREAM`。

## 故障排查

| 现象 | 原因 / 处理 |
| --- | --- |
| `503 dsh is still starting or renewing its session` | dsh 还在启动/更新，或 cookie 正在刷新；看日志，稍后重试 |
| `502 cannot reach dsh` | `UPSTREAM` 或 dsh 端口不对；dsh 没起来 |
| 直接访问 dsh 返回 401 | 正常；token 只在启动 URL 里，需通过代理访问 |
| `dsh-proxy: 拒绝以 root 运行` | 不要 `sudo dsh-proxy`；用 `systemd --user`，或系统级 unit 设置 `User=<用户名>` |
| `dsh exited: exit code 1` 反复出现 | `DSH_HOME` 不可写、`dsh` 命令找不到、或 `DSH_CMD` 自带了一个被占用的固定 `--port`；看日志里的 stderr |
| systemd 下 `dsh: not found` | unit 的 `PATH` 不包含 dsh 目录；用安装脚本，或把 `DSH_CMD` 改成绝对路径 |
| 开机后用户服务没起来 | 需要 `sudo loginctl enable-linger "$USER"`；用 `loginctl show-user "$USER" -p Linger` 确认 `Linger=yes` |
| `systemctl --user`: Failed to connect to bus | 当前会话没有 user manager；正常登录一次，或确认 `/run/user/$(id -u)` 存在；linger 开启后重新登录 |
| `dsh printed an https URL` | 去掉 `--tls-cert` / `--tls-key`，或设置 `UPSTREAM` 指向本地 HTTP 端口 |
| 页面能打开但 API 403 | 检查是否有别的代理改写了 Host/Origin；本代理会改写为上游 `127.0.0.1:<port>`，`-v` 日志可确认 |
| 通过局域网 IP 访问时部分本机功能不可用 | DSH 前端按页面 hostname 判断 loopback；这是 DSH 的预期行为，不是代理故障 |
| 更新卡住 | 看日志最后一行；`AUTO_YES=0` 时不会自动回答；某些 TTY-only 交互在管道下不会出现，需要给命令加 `--yes` 或用包装脚本 |

### Tailscale 设备无法访问

如果代理监听的是 Tailscale IP（例如 `LISTEN=100.114.48.1:8080`），本机 `curl http://100.114.48.1:8080/` 正常，但 tailnet 内其他设备超时，通常是 **firewalld 把 `tailscale0` 落到了默认 `public` zone**，而 `public` 没有放行 8080。

先确认：

```bash
firewall-cmd --get-active-zones
firewall-cmd --get-zone-of-interface=tailscale0   # 常见输出：no zone
firewall-cmd --zone=public --list-all              # 看 ports 是否包含 8080
```

推荐把整个 Tailscale 接口放进 `trusted` zone（Tailscale 自己的 ACL 仍然生效）：

```bash
sudo firewall-cmd --permanent --zone=trusted --add-interface=tailscale0
sudo firewall-cmd --reload

firewall-cmd --get-active-zones
firewall-cmd --zone=trusted --query-interface=tailscale0   # 期望 yes
```

然后在另一台设备上验证：

```bash
curl -v --max-time 5 http://100.114.48.1:8080/
```

如果只想放行 8080、不想把整个接口设为 trusted，可以在 `public` 里加限制到 Tailscale CGNAT 段的 rich rule：

```bash
sudo firewall-cmd --permanent --zone=public --add-interface=tailscale0
sudo firewall-cmd --permanent --zone=public --add-rich-rule='rule family="ipv4" source address="100.64.0.0/10" port port="8080" protocol="tcp" accept'
sudo firewall-cmd --permanent --zone=public --add-rich-rule='rule family="ipv6" source address="fd7a:115c:a1e0::/48" port port="8080" protocol="tcp" accept'
sudo firewall-cmd --reload
```

注意代理只监听 `100.114.48.1`（IPv4）；如果客户端用 MagicDNS 主机名并优先走 IPv6，可能会先失败再回退。可显式使用 `http://100.114.48.1:8080/`，或把 `LISTEN` 改成 `[::]:8080`（dual-stack）并用防火墙限制来源。

## 测试

```bash
make check
```

测试使用 [`tests/mock_dsh.py`](tests/mock_dsh.py) 模拟 dsh，覆盖：

- 自动回答安装/更新提示；
- token URL 解析与 cookie 交换；
- Cookie 注入、Host/Origin/Sec-Fetch-Site/query 重写；
- Content-Length、chunked、`Expect: 100-continue` 请求体；
- `Location` 重写；
- SSE 流式响应；
- WebSocket 握手与双向 echo；
- 杀掉 dsh 后自动重启、重新获取 token（再次自动回答提示）；
- 默认追加 `--port 0`，mock dsh 由系统分配随机端口，代理从打印的 URL 正确发现该端口。

测试会使用 `127.0.0.1:18080`（主代理）、`127.0.0.1:18099`（固定端口 mock）以及 `127.0.0.1:18081`（随机端口测试代理）；可用 `PROXY_PORT` / `MOCK_PORT` / `RANDOM_PROXY_PORT` 覆盖。

## 文件结构

```text
src/main.c          启动、信号处理、poll 主循环
src/config.c        配置文件与命令行解析
src/supervisor.c    拉起 dsh、读取输出、自动回答、重启、停止
src/upstream.c      token URL 解析、cookie 交换/刷新、401 处理
src/proxy.c         监听、HTTP/1.1 与 WebSocket 代理、头部改写
src/net.c           socket / DNS / 监听 / 连接
src/util.c          动态缓冲、URL/头部工具、超时 I/O
src/log.c           日志
packaging/          dsh-proxy.conf、systemd system/user unit 与模板
scripts/install.sh      系统级安装（unit 仍以 User=... 运行）
scripts/install-user.sh systemd --user 安装（推荐，始终当前用户）
tests/              mock dsh、WebSocket 客户端、端到端测试
```
