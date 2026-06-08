import { useState, useEffect, useRef } from "react";
import { 
  Search, 
  Plus, 
  Trash2, 
  Terminal, 
  Sliders, 
  Activity 
} from "lucide-react";
import { invoke } from "@tauri-apps/api/core";
import { listen } from "@tauri-apps/api/event";
import { getCurrentWindow } from "@tauri-apps/api/window";
import { getMatches } from "@tauri-apps/plugin-cli";
import "./App.css";

function App() {
  const appWindow = getCurrentWindow();
  // 核心配置状态
  const [proxyHost, setProxyHost] = useState("127.0.0.1");
  const [proxyPort, setProxyPort] = useState("7897");
  const [bypassIps, setBypassIps] = useState("8.8.8.8, 114.114.114.114");

  // 已代理应用列表
  const [apps, setApps] = useState([]);
  const [selectedAppId, setSelectedAppId] = useState(null);

  // 流量统计状态
  const [selectedStats, setSelectedStats] = useState({
    upload: 0,
    download: 0,
    upSpeed: 0,
    downSpeed: 0
  });

  // 波形图数据
  const [points, setPoints] = useState(new Array(13).fill(0));

  // 搜索框状态
  const [searchQuery, setSearchQuery] = useState("");

  // 日志控制台状态
  const [logs, setLogs] = useState([
    { time: getCurrentTime(), type: "system", text: "DetourFlow 控制台初始化成功，等待代理应用拉起..." }
  ]);

  const consoleEndRef = useRef(null);

  // 获取当前时间字符串 (HH:MM:SS)
  function getCurrentTime() {
    const now = new Date();
    return `${String(now.getHours()).padStart(2, '0')}:${String(now.getMinutes()).padStart(2, '0')}:${String(now.getSeconds()).padStart(2, '0')}`;
  }

  // 自动滚动日志
  useEffect(() => {
    if (consoleEndRef.current) {
      consoleEndRef.current.scrollIntoView({ behavior: "smooth" });
    }
  }, [logs]);

  // 1. 订阅后端推送的真实事件日志流
  useEffect(() => {
    let unlisten = null;
    const startListen = async () => {
      unlisten = await listen("log-line", (event) => {
        const payload = event.payload; // { pid, text }
        
        // 解析日志类型决定颜色
        let logType = "stream";
        if (payload.text.includes("DIRECT") || payload.text.includes("Bypass")) {
          logType = "bypass";
        } else if (payload.text.includes("REJECTING") || payload.text.includes("failed") || payload.text.includes("Error")) {
          logType = "error";
        } else if (payload.text.includes("successful") || payload.text.includes("Loaded") || payload.text.includes("committed")) {
          logType = "system";
        }

        setLogs(prev => [
          ...prev,
          { time: getCurrentTime(), type: logType, text: `[PID ${payload.pid}] ${payload.text}` }
        ]);

        // 动态累加该进程的连接活动数
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
    };
    
    startListen();

    // 解析 CLI 命令行参数
    const parseCliArgs = async () => {
      try {
        const matches = await getMatches();
        let targetPath = "";
        let targetProxy = "";
        let targetBypass = "";

        if (matches.args.path && matches.args.path.value) {
          targetPath = matches.args.path.value.toString();
        }
        if (matches.args.proxy && matches.args.proxy.value) {
          targetProxy = matches.args.proxy.value.toString();
          setProxyPort(targetProxy);
        }
        if (matches.args.bypass && matches.args.bypass.value) {
          targetBypass = matches.args.bypass.value.toString();
          setBypassIps(targetBypass);
          await invoke("save_bypass_ips", { bypassIps: targetBypass });
        }

        if (targetPath) {
          const appName = targetPath.split(/[\\/]/).pop() || "命令行拉起";
          setLogs(prev => [
            ...prev,
            { time: getCurrentTime(), type: "system", text: `[CLI命令行] 收到参数，正在代理启动: ${targetPath}...` }
          ]);
          
          const finalProxyPort = targetProxy || proxyPort;
          const finalBypass = targetBypass || bypassIps;

          const pid = await invoke("launch_app", {
            path: targetPath,
            proxyHost,
            proxyPort: finalProxyPort,
            bypassIps: finalBypass
          });

          const newApp = {
            id: Date.now(),
            name: appName,
            path: targetPath,
            pid: pid,
            streams: 0,
            active: true
          };

          setApps(prev => [...prev, newApp]);
          setSelectedAppId(newApp.id);
          setLogs(prev => [
            ...prev,
            { time: getCurrentTime(), type: "system", text: `[CLI命令行] 应用启动成功！PID: ${pid}` }
          ]);
        }
      } catch (e) {
        console.error("解析命令行参数失败:", e);
      }
    };

    parseCliArgs();

    return () => {
      if (unlisten) {
        unlisten().then(() => {});
      }
    };
  }, []);

  // 2. 仿真流量图表动画（仅在选中的应用处于活动运行状态时开启）
  useEffect(() => {
    const selected = apps.find(a => a.id === selectedAppId);
    if (selected && selected.active) {
      const interval = setInterval(() => {
        const up = (Math.random() * 2).toFixed(1);
        const down = (Math.random() * 5).toFixed(1);
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

  // 调用后端 launch_app 命令
  const handleSelectFile = async () => {
    const appName = prompt("请输入应用程序名称 (例如: 命令行):");
    if (!appName) return;
    const path = prompt("请输入可执行文件绝对路径 (.exe):");
    if (!path) return;

    try {
      setLogs(prev => [
        ...prev,
        { time: getCurrentTime(), type: "system", text: `[控制台] 正在尝试以代理模式拉起: ${appName}...` }
      ]);

      const pid = await invoke("launch_app", {
        path,
        proxyHost,
        proxyPort,
        bypassIps
      });

      const newApp = {
        id: Date.now(),
        name: appName,
        path: path,
        pid: pid,
        streams: 0,
        active: true
      };

      setApps(prev => [...prev, newApp]);
      setSelectedAppId(newApp.id);

      setLogs(prev => [
        ...prev,
        { time: getCurrentTime(), type: "system", text: `[控制台] 应用已启动！分配进程 PID: ${pid}` }
      ]);
    } catch (err) {
      setLogs(prev => [
        ...prev,
        { time: getCurrentTime(), type: "error", text: `[错误] 启动程序失败: ${err}` }
      ]);
    }
  };

  // 调用后端 kill_app 命令切换应用状态
  const handleToggleActive = async (id, e) => {
    e.stopPropagation();
    const app = apps.find(a => a.id === id);
    if (!app) return;

    if (app.active) {
      try {
        setLogs(prev => [
          ...prev,
          { time: getCurrentTime(), type: "system", text: `[控制台] 正在终止进程 PID: ${app.pid} (${app.name})...` }
        ]);

        await invoke("kill_app", { pid: app.pid });

        setApps(prev => prev.map(a => {
          if (a.id === id) {
            return { ...a, active: false, streams: 0 };
          }
          return a;
        }));
      } catch (err) {
        setLogs(prev => [
          ...prev,
          { time: getCurrentTime(), type: "error", text: `[错误] 终止进程失败: ${err}` }
        ]);
      }
    } else {
      try {
        setLogs(prev => [
          ...prev,
          { time: getCurrentTime(), type: "system", text: `[控制台] 正在重新拉起应用: ${app.name}...` }
        ]);

        const pid = await invoke("launch_app", {
          path: app.path,
          proxyHost,
          proxyPort,
          bypassIps
        });

        setApps(prev => prev.map(a => {
          if (a.id === id) {
            return { ...a, active: true, pid, streams: 0 };
          }
          return a;
        }));
      } catch (err) {
        setLogs(prev => [
          ...prev,
          { time: getCurrentTime(), type: "error", text: `[错误] 重新启动程序失败: ${err}` }
        ]);
      }
    }
  };

  // 从列表中删除应用记录
  const handleRemoveApp = async (id, name, e) => {
    e.stopPropagation();
    const app = apps.find(a => a.id === id);
    if (app && app.active) {
      try {
        await invoke("kill_app", { pid: app.pid });
      } catch (err) {
        console.error("清除后台应用时失败: ", err);
      }
    }

    setApps(prev => prev.filter(a => a.id !== id));
    if (selectedAppId === id) {
      setSelectedAppId(null);
    }
    
    setLogs(prev => [
      ...prev,
      { time: getCurrentTime(), type: "error", text: `[控制台] 已将应用 ${name} 从托管面板中移除并终止进程` }
    ]);
  };

  // 搜索过滤
  const filteredApps = apps.filter(a => a.name.toLowerCase().includes(searchQuery.toLowerCase()));

  // 选中应用
  const selectedApp = apps.find(a => a.id === selectedAppId);

  // SVG 折线路径
  const sparklinePath = points.map((p, i) => `${i * 30},${30 - p}`).join(" L ");

  return (
    <div className="app-window">
      {/* 1. 窗口标题栏 */}
      <div className="window-header" data-tauri-drag-region>
        <div className="brand" data-tauri-drag-region>
          <div className="brand-icon">D</div>
          <span className="brand-name" data-tauri-drag-region>DetourFlow 控制台</span>
        </div>
        
        <div className="search-bar">
          <Search size={14} color="var(--color-text-dim)" />
          <input 
            type="text" 
            placeholder="搜索已托管的代理进程..." 
            value={searchQuery}
            onChange={e => setSearchQuery(e.target.value)}
          />
        </div>

        <div className="window-controls">
          <button className="win-btn win-close" onClick={() => appWindow.close()} title="关闭"></button>
          <button className="win-btn win-min" onClick={() => appWindow.minimize()} title="最小化"></button>
          <button className="win-btn win-max" onClick={() => appWindow.toggleMaximize()} title="最大化"></button>
        </div>
      </div>

      {/* 2. 窗口主体内容 */}
      <div className="app-body">
        
        <div className="top-section">
          
          {/* 左侧栏：已代理进程 */}
          <div className="sidebar">
            <div className="section-title">已代理进程</div>
            <div className="process-scroll">
              {filteredApps.map(app => (
                <div 
                  key={app.id} 
                  className={`process-card ${selectedAppId === app.id ? "active" : ""}`}
                  onClick={() => setSelectedAppId(app.id)}
                  style={{ opacity: app.active ? 1 : 0.5 }}
                >
                  <div className="process-icon-wrap">
                    {app.name.substring(0, 1).toUpperCase()}
                  </div>
                  <div className="process-details">
                    <div className="process-meta">
                      <span className="process-name">{app.name}</span>
                      {app.active && (
                        <span className="process-streams">
                          <span style={{ width: '6px', height: '6px', borderRadius: '50%', background: 'var(--neon-green)', boxShadow: '0 0 6px var(--neon-green)' }}></span>
                          {app.streams} 流活动
                        </span>
                      )}
                    </div>
                    <span className="process-subtext">PID: {app.pid} / {app.active ? "托管中" : "已停止"}</span>
                  </div>
                  
                  <div style={{ display: 'flex', gap: '8px', alignItems: 'center' }} onClick={e => e.stopPropagation()}>
                    <input 
                      type="checkbox" 
                      style={{ width: '14px', height: '14px', cursor: 'pointer' }}
                      checked={app.active} 
                      onChange={(e) => handleToggleActive(app.id, e)}
                      title={app.active ? "停止代理" : "重新拉起程序"}
                    />
                    <button 
                      onClick={(e) => handleRemoveApp(app.id, app.name, e)} 
                      style={{ background: 'transparent', border: 'none', color: 'var(--color-text-dim)', cursor: 'pointer', display: 'flex', alignItems: 'center' }}
                      title="移除托管"
                    >
                      <Trash2 size={14} />
                    </button>
                  </div>
                </div>
              ))}
              {apps.length === 0 && (
                <div className="no-apps" style={{ fontSize: '0.8rem', marginTop: '40px' }}>
                  暂无代理应用。请点击右侧选择或拖入可执行文件启动。
                </div>
              )}
            </div>
          </div>

          {/* 右侧：DetourFlow 控制面板 */}
          <div className="control-panel">
            <div className="drag-panel" onClick={handleSelectFile}>
              <Plus size={36} color="var(--neon-green)" style={{ background: 'rgba(57, 255, 20, 0.1)', padding: '6px', borderRadius: '50%' }} />
              <span className="drag-title">拖拽可执行文件 (.exe) 到此处拉起代理</span>
              <button className="drag-btn">选择文件</button>
            </div>

            <div className="middle-split">
              {/* 应用流量统计卡片 */}
              <div className="sub-card">
                <div className="sub-card-title">
                  <Activity size={14} /> 
                  <span>{selectedApp ? `${selectedApp.name} 连接状态` : "进程流量监控"}</span>
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

                <div style={{ display: 'flex', justifyContent: 'space-between', fontSize: '0.75rem', color: 'var(--color-text-dim)', marginTop: '4px' }}>
                  <span>实时速度:</span>
                  <span style={{ fontFamily: 'var(--font-mono)', color: '#fff' }}>
                    {selectedStats.upSpeed} Mbps / {selectedStats.downSpeed} Mbps
                  </span>
                </div>

                <div className="speed-chart-container">
                  <div style={{ fontSize: '0.65rem', color: 'var(--color-text-dim)', alignSelf: 'flex-end', marginBottom: '2px' }}>流通道波形</div>
                  <svg className="sparkline">
                    <path d={`M 0,${30 - points[0]} L ${sparklinePath}`} />
                  </svg>
                </div>
              </div>

              {/* SOCKS5 配置 */}
              <div className="sub-card">
                <div className="sub-card-title">
                  <Sliders size={14} /> <span>SOCKS5 代理配置</span>
                </div>

                <div className="settings-fields">
                  <div className="settings-row" style={{ borderBottom: '1px solid rgba(255,255,255,0.03)', paddingBottom: '4px' }}>
                    <span className="settings-label">代理主机:</span>
                    <input 
                      className="settings-value" 
                      value={proxyHost} 
                      onChange={e => setProxyHost(e.target.value)}
                    />
                  </div>
                  <div className="settings-row" style={{ borderBottom: '1px solid rgba(255,255,255,0.03)', paddingBottom: '4px' }}>
                    <span className="settings-label">端口:</span>
                    <input 
                      className="settings-value" 
                      value={proxyPort} 
                      onChange={e => setProxyPort(e.target.value)}
                      style={{ width: '60px' }}
                    />
                  </div>
                  <div className="settings-row" style={{ flexDirection: 'column', alignItems: 'flex-start', gap: '4px' }}>
                    <span className="settings-label">IP 直连白名单 (透传):</span>
                    <input 
                      className="settings-value" 
                      value={bypassIps} 
                      onChange={async e => {
                        const newVal = e.target.value;
                        setBypassIps(newVal);
                        try {
                          await invoke("save_bypass_ips", { bypassIps: newVal });
                        } catch (err) {
                          console.error("保存白名单失败:", err);
                        }
                      }}
                      style={{ width: '100%', textAlign: 'left', fontSize: '0.75rem', color: 'var(--neon-blue)' }}
                      placeholder="逗号分隔，如: 8.8.8.8, 114.114.114.114"
                    />
                  </div>
                  <div className="settings-row" style={{ marginTop: 'auto' }}>
                    <span className="settings-label">控制台状态:</span>
                    <span style={{ color: 'var(--neon-green)', fontWeight: 600, display: 'flex', alignItems: 'center', gap: '4px' }}>
                      <span style={{ width: '6px', height: '6px', borderRadius: '50%', background: 'var(--neon-green)' }}></span>
                      运行中
                    </span>
                  </div>
                </div>
              </div>
            </div>

          </div>

        </div>

        {/* 3. 底部活动日志 */}
        <div className="bottom-logs">
          <div className="logs-header">
            <div className="logs-title">
              <Terminal size={14} /> <span>活动日志</span>
            </div>
            <div style={{ display: 'flex', gap: '12px', color: 'var(--color-text-dim)', fontSize: '0.75rem' }}>
              <span>托管进程数: <strong style={{ color: '#fff' }}>{apps.filter(a => a.active).length}</strong></span>
              <span>代理端口: <strong style={{ color: 'var(--neon-blue)' }}>{proxyPort}</strong></span>
            </div>
          </div>

          <div className="logs-console">
            {logs.map((log, i) => (
              <div key={i} className="log-row">
                <span className="log-row-time">[{log.time}]</span>
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
