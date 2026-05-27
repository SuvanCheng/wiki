(function () {
  'use strict';

  var searchInput = document.getElementById('searchInput');
  var searchStatus = document.getElementById('searchStatus');
  var cardContainer = document.getElementById('cardContainer');
  var lightCSS = document.getElementById('github-md-light');
  var darkCSS = document.getElementById('github-md-dark');

  var lockIcon = document.getElementById('lockIcon');
  var lockLabel = document.getElementById('lockLabel');
  var lockInputRow = document.getElementById('lockInputRow');
  var lockSecretInput = document.getElementById('lockSecretInput');
  var btnUnlock = document.getElementById('btnUnlock');
  var internalStatRow = document.getElementById('internalStatRow');
  var starredStatRow = document.getElementById('starredStatRow');
  var statsContent = document.getElementById('statsContent');
  var catList = document.getElementById('catList');
  var recentList = document.getElementById('recentList');
  var dbAdminSection = document.getElementById('dbAdminSection');
  var dbSelectorSection = document.getElementById('dbSelectorSection');
  var versionText = document.getElementById('versionText');

  var btnAdd = document.getElementById('btnAdd');
  var btnStarFilter = document.getElementById('btnStarFilter');
  var btnAllDB = document.getElementById('btnAllDB');

  var btnExport = document.getElementById('btnExport');
  var btnImport = document.getElementById('btnImport');
  var importMode = document.getElementById('importMode');
  var conflictRow = document.getElementById('conflictRow');
  var conflictStrategy = document.getElementById('conflictStrategy');
  var dbFileInput = document.getElementById('dbFileInput');
  var importStatus = document.getElementById('importStatus');

  var dbSelector = document.getElementById('dbSelector');
  var btnDBRename = document.getElementById('btnDBRename');
  var btnDBLoad = document.getElementById('btnDBLoad');
  var btnDBRemove = document.getElementById('btnDBRemove');

  var editModal = document.getElementById('editModal');
  var modalTitle = document.getElementById('modalTitle');
  var editForm = document.getElementById('editForm');
  var editId = document.getElementById('editId');
  var editQuestion = document.getElementById('editQuestion');
  var editCategory = document.getElementById('editCategory');
  var editVisibility = document.getElementById('editVisibility');
  var editAuthor = document.getElementById('editAuthor');
  var editAnswer = document.getElementById('editAnswer');
  var editPreview = document.getElementById('editPreview');
  var btnModalClose = document.getElementById('btnModalClose');
  var btnModalCancel = document.getElementById('btnModalCancel');

  var btnUpload = document.getElementById('btnUpload');
  var btnPreview = document.getElementById('btnPreview');
  var fieldPreview = document.getElementById('fieldPreview');
  var fileInput = document.getElementById('fileInput');
  var uploadStatus = document.getElementById('uploadStatus');

  // Conflict modal
  var conflictModal = document.getElementById('conflictModal');
  var conflictMsg = document.getElementById('conflictMsg');
  var conflictNewName = document.getElementById('conflictNewName');
  var btnConflictClose = document.getElementById('btnConflictClose');
  var btnConflictCancel = document.getElementById('btnConflictCancel');
  var btnConflictConfirm = document.getElementById('btnConflictConfirm');

  // DB load modal
  var dbLoadModal = document.getElementById('dbLoadModal');
  var dbLoadPath = document.getElementById('dbLoadPath');
  var dbLoadName = document.getElementById('dbLoadName');
  var dbLoadStatus = document.getElementById('dbLoadStatus');
  var btnDBLoadClose = document.getElementById('btnDBLoadClose');
  var btnDBLoadCancel = document.getElementById('btnDBLoadCancel');
  var btnDBLoadConfirm = document.getElementById('btnDBLoadConfirm');

  var debounceTimer = null;
  var previewVisible = false;
  var formDirty = false;

  // State
  var allDBMode = false;
  var starFilter = false;
  var dbs = [];
  var currentDBName = '';
  var editingDBSource = '';
  var conflictPending = null;

  // ===== Modal resize =====
  var modalEl = document.querySelector('#editModal .modal');
  var resizeHandle = document.getElementById('modalResizeHandle');
  var isResizing = false;
  var resizeStartX, resizeStartY, startWidth, startHeight;

  resizeHandle.addEventListener('mousedown', function (e) {
    e.preventDefault();
    e.stopPropagation();
    isResizing = true;
    resizeStartX = e.clientX;
    resizeStartY = e.clientY;
    startWidth = modalEl.offsetWidth;
    startHeight = modalEl.offsetHeight;
    document.body.style.userSelect = 'none';
    document.body.style.cursor = 'nwse-resize';
    document.addEventListener('mousemove', onResizeMove);
    document.addEventListener('mouseup', onResizeUp);
  });

  function onResizeMove(e) {
    if (!isResizing) return;
    var w = Math.max(360, Math.min(startWidth + (e.clientX - resizeStartX), window.innerWidth - 40));
    var h = Math.max(280, Math.min(startHeight + (e.clientY - resizeStartY), window.innerHeight - 80));
    modalEl.style.maxWidth = w + 'px';
    modalEl.style.width = w + 'px';
    modalEl.style.maxHeight = h + 'px';
    modalEl.style.height = h + 'px';
  }

  function onResizeUp() {
    if (!isResizing) return;
    isResizing = false;
    document.body.style.userSelect = '';
    document.body.style.cursor = '';
    document.removeEventListener('mousemove', onResizeMove);
    document.removeEventListener('mouseup', onResizeUp);
    localStorage.setItem('qa_modal_size', JSON.stringify({
      w: modalEl.style.width,
      h: modalEl.style.height
    }));
  }

  function applySavedModalSize() {
    try {
      var saved = JSON.parse(localStorage.getItem('qa_modal_size'));
      if (saved && saved.w && saved.h) {
        modalEl.style.maxWidth = saved.w;
        modalEl.style.width = saved.w;
        modalEl.style.maxHeight = saved.h;
        modalEl.style.height = saved.h;
      }
    } catch (e) { /* ignore */ }
  }

  // ===== Mermaid =====
  var mermaidReady = false;
  function ensureMermaid() {
    if (mermaidReady || typeof mermaid === 'undefined') return;
    var isDark = window.matchMedia('(prefers-color-scheme: dark)').matches;
    mermaid.initialize({
      startOnLoad: false,
      theme: isDark ? 'dark' : 'default',
      securityLevel: 'loose'
    });
    mermaidReady = true;
  }

  function renderMermaidBlocks(container) {
    if (typeof mermaid === 'undefined') return;
    var blocks = container.querySelectorAll('pre code.language-mermaid');
    if (blocks.length === 0) return;
    ensureMermaid();
    blocks.forEach(function (code) {
      var pre = code.parentElement;
      var wrapper = document.createElement('div');
      wrapper.className = 'mermaid-wrapper';
      var div = document.createElement('div');
      div.className = 'mermaid';
      div.textContent = code.textContent;
      wrapper.appendChild(div);
      pre.replaceWith(wrapper);
    });
    try {
      mermaid.run({ nodes: container.querySelectorAll('.mermaid') });
    } catch (e) {
      console.warn('Mermaid rendering error:', e);
    }
  }

  // ===== Shared copy helper =====
  function copyText(text, btn, cssClass) {
    function done(msg) {
      btn.textContent = msg;
      if (msg === '已复制' && cssClass) btn.classList.add(cssClass);
      setTimeout(function () {
        btn.textContent = '复制';
        if (cssClass) btn.classList.remove(cssClass);
      }, 1500);
    }

    if (navigator.clipboard && navigator.clipboard.writeText) {
      navigator.clipboard.writeText(text).then(function () {
        done('已复制');
      }).catch(function () {
        execFallback();
      });
    } else {
      execFallback();
    }

    function execFallback() {
      var ta = document.createElement('textarea');
      ta.value = text;
      ta.style.position = 'fixed';
      ta.style.left = '-9999px';
      document.body.appendChild(ta);
      ta.select();
      try {
        document.execCommand('copy');
        done('已复制');
      } catch (err) {
        done('失败');
      }
      document.body.removeChild(ta);
    }
  }

  // ===== Code copy buttons =====
  function addCopyButtons(container) {
    container.querySelectorAll('.markdown-body pre').forEach(function (pre) {
      if (pre.querySelector('.btn-copy')) return;
      var btn = document.createElement('button');
      btn.className = 'btn-copy';
      btn.textContent = '复制';
      btn.addEventListener('click', function (e) {
        e.stopPropagation();
        var code = pre.querySelector('code');
        var text = code ? code.textContent : pre.textContent;
        copyText(text, btn, 'copied');
      });
      pre.appendChild(btn);
    });
  }

  // ===== Image zoom lightbox =====
  document.addEventListener('click', function (e) {
    var img = e.target.closest('.markdown-body img');
    if (!img || e.target.closest('a')) return;
    if (img.closest('.img-lightbox')) return;
    var lb = document.createElement('div');
    lb.className = 'img-lightbox';
    var lbImg = document.createElement('img');
    lbImg.src = img.src;
    lb.appendChild(lbImg);
    function close() { lb.remove(); document.removeEventListener('keydown', onEsc); }
    function onEsc(ev) { if (ev.key === 'Escape') close(); }
    lb.addEventListener('click', close);
    lbImg.addEventListener('click', function (ev) { ev.stopPropagation(); });
    document.addEventListener('keydown', onEsc);
    document.body.appendChild(lb);
  });

  // ===== Auth =====
  var authSecret = sessionStorage.getItem('qa_secret') || '';

  function isAdmin() { return !!authSecret; }

  function authHeaders() {
    var h = { 'Content-Type': 'application/json' };
    if (authSecret) h['X-Auth'] = authSecret;
    return h;
  }

  function updateAdminUI() {
    var admin = isAdmin();
    btnAdd.style.display = admin ? '' : 'none';
    dbAdminSection.hidden = !admin;
    dbSelectorSection.hidden = !admin;
    btnUpload.style.display = admin ? '' : 'none';
    btnStarFilter.style.display = admin ? '' : 'none';
    btnAllDB.style.display = admin ? '' : 'none';
    btnDBRename.style.display = admin ? '' : 'none';
    btnDBLoad.style.display = admin ? '' : 'none';
    btnDBRemove.style.display = admin ? '' : 'none';

    if (admin) {
      lockIcon.textContent = '🔓';
      lockLabel.textContent = '已解锁 — 管理员模式';
      document.querySelector('.lock-bar').classList.add('unlocked');
      lockInputRow.hidden = true;
      internalStatRow.hidden = false;
      starredStatRow.hidden = false;
    } else {
      lockIcon.textContent = '🔒';
      lockLabel.textContent = '仅公开条目 — 点击解锁';
      document.querySelector('.lock-bar').classList.remove('unlocked');
      internalStatRow.hidden = true;
      starredStatRow.hidden = true;
    }
  }
  updateAdminUI();

  document.querySelector('.lock-bar').addEventListener('click', function () {
    if (isAdmin()) {
      authSecret = '';
      sessionStorage.removeItem('qa_secret');
      updateAdminUI();
      allDBMode = false;
      starFilter = false;
      btnAllDB.classList.remove('active');
      btnStarFilter.classList.remove('active');
      fetchCards();
      fetchStats();
      fetchDBList();
    } else {
      lockInputRow.hidden = false;
      lockSecretInput.focus();
    }
  });

  btnUnlock.addEventListener('click', function () {
    var secret = lockSecretInput.value.trim();
    if (!secret) return;
    fetch('/api/auth', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ secret: secret })
    })
    .then(function (res) {
      if (!res.ok) throw new Error('密钥错误');
      return res.json();
    })
    .then(function () {
      authSecret = secret;
      sessionStorage.setItem('qa_secret', secret);
      lockSecretInput.value = '';
      updateAdminUI();
      fetchCards();
      fetchStats();
      fetchDBList();
    })
    .catch(function (err) { alert('解锁失败: ' + err.message); });
  });

  lockSecretInput.addEventListener('keydown', function (e) {
    if (e.key === 'Enter') btnUnlock.click();
  });

  // ===== Theme =====
  function applyTheme() {
    var isDark = window.matchMedia('(prefers-color-scheme: dark)').matches;
    lightCSS.disabled = isDark;
    darkCSS.disabled = !isDark;
  }
  applyTheme();
  window.matchMedia('(prefers-color-scheme: dark)').addEventListener('change', function () {
    applyTheme();
    mermaidReady = false;
    if (!editModal.hidden && previewVisible) {
      editPreview.innerHTML = md(editAnswer.value);
      renderMermaidBlocks(editPreview);
      addCopyButtons(editPreview);
    }
  });

  // ===== Marked =====
  if (typeof marked !== 'undefined') {
    marked.setOptions({ breaks: true, gfm: true });
  }
  function md(text) {
    if (typeof marked === 'undefined') return escapeHtml(text);

    var mathBlocks = [];
    var mathInlines = [];

    text = text.replace(/\$\$([\s\S]*?)\$\$/g, function (_, math) {
      mathBlocks.push(math.trim());
      return '\x00MB' + (mathBlocks.length - 1) + '\x00';
    });

    text = text.replace(/\$([^$\s](?:[^$]*[^$\s])?)\$/g, function (_, math) {
      mathInlines.push(math.trim());
      return '\x00MI' + (mathInlines.length - 1) + '\x00';
    });

    var html = marked.parse(text);

    html = html.replace(/\x00MB(\d+)\x00/g, function (_, i) {
      var idx = parseInt(i, 10);
      if (typeof katex !== 'undefined') {
        try {
          return katex.renderToString(mathBlocks[idx], { displayMode: true, throwOnError: false });
        } catch (e) { /* fallthrough */ }
      }
      return '<pre><code>' + escapeHtml(mathBlocks[idx]) + '</code></pre>';
    });

    html = html.replace(/\x00MI(\d+)\x00/g, function (_, i) {
      var idx = parseInt(i, 10);
      if (typeof katex !== 'undefined') {
        try {
          return katex.renderToString(mathInlines[idx], { displayMode: false, throwOnError: false });
        } catch (e) { /* fallthrough */ }
      }
      return '<code>' + escapeHtml(mathInlines[idx]) + '</code>';
    });

    return html;
  }

  // ===== Query string helper =====
  function dbQuery() {
    if (allDBMode) return '&db=*';
    if (currentDBName) return '&db=' + encodeURIComponent(currentDBName);
    return '';
  }

  // For write operations, always use the active (single) database
  function dbQuerySingle() {
    if (currentDBName) return '&db=' + encodeURIComponent(currentDBName);
    return '';
  }

  // ===== Database management =====
  function fetchDBList() {
    if (!isAdmin()) { dbSelector.innerHTML = '<option>仅公开条目</option>'; return; }
    fetch('/api/db/list', { headers: { 'X-Auth': authSecret } })
      .then(function (res) { return res.json(); })
      .then(function (list) {
        dbs = list;
        var html = '';
        for (var i = 0; i < list.length; i++) {
          var sel = list[i].name === currentDBName ? ' selected' : '';
          html += '<option value="' + escapeHtml(list[i].name) + '"' + sel + '>' +
            escapeHtml(list[i].name) + ' (' + list[i].count + ')' +
            '</option>';
        }
        dbSelector.innerHTML = html;
        if (!currentDBName && list.length > 0) {
          currentDBName = list[0].name;
          dbSelector.value = currentDBName;
        }
      })
      .catch(function () {});
  }

  dbSelector.addEventListener('change', function () {
    var name = dbSelector.value;
    if (name === currentDBName) return;
    if (allDBMode) {
      allDBMode = false;
      btnAllDB.classList.remove('active');
    }
    fetch('/api/db/switch', {
      method: 'POST',
      headers: authHeaders(),
      body: JSON.stringify({ name: name })
    })
    .then(function (res) { return res.json(); })
    .then(function () {
      currentDBName = name;
      fetchCards();
      fetchStats();
    })
    .catch(function (err) { alert('切换失败: ' + err.message); });
  });

  btnDBRename.addEventListener('click', function () {
    if (!isAdmin() || !currentDBName) return;
    var current = currentDBName;
    var newName = prompt('重命名数据库 "' + current + '" 为：', current);
    if (!newName || newName.trim() === '' || newName.trim() === current) return;
    newName = newName.trim();

    var dbEntry = null;
    for (var i = 0; i < dbs.length; i++) {
      if (dbs[i].name === current) { dbEntry = dbs[i]; break; }
    }
    if (!dbEntry) return;

    fetch('/api/db/name', {
      method: 'PUT',
      headers: authHeaders(),
      body: JSON.stringify({ path: dbEntry.path, newName: newName })
    })
    .then(function (res) { return res.json(); })
    .then(function (data) {
      if (data.error) { alert('重命名失败: ' + data.error); return; }
      currentDBName = newName;
      fetchDBList();
      fetchCards();
      fetchStats();
    })
    .catch(function (err) { alert('重命名失败: ' + err.message); });
  });

  // DB Load
  btnDBLoad.addEventListener('click', function () {
    if (!isAdmin()) return;
    dbLoadPath.value = '';
    dbLoadName.value = '';
    dbLoadStatus.textContent = '';
    dbLoadModal.hidden = false;
    dbLoadPath.focus();
  });

  btnDBLoadClose.addEventListener('click', function () { dbLoadModal.hidden = true; });
  btnDBLoadCancel.addEventListener('click', function () { dbLoadModal.hidden = true; });
  dbLoadModal.addEventListener('click', function (e) { if (e.target === dbLoadModal) dbLoadModal.hidden = true; });

  btnDBLoadConfirm.addEventListener('click', function () {
    var path = dbLoadPath.value.trim();
    if (!path) { dbLoadStatus.textContent = '请输入路径'; return; }
    var name = dbLoadName.value.trim();
    dbLoadStatus.textContent = '加载中...';
    doLoadDB(path, name);
  });

  function doLoadDB(path, name) {
    var body = { path: path, onConflict: 'rename' };
    if (name) body.name = name;

    fetch('/api/db/load', {
      method: 'POST',
      headers: authHeaders(),
      body: JSON.stringify(body)
    })
    .then(function (res) { return res.json(); })
    .then(function (data) {
      if (data.conflict === 'true') {
        conflictPending = { path: path, name: name };
        showConflictModal(data.error, path, name || '');
        dbLoadModal.hidden = true;
        return;
      }
      if (data.error) {
        dbLoadStatus.textContent = '错误: ' + data.error;
        return;
      }
      dbLoadStatus.textContent = '加载成功（' + (data.count || 0) + ' 条）';
      currentDBName = data.name;
      fetchDBList();
      fetchCards();
      fetchStats();
      setTimeout(function () { dbLoadModal.hidden = true; }, 800);
    })
    .catch(function (err) { dbLoadStatus.textContent = '失败: ' + err.message; });
  }

  // Conflict modal
  function showConflictModal(msg, path, suggestedName) {
    conflictMsg.textContent = msg;
    conflictNewName.value = suggestedName + '_' + new Date().toISOString().slice(5, 10).replace(/-/g, '') + '-' +
      String(new Date().getHours()).padStart(2, '0') + String(new Date().getMinutes()).padStart(2, '0');
    conflictPending = { path: path, name: suggestedName };
    conflictModal.hidden = false;
  }

  btnConflictClose.addEventListener('click', function () { conflictModal.hidden = true; });
  btnConflictCancel.addEventListener('click', function () { conflictModal.hidden = true; });
  conflictModal.addEventListener('click', function (e) { if (e.target === conflictModal) conflictModal.hidden = true; });

  btnConflictConfirm.addEventListener('click', function () {
    var action = document.querySelector('input[name="conflictAction"]:checked').value;
    if (!conflictPending) return;

    if (action === 'skip') {
      conflictModal.hidden = true;
      conflictPending = null;
      return;
    }

    var body = { path: conflictPending.path };
    if (action === 'rename') {
      body.name = conflictNewName.value.trim() || (conflictPending.name + '_new');
      body.onConflict = 'rename';
    } else if (action === 'replace') {
      body.onConflict = 'replace';
    }

    fetch('/api/db/load', {
      method: 'POST',
      headers: authHeaders(),
      body: JSON.stringify(body)
    })
    .then(function (res) { return res.json(); })
    .then(function (data) {
      if (data.error) { alert('加载失败: ' + data.error); return; }
      currentDBName = data.name;
      conflictModal.hidden = true;
      conflictPending = null;
      fetchDBList();
      fetchCards();
      fetchStats();
    })
    .catch(function (err) { alert('加载失败: ' + err.message); });
  });

  // DB Remove
  btnDBRemove.addEventListener('click', function () {
    if (!isAdmin()) return;
    if (dbs.length <= 1) { alert('至少保留一个数据库'); return; }
    var dbEntry = null;
    for (var i = 0; i < dbs.length; i++) {
      if (dbs[i].name === currentDBName) { dbEntry = dbs[i]; break; }
    }
    if (!dbEntry) return;
    if (!confirm('确定要移除数据库 "' + currentDBName + '" 吗？\n（不会删除磁盘上的文件）')) return;

    fetch('/api/db/remove', {
      method: 'POST',
      headers: authHeaders(),
      body: JSON.stringify({ path: dbEntry.path })
    })
    .then(function (res) { return res.json(); })
    .then(function (data) {
      if (data.error) { alert('移除失败: ' + data.error); return; }
      currentDBName = '';
      fetchDBList().then(function () {
        if (dbs.length > 0) currentDBName = dbs[0].name;
        fetchCards();
        fetchStats();
      });
    })
    .catch(function (err) { alert('移除失败: ' + err.message); });
  });

  // ===== All-DB toggle =====
  btnAllDB.addEventListener('click', function () {
    if (!isAdmin()) return;
    allDBMode = !allDBMode;
    if (allDBMode) {
      btnAllDB.classList.add('active');
      dbSelector.disabled = true;
    } else {
      btnAllDB.classList.remove('active');
      dbSelector.disabled = false;
    }
    fetchCards();
    fetchStats();
  });

  // ===== Star filter =====
  btnStarFilter.addEventListener('click', function () {
    if (!isAdmin()) return;
    starFilter = !starFilter;
    if (starFilter) {
      btnStarFilter.classList.add('active');
    } else {
      btnStarFilter.classList.remove('active');
    }
    fetchCards();
  });

  // ===== Search =====
  searchInput.addEventListener('input', function () {
    clearTimeout(debounceTimer);
    debounceTimer = setTimeout(fetchCards, 200);
  });

  // ===== Card click delegation =====
  cardContainer.addEventListener('click', function (e) {
    var btnEdit = e.target.closest('.btn-edit');
    if (btnEdit) {
      e.stopPropagation(); e.preventDefault();
      if (!isAdmin()) return;
      editEntry(parseInt(btnEdit.getAttribute('data-id'), 10), btnEdit.getAttribute('data-db-source'));
      return;
    }
    var btnDel = e.target.closest('.btn-delete');
    if (btnDel) {
      e.stopPropagation(); e.preventDefault();
      if (!isAdmin()) return;
      deleteEntry(parseInt(btnDel.getAttribute('data-id'), 10), btnDel.getAttribute('data-db-source'));
      return;
    }
    var btnCopy = e.target.closest('.btn-copy-qa');
    if (btnCopy) {
      e.stopPropagation(); e.preventDefault();
      var cardEl = btnCopy.closest('.card');
      var q = cardEl.querySelector('.card-question').textContent;
      var bodyEl = cardEl.querySelector('.markdown-body');
      var a = '';
      if (bodyEl) {
        var clone = bodyEl.cloneNode(true);
        clone.querySelectorAll('.btn-copy').forEach(function (b) { b.remove(); });
        a = clone.textContent;
      }
      var text = '# ' + q + '\n\n' + a;
      copyText(text, btnCopy);
      return;
    }
    var btnStar = e.target.closest('.btn-star');
    if (btnStar) {
      e.stopPropagation(); e.preventDefault();
      if (!isAdmin()) return;
      toggleStar(parseInt(btnStar.getAttribute('data-id'), 10), btnStar);
      return;
    }
    var header = e.target.closest('.card-header');
    if (header && !e.target.closest('button')) {
      var card = header.closest('.card');
      card.classList.toggle('open');
      if (card.classList.contains('open')) {
        renderMermaidBlocks(card);
        addCopyButtons(card);
        card.querySelectorAll('img[src$=".gif"]').forEach(function (img) {
          var src = img.src;
          img.src = '';
          requestAnimationFrame(function () { img.src = src; });
        });
      }
    }
  });

  // ===== Modal =====
  btnAdd.addEventListener('click', function () {
    if (!isAdmin()) return;
    openModal();
  });
  btnModalClose.addEventListener('click', closeModal);
  btnModalCancel.addEventListener('click', closeModal);
  editModal.addEventListener('click', function (e) {
    if (e.target === editModal) closeModal();
  });

  editForm.addEventListener('input', function () {
    formDirty = true;
  });
  editForm.addEventListener('submit', function (e) {
    e.preventDefault();
    saveEntry();
  });

  // ===== Preview toggle =====
  var fieldAnswer = document.querySelector('.field-answer');
  btnPreview.addEventListener('click', function () {
    previewVisible = !previewVisible;
    if (previewVisible) {
      fieldAnswer.style.display = 'none';
      fieldPreview.classList.add('preview-expanded');
      fieldPreview.hidden = false;
      btnPreview.textContent = '编辑';
      editPreview.innerHTML = md(editAnswer.value);
      renderMermaidBlocks(editPreview);
      addCopyButtons(editPreview);
    } else {
      fieldAnswer.style.display = '';
      fieldPreview.classList.remove('preview-expanded');
      fieldPreview.hidden = true;
      btnPreview.textContent = '预览';
    }
  });

  // ===== Keyboard shortcuts for Markdown editor =====
  editAnswer.addEventListener('keydown', function (e) {
    var isMac = /Mac/.test(navigator.platform);
    var mod = isMac ? e.metaKey : e.ctrlKey;
    if (!mod) return;

    var ta = e.target;
    var start = ta.selectionStart;
    var end = ta.selectionEnd;
    var sel = ta.value.substring(start, end);
    var lineStart = ta.value.lastIndexOf('\n', start - 1) + 1;
    var currentLine = ta.value.substring(lineStart, start);

    var handled = true;

    if (e.key === 'b' || e.key === 'B') {
      wrapSelection(ta, '**', '**');
    } else if (e.key === 'i' || e.key === 'I') {
      wrapSelection(ta, '*', '*');
    } else if ((e.key === 'k' || e.key === 'K') && e.shiftKey) {
      if (sel) {
        var lang = prompt('代码语言（可选，如 go/python/bash）：') || '';
        wrapSelection(ta, '```' + lang + '\n', '\n```');
      } else {
        insertText(ta, '\n```\n\n```\n');
      }
    } else if (e.key === 'k' || e.key === 'K') {
      if (sel) {
        wrapSelection(ta, '[', '](url)');
      } else {
        insertText(ta, '[text](url)');
      }
    } else if (e.key === '`') {
      wrapSelection(ta, '`', '`');
    } else if (e.key === 'h' || e.key === 'H') {
      var hMatch = currentLine.match(/^(#{1,4})\s/);
      if (hMatch) {
        var level = hMatch[1].length;
        if (level >= 4) {
          ta.value = ta.value.substring(0, lineStart) + currentLine.replace(/^#{1,4}\s/, '') + ta.value.substring(start);
          ta.selectionStart = ta.selectionEnd = lineStart;
          ta.dispatchEvent(new Event('input'));
        } else {
          var newLevel = level + 1;
          var newPrefix = '#'.repeat(newLevel) + ' ';
          ta.value = ta.value.substring(0, lineStart) + newPrefix + currentLine.substring(hMatch[0].length) + ta.value.substring(start);
          ta.selectionStart = ta.selectionEnd = lineStart + newPrefix.length;
          ta.dispatchEvent(new Event('input'));
        }
      } else {
        insertAtLineStart(ta, lineStart, '## ');
      }
    } else if (e.key === 'u' || e.key === 'U') {
      if (e.shiftKey) {
        insertAtLineStart(ta, lineStart, '1. ');
      } else {
        insertAtLineStart(ta, lineStart, '- ');
      }
    } else if ((e.key === 's' || e.key === 'S') && e.shiftKey) {
      wrapSelection(ta, '~~', '~~');
    } else if ((e.key === 'x' || e.key === 'X') && e.shiftKey) {
      insertAtLineStart(ta, lineStart, '- [ ] ');
    } else if (e.key === '>' || (e.key === 'b' && e.shiftKey) || (e.key === 'B' && e.shiftKey)) {
      if (e.key === '>') {
        insertAtLineStart(ta, lineStart, '> ');
      } else {
        insertAtLineStart(ta, lineStart, '> ');
      }
    } else if ((e.key === 'm' || e.key === 'M') && e.shiftKey) {
      if (sel) {
        wrapSelection(ta, '$$\n', '\n$$');
      } else {
        insertText(ta, '\n$$\n\n$$\n');
      }
    } else if (e.key === 'm' || e.key === 'M') {
      if (sel) {
        wrapSelection(ta, '```mermaid\n', '\n```');
      } else {
        insertText(ta, '\n```mermaid\ngraph TD\n  A --> B\n```\n');
      }
    } else if (e.key === '-' && e.shiftKey) {
      insertText(ta, '\n---\n');
    } else {
      handled = false;
    }

    if (handled) {
      e.preventDefault();
      formDirty = true;
      if (previewVisible) {
        editPreview.innerHTML = md(ta.value);
        renderMermaidBlocks(editPreview);
        addCopyButtons(editPreview);
      }
    }
  });

  function wrapSelection(textarea, before, after) {
    var start = textarea.selectionStart;
    var end = textarea.selectionEnd;
    var sel = textarea.value.substring(start, end);
    var text = before + (sel || 'text') + after;
    textarea.value = textarea.value.substring(0, start) + text + textarea.value.substring(end);
    if (!sel) {
      var phStart = start + before.length;
      var phLen = sel ? sel.length : 4;
      textarea.selectionStart = phStart;
      textarea.selectionEnd = phStart + (sel ? sel.length : 4);
    } else {
      textarea.selectionStart = start;
      textarea.selectionEnd = start + text.length;
    }
    textarea.focus();
    textarea.dispatchEvent(new Event('input'));
  }

  function insertText(textarea, text) {
    var start = textarea.selectionStart;
    textarea.value = textarea.value.substring(0, start) + text + textarea.value.substring(start);
    textarea.selectionStart = textarea.selectionEnd = start + text.length;
    textarea.focus();
    textarea.dispatchEvent(new Event('input'));
  }

  function insertAtLineStart(textarea, lineStart, prefix) {
    var start = textarea.selectionStart;
    var before = textarea.value.substring(0, lineStart);
    var after = textarea.value.substring(lineStart);
    textarea.value = before + prefix + after;
    textarea.selectionStart = textarea.selectionEnd = lineStart + prefix.length;
    textarea.focus();
    textarea.dispatchEvent(new Event('input'));
  }

  // ===== File upload (all types) =====
  btnUpload.addEventListener('click', function () { fileInput.click(); });
  fileInput.addEventListener('change', function () {
    var file = fileInput.files[0];
    if (file) uploadFile(file);
    fileInput.value = '';
  });

  editAnswer.addEventListener('paste', function (e) {
    var items = e.clipboardData && e.clipboardData.items;
    if (!items) return;
    for (var i = 0; i < items.length; i++) {
      if (items[i].type.indexOf('image') === 0) {
        e.preventDefault();
        if (!isAdmin()) { alert('需要管理员权限'); return; }
        uploadFile(items[i].getAsFile());
        return;
      }
      if (items[i].kind === 'file') {
        e.preventDefault();
        if (!isAdmin()) { alert('需要管理员权限'); return; }
        uploadFile(items[i].getAsFile());
        return;
      }
    }
  });

  editAnswer.addEventListener('dragover', function (e) { e.preventDefault(); });
  editAnswer.addEventListener('drop', function (e) {
    e.preventDefault();
    var file = e.dataTransfer && e.dataTransfer.files && e.dataTransfer.files[0];
    if (!file) return;
    if (!isAdmin()) { alert('需要管理员权限才能上传文件'); return; }
    uploadFile(file);
  });

  function uploadFile(file) {
    uploadStatus.textContent = '上传中...';
    var fd = new FormData();
    fd.append('file', file);

    fetch('/api/upload', { method: 'POST', headers: { 'X-Auth': authSecret }, body: fd })
      .then(function (res) {
        if (!res.ok) return res.json().then(function (e) { throw new Error(e.error); });
        return res.json();
      })
      .then(function (data) {
        uploadStatus.textContent = '';
        if (data.isImage === 'true') {
          insertAtCursor(editAnswer, '![' + (data.name || 'file') + '](' + data.url + ')');
        } else {
          insertAtCursor(editAnswer, '[' + (data.name || 'file') + '](' + data.url + ')');
        }
        if (previewVisible) {
          editPreview.innerHTML = md(editAnswer.value);
          renderMermaidBlocks(editPreview);
          addCopyButtons(editPreview);
        }
      })
      .catch(function (err) {
        uploadStatus.textContent = '失败: ' + err.message;
        setTimeout(function () { uploadStatus.textContent = ''; }, 3000);
      });
  }

  function insertAtCursor(textarea, text) {
    var s = textarea.selectionStart, e = textarea.selectionEnd;
    textarea.value = textarea.value.substring(0, s) + text + textarea.value.substring(e);
    textarea.selectionStart = textarea.selectionEnd = s + text.length;
    textarea.focus();
    textarea.dispatchEvent(new Event('input'));
  }

  // ===== Modal open/close =====
  function openModal(data) {
    modalEl.style.maxWidth = '';
    modalEl.style.width = '';
    modalEl.style.maxHeight = '';
    modalEl.style.height = '';

    previewVisible = false;
    fieldAnswer.style.display = '';
    fieldPreview.classList.remove('preview-expanded');
    fieldPreview.hidden = true;
    btnPreview.textContent = '预览';

    if (data) {
      modalTitle.textContent = '编辑条目 #' + data.id;
      editId.value = data.id;
      editingDBSource = data.db_source || '';
      editQuestion.value = data.question;
      editCategory.value = data.category;
      editVisibility.value = data.visibility;
      editAuthor.value = data.author || 'author';
      editAnswer.value = data.answer;
      editPreview.innerHTML = md(data.answer);
      renderMermaidBlocks(editPreview);
      addCopyButtons(editPreview);
    } else {
      modalTitle.textContent = '新建条目';
      editId.value = '';
      editingDBSource = '';
      editQuestion.value = '';
      editCategory.value = '';
      editVisibility.value = 'public';
      editAuthor.value = 'author';
      editAnswer.value = '';
      editPreview.innerHTML = '';
    }
    editModal.hidden = false;

    applySavedModalSize();

    editQuestion.focus();

    formDirty = false;
  }

  function closeModal(force) {
    if (!force && formDirty) {
      if (!confirm('编辑内容尚未保存，确定要关闭吗？')) return;
    }
    editModal.hidden = true;
    formDirty = false;
  }

  // ===== CRUD =====
  function saveEntry() {
    if (!isAdmin()) return;
    var id = editId.value;
    var payload = {
      question: editQuestion.value.trim(),
      category: editCategory.value.trim(),
      visibility: editVisibility.value,
      author: editAuthor.value.trim() || 'author',
      answer: editAnswer.value
    };
    var dbSave = editingDBSource ? '&db=' + encodeURIComponent(editingDBSource) : dbQuerySingle();
    var url = '/api/qa' + (id ? '/' + id : '') + '?' + dbSave.substring(1);
    var method = id ? 'PUT' : 'POST';
    fetch(url, { method: method, headers: authHeaders(), body: JSON.stringify(payload) })
      .then(function (res) {
        if (!res.ok) return res.json().then(function (e) { throw new Error(e.error); });
        return res.json();
      })
      .then(function () { formDirty = false; closeModal(true); fetchCards(); fetchStats(); })
      .catch(function (err) { alert('保存失败: ' + err.message); });
  }

  function editEntry(id, dbSource) {
    if (!isAdmin()) return;
    var dbParam = dbSource ? '&db=' + encodeURIComponent(dbSource) : dbQuerySingle();
    fetch('/api/qa/' + id + '?' + dbParam.substring(1), { headers: { 'X-Auth': authSecret } })
      .then(function (res) { if (!res.ok) throw new Error('not found'); return res.json(); })
      .then(openModal)
      .catch(function (err) { alert('加载失败: ' + err.message); });
  }

  function deleteEntry(id, dbSource) {
    if (!isAdmin()) return;
    if (!confirm('确定要删除条目 #' + id + ' 吗？')) return;
    var dbParam = dbSource ? '&db=' + encodeURIComponent(dbSource) : dbQuerySingle();
    fetch('/api/qa/' + id + '?' + dbParam.substring(1), { method: 'DELETE', headers: { 'X-Auth': authSecret } })
      .then(function (res) { if (!res.ok) throw new Error('HTTP ' + res.status); fetchCards(); fetchStats(); })
      .catch(function (err) { alert('删除失败: ' + err.message); });
  }

  function toggleStar(id, btn) {
    if (!isAdmin()) return;
    var dbSrc = btn ? btn.getAttribute('data-db-source') : '';
    var dbParam = dbSrc ? '?db=' + encodeURIComponent(dbSrc) : '?' + dbQuerySingle().substring(1);
    fetch('/api/qa/' + id + '/star' + dbParam, { method: 'POST', headers: authHeaders() })
      .then(function (res) { return res.json(); })
      .then(function (data) {
        if (btn) {
          if (data.starred) {
            btn.textContent = '⭐';
            btn.classList.add('starred');
          } else {
            btn.textContent = '☆';
            btn.classList.remove('starred');
          }
        }
        fetchStats();
      })
      .catch(function () {});
  }

  // ===== Fetch & render cards =====
  function fetchCards() {
    searchStatus.innerHTML = '<span class="spinner"></span>';
    var q = searchInput.value.trim();
    var useRegex = false;
    if (q.length > 2 && q[0] === '/' && q[q.length - 1] === '/') {
      q = q.slice(1, -1);
      useRegex = true;
    }
    var url = '/api/qa?';
    if (q) url += 'q=' + encodeURIComponent(q) + (useRegex ? '&regex=true' : '') + '&';
    if (starFilter) url += 'starred=true&';
    url += dbQuery().substring(1);
    if (url.endsWith('?')) url = url.slice(0, -1);

    var headers = {};
    if (authSecret) headers['X-Auth'] = authSecret;
    return fetch(url, { headers: headers })
      .then(function (res) { if (!res.ok) throw new Error('HTTP ' + res.status); return res.json(); })
      .then(renderCards)
      .catch(function (err) { cardContainer.innerHTML = '<div class="empty-state">加载失败: ' + escapeHtml(err.message) + '</div>'; });
  }

  function renderCards(items) {
    var count = items ? items.length : 0;
    searchStatus.textContent = count === 0 ? '无结果' : count + ' 条';
    if (!items || items.length === 0) {
      cardContainer.innerHTML = '<div class="empty-state">没有找到匹配的条目</div>';
      return;
    }

    var html = '';
    var admin = isAdmin();

    // Check if items have db_source (all-db mode)
    var hasDBSource = false;
    for (var i = 0; i < items.length; i++) {
      if (items[i].db_source) { hasDBSource = true; break; }
    }

    if (hasDBSource && allDBMode) {
      // Group by db_source with section headers
      var groups = {};
      var groupOrder = [];
      for (var i = 0; i < items.length; i++) {
        var src = items[i].db_source || '';
        if (!groups[src]) {
          groups[src] = [];
          groupOrder.push(src);
        }
        groups[src].push(items[i]);
      }

      // Find color for each db
      var dbColors = {};
      for (var j = 0; j < dbs.length; j++) {
        dbColors[dbs[j].name] = dbs[j].color;
      }

      for (var g = 0; g < groupOrder.length; g++) {
        var dbName = groupOrder[g];
        var groupItems = groups[dbName];
        var color = dbColors[dbName] || '#888';

        html += '<div class="db-section-header">' +
          '<span class="db-dot" style="background:' + color + '"></span>' +
          escapeHtml(dbName) +
          '<span class="db-section-count">' + groupItems.length + ' 条</span>' +
          '</div>';

        for (var k = 0; k < groupItems.length; k++) {
          html += renderCard(groupItems[k], admin, color);
        }
      }
    } else {
      for (var i = 0; i < items.length; i++) {
        var dbColor = '';
        if (items[i].db_source) {
          for (var j = 0; j < dbs.length; j++) {
            if (dbs[j].name === items[i].db_source) {
              dbColor = dbs[j].color;
              break;
            }
          }
        }
        html += renderCard(items[i], admin, dbColor);
      }
    }

    cardContainer.innerHTML = html;
    addCopyButtons(cardContainer);
  }

  function renderCard(item, admin, dbColor) {
    var answerHTML = md(item.answer);
    var visBadge = item.visibility === 'internal'
      ? '<span class="badge badge-visibility">INTERNAL</span>' : '';

    var tagsHtml = '';
    if (item.category) {
      var tags = item.category.split(',').map(function (t) { return t.trim(); }).filter(Boolean);
      tags.forEach(function (t) {
        tagsHtml += '<span class="badge badge-category">' + escapeHtml(t) + '</span>';
      });
    }

    var dbSourceBadge = '';
    if (item.db_source) {
      dbSourceBadge = '<span class="db-source-badge">' +
        '<span class="db-source-dot" style="background:' + (dbColor || '#888') + '"></span>' +
        escapeHtml(item.db_source) + '</span>';
    }

    var starIcon = admin
      ? '<button class="btn-star' + (item.starred ? ' starred' : '') + '" data-id="' + item.id + '" data-db-source="' + escapeHtml(item.db_source || '') + '">' +
        (item.starred ? '⭐' : '☆') + '</button>'
      : (item.starred ? '<span style="font-size:0.85rem">⭐</span>' : '');

    var actionsHtml = '<div class="card-actions">' +
      starIcon +
      '<button class="btn-sm btn-copy-qa" data-id="' + item.id + '">复制QA</button>';
    if (admin) {
      actionsHtml +=
        '<button class="btn-sm btn-edit" data-id="' + item.id + '" data-db-source="' + escapeHtml(item.db_source || '') + '">编辑</button>' +
        '<button class="btn-sm btn-delete" data-id="' + item.id + '" data-db-source="' + escapeHtml(item.db_source || '') + '">删除</button>';
    }
    actionsHtml += '</div>';

    var datesHtml = '<div class="card-dates">';
    if (item.author) datesHtml += '<span>作者: ' + escapeHtml(item.author) + '</span>';
    if (item.created_at) datesHtml += '<span>创建: ' + formatDate(item.created_at) + '</span>';
    if (item.updated_at) datesHtml += '<span>更新: ' + formatDate(item.updated_at) + '</span>';
    datesHtml += '</div>';

    var cardStyle = dbColor ? ' style="border-left-color:' + dbColor + '"' : '';
    var cardClass = 'card' + (dbColor ? ' db-source' : '');

    return '<div class="' + cardClass + '"' + cardStyle + '>' +
      '<div class="card-header">' +
        '<span class="card-id">#' + item.id + '</span>' +
        '<div class="card-question">' + escapeHtml(item.question) + '</div>' +
        '<div class="card-meta">' +
          '<div class="card-badges">' + dbSourceBadge + tagsHtml + visBadge + '</div>' +
          actionsHtml +
        '</div>' +
        '<span class="card-chevron">&#9654;</span>' +
      '</div>' +
      '<div class="card-body">' +
        '<div class="markdown-body">' + answerHTML + '</div>' +
        datesHtml +
      '</div>' +
    '</div>';
  }

  function escapeHtml(str) {
    var div = document.createElement('div');
    div.appendChild(document.createTextNode(str));
    return div.innerHTML;
  }

  function formatDate(isoStr) {
    if (!isoStr) return '';
    try {
      var d = new Date(isoStr);
      if (isNaN(d.getTime())) return isoStr;
      return d.getFullYear() + '-' +
        String(d.getMonth() + 1).padStart(2, '0') + '-' +
        String(d.getDate()).padStart(2, '0') + ' ' +
        String(d.getHours()).padStart(2, '0') + ':' +
        String(d.getMinutes()).padStart(2, '0');
    } catch (e) { return isoStr; }
  }

  // ===== Sidebar: stats =====
  function fetchStats() {
    var headers = {};
    if (authSecret) headers['X-Auth'] = authSecret;
    var url = '/api/stats?' + dbQuery().substring(1);
    if (url.endsWith('?')) url = '/api/stats';
    fetch(url, { headers: headers })
      .then(function (res) { return res.json(); })
      .then(renderStats)
      .catch(function () {});
  }

  function renderStats(s) {
    var rows = statsContent.querySelectorAll('.stat-row');
    if (rows.length >= 2) {
      rows[0].querySelector('.stat-value').textContent = s.total || 0;
      rows[1].querySelector('.stat-value').textContent = s.public_count || 0;
    }
    if (s.internal_count > 0 || isAdmin()) {
      var ir = document.getElementById('internalStatRow');
      if (ir) {
        ir.querySelector('.stat-value').textContent = s.internal_count || 0;
        ir.hidden = !isAdmin();
      }
    }
    var sr = document.getElementById('starredStatRow');
    if (sr) {
      sr.querySelector('.stat-value').textContent = s.starred_count || 0;
      sr.hidden = !isAdmin();
    }

    if (s.categories && s.categories.length > 0) {
      var catHTML = '';
      s.categories.forEach(function (c) {
        catHTML += '<div class="cat-item" data-cat="' + escapeHtml(c.name) + '">' +
          '<span class="cat-name">' + escapeHtml(c.name) + '</span>' +
          '<span class="cat-count">' + c.count + '</span></div>';
      });
      catList.innerHTML = catHTML;
      catList.querySelectorAll('.cat-item').forEach(function (el) {
        el.addEventListener('click', function () {
          searchInput.value = el.getAttribute('data-cat');
          fetchCards();
        });
      });
    } else {
      catList.innerHTML = '<span class="text-muted" style="font-size:0.78rem;">暂无分类</span>';
    }

    if (s.recent && s.recent.length > 0) {
      var recHTML = '';
      s.recent.forEach(function (r) {
        var dotHtml = '';
        if (r.db_source) {
          for (var i = 0; i < dbs.length; i++) {
            if (dbs[i].name === r.db_source) {
              dotHtml = '<span class="db-source-dot" style="background:' + dbs[i].color + '"></span>';
              break;
            }
          }
        }
        recHTML += '<div class="recent-item" data-id="' + r.id + '" title="' + escapeHtml(r.question) + '">' +
          dotHtml +
          '<span style="font-family:monospace;color:var(--text-muted);font-size:0.66rem;">#' + r.id + '</span> ' +
          escapeHtml(r.question) + '</div>';
      });
      recentList.innerHTML = recHTML;
      recentList.querySelectorAll('.recent-item').forEach(function (el) {
        el.addEventListener('click', function () {
          var id = el.getAttribute('data-id');
          searchInput.value = '';
          fetchCards().then(function () {
            cardContainer.querySelectorAll('.card').forEach(function (c) {
              var idEl = c.querySelector('.card-id');
              if (idEl && idEl.textContent === '#' + id) {
                c.classList.add('open');
                renderMermaidBlocks(c);
                addCopyButtons(c);
                c.querySelectorAll('img[src$=".gif"]').forEach(function (img) {
                  var s = img.src; img.src = '';
                  requestAnimationFrame(function () { img.src = s; });
                });
                c.scrollIntoView({ behavior: 'smooth', block: 'start' });
              }
            });
          });
        });
      });
    } else {
      recentList.innerHTML = '<span class="text-muted" style="font-size:0.78rem;">暂无条目</span>';
    }
  }

  // ===== Version =====
  function fetchVersion() {
    fetch('/api/version')
      .then(function (res) { return res.json(); })
      .then(function (d) { versionText.textContent = 'v' + (d.version || '--'); })
      .catch(function () { versionText.textContent = '--'; });
  }

  // ===== DB Export / Import =====
  btnExport.addEventListener('click', function () {
    if (!isAdmin()) return;
    fetch('/api/db/export' + (dbQuerySingle() ? '?' + dbQuerySingle().substring(1) : ''), { headers: { 'X-Auth': authSecret } })
      .then(function (res) {
        if (!res.ok) throw new Error('导出失败');
        return res.blob();
      })
      .then(function (blob) {
        var url = URL.createObjectURL(blob);
        var a = document.createElement('a');
        a.href = url;
        a.download = 'qa-wiki-export.zip';
        document.body.appendChild(a);
        a.click();
        document.body.removeChild(a);
        URL.revokeObjectURL(url);
      })
      .catch(function (err) { alert('导出失败: ' + err.message); });
  });

  importMode.addEventListener('change', function () {
    conflictRow.style.display = importMode.value === 'merge' ? '' : 'none';
  });

  btnImport.addEventListener('click', function () {
    if (!isAdmin()) return;
    var mode = importMode.value;
    var msg;
    if (mode === 'load_new') {
      msg = '将导入的数据库文件保存为新数据库并加载，不会影响现有数据库。确定继续？';
    } else if (mode === 'overwrite') {
      msg = '覆盖模式将替换当前选中数据库的全部数据和文件，建议先导出备份。确定继续？';
    } else {
      var strategy = conflictStrategy.options[conflictStrategy.selectedIndex].text;
      msg = '合并模式 — ' + strategy + '\n\n确定继续？';
    }
    if (!confirm(msg)) return;
    dbFileInput.click();
  });

  dbFileInput.addEventListener('change', function () {
    var file = dbFileInput.files[0];
    if (!file) return;
    importStatus.textContent = '导入中...';
    var fd = new FormData();
    fd.append('file', file);
    fd.append('mode', importMode.value);
    if (importMode.value === 'merge') {
      fd.append('conflict', conflictStrategy.value);
    }

    fetch('/api/db/import', { method: 'POST', headers: { 'X-Auth': authSecret }, body: fd })
      .then(function (res) {
        if (!res.ok) return res.json().then(function (e) { throw new Error(e.error); });
        return res.json();
      })
      .then(function (data) {
        var msg;
        if (data.ok === 'loaded_new') {
          msg = '已加载为新数据库: ' + (data.name || '') + '（' + (data.count || 0) + ' 条）';
          currentDBName = data.name;
        } else if (data.ok === 'merged') {
          msg = '合并完成（' + (data.mergedEntries || 0) + ' 条）';
          if (data.filesExtracted && data.filesExtracted > 0) {
            msg += '，含 ' + data.filesExtracted + ' 个文件';
          }
        } else {
          msg = '覆盖导入成功';
          if (data.filesExtracted && data.filesExtracted > 0) {
            msg += '（含 ' + data.filesExtracted + ' 个文件）';
          }
        }
        importStatus.textContent = msg;
        fetchDBList();
        fetchCards();
        fetchStats();
        setTimeout(function () { importStatus.textContent = ''; }, 3000);
      })
      .catch(function (err) {
        importStatus.textContent = '失败: ' + err.message;
        setTimeout(function () { importStatus.textContent = ''; }, 5000);
      });
    dbFileInput.value = '';
  });

  // ===== Initial =====
  fetchDBList();
  fetchCards();
  fetchStats();
  fetchVersion();
})();
