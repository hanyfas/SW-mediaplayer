#pragma once
// Minimal resource IDs – no separate .rc file required for tray icon.
// The tray icon falls back to the default application icon if IDI_APPICON
// is not found; that's handled gracefully in TrayIcon::Create.
#define IDI_APPICON 101
