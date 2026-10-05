// 跨平台文件/路径工具（C++14，不使用 std::filesystem）
// POSIX 使用 UTF-8 字节路径，Windows 内部转换为 UTF-16 宽字符调用系统 API

#define _CRT_SECURE_NO_WARNINGS

#ifndef FS_UTIL_H
#define FS_UTIL_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace fsutil {

struct Entry {
  std::string name;      // UTF-8 文件名
  bool is_dir;
  std::uint64_t size;    // 目录为 0
  long long mtime;       // Unix 时间戳（秒）
};

// ---------- 路径处理（逻辑路径内部统一使用 '/'） ----------
std::string join(const std::string &a, const std::string &b);
std::string absolute(const std::string &path);
std::string canonical(const std::string &path); // 归一化并解析 ./ ../
bool has_trailing_sep(const std::string &path);
std::string trim_trailing_sep(const std::string &path);

// ---------- 文件系统状态 ----------
bool exists(const std::string &path);
bool is_dir(const std::string &path);
bool size_of(const std::string &path, std::uint64_t &out);
long long mtime_of(const std::string &path);
bool list_dir(const std::string &path, std::vector<Entry> &out); // 目录优先、按名排序
bool make_dir(const std::string &path);
bool ensure_dir(const std::string &path); // 递归创建缺失的父目录（单层的父目录）
bool remove_all(const std::string &path);                        // 文件/递归目录

// ---------- 跨平台文件读写 ----------
class File {
public:
  File();
  ~File();
  File(const File &) = delete;
  File &operator=(const File &) = delete;

  bool open_read(const std::string &path);
  bool open_write(const std::string &path);   // 覆盖写（wb）
  bool open_append(const std::string &path);  // 追加写（ab），断点续传用
  bool seek(std::uint64_t off);
  bool read(void *buf, std::size_t n, std::size_t &got);
  bool write(const void *buf, std::size_t n);
  void close();
  bool is_open() const { return fp_ != nullptr; }

private:
  void *fp_;
};

// ---------- 编码与展示 ----------
std::string url_encode(const std::string &s);                 // 保留 '/'
std::string url_decode(const std::string &s);                 // %XX -> 字节
std::string html_escape(const std::string &s);
std::string mime_type(const std::string &filename);
std::string human_size(std::uint64_t n);
std::string time_str(long long t);

// 规范化后的 root 是否是 target 的祖先（防止 ../ 穿越）
bool is_under(const std::string &root_canonical, const std::string &target_canonical);

} // namespace fsutil

#endif // FS_UTIL_H
