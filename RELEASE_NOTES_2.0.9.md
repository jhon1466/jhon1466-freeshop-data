# FreeShop 2.0.9

## Novedades y Cambios

### 📺 Pantalla y Protector OLED
- **Protector de pantalla OLED con reloj**: Corregido el apagado completo del panel al tener activado "Mostrar reloj"; ahora la pantalla permanece encendida atenuada mostrando la hora y porcentaje de batería flotante anti burn-in.
- **Reloj y batería más visibles**: Textos iniciales sincronizados inmediatamente al activar el protector y color de hora más brillante.

### 🌐 Rebranding e Internacionalización
- **Rebranding completo a FreeShop**: Reemplazadas todas las menciones restantes de *pipensx* en interfaz, hints de ayuda y servidor de emparejamiento.
- **Término FTP estandarizado**: Cambiado "Companion Web" a "FTP" en todos los idiomas (Español, Inglés, Italiano, Francés, Chino).
- **Nuevo soporte de idioma**: Agregada localización completa en Italiano (`it`), incluyendo hints y claves de despliegue.

### 📥 Descargas y Cola
- **Cabecera de resumen de cola**: Estado global de la cola visible en todo momento.
- **Pausa / Reanudar todo (Botón Y)**: Acceso rápido en la pantalla de descargas para pausar o reanudar todas las transferencias activas.
- **Checkpoints y Reanudación**: Guardado periódico de bitfield y progreso en medio de la transferencia; la instalación por streaming conserva el progreso al pausar.

### ⚙️ Ajustes del Sistema
- **Confirmación al salir**: Nueva opción configurable para evitar cierres accidentales.
- **Avisos de descargas activas**: Advertencia antes de salir si hay descargas en curso.
- **Personalización de pestañas**: Opción para mostrar/ocultar la pestaña de Inicio.

### 🧲 Motor Torrent y Debrid
- **Mejoras Torrent PEX y Peers**: Aceptación de peers entrantes en texto plano, emisión de HAVE, envío PEX, keepalives y selección optimizada de piezas en fase endgame.
- **Debrid & Real-Debrid**:
  - Fallback a descarga/construcción de archivo `.torrent` cuando falla la creación por magnet.
  - Descargas y streaming secuencial optimizado en CDN de Real-Debrid y TorrServer.
  - Manejo robusto de excepciones en callbacks de escritura y range workers para evitar cuelgues.
  - Validación de archivos y orden de instalación (Base antes de Update).

### 🛡️ Seguridad y Verificación
- **Verificación de Catálogo Ed25519**: Infraestructura integrada para validar firmas del catálogo de forma segura.
- **PIN FTP / Companion**: Mayor cobertura de seguridad y regeneración a prueba de fallos.

### 💾 Almacenamiento y Sistema de Archivos
- **Limpieza de datos huérfanos**: Estimación y opción de limpieza en un toque para archivos temporales o descargas residuales.
- **Rutas anidadas y FAT32**: Fallback inteligente para rutas largas/anidadas y manejo de archivos/carpetas partidas.
- **Límite de archivos aumentado**: Capacidad elevada a 16384 descriptores de archivo.
- **Errores descriptivos**: Mensajes detallados y accionables en caso de fallos de I/O o falta de espacio.

### 📦 Instalador y Actualizaciones
- **Preflight de versiones y mods**: Comprobación previa de versión instalada y compatibilidad de mods antes de actualizar.
- **Importación idempotente de tickets**: Evita duplicación o errores al reimportar tickets de títulos en Switch.
- **Montaje CNMT desde placeholder**: Registro limpio y seguro antes de finalizar la instalación.

### 🛠️ Estabilidad y Diagnóstico
- **Registro de crashes mejorado**: Escritura directa a través del descriptor de archivo de log y volcado inmediato ante excepciones críticas.

---

### Build Assets
- `freeshop-client.nro`
- `freeshop-client.nro.sha256`
