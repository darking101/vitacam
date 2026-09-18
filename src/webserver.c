#include "webserver.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/sysmodule.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/dirent.h>
#include <psp2/io/stat.h>
#include <psp2/power.h>
#include <psp2/appmgr.h>
#include <ctype.h>

#define HTTP_PORT 8080
#define NET_PARAM_MEM_SIZE (512 * 1024)

static SceUID server_thid = -1;
static volatile int server_running = 0;
static volatile int server_enabled = 0;

static char vita_ip[64] = "127.0.0.1";
static int server_active = 0;
static void *net_mem = NULL;
static char web_password[32] = "";

const char *webserver_get_password(void) {
    if (web_password[0] == '\0') {
        uint64_t tick = sceKernelGetProcessTimeWide();
        int initial_pin = (int)((tick % 9000) + 1000);
        snprintf(web_password, sizeof(web_password), "%04d", initial_pin);
    }
    return web_password;
}

void webserver_set_password(const char *pwd) {
    strncpy(web_password, pwd, sizeof(web_password)-1);
    web_password[sizeof(web_password)-1] = '\0';
}

static const char b64_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static void base64_encode(const unsigned char *src, size_t len, char *out) {
    size_t i = 0, j = 0;
    while (i < len) {
        uint32_t octet_a = i < len ? src[i++] : 0;
        uint32_t octet_b = i < len ? src[i++] : 0;
        uint32_t octet_c = i < len ? src[i++] : 0;
        uint32_t triple = (octet_a << 16) + (octet_b << 8) + octet_c;
        out[j++] = b64_table[(triple >> 18) & 0x3F];
        out[j++] = b64_table[(triple >> 12) & 0x3F];
        out[j++] = (i > len + 1) ? '=' : b64_table[(triple >> 6) & 0x3F];
        out[j++] = (i > len) ? '=' : b64_table[triple & 0x3F];
    }
    out[j] = '\0';
}


const char *webserver_get_ip(void) {
    SceNetCtlInfo info;
    memset(&info, 0, sizeof(info));
    if (sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_IP_ADDRESS, &info) >= 0 && info.ip_address[0] != '\0') {
        strncpy(vita_ip, info.ip_address, sizeof(vita_ip) - 1);
        vita_ip[sizeof(vita_ip) - 1] = '\0';
    }
    return vita_ip;
}

int webserver_get_port(void) {
    return HTTP_PORT;
}

int webserver_is_active(void) {
    return server_active && server_enabled;
}

void webserver_set_enabled(int enabled) {
    server_enabled = enabled;
}

int webserver_is_enabled(void) {
    return server_enabled;
}

