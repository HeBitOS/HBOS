#!/usr/bin/env python3
"""HBOS KDE 实时监控面板：左上是屏幕画面，右下是 serial 日志。

用法:
    python3 scripts/kde_monitor.py [--port 8770]

它做两件事:
  1. 若存在 QEMU 的 monitor socket，周期性 `screendump` 到 build/tmp/monitor/screen.ppm
     并转成 screen.png；
  2. 持续 tail 最新的 serial 日志（build/tmp/kde_verify_runs/*/serial.log 或
     build/tmp/monitor/serial.log），供网页显示。

没有 monitor socket 时（例如你直接跑 `make run-kde` 用的是 -monitor none），
画面区会显示提示，但日志区仍然可用（读 build/tmp/monitor/serial.log）。
"""
import argparse
import glob
import html
import os
import socket
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "build", "tmp", "monitor")
SCREEN_PPM = os.path.join(OUT, "screen.ppm")
SCREEN_PNG = os.path.join(OUT, "screen.png")
LOG_TAIL = 220          # 网页显示最后多少行
GRAB_INTERVAL = 0.4     # 抓帧间隔（秒）

_state = {"log_path": None, "mon": None, "last_grab": 0.0, "err": ""}


def _scan_tmp(pattern):
    """递归扫 build/tmp 下所有匹配项，按 mtime 倒序返回。

    之前写死了几个固定路径，每换一个验证脚本（accept / bili / memverify…）
    就找不到 socket 或读到旧日志。改成递归扫描后，任何脚本的产物都能被发现。
    """
    out = []
    for root, _dirs, files in os.walk(os.path.join(ROOT, "build", "tmp")):
        for f in files:
            if pattern(f):
                p = os.path.join(root, f)
                try:
                    out.append((os.path.getmtime(p), p))
                except OSError:
                    pass
    out.sort(reverse=True)
    return [p for _m, p in out]


def newest_serial():
    """取最近被写过的 serial 日志（递归找）。"""
    cands = [p for p in _scan_tmp(lambda f: f.endswith("serial.log"))]
    return cands[0] if cands else None


def find_monitor_socket():
    """取最近的 QEMU monitor socket（递归找），并过滤掉已失效的。

    直接 connect 一次确认；连接失败的（残留 socket 文件）跳过。
    """
    for path in _scan_tmp(lambda f: f.endswith(".sock")):
        try:
            t = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            t.settimeout(1.0)
            t.connect(path)
            t.close()
            return path
        except Exception:
            continue
    return None


def _ppm_ready(path, timeout=3.0):
    """等 PPM 写完整：文件大小连续两次采样相同才算就绪。"""
    t0 = time.time()
    prev = -1
    while time.time() - t0 < timeout:
        try:
            cur = os.path.getsize(path)
        except OSError:
            time.sleep(0.03); continue
        if cur > 1000 and cur == prev:
            return True
        prev = cur; time.sleep(0.03)
    return False


def _share_external_frame():
    """若别的验收脚本正在产图，直接复用它，避免抢 QEMU monitor。

    QEMU 的 HMP unix socket 只接受一个客户端：验收脚本与面板同时连会互相踢掉，
    面板表现为 `抓帧: [Errno 11] Resource temporarily unavailable`。
    """
    for cand in _scan_tmp(lambda f: f in ("cur.png", "frame.png")):
        try:
            if time.time() - os.path.getmtime(cand) > 90:
                continue
            with open(cand, "rb") as f:
                data = f.read()
            if len(data) < 1000:
                continue
            tmp = SCREEN_PNG + ".tmp"
            with open(tmp, "wb") as f:
                f.write(data)
            os.replace(tmp, SCREEN_PNG)
            return True
        except Exception:
            continue
    return False


