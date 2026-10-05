#include "fs_util.h"

#ifdef _WIN32
#define _CRT_SECURE_NO_WARNINGS // MSVC：允许 fopen/_wfopen 等
#endif

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <direct.h>   // _wmkdir / _wrmdir
#include <wchar.h>    // _wremove / _wfopen
#include <sys/stat.h>
#include <sys/types.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace fsutil {

#ifdef _WIN32
namespace {
std::wstring to_wide(const std::string &utf8) {
  if (utf8.empty()) return std::wstring();
  int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), NULL, 0);
  std::wstring out((size_t)n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), &out[0], n);
  return out;
}

std::string to_utf8(const std::wstring &wide) {
  if (wide.empty()) return std::string();
  int n = WideCharToMultiByte(CP_UTF8, 0, wide.data(), (int)wide.size(), NULL, 0,
                              NULL, NULL);
  std::string out((size_t)n, '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide.data(), (int)wide.size(), &out[0], n,
                      NULL, NULL);
  return out;
}

// 把混合分隔符统一成 '\\'（Windows 原生 API 偏好反斜杠）
std::wstring win_native(const std::string &path) {
  std::wstring w = to_wide(path);
  for (size_t i = 0; i < w.size(); i++) {
    if (w[i] == L'/') w[i] = L'\\';
  }
  return w;
}
} // namespace
#endif

// ---------------- 路径处理 ----------------

bool has_trailing_sep(const std::string &path) {
  if (path.empty()) return false;
  char c = path[path.size() - 1];
  return c == '/' || c == '\\';
}

std::string trim_trailing_sep(const std::string &path) {
  std::string p = path;
  while (p.size() > 1 && has_trailing_sep(p)) p.erase(p.size() - 1);
  return p;
}

std::string join(const std::string &a, const std::string &b) {
  if (a.empty()) return b;
  if (b.empty()) return a;
  if (has_trailing_sep(a)) return a + b;
  return a + "/" + b;
}

static std::string normalize_slashes(const std::string &in) {
  std::string p = in;
#ifdef _WIN32
  for (size_t i = 0; i < p.size(); i++) {
    if (p[i] == '\\') p[i] = '/';
  }
#endif
  return p;
}

// 纯文本归一化：去掉 "."，折叠 ".."，压缩重复 '/'
// 注意：必须正确处理 Windows 盘符前缀（C:/foo），否则会错误地拼成 "/C:/foo"，
// 进而导致后续 is_under 前缀校验失败（表现为一切写操作 403 forbidden）。
// 这里不依赖 _WIN32 宏判断，POSIX 下几乎不可能出现 "X:" 开头的路径，无副作用。
static std::string normalize_text(const std::string &in, bool &had_escape) {
  had_escape = false;
  std::string p = normalize_slashes(in);

  std::string prefix; // "/" 或 "C:"（盘符）
  size_t start = 0;
  if (p.size() >= 2 && p[1] == ':') { // 盘符：C:/foo 或 C:foo
    prefix = p.substr(0, 2);
    start = 2;
    if (p.size() > 2 && p[2] == '/') start = 3;
  } else if (!p.empty() && p[0] == '/') { // POSIX 绝对；Windows 下形如 /foo 视为根相对
    prefix = "/";
    start = 1;
  }

  std::vector<std::string> segs;
  size_t i = start;
  while (i < p.size()) {
    size_t j = p.find('/', i);
    if (j == std::string::npos) j = p.size();
    std::string seg = p.substr(i, j - i);
    i = j + 1;
    if (seg.empty() || seg == ".") continue;
    if (seg == "..") {
      if (segs.empty()) {
        had_escape = true; // 已经越过根目录
        continue;
      }
      segs.pop_back();
      continue;
    }
    segs.push_back(seg);
  }

  std::string out = prefix;
  for (size_t k = 0; k < segs.size(); k++) {
    if (!out.empty() && out[out.size() - 1] != '/') out += "/";
    out += segs[k];
  }
  if (out.empty()) out = ".";
  return out;
}

std::string absolute(const std::string &path) {
#ifdef _WIN32
  std::wstring w = win_native(path);
  DWORD n = GetFullPathNameW(w.c_str(), 0, NULL, NULL);
  if (n == 0) return normalize_slashes(path);
  std::wstring buf((size_t)n, L'\0');
  if (GetFullPathNameW(w.c_str(), n, &buf[0], NULL) == 0) {
    return normalize_slashes(path);
  }
  return normalize_slashes(to_utf8(buf.c_str()));
#else
  if (!path.empty() && path[0] == '/') return normalize_slashes(path);
  char cwd[4096];
  if (!getcwd(cwd, sizeof(cwd))) return normalize_slashes(path);
  return normalize_slashes(std::string(cwd) + "/" + path);
#endif
}

