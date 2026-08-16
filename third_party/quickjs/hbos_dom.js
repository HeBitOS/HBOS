/*
 * hbos_dom.js — HBOS 浏览器 DOM 平台（ring3 quickjs 内）。
 *
 * 浏览器管线在 js.hax 里跑：内核把页面 HTML 写到 /system/page.html，
 * `js -p /system/page.html` 启动后：
 *   1. 本文件定义完整（够用的）DOM：HTML 解析器、Node/Element/Document
 *      树、CSS 选择器子集、事件系统、定时器队列。
 *   2. 解析页面 HTML 生成 DOM 树（document.body 等）。
 *   3. 页面脚本（含 Vue 运行时）逐个 eval，操作这棵真实 DOM。
 *   4. HBOS.flushDeferred() 触发 DOMContentLoaded/load 与零延时回调。
 *   5. HBOS.renderPage() 把最终 DOM 序列化成"迷你 HTML"（div/p/h1/a/li/
 *      img…），内核浏览器拿到后用自己的渲染器重新解析，得到完整样式。
 *
 * 兼容性约束：quickjs 是 ES2020 完整实现，但避免 ?. / ?? 等新语法，
 * 全部用 var + 原型方法（老页面脚本同样兼容）。
 */

var HBOS = {};

/* ── 基础工具 ── */
function hb_low(s) { return typeof s === 'string' ? s.toLowerCase() : ''; }
function hb_trim(s) { return String(s).replace(/^[\t\n\r ]+/, '').replace(/[\t\n\r ]+$/, ''); }
function hb_has(s, sub) { return String(s).indexOf(sub) !== -1; }

var HB_VOID = { area:1, base:1, br:1, col:1, embed:1, hr:1, img:1, input:1,
                link:1, meta:1, param:1, source:1, track:1, wbr:1 };
var HB_RAW = { script:1, style:1, textarea:1, title:1 };
/* 新标签入栈前要关掉的顶层标签（HTML 隐式闭合规则子集） */
var HB_CLOSES = {
    p: ['p'], li: ['li'], dt: ['dt'], dd: ['dd'],
    td: ['td','th'], th: ['td','th'], tr: ['tr','td','th'],
    option: ['option'], optgroup: ['option','optgroup'],
    thead: ['tr','td','th','thead'], tbody: ['tr','td','th','tbody'],
    tfoot: ['tr','td','th','tfoot'],
    h1: ['h1','h2','h3','h4','h5','h6'], h2: ['h1','h2','h3','h4','h5','h6'],
    h3: ['h1','h2','h3','h4','h5','h6'], h4: ['h1','h2','h3','h4','h5','h6'],
    h5: ['h1','h2','h3','h4','h5','h6'], h6: ['h1','h2','h3','h4','h5','h6']
};
var HB_BLOCK = { html:1, body:1, div:1, p:1, section:1, article:1, header:1,
    footer:1, nav:1, aside:1, main:1, h1:1, h2:1, h3:1, h4:1, h5:1, h6:1,
    li:1, blockquote:1, pre:1, figure:1, figcaption:1, ul:1, ol:1, dl:1,
    dd:1, dt:1, table:1, thead:1, tbody:1, tfoot:1, tr:1, td:1, th:1,
    hr:1, form:1, fieldset:1, address:1, center:1, video:1, audio:1 };
var HB_SKIP = { script:1, style:1, noscript:1, template:1, link:1, meta:1,
    iframe:1, object:1, embed:1, canvas:1, svg:1, head:1, title:1 };

var HB_ENT = { amp:'&', lt:'<', gt:'>', quot:'"', apos:"'", nbsp:' ',
    mdash:'-', ndash:'-', hellip:'.', copy:'C', reg:'R', trade:'T',
    times:'x', divide:'/', plusmn:'+', deg:'o', pound:'#', yen:'Y',
    euro:'E', laquo:'<', raquo:'>', bull:'.', middot:'.', lsquo:"'",
    rsquo:"'", ldquo:'"', rdquo:'"' };

