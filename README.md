# fileserver —— C++14 跨平台 Demo 文件服务器

基于 [cpp-httplib v0.14.3](https://github.com/yhirose/cpp-httplib)（已 vendored 在 `httplib.h`），
只用 **C++14** 语法，不使用 `std::filesystem`，可在 **Linux  / Windows** 上编译运行。

## 功能

| 能力 | 说明 |
|---|---|
| 目录浏览 | 自动生成 HTML 列表，目录优先排序，中文名/UTF-8 正常显示 |
| 文件下载 | 流式读取（64KB 分块），不整个载入内存；支持 `?dl=1` 触发下载 |
| 断点续传 | 标准 HTTP Range，`Accept-Ranges: bytes` + `206 Partial Content` |
| 上传 | 表单 multipart 多文件上传 + `curl -T` 直传（PUT） |
| 删除 | 删除文件 / 递归删除目录 |
| 新建目录 | 页面表单直接建目录 |
| 只读模式 | `--readonly` 关闭一切写操作 |
| 安全防护 | 拒绝 `..`、拒绝百分号编码穿越、规范化后校验必须在根目录内 |

## 目录结构

```
fileserver/
├── httplib.h        # cpp-httplib v0.14.3（第三方，header-only）
├── fs_util.h        # 跨平台路径/文件/编码工具声明
├── fs_util.cpp      # POSIX 与 Win32 两套实现
├── fileserver.cpp   # 服务器主逻辑与路由
├── Makefile         # 已验证（Linux + MinGW）
├── build            # 已验证（默认输出文件目录）
└── README.md

```

## 构建

### Linux / macOS

```bash

make clean

make                      # 产物 ./fileserver

```

### Windows（MinGW）

```bash
g++ -std=c++14 -O2 -o fileserver.exe fileserver.cpp fs_util.cpp -lws2_32 -lmswsock
```

> Windows 需要链接 `ws2_32`；MSVC 与 MinGW 都已提供宽字符（UTF-16）实现路径，中文文件名可用。

## 运行

```bash
./fileserver -r ./share -p 9876            # 服务 ./share，端口 9876
./fileserver -r D:\\data -p 9876 --readonly # 只读模式
./fileserver -H 127.0.0.1 -p 9876       # 只监听本机（默认 0.0.0.0）
./fileserver --help


使用脚本启动 ./fileserver.h #默认端口9876 根目录是程序目录同级的 root_dir
```

启动后浏览器打开：<http://127.0.0.1:9876/files/>

## HTTP 接口

| 方法 | 路径 | 作用 |
|---|---|---|
| GET | `/files/<path>` | 目录 → HTML 列表；文件 → 下载（支持 Range） |
| HEAD | `/files/<path>` | 仅响应头 |
| PUT | `/files/<path>` | 原始 body 直传落盘（`curl -T`） |
| DELETE | `/files/<path>` | 删除文件或递归删除目录 |
| POST | `/upload?path=<dir>` | multipart 表单上传，可多文件 |
| POST | `/mkdir` | 表单字段 `path` + `name` 新建目录 |
| GET | `/healthz` | 健康检查，返回 `ok` |

## curl 用法示例

```bash
# 列目录
curl http://127.0.0.1:9876/files/

# 下载
curl -O http://127.0.0.1:9876/files/readme.txt

# 断点续传（取前 100 字节）
curl -r 0-99 http://127.0.0.1:9876/files/big.bin -o part.bin

# 单文件上传（表单）
curl -F "file=@./local.txt" "http://127.0.0.1:9876/upload?path=docs"

# 直传
curl -T ./local.txt http://127.0.0.1:9876/files/local.txt

# 删除
curl -X DELETE http://127.0.0.1:9876/files/local.txt
```

## 实现要点

- **不用 `std::filesystem`**：`fs_util` 用 `dirent/stat` 与 `FindFirstFileW/GetFileAttributesW` 双实现，
  Windows 侧把 UTF-8 路径转 UTF-16 后调用系统 API。
- **零拷贝思路**：下载用 `set_content_provider(size, mime, [](offset, length, sink){})`，
  由 httplib 解析 Range 并回 206，服务端按 offset 定位、分块落盘读取，大文件不占内存。
- **路径安全**：逐段拒绝 `..`（httplib 已先做过百分号解码，可防编码穿越），
  再做 `canonical` 归一化并用 `is_under` 校验前缀，符号链接指向根外也会被拦。

## 已知边界（Demo 阶段的取舍）

- 无鉴权：任何能访问端口的人都可读写，请只在内网/本机使用；需要登录可加 Basic Auth 或反向代理。
- HTTP 明文传输，未启用 TLS。
- 未做限速、配额、并发数限制与磁盘空间检查。
- Windows 下 `canonical` 不解析 junction / 符号链接（依赖 `GetFullPathNameW`），Linux/macOS 用 `realpath` 已处理。
- 文件名含 `%` 时可能与百分号解码冲突（HTTP 路径语义固有问题）。
- 前端页面为极简 HTML，未做分页，超大目录（上万项）建议分页或换前端框架。

## 往生产演进的建议

1. 加日志落盘、限速、配额与监控指标。
2. 大目录分页 + 前端搜索，或改用 WebDAV 协议。



