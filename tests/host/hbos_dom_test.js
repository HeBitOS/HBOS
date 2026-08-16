/* Node-side harness for hbos_dom.js — mimics what hbos_js.c does:
 * register globals → eval hbos_dom.js → parsePage → run scripts → flush → render */
const fs = require('fs');

// --- mimic C-side globals ---
globalThis.window = globalThis;   // C: JS_SetPropertyStr(global, "window", global)
globalThis.print = (...a) => console.log(...a);
globalThis.console = console;
globalThis.__hbosFetchRaw = function (url) { if (url.indexOf("/x/player/playurl") >= 0) return "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n" + JSON.stringify({code:0,data:{format:"mp4720",timelength:213000,durl:[{url:"https://cdn.example/video.mp4",size:123456}]}}); throw new Error("unexpected URL"); };
globalThis.location = {
    href: 'https://www.bilibili.com/video/BV1xx411c7mD',
    hostname: 'www.bilibili.com', host: 'www.bilibili.com', protocol: 'https:',
    pathname: '/video/BV1xx411c7mD', search: '', hash: '', origin: 'https://www.bilibili.com'
};

const domSrc = fs.readFileSync(__dirname + '/../../third_party/quickjs/hbos_dom.js', 'utf8');
eval(domSrc);

const html = `<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<title>【测试】我的世界视频 - bilibili</title>
<style>.video-title{color:#00a1d6}</style>
</head>
<body>
<div id="app" class="page">
  <div class="player">
    <video src="https://upos-sz-mirrorcos.bilivideo.com/v.mp4" poster="https://i0.hdslb.com/bfs/archive/pic.jpg">
    </video>
    <span class="duration">10:30</span>
  </div>
  <h1 class="video-title">我的世界：从零开始生存</h1>
  <p class="desc">第一集&amp;第二集，希望大家喜欢！<br>喜欢的话请<strong>三连</strong>支持~</p>
  <a href="https://space.bilibili.com/12345" class="up">UP主：测试君</a>
  <ul class="tags">
    <li>生存</li><li>我的世界</li><li>实况</li>
  </ul>
  <div id="comment">评论区</div>
  <script>document.title = '【测试】改过的标题'; document.getElementById('comment').innerHTML = '<p>第一条评论：好看！</p>';</script>
</div>
</body>
</html>`;

// --- C side: JS_Eval(hbos_dom_js) then HBOS.parsePage(g_page_html) ---
HBOS.parsePage(html);

// script eval like hbos_js.c multi-file mode
const inline = html.match(/<script>([\s\S]*?)<\/script>/g);
if (inline) {
    for (const s of inline) {
        const code = s.slice(8, -9);
        try { eval(code); } catch (e) { console.log('SCRIPT ERR:', e.message); }
    }
}

// --- assertions ---
let fails = 0;
function ok(cond, name) {
    if (cond) console.log('  ok  ' + name);
    else { console.log('  FAIL ' + name); fails++; }
}

console.log('== parse ==');
ok(document.title === '【测试】改过的标题', 'title set by script');
const h1 = document.querySelector('h1.video-title');
ok(h1 && h1.textContent === '我的世界：从零开始生存', 'querySelector h1.video-title');
ok(document.querySelector('#app') !== null, 'querySelector #app');
ok(document.querySelectorAll('li').length === 3, 'querySelectorAll li == 3');
const up = document.querySelector('a.up');
ok(up && up.getAttribute('href') === 'https://space.bilibili.com/12345', 'a href attr');
ok(h1.className === 'video-title', 'className');
ok(h1.classList.contains('video-title'), 'classList.contains');
h1.classList.add('x');
ok(h1.className === 'video-title x', 'classList.add syncs className');
h1.classList.remove('x');
const video = document.querySelector('video');
ok(video && video.tagName === 'video', 'video element');
ok(video.getAttribute('src').indexOf('upos') >= 0, 'video src');
const cmt = document.getElementById('comment');
ok(cmt && cmt.childNodes.length === 1 && cmt.childNodes[0].nodeType === 1 &&
   cmt.childNodes[0].textContent === '第一条评论：好看！', 'innerHTML setter + textContent');
ok(document.getElementsByTagName('p').length >= 2, 'getElementsByTagName p');
ok(document.body.querySelector('.desc').textContent.indexOf('三连') > 0, 'desc text (strong inline)');
ok(document.title !== '', 'title non-empty');
ok(document.readyState === 'loading', 'readyState loading before flush');

console.log('== events ==');
let dcFired = false, loadFired = false;
document.addEventListener('DOMContentLoaded', () => { dcFired = true; });
window.addEventListener('load', () => { loadFired = true; });
HBOS.flushDeferred();
ok(dcFired && loadFired, 'DOMContentLoaded + load fired');
ok(document.readyState === 'complete', 'readyState complete after flush');

console.log('== render ==');
const rendered = HBOS.renderPage();
console.log('--- rendered ---');
console.log(rendered);
console.log('--- end ---');
ok(rendered.indexOf('<h1 class="video-title">') >= 0, 'render keeps h1 class');
ok(rendered.indexOf('<img') >= 0 && rendered.indexOf('▶') >= 0, 'video → img placeholder');
ok(rendered.indexOf('<a href="https://space.bilibili.com/12345">') >= 0, 'render link href');
ok(rendered.indexOf('<li>生存</li>') >= 0, 'render li');
ok(rendered.indexOf('&amp;') >= 0 || rendered.indexOf('三连') >= 0, 'entity round trip');
ok(rendered.indexOf('第一条评论') >= 0, 'script-mutated DOM rendered');

console.log('== mutation ==');
const app = document.getElementById('app');
const li = document.createElement('li');
li.textContent = '新评论';
cmt.appendChild(li);
ok(cmt.childNodes.length === 2, 'appendChild');
ok(li.parentNode === cmt, 'parentNode');
const r2 = HBOS.renderPage();
ok(r2.indexOf('新评论') >= 0, 'render sees appended node');

console.log(fails ? `\n${fails} FAILURES` : '\nALL PASS');
process.exit(fails ? 1 : 0);