std::string canonical(const std::string &path) {
  bool escaped = false;
  std::string text = normalize_text(absolute(path), escaped);
#ifdef _WIN32
  return text; // GetFullPathNameW 已处理相对路径；符号链接/ junction 不做解析
#else
  char *rp = realpath(text.c_str(), NULL);
  if (rp) {
    std::string out(rp);
    free(rp);
    return out;
  }
  return text; // 路径尚不存在（如上传目标）时退化为文本归一化
#endif
}

bool is_under(const std::string &root_canonical, const std::string &target_canonical) {
  std::string r = trim_trailing_sep(normalize_slashes(root_canonical));
  std::string t = trim_trailing_sep(normalize_slashes(target_canonical));
  if (r.empty()) return true;

#ifdef _WIN32
  std::string rl = r, tl = t;
  std::transform(rl.begin(), rl.end(), rl.begin(), ::tolower);
  std::transform(tl.begin(), tl.end(), tl.begin(), ::tolower);
  if (tl == rl) return true;
  // root 形如 "/" 或 "C:/"（文件系统/盘符根）：其下所有路径都应放行，
  // 不能要求再出现一个 '/' 作为分隔，否则会把 "/videos" 误判为越界。
  if (!rl.empty() && rl[rl.size() - 1] == '/') {
    return tl.size() > rl.size() && tl.compare(0, rl.size(), rl) == 0;
  }
  return tl.size() > rl.size() && tl.compare(0, rl.size(), rl) == 0 &&
         (tl[rl.size()] == '/');
#else
  if (t == r) return true;
  if (!r.empty() && r[r.size() - 1] == '/') {
    return t.size() > r.size() && t.compare(0, r.size(), r) == 0;
  }
  return t.size() > r.size() && t.compare(0, r.size(), r) == 0 && t[r.size()] == '/';
#endif
}

// ---------------- 文件系统状态 ----------------

bool exists(const std::string &path) {
#ifdef _WIN32
  DWORD attr = GetFileAttributesW(win_native(path).c_str());
  return attr != INVALID_FILE_ATTRIBUTES;
#else
  struct stat st;
  return ::stat(path.c_str(), &st) == 0;
#endif
}

