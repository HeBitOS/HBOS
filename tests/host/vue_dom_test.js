/* Node-side Vue integration test: load the vendored Vue 2 runtime against
 * our DOM and mount a component, then render the mutated tree. */
const fs = require('fs');

globalThis.window = globalThis;
globalThis.print = (...a) => console.log(...a);
globalThis.console = console;
globalThis.location = {
    href: 'https://example.com/page', hostname: 'example.com', host: 'example.com',
    protocol: 'https:', pathname: '/page', search: '', hash: '', origin: 'https://example.com'
};

const domSrc = fs.readFileSync(__dirname + '/../../third_party/quickjs/hbos_dom.js', 'utf8');
eval(domSrc);

const html = `<!DOCTYPE html>
<html><head><title>Vue 页面</title></head><body>
<div id="app">
  <p class="static-text">静态内容</p>
</div>
<script>
new Vue({
  el: '#app',
  data() { return { msg: 'Hello from Vue!', items: ['甲', '乙', '丙'] }; },
  template: '<div class="vue-root"><h2>{{ msg }}</h2><ul><li v-for="it in items">{{ it }}</li></ul><p v-if="msg">条件渲染 OK</p></div>'
});
</script>
</body></html>`;

HBOS.parsePage(html);

// extract inline scripts (mirror kernel pipeline)
const inline = html.match(/<script>([\s\S]*?)<\/script>/g);
// first eval vendored Vue (like the kernel argv ordering: vue.global.prod.js first)
/* node 的 -e/模块全局里有 module/exports，Vue 的 UMD 会走 CommonJS 分支；
 * 删掉让它在无模块的浏览器全局形态下挂到 globalThis（quickjs 天然如此） */
delete globalThis.module; delete globalThis.exports;
globalThis.eval(fs.readFileSync(__dirname + '/../../third_party/web-vendor/vue.global.prod.js', 'utf8'));
for (const s of inline) {
    const code = s.slice(8, -9);
    try { eval(code); } catch (e) { console.log('SCRIPT ERR:', e.message); }
}

let fails = 0;
function ok(cond, name) {
    if (cond) console.log('  ok  ' + name);
    else { console.log('  FAIL ' + name); fails++; }
}

console.log('== vue mount ==');
ok(typeof Vue !== 'undefined', 'Vue global defined');
const root = document.querySelector('.vue-root');
ok(root !== null, 'vue-root mounted (Vue 2 replaces el with template root)');
ok(root && root.childNodes.length > 0, 'vue-root has children after mount');
const h2 = document.querySelector('.vue-root h2');
ok(h2 && h2.textContent === 'Hello from Vue!', 'vue template text rendered: ' + (h2 && h2.textContent));
ok(document.querySelectorAll('.vue-root li').length === 3, 'v-for rendered 3 li');
ok(document.querySelector('.vue-root p').textContent.indexOf('条件渲染') >= 0, 'v-if rendered');
ok(document.querySelector('.static-text') === null, 'static content replaced by mount');
ok(document.querySelector('#app') === null, '#app replaced by template root');

console.log('== render output ==');
HBOS.flushDeferred();
const rendered = HBOS.renderPage();
console.log('--- rendered ---');
console.log(rendered);
console.log('--- end ---');
ok(rendered.indexOf('Hello from Vue!') >= 0, 'render contains vue text');
ok(rendered.indexOf('<li>甲</li>') >= 0, 'render contains v-for items');
ok(rendered.indexOf('条件渲染') >= 0, 'render contains v-if text');

console.log(fails ? `\n${fails} FAILURES` : '\nALL PASS');
process.exit(fails ? 1 : 0);
