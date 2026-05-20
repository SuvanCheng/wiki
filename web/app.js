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
  var statsContent = document.getElementById('statsContent');
  var catList = document.getElementById('catList');
  var recentList = document.getElementById('recentList');
  var dbAdminSection = document.getElementById('dbAdminSection');
  var versionText = document.getElementById('versionText');

  var btnAdd = document.getElementById('btnAdd');

  var btnExport = document.getElementById('btnExport');
  var btnImport = document.getElementById('btnImport');
  var dbFileInput = document.getElementById('dbFileInput');
  var importStatus = document.getElementById('importStatus');

  var editModal = document.getElementById('editModal');
  var modalTitle = document.getElementById('modalTitle');
  var editForm = document.getElementById('editForm');
  var editId = document.getElementById('editId');
  var editQuestion = document.getElementById('editQuestion');
  var editCategory = document.getElementById('editCategory');
  var editVisibility = document.getElementById('editVisibility');
  var editAnswer = document.getElementById('editAnswer');
  var editPreview = document.getElementById('editPreview');
  var btnModalClose = document.getElementById('btnModalClose');
  var btnModalCancel = document.getElementById('btnModalCancel');

  var btnUpload = document.getElementById('btnUpload');
  var fileInput = document.getElementById('fileInput');
  var uploadStatus = document.getElementById('uploadStatus');

  var debounceTimer = null;
  var previewTimer = null;

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
    btnUpload.style.display = admin ? '' : 'none';

    if (admin) {
      lockIcon.textContent = '🔓'; // unlocked
      lockLabel.textContent = '已解锁 — 管理员模式';
      document.querySelector('.lock-bar').classList.add('unlocked');
      lockInputRow.hidden = true;
      internalStatRow.hidden = false;
    } else {
      lockIcon.textContent = '🔒'; // locked
      lockLabel.textContent = '仅公开条目 — 点击解锁';
      document.querySelector('.lock-bar').classList.remove('unlocked');
      internalStatRow.hidden = true;
    }
  }
  updateAdminUI();

  document.querySelector('.lock-bar').addEventListener('click', function () {
    if (isAdmin()) {
      authSecret = '';
      sessionStorage.removeItem('qa_secret');
      updateAdminUI();
      fetchCards();
      fetchStats();
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
    if (!editModal.hidden) editPreview.innerHTML = md(editAnswer.value);
  });

  // ===== Marked =====
  if (typeof marked !== 'undefined') {
    marked.setOptions({ breaks: true, gfm: true });
  }
  function md(text) {
    return typeof marked !== 'undefined' ? marked.parse(text) : escapeHtml(text);
  }

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
      editEntry(parseInt(btnEdit.getAttribute('data-id'), 10));
      return;
    }
    var btnDel = e.target.closest('.btn-delete');
    if (btnDel) {
      e.stopPropagation(); e.preventDefault();
      if (!isAdmin()) return;
      deleteEntry(parseInt(btnDel.getAttribute('data-id'), 10));
      return;
    }
    var header = e.target.closest('.card-header');
    if (header && !e.target.closest('button')) {
      header.closest('.card').classList.toggle('open');
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
  editForm.addEventListener('submit', function (e) {
    e.preventDefault();
    saveEntry();
  });

  editAnswer.addEventListener('input', function () {
    clearTimeout(previewTimer);
    previewTimer = setTimeout(function () {
      editPreview.innerHTML = md(editAnswer.value);
    }, 200);
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

    // Ctrl/Cmd + B: Bold
    if (e.key === 'b' || e.key === 'B') {
      e.preventDefault();
      wrapSelection(ta, '**', '**');
    }
    // Ctrl/Cmd + I: Italic
    else if (e.key === 'i' || e.key === 'I') {
      e.preventDefault();
      wrapSelection(ta, '*', '*');
    }
    // Ctrl/Cmd + K: Link
    else if (e.key === 'k' || e.key === 'K') {
      e.preventDefault();
      if (sel) {
        wrapSelection(ta, '[', '](url)');
      } else {
        insertText(ta, '[text](url)');
      }
    }
    // Ctrl/Cmd + `: Inline code
    else if (e.key === '`') {
      e.preventDefault();
      wrapSelection(ta, '`', '`');
    }
    // Ctrl/Cmd + Shift + K: Code block
    else if ((e.key === 'k' || e.key === 'K') && e.shiftKey) {
      e.preventDefault();
      if (sel) {
        var lang = prompt('代码语言（可选，如 go/python/bash）：') || '';
        wrapSelection(ta, '```' + lang + '\n', '\n```');
      } else {
        insertText(ta, '\n```\n\n```\n');
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
      // Select placeholder word for easy replacement
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
      // Handle file paste (e.g., from file manager)
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
        editAnswer.dispatchEvent(new Event('input'));
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
  }

  // ===== Modal open/close =====
  function openModal(data) {
    if (data) {
      modalTitle.textContent = '编辑条目 #' + data.id;
      editId.value = data.id;
      editQuestion.value = data.question;
      editCategory.value = data.category;
      editVisibility.value = data.visibility;
      editAnswer.value = data.answer;
      editPreview.innerHTML = md(data.answer);
    } else {
      modalTitle.textContent = '新建条目';
      editId.value = '';
      editQuestion.value = '';
      editCategory.value = '';
      editVisibility.value = 'internal';
      editAnswer.value = '';
      editPreview.innerHTML = '';
    }
    editModal.hidden = false;
    editQuestion.focus();
  }

  function closeModal() { editModal.hidden = true; }

  // ===== CRUD =====
  function saveEntry() {
    if (!isAdmin()) return;
    var id = editId.value;
    var payload = {
      question: editQuestion.value.trim(),
      category: editCategory.value.trim(),
      visibility: editVisibility.value,
      answer: editAnswer.value
    };
    var url = '/api/qa';
    var method = 'POST';
    if (id) { url = '/api/qa/' + id; method = 'PUT'; }
    fetch(url, { method: method, headers: authHeaders(), body: JSON.stringify(payload) })
      .then(function (res) {
        if (!res.ok) return res.json().then(function (e) { throw new Error(e.error); });
        return res.json();
      })
      .then(function () { closeModal(); fetchCards(); fetchStats(); })
      .catch(function (err) { alert('保存失败: ' + err.message); });
  }

  function editEntry(id) {
    if (!isAdmin()) return;
    fetch('/api/qa/' + id, { headers: { 'X-Auth': authSecret } })
      .then(function (res) { if (!res.ok) throw new Error('not found'); return res.json(); })
      .then(openModal)
      .catch(function (err) { alert('加载失败: ' + err.message); });
  }

  function deleteEntry(id) {
    if (!isAdmin()) return;
    if (!confirm('确定要删除条目 #' + id + ' 吗？')) return;
    fetch('/api/qa/' + id, { method: 'DELETE', headers: { 'X-Auth': authSecret } })
      .then(function (res) { if (!res.ok) throw new Error('HTTP ' + res.status); fetchCards(); fetchStats(); })
      .catch(function (err) { alert('删除失败: ' + err.message); });
  }

  // ===== Fetch & render cards =====
  function fetchCards() {
    searchStatus.innerHTML = '<span class="spinner"></span>';
    var url = '/api/qa';
    var q = searchInput.value.trim();
    if (q) url += '?q=' + encodeURIComponent(q);
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

    items.forEach(function (item) {
      var answerHTML = md(item.answer);
      var visBadge = item.visibility === 'internal'
        ? '<span class="badge badge-visibility">INTERNAL</span>' : '';

      // Split comma-separated tags into individual badges
      var tagsHtml = '';
      if (item.category) {
        var tags = item.category.split(',').map(function (t) { return t.trim(); }).filter(Boolean);
        tags.forEach(function (t) {
          tagsHtml += '<span class="badge badge-category">' + escapeHtml(t) + '</span>';
        });
      }

      var actionsHtml = '';
      if (admin) {
        actionsHtml = '<div class="card-actions">' +
          '<button class="btn-sm btn-edit" data-id="' + item.id + '">编辑</button>' +
          '<button class="btn-sm btn-delete" data-id="' + item.id + '">删除</button>' +
        '</div>';
      }

      html +=
        '<div class="card">' +
          '<div class="card-header">' +
            '<span class="card-id">#' + item.id + '</span>' +
            '<div class="card-question">' + escapeHtml(item.question) + '</div>' +
            '<div class="card-meta">' +
              '<div class="card-badges">' + tagsHtml + visBadge + '</div>' +
              actionsHtml +
            '</div>' +
            '<span class="card-chevron">&#9654;</span>' +
          '</div>' +
          '<div class="card-body">' +
            '<div class="markdown-body">' + answerHTML + '</div>' +
          '</div>' +
        '</div>';
    });

    cardContainer.innerHTML = html;
  }

  function escapeHtml(str) {
    var div = document.createElement('div');
    div.appendChild(document.createTextNode(str));
    return div.innerHTML;
  }

  // ===== Sidebar: stats =====
  function fetchStats() {
    var headers = {};
    if (authSecret) headers['X-Auth'] = authSecret;
    fetch('/api/stats', { headers: headers })
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
      var ir = statsContent.querySelector('#internalStatRow');
      if (ir) ir.querySelector('.stat-value').textContent = s.internal_count || 0;
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
        recHTML += '<div class="recent-item" data-id="' + r.id + '" title="' + escapeHtml(r.question) + '">' +
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
    fetch('/api/db/export', { headers: { 'X-Auth': authSecret } })
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

  btnImport.addEventListener('click', function () {
    if (!isAdmin()) return;
    if (!confirm('导入将替换当前全部数据和文件，建议先导出备份。确定继续？')) return;
    dbFileInput.click();
  });

  dbFileInput.addEventListener('change', function () {
    var file = dbFileInput.files[0];
    if (!file) return;
    importStatus.textContent = '导入中...';
    var fd = new FormData();
    fd.append('file', file);

    fetch('/api/db/import', { method: 'POST', headers: { 'X-Auth': authSecret }, body: fd })
      .then(function (res) {
        if (!res.ok) return res.json().then(function (e) { throw new Error(e.error); });
        return res.json();
      })
      .then(function (data) {
        var msg = '导入成功';
        if (data.filesExtracted && data.filesExtracted > 0) {
          msg += '（含 ' + data.filesExtracted + ' 个文件）';
        }
        importStatus.textContent = msg;
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
  fetchCards();
  fetchStats();
  fetchVersion();
})();