// ── Interfaz Web Samsung Gallery / Google Photos + Apple Glassmorphism ─────
static const char *HTML_PAGE = 
"<!DOCTYPE html>\n"
"<html lang=\"es\">\n"
"<head>\n"
"<meta charset=\"UTF-8\">\n"
"<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no, viewport-fit=cover\">\n"
"<title>Galería PS Vita</title>\n"
"<style>\n"
":root {\n"
"  --bg-color: #000000;\n"
"  --surface-color: #121214;\n"
"  --surface-trans: rgba(20, 20, 24, 0.75);\n"
"  --accent-blue: #007aff;\n"
"  --accent-glow: #0a84ff;\n"
"  --text-main: #ffffff;\n"
"  --text-sub: #8e8e93;\n"
"  --border-glass: rgba(255, 255, 255, 0.12);\n"
"  --glass-blur: blur(28px);\n"
"}\n"
"* { box-sizing: border-box; margin: 0; padding: 0; -webkit-tap-highlight-color: transparent; font-family: -apple-system, BlinkMacSystemFont, 'SF Pro Text', 'Segoe UI', Roboto, Helvetica, Arial, sans-serif; user-select: none; }\n"
"body {\n"
"  background-color: var(--bg-color);\n"
"  color: var(--text-main);\n"
"  min-height: 100vh;\n"
"  padding-bottom: 90px;\n"
"  overflow-x: hidden;\n"
"}\n"
"/* Top Navigation Bar */\n"
".top-bar {\n"
"  position: sticky; top: 0; z-index: 100;\n"
"  background: rgba(0, 0, 0, 0.82);\n"
"  backdrop-filter: var(--glass-blur); -webkit-backdrop-filter: var(--glass-blur);\n"
"  border-bottom: 1px solid rgba(255, 255, 255, 0.08);\n"
"  padding: 12px 16px;\n"
"  display: flex; align-items: center; justify-content: space-between;\n"
"}\n"
".app-title {\n"
"  font-size: 20px; font-weight: 700; letter-spacing: -0.3px;\n"
"  display: flex; align-items: center; gap: 8px;\n"
"}\n"
".status-dot { width: 8px; height: 8px; border-radius: 50%; background: #30d158; box-shadow: 0 0 8px #30d158; }\n"
".top-actions { display: flex; align-items: center; gap: 8px; }\n"
".icon-btn {\n"
"  background: rgba(255, 255, 255, 0.08);\n"
"  border: 1px solid var(--border-glass);\n"
"  color: var(--text-main);\n"
"  width: 38px; height: 38px; border-radius: 50%;\n"
"  display: flex; align-items: center; justify-content: center;\n"
"  cursor: pointer; transition: all 0.2s ease;\n"
"}\n"
".icon-btn:active { transform: scale(0.92); background: rgba(255, 255, 255, 0.2); }\n"
".icon-btn.active { background: var(--accent-blue); border-color: transparent; color: #fff; }\n"
".icon-btn svg { width: 18px; height: 18px; stroke: currentColor; fill: none; stroke-width: 2; stroke-linecap: round; stroke-linejoin: round; }\n"
"/* Container */\n"
".container { max-width: 1200px; margin: 0 auto; padding: 12px 4px; }\n"
"/* Date Section */\n"
".date-section { margin-bottom: 20px; }\n"
".date-header {\n"
"  display: flex; align-items: center; justify-content: space-between;\n"
"  padding: 10px 12px 8px 12px;\n"
"  position: sticky; top: 62px; z-index: 50;\n"
"  background: rgba(0, 0, 0, 0.85);\n"
"  backdrop-filter: blur(16px); -webkit-backdrop-filter: blur(16px);\n"
"}\n"
".date-title { font-size: 17px; font-weight: 700; letter-spacing: -0.2px; }\n"
".date-count { font-size: 13px; color: var(--text-sub); font-weight: 500; margin-left: 8px; }\n"
".date-select-btn {\n"
"  font-size: 13px; font-weight: 600; color: var(--accent-blue);\n"
"  background: none; border: none; cursor: pointer; padding: 4px 8px;\n"
"}\n"
"/* Photo Grid (Samsung Gallery / Google Photos Style) */\n"
".grid {\n"
"  display: grid;\n"
"  grid-template-columns: repeat(3, 1fr);\n"
"  gap: 2px;\n"
"}\n"
"@media (min-width: 768px) {\n"
"  .grid { grid-template-columns: repeat(4, 1fr); gap: 4px; }\n"
"  .container { padding: 16px 12px 110px 12px; }\n"
"}\n"
"@media (min-width: 1024px) {\n"
"  .grid { grid-template-columns: repeat(6, 1fr); gap: 6px; }\n"
"}\n"
".photo-item {\n"
"  position: relative;\n"
"  aspect-ratio: 1;\n"
"  background: #111;\n"
"  overflow: hidden;\n"
"  cursor: pointer;\n"
"}\n"
".photo-item img {\n"
"  width: 100%; height: 100%; object-fit: cover;\n"
"  transition: transform 0.25s ease;\n"
"}\n"
".photo-item:active img { transform: scale(0.97); }\n"
"/* Video Badge */\n"
".vid-indicator {\n"
"  position: absolute; bottom: 6px; left: 6px;\n"
"  background: rgba(0, 0, 0, 0.65);\n"
"  backdrop-filter: blur(8px); -webkit-backdrop-filter: blur(8px);\n"
"  padding: 3px 6px; border-radius: 4px;\n"
"  display: flex; align-items: center; gap: 4px;\n"
"  font-size: 11px; font-weight: 700; color: #fff;\n"
"}\n"
".vid-indicator svg { width: 10px; height: 10px; fill: #fff; }\n"
"/* Selection Checkbox Pill */\n"
".check-circle {\n"
"  position: absolute; top: 6px; right: 6px;\n"
"  width: 24px; height: 24px; border-radius: 50%;\n"
"  border: 2px solid rgba(255, 255, 255, 0.8);\n"
"  background: rgba(0, 0, 0, 0.35);\n"
"  backdrop-filter: blur(4px);\n"
"  display: flex; align-items: center; justify-content: center;\n"
"  transition: all 0.2s ease;\n"
"  opacity: 0;\n"
"}\n"
".selecting .check-circle { opacity: 1; }\n"
".photo-item.selected .check-circle {\n"
"  background: var(--accent-blue);\n"
"  border-color: var(--accent-blue);\n"
"  opacity: 1;\n"
"}\n"
".photo-item.selected img { transform: scale(0.93); border-radius: 8px; }\n"
".photo-item.selected { background: #1c1c1e; }\n"
".check-circle svg { width: 14px; height: 14px; stroke: #fff; fill: none; stroke-width: 3; stroke-linecap: round; stroke-linejoin: round; display: none; }\n"
".photo-item.selected .check-circle svg { display: block; }\n"
"/* Floating Circular Bottom Navigation Bar */\n"
".bottom-nav {\n"
"  position: fixed;\n"
"  bottom: 18px;\n"
"  left: 50%;\n"
"  transform: translateX(-50%);\n"
"  z-index: 100;\n"
"  background: rgba(20, 22, 30, 0.84);\n"
"  backdrop-filter: blur(28px);\n"
"  -webkit-backdrop-filter: blur(28px);\n"
"  border: 1px solid rgba(255, 255, 255, 0.16);\n"
"  border-radius: 9999px;\n"
"  display: flex;\n"
"  align-items: center;\n"
"  gap: 2px;\n"
"  padding: 5px 8px;\n"
"  box-shadow: 0 12px 36px rgba(0, 0, 0, 0.8), 0 0 18px rgba(0, 122, 255, 0.18);\n"
"  max-width: calc(100vw - 24px);\n"
"  overflow-x: auto;\n"
"  scrollbar-width: none;\n"
"}\n"
".bottom-nav::-webkit-scrollbar { display: none; }\n"
".nav-item {\n"
"  display: flex;\n"
"  flex-direction: column;\n"
"  align-items: center;\n"
"  gap: 2px;\n"
"  color: var(--text-sub);\n"
"  font-size: 11px;\n"
"  font-weight: 600;\n"
"  cursor: pointer;\n"
"  transition: all 0.22s ease;\n"
"  padding: 6px 14px;\n"
"  border-radius: 9999px;\n"
"  white-space: nowrap;\n"
"}\n"
".nav-item svg {\n"
"  width: 19px;\n"
"  height: 19px;\n"
"  stroke: currentColor;\n"
"  fill: none;\n"
"  stroke-width: 2;\n"
"  stroke-linecap: round;\n"
"  stroke-linejoin: round;\n"
"}\n"
".nav-item:active { transform: scale(0.92); }\n"
".nav-item.active {\n"
"  color: #fff;\n"
"  background: var(--accent-blue);\n"
"  box-shadow: 0 4px 14px rgba(0, 122, 255, 0.45);\n"
"}\n"
"/* Apple Glassmorphic Floating Selection Bar */\n"
".selection-bar {\n"
"  position: fixed;\n"
"  bottom: 84px;\n"
"  left: 50%;\n"
"  transform: translateX(-50%) translateY(140px);\n"
"  z-index: 200;\n"
"  width: calc(100% - 32px);\n"
"  max-width: 500px;\n"
"  background: rgba(28, 28, 34, 0.90);\n"
"  backdrop-filter: var(--glass-blur);\n"
"  -webkit-backdrop-filter: var(--glass-blur);\n"
"  border: 1px solid rgba(255, 255, 255, 0.20);\n"
"  border-radius: 9999px;\n"
"  padding: 8px 16px;\n"
"  display: flex;\n"
"  align-items: center;\n"
"  justify-content: space-between;\n"
"  box-shadow: 0 10px 40px rgba(0, 0, 0, 0.8), 0 0 20px rgba(0, 122, 255, 0.25);\n"
"  transition: transform 0.35s cubic-bezier(0.175, 0.885, 0.32, 1.275);\n"
"}\n"
".selection-bar.visible { transform: translateX(-50%) translateY(0); }\n"
".sel-count { font-size: 14px; font-weight: 700; color: #fff; }\n"
".sel-actions { display: flex; gap: 8px; align-items: center; }\n"
".btn-glass {\n"
"  background: var(--accent-blue);\n"
"  color: #fff; border: none; padding: 8px 16px; border-radius: 16px;\n"
"  font-size: 13px; font-weight: 700; cursor: pointer;\n"
"  display: flex; align-items: center; gap: 6px;\n"
"  box-shadow: 0 4px 12px rgba(0, 122, 255, 0.4);\n"
"}\n"
".btn-glass svg { width: 15px; height: 15px; stroke: #fff; fill: none; stroke-width: 2.5; stroke-linecap: round; stroke-linejoin: round; }\n"
".btn-glass:active { transform: scale(0.95); }\n"
".btn-cancel {\n"
"  background: rgba(255, 255, 255, 0.12);\n"
"  color: #fff; border: none; padding: 8px 12px; border-radius: 16px;\n"
"  font-size: 13px; font-weight: 600; cursor: pointer;\n"
"}\n"
"/* Fullscreen Lightbox */\n"
".lightbox {\n"
"  position: fixed; inset: 0; background: #000000; z-index: 999;\n"
"  display: none; flex-direction: column; justify-content: space-between;\n"
"}\n"
".lightbox.active { display: flex; }\n"
".lb-top {\n"
"  padding: 16px 20px;\n"
"  background: linear-gradient(to bottom, rgba(0,0,0,0.8), transparent);\n"
"  display: flex; align-items: center; justify-content: space-between; z-index: 10;\n"
"}\n"
".lb-info { display: flex; flex-direction: column; }\n"
".lb-title { font-size: 15px; font-weight: 700; }\n"
".lb-subtitle { font-size: 12px; color: var(--text-sub); }\n"
".lb-media-wrap {\n"
"  flex: 1; display: flex; align-items: center; justify-content: center; overflow: hidden; padding: 8px;\n"
"}\n"
".lb-media { max-width: 100%; max-height: 80vh; object-fit: contain; border-radius: 4px; }\n"
".lb-bottom {\n"
"  padding: 16px 20px calc(16px + env(safe-area-inset-bottom));\n"
"  background: linear-gradient(to top, rgba(0,0,0,0.8), transparent);\n"
"  display: flex; align-items: center; justify-content: center; gap: 20px; z-index: 10;\n"
"}\n"
"/* Toast Message */\n"
".toast {\n"
"  position: fixed; top: 80px; left: 50%; transform: translateX(-50%) translateY(-20px);\n"
"  background: rgba(30, 30, 34, 0.9);\n"
"  backdrop-filter: blur(20px);\n"
"  border: 1px solid var(--border-glass);\n"
"  color: #fff; padding: 10px 20px; border-radius: 20px;\n"
"  font-size: 13px; font-weight: 600; opacity: 0; pointer-events: none;\n"
"  transition: all 0.3s ease; z-index: 300;\n"
"}\n"
".toast.show { transform: translateX(-50%) translateY(0); opacity: 1;\n"
"}\n"
"</style>\n"
"</head>\n"
"<body>\n"
"<div class=\"top-bar\">\n"
"  <div class=\"app-title\">\n"
"    <div class=\"status-dot\"></div>\n"
"    <span>PS Vita Galería</span>\n"
"  </div>\n"
"  <div class=\"top-actions\">\n"
"    <button class=\"icon-btn\" id=\"btn-toggle-select\" onclick=\"toggleSelectMode()\" title=\"Seleccionar\">\n"
"      <svg viewBox=\"0 0 24 24\"><polyline points=\"9 11 12 14 22 4\"></polyline><path d=\"M21 12v7a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h11\"></path></svg>\n"
"    </button>\n"
"    <button class=\"icon-btn\" onclick=\"loadData()\" title=\"Actualizar\">\n"
"      <svg viewBox=\"0 0 24 24\"><polyline points=\"23 4 23 10 17 10\"></polyline><polyline points=\"1 20 1 14 7 14\"></polyline><path d=\"M3.51 9a9 9 0 0 1 14.85-3.36L23 10M1 14l4.64 4.36A9 9 0 0 0 20.49 15\"></path></svg>\n"
"    </button>\n"
"  </div>\n"
"</div>\n"
"<div class=\"container\" id=\"main-content\">\n"
"  <div id=\"gallery-sections\" style=\"text-align: center; padding: 60px 20px; color: var(--text-sub); font-size: 14px;\">Cargando fotos desde PS Vita...</div>\n"
"</div>\n"
"<!-- Floating Circular Bottom Tabs -->\n"
"<div class=\"bottom-nav\" id=\"bottom-nav\">\n"
"  <div class=\"nav-item active\" onclick=\"setFilter('all', this)\">\n"
"    <svg viewBox=\"0 0 24 24\"><rect x=\"3\" y=\"3\" width=\"18\" height=\"18\" rx=\"2\" ry=\"2\"></rect><circle cx=\"8.5\" cy=\"8.5\" r=\"1.5\"></circle><polyline points=\"21 15 16 10 5 21\"></polyline></svg>\n"
"    <span>Fotos</span>\n"
"  </div>\n"
"  <div class=\"nav-item\" onclick=\"setFilter('vitacam', this)\">\n"
"    <svg viewBox=\"0 0 24 24\"><path d=\"M23 19a2 2 0 0 1-2 2H3a2 2 0 0 1-2-2V8a2 2 0 0 1 2-2h4l2-3h6l2 3h4a2 2 0 0 1 2 2z\"></path><circle cx=\"12\" cy=\"13\" r=\"4\"></circle></svg>\n"
"    <span>VitaCam</span>\n"
"  </div>\n"
"  <div class=\"nav-item\" onclick=\"setFilter('screenshot', this)\">\n"
"    <svg viewBox=\"0 0 24 24\"><rect x=\"2\" y=\"6\" width=\"20\" height=\"12\" rx=\"2\"></rect><line x1=\"6\" y1=\"12\" x2=\"10\" y2=\"12\"></line><line x1=\"8\" y1=\"10\" x2=\"8\" y2=\"14\"></line><line x1=\"15\" y1=\"13\" x2=\"15.01\" y2=\"13\"></line><line x1=\"18\" y1=\"11\" x2=\"18.01\" y2=\"11\"></line></svg>\n"
"    <span>Capturas</span>\n"
"  </div>\n"
"  <div class=\"nav-item\" onclick=\"setFilter('photo', this)\">\n"
"    <svg viewBox=\"0 0 24 24\"><circle cx=\"12\" cy=\"12\" r=\"10\"></circle><line x1=\"14.31\" y1=\"8\" x2=\"20.05\" y2=\"17.94\"></line><line x1=\"9.69\" y1=\"8\" x2=\"21.17\" y2=\"8\"></line><line x1=\"7.38\" y1=\"12\" x2=\"13.12\" y2=\"2.06\"></line><line x1=\"9.69\" y1=\"16\" x2=\"3.95\" y2=\"6.06\"></line><line x1=\"14.31\" y1=\"16\" x2=\"2.83\" y2=\"16\"></line><line x1=\"16.62\" y1=\"12\" x2=\"10.88\" y2=\"21.94\"></line></svg>\n"
"    <span>Cámara</span>\n"
"  </div>\n"
"  <div class=\"nav-item\" onclick=\"setFilter('video', this)\">\n"
"    <svg viewBox=\"0 0 24 24\"><polygon points=\"23 7 16 12 23 17 23 7\"></polygon><rect x=\"1\" y=\"5\" width=\"15\" height=\"14\" rx=\"2\" ry=\"2\"></rect></svg>\n"
"    <span>Videos</span>\n"
"  </div>\n"
"</div>\n"
"<!-- Apple Glassmorphic Floating Selection Bar -->\n"
"<div class=\"selection-bar\" id=\"selection-bar\">\n"
"  <div class=\"sel-count\" id=\"sel-count-text\">0 seleccionadas</div>\n"
"  <div class=\"sel-actions\">\n"
"    <button class=\"btn-glass\" onclick=\"downloadSelected()\">\n"
"      <svg viewBox=\"0 0 24 24\"><path d=\"M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4\"></path><polyline points=\"7 10 12 15 17 10\"></polyline><line x1=\"12\" y1=\"15\" x2=\"12\" y2=\"3\"></line></svg>\n"
"      <span>Descargar</span>\n"
"    </button>\n"
"    <button class=\"btn-cancel\" onclick=\"toggleSelectMode()\">Cancelar</button>\n"
"  </div>\n"
"</div>\n"
"<!-- Lightbox Viewer -->\n"
"<div class=\"lightbox\" id=\"lightbox\" onclick=\"closeLightbox()\">\n"
"  <div class=\"lb-top\" onclick=\"event.stopPropagation()\">\n"
"    <button class=\"icon-btn\" onclick=\"closeLightbox()\">\n"
"      <svg viewBox=\"0 0 24 24\"><line x1=\"18\" y1=\"6\" x2=\"6\" y2=\"18\"></line><line x1=\"6\" y1=\"6\" x2=\"18\" y2=\"18\"></line></svg>\n"
"    </button>\n"
"    <div class=\"lb-info\">\n"
"      <div class=\"lb-title\" id=\"lb-title\">Foto</div>\n"
"      <div class=\"lb-subtitle\" id=\"lb-subtitle\">--</div>\n"
"    </div>\n"
"    <a class=\"icon-btn\" id=\"lb-dl-btn\" href=\"#\" download=\"photo.jpg\">\n"
"      <svg viewBox=\"0 0 24 24\"><path d=\"M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4\"></path><polyline points=\"7 10 12 15 17 10\"></polyline><line x1=\"12\" y1=\"15\" x2=\"12\" y2=\"3\"></line></svg>\n"
"    </a>\n"
"  </div>\n"
"  <div class=\"lb-media-wrap\" id=\"lb-media-wrap\" onclick=\"event.stopPropagation()\"></div>\n"
"  <div class=\"lb-bottom\" onclick=\"event.stopPropagation()\"></div>\n"
"</div>\n"
"<div class=\"toast\" id=\"toast\">Mensaje</div>\n"
"<script>\n"
"let allItems = [];\n"
"let currentFilter = 'all';\n"
"let isSelectMode = false;\n"
"let selectedItems = new Set();\n"
"async function loadData() {\n"
"  try {\n"
"    const res = await fetch('/api/list');\n"
"    if (res.status === 401) {\n"
"      window.location.href = '/?err=1';\n"
"      return;\n"
"    }\n"
"    allItems = await res.json();\n"
"    allItems.sort((a, b) => (Number(b.timestamp) || 0) - (Number(a.timestamp) || 0));\n"
"    renderGallery();\n"
"    showToast(allItems.length + ' elementos cargados');\n"
"  } catch(e) {\n"
"    document.getElementById('gallery-sections').innerHTML = '<div style=\"padding: 40px; color: #ff453a;\">Error al conectar con PS Vita.</div>';\n"
"  }\n"
"}\n"
"function showToast(msg) {\n"
"  const t = document.getElementById('toast');\n"
"  t.innerText = msg;\n"
"  t.classList.add('show');\n"
"  setTimeout(() => t.classList.remove('show'), 2200);\n"
"}\n"
"function setFilter(f, el) {\n"
"  currentFilter = f;\n"
"  document.querySelectorAll('.nav-item').forEach(item => item.classList.remove('active'));\n"
"  if (el) el.classList.add('active');\n"
"  renderGallery();\n"
"}\n"
"function toggleSelectMode() {\n"
"  isSelectMode = !isSelectMode;\n"
"  document.getElementById('btn-toggle-select').classList.toggle('active', isSelectMode);\n"
"  document.body.classList.toggle('selecting', isSelectMode);\n"
"  if (!isSelectMode) {\n"
"    selectedItems.clear();\n"
"    document.querySelectorAll('.photo-item').forEach(i => i.classList.remove('selected'));\n"
"  }\n"
"  updateSelectionBar();\n"
"}\n"
"function toggleItemSelection(path, el) {\n"
"  if (selectedItems.has(path)) {\n"
"    selectedItems.delete(path);\n"
"    if (el) el.classList.remove('selected');\n"
"  } else {\n"
"    selectedItems.add(path);\n"
"    if (el) el.classList.add('selected');\n"
"  }\n"
"  updateSelectionBar();\n"
"}\n"
"function onCheckClick(e, encPath, el) {\n"
"  e.stopPropagation();\n"
"  const path = decodeURIComponent(encPath);\n"
"  if (!isSelectMode) toggleSelectMode();\n"
"  toggleItemSelection(path, el);\n"
"}\n"
"function updateSelectionBar() {\n"
"  const bar = document.getElementById('selection-bar');\n"
"  const countTxt = document.getElementById('sel-count-text');\n"
"  const cnt = selectedItems.size;\n"
"  countTxt.innerText = cnt === 1 ? '1 seleccionada' : cnt + ' seleccionadas';\n"
"  if (isSelectMode && cnt > 0) {\n"
"    bar.classList.add('visible');\n"
"  } else {\n"
"    bar.classList.remove('visible');\n"
"  }\n"
"}\n"
"function renderGallery() {\n"
"  const container = document.getElementById('gallery-sections');\n"
"  const filtered = allItems.filter(item => {\n"
"    if (currentFilter === 'all') return true;\n"
"    if (currentFilter === 'video') return item.is_video;\n"
"    if (currentFilter === 'vitacam') return item.source === 'vitacam';\n"
"    if (currentFilter === 'photo') return item.source === 'photo';\n"
"    if (currentFilter === 'screenshot') return item.source === 'screenshot';\n"
"    return true;\n"
"  });\n"
"  filtered.sort((a, b) => (Number(b.timestamp) || 0) - (Number(a.timestamp) || 0));\n"
"  if (filtered.length === 0) {\n"
"    container.innerHTML = '<div style=\"text-align: center; padding: 60px 20px; color: var(--text-sub);\">No se encontraron elementos.</div>';\n"
"    return;\n"
"  }\n"
"  // Agrupar por fecha en estricto orden cronológico descendente\n"
"  const groupOrder = [];\n"
"  const groups = {};\n"
"  filtered.forEach(item => {\n"
"    const key = item.date_group || 'Reciente';\n"
"    if (!groups[key]) {\n"
"      groups[key] = [];\n"
"      groupOrder.push(key);\n"
"    }\n"
"    groups[key].push(item);\n"
"  });\n"
"  let html = '';\n"
"  for (const dateTitle of groupOrder) {\n"
"    const items = groups[dateTitle];\n"
"    html += `\n"
"      <div class=\"date-section\">\n"
"        <div class=\"date-header\">\n"
"          <div>\n"
"            <span class=\"date-title\">${dateTitle}</span>\n"
"            <span class=\"date-count\">${items.length}</span>\n"
"          </div>\n"
"        </div>\n"
"        <div class=\"grid\">\n"
"          ${items.map(item => `\n"
"            <div class=\"photo-item ${selectedItems.has(item.path) ? 'selected' : ''}\" data-path=\"${item.path}\" onclick=\"onItemClick(event, '${encodeURIComponent(item.path)}', ${item.is_video}, this)\">\n"
"              <img src=\"/thumb?path=${encodeURIComponent(item.path)}\" loading=\"lazy\" alt=\"\">\n"
"              ${item.is_video ? '<div class=\"vid-indicator\"><svg viewBox=\"0 0 24 24\"><polygon points=\"5 3 19 12 5 21 5 3\"></polygon></svg><span>VID</span></div>' : ''}\n"
"              <div class=\"check-circle\" onclick=\"onCheckClick(event, '${encodeURIComponent(item.path)}', this.closest('.photo-item'))\"><svg viewBox=\"0 0 24 24\"><polyline points=\"20 6 9 17 4 12\"></polyline></svg></div>\n"
"            </div>\n"
"          `).join('')}\n"
"        </div>\n"
"      </div>\n"
"    `;\n"
"  }\n"
"  container.innerHTML = html;\n"
"}\n"
"function onItemClick(e, encPath, isVid, el) {\n"
"  const path = decodeURIComponent(encPath);\n"
"  if (isSelectMode) {\n"
"    toggleItemSelection(path, el);\n"
"  } else {\n"
"    openLightbox(path, isVid);\n"
"  }\n"
"}\n"
"function openLightbox(path, isVid) {\n"
"  const item = allItems.find(i => i.path === path);\n"
"  const wrap = document.getElementById('lb-media-wrap');\n"
"  document.getElementById('lb-title').innerText = item ? item.date_group : 'Detalle';\n"
"  document.getElementById('lb-subtitle').innerText = item ? (item.time_str + ' • ' + (item.size/(1024*1024)).toFixed(2) + ' MB') : '';\n"
"  const dl = document.getElementById('lb-dl-btn');\n"
"  dl.href = `/download?path=${encodeURIComponent(path)}`;\n"
"  dl.download = item ? item.name : 'media';\n"
"  if (isVid) {\n"
"    wrap.innerHTML = `<video class=\"lb-media\" src=\"/view?path=${encodeURIComponent(path)}\" controls autoplay></video>`;\n"
"  } else {\n"
"    wrap.innerHTML = `<img class=\"lb-media\" src=\"/view?path=${encodeURIComponent(path)}\" alt=\"\">`;\n"
"  }\n"
"  document.getElementById('lightbox').classList.add('active');\n"
"}\n"
"function closeLightbox() {\n"
"  document.getElementById('lightbox').classList.remove('active');\n"
"  document.getElementById('lb-media-wrap').innerHTML = '';\n"
"}\n"
"async function downloadSelected() {\n"
"  const list = Array.from(selectedItems);\n"
"  if (!list.length) return;\n"
"  showToast('Iniciando descarga de ' + list.length + ' archivos...');\n"
"  for (let i = 0; i < list.length; i++) {\n"
"    const p = list[i];\n"
"    const item = allItems.find(x => x.path === p);\n"
"    const a = document.createElement('a');\n"
"    a.href = `/download?path=${encodeURIComponent(p)}`;\n"
"    a.download = item ? item.name : 'media';\n"
"    document.body.appendChild(a);\n"
"    a.click();\n"
"    a.remove();\n"
"    await new Promise(r => setTimeout(r, 450));\n"
"  }\n"
"  showToast('Descargas completadas');\n"
"}\n"
"const urlParams = new URLSearchParams(window.location.search);\n"
"const pin = urlParams.get('pin') || urlParams.get('key') || urlParams.get('pwd');\n"
"if (pin) {\n"
"  document.cookie = 'vitapass=' + encodeURIComponent(pin) + '; path=/; max-age=2592000; SameSite=Lax';\n"
"  window.history.replaceState({}, document.title, window.location.pathname);\n"
"}\n"
"loadData();\n"
"</script>\n"
"</body>\n"
"</html>\n";

