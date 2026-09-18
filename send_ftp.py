import os
import sys
import ftplib

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
VITA_IP = os.environ.get("VITA_IP", "192.168.1.68")
VITA_PORT = int(os.environ.get("VITA_PORT", "1337"))
LOCAL_VPK = os.path.join(SCRIPT_DIR, "build", "vitacam.vpk")
REMOTE_PATH = "ux0:/vitacam.vpk"

def upload_vpk(ip=VITA_IP, port=VITA_PORT, local_file=LOCAL_VPK, remote_file=REMOTE_PATH):
    if not os.path.exists(local_file):
        print(f"Error: Archivo no encontrado: {local_file}")
        sys.exit(1)

    file_size = os.path.getsize(local_file)
    print(f"Conectando a PS Vita ({ip}:{port})...")
    
    ftp = ftplib.FTP()
    try:
        ftp.connect(ip, port, timeout=10)
        ftp.login()
        print("Conexión FTP exitosa.")
        
        print(f"Enviando '{local_file}' ({file_size} bytes) -> '{remote_file}'...")
        with open(local_file, 'rb') as f:
            ftp.storbinary(f"STOR {remote_file}", f)
        print(f"¡Transferencia completada con éxito a {remote_file}!")

        # También subir eboot.bin directo para actualización inmediata sin reinstalar
        local_eboot = os.path.join(SCRIPT_DIR, "build", "eboot.bin")
        if os.path.exists(local_eboot):
            try:
                with open(local_eboot, 'rb') as ef:
                    ftp.storbinary("STOR ux0:/app/VCAM00001/eboot.bin", ef)
                print("¡eboot.bin actualizado en ux0:/app/VCAM00001/eboot.bin!")
            except Exception as ee:
                print("Nota: No se pudo escribir eboot directo (la app puede estar en ejecución o ruta distinta):", ee)

        # Actualizar recursos de LiveArea
        livearea_files = [
            ("sce_sys/icon0.png", "ux0:/app/VCAM00001/sce_sys/icon0.png"),
            ("sce_sys/livearea/contents/bg.png", "ux0:/app/VCAM00001/sce_sys/livearea/contents/bg.png"),
            ("sce_sys/livearea/contents/startup.png", "ux0:/app/VCAM00001/sce_sys/livearea/contents/startup.png"),
            ("sce_sys/livearea/contents/template.xml", "ux0:/app/VCAM00001/sce_sys/livearea/contents/template.xml"),
        ]
        for l_src, r_dst in livearea_files:
            if os.path.exists(l_src):
                try:
                    with open(l_src, 'rb') as lf:
                        ftp.storbinary(f"STOR {r_dst}", lf)
                except Exception:
                    pass

        ftp.quit()
    except Exception as e:
        print(f"Error durante la transferencia FTP: {e}")
        sys.exit(1)

if __name__ == "__main__":
    ip = sys.argv[1] if len(sys.argv) > 1 else VITA_IP
    port = int(sys.argv[2]) if len(sys.argv) > 2 else VITA_PORT
    upload_vpk(ip, port)
