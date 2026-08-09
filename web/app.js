const state = { projects: [], tasks: [], dashboard: null, currentView: 'dashboard', clusterTimer: null, agentMode: 'planner' };
const statusMeta = {
  todo: { label: '待开始', color: '#8d8997' },
  in_progress: { label: '进行中', color: '#6754d9' },
  review: { label: '待评审', color: '#eeb64c' },
  done: { label: '已完成', color: '#3cbf9b' }
};
const priorityLabel = { low: '低', medium: '中', high: '高', urgent: '紧急' };
const stageMeta = {
  intake: ['理解目标', '限定执行边界'], observe: ['观察上下文', '读取项目实时数据'],
  plan: ['制定计划', '编排工具调用'], act: ['执行工具', '安全修改数据'], verify: ['结果校验', '检查数据一致性']
};

const $ = (selector, root = document) => root.querySelector(selector);
const $$ = (selector, root = document) => [...root.querySelectorAll(selector)];
const escapeHtml = value => String(value ?? '').replace(/[&<>'"]/g, char => ({'&':'&amp;','<':'&lt;','>':'&gt;',"'":'&#39;','"':'&quot;'}[char]));
const formatDate = value => value ? new Intl.DateTimeFormat('zh-CN', { month: 'short', day: 'numeric' }).format(new Date(`${value}T00:00:00`)) : '未设置';
const relativeTime = value => {
  if (!value) return '';
  const seconds = Math.max(0, (Date.now() - new Date(value.replace(' ', 'T')).getTime()) / 1000);
  if (seconds < 60) return '刚刚';
  if (seconds < 3600) return `${Math.floor(seconds / 60)} 分钟前`;
  if (seconds < 86400) return `${Math.floor(seconds / 3600)} 小时前`;
  return `${Math.floor(seconds / 86400)} 天前`;
};

async function api(path, options = {}) {
  const response = await fetch(path, {
    ...options,
    headers: { 'Content-Type': 'application/json', ...(options.headers || {}) }
  });
  const type = response.headers.get('content-type') || '';
  const body = type.includes('json') ? await response.json() : await response.text();
  if (!response.ok) throw new Error(body.error || body.detail || `请求失败 (${response.status})`);
  return body;
}

function toast(message, error = false) {
  const element = $('#toast');
  element.textContent = message;
  element.className = `toast show${error ? ' error' : ''}`;
  clearTimeout(element.timer);
  element.timer = setTimeout(() => element.className = 'toast', 2600);
}

const viewLabels = {
  dashboard: ['WORKSPACE OVERVIEW', '早上好，欢迎回来'], projects: ['PROJECT PORTFOLIO', '项目空间'],
  board: ['DELIVERY BOARD', '团队任务看板'], agent: ['AI ORCHESTRATION', 'Orbit Agent'],
  cluster: ['DISTRIBUTED RUNTIME', '集群运行状态']
};

async function navigate(view, push = true) {
  if (!viewLabels[view]) view = 'dashboard';
  state.currentView = view;
  $$('.view').forEach(item => item.classList.toggle('active', item.id === `view-${view}`));
  $$('.nav-item').forEach(item => item.classList.toggle('active', item.dataset.view === view));
  $('#pageEyebrow').textContent = viewLabels[view][0];
  $('#pageTitle').textContent = viewLabels[view][1];
  $('.sidebar').classList.remove('open');
  if (push) history.replaceState(null, '', `#${view}`);
  clearInterval(state.clusterTimer);
  try {
    if (view === 'dashboard') await loadDashboard();
    if (view === 'projects') await loadProjects(true);
    if (view === 'board') await loadBoard();
    if (view === 'agent') await loadAgent();
    if (view === 'cluster') {
      await loadCluster();
      state.clusterTimer = setInterval(loadCluster, 5000);
    }
  } catch (error) { toast(error.message, true); }
}

async function loadProjects(render = false) {
  state.projects = await api('/api/projects');
  fillProjectSelects();
  if (render) renderProjects();
}

function fillProjectSelects() {
  const options = state.projects.map(project => `<option value="${project.id}">${escapeHtml(project.name)}</option>`).join('');
  ['#taskProject', '#agentProject'].forEach(selector => {
    const current = $(selector)?.value;
    if ($(selector)) { $(selector).innerHTML = options; if (current) $(selector).value = current; }
  });
  const filter = $('#projectFilter');
  if (filter) {
    const current = filter.value;
    filter.innerHTML = `<option value="">全部项目</option>${options}`;
    filter.value = current;
  }
}

async function loadDashboard() {
  state.dashboard = await api('/api/dashboard');
  state.projects = state.dashboard.projects;
  fillProjectSelects();
  const stats = state.dashboard.stats;
  const total = Number(stats.total_tasks || 0);
  const completed = Number(stats.completed_tasks || 0);
  const percent = total ? Math.round(completed * 100 / total) : 0;
  $('#statProjects').textContent = stats.active_projects ?? 0;
  $('#statCompletion').textContent = `${percent}%`;
  $('#statActive').textContent = stats.active_tasks ?? 0;
  $('#statOverdue').textContent = stats.overdue_tasks ?? 0;
  $('#completionScore').textContent = `${percent}%`;
  $('#heroSummary').textContent = Number(stats.overdue_tasks) > 0
    ? `当前有 ${stats.active_tasks} 项工作正在推进，${stats.overdue_tasks} 项已逾期。让 Agent 帮你重新排序。`
    : `当前有 ${stats.active_tasks} 项工作正在推进，整体节奏健康。继续保持专注。`;
  $('#upcomingList').classList.remove('skeleton-block');
  $('#upcomingList').innerHTML = state.dashboard.upcoming.length ? state.dashboard.upcoming.map(task => `
    <div class="upcoming-item"><i class="upcoming-dot" style="background:${task.project_color}"></i>
      <div><b>${escapeHtml(task.title)}</b><small>${escapeHtml(task.project_name)} · ${escapeHtml(task.assignee)}</small></div>
      <time>${formatDate(task.due_date)}</time></div>`).join('') : '<p class="empty">近期没有截止任务</p>';
  $('#dashboardProjects').innerHTML = state.projects.slice(0, 5).map(project => `
    <div class="project-row"><div class="project-ident"><i class="project-color" style="background:${project.color}"></i><div><b>${escapeHtml(project.name)}</b><small>${project.completed_count}/${project.task_count} 个任务完成</small></div></div>
      <div class="progress-cell"><div class="progress-bar"><i style="width:${project.progress}%;background:${project.color}"></i></div><small>${project.due_date ? `截止 ${formatDate(project.due_date)}` : '持续项目'}</small></div><strong class="project-percent">${project.progress}%</strong></div>`).join('');
  $('#activityList').innerHTML = state.dashboard.activities.map(activity => `
    <div class="activity-item"><span class="activity-avatar">${activity.actor === 'agent' ? 'AI' : '我'}</span><div><p>${escapeHtml(activity.detail)}</p><time>${relativeTime(activity.created_at)}</time></div></div>`).join('') || '<p class="empty">暂无动态</p>';
}

function renderProjects() {
  $('#projectGrid').innerHTML = state.projects.map(project => `
    <article class="project-card" style="--project-color:${project.color}">
      <div class="project-card-top"><span class="project-symbol" style="background:${project.color}">${escapeHtml(project.name.slice(0,1))}</span><span class="status-pill">${project.status === 'active' ? '进行中' : project.status === 'completed' ? '已完成' : '已暂停'}</span></div>
      <h3>${escapeHtml(project.name)}</h3><p>${escapeHtml(project.description || '暂无项目简介')}</p>
      <div class="project-meta"><span>${project.completed_count}/${project.task_count} 个任务</span><span>${project.due_date ? formatDate(project.due_date) + ' 截止' : '无截止日期'}</span></div>
      <div class="progress-bar"><i style="width:${project.progress}%;background:${project.color}"></i></div>
      <div class="project-card-foot"><span>交付进度</span><strong>${project.progress}%</strong></div>
    </article>`).join('') || '<p class="empty">还没有项目，创建第一个吧。</p>';
}

async function loadBoard() {
  if (!state.projects.length) await loadProjects();
  const project = $('#projectFilter').value;
  state.tasks = await api(`/api/tasks${project ? `?project_id=${project}` : ''}`);
  renderBoard();
}

function renderBoard() {
  $('#kanban').innerHTML = Object.entries(statusMeta).map(([status, meta]) => {
    const tasks = state.tasks.filter(task => task.status === status);
    return `<section class="kanban-column" data-status="${status}"><div class="column-head"><h3><i style="background:${meta.color}"></i>${meta.label}</h3><span class="column-count">${tasks.length}</span></div><div class="task-stack">${tasks.map(taskCard).join('')}</div></section>`;
  }).join('');
  $$('.task-card').forEach(card => {
    card.addEventListener('dragstart', () => card.classList.add('dragging'));
    card.addEventListener('dragend', () => card.classList.remove('dragging'));
  });
  $$('.kanban-column').forEach(column => {
    column.addEventListener('dragover', event => { event.preventDefault(); column.classList.add('drop-active'); });
    column.addEventListener('dragleave', () => column.classList.remove('drop-active'));
    column.addEventListener('drop', async event => {
      event.preventDefault(); column.classList.remove('drop-active');
      const card = $('.task-card.dragging');
      if (!card) return;
      const task = state.tasks.find(item => item.id === Number(card.dataset.id));
      if (task.status === column.dataset.status) return;
      try {
        await api(`/api/tasks/${task.id}`, { method: 'PATCH', body: JSON.stringify({ status: column.dataset.status }) });
        toast(`已移动到「${statusMeta[column.dataset.status].label}」`); await loadBoard();
      } catch (error) { toast(error.message, true); }
    });
  });
}

function taskCard(task) {
  const tags = (task.tags || []).slice(0, 2).map(tag => `<span>${escapeHtml(tag)}</span>`).join('');
  return `<article class="task-card" draggable="true" data-id="${task.id}">
    <div class="task-project"><i style="background:${task.project_color}"></i>${escapeHtml(task.project_name)}</div>
    <h4>${escapeHtml(task.title)}</h4><div class="task-tags"><span class="priority-pill ${task.priority}">${priorityLabel[task.priority]}</span>${tags}</div>
    <div class="task-card-foot"><span>${task.due_date ? '◷ ' + formatDate(task.due_date) : '未设截止'}</span><span class="assignee" title="${escapeHtml(task.assignee)}">${escapeHtml(task.assignee === '未分配' ? '?' : task.assignee.slice(0,1))}</span></div>
  </article>`;
}

async function loadAgent() {
  if (!state.projects.length) await loadProjects();
  const [runs, provider, devRuns, devStatus] = await Promise.all([
    api('/api/agent/runs?limit=12'), api('/api/agent/provider'), api('/api/dev/runs?limit=12'), api('/api/dev/status')
  ]);
  const badge = $('#providerBadge');
  badge.classList.toggle('offline', !provider.available || !provider.model_available);
  badge.innerHTML = `<i></i>${provider.available && provider.model_available ? `Ollama 在线 · ${escapeHtml(provider.model)}` : 'Ollama 不可用 · 自动降级规则引擎'}`;
  $('#devWorkspace').textContent = devStatus.workspace;
  renderRuns(runs);
  renderDevRuns(devRuns);
}

function setAgentMode(mode) {
  state.agentMode = mode;
  $$('.agent-mode-tabs button').forEach(button => button.classList.toggle('active', button.dataset.agentMode === mode));
  $('#plannerConsole').classList.toggle('hidden', mode !== 'planner');
  $('#developerConsole').classList.toggle('hidden', mode !== 'developer');
  $('#agentRuns').classList.toggle('hidden', mode !== 'planner');
  $('#devRuns').classList.toggle('hidden', mode !== 'developer');
  $('#historyTitle').textContent = mode === 'developer' ? '开发运行' : '最近运行';
}

function renderRuns(runs) {
  $('#agentRuns').innerHTML = runs.map(run => `<article class="run-item" data-run="${run.id}">
    <div><small>${escapeHtml(run.project_name || '已删除项目')}</small><i class="run-dot ${run.status}"></i></div>
    <h4>${escapeHtml(run.goal)}</h4><small>${relativeTime(run.started_at)} · ${run.mode === 'execute' ? '执行' : '预览'}</small></article>`).join('') || '<p class="empty">暂无 Agent 运行</p>';
  $$('.run-item').forEach(item => item.addEventListener('click', () => showRun(Number(item.dataset.run))));
}

function renderDevRuns(runs) {
  $('#devRuns').innerHTML = runs.map(run => `<article class="run-item" data-dev-run="${run.id}">
    <div><small>自主开发 · ${escapeHtml(run.workspace.split(/[\\/]/).pop())}</small><i class="run-dot ${run.status}"></i></div>
    <h4>${escapeHtml(run.goal)}</h4><small>${relativeTime(run.started_at)} · ${run.output?.rounds || 0} 轮</small></article>`).join('') || '<p class="empty">暂无开发运行</p>';
  $$('[data-dev-run]').forEach(item => item.addEventListener('click', () => showDevRun(Number(item.dataset.devRun))));
}

async function showDevRun(id, poll = false) {
  const run = await api(`/api/dev/runs/${id}`);
  $('#devRunPanel').classList.remove('hidden');
  $('#devRunGoal').textContent = run.goal;
  $('#devRunId').textContent = `DEV #${run.id}`;
  $('#devRunStatus').textContent = { queued: '队列等待', running: '自主开发中', completed: '构建测试通过', failed: '开发失败' }[run.status] || run.status;
  const stageLabels = { observe: '观察代码', plan: '模型规划', write_file: '写入文件', build: '构建', test: '测试', git_diff: '检查差异', verify: '结果验证', error: '异常' };
  $('#devTimeline').innerHTML = (run.steps || []).map(step => {
    const detail = step.output?.path || step.output?.command || step.output?.summary || step.output?.error || '';
    return `<div class="dev-step ${step.status}"><i>${step.status === 'completed' ? 'OK' : '!'}</i><b>${stageLabels[step.stage] || escapeHtml(step.stage)}</b><code>${escapeHtml(detail)}</code><small>${step.status}</small></div>`;
  }).join('') || '<p class="empty">Worker 正在领取任务…</p>';
  if (run.status === 'completed' || run.status === 'failed') {
    const output = run.output || {};
    const uniqueFiles = [...new Set((output.written_files || []).map(file => file.path))];
    const lastTool = (output.tool_results || []).at(-1);
    $('#devResult').classList.remove('hidden');
    $('#devResult').innerHTML = `<h4>${output.success ? '自主开发完成' : '自主开发未通过'}</h4>
      <p>${escapeHtml(output.summary || output.error || '运行结束')}</p>
      ${uniqueFiles.length ? `<div class="file-chips">${uniqueFiles.map(file => `<span>${escapeHtml(file)}</span>`).join('')}</div>` : ''}
      <p>共执行 ${output.rounds || 0} 轮；模型 ${escapeHtml(output.provider?.model || '—')}；构建与测试${output.success ? '全部通过' : '未全部通过'}。</p>
      ${lastTool?.output ? `<pre class="dev-output">${escapeHtml(lastTool.output)}</pre>` : ''}`;
    if (poll) await loadAgent();
  } else {
    $('#devResult').classList.add('hidden');
    if (poll) setTimeout(() => showDevRun(id, true).catch(error => toast(error.message, true)), 900);
  }
}

async function showRun(id, poll = false) {
  const run = await api(`/api/agent/runs/${id}`);
  $('#runPanel').classList.remove('hidden');
  $('#runGoal').textContent = run.goal;
  $('#runId').textContent = `RUN #${run.id}`;
  $('#runStatus').textContent = { queued: '队列等待', running: '正在执行', completed: '已完成', failed: '执行失败' }[run.status] || run.status;
  const steps = run.steps || [];
  $('#workflow').innerHTML = Object.entries(stageMeta).map(([stage, meta], index) => {
    const step = steps.find(item => item.stage === stage);
    return `<div class="workflow-step ${step ? '' : 'pending'}"><i>${step ? '✓' : index + 1}</i><b>${meta[0]}</b><small>${step ? escapeHtml(step.summary) : meta[1]}</small></div>`;
  }).join('');
  if (run.status === 'completed' || run.status === 'failed') {
    const output = run.output || {};
    $('#agentResult').classList.remove('hidden');
    $('#agentResult').innerHTML = run.status === 'failed'
      ? `<h4>执行未完成</h4><p>${escapeHtml(output.error || 'Worker 处理失败')}</p>`
      : `<h4>Agent 结论</h4><p>${escapeHtml(output.summary || '工作流执行完成')}</p>${(output.insights || []).length ? `<ul>${output.insights.map(item => `<li>${escapeHtml(item)}</li>`).join('')}</ul>` : ''}<p>已执行 ${output.execution?.applied_count || 0} 个操作，验证${output.verification?.passed ? '通过' : '未通过'}。${output.provider?.type === 'ollama' ? ` 本次由本地 ${escapeHtml(output.provider.model)} 推理，耗时 ${Math.round(output.provider.total_duration_ms || 0)} ms。` : ' 本次使用规则引擎。'}</p>`;
    if (poll) { await loadAgent(); if (run.mode === 'execute') await loadProjects(); }
  } else {
    $('#agentResult').classList.add('hidden');
    if (poll) setTimeout(() => showRun(id, true).catch(error => toast(error.message, true)), 700);
  }
}

async function loadCluster() {
  const cluster = await api('/api/cluster');
  const labels = { queued: ['等待中', '#eeb64c'], processing: ['执行中', '#6754d9'], completed: ['已完成', '#3cbf9b'], failed: ['失败', '#f47d62'] };
  $('#queueStats').innerHTML = Object.entries(labels).map(([key, [label, color]]) => `<article class="queue-card"><i style="background:${color}"></i><span>${label}任务</span><strong>${cluster.queue[key] || 0}</strong></article>`).join('');
  $('#nodeTable').innerHTML = `<div class="node-row header"><span>节点</span><span>角色</span><span>地址</span><span>最近心跳</span></div>${cluster.nodes.map(node => `
    <div class="node-row"><span class="node-name"><i style="background:${node.status === 'online' ? '#3cbf9b' : '#f47d62'}"></i>${escapeHtml(node.node_id)}</span><span class="role-badge">${escapeHtml(node.role.toUpperCase())}</span><span>${escapeHtml(node.address)}</span><span>${relativeTime(node.last_heartbeat)}</span></div>`).join('')}`;
}

async function refreshCurrent() { await navigate(state.currentView, false); toast('数据已刷新'); }

function bindEvents() {
  $$('.nav-item').forEach(button => button.addEventListener('click', () => navigate(button.dataset.view)));
  $$('[data-go]').forEach(button => button.addEventListener('click', () => navigate(button.dataset.go)));
  $('#mobileMenu').addEventListener('click', () => $('.sidebar').classList.toggle('open'));
  $('#refreshButton').addEventListener('click', refreshCurrent);
  $('#quickAdd').addEventListener('click', () => $('#taskDialog').showModal());
  $('#boardAdd').addEventListener('click', () => $('#taskDialog').showModal());
  $('#newProject').addEventListener('click', () => $('#projectDialog').showModal());
  $('#projectFilter').addEventListener('change', loadBoard);
  $$('[data-prompt]').forEach(button => button.addEventListener('click', () => $('#agentGoal').value = button.dataset.prompt));
  $$('.agent-mode-tabs button').forEach(button => button.addEventListener('click', () => setAgentMode(button.dataset.agentMode)));
  $$('[data-dev-prompt]').forEach(button => button.addEventListener('click', () => $('#devGoal').value = button.dataset.devPrompt));

  $('#taskForm').addEventListener('submit', async event => {
    event.preventDefault();
    try {
      await api('/api/tasks', { method: 'POST', body: JSON.stringify({
        project_id: Number($('#taskProject').value), title: $('#taskTitle').value.trim(),
        description: $('#taskDescription').value.trim(), priority: $('#taskPriority').value,
        assignee: $('#taskAssignee').value.trim() || '未分配', due_date: $('#taskDue').value || null,
        estimate_hours: Number($('#taskHours').value || 0), tags: []
      }) });
      $('#taskDialog').close(); $('#taskForm').reset(); toast('任务创建成功');
      await navigate(state.currentView, false);
    } catch (error) { toast(error.message, true); }
  });
  $('#projectForm').addEventListener('submit', async event => {
    event.preventDefault();
    try {
      await api('/api/projects', { method: 'POST', body: JSON.stringify({
        name: $('#projectName').value.trim(), description: $('#projectDescription').value.trim(),
        due_date: $('#projectDue').value || null, color: $('#projectColor').value
      }) });
      $('#projectDialog').close(); $('#projectForm').reset(); toast('项目创建成功');
      await loadProjects(state.currentView === 'projects');
    } catch (error) { toast(error.message, true); }
  });
  $('#agentForm').addEventListener('submit', async event => {
    event.preventDefault();
    const button = $('.agent-button'); button.disabled = true; button.textContent = '正在入队…';
    try {
      const result = await api('/api/agent/runs', {
        method: 'POST', headers: { 'Idempotency-Key': crypto.randomUUID() },
        body: JSON.stringify({ project_id: Number($('#agentProject').value), goal: $('#agentGoal').value.trim(), mode: $('#executeMode').checked ? 'execute' : 'preview' })
      });
      toast('工作流已进入分布式队列'); await showRun(result.run_id, true);
    } catch (error) { toast(error.message, true); }
    finally { button.disabled = false; button.innerHTML = '<span>✦</span>启动工作流'; }
  });
  $('#devAgentForm').addEventListener('submit', async event => {
    event.preventDefault();
    const button = $('#devAgentForm .agent-button'); button.disabled = true; button.textContent = '正在创建开发作业…';
    try {
      const result = await api('/api/dev/runs', {
        method: 'POST', headers: { 'Idempotency-Key': crypto.randomUUID() },
        body: JSON.stringify({ goal: $('#devGoal').value.trim() })
      });
      toast('开发作业已进入 Worker 队列'); await showDevRun(result.run_id, true);
    } catch (error) { toast(error.message, true); }
    finally { button.disabled = false; button.innerHTML = '<span>⌘</span>开始自主开发'; }
  });
  window.addEventListener('hashchange', () => navigate(location.hash.slice(1), false));
}

document.addEventListener('DOMContentLoaded', async () => {
  $('#today').textContent = new Intl.DateTimeFormat('zh-CN', { year: 'numeric', month: 'long', day: 'numeric', weekday: 'short' }).format(new Date());
  bindEvents();
  await navigate(location.hash.slice(1) || 'dashboard', false);
});