static const char *LOGIN_PAGE =
"<!DOCTYPE html>\n"
"<html lang=\"es\">\n"
"<head>\n"
"<meta charset=\"UTF-8\">\n"
"<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no\">\n"
"<title>VitaCam Hub - Acceso Privado</title>\n"
"<style>\n"
":root {\n"
"  --bg-color: #05070c;\n"
"  --surface-trans: rgba(20, 24, 38, 0.85);\n"
"  --accent-blue: #007aff;\n"
"  --text-main: #ffffff;\n"
"  --text-sub: #8e9bb0;\n"
"  --border-glass: rgba(255, 255, 255, 0.14);\n"
"}\n"
"* { box-sizing: border-box; margin: 0; padding: 0; font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif; }\n"
"body {\n"
"  background: radial-gradient(circle at top, #141f36, #05070c);\n"
"  color: var(--text-main);\n"
"  min-height: 100vh;\n"
"  display: flex; align-items: center; justify-content: center;\n"
"  padding: 20px;\n"
"}\n"
".card {\n"
"  background: var(--surface-trans);\n"
"  backdrop-filter: blur(20px); -webkit-backdrop-filter: blur(20px);\n"
"  border: 1px solid var(--border-glass);\n"
"  border-radius: 24px;\n"
"  padding: 36px 24px;\n"
"  max-width: 360px; width: 100%;\n"
"  text-align: center;\n"
"  box-shadow: 0 20px 48px rgba(0, 0, 0, 0.6);\n"
"}\n"
".icon-box {\n"
"  width: 58px; height: 58px; border-radius: 16px;\n"
"  background: linear-gradient(135deg, #007aff, #5856d6);\n"
"  margin: 0 auto 16px; display: flex; align-items: center; justify-content: center;\n"
"  box-shadow: 0 8px 20px rgba(0, 122, 255, 0.35);\n"
"}\n"
".icon-box svg { width: 30px; height: 30px; stroke: #fff; fill: none; stroke-width: 2; stroke-linecap: round; stroke-linejoin: round; }\n"
"h1 { font-size: 22px; font-weight: 700; margin-bottom: 8px; letter-spacing: -0.3px; }\n"
"p { font-size: 14px; color: var(--text-sub); line-height: 1.45; margin-bottom: 24px; }\n"
".pin-input {\n"
"  width: 100%;\n"
"  background: rgba(255, 255, 255, 0.08);\n"
"  border: 1px solid var(--border-glass);\n"
"  border-radius: 14px;\n"
"  color: #fff;\n"
"  font-size: 26px; font-weight: 700; letter-spacing: 6px; text-align: center;\n"
"  padding: 12px; outline: none; transition: all 0.2s;\n"
"  margin-bottom: 16px;\n"
"}\n"
".pin-input:focus { border-color: var(--accent-blue); box-shadow: 0 0 16px rgba(0, 122, 255, 0.4); }\n"
".btn-submit {\n"
"  width: 100%; background: var(--accent-blue); color: #fff;\n"
"  border: none; border-radius: 14px;\n"
"  font-size: 16px; font-weight: 600; padding: 14px;\n"
"  cursor: pointer; transition: transform 0.1s, opacity 0.2s;\n"
"}\n"
".btn-submit:active { transform: scale(0.98); opacity: 0.9; }\n"
".err-hint { color: #ff453a; font-size: 13px; margin-top: 14px; display: none; font-weight: 500; }\n"
"</style>\n"
"</head>\n"
"<body>\n"
"<div class=\"card\">\n"
"  <div class=\"icon-box\">\n"
"    <svg viewBox=\"0 0 24 24\"><rect x=\"3\" y=\"11\" width=\"18\" height=\"11\" rx=\"2\" ry=\"2\"/><path d=\"M7 11V7a5 5 0 0 1 10 0v4\"/></svg>\n"
"  </div>\n"
"  <h1>VitaCam Hub</h1>\n"
"  <p>Galería privada. Introduce el PIN de acceso que se muestra en tu PS Vita:</p>\n"
"  <form onsubmit=\"doAuth(event)\">\n"
"    <input type=\"text\" id=\"pinIn\" class=\"pin-input\" placeholder=\"••••\" maxlength=\"8\" inputmode=\"numeric\" autofocus autocomplete=\"off\">\n"
"    <button type=\"submit\" class=\"btn-submit\">Desbloquear Galería</button>\n"
"  </form>\n"
"  <div class=\"err-hint\" id=\"errHint\">PIN incorrecto. Verifica el PIN en la Vita.</div>\n"
"</div>\n"
"<script>\n"
"function doAuth(e) {\n"
"  e.preventDefault();\n"
"  const p = document.getElementById('pinIn').value.trim();\n"
"  if (!p) return;\n"
"  document.cookie = 'vitapass=' + encodeURIComponent(p) + '; path=/; max-age=2592000; SameSite=Lax';\n"
"  window.location.href = '/?pin=' + encodeURIComponent(p);\n"
"}\n"
"if (window.location.search.includes('err=1')) {\n"
"  document.getElementById('errHint').style.display = 'block';\n"
"}\n"
"</script>\n"
"</body>\n"
"</html>\n";

