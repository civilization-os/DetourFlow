use serde::Serialize;
use std::fs::File;
use std::io::{BufRead, BufReader};
use std::os::windows::process::CommandExt;
use std::process::Command;
use std::sync::Mutex;
use std::thread;
use std::time::{Duration, Instant};
use tauri::Emitter;
use tauri::Manager;

#[derive(Clone, Serialize)]
struct LogPayload {
    pid: u32,
    text: String,
}

#[derive(Clone, Serialize)]
struct ProcessExitedPayload {
    pid: u32,
    code: i32,
}

/// Managed state: synchronizes detour_bypass.txt writes across commands
struct BypassLock(Mutex<()>);

/// 获取当前 exe 所在目录（打包后与 DetourLauncher.exe / DetourFlow.dll 同目录）
fn exe_dir() -> std::path::PathBuf {
    std::env::current_exe()
        .ok()
        .and_then(|p| p.parent().map(|d| d.to_path_buf()))
        .unwrap_or_else(|| std::path::PathBuf::from("."))
}

/// 定位 DetourLauncher.exe 路径及所在目录，确保与其同目录下的 DetourFlow.dll 一致
fn get_launcher_paths(app: &tauri::AppHandle) -> (std::path::PathBuf, std::path::PathBuf) {
    let dir = exe_dir();
    
    // 1. 优先尝试从 Tauri 资源路径解析（开发模式和标准打包）
    if let Ok(path) = app.path().resolve("resources/DetourLauncher.exe", tauri::path::BaseDirectory::Resource) {
        if path.exists() {
            let parent = path.parent().unwrap_or(&dir).to_path_buf();
            return (path, parent);
        }
    }
    
    // 2. 便携模式：检查 exe 目录下直接是否存在
    let fallback = dir.join("DetourLauncher.exe");
    if fallback.exists() {
        return (fallback, dir.clone());
    }
    
    // 3. 检查 exe 目录下 resources 子目录
    let fallback_res = dir.join("resources").join("DetourLauncher.exe");
    if fallback_res.exists() {
        return (fallback_res, dir.join("resources"));
    }
    
    // 默认行为：即使不存在也返回默认路径（将像之前一样触发 spawn 错误）
    (dir.join("DetourLauncher.exe"), dir)
}

fn resolve_lnk(lnk_path: &str) -> Option<String> {
    use lnk::encoding::WINDOWS_1252;
    use lnk::ShellLink;
    
    // 优先尝试使用 lnk 库解析
    if let Ok(shortcut) = ShellLink::open(lnk_path, WINDOWS_1252) {
        if let Some(link_info) = shortcut.link_info() {
            if let Some(path) = link_info.local_base_path_unicode() {
                return Some(path.to_string());
            }
            if let Some(path) = link_info.local_base_path() {
                return Some(path.to_string());
            }
        }
    }

    // 备用方案：如果第三方库由于编码或特殊格式解析失败，使用 Windows 原生的 PowerShell COM 接口解析
    println!("Lnk library failed to resolve shortcut. Falling back to PowerShell COM parser...");
    const CREATE_NO_WINDOW: u32 = 0x08000000;
    
    // 构造 PowerShell 执行脚本，避免参数中带 & 符号引起混淆
    let ps_script = format!(
        "$sh = New-Object -ComObject WScript.Shell; $lnk = $sh.CreateShortcut(\"{}\"); Write-Output $lnk.TargetPath",
        lnk_path.replace("\"", "`\"")
    );

    if let Ok(output) = Command::new("powershell")
        .args(&["-NoProfile", "-ExecutionPolicy", "Bypass", "-Command", &ps_script])
        .creation_flags(CREATE_NO_WINDOW)
        .output()
    {
        if output.status.success() {
            let stdout = String::from_utf8_lossy(&output.stdout);
            let path = stdout.trim().to_string();
            if !path.is_empty() {
                println!("PowerShell successfully resolved shortcut to: {}", path);
                return Some(path);
            }
        }
    }
    None
}