bool is_dir(const std::string &path) {
#ifdef _WIN32
  DWORD attr = GetFileAttributesW(win_native(path).c_str());
  return (attr != INVALID_FILE_ATTRIBUTES) &&
         (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
#else
  struct stat st;
  return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

bool size_of(const std::string &path, std::uint64_t &out) {
#ifdef _WIN32
  struct _stati64 st;
  if (_wstati64(win_native(path).c_str(), &st) != 0) return false;
  out = (std::uint64_t)st.st_size;
  return true;
#else
  struct stat st;
  if (::stat(path.c_str(), &st) != 0) return false;
  out = (std::uint64_t)st.st_size;
  return true;
#endif
}

long long mtime_of(const std::string &path) {
#ifdef _WIN32
  struct _stati64 st;
  if (_wstati64(win_native(path).c_str(), &st) != 0) return 0;
  return (long long)st.st_mtime;
#else
  struct stat st;
  if (::stat(path.c_str(), &st) != 0) return 0;
  return (long long)st.st_mtime;
#endif
}

bool list_dir(const std::string &path, std::vector<Entry> &out) {
  out.clear();
#ifdef _WIN32
  std::wstring pattern = win_native(path);
  if (!pattern.empty() && pattern[pattern.size() - 1] != L'\\') pattern += L'\\';
  pattern += L'*';
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return false;
  do {
    std::string name = to_utf8(fd.cFileName);
    if (name == "." || name == "..") continue;
    Entry e;
    e.name = name;
    e.is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    if (e.is_dir) {
      e.size = 0;
    } else {
      ULARGE_INTEGER li;
      li.HighPart = fd.nFileSizeHigh;
      li.LowPart = fd.nFileSizeLow;
      e.size = (std::uint64_t)li.QuadPart;
    }
    FILETIME ft = fd.ftLastWriteTime;
    ULARGE_INTEGER uli;
    uli.HighPart = ft.dwHighDateTime;
    uli.LowPart = ft.dwLowDateTime;
    // FILETIME(1601) -> Unix(1970)
    e.mtime = (long long)((uli.QuadPart - 116444736000000000ULL) / 10000000ULL);
    out.push_back(e);
  } while (FindNextFileW(h, &fd));
  FindClose(h);
#else
  DIR *d = ::opendir(path.c_str());
  if (!d) return false;
  while (struct dirent *de = ::readdir(d)) {
    std::string name = de->d_name;
    if (name == "." || name == "..") continue;
    Entry e;
    e.name = name;
    e.is_dir = false;
    e.size = 0;
    e.mtime = 0;
    std::string full = join(path, name);
    struct stat st;
    if (::stat(full.c_str(), &st) == 0) {
      e.is_dir = S_ISDIR(st.st_mode);
      e.size = e.is_dir ? 0 : (std::uint64_t)st.st_size;
      e.mtime = (long long)st.st_mtime;
    }
    out.push_back(e);
  }
  ::closedir(d);
#endif
  std::sort(out.begin(), out.end(), [](const Entry &a, const Entry &b) {
    if (a.is_dir != b.is_dir) return a.is_dir > b.is_dir;
    return a.name < b.name;
  });
  return true;
}

bool make_dir(const std::string &path) {
#ifdef _WIN32
  return _wmkdir(win_native(path).c_str()) == 0 || is_dir(path);
#else
  return ::mkdir(path.c_str(), 0755) == 0 || is_dir(path);
#endif
}

static std::string parent_of(const std::string &p) {
  std::string q = trim_trailing_sep(normalize_slashes(p));
  size_t pos = q.find_last_of('/');
  if (pos == std::string::npos) return std::string();
  if (pos == 0) return std::string(1, q[0]); // "/xxx" 的父是 "/"
  return q.substr(0, pos);
}

bool ensure_dir(const std::string &path) {
  std::string parent = parent_of(path);
  if (parent.empty()) return true; // 没有父目录（相对路径），交给调用方/系统默认
  if (is_dir(parent)) return true;

  // 自顶向下逐级创建
  std::vector<std::string> chain;
  std::string cur = parent;
  for (;;) {
    if (cur.empty() || is_dir(cur)) break;
    chain.push_back(cur);
    std::string up = parent_of(cur);
    if (up == cur || up.empty()) break;
    cur = up;
  }
  for (size_t i = chain.size(); i-- > 0;) {
    if (!make_dir(chain[i])) return false;
  }
  return is_dir(parent);
}

bool remove_all(const std::string &path) {
  if (!exists(path)) return false;
  if (!is_dir(path)) {
#ifdef _WIN32
    return _wremove(win_native(path).c_str()) == 0;
#else
    return ::remove(path.c_str()) == 0;
#endif
  }
  std::vector<Entry> entries;
  if (list_dir(path, entries)) {
    for (size_t i = 0; i < entries.size(); i++) {
      remove_all(join(path, entries[i].name));
    }
  }
#ifdef _WIN32
  return _wrmdir(win_native(path).c_str()) == 0;
#else
  return ::rmdir(path.c_str()) == 0;
#endif
}

// ---------------- 文件读写 ----------------

File::File() : fp_(nullptr) {}

File::~File() { close(); }

bool File::open_read(const std::string &path) {
  close();
#ifdef _WIN32
  fp_ = (void *)::_wfopen(win_native(path).c_str(), L"rb");
#else
  fp_ = (void *)std::fopen(path.c_str(), "rb");
#endif
  return fp_ != nullptr;
}

bool File::open_write(const std::string &path) {
  close();
#ifdef _WIN32
  fp_ = (void *)::_wfopen(win_native(path).c_str(), L"wb");
#else
  fp_ = (void *)std::fopen(path.c_str(), "wb");
#endif
  return fp_ != nullptr;
}

bool File::open_append(const std::string &path) {
  close();
#ifdef _WIN32
  fp_ = (void *)::_wfopen(win_native(path).c_str(), L"ab");
#else
  fp_ = (void *)std::fopen(path.c_str(), "ab");
#endif
  return fp_ != nullptr;
}

bool File::seek(std::uint64_t off) {
  if (!fp_) return false;
#ifdef _WIN32
  return ::_fseeki64((FILE *)fp_, (long long)off, SEEK_SET) == 0;
#else
  return ::fseeko((FILE *)fp_, (off_t)off, SEEK_SET) == 0;
#endif
}

bool File::read(void *buf, std::size_t n, std::size_t &got) {
  got = 0;
  if (!fp_) return false;
  size_t r = std::fread(buf, 1, n, (FILE *)fp_);
  got = r;
  if (r == 0 && n > 0) return std::ferror((FILE *)fp_) == 0; // EOF 视为正常结束
  return true;
}

bool File::write(const void *buf, std::size_t n) {
  if (!fp_) return false;
  return std::fwrite(buf, 1, n, (FILE *)fp_) == n;
}

void File::close() {
  if (fp_) {
    std::fclose((FILE *)fp_);
    fp_ = nullptr;
  }
}

// ---------------- 编码与展示 ----------------

static const char *kHex = "0123456789ABCDEF";

std::string url_encode(const std::string &s) {
  std::string out;
  for (size_t i = 0; i < s.size(); i++) {
    unsigned char c = (unsigned char)s[i];
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
        c == '~' || c == '/') {
      out += (char)c;
    } else {
      out += '%';
      out += kHex[(c >> 4) & 0xF];
      out += kHex[c & 0xF];
    }
  }
  return out;
}

std::string url_decode(const std::string &s) {
  std::string out;
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] == '%' && i + 2 < s.size()) {
      auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
      };
      int h = hex(s[i + 1]), l = hex(s[i + 2]);
      if (h >= 0 && l >= 0) {
        out += (char)((h << 4) | l);
        i += 2;
        continue;
      }
    }
    out += s[i];
  }
  return out;
}