// ── Escaneo Recursivo de Archivos para la API JSON ─────────────────────────
static const char *month_names_es[12] = {
    "Ene", "Feb", "Mar", "Abr", "May", "Jun",
    "Jul", "Ago", "Sep", "Oct", "Nov", "Dic"
};

static void parse_file_date_json(const char *name, const SceIoStat *st, char *date_group, size_t dg_len, char *time_str, size_t t_len, uint64_t *timestamp) {
    int y = 0, m = 0, d = 0, hh = 12, mm = 0, ss = 0;
    if ((sscanf(name, "photo_%4d%2d%2d_%2d%2d%2d", &y, &m, &d, &hh, &mm, &ss) >= 5 ||
         sscanf(name, "VID_%4d%2d%2d_%2d%2d%2d", &y, &m, &d, &hh, &mm, &ss) >= 5 ||
         sscanf(name, "video_%4d%2d%2d_%2d%2d%2d", &y, &m, &d, &hh, &mm, &ss) >= 5 ||
         sscanf(name, "IMG_%4d%2d%2d_%2d%2d%2d", &y, &m, &d, &hh, &mm, &ss) >= 5) &&
        y > 2000 && m >= 1 && m <= 12 && d >= 1 && d <= 31) {
    } else if (st && st->st_ctime.year > 2000) {
        y = st->st_ctime.year;
        m = st->st_ctime.month;
        d = st->st_ctime.day;
        hh = st->st_ctime.hour;
        mm = st->st_ctime.minute;
        ss = st->st_ctime.second;
    } else {
        y = 2026; m = 1; d = 1; hh = 12; mm = 0; ss = 0;
    }

    *timestamp = ((uint64_t)y * 10000000000ULL) +
                 ((uint64_t)m * 100000000ULL) +
                 ((uint64_t)d * 1000000ULL) +
                 ((uint64_t)hh * 10000ULL) +
                 ((uint64_t)mm * 100ULL) +
                 (uint64_t)ss;

    int h12 = hh % 12;
    if (h12 == 0) h12 = 12;
    const char *ampm = (hh >= 12) ? "PM" : "AM";
    snprintf(time_str, t_len, "%d:%02d %s", h12, mm, ampm);

    const char *mname = (m >= 1 && m <= 12) ? month_names_es[m - 1] : "Mes";
    snprintf(date_group, dg_len, "%d %s %04d", d, mname, y);
}

