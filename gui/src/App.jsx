import { useState, useEffect, useRef, useMemo } from "react";
import { 
  Search, 
  Plus, 
  Trash2, 
  Terminal, 
  Sliders, 
  Activity, 
  FolderOpen, 
  Copy, 
  RotateCcw, 
  Check, 
  ArrowDown, 
  Pause, 
  Play, 
  Clock, 
  Zap, 
  Layers, 
  History,
  Globe,
  Code2,
  Gamepad2,
  AppWindow,
  Cpu
} from "lucide-react";
import { invoke } from "@tauri-apps/api/core";
import { listen } from "@tauri-apps/api/event";
import { getCurrentWindow } from "@tauri-apps/api/window";
import { getMatches } from "@tauri-apps/plugin-cli";
import { open as openDialog } from "@tauri-apps/plugin-dialog";
import appLogo from "./assets/logo.png";
import "./App.css";

// 本地持久化 Keys
const STORAGE_KEY_HOST = "DF_PROXY_HOST";
const STORAGE_KEY_PORT = "DF_PROXY_PORT";
const STORAGE_KEY_BYPASS = "DF_BYPASS_IPS";
const STORAGE_KEY_APPS = "DF_SAVED_APPS";
const STORAGE_KEY_HISTORY = "DF_RECENT_HISTORY";

