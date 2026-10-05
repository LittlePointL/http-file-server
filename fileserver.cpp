// 跨平台 Demo 级文件服务器（C++14 + cpp-httplib v0.14.3）
//
// 功能：目录浏览 / 文件下载（支持断点续传）/ 上传（PUT 与表单）/ 删除 / 新建目录
// 构建：见 README.md
#include "httplib.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "fs_util.h"

using httplib::Request;
using httplib::Response;
using httplib::Server;

namespace {

const size_t kChunk = 64 * 1024;

std::string g_root;        // 规范化后的根目录
std::string g_root_display;
bool g_readonly = false;
bool g_debug = false;      // 开启后 403/404 会回显规范化路径，便于排查

// URL 子路径 -> 安全的本地绝对路径
bool resolve(const std::string &sub, std::string &out, std::string &err) {
  // 1) 逐段检查，显式拒绝 ".."（httplib 已做百分号解码，所以还要防编码穿越）
  std::vector<std::string> segs;
  std::string cur;
  for (size_t i = 0; i <= sub.size(); i++) {
    if (i == sub.size() || sub[i] == '/' || sub[i] == '\\') {
      if (!cur.empty()) segs.push_back(cur);
      cur.clear();
      continue;
    }
    cur += sub[i];
  }
  for (size_t i = 0; i < segs.size(); i++) {
    if (segs[i] == "..") {
      err = "invalid path";
      return false;
    }
  }

  std::string path = g_root;
  for (size_t i = 0; i < segs.size(); i++) path = fsutil::join(path, segs[i]);

  std::string can = fsutil::canonical(path);
  if (!fsutil::is_under(g_root, can)) { // 2) 兜底：符号链接等仍可能被解析到根外
    err = "forbidden";
    if (g_debug) {
      err += ": target=" + can + " root=" + g_root + " sub=" + sub;
    }
    return false;
  }
  out = can;
  return true;
}

bool resolve_or_fail(const Request &req, Response &res, const std::string &sub,
                     std::string &out) {
  (void)req;
  std::string err;
  if (!resolve(sub, out, err)) {
    res.status = 403;
    res.set_content(err + "\n", "text/plain; charset=utf-8");
    return false;
  }
  return true;
}

std::string sub_of(const Request &req) {
  std::string s;
  if (req.matches.size() > 1) s = req.matches[1];
  // 目录请求形如 /files/docs/ ，去掉尾斜杠，否则父目录链接会算错
  while (!s.empty() && s[s.size() - 1] == '/') s.erase(s.size() - 1);
  return s;
}

std::string html_head(const std::string &sub) {
  std::string title = "Index of /" + sub;
  std::ostringstream o;
  o << "<!doctype html><html><head><meta charset=\"utf-8\">"
    << "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    << "<title>" << fsutil::html_escape(title) << "</title><style>"
    << "body{font-family:-apple-system,Segoe UI,Microsoft YaHei,sans-serif;"
       "margin:24px;color:#222;background:#fafafa}"
    << "h1{font-size:20px;font-weight:600}"
    << ".bar{display:flex;gap:16px;flex-wrap:wrap;align-items:center;"
       "margin:12px 0 18px;padding:12px;background:#fff;border:1px solid #e5e5e5;"
       "border-radius:8px}"
    << "table{border-collapse:collapse;width:100%;background:#fff;"
       "border:1px solid #e5e5e5;border-radius:8px;overflow:hidden}"
    << "th,td{padding:8px 12px;text-align:left;border-bottom:1px solid #f0f0f0;"
       "font-size:14px}"
    << "th{background:#f7f7f7;color:#666;font-weight:600}"
    << "a{color:#1677ff;text-decoration:none}a:hover{text-decoration:underline}"
    << ".size,.time{color:#888;white-space:nowrap}"
    << ".del{color:#d4380d;cursor:pointer;background:none;border:none;font-size:14px}"
    << "button,input[type=file]{font-size:14px}"
    << "button{padding:5px 14px;border:1px solid #d9d9d9;background:#fff;"
       "border-radius:6px;cursor:pointer}"
    << "button:hover{border-color:#1677ff;color:#1677ff}"
    << "input[type=text]{padding:5px 8px;border:1px solid #d9d9d9;border-radius:6px}"
    << ".hint{color:#999;font-size:12px;margin-top:16px}"
    << "</style></head><body>";
  return o.str();
}

std::string render_listing(const std::string &sub, const std::string &fs_path) {
  std::vector<fsutil::Entry> entries;
  fsutil::list_dir(fs_path, entries);

  std::ostringstream o;
  o << html_head(sub);
  o << "<h1>Index of /" << fsutil::html_escape(sub) << "</h1>";

  // 面包屑
  o << u8"<div class=\"bar\"><span><a href=\"/files/\">根目录</a>";
  std::string acc;
  std::vector<std::string> segs;
  {
    std::string cur;
    for (size_t i = 0; i <= sub.size(); i++) {
      if (i == sub.size() || sub[i] == '/') {
        if (!cur.empty()) segs.push_back(cur);
        cur.clear();
        continue;
      }
      cur += sub[i];
    }
  }
  for (size_t i = 0; i < segs.size(); i++) {
    acc = fsutil::join(acc, segs[i]);
    o << " / <a href=\"/files/" << fsutil::url_encode(acc) << "/\">"
      << fsutil::html_escape(segs[i]) << "</a>";
  }
  o << "</span></div>";

  if (!g_readonly) {
    o << "<div class=\"bar\">"
      << "<form action=\"/upload?path=" << fsutil::url_encode(sub)
      << "\" method=\"post\" enctype=\"multipart/form-data\">"
      << "<input type=\"file\" name=\"file\" multiple> "
      << u8"<button type=\"submit\">上传到此目录</button></form>"
      << "<form action=\"/mkdir\" method=\"post\">"
      << "<input type=\"hidden\" name=\"path\" value=\""
      << fsutil::html_escape(sub) << "\">"
      << u8"<input type=\"text\" name=\"name\" placeholder=\"新建目录名\"> "
      << u8"<button type=\"submit\">新建目录</button></form></div>";
  }

  o << u8"<table><tr><th>名称</th><th>大小</th><th>修改时间</th><th>操作</th></tr>";
  if (!sub.empty()) {
    std::string parent = sub;
    size_t slash = parent.find_last_of('/');
    if (slash != std::string::npos) parent.erase(slash);
    else parent.clear();
    std::string parent_href =
        parent.empty() ? std::string("/files/")
                       : "/files/" + fsutil::url_encode(parent) + "/";
    o << "<tr><td><a href=\"" << parent_href
      << "\">../</a></td><td class=\"size\">-</td><td class=\"time\">-</td><td>-</td></tr>";
  }
  for (size_t i = 0; i < entries.size(); i++) {
    const fsutil::Entry &e = entries[i];
    std::string url_sub = fsutil::join(sub, e.name);
    std::string href = "/files/" + fsutil::url_encode(url_sub) + (e.is_dir ? "/" : "");
    o << "<tr><td>" << (e.is_dir ? "\xf0\x9f\x93\x81 " : "\xf0\x9f\x93\x84 ")
      << "<a href=\"" << href << "\">" << fsutil::html_escape(e.name)
      << (e.is_dir ? "/" : "") << "</a></td>"
      << "<td class=\"size\">"
      << (e.is_dir ? "-" : fsutil::human_size(e.size)) << "</td>"
      << "<td class=\"time\">" << fsutil::time_str(e.mtime) << "</td><td>";
    if (e.is_dir) {
      o << "-";
    } else {
      o << "<a href=\"" << href << u8"?dl=1\">下载</a>";
      if (!g_readonly) {
        o << u8" <button class=\"del\" onclick=\"del('" << href << u8"')\">删除</button>";
      }
    }
    o << "</td></tr>";
  }
  o << "</table>";
  if (g_readonly) o << "<div class=\"hint\">只读模式已开启。</div>";
  o << u8"<div class=\"hint\">根目录：" << fsutil::html_escape(g_root_display)
    << u8" ｜ 上传也可用：curl -T 本地文件 http://host:port/files/目标文件名</div>";
  if (!g_readonly) {
    o << u8"<script>function del(u){if(!confirm('确定删除 '+decodeURIComponent(u)+' ?'))return;"
         u8"fetch(u,{method:'DELETE'}).then(function(){location.reload();})"
         u8".catch(function(a){alert('删除失败: '+a);});}</script>";
  }
  o << "</body></html>";
  return o.str();
}

// ---------- GET / HEAD：目录浏览与文件下载 ----------
void handle_get(const Request &req, Response &res) {
  std::string sub = sub_of(req);
  std::string path;
  if (!resolve_or_fail(req, res, sub, path)) return;

  if (!fsutil::exists(path)) {
    res.status = 404;
    res.set_content("404 Not Found: " + sub + "\n", "text/plain; charset=utf-8");
    return;
  }

  if (fsutil::is_dir(path)) {
    if (!fsutil::has_trailing_sep(req.path) && !sub.empty()) {
      res.set_redirect("/files/" + fsutil::url_encode(sub) + "/", 302);
      return;
    }
    res.set_content(render_listing(sub, path), "text/html; charset=utf-8");
    return;
  }

  std::uint64_t size = 0;
  if (!fsutil::size_of(path, size)) {
    res.status = 500;
    res.set_content("cannot stat file\n", "text/plain; charset=utf-8");
    return;
  }

  std::string name = sub;
  size_t slash = name.find_last_of('/');
  if (slash != std::string::npos) name = name.substr(slash + 1);

  std::shared_ptr<fsutil::File> fp(new fsutil::File());
  if (!fp->open_read(path)) {
    res.status = 403;
    res.set_content("cannot open file\n", "text/plain; charset=utf-8");
    return;
  }

  res.set_header("Accept-Ranges", "bytes");
  res.set_header("Last-Modified", fsutil::time_str(fsutil::mtime_of(path)));
  res.set_header("Cache-Control", "no-cache");
  if (req.has_param("dl")) {
    res.set_header("Content-Disposition",
                   "attachment; filename=\"" + name + "\"; filename*=UTF-8''" +
                       fsutil::url_encode(name));
  }

  // 提供 (offset, length) 形式的 provider：cpp-httplib 会自动处理 Range 并返回 206
  res.set_content_provider(
      (size_t)size, fsutil::mime_type(name),
      [fp](size_t offset, size_t length, httplib::DataSink &sink) {
        if (!fp->seek(offset)) return false;
        std::vector<char> buf(kChunk);
        size_t left = length;
        while (left > 0) {
          size_t want = left < kChunk ? left : kChunk;
          size_t got = 0;
          if (!fp->read(&buf[0], want, got) || got == 0) return false;
          if (!sink.write(&buf[0], got)) return false;
          left -= got;
        }
        return true;
      });
}

// ---------- PUT：curl -T 直传 ----------
void handle_put(const Request &req, Response &res,
                const httplib::ContentReader &content_reader) {
  if (g_readonly) {
    res.status = 403;
    res.set_content("readonly\n", "text/plain; charset=utf-8");
    return;
  }
  std::string path;
  if (!resolve_or_fail(req, res, sub_of(req), path)) return;
  if (fsutil::is_dir(path)) {
    res.status = 400;
    res.set_content("target is a directory\n", "text/plain; charset=utf-8");
    return;
  }

  // 自动创建缺失的父目录，让「上传到 videos/a.txt」不必先手动建 videos
  if (!fsutil::ensure_dir(path)) {
    res.status = 500;
    res.set_content("cannot create parent directory\n", "text/plain; charset=utf-8");
    return;
  }

  fsutil::File f;
  if (!f.open_write(path)) {
    res.status = 500;
    res.set_content("cannot create file (permission denied?)\n",
                    "text/plain; charset=utf-8");
    return;
  }
  bool ok = content_reader(
      [&](const char *data, size_t len) { return f.write(data, len); });
  f.close();
  if (!ok) {
    res.status = 500;
    res.set_content("write failed (disk full?)\n", "text/plain; charset=utf-8");
    return;
  }
  res.status = 201;
  res.set_content("uploaded\n", "text/plain; charset=utf-8");
}

// ---------- POST /upload：multipart 表单上传（支持多文件） ----------
void handle_upload(const Request &req, Response &res,
                   const httplib::ContentReader &content_reader) {
  if (g_readonly) {
    res.status = 403;
    res.set_content("readonly\n", "text/plain; charset=utf-8");
    return;
  }
  std::string dir_sub = req.get_param_value("path");


  std::string dir;
  if (!resolve_or_fail(req, res, dir_sub, dir)) return;
  if (!fsutil::is_dir(dir)) {
    res.status = 400;
    res.set_content("target dir not found\n", "text/plain; charset=utf-8");
    return;
  }

  int saved = 0;
  std::string failure;
  fsutil::File f;

  content_reader(
      [&](const httplib::MultipartFormData &file) -> bool {
        if (file.filename.empty()) return true; // 普通表单字段，忽略
        if (file.name != "file") return true;
        if (!f.is_open()) {
          std::string target;
          if (!resolve(fsutil::join(dir_sub, file.filename), target, failure)) {
            failure = "invalid filename: " + file.filename;
            return true;
          }
          if (fsutil::is_dir(target)) {
            failure = "target is a directory: " + file.filename;
            return true;
          }
          if (!fsutil::ensure_dir(target)) {
            failure = "cannot create parent dir: " + file.filename;
            return true;
          }
          if (!f.open_write(target)) {
            failure = "cannot create: " + file.filename;
            return true;
          }
        }
        return true;
      },
      [&](const char *data, size_t len) -> bool {


        if (!f.is_open()) return true; // 非目标字段的内容，直接丢弃
        if (!f.write(data, len)) {
          failure = "write failed";
          f.close();
          return false;
        }
        return true;
      });

  f.close();
  if (!failure.empty()) {
    res.status = 500;
    res.set_content(failure + "\n", "text/plain; charset=utf-8");
    return;
  }
  saved = 1;
  (void)saved;
  res.set_redirect("/files/" + fsutil::url_encode(dir_sub) + "/", 303);
}

// ---------- DELETE：删除文件或（递归）目录 ----------
void handle_delete(const Request &req, Response &res) {
  if (g_readonly) {
    res.status = 403;
    res.set_content("readonly\n", "text/plain; charset=utf-8");
    return;
  }
  std::string sub = sub_of(req);
  if (sub.empty()) {
    res.status = 400;
    res.set_content("refuse to delete root\n", "text/plain; charset=utf-8");
    return;
  }
  std::string path;
  if (!resolve_or_fail(req, res, sub, path)) return;
  if (!fsutil::exists(path)) {
    res.status = 404;
    res.set_content("not found\n", "text/plain; charset=utf-8");
    return;
  }
  if (!fsutil::remove_all(path)) {
    res.status = 500;
    res.set_content("delete failed\n", "text/plain; charset=utf-8");
    return;
  }
  res.set_content("deleted\n", "text/plain; charset=utf-8");
}

// ---------- POST /mkdir ----------
void handle_mkdir(const Request &req, Response &res) {
  if (g_readonly) {
    res.status = 403;
    res.set_content("readonly\n", "text/plain; charset=utf-8");
    return;
  }
  std::string dir_sub = req.get_param_value("path");
  std::string name = req.get_param_value("name");
  if (name.empty() || name.find("..") != std::string::npos) {
    res.status = 400;
    res.set_content("invalid dir name\n", "text/plain; charset=utf-8");
    return;
  }
  std::string target;
  if (!resolve_or_fail(req, res, fsutil::join(dir_sub, name), target)) return;
  if (!fsutil::make_dir(target)) {
    res.status = 500;
    res.set_content("mkdir failed\n", "text/plain; charset=utf-8");
    return;
  }
  res.set_redirect("/files/" + fsutil::url_encode(dir_sub) + "/", 303);
}

void usage(const char *argv0) {
  std::cout << "用法: " << argv0 << " [选项]\n"
            << "  -r, --root <dir>   服务根目录，默认当前目录\n"
            << "  -p, --port <num>   监听端口，默认 8080\n"
            << "  -H, --host <addr>  监听地址，默认 0.0.0.0（仅本机用 127.0.0.1）\n"
            << "      --readonly     只读模式（禁止上传/删除/建目录）\n"
            << "      --debug        排错模式（403/404 回显规范化路径）\n"
            << "  -h, --help         显示帮助\n";
}

} // namespace