static const char *case_str_search(const char *haystack, const char *needle) {
    if (!haystack || !needle) return NULL;
    size_t nlen = strlen(needle);
    while (*haystack) {
        if (strncasecmp(haystack, needle, nlen) == 0) return haystack;
        haystack++;
    }
    return NULL;
}

static void scan_directory_json_recursive(const char *dir_path, const char *source_name, char *json_buf, size_t max_len, int *is_first, int depth) {
    if (depth > 4) return;

    SceUID dfd = sceIoDopen(dir_path);
    if (dfd < 0) return;

    SceIoDirent dirent;
    memset(&dirent, 0, sizeof(dirent));

    while (sceIoDread(dfd, &dirent) > 0) {
        const char *name = dirent.d_name;
        if (name[0] == '.') {
            memset(&dirent, 0, sizeof(dirent));
            continue;
        }

        char subpath[256];
        snprintf(subpath, sizeof(subpath), "%s/%s", dir_path, name);

        const char *effective_source = source_name;
        if (case_str_search(subpath, "screenshot")) {
            effective_source = "screenshot";
        }

        int is_dir = (SCE_S_ISDIR(dirent.d_stat.st_mode) || (dirent.d_stat.st_attr & 0x10));
        if (!is_dir) {
            SceUID sub_dfd = sceIoDopen(subpath);
            if (sub_dfd >= 0) {
                sceIoDclose(sub_dfd);
                is_dir = 1;
            }
        }

        if (is_dir) {
            scan_directory_json_recursive(subpath, effective_source, json_buf, max_len, is_first, depth + 1);
            memset(&dirent, 0, sizeof(dirent));
            continue;
        }



        int len = strlen(name);
        if (len > 4) {
            const char *ext = name + len - 4;
            const char *ext5 = (len > 5) ? name + len - 5 : "";
            int is_jpg = (strcasecmp(ext, ".jpg") == 0 || strcasecmp(ext5, ".jpeg") == 0 || strcasecmp(ext, ".png") == 0 || strcasecmp(ext, ".bmp") == 0);
            int is_avi = (strcasecmp(ext, ".avi") == 0 || strcasecmp(ext, ".mp4") == 0);

            if (is_jpg || is_avi) {
                if (is_jpg && strncmp(name, "VID_", 4) == 0) {
                    char avi_check[256];
                    snprintf(avi_check, sizeof(avi_check), "%s/%s", dir_path, name);
                    int clen = strlen(avi_check);
                    if (clen > 4) {
                        strcpy(avi_check + clen - 4, ".avi");
                        SceIoStat st;
                        if (sceIoGetstat(avi_check, &st) >= 0) {
                            memset(&dirent, 0, sizeof(dirent));
                            continue;
                        }
                    }
                }

                char date_grp[64];
                char time_str[32];
                uint64_t ts = 0;
                parse_file_date_json(name, &dirent.d_stat, date_grp, sizeof(date_grp), time_str, sizeof(time_str), &ts);

                char item_json[512];
                snprintf(item_json, sizeof(item_json),
                    "%s{\"name\":\"%s\",\"path\":\"%s\",\"is_video\":%s,\"size\":%llu,\"source\":\"%s\",\"date_group\":\"%s\",\"time_str\":\"%s\",\"timestamp\":%llu}",
                    (*is_first) ? "" : ",",
                    name, subpath, is_avi ? "true" : "false",
                    (unsigned long long)dirent.d_stat.st_size, effective_source,
                    date_grp, time_str, (unsigned long long)ts);

                *is_first = 0;
                size_t cur_len = strlen(json_buf);
                if (cur_len + strlen(item_json) + 4 < max_len) {
                    strcat(json_buf, item_json);
                }
            }
        }
        memset(&dirent, 0, sizeof(dirent));
    }
    sceIoDclose(dfd);
}

