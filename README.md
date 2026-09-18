# VitaCam Pro

[![Platform](https://img.shields.io/badge/Platform-PlayStation%20Vita-blue.svg)](https://github.com/vitasdk)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![Download VPK](https://img.shields.io/badge/Download-vitacam.vpk-2ea44f?style=flat&logo=playstation)](https://github.com/darking101/vitacam/releases/latest)

> **Languages / Idiomas:** [English](#english) | [Español](#español)

---

<a name="english"></a>
## English

**VitaCam Pro** is an advanced camera application, gallery viewer, and high-speed Wi-Fi web hub for the **PlayStation Vita**, developed in native C using [VitaSDK](https://vitasdk.org/).

Auto-detects your console's language: displays in **English** by default on international systems and in **Spanish** on Spanish-configured consoles.

### Key Features
- **Native Hardware EXIF Metadata:** Automatically injects standard EXIF headers into captured JPEGs (Sony PlayStation Vita, real timestamps, f/2.8 aperture, ISO 100, and sRGB color space).
- **Realistic Shutter Sound:** Mechanical camera shutter sound and countdown beeps powered by native hardware `SceAudio` at 48,000 Hz stereo at 0 dB.
- **Self-Timer & Burst Mode:** Configurable countdown timer (Off, 3s, 5s, 10s) with on-screen visual animation and audio beeps, plus continuous burst shooting (3, 5, or 10 consecutive photos).
- **Multitouch Pinch-to-Zoom:** Smooth 1.0x to 4.0x continuous scaling using two-finger pinch gestures on the front capacitive touchscreen or the left analog stick.
- **Front Screen Flash / Softbox:** High-intensity full white screen and light ring for selfies in dark environments (flash icon stays visible on rear camera as disabled).
- **Adaptive [PS] VITA Watermark:** Official vector logo with intelligent background luminance detection (switches dynamically between pure white and high-contrast black).
- **Wi-Fi Web Hub:** Built-in lightweight HTTP server to view, download, or delete photos remotely from your smartphone or PC with real-time sync to the PS Vita memory gallery.
- **Persistent User Preferences:** Saves your camera settings (timer, burst, sound, flash, grid) automatically to `ux0:data/VitaCam/config.dat`.
- **System & RAM Telemetry:** Real-time diagnostics of free and total RAM (`sceKernelGetFreeMemorySize`) and battery percentage in the `[ (i) ]` info modal.

### Controls & Shortcuts
| Control | Camera Mode | Gallery Mode |
| :--- | :--- | :--- |
| **R Trigger / Touch** | Take photo | Next tab |
| **L Trigger** | - | Previous tab |
| **Select** | Switch Rear / Front camera | Wi-Fi Web Server & QR |
| **Triangle** | Toggle Flash mode | Wi-Fi Web Server & QR |
| **Square** | Toggle Composition Grid | Multi-select mode (batch delete) |
| **Cross (X)** | Toggle Watermark | Open photo fullscreen |
| **Left Stick / D-Pad** | Dynamic Zoom (1.0x - 4.0x) | Navigate photos |
| **Touch Pinch** | Pinch-to-Zoom (1.0x - 4.0x) | Zoom in fullscreen |
| **Top HUD Icons** | Toggle Timer, Burst, Shutter Sound | - |
| **Circle** | Switch to Gallery / Cancel timer | Back to Camera / Cancel |
| **Touch `[ (i) ]`** | - | RAM monitor & About dialog |

### Installation
1. Download the official installer `vitacam.vpk` from [Releases](https://github.com/darking101/vitacam/releases/latest).
2. Transfer `vitacam.vpk` to your PS Vita via VitaShell (USB cable or FTP).
3. In VitaShell, navigate to `vitacam.vpk` and press **Cross (X)** to install.

---

<a name="español"></a>
## Español

**VitaCam Pro** es una aplicación de cámara avanzada, visor de galería y servidor web Wi-Fi de alta velocidad para la **PlayStation Vita**, desarrollada en C nativo utilizando [VitaSDK](https://vitasdk.org/).

Detecta automáticamente el idioma de tu consola: se muestra en **español** si tu consola está configurada en español y en **inglés** para cualquier otro idioma.

### Características Principales
- **Metadatos EXIF de Hardware Nativos:** Cada captura en formato JPEG incorpora automáticamente cabeceras EXIF completas con fabricante (Sony), modelo (PlayStation Vita), fecha y hora exacta, resolución y valores de toma reales.
- **Audio Mecánico de Disparo:** Sonido realista de obturador de cámara réflex y beeps de cuenta atrás mediante hardware nativo `SceAudio` a 48,000 Hz estéreo a 0 dB.
- **Temporizador y Modo Ráfaga:** Conteo regresivo configurable (Desactivado, 3s, 5s, 10s) con animación en pantalla, y ráfaga continua (3, 5 o 10 fotos consecutivas).
- **Zoom Dinámico y Pellizco Táctil:** Control suave de 1.0x a 4.0x mediante gestos multitáctiles de pellizco (*pinch-to-zoom*) en la pantalla capacitiva o sticks analógicos.
- **Flash / Softbox Frontal:** Aro de luz y pantalla blanca completa de alta luminosidad para selfies en entornos oscuros.
- **Sello Inteligente Adaptativo [PS] VITA:** Marca de agua oficial con detección automática de luminosidad (cambia inteligentemente entre blanco puro y negro de alto contraste según el fondo).
- **Servidor Web Wi-Fi (Web Hub):** Servidor HTTP integrado para descargar o eliminar fotos desde el navegador web del teléfono o PC con sincronización en tiempo real con la galería de la Vita.
- **Persistencia de Configuración:** Guarda automáticamente todas tus preferencias de usuario en `ux0:data/VitaCam/config.dat`.
- **Monitor de Sistema en Tiempo Real:** Diagnóstico de consumo de memoria RAM (libre y total) y estado de batería en el botón `[ (i) ]`.

### Atajos y Controles
| Control | Modo Cámara | Modo Galería |
| :--- | :--- | :--- |
| **Gatillo R / Tap Pantalla** | Capturar fotografía | Siguiente pestaña |
| **Gatillo L** | - | Pestaña anterior |
| **Select** | Conmutar cámara Trasera / Frontal | Menú Servidor Web y QR |
| **Triángulo** | Alternar modo Flash | Menú Servidor Web y QR |
| **Cuadrado** | Activar / Desactivar Cuadrícula | Modo selección múltiple (borrado en lote) |
| **Cruz (X)** | Activar / Desactivar Marca de agua | Abrir foto a pantalla completa |
| **Stick Izquierdo / D-Pad** | Zoom dinámico (1.0x - 4.0x) | Navegar fotos |
| **Pellizco táctil** | Pinch-to-zoom (1.0x - 4.0x) | Zoom en pantalla completa |
| **Botonera Táctil Superior** | Alternar Temporizador, Ráfaga y Sonido | - |
| **Círculo** | Salir a Galería / Cancelar temporizador | Volver a Cámara / Cancelar |
| **Touch `[ (i) ]`** | - | Monitor de RAM y créditos |

### Instalación
1. Descarga el paquete instalable oficial `vitacam.vpk` desde la sección de [Releases](https://github.com/darking101/vitacam/releases/latest).
2. Transfiere el archivo a tu PlayStation Vita mediante VitaShell (usando cable USB o servidor FTP).
3. En VitaShell, navega hasta `vitacam.vpk` y presiona **Cruz (X)** para completar la instalación.

---

## Compilación desde Código Fuente / Building

### Requirements
- [VitaSDK](https://vitasdk.org/) installed and configured (`$VITASDK`).
- CMake (version 3.10+) and Make.

```bash
git clone https://github.com/darking101/vitacam.git
cd vitacam
mkdir build && cd build
cmake -DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake ..
make -j4
```

El proceso generará `build/vitacam.vpk` listo para transferir e instalar en tu consola.

---

## Licencia y Créditos / Credits
Desarrollado y creado por **[darking101](https://github.com/darking101)**. Licencia MIT.
Agradecimientos a la comunidad de homebrew de PlayStation Vita, VitaSDK y libvita2d.