std::string html_escape(const std::string &s) {
  std::string out;
  for (size_t i = 0; i < s.size(); i++) {
    char c = s[i];
    switch (c) {
    case '&': out += "&amp;"; break;
    case '<': out += "&lt;"; break;
    case '>': out += "&gt;"; break;
    case '"': out += "&quot;"; break;
    case '\'': out += "&#39;"; break;
    default: out += c; break;
    }
  }
  return out;
}

std::string mime_type(const std::string &filename) {
  std::string ext;
  size_t dot = filename.rfind('.');
  if (dot != std::string::npos && dot + 1 < filename.size()) {
    ext = filename.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
  }
  struct Mapping { const char *ext; const char *type; };
  static const Mapping table[] = {
      {"html", "text/html; charset=utf-8"},
      {"htm", "text/html; charset=utf-8"},
      {"css", "text/css; charset=utf-8"},
      {"js", "application/javascript; charset=utf-8"},
      {"json", "application/json; charset=utf-8"},
      {"txt", "text/plain; charset=utf-8"},
      {"md", "text/markdown; charset=utf-8"},
      {"csv", "text/csv; charset=utf-8"},
      {"xml", "application/xml; charset=utf-8"},
      {"png", "image/png"},
      {"jpg", "image/jpeg"},
      {"jpeg", "image/jpeg"},
      {"gif", "image/gif"},
      {"bmp", "image/bmp"},
      {"svg", "image/svg+xml"},
      {"ico", "image/x-icon"},
      {"webp", "image/webp"},
      {"mp3", "audio/mpeg"},
      {"wav", "audio/wav"},
      {"mp4", "video/mp4"},
      {"webm", "video/webm"},
      {"mov", "video/quicktime"},
      {"pdf", "application/pdf"},
      {"zip", "application/zip"},
      {"gz", "application/gzip"},
      {"tar", "application/x-tar"},
      {"7z", "application/x-7z-compressed"},
      {"rar", "application/x-rar-compressed"},
      {"exe", "application/octet-stream"},
      {"dll", "application/octet-stream"},
      {"so", "application/octet-stream"},
      {"bin", "application/octet-stream"},
  };
  for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
    if (ext == table[i].ext) return table[i].type;
  }
  return "application/octet-stream";
}

std::string human_size(std::uint64_t n) {
  const char *units[] = {"B", "KB", "MB", "GB", "TB"};
  double v = (double)n;
  size_t u = 0;
  while (v >= 1024.0 && u < 4) {
    v /= 1024.0;
    u++;
  }
  char buf[64];
  if (u == 0) {
    std::snprintf(buf, sizeof(buf), "%llu B", (unsigned long long)n);
  } else {
    std::snprintf(buf, sizeof(buf), "%.1f %s", v, units[u]);
  }
  return std::string(buf);
}

std::string time_str(long long t) {
  if (t <= 0) return "-";
  time_t tt = (time_t)t;
  struct tm tm_buf;
#ifdef _WIN32
  localtime_s(&tm_buf, &tt);
#else
  localtime_r(&tt, &tm_buf);
#endif
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tm_buf);
  return std::string(buf);
}

} // namespace fsutil