def grab_loop():
    """保持一条到 QEMU monitor 的长连接，反复 screendump。

    每次重连都要走 connect + 读欢迎横幅，延迟 300~800ms，画面就会一顿一顿。
    这里只在连接断开时重连。
    """
    sock = None
    while True:
        try:
            if _share_external_frame():
                _state["err"] = ""
                time.sleep(1.0)
                continue
            if sock is None:
                path = find_monitor_socket()
                if not path:
                    _state["err"] = "未找到 QEMU monitor socket"
                    time.sleep(2)
                    continue
                sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                sock.settimeout(8)
                sock.connect(path)
                time.sleep(0.2)
                try:
                    sock.recv(65536)          # 吃掉欢迎横幅
                except Exception:
                    pass
                _state["err"] = ""

            sock.sendall(f"screendump {SCREEN_PPM}\n".encode())
            try:
                sock.settimeout(2)
                sock.recv(65536)
            except Exception:
                pass

            # QEMU 是异步写文件的：recv 返回时 PPM 往往还没落完盘，
            # 直接转 PNG 会得到 "image file is truncated"，
            # 于是每帧都要重试，画面就一顿一顿。
            # 这里等文件大小连续两次相同（即写完了）再转。
            if _ppm_ready(SCREEN_PPM):
                try:
                    from PIL import Image
                    tmp = SCREEN_PNG + ".tmp"
                    Image.open(SCREEN_PPM).save(tmp, format="PNG")
                    os.replace(tmp, SCREEN_PNG)       # 原子替换，浏览器不会读到半个文件
                    _state["last_grab"] = time.time()
                    _state["err"] = ""
                except Exception as e:
                    _state["err"] = f"PPM->PNG: {e}"
            time.sleep(GRAB_INTERVAL)
        except Exception as e:
            _state["err"] = f"抓帧: {e}"
            try:
                if sock:
                    sock.close()
            except Exception:
                pass
            sock = None
            time.sleep(1.5)