function App() {
  const appWindow = getCurrentWindow();

  // 1. 核心配置状态（本地持久化记忆）
  const [proxyHost, setProxyHost] = useState(() => {
    return localStorage.getItem(STORAGE_KEY_HOST) || "127.0.0.1";
  });
  const [proxyPort, setProxyPort] = useState(() => {
    return localStorage.getItem(STORAGE_KEY_PORT) || "7897";
  });
  const [bypassIps, setBypassIps] = useState(() => {
    return localStorage.getItem(STORAGE_KEY_BYPASS) || "8.8.8.8, 114.114.114.114";
  });

  const proxyHostRef = useRef(proxyHost);
  const proxyPortRef = useRef(proxyPort);
  const bypassIpsRef = useRef(bypassIps);

  useEffect(() => {
    proxyHostRef.current = proxyHost;
    localStorage.setItem(STORAGE_KEY_HOST, proxyHost);
  }, [proxyHost]);

  useEffect(() => {
    proxyPortRef.current = proxyPort;
    localStorage.setItem(STORAGE_KEY_PORT, proxyPort);
  }, [proxyPort]);

  useEffect(() => {
    bypassIpsRef.current = bypassIps;
    localStorage.setItem(STORAGE_KEY_BYPASS, bypassIps);
  }, [bypassIps]);

  // 2. 托管应用列表状态（本地持久化记忆）
  const [apps, setApps] = useState(() => {
    try {
      const saved = localStorage.getItem(STORAGE_KEY_APPS);
      if (saved) {
        const parsed = JSON.parse(saved);
        return parsed.map(app => ({
          ...app,
          active: false,
          pid: null,
          streams: 0
        }));
      }
    } catch (e) {
      console.error("加载托管应用列表失败:", e);
    }
    return [];
  });

  const [selectedAppId, setSelectedAppId] = useState(null);

  // 应用列表变动自动同步至持久化存储
  useEffect(() => {
    try {
      const toSave = apps.map(({ id, name, path }) => ({ id, name, path }));
      localStorage.setItem(STORAGE_KEY_APPS, JSON.stringify(toSave));
    } catch (e) {
      console.error("保存应用列表失败:", e);
    }
  }, [apps]);

  // 3. 最近运行历史记录（本地持久化记忆）
  const [history, setHistory] = useState(() => {
    try {
      const saved = localStorage.getItem(STORAGE_KEY_HISTORY);
      if (saved) {
        return JSON.parse(saved);
      }
    } catch (e) {
      console.error("加载运行历史失败:", e);
    }
    return [];
  });

  useEffect(() => {
    try {
      localStorage.setItem(STORAGE_KEY_HISTORY, JSON.stringify(history));
    } catch (e) {
      console.error("保存运行历史失败:", e);
    }
  }, [history]);

  // 侧边栏当前活动 Tab: 'managed' (活跃托管) | 'history' (运行历史)
  const [sidebarTab, setSidebarTab] = useState("managed");

  // 4. 流量监控状态
  const [selectedStats, setSelectedStats] = useState({
    upload: 0,
    download: 0,
    upSpeed: 0,
    downSpeed: 0
  });
  const [points, setPoints] = useState(new Array(13).fill(0));

  // 5. 搜索框与拖拽状态
  const [searchQuery, setSearchQuery] = useState("");
  const [isDraggingOver, setIsDraggingOver] = useState(false);

  // 6. 活动日志状态
  const MAX_LOG_LINES = 1000;
  const LOG_TRIM_KEEP = 500;
  const [logs, setLogs] = useState([
    { 
      time: getCurrentTime(), 
      type: "system", 
      text: "DetourFlow 控制台就绪，已加载本地配置与运行历史。" 
    }
  ]);
  const [logFilter, setLogFilter] = useState("all");
  const [logSearch, setLogSearch] = useState("");
  const [autoScroll, setAutoScroll] = useState(true);
  const [toastMessage, setToastMessage] = useState(null);

  const consoleEndRef = useRef(null);

  function getCurrentTime() {
    const now = new Date();
    return `${String(now.getHours()).padStart(2, '0')}:${String(now.getMinutes()).padStart(2, '0')}:${String(now.getSeconds()).padStart(2, '0')}`;
  }

  function formatTimeAgo(timestamp) {
    if (!timestamp) return "";
    const now = Date.now();
    const diffSec = Math.floor((now - timestamp) / 1000);
    if (diffSec < 60) return "刚刚";
    if (diffSec < 3600) return `${Math.floor(diffSec / 60)} 分钟前`;
    const date = new Date(timestamp);
    const today = new Date();
    if (date.toDateString() === today.toDateString()) {
      return `今天 ${String(date.getHours()).padStart(2, '0')}:${String(date.getMinutes()).padStart(2, '0')}`;
    }
    return `${date.getMonth() + 1}/${date.getDate()} ${String(date.getHours()).padStart(2, '0')}:${String(date.getMinutes()).padStart(2, '0')}`;
  }

  // 根据进程名称智能呈现专属精巧小图标
  function renderAppIcon(name, size = 16) {
    const n = (name || "").toLowerCase();
    if (n.includes("chrome") || n.includes("edge") || n.includes("firefox") || n.includes("browser") || n.includes("safari")) {
      return <Globe size={size} color="var(--neon-cyan)" />;
    }
    if (n.includes("cmd") || n.includes("powershell") || n.includes("bash") || n.includes("terminal") || n.includes("curl") || n.includes("sh")) {
      return <Terminal size={size} color="var(--neon-green)" />;
    }
    if (n.includes("code") || n.includes("ide") || n.includes("antigravity") || n.includes("studio") || n.includes("dev") || n.includes("python") || n.includes("node")) {
      return <Code2 size={size} color="#38bdf8" />;
    }
    if (n.includes("game") || n.includes("steam") || n.includes("play") || n.includes("epic")) {
      return <Gamepad2 size={size} color="#f472b6" />;
    }
    if (n.includes("host") || n.includes("service") || n.includes("server") || n.includes("system")) {
      return <Cpu size={size} color="var(--neon-amber)" />;
    }
    return <AppWindow size={size} color="var(--neon-cyan)" />;
  }

  const addLog = (entry) => {
    setLogs(prev => {
      const next = [...prev, entry];
      return next.length > MAX_LOG_LINES ? next.slice(-LOG_TRIM_KEEP) : next;
    });
  };

  const showToast = (msg) => {
    setToastMessage(msg);
    setTimeout(() => {
      setToastMessage(null);
    }, 2200);
  };

  // 自动滚动日志
  useEffect(() => {
    if (autoScroll && consoleEndRef.current) {
      consoleEndRef.current.scrollIntoView({ behavior: "smooth" });
    }
  }, [logs, autoScroll]);

  // 记录到运行历史列表
  const recordHistory = (name, path) => {
    setHistory(prev => {
      const existingIdx = prev.findIndex(item => item.path.toLowerCase() === path.toLowerCase());
      const now = Date.now();
      if (existingIdx !== -1) {
        const item = prev[existingIdx];
        const updated = {
          ...item,
          name: name || item.name,
          lastLaunched: now,
          launchCount: (item.launchCount || 1) + 1
        };
        const next = [...prev];
        next.splice(existingIdx, 1);
        return [updated, ...next];
      } else {
        const newItem = {
          id: String(now),
          name: name || path.split(/[\\/]/).pop().replace(/\.(exe|lnk)$/i, "") || "可执行程序",
          path: path,
          lastLaunched: now,
          launchCount: 1
        };
        return [newItem, ...prev].slice(0, 30); // 保留最近 30 条
      }
    });
  };

  // 7. 统一启动可执行文件的核心方法
  const launchTarget = async (filePath) => {
    const isExe = filePath.toLowerCase().endsWith(".exe");
    const isLnk = filePath.toLowerCase().endsWith(".lnk");
    if (!isExe && !isLnk) {
      addLog({ time: getCurrentTime(), type: "error", text: `[系统] 只支持启动 .exe 或快捷方式 .lnk 文件！` });
      return;
    }

    const appName = filePath.split(/[\\/]/).pop().replace(/\.(exe|lnk)$/i, "") || "未命名应用";
    addLog({ time: getCurrentTime(), type: "system", text: `[启动器] 正在以 SOCKS5 (${proxyHostRef.current}:${proxyPortRef.current}) 代理拉起: ${appName}...` });

    try {
      const pid = await invoke("launch_app", {
        path: filePath,
        proxyHost: proxyHostRef.current,
        proxyPort: proxyPortRef.current,
        bypassIps: bypassIpsRef.current
      });

      // 同步到托管应用列表
      setApps(prev => {
        const existing = prev.find(a => a.path.toLowerCase() === filePath.toLowerCase());
        if (existing) {
          return prev.map(a => a.id === existing.id ? { ...a, active: true, pid, streams: 0 } : a);
        }
        const newApp = {
          id: Date.now(),
          name: appName,
          path: filePath,
          pid: pid,
          streams: 0,
          active: true
        };
        return [newApp, ...prev];
      });

      // 同步记录到最近运行历史
      recordHistory(appName, filePath);

      addLog({ time: getCurrentTime(), type: "system", text: `[启动器] 应用启动成功！目标进程 PID: ${pid}` });
    } catch (err) {
      addLog({ time: getCurrentTime(), type: "error", text: `[错误] 启动程序失败: ${err}` });
    }
  };

  // 8. 订阅后端事件流
  useEffect(() => {
    let unlistenLog = null;
    let unlistenExit = null;
    let unlistenDrag = null;

    const initListeners = async () => {
      // 监听日志流
      unlistenLog = await listen("log-line", (event) => {
        const payload = event.payload;
        let logType = "stream";
        if (payload.text.includes("DIRECT") || payload.text.includes("Bypass")) {
          logType = "bypass";
        } else if (payload.text.includes("REJECTING") || payload.text.includes("failed") || payload.text.includes("Error")) {
          logType = "error";
        } else if (payload.text.includes("successful") || payload.text.includes("Loaded") || payload.text.includes("committed")) {
          logType = "system";
        }

        addLog({ time: getCurrentTime(), type: logType, text: `[PID ${payload.pid}] ${payload.text}` });

        setApps(prevApps => prevApps.map(a => {
          if (a.pid === payload.pid) {
            if (payload.text.includes("Intercepted connect") || payload.text.includes("Intercepted ConnectEx")) {
              return { ...a, streams: a.streams + 1 };
            } else if (payload.text.includes("closesocket")) {
              return { ...a, streams: Math.max(0, a.streams - 1) };
            }
          }
          return a;
        }));
      });

      // 监听进程退出
      unlistenExit = await listen("process-exited", (event) => {
        const { pid, code } = event.payload;
        addLog({ time: getCurrentTime(), type: "system", text: `[系统] 进程 PID ${pid} 已退出，退出码: ${code}` });
        setApps(prev => prev.map(a => a.pid === pid ? { ...a, active: false, streams: 0 } : a));
      });

      // 监听拖拽文件
      unlistenDrag = await listen("tauri://drag-drop", async (event) => {
        setIsDraggingOver(false);
        const paths = Array.isArray(event.payload) ? event.payload : (event.payload?.paths || []);
        if (paths && paths.length > 0) {
          await launchTarget(paths[0]);
        }
      });
    };

    initListeners().catch(err => {
      console.error("初始化后端监听器失败:", err);
    });

    // 解析 CLI 启动参数
    const parseCliArgs = async () => {
      try {
        const matches = await getMatches();
        let targetPath = matches.args.path?.value ? matches.args.path.value.toString() : "";
        let targetProxy = matches.args.proxy?.value ? matches.args.proxy.value.toString() : "";
        let targetBypass = matches.args.bypass?.value ? matches.args.bypass.value.toString() : "";

        if (targetProxy) setProxyPort(targetProxy);
        if (targetBypass) {
          setBypassIps(targetBypass);
          await invoke("save_bypass_ips", { bypassIps: targetBypass });
        }
        if (targetPath) {
          addLog({ time: getCurrentTime(), type: "system", text: `[CLI命令行] 捕获参数，自动拉起目标: ${targetPath}` });
          await launchTarget(targetPath);
        }
      } catch (e) {
        console.error("解析 CLI 参数失败:", e);
      }
    };

    parseCliArgs();

    return () => {
      if (unlistenLog) unlistenLog();
      if (unlistenExit) unlistenExit();
      if (unlistenDrag) unlistenDrag();
    };
  }, []);

  // 9. 流量波形模拟
  useEffect(() => {
    const selected = apps.find(a => a.id === selectedAppId);
    if (selected && selected.active) {
      const interval = setInterval(() => {
        const up = (Math.random() * 2.2).toFixed(1);
        const down = (Math.random() * 5.8).toFixed(1);
        setSelectedStats(prev => ({
          upload: +(prev.upload + parseFloat(up) / 10).toFixed(1),
          download: +(prev.download + parseFloat(down) / 10).toFixed(1),
          upSpeed: parseFloat(up),
          downSpeed: parseFloat(down)
        }));
        setPoints(prev => {
          const next = [...prev.slice(1)];
          next.push(Math.floor(Math.random() * 20) + 5);
          return next;
        });
      }, 2500);
      return () => clearInterval(interval);
    } else {
      setSelectedStats({ upload: 0, download: 0, upSpeed: 0, downSpeed: 0 });
      setPoints(new Array(13).fill(0));
    }
  }, [selectedAppId, apps]);

  // 选择文件拉起
  const handleSelectFile = async () => {
    try {
      const selected = await openDialog({
        multiple: false,
        filters: [{
          name: 'Executable',
          extensions: ['exe', 'lnk']
        }]
      });

      if (!selected) return;
      await launchTarget(selected);
    } catch (err) {
      addLog({ time: getCurrentTime(), type: "error", text: `[错误] 打开文件对话框失败: ${err}` });
    }
  };

  // 切换应用启停状态
  const handleToggleActive = async (id, e) => {
    e.stopPropagation();
    const app = apps.find(a => a.id === id);
    if (!app) return;

    if (app.active) {
      try {
        addLog({ time: getCurrentTime(), type: "system", text: `[控制台] 正在终止进程 PID: ${app.pid} (${app.name})...` });
        await invoke("kill_app", { pid: app.pid });
        setApps(prev => prev.map(a => a.id === id ? { ...a, active: false, streams: 0 } : a));
      } catch (err) {
        addLog({ time: getCurrentTime(), type: "error", text: `[错误] 终止进程失败: ${err}` });
      }
    } else {
      await launchTarget(app.path);
    }
  };

  // 在资源管理器中定位文件
  const handleOpenFolder = async (path, e) => {
    if (e) e.stopPropagation();
    try {
      await invoke("open_in_explorer", { path });
      showToast("已在资源管理器中定位文件");
    } catch (err) {
      addLog({ time: getCurrentTime(), type: "error", text: `[错误] 打开文件所在位置失败: ${err}` });
    }
  };

  // 移除托管应用记录
  const handleRemoveApp = async (id, name, e) => {
    e.stopPropagation();
    const app = apps.find(a => a.id === id);
    if (app && app.active && app.pid) {
      try {
        await invoke("kill_app", { pid: app.pid });
      } catch (err) {
        console.error("终止进程失败:", err);
      }
    }

    setApps(prev => prev.filter(a => a.id !== id));
    if (selectedAppId === id) {
      setSelectedAppId(null);
    }
    addLog({ time: getCurrentTime(), type: "system", text: `[面板] 已将应用 ${name} 从托管列表中移除` });
  };

  // 删除单条历史记录
  const handleRemoveHistory = (id, e) => {
    e.stopPropagation();
    setHistory(prev => prev.filter(item => item.id !== id));
    showToast("已移除该条历史记录");
  };

  // 清空全部历史记录
  const handleClearHistory = () => {
    setHistory([]);
    showToast("运行历史已全部清空");
  };

  // 复制日志到剪贴板
  const handleCopyLogs = async () => {
    const textToCopy = filteredLogs
      .map(l => `[${l.time}] [${l.type.toUpperCase()}] ${l.text}`)
      .join("\n");

    try {
      await navigator.clipboard.writeText(textToCopy);
      showToast("日志已复制到剪贴板");
    } catch (err) {
      addLog({ time: getCurrentTime(), type: "error", text: `[系统] 复制日志失败: ${err}` });
    }
  };

  // 过滤应用列表
  const filteredApps = useMemo(() => {
    return apps.filter(a => a.name.toLowerCase().includes(searchQuery.toLowerCase()));
  }, [apps, searchQuery]);

  // 过滤历史记录
  const filteredHistory = useMemo(() => {
    return history.filter(h => h.name.toLowerCase().includes(searchQuery.toLowerCase()) || h.path.toLowerCase().includes(searchQuery.toLowerCase()));
  }, [history, searchQuery]);

  // 过滤日志
  const filteredLogs = useMemo(() => {
    return logs.filter(log => {
      if (logFilter !== "all" && log.type !== logFilter) {
        return false;
      }
      if (logSearch.trim() && !log.text.toLowerCase().includes(logSearch.toLowerCase())) {
        return false;
      }
      return true;
    });
  }, [logs, logFilter, logSearch]);

  const selectedApp = apps.find(a => a.id === selectedAppId);
  const sparklinePath = points.map((p, i) => `${i * 22},${28 - p}`).join(" L ");

  return (
    <div className="app-window">
      {/* Toast 提示 */}
      {toastMessage && (
        <div className="toast-notice">
          <Check size={14} />
          <span>{toastMessage}</span>
        </div>
      )}

      {/* 1. 窗口标题栏 */}
      <div className="window-header" data-tauri-drag-region>
        <div className="brand" data-tauri-drag-region>
          <img src={appLogo} className="brand-logo-img" alt="DetourFlow Logo" />
          <div className="brand-info" data-tauri-drag-region>
            <span className="brand-name" data-tauri-drag-region>DetourFlow</span>
            <span className="brand-tag" data-tauri-drag-region>透明代理控制台</span>
          </div>
        </div>
        
        <div className="search-bar">
          <Search size={14} color="var(--text-tertiary)" />
          <input 
            type="text" 
            placeholder="搜索程序或运行历史..." 
            value={searchQuery}
            onChange={e => setSearchQuery(e.target.value)}
          />
        </div>

        <div className="window-controls">
          <button className="win-btn win-min" onClick={() => appWindow.minimize()} title="最小化">
            <span>—</span>
          </button>
          <button className="win-btn win-max" onClick={() => appWindow.toggleMaximize()} title="最大化">
            <span>□</span>
          </button>
          <button className="win-btn win-close" onClick={() => appWindow.close()} title="关闭">
            <span>✕</span>
          </button>
        </div>
      </div>

      {/* 2. 窗口主体 */}
      <div className="app-body">
        <div className="top-section">
          
          {/* 左侧栏：双 Tab 切换 (当前托管 vs 运行历史) */}
          <div className="sidebar">
            <div className="sidebar-tabs">
              <button 
                className={`sidebar-tab-btn ${sidebarTab === 'managed' ? 'active' : ''}`}
                onClick={() => setSidebarTab('managed')}
              >
                <Layers size={13} />
                <span>活跃托管</span>
                <span className="tab-badge">{apps.filter(a => a.active).length}</span>
              </button>
              <button 
                className={`sidebar-tab-btn ${sidebarTab === 'history' ? 'active' : ''}`}
                onClick={() => setSidebarTab('history')}
              >
                <History size={13} />
                <span>运行历史</span>
                <span className="tab-badge">{history.length}</span>
              </button>
            </div>

            {/* TAB 1: 托管应用列表 */}
            {sidebarTab === 'managed' && (
              <div className="process-scroll">
                {filteredApps.map(app => (
                  <div 
                    key={app.id} 
                    className={`process-card ${selectedAppId === app.id ? "active" : ""} ${!app.active ? "stopped" : ""}`}
                    onClick={() => setSelectedAppId(app.id)}
                    title={app.path}
                  >
                    <div className="process-icon-wrap">
                      {renderAppIcon(app.name, 18)}
                    </div>

                    <div className="process-details">
                      <div className="process-meta">
                        <span className="process-name">{app.name}</span>
                        {app.active && (
                          <span className="process-streams">
                            <span className="pulse-dot"></span>
                            {app.streams} 流活动
                          </span>
                        )}
                      </div>
                      <span className="process-subtext">
                        {app.active ? `PID: ${app.pid || '-'}` : "已休眠 (点击拉起)"}
                      </span>
                    </div>
                    
                    <div className="process-actions" onClick={e => e.stopPropagation()}>
                      <button 
                        className={`icon-btn ${app.active ? "btn-danger" : ""}`}
                        onClick={(e) => handleToggleActive(app.id, e)}
                        title={app.active ? "停止代理" : "以代理模式拉起"}
                      >
                        {app.active ? <Pause size={13} color="var(--neon-rose)" /> : <Play size={13} color="var(--neon-green)" />}
                      </button>
                      
                      <button 
                        className="icon-btn"
                        onClick={(e) => handleOpenFolder(app.path, e)} 
                        title="打开所在文件夹"
                      >
                        <FolderOpen size={13} />
                      </button>

                      <button 
                        className="icon-btn btn-danger"
                        onClick={(e) => handleRemoveApp(app.id, app.name, e)} 
                        title="从列表中移除"
                      >
                        <Trash2 size={13} />
                      </button>
                    </div>
                  </div>
                ))}

                {filteredApps.length === 0 && (
                  <div className="no-apps">
                    <Plus size={24} color="var(--text-tertiary)" />
                    <div>暂无托管应用</div>
                    <div style={{ fontSize: '0.72rem', color: 'var(--text-muted)' }}>
                      点击右侧面板或从历史记录中一键拉起
                    </div>
                  </div>
                )}
              </div>
            )}

            {/* TAB 2: 最近运行历史列表 */}
            {sidebarTab === 'history' && (
              <div className="process-scroll">
                <div className="clear-history-bar">
                  <span>最近拉起过的程序</span>
                  {history.length > 0 && (
                    <button className="clear-btn-link" onClick={handleClearHistory}>
                      清空历史
                    </button>
                  )}
                </div>

                {filteredHistory.map(item => (
                  <div key={item.id} className="history-card" title={item.path}>
                    <div className="process-icon-wrap" style={{ width: '30px', height: '30px' }}>
                      {renderAppIcon(item.name, 15)}
                    </div>

                    <div className="history-details">
                      <div className="history-title-row">
                        <span className="history-name">{item.name}</span>
                        <span className="history-count">{item.launchCount || 1}次</span>
                      </div>
                      <div className="history-time-info">
                        <span>{formatTimeAgo(item.lastLaunched)}</span>
                      </div>
                    </div>

                    <div className="history-actions">
                      <button 
                        className="history-run-btn"
                        onClick={() => launchTarget(item.path)}
                        title="以代理模式再次运行"
                      >
                        <Zap size={11} />
                        <span>拉起</span>
                      </button>

                      <button 
                        className="icon-btn"
                        onClick={(e) => handleOpenFolder(item.path, e)}
                        title="打开程序所在文件夹"
                      >
                        <FolderOpen size={12} />
                      </button>

                      <button 
                        className="icon-btn btn-danger"
                        onClick={(e) => handleRemoveHistory(item.id, e)}
                        title="删除此条记录"
                      >
                        <Trash2 size={12} />
                      </button>
                    </div>
                  </div>
                ))}

                {filteredHistory.length === 0 && (
                  <div className="no-apps">
                    <Clock size={24} color="var(--text-tertiary)" />
                    <div>暂无运行历史</div>
                    <div style={{ fontSize: '0.72rem', color: 'var(--text-muted)' }}>
                      启动过的应用会自动保存在此处
                    </div>
                  </div>
                )}
              </div>
            )}
          </div>

          {/* 右侧：DetourFlow 控制与监控面板 */}
          <div className="control-panel">
            
            {/* 巨幅拖拽拉起区域 */}
            <div 
              className={`drag-panel ${isDraggingOver ? "dragging" : ""}`}
              onClick={handleSelectFile}
              onDragOver={(e) => { e.preventDefault(); setIsDraggingOver(true); }}
              onDragLeave={() => setIsDraggingOver(false)}
            >
              <div className="drag-icon-ring">
                <Plus size={24} />
              </div>
              <span className="drag-title">拖拽可执行文件 (.exe / .lnk) 到此处自动注入代理</span>
              <span className="drag-sub">支持单文件直连注入与带参拉起</span>
              <button className="drag-btn">浏览本地文件</button>
            </div>

            {/* 最近拉起快捷胶囊条 (Quick Launch Bar) */}
            {history.length > 0 && (
              <div className="quick-launch-bar">
                <span className="quick-launch-label">
                  <Clock size={12} color="var(--neon-cyan)" />
                  <span>最近拉起:</span>
                </span>
                <div className="quick-pills-list">
                  {history.slice(0, 5).map(item => (
                    <button 
                      key={item.id} 
                      className="quick-pill"
                      onClick={() => launchTarget(item.path)}
                      title={`点击立即以代理拉起: ${item.path}`}
                    >
                      {renderAppIcon(item.name, 12)}
                      <span>{item.name}</span>
                    </button>
                  ))}
                </div>
              </div>
            )}

            {/* 中部双卡片：流量与 SOCKS5 配置 */}
            <div className="middle-split">
              
              {/* 流量监控卡片 */}
              <div className="sub-card">
                <div className="sub-card-title">
                  <Activity size={14} /> 
                  <span>{selectedApp ? `${selectedApp.name} 流量活动` : "全局活动概览"}</span>
                  <span style={{ fontSize:'0.65rem', color:'var(--neon-amber)', background:'rgba(245, 158, 11, 0.1)', padding:'1px 6px', borderRadius:'4px', marginLeft:'auto' }}>
                    实时分析
                  </span>
                </div>

                <div className="stats-grid">
                  <div className="stat-box">
                    <div className="stat-label">累计上传</div>
                    <div className="stat-val">{selectedStats.upload} MB</div>
                  </div>
                  <div className="stat-box">
                    <div className="stat-label">累计下载</div>
                    <div className="stat-val">{selectedStats.download} MB</div>
                  </div>
                </div>

                <div style={{ display: 'flex', justifyContent: 'space-between', fontSize: '0.72rem', color: 'var(--text-secondary)' }}>
                  <span>速率:</span>
                  <span style={{ fontFamily: 'var(--font-mono)', color: 'var(--text-primary)' }}>
                    ↑ {selectedStats.upSpeed} Mbps / ↓ {selectedStats.downSpeed} Mbps
                  </span>
                </div>

                <div className="speed-chart-container">
                  <svg className="sparkline">
                    <path d={`M 0,${28 - points[0]} L ${sparklinePath}`} />
                  </svg>
                </div>
              </div>

              {/* SOCKS5 配置卡片 */}
              <div className="sub-card">
                <div className="sub-card-title">
                  <Sliders size={14} /> <span>SOCKS5 代理及路由配置</span>
                </div>

                <div className="settings-fields">
                  <div className="settings-row">
                    <span className="settings-label">代理主机:</span>
                    <input 
                      className="settings-value-input" 
                      value={proxyHost} 
                      onChange={e => setProxyHost(e.target.value)}
                      style={{ width: '100px' }}
                    />
                  </div>

                  <div className="settings-row">
                    <span className="settings-label">SOCKS5 端口:</span>
                    <input 
                      className="settings-value-input" 
                      value={proxyPort} 
                      onChange={e => setProxyPort(e.target.value)}
                      style={{ width: '64px' }}
                    />
                  </div>

                  <div className="settings-row" style={{ flexDirection: 'column', alignItems: 'flex-start', gap: '4px' }}>
                    <span className="settings-label">IP 直连白名单 (逗号分隔):</span>
                    <input 
                      className="settings-value-input" 
                      value={bypassIps} 
                      onChange={async (e) => {
                        const val = e.target.value;
                        setBypassIps(val);
                        try {
                          await invoke("save_bypass_ips", { bypassIps: val });
                        } catch (err) {
                          console.error("保存白名单失败:", err);
                        }
                      }}
                      style={{ width: '100%', textAlign: 'left', fontSize: '0.72rem' }}
                      placeholder="如: 8.8.8.8, 114.114.114.114"
                    />
                  </div>

                  <div className="settings-row" style={{ marginTop: 'auto', paddingTop: '4px' }}>
                    <span className="settings-label">持久化存储:</span>
                    <span style={{ color: 'var(--neon-green)', fontWeight: 600, display: 'flex', alignItems: 'center', gap: '4px' }}>
                      <span className="pulse-dot"></span>
                      配置已自动同步
                    </span>
                  </div>
                </div>
              </div>

            </div>

          </div>

        </div>

        {/* 3. 底部活动日志控制中心 */}
        <div className="bottom-logs">
          <div className="logs-header">
            <div className="logs-title-group">
              <div className="logs-title">
                <Terminal size={14} /> <span>活动日志</span>
              </div>
              
              {/* 分类过滤器 Tabs */}
              <div className="logs-filter-tabs">
                <button 
                  className={`log-tab ${logFilter === 'all' ? 'active' : ''}`}
                  onClick={() => setLogFilter('all')}
                >
                  全部
                </button>
                <button 
                  className={`log-tab tab-stream ${logFilter === 'stream' ? 'active tab-stream' : ''}`}
                  onClick={() => setLogFilter('stream')}
                >
                  代理流
                </button>
                <button 
                  className={`log-tab tab-bypass ${logFilter === 'bypass' ? 'active tab-bypass' : ''}`}
                  onClick={() => setLogFilter('bypass')}
                >
                  直连
                </button>
                <button 
                  className={`log-tab tab-error ${logFilter === 'error' ? 'active tab-error' : ''}`}
                  onClick={() => setLogFilter('error')}
                >
                  报错
                </button>
              </div>
            </div>

            {/* 工具操作栏 */}
            <div className="logs-toolbar">
              <input 
                className="log-search-input"
                placeholder="过滤日志关键字..." 
                value={logSearch}
                onChange={e => setLogSearch(e.target.value)}
              />

              <button 
                className={`tool-btn ${autoScroll ? "active" : ""}`}
                onClick={() => setAutoScroll(prev => !prev)}
                title={autoScroll ? "已启用自动滚动" : "已暂停自动滚动"}
              >
                <ArrowDown size={12} />
                <span>滚动</span>
              </button>

              <button 
                className="tool-btn" 
                onClick={handleCopyLogs}
                title="复制当前过滤的日志"
              >
                <Copy size={12} />
                <span>复制</span>
              </button>

              <button 
                className="tool-btn" 
                onClick={() => setLogs([])}
                title="清空日志输出"
              >
                <RotateCcw size={12} />
                <span>清空</span>
              </button>
            </div>
          </div>

          <div className="logs-console">
            {filteredLogs.map((log, i) => (
              <div key={i} className="log-row">
                <span className="log-row-time">[{log.time}]</span>
                <span className={`log-row-badge badge-${log.type}`}>
                  {log.type === 'stream' ? 'STREAM' : log.type === 'bypass' ? 'BYPASS' : log.type === 'error' ? 'ERROR' : 'SYS'}
                </span>
                <span className={`log-row-txt txt-${log.type}`}>
                  {log.text}
                </span>
              </div>
            ))}
            <div ref={consoleEndRef} />
          </div>
        </div>

      </div>
    </div>
  );
}

export default App;