function hb_decodeEnt(str) {
    if (str.indexOf('&') < 0) return str;
    return str.replace(/&(#x?[0-9a-fA-F]+|[a-zA-Z][a-zA-Z0-9]*);/g,
                       function (m, e) {
        if (e.charAt(0) === '#') {
            var code;
            if (e.charAt(1) === 'x' || e.charAt(1) === 'X')
                code = parseInt(e.slice(2), 16);
            else
                code = parseInt(e.slice(1), 10);
            if (!isNaN(code) && code >= 32 && code <= 0xffff) {
                try { return String.fromCharCode(code); } catch (err) { return m; }
            }
            return m;
        }
        return HB_ENT[e] !== undefined ? HB_ENT[e] : m;
    });
}

/* 渲染输出转义：内核解析器认得 &amp;/&lt;/&gt;/&quot;，来回一致。 */
function hb_esc(s) {
    s = String(s);
    if (!hb_has(s, '&') && !hb_has(s, '<') && !hb_has(s, '>') && !hb_has(s, '"'))
        return s;
    return s.replace(/&/g, '&amp;').replace(/</g, '&lt;')
            .replace(/>/g, '&gt;').replace(/"/g, '&quot;');
}

/* ── 节点树 ── */
function HBNode(type) {
    this.nodeType = type;      /* 1 元素, 3 文本, 9 文档 */
    this.childNodes = [];
    this.parentNode = null;
    this._doc = null;
}
HBNode.prototype._append = function (c) {
    if (c === null || c === undefined) return c;
    if (c.parentNode) {
        var i = c.parentNode.childNodes.indexOf(c);
        if (i >= 0) c.parentNode.childNodes.splice(i, 1);
    }
    c.parentNode = this;
    this.childNodes.push(c);
    c._doc = this._doc || c._doc;
    return c;
};
HBNode.prototype.appendChild = function (c) { this._append(c); return c; };
HBNode.prototype.removeChild = function (c) {
    var i = this.childNodes.indexOf(c);
    if (i >= 0) {
        this.childNodes.splice(i, 1);
        c.parentNode = null;
    }
    return c;
};
HBNode.prototype.insertBefore = function (c, ref) {
    if (c === null || c === undefined) return c;
    if (ref === null || ref === undefined) return this._append(c);
    var i = this.childNodes.indexOf(ref);
    if (i < 0) return this._append(c);
    if (c.parentNode) {
        var j = c.parentNode.childNodes.indexOf(c);
        if (j >= 0) c.parentNode.childNodes.splice(j, 1);
    }
    c.parentNode = this;
    this.childNodes.splice(i, 0, c);
    c._doc = this._doc;
    return c;
};
HBNode.prototype.replaceChild = function (c, old) {
    var i = this.childNodes.indexOf(old);
    if (i < 0) return this._append(c);
    if (c.parentNode) {
        var j = c.parentNode.childNodes.indexOf(c);
        if (j >= 0) c.parentNode.childNodes.splice(j, 1);
    }
    c.parentNode = this;
    this.childNodes[i] = c;
    c._doc = this._doc;
    old.parentNode = null;
    return old;
};
HBNode.prototype.contains = function (n) {
    for (var p = n; p; p = p.parentNode)
        if (p === this) return true;
    return false;
};
HBNode.prototype.cloneNode = function (deep) {
    var c = this._cloneShallow();
    if (deep) {
        for (var i = 0; i < this.childNodes.length; i++)
            c._append(this.childNodes[i].cloneNode(true));
    }
    return c;
};
HBNode.prototype._cloneShallow = function () {
    var c = new HBNode(this.nodeType);
    c._doc = this._doc;
    return c;
};
HBNode.prototype.remove = function () {
    if (this.parentNode) this.parentNode.removeChild(this);
};
Object.defineProperty(HBNode.prototype, 'firstChild', {
    get: function () { return this.childNodes.length ? this.childNodes[0] : null; }
});
Object.defineProperty(HBNode.prototype, 'lastChild', {
    get: function () { return this.childNodes.length ? this.childNodes[this.childNodes.length - 1] : null; }
});
Object.defineProperty(HBNode.prototype, 'nextSibling', {
    get: function () {
        if (!this.parentNode) return null;
        var i = this.parentNode.childNodes.indexOf(this);
        return i >= 0 && i + 1 < this.parentNode.childNodes.length ?
               this.parentNode.childNodes[i + 1] : null;
    }
});
Object.defineProperty(HBNode.prototype, 'previousSibling', {
    get: function () {
        if (!this.parentNode) return null;
        var i = this.parentNode.childNodes.indexOf(this);
        return i > 0 ? this.parentNode.childNodes[i - 1] : null;
    }
});
Object.defineProperty(HBNode.prototype, 'ownerDocument', {
    get: function () { return this._doc; }
});

/* 文本节点 */
function HBText(text) {
    HBNode.call(this, 3);
    this.nodeName = '#text';
    this._text = text === undefined || text === null ? '' : String(text);
    this.data = this._text;
}
HBText.prototype = Object.create(HBNode.prototype);
HBText.prototype._cloneShallow = function () {
    return new HBText(this._text);
};
Object.defineProperty(HBText.prototype, 'textContent', {
    get: function () { return this._text; },
    set: function (v) { this._text = v === undefined || v === null ? '' : String(v); this.data = this._text; }
});
Object.defineProperty(HBText.prototype, 'nodeValue', {
    get: function () { return this._text; },
    set: function (v) { this._text = v === undefined || v === null ? '' : String(v); this.data = this._text; }
});
HBText.prototype.splitText = function (off) {
    var head = new HBText(this._text.slice(0, off));
    this._text = this._text.slice(off);
    this.data = this._text;
    if (this.parentNode) this.parentNode.insertBefore(head, this);
    return head;
};

/* 元素 */
function HBElement(tag, doc) {
    HBNode.call(this, 1);
    this._doc = doc;
    this.tagName = hb_low(tag);
    this.nodeName = this.tagName.toUpperCase();
    this._attrs = {};
    this._listeners = {};
    this._style = {};
    this.value = '';
    this.checked = false;
    this.selected = false;
    this.disabled = false;
    this.hidden = false;
    this._dataset = {};
    this.scrollTop = 0;
    this.scrollLeft = 0;
}
HBElement.prototype = Object.create(HBNode.prototype);
HBElement.prototype._cloneShallow = function () {
    var c = new HBElement(this.tagName, this._doc);
    var k;
    for (k in this._attrs) if (Object.prototype.hasOwnProperty.call(this._attrs, k))
        c._attrs[k] = this._attrs[k];
    c._style = {};
    for (k in this._style) if (Object.prototype.hasOwnProperty.call(this._style, k))
        c._style[k] = this._style[k];
    c.value = this.value;
    c.checked = this.checked;
    return c;
};
HBElement.prototype._syncClass = function () {
    var cls = this._attrs['class'] || '';
    this._classList = hb_trim(cls).split(/[\t\n\r ]+/).filter(function (s) { return s; });
};
HBElement.prototype.getAttribute = function (name) {
    var v = this._attrs[hb_low(name)];
    return v === undefined ? null : v;
};
HBElement.prototype.hasAttribute = function (name) {
    return this._attrs[hb_low(name)] !== undefined;
};
HBElement.prototype.setAttribute = function (name, value) {
    var n = hb_low(name);
    var v = value === undefined || value === null ? '' : String(value);
    this._attrs[n] = v;
    if (n === 'class') this._syncClass();
    if (n === 'style') this._styleFromString(v);
    if (n.indexOf('data-') === 0) this._dataset[n.slice(5)] = v;
    if (n === 'id') this.id = v;
};
HBElement.prototype.removeAttribute = function (name) {
    var n = hb_low(name);
    delete this._attrs[n];
    if (n === 'class') this._syncClass();
    if (n.indexOf('data-') === 0) delete this._dataset[n.slice(5)];
};
HBElement.prototype.toggleAttribute = function (name, force) {
    if (force === undefined) force = !this.hasAttribute(name);
    if (force) this.setAttribute(name, '');
    else this.removeAttribute(name);
    return force;
};
HBElement.prototype._styleFromString = function (css) {
    var st = {};
    var parts = css.split(';');
    for (var i = 0; i < parts.length; i++) {
        var p = parts[i];
        var c = p.indexOf(':');
        if (c < 0) continue;
        var k = hb_trim(p.slice(0, c));
        if (!k) continue;
        st[hb_low(k)] = hb_trim(p.slice(c + 1));
    }
    this._style = st;
};

Object.defineProperty(HBElement.prototype, 'id', {
    get: function () { return this._attrs['id'] !== undefined ? this._attrs['id'] : ''; },
    set: function (v) { this._attrs['id'] = String(v); }
});
Object.defineProperty(HBElement.prototype, 'className', {
    get: function () { return this._attrs['class'] !== undefined ? this._attrs['class'] : ''; },
    set: function (v) { this._attrs['class'] = String(v); this._syncClass(); }
});
Object.defineProperty(HBElement.prototype, 'classList', {
    get: function () {
        if (!this._classList) this._syncClass();
        var el = this;
        return {
            add: function () {
                for (var i = 0; i < arguments.length; i++) {
                    if (el._classList.indexOf(arguments[i]) < 0)
                        el._classList.push(arguments[i]);
                }
                el._attrs['class'] = el._classList.join(' ');
            },
            remove: function () {
                for (var i = 0; i < arguments.length; i++) {
                    var j = el._classList.indexOf(arguments[i]);
                    if (j >= 0) el._classList.splice(j, 1);
                }
                el._attrs['class'] = el._classList.join(' ');
            },
            contains: function (c) { return el._classList.indexOf(c) >= 0; },
            toggle: function (c, force) {
                var has = el._classList.indexOf(c) >= 0;
                var want = force === undefined ? !has : !!force;
                if (want && !has) { el._classList.push(c); }
                if (!want && has) {
                    var j = el._classList.indexOf(c);
                    el._classList.splice(j, 1);
                }
                el._attrs['class'] = el._classList.join(' ');
                return want;
            }
        };
    }
});
Object.defineProperty(HBElement.prototype, 'style', {
    get: function () {
        var el = this;
        var st = el._style;
        st.setProperty = function (k, v) { st[hb_low(k)] = String(v); };
        st.getPropertyValue = function (k) {
            var v = st[hb_low(k)];
            return v === undefined ? '' : v;
        };
        st.removeProperty = function (k) { delete st[hb_low(k)]; return ''; };
        Object.defineProperty(st, 'cssText', {
            get: function () {
                var out = [];
                for (var k in st) {
                    if (k === 'cssText' || typeof st[k] === 'function') continue;
                    out.push(k + ': ' + st[k]);
                }
                return out.join('; ');
            },
            set: function (v) { el._styleFromString(String(v)); },
            configurable: true
        });
        return st;
    }
});
Object.defineProperty(HBElement.prototype, 'dataset', {
    get: function () { return this._dataset; }
});
Object.defineProperty(HBElement.prototype, 'href', {
    get: function () { return this.getAttribute('href'); },
    set: function (v) { this.setAttribute('href', v); }
});
Object.defineProperty(HBElement.prototype, 'src', {
    get: function () { return this.getAttribute('src'); },
    set: function (v) { this.setAttribute('src', v); }
});

HBElement.prototype._textOf = function () {
    var out = '';
    for (var i = 0; i < this.childNodes.length; i++) {
        var c = this.childNodes[i];
        if (c.nodeType === 3) out += c._text;
        else if (c.nodeType === 1) out += c._textOf();
    }
    return out;
};
Object.defineProperty(HBElement.prototype, 'textContent', {
    get: function () { return this._textOf(); },
    set: function (v) {
        this.childNodes = [];
        this._append(new HBText(v === undefined || v === null ? '' : String(v)));
    }
});
Object.defineProperty(HBElement.prototype, 'innerText', {
    get: function () { return this._textOf(); },
    set: function (v) {
        this.childNodes = [];
        this._append(new HBText(v === undefined || v === null ? '' : String(v)));
    }
});
Object.defineProperty(HBElement.prototype, 'innerHTML', {
    get: function () {
        var out = '';
        for (var i = 0; i < this.childNodes.length; i++) {
            var c = this.childNodes[i];
            if (c.nodeType === 3) out += hb_esc(c._text);
            else out += HBOS.serializeNode(c);
        }
        return out;
    },
    set: function (v) {
        this.childNodes = [];
        if (v) HBOS.parseFragment(String(v), this);
    }
});
Object.defineProperty(HBElement.prototype, 'outerHTML', {
    get: function () { return HBOS.serializeNode(this); }
});

Object.defineProperty(HBElement.prototype, 'children', {
    get: function () {
        return this.childNodes.filter(function (c) { return c.nodeType === 1; });
    }
});
Object.defineProperty(HBElement.prototype, 'firstElementChild', {
    get: function () {
        for (var i = 0; i < this.childNodes.length; i++)
            if (this.childNodes[i].nodeType === 1) return this.childNodes[i];
        return null;
    }
});
Object.defineProperty(HBElement.prototype, 'lastElementChild', {
    get: function () {
        for (var i = this.childNodes.length - 1; i >= 0; i--)
            if (this.childNodes[i].nodeType === 1) return this.childNodes[i];
        return null;
    }
});
Object.defineProperty(HBElement.prototype, 'nextElementSibling', {
    get: function () {
        if (!this.parentNode) return null;
        var sib = this.nextSibling;
        while (sib && sib.nodeType !== 1) sib = sib.nextSibling;
        return sib;
    }
});
Object.defineProperty(HBElement.prototype, 'previousElementSibling', {
    get: function () {
        if (!this.parentNode) return null;
        var sib = this.previousSibling;
        while (sib && sib.nodeType !== 1) sib = sib.previousSibling;
        return sib;
    }
});

/* ── 事件系统 ── */
function HBEvent(type, opts) {
    opts = opts || {};
    this.type = type;
    this.target = null;
    this.currentTarget = null;
    this.bubbles = !!opts.bubbles;
    this.cancelable = !!opts.cancelable;
    this.defaultPrevented = false;
    this.detail = opts.detail !== undefined ? opts.detail : null;
    this.timeStamp = Date.now();
    this._stopped = false;
    this._immediate = false;
}
HBEvent.prototype.preventDefault = function () { this.defaultPrevented = true; };
HBEvent.prototype.stopPropagation = function () { this._stopped = true; };
HBEvent.prototype.stopImmediatePropagation = function () {
    this._stopped = true; this._immediate = true;
};

HBNode.prototype.addEventListener = function (type, fn) {
    if (typeof fn !== 'function') return;
    var t = hb_low(type);
    if (!this._listeners) this._listeners = {};
    if (!this._listeners[t]) this._listeners[t] = [];
    if (this._listeners[t].indexOf(fn) < 0) this._listeners[t].push(fn);
};
HBNode.prototype.removeEventListener = function (type, fn) {
    var t = hb_low(type);
    if (!this._listeners || !this._listeners[t]) return;
    var i = this._listeners[t].indexOf(fn);
    if (i >= 0) this._listeners[t].splice(i, 1);
};
HBNode.prototype.dispatchEvent = function (ev) {
    ev.target = ev.target || this;
    var path = [];
    for (var p = this; p; p = p.parentNode) path.push(p);
    ev.currentTarget = this;
    for (var i = 0; i < path.length && !ev._stopped; i++) {
        var cur = path[i];
        if (i > 0 && !ev.bubbles) break;
        ev.currentTarget = cur;
        var ls = cur._listeners ? cur._listeners[hb_low(ev.type)] : null;
        if (ls) {
            var copy = ls.slice();
            for (var j = 0; j < copy.length && !ev._immediate; j++) {
                if (ls.indexOf(copy[j]) >= 0) copy[j].call(cur, ev);
            }
        }
    }
    return !ev.defaultPrevented;
};
HBElement.prototype.click = function () {
    this.dispatchEvent(new HBEvent('click', { bubbles: true }));
};
HBElement.prototype.focus = function () { };
HBElement.prototype.blur = function () { };
HBElement.prototype.scrollIntoView = function () { };
HBElement.prototype.getBoundingClientRect = function () {
    var w = 0, h = 0;
    var sw = this._style['width'], sh = this._style['height'];
    function px(v) {
        if (!v) return 0;
        v = hb_trim(String(v));
        var m = /^(-?[\d.]+)(px|%)?$/.exec(v);
        if (!m) return 0;
        var val = parseFloat(m[1]);
        if (m[2] === '%') val = val / 100 * 1000;
        return val;
    }
    w = px(sw) || px(this._attrs['width']) || (this.tagName === 'img' ? 0 : 0);
    h = px(sh) || px(this._attrs['height']);
    if (this.tagName === 'video') { if (!w) w = 960; if (!h) h = 540; }
    return { top: 0, left: 0, right: w, bottom: h, width: w, height: h,
             x: 0, y: 0 };
};
Object.defineProperty(HBElement.prototype, 'offsetWidth', {
    get: function () { return this.getBoundingClientRect().width; }
});
Object.defineProperty(HBElement.prototype, 'offsetHeight', {
    get: function () { return this.getBoundingClientRect().height; }
});
HBElement.prototype.matches = function (sel) {
    return HBOS.matches(this, sel);
};
HBElement.prototype.closest = function (sel) {
    for (var p = this; p && p.nodeType === 1; p = p.parentNode)
        if (HBOS.matches(p, sel)) return p;
    return null;
};
HBElement.prototype.getAttributeNS = HBElement.prototype.getAttribute;
HBElement.prototype.setAttributeNS = HBElement.prototype.setAttribute;
HBElement.prototype.removeAttributeNS = HBElement.prototype.removeAttribute;
HBElement.prototype.querySelector = function (sel) {
    return HBOS.query(this, sel, false);
};
HBElement.prototype.querySelectorAll = function (sel) {
    return HBOS.query(this, sel, true);
};
HBElement.prototype.getElementsByTagName = function (tag) {
    var t = hb_low(tag);
    var out = [];
    (function walk(n) {
        for (var i = 0; i < n.childNodes.length; i++) {
            var c = n.childNodes[i];
            if (c.nodeType !== 1) continue;
            if (t === '*' || c.tagName === t) out.push(c);
            walk(c);
        }
    })(this);
    return out;
};
HBElement.prototype.getElementsByClassName = function (cls) {
    var wanted = hb_trim(cls).split(/[\t\n\r ]+/);
    var out = [];
    (function walk(n) {
        for (var i = 0; i < n.childNodes.length; i++) {
            var c = n.childNodes[i];
            if (c.nodeType !== 1) continue;
            var ok = true;
            for (var j = 0; j < wanted.length; j++) {
                if (!c.classList.contains(wanted[j])) { ok = false; break; }
            }
            if (ok) out.push(c);
            walk(c);
        }
    })(this);
    return out;
};
HBElement.prototype.insertAdjacentHTML = function (where, html) {
    var frag = HBOS.parseFragment(html, this._doc ? this._doc.documentElement : null);
    if (where === 'beforebegin' || where === 'afterend') {
        var tmp = document.createElement('div');
        tmp.childNodes = frag;
        if (where === 'beforebegin') {
            for (var i = 0; i < frag.length; i++)
                if (this.parentNode) this.parentNode.insertBefore(frag[i], this);
        } else {
            for (var j = 0; j < frag.length; j++)
                if (this.parentNode) this.parentNode.insertBefore(frag[j], this.nextSibling);
        }
        return;
    }
    if (where === 'afterbegin') {
        for (var k = frag.length - 1; k >= 0; k--)
            this.insertBefore(frag[k], this.firstChild);
        return;
    }
    for (var m = 0; m < frag.length; m++) this._append(frag[m]);
};
HBElement.prototype.after = function () {
    for (var i = 0; i < arguments.length; i++) {
        var n = arguments[i];
        if (typeof n === 'string') n = document.createTextNode(n);
        if (this.parentNode) this.parentNode.insertBefore(n, this.nextSibling);
    }
};
HBElement.prototype.before = function () {
    for (var i = 0; i < arguments.length; i++) {
        var n = arguments[i];
        if (typeof n === 'string') n = document.createTextNode(n);
        if (this.parentNode) this.parentNode.insertBefore(n, this);
    }
};
HBElement.prototype.replaceWith = function () {
    for (var i = 0; i < arguments.length; i++) {
        var n = arguments[i];
        if (typeof n === 'string') n = document.createTextNode(n);
        if (this.parentNode) this.parentNode.insertBefore(n, this);
    }
    if (this.parentNode) this.parentNode.removeChild(this);
};

/* ── CSS 选择器子集 ── */
HBOS.parseSelector = function (sel) {
    /* 返回 groups: [{steps:[{comb:' '|'>', simple:{tag,id,classes,attrs}}]}],
     * 解析失败返回 null */
    var groups = [];
    var parts = String(sel).split(',');
    for (var g = 0; g < parts.length; g++) {
        var tokens = [];
        var rest = parts[g];
        while (hb_trim(rest)) {
            rest = rest.replace(/^[\t\n\r ]+/, '');
            var comb = ' ';
            if (rest.charAt(0) === '>') { comb = '>'; rest = rest.slice(1).replace(/^[\t\n\r ]+/, ''); }
            var simple = { tag: null, id: null, classes: [], attrs: [] };
            var matched = false;
            while (rest) {
                var c0 = rest.charAt(0);
                if (c0 === '>' || c0 === ',') break;
                if (c0 === ' ' || c0 === '\t' || c0 === '\n' || c0 === '\r') break;
                if (c0 === '#') {
                    var m = /^#([\w-]+)/.exec(rest);
                    if (!m) return null;
                    simple.id = m[1];
                    rest = rest.slice(m[0].length);
                    matched = true;
                } else if (c0 === '.') {
                    var m2 = /^\.([\w-]+)/.exec(rest);
                    if (!m2) return null;
                    simple.classes.push(m2[1]);
                    rest = rest.slice(m2[0].length);
                    matched = true;
                } else if (c0 === '[') {
                    var m3 = /^\[([\w-]+)([~|^$*]?=)?\s*(?:"([^"]*)"|'([^']*)'|([^\]]*))?\]/.exec(rest);
                    if (!m3) return null;
                    simple.attrs.push({ name: m3[1], op: m3[2] || 'exists', val: m3[3] !== undefined ? m3[3] : (m3[4] !== undefined ? m3[4] : (m3[5] || '')) });
                    rest = rest.slice(m3[0].length);
                    matched = true;
                } else {
                    var m4 = /^([\w-]+|\*)/.exec(rest);
                    if (!m4) return null;
                    simple.tag = m4[1].toLowerCase();
                    rest = rest.slice(m4[0].length);
                    matched = true;
                }
            }
            if (!matched) return null;
            tokens.push({ comb: comb, simple: simple });
        }
        if (tokens.length) groups.push(tokens);
    }
    return groups.length ? groups : null;
};

HBOS.matchSimple = function (el, s) {
    if (s.tag && s.tag !== '*' && el.tagName !== s.tag) return false;
    if (s.id && el.id !== s.id) return false;
    for (var i = 0; i < s.classes.length; i++)
        if (!el.classList.contains(s.classes[i])) return false;
    for (var j = 0; j < s.attrs.length; j++) {
        var a = s.attrs[j];
        var v = el.getAttribute(a.name);
        if (v === null) return false;
        if (a.op === 'exists') continue;
        if (a.op === '=') { if (v !== a.val) return false; continue; }
        if (a.op === '^=') { if (v.indexOf(a.val) !== 0) return false; continue; }
        if (a.op === '$=') { if (v.length < a.val.length || v.slice(v.length - a.val.length) !== a.val) return false; continue; }
        if (a.op === '*=') { if (v.indexOf(a.val) < 0) return false; continue; }
        if (a.op === '~=') {
            var words = v.split(/[\t\n\r ]+/);
            if (words.indexOf(a.val) < 0) return false;
            continue;
        }
    }
    return true;
};

/* 在 root 子树内按 groups 找元素；first=true 返回第一个（文档序） */
HBOS.query = function (root, sel, all) {
    var groups = HBOS.parseSelector(sel);
    if (!groups) return all ? [] : null;
    var found = [];
    var seen = {};
    for (var g = 0; g < groups.length; g++) {
        var steps = groups[g];
        var cur = [];
        (function seed(n) {
            for (var i = 0; i < n.childNodes.length; i++) {
                var c = n.childNodes[i];
                if (c.nodeType !== 1) continue;
                if (HBOS.matchSimple(c, steps[0].simple)) cur.push(c);
                seed(c);
            }
        })(root);
        for (var s = 1; s < steps.length; s++) {
            var nxt = [];
            for (var i = 0; i < cur.length; i++) {
                if (steps[s].comb === '>') {
                    for (var j = 0; j < cur[i].childNodes.length; j++) {
                        var ch = cur[i].childNodes[j];
                        if (ch.nodeType === 1 && HBOS.matchSimple(ch, steps[s].simple))
                            nxt.push(ch);
                    }
                } else {
                    (function walk(n) {
                        for (var k = 0; k < n.childNodes.length; k++) {
                            var c2 = n.childNodes[k];
                            if (c2.nodeType !== 1) continue;
                            if (HBOS.matchSimple(c2, steps[s].simple)) nxt.push(c2);
                            walk(c2);
                        }
                    })(cur[i]);
                }
            }
            cur = nxt;
        }
        for (var m = 0; m < cur.length; m++) {
            var el = cur[m];
            if (seen[el._hbos_id]) continue;
            if (!seen[el._hbos_id]) {
                if (!el._hbos_id) el._hbos_id = (++HBOS._idCounter).toString(36);
                seen[el._hbos_id] = 1;
                found.push(el);
            }
        }
    }
    if (!all) return found.length ? found[0] : null;
    return found;
};

HBOS.matches = function (el, sel) {
    var groups = HBOS.parseSelector(sel);
    if (!groups) return false;
    for (var g = 0; g < groups.length; g++) {
        var steps = groups[g];
        var ok = HBOS.matchSimple(el, steps[steps.length - 1].simple);
        if (!ok) continue;
        var cur = el;
        var s = steps.length - 2;
        while (s >= 0 && cur) {
            if (steps[s + 1].comb === '>') {
                cur = cur.parentNode;
                if (!cur || cur.nodeType !== 1 || !HBOS.matchSimple(cur, steps[s].simple)) { ok = false; break; }
            } else {
                var anc = cur.parentNode;
                while (anc && anc.nodeType === 1 && !HBOS.matchSimple(anc, steps[s].simple)) anc = anc.parentNode;
                if (!anc || anc.nodeType !== 1) { ok = false; break; }
                cur = anc;
            }
            s--;
        }
        if (ok && s < 0) return true;
    }
    return false;
};
HBOS._idCounter = 0;

/* ── HTML 解析器 ── */
HBOS.parseFragment = function (html, parent) {
    /* 把 html 解析出的节点依次挂到 parent 下，返回节点数组 */
    var doc = parent._doc || (parent.nodeType === 9 ? parent : null);
    var out = [];
    var i = 0;
    var n = html.length;
    if (parent && parent.nodeType === 1) parent._hbos_stack = [];
    function fragTop() {
        if (parent && parent.nodeType === 1 && parent._hbos_stack &&
            parent._hbos_stack.length)
            return parent._hbos_stack[parent._hbos_stack.length - 1];
        return parent;
    }
    while (i < n) {
        var lt = html.indexOf('<', i);
        if (lt < 0) {
            var txt = hb_decodeEnt(html.slice(i));
            if (txt) {
                var tn = new HBText(txt);
                tn._doc = doc;
                var ft = fragTop();
                if (ft && ft.nodeType === 1) ft._append(tn);
                out.push(tn);
            }
            break;
        }
        if (lt > i) {
            var t2 = hb_decodeEnt(html.slice(i, lt));
            if (t2) {
                var tn2 = new HBText(t2);
                tn2._doc = doc;
                var ft2 = fragTop();
                if (ft2 && ft2.nodeType === 1) ft2._append(tn2);
                out.push(tn2);
            }
        }
        /* 找标签结束 */
        var gt = -1;
        var quote = 0;
        for (var k = lt + 1; k < n; k++) {
            var ch = html.charAt(k);
            if (quote) { if (ch === quote) quote = 0; continue; }
            if (ch === '"' || ch === "'") { quote = ch; continue; }
            if (ch === '>') { gt = k; break; }
        }
        if (gt < 0) break;
        var raw = html.slice(lt, gt + 1);
        var tagText = raw.slice(1, -1);
        if (tagText.charAt(0) === '!' || tagText.charAt(0) === '?') {
            i = gt + 1;
            continue;
        }
        var isClose = tagText.charAt(0) === '/';
        var body = isClose ? tagText.slice(1) : tagText;
        var nameMatch = /^[\t\n\r ]*([a-zA-Z][\w:-]*)/.exec(body);
        if (!nameMatch) { i = gt + 1; continue; }
        var tag = nameMatch[1].toLowerCase();
        var selfClose = /\/\s*$/.test(body) || HB_VOID[tag] === 1;
        if (isClose) {
            /* 从栈顶往下找同名元素，把匹配到的（含其上所有）全部关掉 */
            var stack = parent._hbos_stack || null;
            if (stack) {
                for (var si = stack.length - 1; si >= 0; si--) {
                    if (stack[si].tagName === tag) {
                        var removed = stack.splice(si);
                        for (var ri = 0; ri < removed.length; ri++)
                            delete removed[ri]._hbos_stack;
                        break;
                    }
                }
            }
            i = gt + 1;
            continue;
        }
        var el = new HBElement(tag, doc);
        el._hbos_stack = [];
        /* 解析属性 */
        var attrRe = /([\w:-]+)(?:\s*=\s*(?:"([^"]*)"|'([^']*)'|([^\s"'=<>`]+)))?/g;
        var am;
        var attrSrc = body.slice(nameMatch[0].length);
        while ((am = attrRe.exec(attrSrc)) !== null) {
            var an = am[1].toLowerCase();
            var av = am[2] !== undefined ? am[2] : (am[3] !== undefined ? am[3] : (am[4] !== undefined ? am[4] : ''));
            el.setAttribute(an, hb_decodeEnt(av));
        }
        /* 挂到 parent；若无 parent（fragment 顶层），先收集 */
        var target = parent;
        if (parent && parent.nodeType === 1) {
            /* 隐式闭合：新标签要关掉某些顶层标签 */
            if (HB_CLOSES[tag]) {
                var stk = parent._hbos_stack || [];
                while (stk.length && HB_CLOSES[tag].indexOf(stk[stk.length - 1].tagName) >= 0) {
                    var closed = stk.pop();
                    delete closed._hbos_stack;
                }
            }
            if (HB_BLOCK[tag] && parent._hbos_stack && parent._hbos_stack.length &&
                parent._hbos_stack[parent._hbos_stack.length - 1].tagName === 'p') {
                var closedP = parent._hbos_stack.pop();
                delete closedP._hbos_stack;
            }
            if (selfClose) {
                parent._append(el);
                out.push(el);
            } else if (HB_RAW[tag]) {
                parent._append(el);
                out.push(el);
                /* 原始文本：直到 </tag */
                var closeTag = '</' + tag;
                var ci = html.toLowerCase().indexOf(closeTag, gt + 1);
                var endIdx = ci >= 0 ? ci + closeTag.length : n;
                var rawText = html.slice(gt + 1, ci >= 0 ? ci : n);
                if (rawText) {
                    el._append(new HBText(rawText));
                    if (tag === 'title' && parent._doc) {
                        parent._doc._titleText = rawText;
                    }
                }
                if (ci >= 0) {
                    var gt2 = html.indexOf('>', ci);
                    i = gt2 >= 0 ? gt2 + 1 : endIdx;
                } else {
                    i = n;
                }
                continue;
            } else {
                parent._append(el);
                out.push(el);
                parent._hbos_stack = parent._hbos_stack || [];
                parent._hbos_stack.push(el);
                el._hbos_stack = parent._hbos_stack;
            }
        } else {
            if (selfClose || HB_RAW[tag]) {
                out.push(el);
            } else {
                out.push(el);
                el._hbos_stack = [];
            }
        }
        i = gt + 1;
    }
    return out;
};

HBOS.parseHTML = function (html, doc) {
    /* 完整页面解析：head/body 结构与标签栈。head/body 用 ensureHead/
     * ensureBody 惰性创建并确保挂在 documentElement 下（真实页面经常
     * 省略标签、或在 <head> 之前就有空白文本，隐式创建后必须真正
     * append 到树上，否则查询不到）。 */
    var root = doc.documentElement;
    var stack = [root];
    var head = null, body = null;
    var headDone = false;
    var HEAD_CONTENT = { base:1, link:1, meta:1, title:1, style:1, script:1 };

    function ensureHead() {
        if (!head) { head = doc.head; root._append(head); }
        if (stack[stack.length - 1] !== head) stack.push(head);
    }
    function ensureBody() {
        if (!body) { body = doc.body; root._append(body); }
        if (stack[stack.length - 1] !== body) stack.push(body);
    }

    var i = 0;
    var n = html.length;
    while (i < n) {
        var lt = html.indexOf('<', i);
        if (lt < 0) {
            var txt = hb_decodeEnt(html.slice(i));
            if (txt && stack.length > 0 &&
                stack[stack.length - 1].nodeType === 1) {
                if (stack[stack.length - 1] === root) {
                    if (!headDone) ensureHead(); else ensureBody();
                }
                stack[stack.length - 1]._append(new HBText(txt));
            }
            break;
        }
        if (lt > i) {
            var t2 = hb_decodeEnt(html.slice(i, lt));
            if (t2 && stack[stack.length - 1].nodeType === 1) {
                if (stack[stack.length - 1] === root) {
                    if (!headDone) ensureHead(); else ensureBody();
                }
                stack[stack.length - 1]._append(new HBText(t2));
            }
        }
        var quote = 0, gt = -1;
        for (var k = lt + 1; k < n; k++) {
            var ch = html.charAt(k);
            if (quote) { if (ch === quote) quote = 0; continue; }
            if (ch === '"' || ch === "'") { quote = ch; continue; }
            if (ch === '>') { gt = k; break; }
        }
        if (gt < 0) break;
        var tagText = html.slice(lt + 1, gt);
        var isClose = tagText.charAt(0) === '/';
        var body2 = isClose ? tagText.slice(1) : tagText;
        var nameMatch = /^[\t\n\r ]*([a-zA-Z][\w:-]*)/.exec(body2);
        if (!nameMatch) { i = gt + 1; continue; }
        var tag = nameMatch[1].toLowerCase();
        var selfClose = /\/\s*$/.test(tagText) || HB_VOID[tag] === 1;
        if (tagText.charAt(0) === '!' || tagText.charAt(0) === '?') {
            if (/^!doctype/i.test(tagText)) doc._doctype = tagText.slice(2);
            i = gt + 1;
            continue;
        }
        if (isClose) {
            for (var si = stack.length - 1; si >= 0; si--) {
                if (stack[si].nodeType === 1 && stack[si].tagName === tag) {
                    if (tag === 'head') headDone = true;
                    /* </html> 或 </body> 之后还有文本时栈不能清空：留着
                     * root，尾部内容继续落到 html/body 下。 */
                    stack.length = si < 1 ? 1 : si;
                    break;
                }
            }
            i = gt + 1;
            continue;
        }
        if (tag === 'html') { i = gt + 1; continue; }
        if (tag === 'head') {
            ensureHead();
            headDone = true;
            i = gt + 1;
            continue;
        }
        if (tag === 'body') {
            ensureBody();
            i = gt + 1;
            continue;
        }
        var top = stack[stack.length - 1];
        if (top === root) {
            if (!headDone && HEAD_CONTENT[tag]) ensureHead();
            else ensureBody();
        }
        var el = new HBElement(tag, doc);
        var attrRe = /([\w:-]+)(?:\s*=\s*(?:"([^"]*)"|'([^']*)'|([^\s"'=<>`]+)))?/g;
        var am;
        var attrSrc = body2.slice(nameMatch[0].length);
        while ((am = attrRe.exec(attrSrc)) !== null) {
            el.setAttribute(am[1].toLowerCase(),
                am[2] !== undefined ? am[2] : (am[3] !== undefined ? am[3] : (am[4] !== undefined ? am[4] : '')));
        }
        if (HB_CLOSES[tag]) {
            while (stack.length > 1 && HB_CLOSES[tag].indexOf(stack[stack.length - 1].tagName) >= 0)
                stack.pop();
        }
        if (HB_BLOCK[tag] && stack[stack.length - 1].tagName === 'p') stack.pop();
        var parent = stack[stack.length - 1];
        parent._append(el);
        if (HB_RAW[tag]) {
            var closeTag = '</' + tag;
            var ci = html.toLowerCase().indexOf(closeTag, gt + 1);
            var rawEnd = ci >= 0 ? ci : n;
            var rawText = html.slice(gt + 1, rawEnd);
            if (rawText) {
                el._append(new HBText(rawText));
                if (tag === 'title') doc._titleText = hb_trim(rawText);
            }
            if (ci >= 0) {
                var gt2 = html.indexOf('>', ci);
                i = gt2 >= 0 ? gt2 + 1 : rawEnd;
            } else {
                i = n;
            }
            continue;
        }
        if (!selfClose) stack.push(el);
        i = gt + 1;
    }
    /* 兜底：无 body 时建空 body */
    if (!body) {
        body = doc.body;
        root._append(body);
    }
    if (!doc._titleText) {
        var ts = doc.getElementsByTagName('title');
        if (ts.length && ts[0].childNodes.length)
            doc._titleText = hb_trim(ts[0].childNodes[0]._text);
    }
    return doc;
};

/* ── 序列化（innerHTML/outerHTML 用）── */
HBOS.serializeNode = function (el) {
    var out = '<' + el.tagName;
    for (var k in el._attrs) {
        if (Object.prototype.hasOwnProperty.call(el._attrs, k)) {
            if (k === el.tagName) continue;
            out += ' ' + k + '="' + hb_esc(el._attrs[k]) + '"';
        }
    }
    if (HB_VOID[el.tagName]) return out + '>';
    out += '>';
    for (var i = 0; i < el.childNodes.length; i++) {
        var c = el.childNodes[i];
        if (c.nodeType === 3) out += hb_esc(c._text);
        else out += HBOS.serializeNode(c);
    }
    return out + '</' + el.tagName + '>';
};

/* ── Document ── */
function HBDocument() {
    HBNode.call(this, 9);
    this._doc = this;
    this.readyState = 'loading';
    this._titleText = '';
    var html = new HBElement('html', this);
    this._append(html);
    this._head = new HBElement('head', this);
    this._body = new HBElement('body', this);
}
HBDocument.prototype = Object.create(HBNode.prototype);
Object.defineProperty(HBDocument.prototype, 'documentElement', {
    get: function () { return this.childNodes[0]; }
});
Object.defineProperty(HBDocument.prototype, 'head', {
    get: function () { return this._head; }
});
Object.defineProperty(HBDocument.prototype, 'body', {
    get: function () { return this._body; }
});
Object.defineProperty(HBDocument.prototype, 'title', {
    get: function () { return this._titleText || ''; },
    set: function (v) { this._titleText = String(v); }
});
HBDocument.prototype.createElement = function (tag) {
    return new HBElement(hb_low(tag), this);
};
HBDocument.prototype.createElementNS = function (ns, tag) {
    return new HBElement(hb_low(tag), this);
};
HBDocument.prototype.createTextNode = function (text) {
    var t = new HBText(text);
    t._doc = this;
    return t;
};
HBDocument.prototype.createEvent = function (type) {
    return {
        type: String(type || ''),
        bubbles: false,
        cancelable: false,
        detail: null,
        initEvent: function (t, b, c) {
            this.type = String(t); this.bubbles = !!b; this.cancelable = !!c;
        },
        initCustomEvent: function (t, b, c, d) {
            this.initEvent(t, b, c); this.detail = d;
        },
        addEventListener: function () { },
        removeEventListener: function () { },
        dispatchEvent: function () { return true; }
    };
};
HBDocument.prototype.createComment = function () {
    var t = new HBText('');
    t._doc = this;
    t.nodeType = 8;
    t.nodeName = '#comment';
    return t;
};
HBDocument.prototype.createDocumentFragment = function () {
    var f = new HBNode(11);
    f._doc = this;
    return f;
};
HBDocument.prototype.getElementById = function (id) {
    var out = null;
    (function walk(n) {
        if (out) return;
        for (var i = 0; i < n.childNodes.length; i++) {
            var c = n.childNodes[i];
            if (c.nodeType !== 1) continue;
            if (c.id === id) { out = c; return; }
            walk(c);
        }
    })(this.documentElement);
    return out;
};
HBDocument.prototype.getElementsByTagName = function (tag) {
    return this.documentElement.getElementsByTagName(tag);
};
HBDocument.prototype.getElementsByClassName = function (cls) {
    return this.documentElement.getElementsByClassName(cls);
};
HBDocument.prototype.querySelector = function (sel) {
    return HBOS.query(this.documentElement, sel, false);
};
HBDocument.prototype.querySelectorAll = function (sel) {
    return HBOS.query(this.documentElement, sel, true);
};
HBDocument.prototype.write = function () {
    var str = '';
    for (var i = 0; i < arguments.length; i++) str += String(arguments[i]);
    if (str) HBOS.parseFragment(str, this._body);
};
HBDocument.prototype.writeln = function () {
    var str = '';
    for (var i = 0; i < arguments.length; i++) str += String(arguments[i]);
    this.write(str + '\n');
};
HBDocument.prototype.open = function () { return this; };
HBDocument.prototype.close = function () { };
HBDocument.prototype.hasFocus = function () { return true; };
HBDocument.prototype.createRange = function () {
    return {
        setStart: function () {}, setEnd: function () {},
        selectNodeContents: function () {}, selectNode: function () {},
        collapse: function () {},
        getBoundingClientRect: function () { return { top: 0, left: 0, width: 0, height: 0, right: 0, bottom: 0 }; },
        cloneRange: function () { return this; }
    };
};
HBDocument.prototype.getSelection = function () {
    return { rangeCount: 0, removeAllRanges: function () {} };
};
HBDocument.prototype.exitFullscreen = function () { };
HBDocument.prototype.requestFullscreen = function () { };
Object.defineProperty(HBDocument.prototype, 'cookie', {
    get: function () { return ''; },
    set: function () { }
});
Object.defineProperty(HBDocument.prototype, 'referrer', {
    get: function () { return ''; }
});
Object.defineProperty(HBDocument.prototype, 'documentURI', {
    get: function () { return window.location.href; }
});
Object.defineProperty(HBDocument.prototype, 'hidden', {
    get: function () { return false; }
});
Object.defineProperty(HBDocument.prototype, 'visibilityState', {
    get: function () { return 'visible'; }
});
Object.defineProperty(HBDocument.prototype, 'readyState', {
    get: function () { return this._readyState || 'loading'; },
    set: function (v) { this._readyState = v; }
});
Object.defineProperty(HBDocument.prototype, 'scripts', {
    get: function () { return this.getElementsByTagName('script'); }
});
Object.defineProperty(HBDocument.prototype, 'images', {
    get: function () { return this.getElementsByTagName('img'); }
});
Object.defineProperty(HBDocument.prototype, 'links', {
    get: function () { return this.getElementsByTagName('a'); }
});
Object.defineProperty(HBDocument.prototype, 'forms', {
    get: function () { return this.getElementsByTagName('form'); }
});

/* 文档创建 */
HBOS.createDocument = function () {
    var d = new HBDocument();
    d._doc = d;
    d._readyState = 'loading';
    return d;
};

/* ── 定时器/动画帧队列（脚本期 defer，脚本后统一 flush）── */
HBOS._timeouts = [];
HBOS._raf = [];
HBOS._timerId = 1;
function hb_setTimeout(fn, ms) {
    if (typeof fn !== 'function') return 0;
    if (ms !== undefined && ms > 16) return 0;  /* 长定时器不执行（防挂） */
    var id = HBOS._timerId++;
    HBOS._timeouts.push(fn);
    return id;
}
function hb_clearTimeout() { }
function hb_setInterval() { return 0; }
function hb_clearInterval() { }
function hb_requestAnimationFrame(fn) {
    if (typeof fn !== 'function') return 0;
    var id = HBOS._timerId++;
    HBOS._raf.push(fn);
    return id;
}
function hb_cancelAnimationFrame() { }

HBOS.flushDeferred = function () {
    /* 脚本 eval 完成后按浏览器语义触发：DOMContentLoaded → 零延时
     * 回调/rAF（带轮次上限防死循环）→ load。 */
    var doc = document;
    doc.dispatchEvent(new HBEvent('DOMContentLoaded', { bubbles: true }));
    for (var pass = 0; pass < 4; pass++) {
        var t = HBOS._timeouts.slice();
        HBOS._timeouts = [];
        for (var i = 0; i < t.length && i < 8; i++) {
            try { t[i](); } catch (e) { }
        }
        var r = HBOS._raf.slice();
        HBOS._raf = [];
        for (var j = 0; j < r.length && j < 8; j++) {
            try { r[j](Date.now()); } catch (e2) { }
        }
        if (!HBOS._timeouts.length && !HBOS._raf.length) break;
    }
    HBOS._timeouts = [];
    HBOS._raf = [];
    doc._readyState = 'complete';
    window.dispatchEvent(new HBEvent('load', { bubbles: false }));
    window.dispatchEvent(new HBEvent('pageshow', { bubbles: false }));
    doc.dispatchEvent(new HBEvent('readystatechange', { bubbles: false }));
};

/* Bilibili 视频页增强：官方完整播放器依赖 MSE/Workers/大 bundle，LiteJS
 * 暂时无法执行，但 __INITIAL_STATE__ 已含真实视频元数据。把它转换为语义
 * DOM，使视频页在轻量模式下仍可浏览标题、封面、UP、统计、简介、标签和
 * 相关推荐；后续 MSE 后端可替换播放器占位而不改页面数据层。 */
HBOS.enhanceBilibiliVideo = function () {
    if (!window.__INITIAL_STATE__ || !window.__INITIAL_STATE__.videoData)
        return false;
    var state = window.__INITIAL_STATE__;
    var v = state.videoData;
    if (!v || !v.title) return false;
    var body = document.body;
    if (!body) return false;
    var currentText = body.textContent || '';
    if (currentText.indexOf(v.title) >= 0 && currentText.length > 300)
        return false;

    function fmt(n) {
        n = Number(n) || 0;
        if (n >= 100000000) return (n / 100000000).toFixed(1) + '亿';
        if (n >= 10000) return (n / 10000).toFixed(1) + '万';
        return String(n);
    }
    function add(parent, tag, text, cls) {
        var el = document.createElement(tag);
        if (cls) el.className = cls;
        if (text !== undefined && text !== null) el.textContent = String(text);
        parent.appendChild(el);
        return el;
    }

    body.innerHTML = '';
    var root = add(body, 'main', null, 'bili-lite-video');
    var media = add(root, 'section', null, 'bili-media');
    if (v.pic) {
        var img = document.createElement('img');
        img.setAttribute('src', String(v.pic).replace(/^http:/, 'https:'));
        img.setAttribute('alt', v.title);
        media.appendChild(img);
    }
    var video = document.createElement('video');
    video.setAttribute('title', v.title);
    media.appendChild(video);

    /* 播放地址通过官方 playurl API 获取。最终 DOM 渲染阶段不能阻塞做
     * 公网 TLS 请求（会让浏览器后台加载任务长时间占用）；只有页面脚本
     * 已经提供 playinfo 时才展示地址。显式点击加载由后续媒体控制器处理。 */
    var playUrl = '';
    var playQuality = '';
    var playLength = Number(v.duration) * 1000 || 0;
    try {
        var pd = window.__playinfo__ && window.__playinfo__.data;
        if (pd) {
            playQuality = pd.format || (pd.quality ? String(pd.quality) + 'p' : '');
            playLength = Number(pd.timelength) || playLength;
            if (pd.durl && pd.durl.length && pd.durl[0].url)
                playUrl = String(pd.durl[0].url);
        }
    } catch (playErr) { }
    if (playUrl) {
        var playLink = add(media, 'a', '▶ 播放 ' + (playQuality || '视频') +
            '（' + Math.round(playLength / 1000) + ' 秒）', 'play-link');
        playLink.setAttribute('href', playUrl);
        playLink.setAttribute('download', '');
    }
    add(media, 'p', playUrl ?
        '轻量播放模式 · 已获取真实视频流地址；本版本尚未集成 H.264/AAC 解码' :
        '轻量播放模式 · 视频流地址获取失败', 'player-state');

    add(root, 'h1', v.title, 'video-title');
    var owner = v.owner || state.upData || {};
    add(root, 'h2', owner.name ? 'UP主：' + owner.name : 'Bilibili 视频', 'owner');
    var stat = v.stat || {};
    add(root, 'p', '播放 ' + fmt(stat.view) + '  ·  弹幕 ' + fmt(stat.danmaku) +
        '  ·  点赞 ' + fmt(stat.like) + '  ·  投币 ' + fmt(stat.coin) +
        '  ·  收藏 ' + fmt(stat.favorite) + '  ·  评论 ' + fmt(stat.reply), 'stats');

    if (state.tags && state.tags.length) {
        var tags = add(root, 'p', null, 'tags');
        for (var i = 0; i < state.tags.length && i < 12; i++) {
            var name = state.tags[i] && (state.tags[i].tag_name || state.tags[i].name);
            if (name) tags.appendChild(document.createTextNode((i ? ' · ' : '') + name));
        }
    }
    if (v.desc && v.desc !== '-') {
        add(root, 'h2', '视频简介');
        add(root, 'p', v.desc, 'description');
    }
    if (state.related && state.related.length) {
        add(root, 'h2', '相关推荐');
        var list = add(root, 'ul', null, 'related');
        for (var j = 0; j < state.related.length && j < 12; j++) {
            var item = state.related[j];
            if (!item || !item.title) continue;
            var li = add(list, 'li');
            var a = add(li, 'a', item.title);
            if (item.bvid) a.setAttribute('href', 'https://www.bilibili.com/video/' + item.bvid);
            var views = item.stat && item.stat.view;
            if (views !== undefined) li.appendChild(document.createTextNode('  播放 ' + fmt(views)));
        }
    }
    document.title = v.title + '_哔哩哔哩_bilibili';
    return true;
};

/* ── 渲染器：DOM → 迷你 HTML（内核浏览器再解析，获得完整样式）── */
HBOS.renderPage = function () {
    try { HBOS.enhanceBilibiliVideo(); } catch (e) { }
    var out = [];
    var budget = 4000;      /* 遍历节点上限 */
    var outCap = 24000;     /* 输出字符上限 */

    function blockOpen(tag, el) {
        var s = '<' + tag;
        var cls = el._attrs['class'];
        var style = el._attrs['style'];
        if (cls) s += ' class="' + hb_esc(cls) + '"';
        if (style) s += ' style="' + hb_esc(style) + '"';
        return s + '>';
    }

    function walk(el) {
        if (budget <= 0) return;
        budget--;
        var tag = el.tagName;
        if (HB_SKIP[tag]) return;
        if (tag === 'br') { out.push('\n'); return; }
        if (tag === 'img') {
            var alt = el._attrs['alt'] || el._attrs['title'] || '';
            var src = el._attrs['src'];
            out.push('<img');
            if (src && src.indexOf('data:') !== 0)
                out.push(' src="' + hb_esc(src) + '"');
            if (alt) out.push(' alt="' + hb_esc(alt) + '"');
            out.push('>');
            return;
        }
        if (tag === 'video' || tag === 'audio') {
            var vtitle = el._attrs['title'] || el._attrs['alt'] || '视频';
            out.push('<img alt="▶ ' + hb_esc(vtitle) + '">');
            return;
        }
        if (tag === 'input') {
            var it = el._attrs['type'] || 'text';
            var ph = el._attrs['placeholder'] || '';
            out.push('[输入框' + (ph ? ':' + hb_esc(ph) : '') + ']');
            return;
        }
        if (tag === 'button' || tag === 'a') {
            /* a 保留 href 供内核渲染成链接（BRK_LINK）；按钮降级为文本 */
            if (tag === 'a') {
                var href = el._attrs['href'];
                out.push('<a');
                if (href) out.push(' href="' + hb_esc(href) + '"');
                out.push('>');
                walkChildren(el);
                out.push('</a>');
                return;
            }
            out.push('[按钮]');
            walkChildren(el);
            out.push('[/按钮]');
            return;
        }
        if (tag === 'textarea') {
            out.push('[多行输入]');
            if (el.childNodes.length) out.push(hb_esc(el._textOf()));
            out.push('[/多行输入]');
            return;
        }
        if (tag === 'select') {
            out.push('[下拉]' + hb_esc(el._textOf()) + '[/下拉]');
            return;
        }
        if (tag === 'option') { return; }   /* 已在 select 文本里 */
        if (tag === 'label' || tag === 'span' || tag === 'strong' || tag === 'b' ||
            tag === 'em' || tag === 'i' || tag === 'u' || tag === 'small' ||
            tag === 'code' || tag === 'sub' || tag === 'sup' || tag === 'mark') {
            out.push(blockOpen(tag, el));
            walkChildren(el);
            out.push('</' + tag + '>');
            return;
        }
        if (tag === 'li') {
            /* 内核渲染器会给 <ol> 的 li 自己编号（BRK_LI_NUM），这里只出
             * 结构，不写数字，避免双份编号。 */
            out.push('<li>');
            walkChildren(el);
            out.push('</li>');
            return;
        }
        if (tag === 'ul' || tag === 'ol') {
            out.push(blockOpen(tag, el));
            walkChildren(el);
            out.push('</' + tag + '>');
            return;
        }
        if (tag === 'tr') {
            var cells = [];
            for (var i = 0; i < el.childNodes.length; i++) {
                var c = el.childNodes[i];
                if (c.nodeType === 1 && (c.tagName === 'td' || c.tagName === 'th')) {
                    cells.push(hb_trim(c._textOf()));
                }
            }
            if (cells.length) out.push('<div>' + hb_esc(cells.join(' | ')) + '</div>');
            return;
        }
        if (tag === 'table' || tag === 'thead' || tag === 'tbody' || tag === 'tfoot') {
            walkChildren(el);
            return;
        }
        if (tag === 'pre') {
            out.push('<pre>' + hb_esc(el._textOf()) + '</pre>');
            return;
        }
        if (tag === 'hr') { out.push('<hr>'); return; }
        if (tag === 'p' || tag === 'div' || tag === 'section' || tag === 'article' ||
            tag === 'header' || tag === 'footer' || tag === 'nav' || tag === 'aside' ||
            tag === 'main' || tag === 'figure' || tag === 'figcaption' ||
            tag === 'blockquote' || tag === 'address' || tag === 'center' ||
            tag === 'h1' || tag === 'h2' || tag === 'h3' || tag === 'h4' ||
            tag === 'h5' || tag === 'h6' || tag === 'dl' || tag === 'dt' ||
            tag === 'dd' || tag === 'form' || tag === 'fieldset') {
            var otag = (tag === 'div' || tag === 'section' || tag === 'article' ||
                        tag === 'header' || tag === 'footer' || tag === 'nav' ||
                        tag === 'aside' || tag === 'main' || tag === 'figure' ||
                        tag === 'figcaption' || tag === 'address' || tag === 'center' ||
                        tag === 'dl' || tag === 'form' || tag === 'fieldset') ? 'div' : tag;
            out.push(blockOpen(otag, el));
            walkChildren(el);
            out.push('</' + otag + '>');
            return;
        }
        /* 其他未知块级/行内标签：按文本处理 */
        walkChildren(el);
    }

    function walkChildren(el) {
        for (var i = 0; i < el.childNodes.length && budget > 0; i++) {
            var c = el.childNodes[i];
            if (c.nodeType === 3) {
                out.push(hb_esc(c._text));
            } else if (c.nodeType === 1) {
                walk(c);
            }
        }
    }

    var body = document.body;
    if (!body) body = document.documentElement;
    walkChildren(body);

    var s = out.join('');
    if (s.length > outCap) s = s.slice(0, outCap);
    return s;
};

/* window 是 globalThis（不是 HBNode），事件监听单独存 */
HBOS._winListeners = {};
window.addEventListener = function (type, fn) {
    if (typeof fn !== 'function') return;
    var t = hb_low(type);
    if (!HBOS._winListeners[t]) HBOS._winListeners[t] = [];
    if (HBOS._winListeners[t].indexOf(fn) < 0) HBOS._winListeners[t].push(fn);
};
window.removeEventListener = function (type, fn) {
    var t = hb_low(type);
    if (!HBOS._winListeners[t]) return;
    var i = HBOS._winListeners[t].indexOf(fn);
    if (i >= 0) HBOS._winListeners[t].splice(i, 1);
};
window.dispatchEvent = function (ev) {
    ev.target = ev.target || window;
    ev.currentTarget = window;
    var ls = HBOS._winListeners[hb_low(ev.type)];
    if (ls) {
        var copy = ls.slice();
        for (var i = 0; i < copy.length; i++) copy[i].call(window, ev);
    }
    return !ev.defaultPrevented;
};

/* ── window/document 环境装配 ── */
function hb_assemble() {
    var doc = HBOS.createDocument();
    document = doc;

    window.setTimeout = hb_setTimeout;
    window.clearTimeout = hb_clearTimeout;
    window.setInterval = hb_setInterval;
    window.clearInterval = hb_clearInterval;
    window.requestAnimationFrame = hb_requestAnimationFrame;
    window.cancelAnimationFrame = hb_cancelAnimationFrame;
    window.getComputedStyle = function (el) {
        return {
            getPropertyValue: function (k) {
                var v = el._style[hb_low(k)];
                return v === undefined ? '' : v;
            },
            display: el._style['display'] || 'block',
            visibility: 'visible',
            opacity: '1',
            position: el._style['position'] || 'static'
        };
    };
    window.scrollTo = function () { };
    window.scrollBy = function () { };
    window.alert = function () { };
    window.confirm = function () { return true; };
    window.prompt = function () { return null; };
    window.open = function () { return null; };
    window.close = function () { };
    window.focus = function () { };
    window.blur = function () { };
    window.getSelection = function () {
        return { rangeCount: 0, removeAllRanges: function () {} };
    };
    window.innerWidth = 1280;
    window.innerHeight = 720;
    window.outerWidth = 1280;
    window.outerHeight = 720;
    window.devicePixelRatio = 1;
    window.screenX = 0;
    window.screenY = 0;
    window.localStorage = { _d: {}, getItem: function (k) { return this._d[k] !== undefined ? this._d[k] : null; }, setItem: function (k, v) { this._d[k] = String(v); }, removeItem: function (k) { delete this._d[k]; }, clear: function () { this._d = {}; }, key: function (i) { var ks = Object.keys(this._d); return i < ks.length ? ks[i] : null; }, get length() { return Object.keys(this._d).length; } };
    window.sessionStorage = window.localStorage;
    function hb_parseResponse(raw, url) {
        var split = raw.indexOf('\r\n\r\n');
        var sepLen = 4;
        if (split < 0) { split = raw.indexOf('\n\n'); sepLen = 2; }
        var head = split >= 0 ? raw.slice(0, split) : '';
        var body = split >= 0 ? raw.slice(split + sepLen) : raw;
        var lines = head.split(/\r?\n/);
        var status = 0;
        var sm = /^HTTP\/\d(?:\.\d)?\s+(\d+)/i.exec(lines[0] || '');
        if (sm) status = parseInt(sm[1], 10) || 0;
        var headers = {};
        for (var i = 1; i < lines.length; i++) {
            var c = lines[i].indexOf(':');
            if (c <= 0) continue;
            var name = hb_low(hb_trim(lines[i].slice(0, c)));
            var value = hb_trim(lines[i].slice(c + 1));
            headers[name] = headers[name] ? headers[name] + ', ' + value : value;
        }
        if (hb_low(headers['transfer-encoding'] || '').indexOf('chunked') >= 0) {
            var decoded = '', pos = 0;
            while (pos < body.length) {
                var eol = body.indexOf('\r\n', pos);
                var skip = 2;
                if (eol < 0) { eol = body.indexOf('\n', pos); skip = 1; }
                if (eol < 0) break;
                var size = parseInt(hb_trim(body.slice(pos, eol)).split(';')[0], 16);
                if (!isFinite(size) || size <= 0) break;
                pos = eol + skip;
                decoded += body.slice(pos, pos + size);
                pos += size;
                if (body.slice(pos, pos + 2) === '\r\n') pos += 2;
                else if (body.charAt(pos) === '\n') pos++;
            }
            body = decoded;
        }
        var responseHeaders = {
            get: function (name) {
                var v = headers[hb_low(name)];
                return v === undefined ? null : v;
            },
            has: function (name) { return headers[hb_low(name)] !== undefined; },
            forEach: function (fn) {
                for (var k in headers) if (Object.prototype.hasOwnProperty.call(headers, k))
                    fn(headers[k], k, responseHeaders);
            }
        };
        return {
            ok: status >= 200 && status < 300,
            status: status,
            statusText: lines[0] || '',
            url: String(url),
            redirected: false,
            type: 'basic',
            headers: responseHeaders,
            bodyUsed: false,
            text: function () { this.bodyUsed = true; return Promise.resolve(body); },
            json: function () { this.bodyUsed = true; return Promise.resolve(JSON.parse(body)); },
            clone: function () { return hb_parseResponse(raw, url); },
            _bodyText: body,
            _rawHeaders: head
        };
    }
    window.fetch = function (url, init) {
        init = init || {};
        var method = hb_low(init.method || 'get');
        if (method !== 'get') return Promise.reject(new Error('fetch: only GET is supported'));
        try { return Promise.resolve(hb_parseResponse(__hbosFetchRaw(String(url)), url)); }
        catch (e) { return Promise.reject(e); }
    };
    window.XMLHttpRequest = function () {
        this.readyState = 0;
        this.status = 0;
        this.statusText = '';
        this.responseText = '';
        this.response = '';
        this.responseType = '';
        this._method = 'GET';
        this._url = '';
        this._responseHeaders = '';
        this.open = function (method, url) {
            this._method = String(method || 'GET').toUpperCase();
            this._url = String(url);
            this.readyState = 1;
            if (this.onreadystatechange) this.onreadystatechange();
        };
        this.setRequestHeader = function () { };
        this.send = function () {
            try {
                if (this._method !== 'GET') throw new Error('XHR: only GET is supported');
                var res = hb_parseResponse(__hbosFetchRaw(this._url), this._url);
                this.status = res.status;
                this.statusText = res.statusText;
                this.responseText = res._bodyText;
                this.response = this.responseType === 'json' ? JSON.parse(res._bodyText) : res._bodyText;
                this._responseHeaders = res._rawHeaders;
                this.readyState = 4;
                if (this.onreadystatechange) this.onreadystatechange();
                if (this.onload) this.onload();
            } catch (e) {
                this.readyState = 4;
                this.status = 0;
                if (this.onreadystatechange) this.onreadystatechange();
                if (this.onerror) this.onerror(e);
            }
        };
        this.abort = function () { this.readyState = 0; };
        this.getAllResponseHeaders = function () { return this._responseHeaders; };
        this.getResponseHeader = function (name) {
            var lines = this._responseHeaders.split(/\r?\n/);
            name = hb_low(name);
            for (var i = 1; i < lines.length; i++) {
                var c = lines[i].indexOf(':');
                if (c > 0 && hb_low(hb_trim(lines[i].slice(0, c))) === name)
                    return hb_trim(lines[i].slice(c + 1));
            }
            return null;
        };
    };
    window.postMessage = function () { };
    window.matchMedia = function () {
        return { matches: false, media: '', addListener: function () {}, removeListener: function () {}, addEventListener: function () {}, removeEventListener: function () {} };
    };
    window.history = { length: 1, state: null, pushState: function () {}, replaceState: function () {}, back: function () {}, forward: function () {}, go: function () {} };
    window.customElements = { define: function () {}, get: function () { return undefined; } };
    window.devicePixelRatio = 1;
    window.screen = { width: 1280, height: 720, availWidth: 1280, availHeight: 720, colorDepth: 32 };
    window.performance = { now: function () { return Date.now(); }, timing: { navigationStart: 0 } };
    window.crypto = { getRandomValues: function (arr) { for (var i = 0; i < arr.length; i++) arr[i] = (Math.random() * 256) | 0; return arr; } };
    window.ResizeObserver = function () { this.observe = function () {}; this.unobserve = function () {}; this.disconnect = function () {}; };
    window.IntersectionObserver = function () { this.observe = function () {}; this.unobserve = function () {}; this.disconnect = function () {}; };
    window.MutationObserver = function (cb) { this.observe = function () {}; this.disconnect = function () {}; this.takeRecords = function () { return []; }; };
    window.Node = HBNode;
    window.Element = HBElement;
    window.Text = HBText;
    window.Document = HBDocument;
    window.Event = HBEvent;
    /* Vue 等库会直接引用这些构造器（如 el instanceof SVGElement），
     * 必须有定义；元素本身都是 HBElement，instanceof 匹配不到也无妨。 */
    window.SVGElement = HBElement;
    window.HTMLElement = HBElement;
    window.HTMLDivElement = HBElement;
    window.MathMLElement = HBElement;
    window.location = location; /* location 由 C 侧提供，保留 */
    document.location = location;
    document.currentScript = null;
    document.baseURI = location.href;
}

/* 页面装配入口：C 侧把原始 HTML 放到 g_page_html 后调用 */
HBOS.parsePage = function (html) {
    HBOS.parseHTML(String(html), document);
    return document;
};

HBOS.pageTitle = function () {
    return document.title || '';
};

/* 装配 window/document（在 C 的 register_globals 之后立即执行） */
hb_assemble();