/// Wait up to `timeout` for a log file to appear (DLL creates it asynchronously)
fn wait_for_log_file(log_path: &std::path::Path, timeout: Duration) -> Option<File> {
    let start = Instant::now();
    while start.elapsed() < timeout {
        if let Ok(f) = File::open(log_path) {
            return Some(f);
        }
        thread::sleep(Duration::from_millis(200));
    }
    None
}

/// Read remaining lines from the log file after process exit (best-effort)
fn drain_logs(reader: &mut BufReader<File>, app: &tauri::AppHandle, pid: u32) {
    let mut line = String::new();
    for _ in 0..100 {
        // cap at 100 lines to avoid unbounded reads
        line.clear();
        match reader.read_line(&mut line) {
            Ok(0) => break,
            Ok(_) => {
                let text = line.trim_end().to_string();
                let _ = app.emit("log-line", LogPayload { pid, text });
            }
            Err(_) => break,
        }
    }
}

#[tauri::command]
fn launch_app(
    app: tauri::AppHandle,
    state: tauri::State<'_, BypassLock>,
    path: String,
    proxy_host: String,
    proxy_port: String,
    bypass_ips: String,
) -> Result<u32, String> {
    let mut target_path = path.clone();
    if path.to_lowercase().ends_with(".lnk") {
        if let Some(resolved) = resolve_lnk(&path) {
            target_path = resolved;
            println!("Resolved shortcut {} to {}", path, target_path);
        } else {
            return Err(format!("无法解析快捷方式的目标路径: {}", path));
        }
    }

    let (launcher_path, launcher_dir) = get_launcher_paths(&app);

    // Write bypass file under lock (best-effort, don't fail the launch)
    {
        let _lock = state.0.lock().unwrap_or_else(|e| e.into_inner());
        let bypass_file_path = launcher_dir.join("detour_bypass.txt");
        if let Err(e) = std::fs::write(&bypass_file_path, &bypass_ips) {
            eprintln!("写入直连白名单文件失败: {}", e);
        }
    }

    const CREATE_NO_WINDOW: u32 = 0x08000000;
    let mut cmd = Command::new(&launcher_path);
    cmd.arg(&target_path);
    cmd.env("DETOUR_PROXY_HOST", &proxy_host);
    cmd.env("DETOUR_PROXY_PORT", &proxy_port);
    cmd.env("DETOUR_BYPASS_IPS", &bypass_ips);
    cmd.env("no_proxy", "localhost,127.0.0.1,::1");
    cmd.env("NO_PROXY", "localhost,127.0.0.1,::1");
    cmd.stdout(std::process::Stdio::piped());
    cmd.creation_flags(CREATE_NO_WINDOW);

    let mut child = cmd.spawn().map_err(|e| format!("启动程序失败: {}", e))?;
    let launcher_pid = child.id();
    let mut target_pid = launcher_pid;

    // 从 DetourLauncher 标准输出中尝试解析真实的目标进程 PID
    if let Some(ref mut stdout) = child.stdout {
        let mut reader = BufReader::new(stdout);
        let mut line = String::new();
        for _ in 0..15 {
            line.clear();
            if let Ok(n) = reader.read_line(&mut line) {
                if n == 0 {
                    break;
                }
                let trimmed = line.trim();
                if let Some(pos) = trimmed.find("[DETOUR_TARGET_PID]") {
                    let pid_part = trimmed[pos + "[DETOUR_TARGET_PID]".len()..].trim();
                    if let Ok(parsed) = pid_part.parse::<u32>() {
                        target_pid = parsed;
                        println!("精准捕获到目标进程真实 PID: {}", target_pid);
                        break;
                    }
                }
            } else {
                break;
            }
        }
    }

    let log_path = launcher_dir.join(format!("detour_flow_{}.log", target_pid));

    // Background thread: owns Child handle, reads logs, exits when process exits
    thread::spawn(move || {
        let file = wait_for_log_file(&log_path, Duration::from_secs(30));

        if let Some(f) = file {
            let mut reader = BufReader::new(f);
            let mut line = String::new();

            loop {
                // Check if the target process has exited
                match child.try_wait() {
                    Ok(Some(status)) => {
                        let code = status.code().unwrap_or(-1);
                        // Best-effort: drain any remaining log lines
                        drain_logs(&mut reader, &app, target_pid);
                        let _ = app.emit(
                            "process-exited",
                            ProcessExitedPayload { pid: target_pid, code },
                        );
                        break; // thread terminates — file handle drops
                    }
                    Ok(None) => {} // still running
                    Err(e) => {
                        eprintln!("检查进程 {} 状态失败: {}", target_pid, e);
                        break;
                    }
                }

                line.clear();
                match reader.read_line(&mut line) {
                    Ok(0) => {
                        // EOF — wait briefly for more data
                        thread::sleep(Duration::from_millis(200));
                    }
                    Ok(_) => {
                        let text = line.trim_end().to_string();
                        if let Err(e) = app.emit("log-line", LogPayload { pid: target_pid, text }) {
                            eprintln!("发送日志事件失败: {}", e);
                            // Continue even if frontend is gone — the process may still need monitoring
                        }
                    }
                    Err(e) => {
                        eprintln!("读取日志文件失败: {}", e);
                        break;
                    }
                }
            }
        }
        // Child handle drops here → OS closes the process handle
    });

    Ok(target_pid)
}