static void url_decode(char *dst, const char *src) {
    char a, b;
    while (*src) {
        if ((*src == '%') && ((a = src[1]) && (b = src[2])) && (isxdigit(a) && isxdigit(b))) {
            if (a >= 'a') a -= 'a' - 'A';
            if (a >= 'A') a -= ('A' - 10);
            else a -= '0';
            if (b >= 'a') b -= 'a' - 'A';
            if (b >= 'A') b -= ('A' - 10);
            else b -= '0';
            *dst++ = 16 * a + b;
            src += 3;
        } else if (*src == '+') {
            *dst++ = ' ';
            src++;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

// ── Autenticación Privada con PIN / Cookies / URL / Basic Auth ─────────────
static int check_cookie_pin(const char *req_buf, const char *pin) {
    if (!pin || !*pin) return 1;
    char search_pattern[48];
    snprintf(search_pattern, sizeof(search_pattern), "vitapass=%s", pin);
    const char *pos = case_str_search(req_buf, search_pattern);
    if (!pos) return 0;
    size_t plen = strlen(search_pattern);
    char next_char = pos[plen];
    if (next_char == ';' || next_char == ' ' || next_char == '\r' || next_char == '\n' || next_char == '\0') {
        return 1;
    }
    return 0;
}

static int check_query_pin(const char *uri, const char *pin) {
    if (!pin || !*pin) return 1;
    const char *q = strchr(uri, '?');
    if (!q) return 0;
    const char *keys[] = { "pin=", "key=", "pwd=", NULL };
    for (int k = 0; keys[k]; k++) {
        const char *found = strstr(q, keys[k]);
        while (found) {
            if (found == q + 1 || *(found - 1) == '&' || *(found - 1) == '?') {
                const char *val = found + strlen(keys[k]);
                size_t pin_len = strlen(pin);
                if (strncmp(val, pin, pin_len) == 0) {
                    char term = val[pin_len];
                    if (term == '&' || term == ' ' || term == '\0' || term == '#') {
                        return 1;
                    }
                }
            }
            found = strstr(found + 1, keys[k]);
        }
    }
    return 0;
}

static int check_basic_auth(const char *req_buf, const char *pin) {
    if (!pin || !*pin) return 1;
    char auth_str[128];
    char b64[256];
    char expected_header[512];

    snprintf(auth_str, sizeof(auth_str), "vitacam:%s", pin);
    base64_encode((const unsigned char*)auth_str, strlen(auth_str), b64);
    snprintf(expected_header, sizeof(expected_header), "Authorization: Basic %s", b64);
    if (case_str_search(req_buf, expected_header)) return 1;

    snprintf(auth_str, sizeof(auth_str), ":%s", pin);
    base64_encode((const unsigned char*)auth_str, strlen(auth_str), b64);
    snprintf(expected_header, sizeof(expected_header), "Authorization: Basic %s", b64);
    if (case_str_search(req_buf, expected_header)) return 1;

    return 0;
}

static int is_request_authorized(const char *req_buf, const char *uri) {
    if (web_password[0] == '\0') {
        return 1;
    }
    if (check_query_pin(uri, web_password)) {
        return 1;
    }
    if (check_cookie_pin(req_buf, web_password)) {
        return 1;
    }
    if (check_basic_auth(req_buf, web_password)) {
        return 1;
    }
    return 0;
}

// ── Manejo de Conexiones HTTP ─────────────────────────────────────────────
static void handle_http_client(int client_sock) {
    char req_buf[2048];
    int r = sceNetRecv(client_sock, req_buf, sizeof(req_buf) - 1, 0);
    if (r <= 0) {
        sceNetSocketClose(client_sock);
        return;
    }
    req_buf[r] = '\0';

    char method[16], uri[512], proto[16];
    if (sscanf(req_buf, "%15s %511s %15s", method, uri, proto) < 2) {
        sceNetSocketClose(client_sock);
        return;
    }

    int authed = is_request_authorized(req_buf, uri);
    int is_root_page = (strcmp(uri, "/") == 0 || strcmp(uri, "/index.html") == 0 ||
                        strncmp(uri, "/?", 2) == 0 || strncmp(uri, "/index.html?", 12) == 0);

    if (!authed) {
        if (is_root_page) {
            char header[256];
            int login_len = strlen(LOGIN_PAGE);
            snprintf(header, sizeof(header),
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/html; charset=utf-8\r\n"
                "Content-Length: %d\r\n"
                "Connection: close\r\n\r\n", login_len);
            sceNetSend(client_sock, header, strlen(header), 0);
            sceNetSend(client_sock, LOGIN_PAGE, login_len, 0);
            sceNetSocketClose(client_sock);
            return;
        }

        const char *auth_req = "HTTP/1.1 401 Unauthorized\r\n"
                               "Content-Type: application/json; charset=utf-8\r\n"
                               "Content-Length: 26\r\n"
                               "Connection: close\r\n\r\n"
                               "{\"error\":\"Unauthorized\"}\n";
        sceNetSend(client_sock, auth_req, strlen(auth_req), 0);
        sceNetSocketClose(client_sock);
        return;
    }

    if (is_root_page) {
        char header[384];
        int page_len = strlen(HTML_PAGE);
        if (web_password[0] != '\0') {
            snprintf(header, sizeof(header),
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/html; charset=utf-8\r\n"
                "Set-Cookie: vitapass=%s; Path=/; Max-Age=2592000; SameSite=Lax\r\n"
                "Content-Length: %d\r\n"
                "Connection: close\r\n\r\n", web_password, page_len);
        } else {
            snprintf(header, sizeof(header),
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/html; charset=utf-8\r\n"
                "Content-Length: %d\r\n"
                "Connection: close\r\n\r\n", page_len);
        }
        sceNetSend(client_sock, header, strlen(header), 0);
        sceNetSend(client_sock, HTML_PAGE, page_len, 0);
    }
    else if (strcmp(uri, "/api/list") == 0) {
        char *json_buf = malloc(512 * 1024);
        if (json_buf) {
            strcpy(json_buf, "[");
            int is_first = 1;
            scan_directory_json_recursive("ux0:data/vitacam", "vitacam", json_buf, 512 * 1024, &is_first, 0);
            scan_directory_json_recursive("ux0:picture/CAMERA", "photo", json_buf, 512 * 1024, &is_first, 0);
            scan_directory_json_recursive("ux0:picture/SCREENSHOT", "screenshot", json_buf, 512 * 1024, &is_first, 0);
            strcat(json_buf, "]");

            char header[256];
            int jlen = strlen(json_buf);
            snprintf(header, sizeof(header),
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: application/json; charset=utf-8\r\n"
                "Content-Length: %d\r\n"
                "Access-Control-Allow-Origin: *\r\n"
                "Connection: close\r\n\r\n", jlen);
            sceNetSend(client_sock, header, strlen(header), 0);
            sceNetSend(client_sock, json_buf, jlen, 0);
            free(json_buf);
        }
    }
    else if (strcmp(uri, "/api/debug") == 0) {
        // Endpoint de diagnostico: prueba sceIoDopen en todas las rutas conocidas
        char *dbg = malloc(64 * 1024);
        if (dbg) {
            dbg[0] = '\0';
            const char *test_paths[] = {
                "ux0:data/vitacam",
                "ux0:picture",
                "ux0:picture/CAMERA",
                "ux0:picture/SCREENSHOT",
                "ux0:picture/CAMERA/gf",
                "ux0:picture/CAMERA/kk",
                "ux0:picture/CAMERA/bb",
                "ux0:picture/SCREENSHOT/bb",
                "ux0:picture/SCREENSHOT/dh",
                NULL
            };
            char line[512];
            snprintf(line, sizeof(line), "=== VitaCam Debug - Directory Probe ===\n");
            strncat(dbg, line, 64*1024 - strlen(dbg) - 1);

            for (int i = 0; test_paths[i] != NULL; i++) {
                SceUID dfd = sceIoDopen(test_paths[i]);
                if (dfd < 0) {
                    snprintf(line, sizeof(line), "OPEN FAIL  [0x%08X]: %s\n", (unsigned int)dfd, test_paths[i]);
                    strncat(dbg, line, 64*1024 - strlen(dbg) - 1);
                } else {
                    int file_count = 0, dir_count = 0;
                    SceIoDirent de;
                    memset(&de, 0, sizeof(de));
                    while (sceIoDread(dfd, &de) > 0) {
                        int is_d = (SCE_S_ISDIR(de.d_stat.st_mode) || (de.d_stat.st_attr & 0x10));
                        if (de.d_name[0] != '.') {
                            if (is_d) dir_count++;
                            else file_count++;
                        }
                        memset(&de, 0, sizeof(de));
                    }
                    sceIoDclose(dfd);
                    snprintf(line, sizeof(line), "OK  [dirs=%d files=%d]: %s\n", dir_count, file_count, test_paths[i]);
                    strncat(dbg, line, 64*1024 - strlen(dbg) - 1);
                }
            }

            // Tambien listar CAMERA subfolders dinamicamente
            snprintf(line, sizeof(line), "\n--- CAMERA subdirs contents ---\n");
            strncat(dbg, line, 64*1024 - strlen(dbg) - 1);
            SceUID camfd = sceIoDopen("ux0:picture/CAMERA");
            if (camfd >= 0) {
                SceIoDirent cde;
                memset(&cde, 0, sizeof(cde));
                while (sceIoDread(camfd, &cde) > 0) {
                    if (cde.d_name[0] != '.') {
                        char camsubpath[128];
                        snprintf(camsubpath, sizeof(camsubpath), "ux0:picture/CAMERA/%s", cde.d_name);
                        SceUID sfd = sceIoDopen(camsubpath);
                        if (sfd >= 0) {
                            int fc = 0;
                            SceIoDirent sde;
                            memset(&sde, 0, sizeof(sde));
                            while (sceIoDread(sfd, &sde) > 0) {
                                if (sde.d_name[0] != '.') {
                                    snprintf(line, sizeof(line), "  CAMERA/%s/%s [size=%d]\n", cde.d_name, sde.d_name, (int)sde.d_stat.st_size);
                                    strncat(dbg, line, 64*1024 - strlen(dbg) - 1);
                                    fc++;
                                }
                                memset(&sde, 0, sizeof(sde));
                            }
                            sceIoDclose(sfd);
                            if (fc == 0) {
                                snprintf(line, sizeof(line), "  CAMERA/%s/ (VACIA)\n", cde.d_name);
                                strncat(dbg, line, 64*1024 - strlen(dbg) - 1);
                            }
                        }
                    }
                    memset(&cde, 0, sizeof(cde));
                }
                sceIoDclose(camfd);
            }

            int dbg_len = strlen(dbg);
            char header[256];
            snprintf(header, sizeof(header),
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/plain; charset=utf-8\r\n"
                "Content-Length: %d\r\n"
                "Connection: close\r\n\r\n", dbg_len);
            sceNetSend(client_sock, header, strlen(header), 0);
            sceNetSend(client_sock, dbg, dbg_len, 0);
            free(dbg);
        }
    }
    else if (strncmp(uri, "/view?path=", 11) == 0 || strncmp(uri, "/download?path=", 15) == 0 || strncmp(uri, "/thumb?path=", 12) == 0) {
        int is_download = (strncmp(uri, "/download?path=", 15) == 0);
        int is_thumb = (strncmp(uri, "/thumb?path=", 12) == 0);
        const char *encoded_path = is_download ? uri + 15 : (is_thumb ? uri + 12 : uri + 11);

        char filepath[256];
        url_decode(filepath, encoded_path);

        if (is_thumb) {
            int flen = strlen(filepath);
            if (flen > 4 && strcasecmp(filepath + flen - 4, ".avi") == 0) {
                char thumb_path[256];
                strncpy(thumb_path, filepath, sizeof(thumb_path) - 1);
                strcpy(thumb_path + flen - 4, ".jpg");
                SceIoStat st;
                if (sceIoGetstat(thumb_path, &st) >= 0) {
                    strcpy(filepath, thumb_path);
                }
            }
        }

        SceUID fd = sceIoOpen(filepath, SCE_O_RDONLY, 0);
        if (fd >= 0) {
            SceIoStat st;
            sceIoGetstat(filepath, &st);
            uint32_t file_size = (uint32_t)st.st_size;

            const char *mime = "image/jpeg";
            int flen = strlen(filepath);
            if (flen > 4 && strcasecmp(filepath + flen - 4, ".avi") == 0) {
                mime = "video/x-msvideo";
            } else if (flen > 4 && strcasecmp(filepath + flen - 4, ".png") == 0) {
                mime = "image/png";
            }

            const char *base_name = strrchr(filepath, '/');
            base_name = base_name ? base_name + 1 : filepath;

            char header[512];
            if (is_download) {
                snprintf(header, sizeof(header),
                    "HTTP/1.1 200 OK\r\n"
                    "Content-Type: %s\r\n"
                    "Content-Length: %u\r\n"
                    "Content-Disposition: attachment; filename=\"%s\"\r\n"
                    "Connection: close\r\n\r\n", mime, file_size, base_name);
            } else {
                snprintf(header, sizeof(header),
                    "HTTP/1.1 200 OK\r\n"
                    "Content-Type: %s\r\n"
                    "Content-Length: %u\r\n"
                    "Connection: close\r\n\r\n", mime, file_size);
            }
            sceNetSend(client_sock, header, strlen(header), 0);

            char *stream_buf = malloc(32 * 1024);
            if (stream_buf) {
                int read_bytes;
                while ((read_bytes = sceIoRead(fd, stream_buf, 32 * 1024)) > 0) {
                    if (sceNetSend(client_sock, stream_buf, read_bytes, 0) < 0) break;
                }
                free(stream_buf);
            }
            sceIoClose(fd);
        } else {
            const char *not_found = "HTTP/1.1 404 Not Found\r\nContent-Length: 13\r\nConnection: close\r\n\r\nFile Not Found";
            sceNetSend(client_sock, not_found, strlen(not_found), 0);
        }
    }
    else {
        const char *not_found = "HTTP/1.1 404 Not Found\r\nContent-Length: 9\r\nConnection: close\r\n\r\nNot Found";
        sceNetSend(client_sock, not_found, strlen(not_found), 0);
    }

    sceNetSocketClose(client_sock);
}

// ── Hilo del Servidor Web ─────────────────────────────────────────────────
static int webserver_worker_thread(SceSize args, void *argp) {
    (void)args;
    (void)argp;

    int server_sock = sceNetSocket("VitaCam_HTTP", SCE_NET_AF_INET, SCE_NET_SOCK_STREAM, 0);
    if (server_sock < 0) {
        server_active = 0;
        return sceKernelExitDeleteThread(0);
    }

    int opt = 1;
    sceNetSetsockopt(server_sock, SCE_NET_SOL_SOCKET, SCE_NET_SO_REUSEADDR, &opt, sizeof(opt));

    SceNetSockaddrIn server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = SCE_NET_AF_INET;
    server_addr.sin_port = sceNetHtons(HTTP_PORT);
    server_addr.sin_addr.s_addr = sceNetHtonl(SCE_NET_INADDR_ANY);

    if (sceNetBind(server_sock, (SceNetSockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        sceNetSocketClose(server_sock);
        server_active = 0;
        return sceKernelExitDeleteThread(0);
    }

    if (sceNetListen(server_sock, 8) < 0) {
        sceNetSocketClose(server_sock);
        server_active = 0;
        return sceKernelExitDeleteThread(0);
    }

    server_active = 1;

    while (server_running) {
        if (!server_enabled) {
            sceKernelDelayThread(50000);
            continue;
        }

        SceNetSockaddrIn client_addr;
        unsigned int client_len = sizeof(client_addr);
        int client_sock = sceNetAccept(server_sock, (SceNetSockaddr *)&client_addr, &client_len);
        if (client_sock >= 0) {
            handle_http_client(client_sock);
        } else {
            sceKernelDelayThread(10000);
        }
    }

    sceNetSocketClose(server_sock);
    server_active = 0;
    return sceKernelExitDeleteThread(0);
}

int webserver_init(void) {
    if (server_running) return 0;

    sceSysmoduleLoadModule(SCE_SYSMODULE_NET);

    net_mem = malloc(NET_PARAM_MEM_SIZE);
    if (net_mem) {
        SceNetInitParam net_param;
        net_param.memory = net_mem;
        net_param.size = NET_PARAM_MEM_SIZE;
        net_param.flags = 0;
        sceNetInit(&net_param);
    }
    sceNetCtlInit();

    webserver_get_password();
    webserver_get_ip();

    server_running = 1;
    server_enabled = 0; // Por defecto apagado al iniciar la app
    server_thid = sceKernelCreateThread("VitaCam_WebServer", webserver_worker_thread, 0x10000100, 0x10000, 0, 0, NULL);
    if (server_thid >= 0) {
        sceKernelStartThread(server_thid, 0, NULL);
    }

    return 0;
}

void webserver_term(void) {
    server_running = 0;
    if (server_thid >= 0) {
        sceKernelWaitThreadEnd(server_thid, NULL, NULL);
        server_thid = -1;
    }
    sceNetCtlTerm();
    sceNetTerm();
    if (net_mem) {
        free(net_mem);
        net_mem = NULL;
    }
}
