/* dashboard.js - scout-dashboard 交互层（V7 S3c，vanilla 零依赖零构建）
 *
 * 数据装载：C++ 注入的 script[type=application/json]#dashboard-data
 * （精简字段名，embedJsonSafe 已封死脚本标签提前闭合注入面；本文件只消费）。
 * 注意：本文件整体进入 HTML script 块，任何注释/字符串禁止出现脚本闭合
 * 标签字面序列（parser 不管它是否在 JS 注释里）——已用「脚本闭合标签」指代。
 * JSON.parse 异常 → 静默退出，页面回落为纯静态表格（双形态共存）。
 *
 * 能力边界（明确不做：图表库/动画/导出/虚拟滚动）：
 *   时间轴泳道（只画 freeze span：recovered 实心蓝 / ongoing 右端开放条纹 /
 *   lost 灰）、缩放平移（wheel 以光标为中心 0.2x–50x + drag + dblclick 复位，
 *   改 transform 不重排）、悬停详情（事件委托 + data-*）、筛选（EventClass +
 *   receiver 复选）、明细分页（100 条/页 innerHTML 拼串）。
 *
 * XSS 分工：C++ 静态位 escapeHtml；JSON 内嵌位只防 "</"（embedJsonSafe）；
 * 本文件任何 innerHTML 拼串必经 esc()（receiver/url 可能含载荷）。
 */
