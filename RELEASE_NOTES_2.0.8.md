# FreeShop Client 2.0.8

## Cambios

### Fixes Críticos
- **SSL rc=60**: Resuelto error de verificación SSL en Switch. Causa raíz: fecha/hora incorrecta de la consola. Se incluye bundle CA Mozilla completo (192 KB, `cacert.pem`) cubriendo GitHub/Cloudflare/TorBox. Logs diagnósticos `clock=` y `verify_result=` añadidos en servicios de catálogo, metadatos y actualizaciones.

### Mejoras de UI
- **Updates tab badge**: Contador de actualizaciones visible desde el arranque de la app, sin necesidad de entrar a la sección Updates. Implementado join del scanner de títulos instalados + `checkAll()` sincrónico en `firstFrame` + `setUpdateCountBadge()` directo.
- **Iconos Settings hub**: Rediseño de iconos del rail lateral para coincidir con su función:
  - General → Engranaje (configuración universal)
  - Source → Cilindro base de datos (fuente de datos)
  - Catalog → Grilla 2×2 (lista/catálogo)
  - System → Llave inglesa (mantenimiento/herramientas)
  - Downloads, Network, Storage sin cambios (ya intuitivos)

### Fixes de Instalación
- **CNMT required_system_version**: `patchRequiredSystemVersion()` pone a cero el campo en CNMT de Application/Patch metas al instalar. HOS bloquea lanzamiento si firmware del título > firmware consola; Tinfoil/Awoo ya lo hacían. Añade `content_meta.hpp` + tests unitarios (`test_content_meta.cpp`).
- **Port uninstall fix**: Receipts v3 guardan `titleId` parseado del NSP forwarder (`Port [01d2c0b236000000].nsp`). `PortUninstallService::plan` usa receipts v3 → fallback manifest → fallback metadata index. Evita archivos huérfanos en `/switch` al desinstalar ports.

### Build
- Version bump: 2.0.7 → 2.0.8
- Build Switch NRO OK (Docker `freeshop-client-builder`, devkitPro `/opt/devkitpro`)
- NRO: `freeshop-client.nro` (16.6 MB)

## Instalación
1. Copiar `freeshop-client.nro` a `sdmc:/switch/freeshop-client.nro`
2. Si actualizando desde versión anterior, la app migrará automáticamente los receipts y ajustes

## Notas
- El error SSL `rc=60` era por **fecha/hora incorrecta de la consola** (no por el bundle CA). Verificar hora en Configuración de la Switch.
- Requiere Atmosphere + hbloader/ovl-sysmodules actualizados.