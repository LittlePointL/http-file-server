#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
HTTP 文件服务器测试脚本（纯标准库）
功能：
  1 -> 列出服务器 /files/ 目录下的文件
  2 -> 下载 /files/ 目录下所有文件到 ./downloads/
  3 -> 上传指定文件到服务器 /files/
"""

import http.client
import urllib.parse
import urllib.request
import urllib.error
import os
import sys
import html.parser
import mimetypes
from pathlib import Path

import requests

import pytest


import re


BASE_HOST = "127.0.0.1"
BASE_PORT = 9876
BASE_PATH = "/files/"


class LinkParser(html.parser.HTMLParser):
    """从目录列表 HTML 中提取文件名（a 标签）"""
    def __init__(self, base_url):
        super().__init__()
        self.base_url = base_url
        self.links = []

    def handle_starttag(self, tag, attrs):
        if tag == "a":
            for attr, value in attrs:
                if attr == "href":
                    # 跳过父目录链接和查询参数
                    if value and not value.startswith("?") and not value.startswith("/"):
                        self.links.append(value)


def list_files(remote_dir = ""):
    """功能1：列出服务器根目录下的文件"""
    print(f"[*] 正在请求 {BASE_PATH} ...")
    try:
        URL = f"http://{BASE_HOST}:{BASE_PORT}{BASE_PATH}/test"
        # 1. 请求页面
        with urllib.request.urlopen(URL) as resp:
            html = resp.read().decode("utf-8")

        files = []

        # 2. 提取所有 <tr>...</tr>
        trs = re.findall(r"<tr>(.*?)</tr>", html, re.DOTALL)

        for tr in trs[1:]:  # 跳过表头
            # 3. 提取所有 <td>...</td>
            tds = re.findall(r"<td[^>]*>(.*?)</td>", tr, re.DOTALL)
            if len(tds) < 3:
                continue

        # 4. 提取文件名
            name_match = re.search(r">([^<]+)</a>", tds[0])
            if not name_match:
                continue
            name = name_match.group(1).strip()

        # 跳过父目录
            if name in ("../", ".."):
                continue

            size = re.sub(r"<[^>]+>", "", tds[1]).strip()
            mtime = re.sub(r"<[^>]+>", "", tds[2]).strip()

            files.append({
                "name": name,
                "size": size,
                "mtime": mtime
            })

        # 5. 输出结果
        for f in files:
            print(f"{f['name']}\t{f['size']}\t{f['mtime']}")

        if resp.status != 200:
            print(f"[!] 服务器返回 {resp.status} {resp.reason}")
            return []

        html_data = resp.read().decode("utf-8", errors="ignore")
        parser = LinkParser(BASE_PATH)
        parser.feed(html_data)

        
        return files

    except Exception as e:
        print(f"[!] 连接失败: {e}")
        return []


def download_all(files):
    print(files)
    if not files:
        print("[!] 没有文件可下载")
        return

    Path("./downloads").mkdir(exist_ok=True)
    
    for fileinfo in files:
        filename = fileinfo['name']
        n = urllib.parse.quote(filename)

        print(n)

        url = f"http://{BASE_HOST}:{BASE_PORT}{BASE_PATH}{n}"
        save_path = f"./downloads/{filename}"
        print(f"[*] 下载: {filename} ...")
        try:
            urllib.request.urlretrieve(url, save_path)
            size = os.path.getsize(save_path)
            print(f"[+] 成功 -> {save_path} ({size} bytes)")
        except urllib.error.HTTPError as e:
            print(f"[!] 下载失败 {filename}: HTTP {e.code}")
        except Exception as e:
            print(f"[!] 下载失败 {filename}: {e}")


def upload_file(localfile = "",remote_dir = ""):
    """上传指定文件到服务器"""
    if len(localfile) == 0:
        localfile = input("[?] 请输入要上传的文件路径: ").strip()

    if not os.path.exists(localfile):
        print(f"[!] 文件不存在: {localfile}")
        return
    
    #localfile = os.path.basename(localfile)
    mime_type, _ = mimetypes.guess_type(localfile)
    if mime_type is None:
        mime_type = "application/octet-stream"

    print(f"[*] 上传 {localfile} (type={mime_type}) ...")


    url = f"http://{BASE_HOST}:{BASE_PORT}/upload"
    
    with open(localfile, "rb") as f:
        r = requests.post(url,
                        files={"file": f},
                        params={"path":remote_dir})


    if r.status_code in (200, 201, 204):
        print(f"[+] 上传成功 (HTTP {r.status_code})")
    else:
        print(f"[!] 上传失败: HTTP {r.status_code}")

    return


def delete_file(remote_file):
    """上传指定文件到服务器"""
    if len(remote_file) == 0:
        remote_file = input("[?] 请输入要上传的文件路径: ").strip()

    print(f"[*] 删除 {remote_file} ...")

    url = f"http://{BASE_HOST}:{BASE_PORT}/files/{remote_file}"
    
    r = requests.delete(url)

    if r.status_code in (200, 201, 204):
        print(f"[+] 删除成功 (HTTP {r.status_code})")
    else:
        print(f"[!] 删除失败: HTTP {r.status_code}")

    return



def mk_dir(remote_dir):
    """上传指定文件到服务器"""
    if len(remote_dir) == 0:
        remote_dir = input("[?] 请输入要新增的目录: ").strip()

    url = f"http://{BASE_HOST}:{BASE_PORT}/mkdir"

    params = {
        "path": "./",
        "name": remote_dir
    }

    print(f"[*] 生成目录 {remote_dir} ...")

    r = requests.post(url, params)

    if r.status_code in (200, 201, 204):
        print(f"[+] 上传成功 (HTTP {r.status_code})")
    else:
        print(f"[!] 上传失败: HTTP {r.status_code}")

    return 



'''def main():
    print("=" * 50)
    print("  HTTP 文件服务器测试工具")
    print(f"  服务器: http://{BASE_HOST}:{BASE_PORT}{BASE_PATH}")
    print("=" * 50)
    print("  1 -> 列出服务器文件")
    print("  2 -> 下载所有文件")
    print("  3 -> 上传文件")
    print("  4 -> 上传目录")
    print("  5 -> 删除文件")
    print("  q -> 退出")
    print("-" * 50)

    # 先缓存文件列表
    cached_files = []

    while True:
        choice = input("\n[?] 请选择操作: ").strip()
        if choice == "q":
            print("[*] 退出")
            break
        elif choice == "1":
            cached_files = list_files()
        elif choice == "2":
            if not cached_files:
                print("[*] 先执行 1 获取文件列表...")
                cached_files = list_files()
            download_all(cached_files)
        elif choice == "3":
            upload_file()
        elif choice == "4":
            mk_dir()
        elif choice == "5":
            delete_file()
        else:
            print("[!] 无效选择")'''


def test_upload_file0():
    upload_file("./test_file/testhttp.py")
    upload_file("./test_file/random_1gb.bin")
    upload_file(r"./test_file/test\x00.txt")
    upload_file(r"./test_file/test?.txt")
    upload_file(r"./test_file/test#.txt")
    upload_file(r"./test_file/test%00.txt")
    upload_file("./test_file/测试😀.txt")
    upload_file("./test_file/测试😀.txt","new_dir")

def test_mk_dir0():
    mk_dir("临时文件夹")
    mk_dir("临时0")
    mk_dir("临时0/临时0")


def test_delete_file0():
    delete_file("./test_file/testhttp.py")
    delete_file("不存在的文件.py")
    delete_file("临时文件夹")

def test_list_file0():
    list_files("test")



#if __name__ == "__main__":
#    main()