#[tauri::command]
fn kill_app(pid: u32) -> Result<(), String> {
    const CREATE_NO_WINDOW: u32 = 0x08000000;
    let output = Command::new("taskkill")
        .args(&["/F", "/T", "/PID", &pid.to_string()])
        .creation_flags(CREATE_NO_WINDOW)
        .output()
        .map_err(|e| format!("结束进程失败: {}", e))?;

    if output.status.success() {
        Ok(())
    } else {
        // taskkill fails with non-zero if the process already exited — treat that as success
        let stderr = String::from_utf8_lossy(&output.stderr);
        if stderr.contains("not found") || stderr.contains("不存在") {
            Ok(())
        } else {
            Err(format!("结束进程失败: {}", stderr.trim()))
        }
    }
}

#[tauri::command]
fn open_in_explorer(path: String) -> Result<(), String> {
    const CREATE_NO_WINDOW: u32 = 0x08000000;
    Command::new("explorer")
        .arg(format!("/select,{}", path))
        .creation_flags(CREATE_NO_WINDOW)
        .spawn()
        .map_err(|e| format!("在资源管理器中打开失败: {}", e))?;
    Ok(())
}

#[tauri::command]
fn save_bypass_ips(
    app: tauri::AppHandle,
    state: tauri::State<'_, BypassLock>,
    bypass_ips: String,
) -> Result<(), String> {
    let _lock = state.0.lock().unwrap_or_else(|e| e.into_inner());
    let (_, launcher_dir) = get_launcher_paths(&app);
    let bypass_file_path = launcher_dir.join("detour_bypass.txt");
    std::fs::write(&bypass_file_path, &bypass_ips)
        .map_err(|e| format!("写入直连白名单文件失败: {}", e))
}

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    tauri::Builder::default()
        .manage(BypassLock(Mutex::new(())))
        .plugin(tauri_plugin_dialog::init())
        .plugin(tauri_plugin_opener::init())
        .plugin(tauri_plugin_cli::init())
        .setup(|app| {
            // 1. 强制为当前进程设置全新的 Windows AppUserModelID，彻底斩断与旧 Tauri 默认图标的缓存关联
            #[cfg(target_os = "windows")]
            {
                use std::ffi::OsStr;
                use std::os::windows::ffi::OsStrExt;
                #[link(name = "shell32")]
                extern "system" {
                    fn SetCurrentProcessExplicitAppUserModelID(AppID: *const u16) -> i32;
                }
                let aumid: Vec<u16> = OsStr::new("DetourFlow.NetworkRouter.App.V1")
                    .encode_wide()
                    .chain(std::iter::once(0))
                    .collect();
                unsafe {
                    let _ = SetCurrentProcessExplicitAppUserModelID(aumid.as_ptr());
                }
            }

            // 2. 遍历所有窗口应用最新图标
            for (_label, window) in app.webview_windows() {
                if let Some(icon) = app.default_window_icon() {
                    let _ = window.set_icon(icon.clone());
                }
            }
            Ok(())
        })
        .invoke_handler(tauri::generate_handler![
            launch_app,
            kill_app,
            save_bypass_ips,
            open_in_explorer
        ])
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}
