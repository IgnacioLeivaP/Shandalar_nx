# Shandalar en Nintendo Switch (con Autorun)

Esta carpeta prepara el juego para correr en una Switch con homebrew mediante
[Autorun](https://github.com/autorunhq/autorun) (Wine para Switch: corre
programas x86 de Windows sobre Horizon, sin Linux).

No es un port nativo: el juego original (`Shandalar.exe` y `shandalar.dll`) se
ejecuta tal cual bajo Wine y Box64/FEX. Lo que se agrega aquí son los ajustes y
el control con el mando.

## Contenido

| Archivo | Para qué sirve |
|---|---|
| `preparar_switch.py` | Copia el juego a la microSD y aplica todos los ajustes |
| `ShandalarPad/` | Control del mouse con el mando, pensado para Shandalar (código, Makefile y `bin/image.dll` ya compilado) |
| `ShandalarPad/ShandalarPad.ini` | Configuración de ShandalarPad (botones, velocidad, ventana) |
| `autorun/Shandalar.keys.txt` | Perfil de controles de Autorun, usado cuando ShandalarPad está apagado |

## Instalación

Requisitos: una Switch con Atmosphère y Autorun instalado (con sus "Windows
DLLs" descargadas desde el launcher), y Python 3 en el PC.

1. Conecta la microSD al PC (o usa MTP/FTP) y ejecuta, desde la raíz del
   repositorio:

   ```
   python switch/preparar_switch.py E:\switch\wine\drive_c\Shandalar
   ```

   Cambia `E:` por la letra de tu microSD. La primera copia mueve unos 2 GB.
   Con `--slim` se omiten copias duplicadas y herramientas de desarrollo
   (`Program`, `Manalink3`, `Editor`, `magic_updater` y `PlayDeckAnalyser`),
   lo que deja unos 1,4 GB. Si algo falla, vuelve a la copia completa.

2. En la Switch abre Autorun, pulsa **+ → Add game** y elige
   `C:\Shandalar\Shandalar.exe`.

3. Con **Y** sobre el juego puedes cambiar el traductor de CPU (FEX o Box64).
   Si uno da problemas, prueba el otro.

### Qué cambia el script en la copia (el repositorio no se toca)

- `image.dll` pasa a llamarse `image_orig.dll` y en su lugar queda ShandalarPad.
  La única función que exporta la DLL original (`LoadImage`) se reenvía a la
  original, así que el juego no nota la diferencia.
- `config.txt`: `Debug:0` (en el repositorio viene con `Debug:1`).
- `Shandalar.ini`: `Window = 2` (ver «Resolución» más abajo).
- Se copian `ShandalarPad.ini` (si ya existe, se conserva tu versión) y
  `Shandalar.keys.txt`.

Para quitar ShandalarPad, borra `image.dll` y renombra `image_orig.dll` a
`image.dll`.

## Controles (ShandalarPad)

| Botón | Acción |
|---|---|
| Stick izquierdo o derecho | Mover el cursor |
| ZL (mantener) | Cursor lento (precisión) |
| ZR (mantener) | Cursor rápido |
| A | Clic izquierdo (mantener = arrastrar) |
| B | Clic derecho (menú contextual) |
| Y | Doble clic izquierdo (auto-tap de maná, ordenar cartas) |
| X | Doble clic derecho (ver la carta completa) |
| L | Enter |
| R | Espacio |
| + | Escape |
| D-pad | Flechas |
| Pantalla táctil | Mueve el cursor y hace clic (siempre activa) |
| L3 + R3 (1 segundo) | Suspender o reactivar ShandalarPad |

Atajos de Autorun que siguen activos: **− + clic del stick derecho** abre el
teclado en pantalla (útil para escribir tu nombre), y mantener **+ y −** un
segundo cierra el juego.

Todo se puede cambiar en `ShandalarPad.ini`, junto a `Shandalar.exe`. Para
otras combinaciones de teclas usa códigos de tecla virtual de Windows, por
ejemplo `L=0x11+0x53` para Ctrl+S (guardar la partida en duelo).

### Cómo funciona

Autorun ya trae su propio mouse: stick derecho, A/B y pantalla táctil. Pero
cuando un programa lee el mando por XInput, Autorun le cede el mando por
completo y solo mantiene el táctil. ShandalarPad aprovecha eso. Se carga
dentro del proceso del juego, lee XInput y genera el mouse y el teclado con
`SendInput`, con funciones que el control por defecto no tiene: doble clic
izquierdo y derecho, modo de precisión, curva de aceleración y ambos sticks.

Al suspenderlo con L3 + R3 vuelven los controles de Autorun definidos en
`Shandalar.keys.txt`.

Con `Log=1` (por defecto), ShandalarPad escribe `ShandalarPad.log`
junto al juego. Ese archivo, junto con `switch/wine/logs/Shandalar.log` de
Autorun, es lo primero que conviene revisar si algo falla.

### Compilar ShandalarPad

```
cd switch/ShandalarPad
make            # requiere i686-w64-mingw32-gcc (Debian/Ubuntu: gcc-mingw-w64-i686)
```

El resultado es `bin/image.dll` (32 bits, sin dependencias aparte de
kernel32, user32 y msvcrt).

## Resolución: lo más importante para probar

Autorun ofrece una única resolución de pantalla, 1280×720, sin cambios de modo.
Shandalar fue hecho para 1024×768 (o 800×600 y 640×480) y cambia la
resolución al arrancar. Esto se probó con Wine 9 en Linux, en una pantalla
virtual de 1280×720 (la misma que reporta Autorun):

| `Window =` en `Shandalar.ini` | Menú y aventura | Duelo |
|---|---|---|
| `0` (por defecto en PC) | El menú se ve a 640×480 en la esquina y el juego **se cae** al crear el personaje (`WM_CREATE CreateDIBSection`) | Se cae |
| `1` | Funciona, ventana de 1024×768 | Carga, pero la mano, la carta grande y los diálogos quedan **en negro** |
| `2` (**el que pone el script**) | Funciona, ventana de 1024×768 | Pantalla completa a 1280×720; mismo problema de zonas en negro |

En una pantalla de exactamente 1024×768, los mismos duelos se ven
perfectamente con `Window = 0` y con `Window = 2`. Con 1280×720 y 1280×800
aparecen las zonas en negro, así que el problema viene de que la resolución
no sea 1024×768. Con el escritorio virtual de Wine pasa lo mismo, así que no
parece exclusivo de X11. Falta confirmar si en Autorun también ocurre: es lo
primero que hay que probar.

Lo que ShandalarPad ya hace al respecto (`[Window]` en `ShandalarPad.ini`):

- **`FixMainWindow=1`** (activo): la ventana de aventura se crea sin barra de
  título ni bordes, en la esquina superior y a 1024×768 exactos. Sin esto, el
  juego la dimensiona a 1030×800 y Wine la recorta a 732 px de alto. Ahora
  solo quedan fuera las últimas 48 líneas, que en el mapa son la franja
  decorativa inferior.
- **`FakeScreen=1`** (experimental, apagado): le dice al juego que la
  pantalla mide 1024×768 y que el cambio de resolución funcionó, parcheando
  sus importaciones de `GetSystemMetrics`, `GetDeviceCaps`,
  `ChangeDisplaySettings` y `SystemParametersInfo`. Con esto, `Window = 0`
  ya no se cae y el duelo usa el diseño de 1024×768, aunque en las pruebas
  seguía con las zonas en negro.

### Si los duelos se ven con zonas en negro en la Switch

Prueba en este orden y anota qué pasa con cada uno:

1. `Window = 2` (por defecto).
2. `Window = 0` con `FakeScreen=1` en `ShandalarPad.ini`.
3. `Window = 1`.

Si ninguna funciona, las siguientes vías son:

- Pedir a Autorun que ofrezca un modo de 1024×768 escalado (ya escala los
  juegos Direct3D de 800×600 y 640×480).
- Investigar en `shandalar.dll` por qué el duelo no pinta sus subventanas
  cuando la pantalla no es de 1024×768.
- Seguir con el port nativo, que no depende de nada de esto.

## Lo que no se probó aún

Nada de esto se ha probado en una Switch real. Se verificó con Wine 9 en Linux:

- ShandalarPad carga dentro de `Shandalar.exe` y `LoadImage` se reenvía a la
  DLL original.
- Con un XInput simulado: movimiento a la velocidad configurada, modo de
  precisión, clic y doble clic izquierdo y derecho, teclas, y suspensión con
  L3 + R3.
- El juego llega al mapa del mundo y a encuentros en 1280×720 con `Window = 2`.

Queda por ver en la Switch:

- Rendimiento con Box64 y FEX.
- Sonido y video (`.avi`).
- Los duelos (ver la sección anterior).
- El teclado en pantalla para el nombre.
- El FaceMaker.

El juego lanza `Magic.exe` y `FaceMaker.exe` como procesos aparte en un par de
lugares (volver al menú principal, editor de caras). Autorun corre un solo
programa a la vez, así que esas opciones probablemente no funcionen. No son
necesarias para jugar el modo aventura.