int main(int argc, char **argv) {



  std::string root = ".";
  std::string host = "0.0.0.0";
  int port = 9876;

  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    auto next = [&](void) -> std::string {
      if (i + 1 >= argc) {
        std::cerr << "缺少参数: " << a << "\n";
        std::exit(1);
      }
      return argv[++i];
    };
    if (a == "-r" || a == "--root") root = next();
    else if (a == "-p" || a == "--port") port = std::atoi(next().c_str());
    else if (a == "-H" || a == "--host") host = next();
    else if (a == "--readonly") g_readonly = true;
    else if (a == "--debug") g_debug = true;
    else if (a == "-h" || a == "--help") { usage(argv[0]); return 0; }
    else {
      std::cerr << "未知参数: " << a << "\n";
      usage(argv[0]);
      return 1;
    }
  }

  g_root_display = fsutil::absolute(root);
  g_root = fsutil::canonical(root);
  if (!fsutil::is_dir(g_root)) {
    std::cerr << "根目录不存在或不是目录: " << g_root << "\n";
    return 1;
  }

  // 启动自检：用一个「尚不存在」的路径走一遍安全校验。
  // mkdir / PUT 新建的目标都是不存在的路径，若 canonical 归一化有问题会在这里提前暴露，
  // 而不是等到客户端 403 才发现。
  {
    std::string probe, err;
    if (!resolve("__fileserver_selftest__", probe, err)) {
      std::cerr << "[自检失败] 服务根目录路径校验异常: " << err << "\n"
                << "  这通常意味着根目录路径归一化出错，请检查 -r 参数。\n";
      return 1;
    }
  }

  Server svr;

  svr.Get("/", [](const Request &, Response &res) { res.set_redirect("/files/", 302); });
  svr.Get("/healthz", [](const Request &, Response &res) {
    res.set_content("ok\n", "text/plain; charset=utf-8");
  });
  svr.Get("/files", [](const Request &, Response &res) { res.set_redirect("/files/", 302); });

  svr.Get("/files/(.*)", handle_get);
  svr.Put("/files/(.*)", handle_put);
  svr.Delete("/files/(.*)", handle_delete);
  svr.Post("/upload", handle_upload);
  svr.Post("/mkdir", handle_mkdir);

  svr.set_logger([](const Request &req, const Response &res) {
    std::printf("%s %s -> %d\n", req.method.c_str(), req.path.c_str(), res.status);
    std::fflush(stdout);
  });

  std::cout << "文件服务器已启动\n"
            << "  根目录 : " << g_root_display << "\n"
            << "  规范化 : " << g_root << "  (自检通过)\n"
            << "  地址   : http://" << (host == "0.0.0.0" ? "127.0.0.1" : host)
            << ":" << port << "/files/\n"
            << "  模式   : " << (g_readonly ? "只读" : "读写") << "\n"
            << "  Ctrl+C 退出\n"
            << std::flush;

  if (!svr.listen(host.c_str(), port)) {
    std::cerr << "监听失败（端口被占用？）: " << host << ":" << port << "\n";
    return 1;
  }
  return 0;
}
