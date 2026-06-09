use serde::Serialize;
use std::fs::File;
use std::io::{BufRead, BufReader};
use std::os::windows::process::CommandExt;
use std::process::Command;
use std::thread;
use std::time::Duration;
use tauri::Emitter;

#[derive(Clone, Serialize)]
struct LogPayload {
    pid: u32,
    text: String,
}

/// 获取当前 exe 所在目录（打包后与 DetourLauncher.exe / DetourFlow.dll 同目录）
fn exe_dir() -> std::path::PathBuf {
    std::env::current_exe()
        .ok()
        .and_then(|p| p.parent().map(|d| d.to_path_buf()))
        .unwrap_or_else(|| std::path::PathBuf::from("."))
}

fn resolve_lnk(lnk_path: &str) -> Option<String> {
    use lnk::ShellLink;
    use lnk::encoding::WINDOWS_1252;
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
    None
}

#[tauri::command]
fn launch_app(
    app: tauri::AppHandle,
    path: String,
    proxy_host: String,
    proxy_port: String,
    bypass_ips: String,
) -> Result<u32, String> {
    let dir = exe_dir();

    let mut target_path = path.clone();
    if path.to_lowercase().ends_with(".lnk") {
        if let Some(resolved) = resolve_lnk(&path) {
            target_path = resolved;
            println!("Resolved shortcut {} to {}", path, target_path);
        } else {
            return Err(format!("无法解析快捷方式的目标路径: {}", path));
        }
    }

    // 定位与 GUI exe 同目录下的 DetourLauncher.exe
    let launcher_path = dir.join("DetourLauncher.exe");

    let mut cmd = Command::new(&launcher_path);
    cmd.arg(&target_path);
    cmd.env("DETOUR_PROXY_HOST", &proxy_host);
    cmd.env("DETOUR_PROXY_PORT", &proxy_port);
    cmd.env("DETOUR_BYPASS_IPS", &bypass_ips);
    cmd.env("no_proxy", "localhost,127.0.0.1,::1");
    cmd.env("NO_PROXY", "localhost,127.0.0.1,::1");

    // 实时写入同目录下的 detour_bypass.txt，使已运行进程和后续新建连接可以无感刷新白名单 IP 列表
    let bypass_file_path = dir.join("detour_bypass.txt");
    if let Err(e) = std::fs::write(&bypass_file_path, &bypass_ips) {
        println!("写入直连白名单文件失败: {}", e);
    }

    // 在 Windows 下隐藏命令行窗口
    const CREATE_NO_WINDOW: u32 = 0x08000000;
    cmd.creation_flags(CREATE_NO_WINDOW);

    let child = cmd.spawn().map_err(|e| format!("启动程序失败: {}", e))?;
    let pid = child.id();

    // 后台日志文件路径 detour_flow_<pid>.log（与 exe 同目录）
    let log_path = dir.join(format!("detour_flow_{}.log", pid));

    // 启动后台线程轮询该进程生成的日志文件并推送
    thread::spawn(move || {
        let mut file = None;
        // 尝试等待最多 10 秒以让注入的 DLL 创建文件
        for _ in 0..50 {
            if let Ok(f) = File::open(&log_path) {
                file = Some(f);
                break;
            }
            thread::sleep(Duration::from_millis(200));
        }

        if let Some(f) = file {
            let mut reader = BufReader::new(f);
            loop {
                let mut line = String::new();
                match reader.read_line(&mut line) {
                    Ok(0) => {
                        // 读到文件末尾，等待新日志行写入
                        thread::sleep(Duration::from_millis(200));
                    }
                    Ok(_) => {
                        let text = line.trim_end().to_string();
                        let _ = app.emit("log-line", LogPayload { pid, text });
                    }
                    Err(_) => break,
                }
            }
        }
    });

    Ok(pid)
}

#[tauri::command]
fn kill_app(pid: u32) -> Result<(), String> {
    const CREATE_NO_WINDOW: u32 = 0x08000000;
    Command::new("taskkill")
        .args(&["/F", "/T", "/PID", &pid.to_string()])
        .creation_flags(CREATE_NO_WINDOW)
        .status()
        .map_err(|e| format!("结束进程失败: {}", e))?;
    Ok(())
}

#[tauri::command]
fn save_bypass_ips(bypass_ips: String) -> Result<(), String> {
    let bypass_file_path = exe_dir().join("detour_bypass.txt");
    std::fs::write(&bypass_file_path, &bypass_ips)
        .map_err(|e| format!("写入直连白名单文件失败: {}", e))?;
    Ok(())
}

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    tauri::Builder::default()
        .plugin(tauri_plugin_dialog::init())
        .plugin(tauri_plugin_opener::init())
        .plugin(tauri_plugin_cli::init())
        .invoke_handler(tauri::generate_handler![
            launch_app,
            kill_app,
            save_bypass_ips
        ])
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}
