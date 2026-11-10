// jsprelude.js - the page features scripts use (document, elements, events,
// timers...), built on the browser's own functions in __n (jsengine.c).
// Turned into source/jsprelude.c by tools/make_jsprelude.py.
// It evaluates to the functions the browser calls: run timers, dispatch an
// event, mark the page loaded.
(function (g) {
  'use strict';
  const N = g.__n;
  delete g.__n;

  // ------------------------------------------------------------ wrappers
  const cache = new Map();
  function W(h) {
    if (!h) return null;
    let e = cache.get(h);
    if (e) return e;
    const t = N.type(h);
    let proto = ElementProto;
    if (t === 3) proto = TextProto;
    else if (t === 9) proto = DocumentProto;
    else {
      const tag = N.tag(h);
      if (tag === 'form') proto = FormProto;
      else if (tag === 'select') proto = SelectProto;
      else if (tag === 'input' || tag === 'textarea' || tag === 'button') proto = InputProto;
      else if (tag === 'a') proto = AnchorProto;
      else if (tag === 'img') proto = ImageProto;
    }
    e = Object.create(proto);
    Object.defineProperty(e, '_h', { value: h });
    cache.set(h, e);
    return e;
  }
  const H = (e) => (e && e._h) || 0;
  const list = (arr) => { const out = []; for (let i = 0; i < arr.length; i++) out.push(W(arr[i])); out.item = (i) => out[i] || null; return out; };
  const str = (v) => (v === undefined || v === null) ? '' : String(v);
  const camel = (s) => s.replace(/-([a-z])/g, (m, c) => c.toUpperCase());
  const dash = (s) => s.replace(/[A-Z]/g, (c) => '-' + c.toLowerCase());

  // ------------------------------------------------------------ events
  function Event(type, init) {
    this.type = String(type);
    this.bubbles = !!(init && init.bubbles);
    this.cancelable = !!(init && init.cancelable);
    this.defaultPrevented = false;
    this._stop = false;
    this._stopNow = false;
    this.target = null;
    this.currentTarget = null;
    this.timeStamp = N.now();
    this.detail = init && init.detail;
  }
  Event.prototype.preventDefault = function () { this.defaultPrevented = true; this.returnValue = false; };
  Event.prototype.stopPropagation = function () { this._stop = true; this.cancelBubble = true; };
  Event.prototype.stopImmediatePropagation = function () { this._stop = this._stopNow = true; };
  function CustomEvent(type, init) { Event.call(this, type, init); }
  CustomEvent.prototype = Object.create(Event.prototype);

  const listeners = new WeakMap();
  function addL(t, type, fn, opts) {
    if (typeof fn !== 'function' && !(fn && typeof fn.handleEvent === 'function')) return;
    let m = listeners.get(t);
    if (!m) { m = {}; listeners.set(t, m); }
    const l = m[type] || (m[type] = []);
    if (l.some((x) => x.fn === fn)) return;
    l.push({ fn, once: !!(opts && typeof opts === 'object' && opts.once) });
    if (type === 'click' && t._h) N.listen(t._h);
  }
  function removeL(t, type, fn) {
    const m = listeners.get(t);
    if (!m || !m[type]) return;
    m[type] = m[type].filter((x) => x.fn !== fn);
  }
  const handlers = new WeakMap();   // el.onclick = fn
  function inlineHandler(t, type) {
    if (t === win) { const f = win['on' + type]; return typeof f === 'function' ? f : null; }
    const hm = handlers.get(t);
    if (hm && Object.prototype.hasOwnProperty.call(hm, type)) return hm[type];
    if (hm && hm['_code_' + type] !== undefined) return hm['_code_' + type];
    if (!t._h || N.type(t._h) !== 1) return null;
    const code = N.attr(t._h, 'on' + type);
    if (code === null) return null;
    let fn = null;
    try { fn = new Function('event', code); } catch (e) { console.log('handler: ' + e); }
    let m = handlers.get(t);
    if (!m) { m = {}; handlers.set(t, m); }
    m['_code_' + type] = fn;
    return fn;
  }
  function callOn(t, ev) {
    ev.currentTarget = t;
    const fn = inlineHandler(t, ev.type);
    if (typeof fn === 'function') {
      let r;
      try { r = fn.call(t, ev); } catch (e) { reportError(e); }
      if (r === false) ev.preventDefault();
    }
    const m = listeners.get(t);
    if (m && m[ev.type]) {
      for (const x of m[ev.type].slice()) {
        if (x.once) removeL(t, ev.type, x.fn);
        try {
          if (typeof x.fn === 'function') x.fn.call(t, ev); else x.fn.handleEvent(ev);
        } catch (e) { reportError(e); }
        if (ev._stopNow) break;
      }
    }
  }
  function dispatch(t, ev) {
    ev.target = t;
    const path = [];
    for (let n = t; n; n = n.parentNode) path.push(n);
    if (t !== win) path.push(win);
    for (const n of path) {
      callOn(n, ev);
      if (ev._stop || !ev.bubbles) break;
    }
    return !ev.defaultPrevented;
  }
  function reportError(e) {
    console.log('Script error: ' + (e && e.stack ? e.message + ' ' + e.stack : e));
    try { const ev = new Event('error'); ev.message = String(e && e.message || e); ev.error = e; callOn(win, ev); } catch (x) {}
  }
  const EventTarget = {
    addEventListener(type, fn, opts) { addL(this, type, fn, opts); },
    removeEventListener(type, fn) { removeL(this, type, fn); },
    dispatchEvent(ev) { return dispatch(this, ev); },
  };
  const evNames = ['click', 'dblclick', 'mousedown', 'mouseup', 'mouseover', 'mouseout', 'mousemove', 'mouseenter', 'mouseleave',
    'change', 'input', 'submit', 'reset', 'focus', 'blur', 'load', 'error', 'keydown', 'keyup', 'keypress', 'resize', 'scroll',
    'touchstart', 'touchend', 'select', 'abort'];
  function defineHandlers(proto) {
    for (const t of evNames)
      Object.defineProperty(proto, 'on' + t, {
        get() { const m = handlers.get(this); return (m && m[t]) || null; },
        set(fn) {
          let m = handlers.get(this);
          if (!m) { m = {}; handlers.set(this, m); }
          m[t] = typeof fn === 'function' ? fn : null;
          if (t === 'click' && this._h && m[t]) N.listen(this._h);
        },
        configurable: true,
      });
  }

  // ------------------------------------------------------------ nodes
  const NodeProto = Object.create(EventTarget);
  Object.defineProperties(NodeProto, {
    nodeType: { get() { return N.type(this._h); } },
    nodeName: { get() { const t = N.type(this._h); return t === 3 ? '#text' : t === 9 ? '#document' : N.tag(this._h).toUpperCase(); } },
    parentNode: { get() { const p = N.parent(this._h); return p ? W(p) : null; } },
    parentElement: { get() { const p = N.parent(this._h); return p && N.type(p) === 1 ? W(p) : null; } },
    firstChild: { get() { return W(N.first(this._h)); } },
    lastChild: { get() { return W(N.last(this._h)); } },
    nextSibling: { get() { return W(N.next(this._h)); } },
    previousSibling: { get() { return W(N.prev(this._h)); } },
    childNodes: { get() { return list(N.kids(this._h, false)); } },
    ownerDocument: { get() { return doc; } },
    isConnected: { get() { return N.connected(this._h); } },
    textContent: {
      get() { return N.text(this._h); },
      set(v) { N.setText(this._h, str(v)); },
    },
    nodeValue: {
      get() { return N.type(this._h) === 3 ? N.text(this._h) : null; },
      set(v) { if (N.type(this._h) === 3) N.setText(this._h, str(v)); },
    },
  });
  Object.assign(NodeProto, {
    appendChild(c) { if (c && c._frag) { for (const k of c.childNodes) N.append(this._h, k._h); return c; } N.append(this._h, H(c)); return c; },
    insertBefore(c, ref) {
      if (c && c._frag) { for (const k of c.childNodes) N.insert(this._h, k._h, H(ref)); return c; }
      N.insert(this._h, H(c), H(ref)); return c;
    },
    removeChild(c) { N.detach(H(c)); return c; },
    replaceChild(nc, old) { this.insertBefore(nc, old); N.detach(H(old)); return old; },
    cloneNode(deep) { return W(N.clone(this._h, !!deep)); },
    contains(o) { for (let n = o; n; n = n.parentNode) if (n === this) return true; return false; },
    hasChildNodes() { return !!N.first(this._h); },
    remove() { N.detach(this._h); },
    getRootNode() { return doc; },
    isSameNode(o) { return o === this; },
    normalize() {},
  });

  const TextProto = Object.create(NodeProto);
  Object.defineProperties(TextProto, {
    data: { get() { return N.text(this._h); }, set(v) { N.setText(this._h, str(v)); } },
    length: { get() { return N.text(this._h).length; } },
    wholeText: { get() { return N.text(this._h); } },
  });

  // classList
  function ClassList(el) { this._el = el; }
  ClassList.prototype = {
    _get() { return str(this._el.getAttribute('class')).split(/\s+/).filter(Boolean); },
    _set(a) { this._el.setAttribute('class', a.join(' ')); },
    contains(c) { return this._get().indexOf(c) >= 0; },
    add(...cs) { const a = this._get(); for (const c of cs) if (a.indexOf(c) < 0) a.push(c); this._set(a); },
    remove(...cs) { this._set(this._get().filter((x) => cs.indexOf(x) < 0)); },
    toggle(c, force) {
      const has = this.contains(c);
      const want = force === undefined ? !has : !!force;
      if (want && !has) this.add(c); else if (!want && has) this.remove(c);
      return want;
    },
    replace(a, b) { const l = this._get(); const i = l.indexOf(a); if (i < 0) return false; l[i] = b; this._set(l); return true; },
    item(i) { return this._get()[i] || null; },
    get length() { return this._get().length; },
    get value() { return str(this._el.getAttribute('class')); },
    toString() { return this.value; },
    forEach(f) { this._get().forEach(f); },
  };

  // style: reads and writes the element's style attribute
  function parseStyle(s) {
    const m = new Map();
    for (const part of str(s).split(';')) {
      const i = part.indexOf(':');
      if (i < 0) continue;
      const k = part.slice(0, i).trim().toLowerCase();
      if (k) m.set(k, part.slice(i + 1).trim());
    }
    return m;
  }
  function writeStyle(el, m) {
    let s = '';
    for (const [k, v] of m) if (v !== '') s += k + ': ' + v + '; ';
    if (s) el.setAttribute('style', s.trim()); else el.removeAttribute('style');
  }
  function styleFor(el) {
    const api = {
      getPropertyValue(k) { return parseStyle(el.getAttribute('style')).get(String(k).toLowerCase()) || ''; },
      setProperty(k, v) { const m = parseStyle(el.getAttribute('style')); m.set(String(k).toLowerCase(), str(v)); writeStyle(el, m); },
      removeProperty(k) { const m = parseStyle(el.getAttribute('style')); const o = m.get(k) || ''; m.delete(String(k).toLowerCase()); writeStyle(el, m); return o; },
      item(i) { return Array.from(parseStyle(el.getAttribute('style')).keys())[i] || ''; },
    };
    return new Proxy(api, {
      get(t, k) {
        if (typeof k !== 'string') return undefined;
        if (k in api) return api[k];
        if (k === 'cssText') return str(el.getAttribute('style'));
        if (k === 'length') return parseStyle(el.getAttribute('style')).size;
        return parseStyle(el.getAttribute('style')).get(dash(k === 'cssFloat' ? 'float' : k)) || '';
      },
      set(t, k, v) {
        if (typeof k !== 'string') return true;
        if (k === 'cssText') { if (str(v)) el.setAttribute('style', str(v)); else el.removeAttribute('style'); return true; }
        const m = parseStyle(el.getAttribute('style'));
        const name = dash(k === 'cssFloat' ? 'float' : k);
        if (v === null || v === undefined || v === '') m.delete(name); else m.set(name, typeof v === 'number' && v !== 0 && !/opacity|z-index|line-height|font-weight|flex|order/.test(name) ? v + 'px' : String(v));
        writeStyle(el, m);
        return true;
      },
    });
  }
  const styles = new WeakMap();

  const ElementProto = Object.create(NodeProto);
  defineHandlers(ElementProto);
  const attrProp = (name) => ({ get() { return str(N.attr(this._h, name)); }, set(v) { N.setAttr(this._h, name, str(v)); } });
  const boolAttr = (name) => ({ get() { return N.attr(this._h, name) !== null; }, set(v) { if (v) N.setAttr(this._h, name, ''); else N.rmAttr(this._h, name); } });
  Object.defineProperties(ElementProto, {
    tagName: { get() { return N.tag(this._h).toUpperCase(); } },
    localName: { get() { return N.tag(this._h); } },
    id: attrProp('id'),
    className: attrProp('class'),
    title: attrProp('title'),
    lang: attrProp('lang'),
    dir: attrProp('dir'),
    hidden: boolAttr('hidden'),
    classList: { get() { return new ClassList(this); } },
    style: {
      get() { let s = styles.get(this); if (!s) { s = styleFor(this); styles.set(this, s); } return s; },
      set(v) { this.setAttribute('style', str(v)); },
    },
    dataset: {
      get() {
        const el = this;
        return new Proxy({}, {
          get(t, k) { if (typeof k !== 'string') return undefined; const v = el.getAttribute('data-' + dash(k)); return v === null ? undefined : v; },
          set(t, k, v) { el.setAttribute('data-' + dash(String(k)), str(v)); return true; },
          deleteProperty(t, k) { el.removeAttribute('data-' + dash(String(k))); return true; },
          has(t, k) { return el.hasAttribute('data-' + dash(String(k))); },
          ownKeys() { return N.attrs(el._h).filter((a) => a[0].startsWith('data-')).map((a) => camel(a[0].slice(5))); },
          getOwnPropertyDescriptor(t, k) { return { enumerable: true, configurable: true, value: this.get(t, k) }; },
        });
      },
    },
    attributes: { get() { const a = N.attrs(this._h).map((x) => ({ name: x[0], value: x[1], nodeName: x[0], nodeValue: x[1] })); a.item = (i) => a[i] || null; a.getNamedItem = (n) => a.find((x) => x.name === n) || null; return a; } },
    children: { get() { return list(N.kids(this._h, true)); } },
    childElementCount: { get() { return N.kids(this._h, true).length; } },
    firstElementChild: { get() { const k = N.kids(this._h, true); return k.length ? W(k[0]) : null; } },
    lastElementChild: { get() { const k = N.kids(this._h, true); return k.length ? W(k[k.length - 1]) : null; } },
    nextElementSibling: { get() { let n = N.next(this._h); while (n && N.type(n) !== 1) n = N.next(n); return W(n); } },
    previousElementSibling: { get() { let n = N.prev(this._h); while (n && N.type(n) !== 1) n = N.prev(n); return W(n); } },
    innerHTML: { get() { return N.inner(this._h); }, set(v) { N.setInner(this._h, str(v)); } },
    outerHTML: {
      get() { return N.outer(this._h); },
      set(v) { const p = this.parentNode; if (!p) return; const t = doc.createElement('div'); t.innerHTML = str(v); while (t.firstChild) p.insertBefore(t.firstChild, this); this.remove(); },
    },
    innerText: { get() { return N.text(this._h); }, set(v) { N.setText(this._h, str(v)); } },
    outerText: { get() { return N.text(this._h); } },
    offsetWidth: { get() { return N.shown(this._h) ? 100 : 0; } },
    offsetHeight: { get() { return N.shown(this._h) ? 20 : 0; } },
    clientWidth: { get() { return N.shown(this._h) ? 100 : 0; } },
    clientHeight: { get() { return N.shown(this._h) ? 20 : 0; } },
    scrollWidth: { get() { return N.shown(this._h) ? 100 : 0; } },
    scrollHeight: { get() { return N.shown(this._h) ? 20 : 0; } },
    offsetTop: { get() { return 0; } },
    offsetLeft: { get() { return 0; } },
    offsetParent: { get() { return N.shown(this._h) ? doc.body : null; } },
    scrollTop: { get() { return 0; }, set(v) {} },
    scrollLeft: { get() { return 0; }, set(v) {} },
    tabIndex: { get() { return parseInt(N.attr(this._h, 'tabindex') || '-1', 10); }, set(v) { N.setAttr(this._h, 'tabindex', str(v)); } },
    // (for <script>, <link>, <iframe>, <source>...: the attribute, as an address)
    src: { get() { const v = N.attr(this._h, 'src'); return v === null ? '' : N.resolve(v); }, set(v) { N.setAttr(this._h, 'src', str(v)); } },
    href: { get() { const v = N.attr(this._h, 'href'); return v === null ? '' : N.resolve(v); }, set(v) { N.setAttr(this._h, 'href', str(v)); } },
    type: attrProp('type'),
    rel: attrProp('rel'),
    async: boolAttr('async'),
    defer: boolAttr('defer'),
    charset: attrProp('charset'),
    crossOrigin: attrProp('crossorigin'),
    integrity: attrProp('integrity'),
    media: attrProp('media'),
  });
  Object.assign(ElementProto, {
    getAttribute(n) { return N.attr(this._h, String(n).toLowerCase()); },
    setAttribute(n, v) { N.setAttr(this._h, String(n).toLowerCase(), str(v)); },
    removeAttribute(n) { N.rmAttr(this._h, String(n).toLowerCase()); },
    hasAttribute(n) { return N.attr(this._h, String(n).toLowerCase()) !== null; },
    toggleAttribute(n, f) { const h = this.hasAttribute(n); const w = f === undefined ? !h : !!f; if (w) this.setAttribute(n, ''); else this.removeAttribute(n); return w; },
    getAttributeNames() { return N.attrs(this._h).map((a) => a[0]); },
    getElementsByTagName(t) { return list(N.byTag(this._h, String(t).toLowerCase())); },
    getElementsByClassName(c) { return list(N.byClass(this._h, String(c))); },
    querySelector(s) { const r = N.query(this._h, String(s), true); return r.length ? W(r[0]) : null; },
    querySelectorAll(s) { return list(N.query(this._h, String(s), false)); },
    matches(s) { return N.matches(this._h, String(s)); },
    closest(s) { for (let n = this; n && n.nodeType === 1; n = n.parentNode) if (n.matches(s)) return n; return null; },
    append(...ns) { for (const n of ns) this.appendChild(typeof n === 'string' ? doc.createTextNode(n) : n); },
    prepend(...ns) { const f = this.firstChild; for (const n of ns) this.insertBefore(typeof n === 'string' ? doc.createTextNode(n) : n, f); },
    before(...ns) { const p = this.parentNode; if (p) for (const n of ns) p.insertBefore(typeof n === 'string' ? doc.createTextNode(n) : n, this); },
    after(...ns) { const p = this.parentNode; if (!p) return; const r = this.nextSibling; for (const n of ns) p.insertBefore(typeof n === 'string' ? doc.createTextNode(n) : n, r); },
    replaceWith(...ns) { this.before(...ns); this.remove(); },
    replaceChildren(...ns) { while (this.firstChild) this.firstChild.remove(); this.append(...ns); },
    insertAdjacentHTML(pos, html) {
      const t = doc.createElement('div');
      t.innerHTML = str(html);
      const kids = t.childNodes;
      pos = String(pos).toLowerCase();
      if (pos === 'beforebegin') { for (const k of kids) this.before(k); }
      else if (pos === 'afterbegin') { const first = this.firstChild; for (const k of kids) this.insertBefore(k, first); }
      else if (pos === 'beforeend') { for (const k of kids) this.appendChild(k); }
      else if (pos === 'afterend') { const r = this.nextSibling, p = this.parentNode; if (p) for (const k of kids) p.insertBefore(k, r); }
    },
    insertAdjacentElement(pos, el) {
      pos = String(pos).toLowerCase();
      if (pos === 'beforebegin') this.before(el); else if (pos === 'afterbegin') this.prepend(el);
      else if (pos === 'beforeend') this.appendChild(el); else if (pos === 'afterend') this.after(el);
      return el;
    },
    insertAdjacentText(pos, s) { this.insertAdjacentElement(pos, doc.createTextNode(str(s))); },
    click() { const ev = new Event('click', { bubbles: true, cancelable: true }); const ok = dispatch(this, ev); if (ok) N.activate(this._h); },
    focus() {}, blur() {}, scrollIntoView() {}, scrollTo() {}, scrollBy() {},
    getBoundingClientRect() { const s = N.shown(this._h); return { x: 0, y: 0, top: 0, left: 0, right: s ? 100 : 0, bottom: s ? 20 : 0, width: s ? 100 : 0, height: s ? 20 : 0 }; },
    getClientRects() { return [this.getBoundingClientRect()]; },
    animate() { return { finished: Promise.resolve(), cancel() {}, onfinish: null }; },
    attachShadow() { return this; },
  });

  const AnchorProto = Object.create(ElementProto);
  Object.defineProperties(AnchorProto, {
    href: { get() { return N.resolve(str(N.attr(this._h, 'href'))); }, set(v) { N.setAttr(this._h, 'href', str(v)); } },
    target: attrProp('target'),
    rel: attrProp('rel'),
    hash: { get() { const h = str(N.attr(this._h, 'href')); const i = h.indexOf('#'); return i >= 0 ? h.slice(i) : ''; } },
    pathname: { get() { const m = /^[a-z]+:\/\/[^/]+(\/[^?#]*)?/i.exec(this.href); return (m && m[1]) || '/'; } },
    hostname: { get() { const m = /^[a-z]+:\/\/([^/:]+)/i.exec(this.href); return m ? m[1] : ''; } },
    host: { get() { const m = /^[a-z]+:\/\/([^/]+)/i.exec(this.href); return m ? m[1] : ''; } },
    protocol: { get() { const m = /^([a-z]+:)/i.exec(this.href); return m ? m[1] : ''; } },
    search: { get() { const h = this.href.split('#')[0]; const i = h.indexOf('?'); return i >= 0 ? h.slice(i) : ''; } },
  });
  const ImageProto = Object.create(ElementProto);
  Object.defineProperties(ImageProto, {
    src: { get() { return N.resolve(str(N.attr(this._h, 'src'))); }, set(v) { N.setAttr(this._h, 'src', str(v)); } },
    alt: attrProp('alt'),
    width: { get() { return parseInt(N.attr(this._h, 'width') || '0', 10); }, set(v) { N.setAttr(this._h, 'width', str(v)); } },
    height: { get() { return parseInt(N.attr(this._h, 'height') || '0', 10); }, set(v) { N.setAttr(this._h, 'height', str(v)); } },
    complete: { get() { return true; } },
    naturalWidth: { get() { return 0; } },
    naturalHeight: { get() { return 0; } },
  });
  const InputProto = Object.create(ElementProto);
  Object.defineProperties(InputProto, {
    value: { get() { return N.value(this._h); }, set(v) { N.setValue(this._h, str(v)); } },
    defaultValue: attrProp('value'),
    checked: { get() { return N.checked(this._h); }, set(v) { N.setChecked(this._h, !!v); } },
    defaultChecked: boolAttr('checked'),
    type: { get() { return str(N.attr(this._h, 'type')).toLowerCase() || (N.tag(this._h) === 'textarea' ? 'textarea' : 'text'); }, set(v) { N.setAttr(this._h, 'type', str(v)); } },
    name: attrProp('name'),
    placeholder: attrProp('placeholder'),
    disabled: boolAttr('disabled'),
    readOnly: boolAttr('readonly'),
    required: boolAttr('required'),
    form: { get() { for (let n = this.parentNode; n; n = n.parentNode) if (n.tagName === 'FORM') return n; return null; } },
    selectionStart: { get() { return this.value.length; }, set(v) {} },
    selectionEnd: { get() { return this.value.length; }, set(v) {} },
  });
  Object.assign(InputProto, { select() {}, setSelectionRange() {}, checkValidity() { return true; }, reportValidity() { return true; }, setCustomValidity() {} });
  const SelectProto = Object.create(InputProto);
  Object.defineProperties(SelectProto, {
    options: { get() { const o = this.getElementsByTagName('option'); o.selectedIndex = this.selectedIndex; return o; } },
    selectedIndex: {
      get() { const o = this.getElementsByTagName('option'); const v = this.value; for (let i = 0; i < o.length; i++) if (optVal(o[i]) === v) return i; return o.length ? 0 : -1; },
      set(i) { const o = this.getElementsByTagName('option'); if (o[i]) this.value = optVal(o[i]); },
    },
    length: { get() { return this.getElementsByTagName('option').length; } },
    selectedOptions: { get() { const i = this.selectedIndex; return i >= 0 ? [this.options[i]] : []; } },
  });
  const optVal = (o) => { const v = o.getAttribute('value'); return v === null ? o.textContent.trim() : v; };
  Object.defineProperty(ElementProto, 'selected', { get() { return this.tagName === 'OPTION' && this.parentNode && this.parentNode.value === optVal(this); }, set(v) { if (v && this.parentNode) this.parentNode.value = optVal(this); } });
  Object.defineProperty(ElementProto, 'text', { get() { return this.textContent; }, set(v) { this.textContent = v; } });
  const FormProto = Object.create(ElementProto);
  Object.defineProperties(FormProto, {
    elements: { get() { const l = this.querySelectorAll('input,select,textarea,button'); for (const e of l) { const n = e.getAttribute('name'); if (n && !(n in l)) l[n] = e; } return l; } },
    action: { get() { return N.resolve(str(N.attr(this._h, 'action'))); }, set(v) { N.setAttr(this._h, 'action', str(v)); } },
    method: attrProp('method'),
    name: attrProp('name'),
    length: { get() { return this.elements.length; } },
  });
  Object.assign(FormProto, {
    submit() { N.submit(this._h); },
    requestSubmit() { if (dispatch(this, new Event('submit', { bubbles: true, cancelable: true }))) N.submit(this._h); },
    reset() {},
    checkValidity() { return true; },
    reportValidity() { return true; },
  });

  // ------------------------------------------------------------ document
  const DocumentProto = Object.create(NodeProto);
  defineHandlers(DocumentProto);
  const doc = W(N.doc());
  const find = (tag) => { const r = N.byTag(N.doc(), tag); return r.length ? W(r[0]) : null; };
  let readyState = 'loading';
  Object.defineProperties(DocumentProto, {
    documentElement: { get() { return find('html') || doc; } },
    body: { get() { return find('body') || find('html') || doc; } },
    head: { get() { return find('head') || find('html') || doc; } },
    title: { get() { return N.title(); }, set(v) { N.setTitle(str(v)); } },
    cookie: { get() { return N.cookie(); }, set(v) { N.setCookie(str(v)); } },
    readyState: { get() { return readyState; } },
    URL: { get() { return N.url(); } },
    documentURI: { get() { return N.url(); } },
    baseURI: { get() { return N.url(); } },
    domain: { get() { return loc.hostname; } },
    referrer: { get() { return ''; } },
    location: { get() { return loc; }, set(v) { loc.href = v; } },
    forms: { get() { return this.getElementsByTagName('form'); } },
    images: { get() { return this.getElementsByTagName('img'); } },
    links: { get() { return this.querySelectorAll('a[href]'); } },
    scripts: { get() { return this.getElementsByTagName('script'); } },
    activeElement: { get() { return this.body; } },
    defaultView: { get() { return win; } },
    hidden: { get() { return false; } },
    visibilityState: { get() { return 'visible'; } },
    characterSet: { get() { return 'UTF-8'; } },
    compatMode: { get() { return 'CSS1Compat'; } },
    currentScript: { get() { return W(N.currentScript()); } },
    documentMode: { get() { return undefined; } },
    implementation: { get() { return impl; } },
  });
  // a separate little document (jQuery builds HTML in one)
  const impl = {
    hasFeature() { return true; },
    createHTMLDocument(title) {
      const html = doc.createElement('html'), head = doc.createElement('head'), body = doc.createElement('body');
      html.appendChild(head); html.appendChild(body);
      if (title) { const t = doc.createElement('title'); t.textContent = title; head.appendChild(t); }
      return { nodeType: 9, documentElement: html, head, body, implementation: impl, defaultView: null,
        createElement: (t) => doc.createElement(t), createTextNode: (s) => doc.createTextNode(s),
        createDocumentFragment: () => doc.createDocumentFragment(), createComment: () => doc.createComment(),
        getElementById: (id) => html.querySelector('#' + id), getElementsByTagName: (t) => html.getElementsByTagName(t),
        querySelector: (s) => html.querySelector(s), querySelectorAll: (s) => html.querySelectorAll(s),
        addEventListener() {}, removeEventListener() {} };
    },
  };
  Object.assign(DocumentProto, {
    getElementById(id) { return W(N.byId(String(id))); },
    getElementsByTagName(t) { return list(N.byTag(N.doc(), String(t).toLowerCase())); },
    getElementsByClassName(c) { return list(N.byClass(N.doc(), String(c))); },
    getElementsByName(n) { return list(N.query(N.doc(), '[name="' + String(n).replace(/"/g, '') + '"]', false)); },
    querySelector(s) { const r = N.query(N.doc(), String(s), true); return r.length ? W(r[0]) : null; },
    querySelectorAll(s) { return list(N.query(N.doc(), String(s), false)); },
    createElement(t) { return W(N.create(String(t))); },
    createElementNS(ns, t) { return W(N.create(String(t))); },
    createTextNode(s) { return W(N.createText(str(s))); },
    createComment() { return W(N.createText('')); },
    createDocumentFragment() { const f = W(N.create('div')); Object.defineProperty(f, '_frag', { value: true }); return f; },
    createEvent() { return new Event(''); },
    write(...s) { N.write(s.join('')); },
    writeln(...s) { N.write(s.join('') + '\n'); },
    open() {}, close() {},
    hasFocus() { return true; },
    execCommand() { return false; },
    elementFromPoint() { return null; },
  });
  Event.prototype.initEvent = function (t, b, c) { this.type = t; this.bubbles = !!b; this.cancelable = !!c; };

  // ------------------------------------------------------------ window
  const win = g;
  Object.assign(win, EventTarget);
  defineHandlers(win);
  win.window = win;
  win.self = win;
  win.top = win;
  win.parent = win;
  win.frames = win;
  win.globalThis = win;
  win.document = doc;
  win.Event = Event;
  win.CustomEvent = CustomEvent;
  win.MouseEvent = Event;
  win.KeyboardEvent = Event;
  win.UIEvent = Event;
  win.FocusEvent = Event;
  win.HTMLElement = function HTMLElement() {};
  win.HTMLElement.prototype = ElementProto;
  win.Element = win.HTMLElement;
  win.Node = function Node() {};
  win.Node.prototype = NodeProto;
  Object.assign(win.Node, { ELEMENT_NODE: 1, TEXT_NODE: 3, DOCUMENT_NODE: 9 });
  win.Text = function Text() {};
  win.Text.prototype = TextProto;
  win.HTMLDocument = win.Document = function Document() {};
  win.Document.prototype = DocumentProto;
  win.HTMLFormElement = function () {}; win.HTMLFormElement.prototype = FormProto;
  win.HTMLInputElement = function () {}; win.HTMLInputElement.prototype = InputProto;
  win.HTMLSelectElement = function () {}; win.HTMLSelectElement.prototype = SelectProto;
  win.HTMLAnchorElement = function () {}; win.HTMLAnchorElement.prototype = AnchorProto;
  win.HTMLImageElement = function () {}; win.HTMLImageElement.prototype = ImageProto;
  win.Image = function Image(w, h) { const i = doc.createElement('img'); if (w) i.width = w; if (h) i.height = h; return i; };
  win.Option = function Option(t, v) { const o = doc.createElement('option'); if (t !== undefined) o.textContent = t; if (v !== undefined) o.setAttribute('value', v); return o; };

  const nav = { userAgent: N.ua(), appName: 'Netscape', appVersion: '5.0', platform: 'GameCube', language: 'en-US', languages: ['en-US', 'en'],
    cookieEnabled: true, onLine: true, vendor: '', product: 'Gecko', maxTouchPoints: 0, hardwareConcurrency: 1,
    javaEnabled() { return false; }, sendBeacon(u) { netNote(u); return true; } };
  win.navigator = nav;
  win.screen = { width: 640, height: 480, availWidth: 640, availHeight: 480, colorDepth: 24, pixelDepth: 24, orientation: { type: 'landscape-primary', angle: 0 } };
  win.innerWidth = 480; win.innerHeight = 640; win.outerWidth = 640; win.outerHeight = 480;
  win.devicePixelRatio = 1; win.scrollX = win.scrollY = win.pageXOffset = win.pageYOffset = 0;
  win.scrollTo = win.scroll = win.scrollBy = win.focus = win.blur = win.print = function () {};
  win.open = function (u) { if (u) loc.assign(u); return null; };
  win.close = function () {};
  win.alert = function (m) { N.alert(str(m)); };
  win.confirm = function (m) { N.alert(str(m)); return true; };
  win.prompt = function () { return null; };
  win.getComputedStyle = function (el) {
    const s = el && el.style ? el.style : {};
    return new Proxy({}, { get(t, k) {
      if (k === 'getPropertyValue') return (p) => (p === 'display' ? (N.shown(el._h) ? (s.display || 'block') : 'none') : (s.getPropertyValue ? s.getPropertyValue(p) : ''));
      if (k === 'display') return N.shown(el._h) ? (s.display || 'block') : 'none';
      if (k === 'visibility') return s.visibility || 'visible';
      return s[k] || '';
    } });
  };
  win.matchMedia = function (q) { return { matches: /max-width:\s*(\d+)/.test(q) ? parseInt(RegExp.$1, 10) >= 480 : false, media: q,
    addListener() {}, removeListener() {}, addEventListener() {}, removeEventListener() {} }; };
  win.requestIdleCallback = (fn) => setTimeout(() => fn({ didTimeout: false, timeRemaining: () => 10 }), 1);
  win.cancelIdleCallback = (id) => clearTimeout(id);

  // console
  const fmt = (a) => a.map((x) => { try { return typeof x === 'object' ? JSON.stringify(x) : String(x); } catch (e) { return String(x); } }).join(' ');
  win.console = { log: (...a) => N.log(fmt(a)), info: (...a) => N.log(fmt(a)), warn: (...a) => N.log(fmt(a)), error: (...a) => N.log(fmt(a)),
    debug() {}, trace() {}, dir() {}, group() {}, groupEnd() {}, groupCollapsed() {}, time() {}, timeEnd() {}, table() {}, assert() {}, count() {} };

  // location
  const loc = {};
  const part = (re, i, d) => { const m = re.exec(N.url()); return (m && m[i]) || d; };
  Object.defineProperties(loc, {
    href: { get() { return N.url(); }, set(v) { N.go(N.resolve(str(v)), false); }, enumerable: true },
    protocol: { get() { return part(/^([a-z]+:)/i, 1, ''); } },
    host: { get() { return part(/^[a-z]+:\/\/([^/?#]+)/i, 1, ''); } },
    hostname: { get() { return part(/^[a-z]+:\/\/([^/?#:]+)/i, 1, ''); } },
    port: { get() { return part(/^[a-z]+:\/\/[^/?#:]+:(\d+)/i, 1, ''); } },
    pathname: { get() { return part(/^[a-z]+:\/\/[^/?#]+([^?#]*)/i, 1, '/') || '/'; }, set(v) { N.go(N.resolve(str(v)), false); } },
    search: { get() { return part(/^[^?#]*(\?[^#]*)/, 1, ''); }, set(v) { N.go(N.url().split(/[?#]/)[0] + (String(v)[0] === '?' ? '' : '?') + v, false); } },
    hash: { get() { return part(/(#.*)$/, 1, ''); }, set(v) {} },
    origin: { get() { return part(/^([a-z]+:\/\/[^/?#]+)/i, 1, ''); } },
  });
  Object.assign(loc, {
    assign(u) { N.go(N.resolve(str(u)), false); },
    replace(u) { N.go(N.resolve(str(u)), true); },
    reload() { N.go(N.url(), true); },
    toString() { return N.url(); },
  });
  Object.defineProperty(win, 'location', { get() { return loc; }, set(v) { loc.href = v; }, configurable: true });
  win.history = { length: 1, state: null, back() { N.back(); }, go(n) { if (n < 0) N.back(); }, forward() {},
    pushState(s) { this.state = s; }, replaceState(s) { this.state = s; } };

  // storage (kept for this visit only)
  function Storage() { const m = new Map(); return {
    getItem: (k) => (m.has(String(k)) ? m.get(String(k)) : null), setItem: (k, v) => { m.set(String(k), String(v)); },
    removeItem: (k) => { m.delete(String(k)); }, clear: () => m.clear(), key: (i) => Array.from(m.keys())[i] || null,
    get length() { return m.size; } }; }
  win.localStorage = Storage();
  win.sessionStorage = Storage();

  // timers
  let timers = [];
  let nextId = 1;
  // tells the browser when the next timer is due (it calls runTimers then)
  function due() { let m = Infinity; for (const t of timers) if (t.at < m) m = t.at; N.timers(timers.length > 0, m === Infinity ? 0 : m); }
  function addTimer(fn, ms, args, repeat) {
    if (typeof fn === 'string') { const code = fn; fn = () => (0, eval)(code); }
    if (typeof fn !== 'function') return 0;
    ms = Math.max(4, Number(ms) || 0);
    const id = nextId++;
    timers.push({ id, fn, args, at: N.now() + ms, ms, repeat });
    due();
    return id;
  }
  win.setTimeout = (fn, ms, ...a) => addTimer(fn, ms, a, false);
  win.setInterval = (fn, ms, ...a) => addTimer(fn, Math.max(ms || 0, 16), a, true);
  win.clearTimeout = win.clearInterval = (id) => { const n = timers.length; timers = timers.filter((t) => t.id !== id); if (timers.length !== n) due(); };
  win.requestAnimationFrame = (fn) => addTimer(() => fn(N.now()), 33, [], false);
  win.cancelAnimationFrame = win.clearTimeout;
  win.queueMicrotask = (fn) => Promise.resolve().then(fn);
  function runTimers(now) {
    const ready = timers.filter((t) => t.at <= now).sort((a, b) => a.at - b.at);
    for (const t of ready) {
      if (t.repeat) t.at = now + t.ms; else timers = timers.filter((x) => x !== t);
      try { t.fn.apply(win, t.args); } catch (e) { reportError(e); }
    }
    due();
  }

  win.performance = { now: () => N.now(), timing: { navigationStart: Date.now() }, mark() {}, measure() {}, getEntriesByType() { return []; }, getEntriesByName() { return []; } };

  // base64
  const B64 = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
  win.btoa = (s) => { s = String(s); let o = ''; for (let i = 0; i < s.length; i += 3) { const a = s.charCodeAt(i), b = s.charCodeAt(i + 1), c = s.charCodeAt(i + 2);
    o += B64[a >> 2] + B64[((a & 3) << 4) | (b >> 4 || 0)] + (i + 1 < s.length ? B64[((b & 15) << 2) | (c >> 6 || 0)] : '=') + (i + 2 < s.length ? B64[c & 63] : '='); } return o; };
  win.atob = (s) => { s = String(s).replace(/[^A-Za-z0-9+/]/g, ''); let o = ''; for (let i = 0; i < s.length; i += 4) { const a = B64.indexOf(s[i]), b = B64.indexOf(s[i + 1]), c = B64.indexOf(s[i + 2]), d = B64.indexOf(s[i + 3]);
    o += String.fromCharCode((a << 2) | (b >> 4)); if (c >= 0 && s[i + 2]) o += String.fromCharCode(((b & 15) << 4) | (c >> 2)); if (d >= 0 && s[i + 3]) o += String.fromCharCode(((c & 3) << 6) | d); } return o; };

  // network: not yet (scripts that ask get a polite failure)
  function XMLHttpRequest() { this.readyState = 0; this.status = 0; this.responseText = ''; this.response = null; this._h = {}; }
  XMLHttpRequest.prototype = Object.assign(Object.create(EventTarget), {
    open(m, u) { this.readyState = 1; this._u = String(u); }, setRequestHeader() {}, getResponseHeader() { return null; }, getAllResponseHeaders() { return ''; },
    overrideMimeType() {}, abort() {},
    send() { const x = this; netNote(x._u); setTimeout(() => { x.readyState = 4; x.status = 0;
      if (x.onreadystatechange) try { x.onreadystatechange(); } catch (e) { reportError(e); }
      if (x.onerror) try { x.onerror(new Event('error')); } catch (e) { reportError(e); }
      callOn(x, new Event('error')); callOn(x, new Event('loadend')); }, 1); },
  });
  XMLHttpRequest.UNSENT = 0; XMLHttpRequest.DONE = 4;
  win.XMLHttpRequest = XMLHttpRequest;
  // (noted in about:console, so a page that waits on one can be explained)
  function netNote(u) { N.note('A script tried to download ' + String(u || '?').slice(0, 120) + ' (scripts can\'t download data yet)'); }
  win.fetch = (u) => { netNote(u && u.url ? u.url : u); return Promise.reject(new TypeError('Network requests from scripts are not supported yet')); };
  win.WebSocket = function () { throw new Error('WebSocket is not supported'); };
  win.MutationObserver = function () { return { observe() {}, disconnect() {}, takeRecords() { return []; } }; };
  win.IntersectionObserver = function (cb) { return { observe(el) { setTimeout(() => cb([{ target: el, isIntersecting: true, intersectionRatio: 1 }], this), 1); }, unobserve() {}, disconnect() {} }; };
  win.ResizeObserver = function () { return { observe() {}, unobserve() {}, disconnect() {} }; };
  win.URLSearchParams = function (s) {
    const m = [];
    String(s || '').replace(/^\?/, '').split('&').forEach((p) => { if (!p) return; const i = p.indexOf('='); m.push([decodeURIComponent((i < 0 ? p : p.slice(0, i)).replace(/\+/g, ' ')), i < 0 ? '' : decodeURIComponent(p.slice(i + 1).replace(/\+/g, ' '))]); });
    return { get: (k) => { const e = m.find((x) => x[0] === k); return e ? e[1] : null; }, has: (k) => m.some((x) => x[0] === k),
      getAll: (k) => m.filter((x) => x[0] === k).map((x) => x[1]), set(k, v) { const e = m.find((x) => x[0] === k); if (e) e[1] = String(v); else m.push([k, String(v)]); },
      append(k, v) { m.push([k, String(v)]); }, delete(k) { for (let i = m.length - 1; i >= 0; i--) if (m[i][0] === k) m.splice(i, 1); },
      toString: () => m.map((x) => encodeURIComponent(x[0]) + '=' + encodeURIComponent(x[1])).join('&'), forEach(f) { m.forEach((x) => f(x[1], x[0])); } };
  };

  // ------------------------------------------------------------ for the browser
  return {
    timers: runTimers,
    // an event on an element (from the browser): true if the page stopped
    // what the browser would do (followed a link, sent a form...)
    dispatch(h, type) {
      const t = W(h);
      if (!t) return false;
      const ev = new Event(type, { bubbles: type !== 'focus' && type !== 'blur' && type !== 'load' && type !== 'error', cancelable: true });
      const ok = dispatch(t, ev);
      if (type === 'click') {
        for (let n = t; n; n = n.parentNode) {
          if (n.tagName === 'A') {
            const href = str(n.getAttribute('href'));
            if (/^javascript:/i.test(href)) {
              if (ok) { try { (0, eval)(decodeURIComponent(href.slice(11))); } catch (e) { reportError(e); } }
              return true;
            }
            break;
          }
        }
      }
      return !ok;
    },
    // the page has finished: DOMContentLoaded, then load
    loaded() {
      readyState = 'interactive';
      dispatch(doc, new Event('DOMContentLoaded', { bubbles: true }));
      readyState = 'complete';
      dispatch(doc, new Event('readystatechange'));
      const body = doc.body;
      if (body && body !== doc && body._h && N.attr(body._h, 'onload') !== null) {
        const ev = new Event('load');
        ev.target = doc;
        callOn(body, ev);
      }
      const ev = new Event('load');
      ev.target = doc;
      callOn(win, ev);
    },
    // a script's error (for the log)
    error: reportError,
  };
})(globalThis)