(function () {
	'use strict';

	var node = document.getElementById('dashboard-data');
	if (!node) return;
	var data;
	try {
		data = JSON.parse(node.textContent);
	} catch (e) {
		return;									// 降级静态表
	}

	// 内嵌 JSON 数组位索引（C++ buildEmbedJson 同步约定）
	var E = { CLS: 0, REL: 1, COST: 2, THR: 3, ST: 4, RC: 5, EV: 6,
			  URL: 7, SRC: 8, TM: 9 };
	var S = { RC: 0, START: 1, DUR: 2, END: 3 };
	var CLS_NAMES = ['Freeze', 'CdpLongTask', 'CpuSpin', 'SlowEvent',
					 'MetaCall', 'Qss', 'Other'];
	var CLS_BADGES = ['badge-freeze', 'badge-cdp', 'badge-cpu',
					  'badge-slowEvent', 'badge-metaCall', 'badge-qss',
					  'badge-other'];
	var SPAN_NAMES = { recovered: '已恢复', lost: '目标丢失', ongoing: '进行中' };

	function esc(s) {
		return String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;')
						.replace(/>/g, '&gt;').replace(/"/g, '&quot;');
	}
	function $(id) { return document.getElementById(id); }

	// ---------- 数据展平 ----------
	var all = [];								// { pid, e[10] }
	(data.ss || []).forEach(function (ss) {
		(ss.ev || []).forEach(function (e) {
			all.push({ pid: ss.p, e: e });
		});
	});
	all.sort(function (a, b) {					// time 倒序（同日字符串序）
		return b.e[E.TM].localeCompare(a.e[E.TM]);
	});

	var receivers = [];
	all.forEach(function (x) {
		var rc = x.e[E.RC];
		if (rc && receivers.indexOf(rc) < 0) receivers.push(rc);
	});
	receivers.sort();

	// ---------- 筛选状态 ----------
	var clsOff = {};							// cls index -> 隐藏?
	var rcOff = {};								// receiver -> 隐藏?
	function visible(x) {
		return !clsOff[x.e[E.CLS]] && !rcOff[x.e[E.RC]];
	}

	// ---------- 筛选器 ----------
	function buildFilters() {
		var host = $('dash-filters');
		if (!host) return;
		var html = '<span class="ft-group">类型：</span>';
		CLS_NAMES.forEach(function (n, i) {
			html += '<label class="ft-chk"><input type="checkbox" checked '
				  + 'data-cls="' + i + '"> ' + n + '</label>';
		});
		if (receivers.length > 1) {
			html += '<span class="ft-group">目标：</span>';
			receivers.forEach(function (r) {
				html += '<label class="ft-chk"><input type="checkbox" checked '
					  + 'data-rc="' + esc(r) + '"> ' + esc(r) + '</label>';
			});
		}
		host.innerHTML = html;
		host.addEventListener('change', function (ev) {
			var t = ev.target;
			if (t.getAttribute('data-cls') !== null)
				clsOff[t.getAttribute('data-cls')] = !t.checked;
			if (t.getAttribute('data-rc') !== null)
				rcOff[t.getAttribute('data-rc')] = !t.checked;
			renderTimeline();
			page = 1;
			renderPage();
		});
	}

	// ---------- 冻结时间轴（缩放/平移改 transform，不重排） ----------
	var scale = 1, tx = 0;

	function maxRel() {
		var m = 1;
		(data.ss || []).forEach(function (ss) {
			(ss.ev || []).forEach(function (e) {
				if (e[E.REL] > m) m = e[E.REL];
			});
		});
		return m;
	}

	function renderTimeline() {
		var canvas = $('ft-canvas');
		if (!canvas) return;
		if (clsOff[0]) {					// Freeze 类型被筛选隐藏 → 时间轴联动清空
			canvas.innerHTML = '<p class="muted" style="padding:0 12px">'
				+ 'Freeze 类型已被筛选隐藏</p>';
			return;
		}
		var mr = maxRel();
		var lanes = [];
		(data.ss || []).forEach(function (ss) {
			var byRc = {};
			(ss.sp || []).forEach(function (sp) {
				if (rcOff[sp[S.RC]]) return;
				(byRc[sp[S.RC]] = byRc[sp[S.RC]] || []).push(sp);
			});
			Object.keys(byRc).forEach(function (rc) {
				lanes.push({ pid: ss.p, rc: rc, spans: byRc[rc] });
			});
		});
		var html = '';
		lanes.forEach(function (ln) {
			var spans = '';
			ln.spans.forEach(function (sp) {
				var left = sp[S.START] / mr * 100;
				var w = sp[S.DUR] < 0
					? (mr - sp[S.START]) / mr * 100		// ongoing：画到视口末端
					: sp[S.DUR] / mr * 100;
				var tip = esc(ln.rc) + ' · ' + SPAN_NAMES[sp[S.END]]
						+ (sp[S.DUR] < 0 ? '（未闭合）'
										 : ' ' + sp[S.DUR] + 'ms')
						+ ' · rel ' + sp[S.START] + 'ms';
				spans += '<div class="ft-span ft-' + sp[S.END]
					   + '" style="left:' + left + '%;width:'
					   + Math.max(0.3, w) + '%" data-tip="' + tip
					   + '"></div>';
			});
			html += '<div class="ft-lane"><div class="ft-label">' + esc(ln.rc)
				  + ' <span class="muted">pid ' + ln.pid + '</span></div>'
				  + '<div class="ft-track"><div class="ft-inner">' + spans
				  + '</div></div></div>';
		});
		canvas.innerHTML = lanes.length
			? html
			: '<p class="muted" style="padding:0 12px">无可显示时段'
			+ '（无冻结记录或被筛选）</p>';
		applyTransform();
	}

	function applyTransform() {
		var nodes = document.querySelectorAll('.ft-inner');
		for (var i = 0; i < nodes.length; ++i) {
			nodes[i].style.transform =
				'translateX(' + tx + 'px) scale(' + scale + ',1)';
		}
	}

	function clampTx() {
		var view = $('ft-view');
		if (!view) return;
		var min = view.clientWidth - view.clientWidth * scale;
		if (tx > 0) tx = 0;
		if (tx < min) tx = min;
	}

	function bindTimeline() {
		var view = $('ft-view');
		if (!view) return;
		view.addEventListener('wheel', function (ev) {
			ev.preventDefault();
			var rect = view.getBoundingClientRect();
			var px = ev.clientX - rect.left;
			var factor = ev.deltaY < 0 ? 1.2 : 1 / 1.2;
			var ns = Math.min(50, Math.max(0.2, scale * factor));
			tx = px - (px - tx) * (ns / scale);		// 光标为中心
			scale = ns;
			clampTx();
			applyTransform();
		}, { passive: false });
		var drag = null;
		view.addEventListener('mousedown', function (ev) {
			drag = { x: ev.clientX, tx: tx };
		});
		window.addEventListener('mousemove', function (ev) {
			if (!drag) return;
			tx = drag.tx + (ev.clientX - drag.x);
			clampTx();
			applyTransform();
		});
		window.addEventListener('mouseup', function () { drag = null; });
		view.addEventListener('dblclick', function () {
			scale = 1;
			tx = 0;
			applyTransform();
		});
	}

	// 悬停详情（事件委托 + data-*）
	function bindTip() {
		var tip = $('ft-tip');
		if (!tip) return;
		document.addEventListener('mouseover', function (ev) {
			var t = ev.target;
			while (t && t !== document && !t.classList.contains('ft-span'))
				t = t.parentNode;
			if (t && t !== document && t.dataset && t.dataset.tip) {
				tip.textContent = t.dataset.tip;
				tip.hidden = false;
			} else {
				tip.hidden = true;
			}
		});
	}

	// ---------- 交互明细分页（100 条/页） ----------
	var PAGE_SIZE = 100;
	var page = 1;

	function filteredList() {
		return all.filter(visible);
	}

	function renderPage() {
		var body = $('dash-pagebody');
		if (!body) return;
		var list = filteredList();
		var pages = Math.max(1, Math.ceil(list.length / PAGE_SIZE));
		if (page > pages) page = pages;
		var begin = (page - 1) * PAGE_SIZE;
		var html = '';
		for (var i = begin; i < Math.min(list.length, begin + PAGE_SIZE); ++i) {
			var x = list[i], e = x.e, c = e[E.CLS];
			html += '<tr><td class="mono">' + esc(e[E.TM]) + '</td>'
				  + '<td class="mono">' + x.pid + '</td>'
				  + '<td><span class="badge '
				  + (CLS_BADGES[c] || 'badge-other') + '">'
				  + (CLS_NAMES[c] || 'Other') + '</span></td>'
				  + '<td class="mono">' + esc(e[E.RC] || '-') + '</td>'
				  + '<td>' + esc(e[E.EV] || '-') + '</td>'
				  + '<td>' + esc(e[E.SRC] || '-') + '</td>'
				  + '<td class="num">' + Number(e[E.COST]).toFixed(1) + '</td>'
				  + '<td class="mono" title="' + esc(e[E.URL] || '') + '">'
				  + esc(e[E.URL] || '-') + '</td></tr>';
		}
		body.innerHTML = html
			|| '<tr><td colspan="8" class="muted">无匹配记录</td></tr>';
		var bar = $('dash-pagebar');
		if (bar) {
			bar.innerHTML = '共 ' + list.length + ' 条 · 第 ' + page + ' / '
				+ pages + ' 页 '
				+ '<button data-pg="prev"' + (page <= 1 ? ' disabled' : '')
				+ '>‹ 上一页</button>'
				+ '<button data-pg="next"' + (page >= pages ? ' disabled' : '')
				+ '>下一页 ›</button>';
		}
	}

	function bindPagebar() {
		document.addEventListener('click', function (ev) {
			var t = ev.target;
			while (t && t !== document && t.getAttribute
				   && t.getAttribute('data-pg') === null)
				t = t.parentNode;
			if (!t || t === document) return;
			var pg = t.getAttribute('data-pg');
			if (pg === 'prev' && page > 1) { --page; renderPage(); }
			if (pg === 'next') { ++page; renderPage(); }
		});
	}

	// ---------- 装配 ----------
	buildFilters();
	renderTimeline();
	bindTimeline();
	bindTip();
	renderPage();
	bindPagebar();
})();
