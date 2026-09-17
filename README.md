# VitaCam Pro

[![Platform](https://img.shields.io/badge/Platform-PlayStation%20Vita-blue.svg)](https://github.com/vitasdk)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![Developer](https://img.shields.io/badge/Developer-darking101-00d2ff.svg)](https://github.com/darking101)

**VitaCam Pro** es una aplicación de cámara avanzada, visor de galería y servidor web Wi-Fi de alta velocidad para la **PlayStation Vita**, desarrollada en C nativo utilizando [VitaSDK](https://vitasdk.org/).

Transforma tu PS Vita en una cámara retro moderna con controles manuales, flash de pantalla frontal, marca de agua adaptativa oficial con fecha y hora, y transferencia inalámbrica directa a tu smartphone o PC mediante código QR sin cables.

---

## Características Principales

### 📸 Cámara Pro y Visor en Tiempo Real
- **Cámara Trasera y Frontal:** Alterna instantáneamente entre el sensor trasero y delantero.
- **Modo Flash / Softbox Frontal:** Aro de luz y pantalla blanca completa de alta luminosidad para selfies y entornos oscuros.
- **Sello Inteligente Adaptativo [PS] VITA:**
  - Marca de agua con el logotipo vectorizado oficial de PlayStation Vita.
  - Detección automática de luminosidad: el logotipo y la fecha cambian inteligentemente entre blanco puro y negro de alto contraste según el fondo de la toma.
  - Estampa de fecha y hora exacta de la captura.
- **Zoom Continuo Dinámico:** Control suave de 1.0x a 5.0x mediante el stick analógico o gestos táctiles de pellizco (*pinch-to-zoom*).
- **Cuadrícula de Composición:** Guía de regla de tercios para encuadres precisos.

### 🖼️ Galería Continua y Visor Multimedia
- **Cuadrícula Rápida con Caché:** Navegación ultra fluida con miniaturas cacheadas en memoria.
- **Filtro por Pestañas de Origen:**
  - **VitaCam:** Fotografías capturadas con la aplicación (`ux0:/data/vitacam/`).
  - **Fotos Vita:** Álbum nativo del sistema (`ux0:/picture/`).
  - **Screenshots:** Capturas de pantalla de tus juegos (`ux0:/picture/SCREENSHOT/`).
  - **Todo:** Vista unificada de todas las imágenes de la consola.
- **Visor a Pantalla Completa:** Inspección detallada con zoom dinámico y paneo táctil.
- **Modo Selección Múltiple:** Selección rápida de fotografías para borrado individual o en lote.
- **Barra de Estado Completa:** Reloj en tiempo real, indicador gráfico de batería con porcentaje y animación de carga, y botón de información `[ (i) ]`.

### 🌐 Servidor Web Wi-Fi Integrado (Web Hub)
- **Transferencia sin Cables:** Servidor HTTP ligero integrado en C nativo para descargar tus fotos directamente al teléfono, tablet o computadora.
- **Acceso Instantáneo por Código QR:** Escanea el código QR que se muestra en pantalla con la cámara de tu teléfono para entrar directamente a la galería web.
- **Autenticación por PIN y Cookies:** Generación de PIN de seguridad con persistencia de sesión automática vía URL y Cookie.
- **Interfaz Web Moderna y Responsiva:** Galería web oscura con vista previa en alta resolución y descargas con un solo clic.

---

## Atajos y Controles

### Modo Cámara
| Botón / Control | Acción |
| :--- | :--- |
| **Gatillo R** / **Tap en Pantalla** | Capturar fotografía |
| **Select** | Cambiar entre cámara Trasera y Frontal |
| **Triángulo** | Alternar modo Flash (Apagado / Pantalla Blanca / Aro de Luz) |
| **Cuadrado** | Activar / Desactivar Cuadrícula de composición |
| **Cruz (X)** | Activar / Desactivar Marca de agua [PS] VITA |
| **Stick Izquierdo / D-Pad** | Zoom dinámico (Arriba/Abajo) |
| **Círculo** | Salir a la Galería |

### Modo Galería
| Botón / Control | Acción |
| :--- | :--- |
| **D-Pad / Stick** | Navegar entre fotos |
| **Gatillos L / R** | Cambiar pestaña de origen (VitaCam / Fotos / Capturas / Todo) |
| **Cruz (X)** | Abrir foto seleccionada a pantalla completa |
| **Cuadrado** | Iniciar modo de selección múltiple (o marcar/desmarcar foto) |
| **Triángulo** | Abrir menú del Servidor Web Wi-Fi y Código QR |
| **Select** | Abrir menú del Servidor Web Wi-Fi |
| **Touch en `[ (i) ]`** | Abrir créditos de autor (@darking101) y guía rápida |
| **Touch en Pestañas / Fotos** | Selección, navegación y apertura directa |

---

## Compilación desde Código Fuente

### Requisitos
1. [VitaSDK](https://vitasdk.org/) instalado y configurado en tu entorno (`$VITASDK`).
2. CMake (versión 3.10 o superior) y Make o Ninja.

### Pasos de Compilación
```bash
# Clonar el repositorio
git clone https://github.com/darking101/vitacam.git
cd vitacam

# Crear directorio de compilación
mkdir build && cd build

# Configurar con la toolchain de VitaSDK
cmake -DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake ..

# Compilar el binario y generar el VPK
make -j4
```

El proceso generará:
- `build/vitacam.vpk`: Paquete instalable para PlayStation Vita.
- `build/eboot.bin`: Binario ejecutable para actualización rápida vía FTP.

---

## Instalación en PS Vita

1. **Método VPK (Recomendado):**
   - Transfiere `vitacam.vpk` a tu consola usando VitaShell (FTP o USB).
   - En VitaShell, navega hasta el archivo y presiona **Cruz (X)** para instalarlo.

2. **Método Rápido por FTP (Desarrollo):**
   - Abre VitaShell en tu PS Vita y presiona **Select** para iniciar el servidor FTP.
   - Ejecuta desde tu PC:
     ```bash
     VITA_IP="TU_IP_VITA" python3 send_ftp.py
     ```

---

## Licencia

Este proyecto está bajo la Licencia MIT. Consulta el archivo [LICENSE](LICENSE) para más detalles.

---

## Créditos

Desarrollado y creado por **[darking101](https://github.com/darking101)**.

Agradecimientos a la comunidad de homebrew de PS Vita y a los creadores de [VitaSDK](https://vitasdk.org/) y [vita2d](https://github.com/xerpi/libvita2d).