PAGE = """<!doctype html>
<html lang="zh" data-theme="dark"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>HBOS · KDE Plasma 6</title>
<style>
  /* ============ 主题变量：深色（默认，保留科技感） ============ */
  :root{
    --bg-a:#16233a; --bg-b:#1a1533; --bg:#080b10;
    --panel:#0f141d; --panel-2:#0b1017; --line:#1d2635;
    --txt:#c8d3e0; --dim:#5d6b80; --acc:#38e1ff; --acc2:#7c5cff;
    --ok:#2ee6a8; --warn:#ffb020; --err:#ff4d6d;
    --log-bg:#070a0f; --log-line:#1a2331;
    --head-bg:linear-gradient(180deg,#141b27,#0e131c);
    --pill-bg:#131a26;
    --pill-on-bg:#0d1f1a; --pill-on-line:#1d4d3d;
    --pill-off-bg:#1f0d12; --pill-off-line:#4d1d28;
    --pill-warn-bg:#241a08; --pill-warn-line:#5c4413;
    --screen-bg:#05070a; --screen-line:#1d2635;
    --screen-shade:radial-gradient(120% 100% at 50% 50%, transparent 42%, #000000b8 100%);
    --screen-ring:#00000070;
    --shadow-sm:0 1px 0 #ffffff08 inset;
    --shadow-card:0 8px 24px -12px #000 inset, 0 1px 0 #ffffff08;
    --ov-mask:radial-gradient(circle at 50% 45%, #0a0f16ee, #05070af5 70%);
    --ov-title:#e6eef8; --ov-sub:#8b9bb1;
    --banner-bg:linear-gradient(90deg,#2a1118,#1c0c11);
    --banner-line:#5b2030; --banner-txt:#ffd7de;
    --k:#4dd6a0; --e:#ff6b81; --w:#e3b341; --d:#414d5e; --ts:#3d5a7a;
    --scroll:#1b2635; --scroll-h:#28384d;
    --btn-bg:#131a26; --btn-line:#243044; --btn-txt:#c8d3e0;
    --glow:16px;
  }
  /* ============ 浅色（冷灰底 + 层次靠阴影） ============ */
  html[data-theme="light"]{
    --bg-a:#f6f7f9; --bg-b:#eef1f6; --bg:#f6f7f9;
    --panel:#ffffff; --panel-2:#ffffff; --line:#e3e8ef;
    --txt:#1a2230; --dim:#64748b; --acc:#2563eb; --acc2:#0891b2;
    --ok:#0f9d6e; --warn:#b45309; --err:#dc2626;
    --log-bg:#f8fafc; --log-line:#e8edf4;
    --head-bg:#ffffff;
    --pill-bg:#f5f7fa;
    --pill-on-bg:#ecfdf5; --pill-on-line:#a7f3d0;
    --pill-off-bg:#fef2f2; --pill-off-line:#fecaca;
    --pill-warn-bg:#fffbeb; --pill-warn-line:#fde68a;
    --screen-bg:#0b1017; --screen-line:#0b1220;
    --screen-shade:radial-gradient(120% 100% at 50% 50%, transparent 44%, #000000cc 100%);
    --screen-ring:#0b122033;
    --shadow-sm:0 1px 2px rgba(16,24,40,.04);
    --shadow-card:0 1px 2px rgba(16,24,40,.04), 0 10px 28px -16px rgba(16,24,40,.28);
    --ov-mask:radial-gradient(circle at 50% 45%, #0b1220ee, #05070af8 70%);
    --ov-title:#eef4fb; --ov-sub:#9aa8bb;
    --banner-bg:linear-gradient(90deg,#fff1f2,#fef6f6);
    --banner-line:#fecdd3; --banner-txt:#9f1239;
    --k:#0f766e; --e:#b91c1c; --w:#a16207; --d:#94a3b8; --ts:#64748b;
    --scroll:#cbd5e1; --scroll-h:#94a3b8;
    --btn-bg:#ffffff; --btn-line:#e3e8ef; --btn-txt:#1a2230;
    --glow:8px;
  }

  *{box-sizing:border-box}
  html,body{margin:0;height:100%;overflow:hidden}
  body{
    background:
      radial-gradient(1200px 600px at 15% -10%, var(--bg-a) 0%, transparent 60%),
      radial-gradient(900px 500px at 100% 110%, var(--bg-b) 0%, transparent 55%),
      var(--bg);
    color:var(--txt);
    font:13px/1.5 -apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,"Helvetica Neue","Noto Sans SC",Arial,sans-serif;
    transition:background .25s ease,color .25s ease;
  }
  html[data-theme="light"] body{
    background:linear-gradient(180deg,#f6f7f9 0%,#eef1f6 100%);
  }
  .mono,#log,#clock,.stat b,#res,#fps,.ov-detail{
    font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,"Liberation Mono",monospace;
    font-variant-numeric:tabular-nums;font-feature-settings:"tnum" 1;
  }

  #app{display:grid;height:100vh;padding:12px;gap:12px;
    grid-template-columns:minmax(0,1.5fr) minmax(0,1fr);
    grid-template-rows:54px auto minmax(0,1fr);
    grid-template-areas:"head head" "banner banner" "screen logs"}

  /* ---------------- 顶栏 ---------------- */
  header{grid-area:head;display:flex;align-items:center;gap:14px;
    background:var(--head-bg);border:1px solid var(--line);border-radius:14px;
    padding:0 16px;box-shadow:var(--shadow-card);min-width:0}
  .brand{font-weight:750;letter-spacing:.13em;font-size:12.5px;white-space:nowrap;
    background:linear-gradient(90deg,var(--acc),var(--acc2));
    -webkit-background-clip:text;background-clip:text;color:transparent}
  .pill{display:inline-flex;align-items:center;gap:7px;padding:5px 11px;border-radius:20px;
    background:var(--pill-bg);border:1px solid var(--line);font-size:11.5px;
    letter-spacing:.02em;color:var(--dim);white-space:nowrap;
    transition:color .2s,border-color .2s,background .2s}
  .dot{width:7px;height:7px;border-radius:50%;background:currentColor;
    box-shadow:0 0 6px currentColor,0 0 calc(var(--glow) * 1.2) currentColor;
    animation:breathe 2.6s ease-in-out infinite}
  @keyframes breathe{
    0%,100%{opacity:.62;box-shadow:0 0 5px currentColor}
    50%{opacity:1;box-shadow:0 0 10px currentColor,0 0 calc(var(--glow) * 1.6) currentColor}
  }
  .pill.on{color:var(--ok);background:var(--pill-on-bg);border-color:var(--pill-on-line)}
  .pill.off{color:var(--err);background:var(--pill-off-bg);border-color:var(--pill-off-line)}
  .pill.warn{color:var(--warn);background:var(--pill-warn-bg);border-color:var(--pill-warn-line)}
  .pill.off .dot,.pill.warn .dot{animation-duration:1.1s}

  .right{margin-left:auto;display:flex;align-items:center;gap:16px;min-width:0}
  .stat{color:var(--dim);font-size:11.5px;white-space:nowrap;display:inline-flex;gap:6px;align-items:baseline}
  .stat b{color:var(--txt);font-weight:600;font-size:12px}
  #themeBtn{display:inline-flex;align-items:center;gap:6px;cursor:pointer;
    padding:6px 11px;border-radius:9px;
    font:inherit;font-size:11.5px;
    color:var(--btn-txt);background:var(--btn-bg);border:1px solid var(--btn-line);
    box-shadow:var(--shadow-sm);transition:border-color .2s,transform .12s}
  #themeBtn:hover{border-color:var(--acc)}
  #themeBtn:active{transform:translateY(1px)}
  #themeBtn svg{width:13px;height:13px;display:block}

  /* ---------------- 断连横幅 ---------------- */
  #banner{grid-area:banner;display:none;align-items:center;gap:11px;
    padding:9px 16px;border-radius:12px;font-size:12.5px;
    background:var(--banner-bg);border:1px solid var(--banner-line);
    color:var(--banner-txt);box-shadow:var(--shadow-card)}
  #banner.show{display:flex;animation:slidein .18s ease-out}
  @keyframes slidein{from{opacity:0;transform:translateY(-4px)}to{opacity:1;transform:none}}
  #banner svg{width:15px;height:15px;flex:none}
  #banner .btxt{font-weight:600}
  #banner .bsec{margin-left:auto;font-family:ui-monospace,Menlo,Consolas,monospace;
    font-variant-numeric:tabular-nums;font-size:12px;opacity:.85;white-space:nowrap}

  /* ---------------- 画面 ---------------- */
  #screen{grid-area:screen;position:relative;min-width:0;min-height:0;
    background:var(--screen-bg);border:1px solid var(--screen-line);
    border-radius:14px;overflow:hidden;display:flex;align-items:center;justify-content:center;
    box-shadow:0 0 0 1px var(--screen-ring), var(--shadow-card),
      0 26px 64px -34px #000}
  #img{width:100%;height:100%;object-fit:contain;display:none;
    image-rendering:-webkit-optimize-contrast}
  /* 屏幕感：内阴影暗角 + 极淡扫描线 */
  #screen::after{content:'';position:absolute;inset:0;pointer-events:none;
    background:var(--screen-shade);
    box-shadow:inset 0 0 90px -24px #000000d0, inset 0 0 0 1px #ffffff0a}
  #screen::before{content:'';position:absolute;inset:0;pointer-events:none;z-index:1;
    background:repeating-linear-gradient(0deg,#00000000 0 2px,#00000014 2px 4px);
    opacity:.45}
  .badge{position:absolute;top:10px;left:10px;z-index:3;padding:3px 9px;border-radius:7px;
    background:#000000a8;border:1px solid #ffffff1f;font-size:10.5px;letter-spacing:.14em;
    color:var(--acc);backdrop-filter:blur(6px)}
  .badge.warn{color:var(--warn)}
  .badge.off{color:var(--err)}
  .res{position:absolute;top:10px;right:10px;z-index:3;padding:3px 9px;border-radius:7px;
    background:#000000a8;border:1px solid #ffffff1f;font-size:10.5px;color:#cbd5e1;
    backdrop-filter:blur(6px);letter-spacing:.03em}

  /* 断连遮罩 */
  #overlay{position:absolute;inset:0;z-index:2;display:flex;flex-direction:column;
    align-items:center;justify-content:center;gap:10px;padding:24px;
    background:var(--ov-mask);backdrop-filter:blur(3px);
    transition:opacity .25s ease}
  #overlay.hidden{display:none}
  #overlay .ov-icon{position:relative;width:46px;height:46px;display:grid;place-items:center;
    color:var(--ov-sub);margin-bottom:2px}
  #overlay .ov-icon svg{width:34px;height:34px;display:none}
  #overlay[data-mode="lost"] .ov-icon{color:#38e1ff;
    animation:pulse 1.5s ease-in-out infinite}
  @keyframes pulse{0%,100%{opacity:.82}50%{opacity:1}}
  #overlay[data-mode="offline"] .ov-icon{color:var(--warn)}
  #overlay[data-mode="waiting"] .ov-icon{color:#7c8aa0}
  #overlay[data-mode="lost"] .ic-lost,
  #overlay[data-mode="offline"] .ic-off,
  #overlay[data-mode="waiting"] .ic-wait{display:block}
  #overlay .ov-icon::before{content:'';position:absolute;inset:-4px;border-radius:50%;
    border:1px solid currentColor;opacity:0;animation:halo 1.9s ease-out infinite}
  #overlay[data-mode="offline"] .ov-icon::before{animation-duration:3.4s}
  @keyframes halo{0%{transform:scale(.72);opacity:.55}70%{transform:scale(1.35);opacity:0}100%{opacity:0}}
  .ov-title{font-size:14.5px;font-weight:650;color:var(--ov-title);letter-spacing:.04em}
  .ov-sub{font-size:12px;color:var(--ov-sub)}
  .ov-detail{max-width:min(560px,90%);text-align:center;font-size:11px;line-height:1.6;
    color:#8496ad;background:#ffffff0d;border:1px solid #ffffff14;border-radius:8px;
    padding:6px 10px;word-break:break-word}
  .ov-detail:empty{display:none}

  /* ---------------- 日志 ---------------- */
  #logs{grid-area:logs;min-width:0;min-height:0;background:var(--log-bg);
    border:1px solid var(--line);border-radius:14px;display:flex;flex-direction:column;
    overflow:hidden;box-shadow:var(--shadow-card)}
  .lhead{display:flex;align-items:center;gap:9px;padding:9px 14px;
    border-bottom:1px solid var(--line);background:var(--panel-2);
    font-size:11.5px;color:var(--dim);flex:none;min-width:0}
  .lhead .t{color:var(--txt);font-weight:650;letter-spacing:.09em;font-size:11px}
  .lhead #src{color:var(--dim);font-family:ui-monospace,Menlo,Consolas,monospace;
    font-size:10.5px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;min-width:0}
  .chip{padding:1px 7px;border-radius:20px;font-size:10px;white-space:nowrap;
    background:var(--pill-bg);border:1px solid var(--line);color:var(--dim)}
  .chip.hide{display:none}
  #fps{color:var(--acc);font-size:11px;margin-left:auto;white-space:nowrap}
  #logbox{flex:1;overflow:auto;padding:10px 14px;min-height:0;overscroll-behavior:contain}
  #log{white-space:pre-wrap;word-break:break-word;font-size:11.5px;line-height:1.58}
  .k{color:var(--k)}.e{color:var(--e)}.w{color:var(--w)}.d{color:var(--d)}
  .ts{color:var(--ts)}
  #logbox::-webkit-scrollbar{width:9px;height:9px}
  #logbox::-webkit-scrollbar-thumb{background:var(--scroll);border-radius:5px}
  #logbox::-webkit-scrollbar-thumb:hover{background:var(--scroll-h)}
  #logbox::-webkit-scrollbar-track{background:transparent}

  @media (max-width:1000px){
    #app{grid-template-columns:minmax(0,1fr);
      grid-template-rows:54px auto minmax(180px,1.25fr) minmax(140px,.9fr);
      grid-template-areas:"head" "banner" "screen" "logs"}
    .brand{display:none}
    .right{gap:10px}
  }
</style>
<script>
(function(){
  var KEY='hbos.monitor.theme';
  var t='dark';
  try{ t = localStorage.getItem(KEY) || (window.matchMedia && window.matchMedia('(prefers-color-scheme: light)').matches ? 'light' : 'dark'); }catch(e){}
  document.documentElement.setAttribute('data-theme', t);
})();
</script>
</head>
<body><div id="app">
  <header>
    <span class="brand">HBOS × KDE PLASMA 6</span>
    <span class="pill" id="qemu"><span class="dot"></span><span id="qemu-txt">QEMU</span></span>
    <span class="pill" id="feed"><span class="dot"></span><span id="feed-txt">画面</span></span>
    <div class="right">
      <span class="stat">镜像 <b id="imgstat">—</b></span>
      <span class="stat">日志 <b id="logsz">—</b></span>
      <span class="stat" id="clock">—</span>
      <button id="themeBtn" type="button" title="切换浅色 / 深色">
        <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.7"
             stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">
          <circle cx="12" cy="12" r="4"></circle>
          <path d="M12 2v2M12 20v2M2 12h2M20 12h2M4.9 4.9l1.4 1.4M17.7 17.7l1.4 1.4M19.1 4.9l-1.4 1.4M6.3 17.7l-1.4 1.4"></path>
        </svg>
        <span id="themeTxt">浅色</span>
      </button>
    </div>
  </header>

  <div id="banner" role="alert">
    <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8"
         stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">
      <path d="M10.3 3.9 1.9 18a2 2 0 0 0 1.7 3h16.8a2 2 0 0 0 1.7-3L13.7 3.9a2 2 0 0 0-3.4 0z"></path>
      <path d="M12 9v4M12 17h.01"></path>
    </svg>
    <span class="btxt">与监控服务的连接已断开，正在重试…</span>
    <span class="bsec" id="bsec">已断开 0s</span>
  </div>

  <div id="screen">
    <img id="img" alt="KDE 画面">
    <div id="overlay" data-mode="waiting">
      <div class="ov-icon">
        <svg class="ic-lost" viewBox="0 0 24 24" fill="none" stroke="currentColor"
             stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round">
          <rect x="2.5" y="4" width="19" height="13" rx="2"></rect>
          <path d="M8.5 21h7M12 17v4M3 3l18 18"></path>
        </svg>
        <svg class="ic-off" viewBox="0 0 24 24" fill="none" stroke="currentColor"
             stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round">
          <path d="M12 3v8"></path>
          <path d="M18.4 6.6a9 9 0 1 1-12.8 0"></path>
        </svg>
        <svg class="ic-wait" viewBox="0 0 24 24" fill="none" stroke="currentColor"
             stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round">
          <circle cx="12" cy="12" r="9"></circle>
          <path d="M12 7v5l3 2"></path>
        </svg>
      </div>
      <div class="ov-title" id="ov-title">等待画面</div>
      <div class="ov-sub" id="ov-sub">正在获取首帧…</div>
      <div class="ov-detail" id="ov-detail"></div>
    </div>
    <div class="badge" id="badge">LIVE</div>
    <div class="res" id="res">—</div>
  </div>

  <div id="logs">
    <div class="lhead">
      <span class="t">SERIAL</span>
      <span id="src" title="">—</span>
      <span class="chip hide" id="noise"></span>
      <span id="fps">—</span>
    </div>
    <div id="logbox"><div id="log">连接中…</div></div>
  </div>
</div>
<script>
(function(){
  'use strict';
  var $ = function(id){ return document.getElementById(id); };

  /* ---------------- 主题切换 ---------------- */
  var TKEY = 'hbos.monitor.theme';
  function paintTheme(t){
    document.documentElement.setAttribute('data-theme', t);
    $('themeTxt').textContent = (t === 'light' ? '深色' : '浅色');
    $('themeBtn').title = (t === 'light' ? '切换到深色模式' : '切换到浅色模式');
  }
  paintTheme(document.documentElement.getAttribute('data-theme') || 'dark');
  $('themeBtn').addEventListener('click', function(){
    var next = document.documentElement.getAttribute('data-theme') === 'light' ? 'dark' : 'light';
    paintTheme(next);
    try { localStorage.setItem(TKEY, next); } catch(e) {}
  });

  /* ---------------- 画面：高频轮询 ---------------- */
  var FAIL_LIMIT = 3;          // 连续 N 次读图失败才算断
  var img = $('img');
  var scr = { fail:0, ok:false, qemu:false, hasPng:false, err:'', errHits:0, mode:'' };

  img.addEventListener('load', function(){
    scr.fail = 0; scr.ok = true;
    img.style.display = 'block';
    $('res').textContent = img.naturalWidth + '×' + img.naturalHeight;
    frames++;
    var dt = (performance.now() - fpsT0) / 1000;
    if (dt >= 1.5){ $('fps').textContent = (frames / dt).toFixed(1) + ' fps'; frames = 0; fpsT0 = performance.now(); }
    renderScreen();
  });
  img.addEventListener('error', function(){
    scr.fail++;
    renderScreen();
  });

  function applyOverlay(mode, title, sub, detail){
    if (scr.mode !== mode){
      scr.mode = mode;
      $('overlay').setAttribute('data-mode', mode);
      $('overlay').classList.toggle('hidden', mode === 'live');
    }
    $('ov-title').textContent = title;
    $('ov-sub').textContent = sub;
    $('ov-detail').textContent = detail || '';

    var pillCls = mode === 'live' ? 'on' : (mode === 'offline' ? 'off' : 'warn');
    $('feed').className = 'pill ' + pillCls;
    $('feed-txt').textContent = mode === 'live' ? '画面正常'
      : (mode === 'offline' ? '虚拟机停止' : (mode === 'waiting' ? '等待画面' : '画面中断'));
    $('badge').className = 'badge ' + (mode === 'live' ? '' : pillCls);
    $('badge').textContent = mode === 'live' ? 'LIVE'
      : (mode === 'offline' ? 'STOPPED' : (mode === 'waiting' ? 'WAIT' : 'RECONNECT'));
  }

  function renderScreen(){
    if (!scr.qemu){
      applyOverlay('offline', '虚拟机已停止', '等待 QEMU 启动，画面恢复后自动继续',
                   scr.err || '');
    } else if (scr.errHits >= 2 && scr.err){
      applyOverlay('lost', '画面中断', '正在重连…', scr.err);
    } else if (scr.fail >= FAIL_LIMIT){
      applyOverlay('lost', '画面中断', '正在重连…',
                   '连续 ' + scr.fail + ' 次读取 /screen.png 失败');
    } else if (!scr.hasPng && !scr.ok){
      applyOverlay('waiting', '等待画面', '已连接监控服务，正在获取首帧…', '');
    } else {
      applyOverlay('live', '画面正常', '', '');
    }
    var okBadge = scr.mode === 'live';
    if (okBadge) img.style.display = 'block';
  }

  function pumpScreen(){
    if (!document.hidden) img.src = '/screen.png?t=' + Date.now();
    setTimeout(pumpScreen, 160);
  }

  /* ---------------- 状态 + 日志：1s ---------------- */
  var lastLog = '', frames = 0, fpsT0 = performance.now();
  var stick = true;
  var logbox = $('logbox');
  logbox.addEventListener('scroll', function(){
    stick = (logbox.scrollHeight - logbox.scrollTop - logbox.clientHeight) < 64;
  });

  var api = { down:false, since:0 };

  function setApiDown(down){
    if (down && !api.down) api.since = Date.now();
    api.down = down;
    var b = $('banner');
    b.classList.toggle('show', down);
    if (!down) $('bsec').textContent = '已断开 0s';
  }

  function tickSeconds(){
    if (api.down){
      var s = Math.max(0, Math.round((Date.now() - api.since) / 1000));
      $('bsec').textContent = '已断开 ' + s + 's';
    }
    var d = new Date();
    $('clock').textContent = d.toLocaleTimeString('zh-CN', {hour12:false});
  }

  async function pumpState(){
    var ctrl = (typeof AbortController !== 'undefined') ? new AbortController() : null;
    var timer = ctrl ? setTimeout(function(){ ctrl.abort(); }, 4000) : 0;
    try {
      var res = await fetch('/api/state', {cache:'no-store', signal: ctrl ? ctrl.signal : undefined});
      if (!res.ok) throw new Error('HTTP ' + res.status);
      var j = await res.json();
      clearTimeout(timer);
      setApiDown(false);

      scr.qemu = !!j.qemu;
      scr.hasPng = !!j.has_png;
      scr.err = j.err || '';
      scr.errHits = scr.err ? scr.errHits + 1 : 0;

      $('qemu').className = 'pill ' + (j.qemu ? 'on' : 'off');
      $('qemu-txt').textContent = j.qemu ? 'QEMU 运行中' : 'QEMU 已停止';
      $('imgstat').textContent = j.img_mtime || '—';
      $('imgstat').title = j.img_mtime || '';
      $('logsz').textContent = (j.log_size || 0).toLocaleString('zh-CN') + ' B';

      var lp = j.log_path || '';
      var tail = lp ? lp.split('/').slice(-3).join('/') : '无日志';
      $('src').textContent = tail;
      $('src').title = lp || '未找到 serial 日志';

      var n = j.log_noise || 0;
      $('noise').classList.toggle('hide', !n);
      if (n) $('noise').textContent = '已折叠 ' + n + ' 条噪音';

      if (j.log && j.log !== lastLog){
        lastLog = j.log;
        $('log').innerHTML = j.log;
        if (stick) logbox.scrollTop = logbox.scrollHeight;
      }
      renderScreen();
    } catch(e){
      clearTimeout(timer);
      setApiDown(true);
      scr.err = '';
      scr.errHits = 0;
      renderScreen();
    }
    setTimeout(pumpState, 1000);
  }

  setInterval(tickSeconds, 500);
  tickSeconds();
  pumpScreen();
  pumpState();
})();
</script></body></html>
"""
def colorize(line: str) -> str:
    e = html.escape(line)
    low = line.lower()
    if "open-fail" in low:
        return f'<span class="d">{e}</span>'
    if "[exit]" in low and "code=0" not in low:
        return f'<span class="e">{e}</span>'
    if "[exit]" in low:
        return f'<span class="w">{e}</span>'
    if "[exec]" in low or "hbos_wl_fb" in low:
        return f'<span class="k">{e}</span>'
    return e


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def do_GET(self):
        if self.path.startswith("/api/state"):
            import json
            lp = newest_serial()
            _state["log_path"] = lp
            lines = []
            if lp and os.path.exists(lp):
                try:
                    with open(lp, "r", errors="replace") as f:
                        lines = f.readlines()[-LOG_TAIL:]
                except Exception as e:
                    lines = [f"读日志失败: {e}"]
            body = {
                "qemu": any("qemu-system" in c for c in _procs()),
                "err": _state["err"],
                "has_png": os.path.exists(SCREEN_PNG),
                "png_mtime": int(os.path.getmtime(SCREEN_PNG)) if os.path.exists(SCREEN_PNG) else 0,
                "img_mtime": time.strftime(
                    "%H:%M:%S",
                    time.localtime(os.path.getmtime(os.path.join(ROOT, "build", "hbos-kde-chromium.img")))
                ) if os.path.exists(os.path.join(ROOT, "build", "hbos-kde-chromium.img")) else "",
                "log": "".join(colorize(l.rstrip("\n")) + "\n" for l in lines),
                "log_path": lp or "",
                "log_size": os.path.getsize(lp) if lp and os.path.exists(lp) else 0,
                # 供前端提示「已折叠 N 条噪音」；口径与 colorize() 里 OPEN-FAIL 一致
                "log_noise": sum(1 for l in lines if "open-fail" in l.lower()),
            }
            data = json.dumps(body).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)
            return

        if self.path.startswith("/screen.png"):
            if os.path.exists(SCREEN_PNG):
                data = open(SCREEN_PNG, "rb").read()
                self.send_response(200)
                self.send_header("Content-Type", "image/png")
                self.send_header("Cache-Control", "no-store")
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)
            else:
                self.send_error(404)
            return

        body = PAGE.encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


def _procs():
    out = []
    for d in glob.glob("/proc/[0-9]*/cmdline"):
        try:
            out.append(open(d, "rb").read().decode(errors="replace").replace("\x00", " "))
        except Exception:
            pass
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8770)
    args = ap.parse_args()
    os.makedirs(OUT, exist_ok=True)
    threading.Thread(target=grab_loop, daemon=True).start()
    srv = ThreadingHTTPServer(("0.0.0.0", args.port), Handler)
    print(f"[kde_monitor] http://127.0.0.1:{args.port}/   (Ctrl-C 退出)")
    print(f"[kde_monitor] 抓帧目录 {OUT}")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
