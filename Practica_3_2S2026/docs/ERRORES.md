# Registro de errores - Práctica 3

---

## Error 1 - Uso excesivo de pila del kernel
### Error encontrado
Durante la compilación parcial de `kernel/sys.o`, GCC mostró las siguientes advertencias:

- `so2_log_write_match`: frame de 1472 bytes.
- `so2_log_scan_file`: frame de 1072 bytes.

El compilador advertía que ambos tamaños superaban los 1024 bytes configurados como referencia.

### Causa
Se estaban declarando buffers relativamente grandes como variables locales dentro de funciones ejecutadas en espacio de kernel.
En particular, ambas funciones reservaban memoria directamente en la pila del kernel, lo cual no es recomendable porque el espacio disponible para la pila es limitado.

### Código antes de la solución
```c
char entry[SO2_LOG_LINE_MAX + SO2_MAX_PATH +
           SO2_MAX_KEYWORD + 96];

char buffer[SO2_LOG_READ_CHUNK];
```

Estas variables se creaban localmente cada vez que las funciones eran ejecutadas.

### Solución aplicada
Los buffers fueron trasladados de la pila del kernel a memoria dinámica utilizando `kmalloc()`.
Para `so2_log_write_match`:

```c
size_t entry_size;

entry_size = SO2_LOG_LINE_MAX + SO2_MAX_PATH +
             SO2_MAX_KEYWORD + 96;

entry = kmalloc(entry_size, GFP_KERNEL);

if (!entry) {
    pr_err("SO2 P3: watch=%d sin memoria para entrada de log\n",
           watch->id);
    return;
}
```

Después de utilizar el buffer se libera mediante:
```c
kfree(entry);
```

Para `so2_log_scan_file` se realizó el mismo cambio:
```c
buffer = kmalloc(SO2_LOG_READ_CHUNK, GFP_KERNEL);

if (!buffer) {
    pr_err("SO2 P3: watch=%d sin memoria para lectura de %s\n",
           watch->id,
           watch->rutas[file_index]);

    filp_close(source, NULL);
    return;
}
```

Y al terminar:
```c
kfree(buffer);
```

### Resultado
Después de aplicar la corrección se eliminó el objeto anterior:
```bash
rm -f kernel/sys.o
```

y se volvió a compilar:
```bash
make -j1 kernel/sys.o
```

La compilación terminó correctamente:
```text
CALL    scripts/checksyscalls.sh
DESCEND objtool
INSTALL libsubcmd_headers
CC      kernel/sys.o
```

Esta vez no aparecieron las advertencias relacionadas con el tamaño del stack frame.

### Fuentes consultadas
Se revisaron las funciones de asignación dinámica disponibles en el kernel Linux 6.12.69, particularmente:

```text
include/linux/slab.h
kmalloc()
kfree()
GFP_KERNEL
```

También se utilizó la advertencia generada por GCC durante la compilación para identificar que los buffers locales estaban utilizando demasiado espacio de pila.

### Validación de la corrección
La corrección fue validada mediante una nueva compilación parcial de `kernel/sys.o`.
La primera compilación generó advertencias por frames de 1472 y 1072 bytes.
Después de mover los buffers a memoria dinámica, `kernel/sys.o` compiló correctamente sin dichas advertencias.

---

## Error 2 - El monitor no detectaba eventos después de truncar un archivo de log
### Error encontrado
Durante las pruebas del mecanismo `log_watch` se detectó que el monitor funcionaba correctamente mientras el archivo de log solamente aumentaba de tamaño.
Sin embargo, si el archivo vigilado era truncado mientras el monitoreo continuaba activo, los nuevos eventos escritos después del truncamiento dejaban de ser detectados.

### Prueba realizada
Se inició el monitoreo del archivo `app1.log` utilizando la palabra clave `ERROR`.
Primero se agregó:
```bash
echo "ERROR primera deteccion antes de truncar" >> "$BASE/app1.log"
```

El monitor detectó correctamente el evento y lo escribió en `central.log`.
En ese momento el archivo tenía:
```text
61 bytes
```

Posteriormente se truncó el archivo mediante:
```bash
: > "$BASE/app1.log"
```

El tamaño pasó a:
```text
0 bytes
```

Después se agregó un nuevo evento:
```bash
echo "ERROR despues de truncar" >> "$BASE/app1.log"
```

El nuevo contenido estaba presente en `app1.log`:
```text
ERROR despues de truncar
```

pero `central.log` solamente contenía la primera detección:
```text
[watch=1] archivo=/home/jordiciocio/SO2/Practica_3_2S2026/pruebas_logs/app1.log palabra=ERROR linea=ERROR primera deteccion antes de truncar
```

Por lo tanto, el segundo evento no fue detectado.

### Causa
El monitor conserva en:
```c
watch->posiciones[file_index]
```

la posición hasta la cual había leído cada archivo.
Antes del truncamiento, la posición ya se encontraba aproximadamente en el byte 61.
Cuando el archivo fue reducido a 0 bytes, esa posición no fue reiniciada. Por lo tanto, las siguientes llamadas a `kernel_read()` intentaban continuar leyendo desde una posición que se encontraba más allá del nuevo final del archivo.
Esto provocaba que el monitor encontrara EOF y no procesara los nuevos datos escritos después del truncamiento.

### Código antes de la solución
La función abría el archivo y continuaba directamente utilizando la posición previamente almacenada:

```c
source = filp_open(watch->rutas[file_index], O_RDONLY, 0);

if (IS_ERR(source))
	return;

buffer = kmalloc(SO2_LOG_READ_CHUNK, GFP_KERNEL);
```

No existía ninguna comprobación para determinar si el tamaño del archivo se había reducido.

### Solución aplicada
Después de abrir el archivo se compara su tamaño actual contra la posición almacenada por el monitor.
Si la posición es mayor que el tamaño actual, significa que el archivo fue truncado y se reinicia su estado de lectura:
```c
if (watch->posiciones[file_index] >
    i_size_read(file_inode(source))) {

	pr_info("SO2 P3: watch=%d archivo truncado, reiniciando posicion: %s\n",
		watch->id,
		watch->rutas[file_index]);

	watch->posiciones[file_index] = 0;
	watch->parcial_len[file_index] = 0;
	watch->parcial[file_index][0] = '\0';
}
```

También se reinicia el buffer utilizado para conservar líneas incompletas, evitando combinar contenido anterior al truncamiento con contenido nuevo.

### Fuentes consultadas
Se revisó el código fuente de Linux 6.12.69 y las estructuras y funciones disponibles para el manejo de archivos del kernel, particularmente:

```text
include/linux/fs.h
file_inode()
i_size_read()
kernel_read()
```

También se utilizó el comportamiento observado durante las pruebas del monitor para determinar la causa del problema.

### Validación de la corrección
Después de modificar `kernel/sys.c` se volvió a compilar únicamente el archivo afectado:

```bash
rm -f kernel/sys.o
make -j1 kernel/sys.o
```

La compilación finalizó correctamente:

```text
CALL    scripts/checksyscalls.sh
DESCEND objtool
INSTALL libsubcmd_headers
CC      kernel/sys.o
```

sin errores ni advertencias.
La corrección deberá validarse finalmente ejecutando nuevamente la prueba después de instalar el kernel actualizado y comprobando que el evento escrito después del truncamiento aparezca en `central.log`.

---

## Error 3 - El log central podía monitorearse a sí mismo
### Error encontrado
Durante las pruebas del mecanismo `log_watch` se comprobó que `start_log_watch` permitía utilizar el mismo archivo como archivo vigilado y como archivo de log central.
Esta configuración podía provocar que el monitor detectara una línea con la palabra clave, escribiera la detección en el mismo archivo y posteriormente volviera a detectar la nueva línea generada por él mismo.

### Prueba realizada
Se creó un archivo de prueba:
```bash
: > "$BASE/loop.log"
```

Luego se intentó utilizar el mismo archivo como log central y como archivo vigilado:
```bash
./start_log_watch \
ERROR \
"$BASE/loop.log" \
"$BASE/loop.log"
```

Antes de aplicar la validación, esta configuración era aceptada por la syscall.
Si posteriormente se agregaba:
```bash
echo "ERROR prueba de auto monitoreo" >> "$BASE/loop.log"
```

el monitor podía detectar la línea y escribir una nueva entrada en el mismo archivo.
Como la nueva entrada también contenía la palabra `ERROR`, podía ser detectada nuevamente por el hilo de monitoreo, provocando un ciclo de auto monitoreo.

### Causa
La implementación original de `sys_start_log_watch` verificaba que las rutas recibidas fueran válidas, pero no comprobaba que `ruta_log_central` fuera diferente de cada una de las rutas incluidas en la lista de archivos vigilados.

### Código antes de la solución
Después de copiar las rutas, el log central y la palabra clave, la implementación continuaba directamente con la creación del monitor:
```c
watch->cantidad = cantidad;

watch->id =
	atomic_inc_return(&so2_log_watch_next_id);

INIT_LIST_HEAD(&watch->list);
```

No existía una comparación entre:
```c
watch->rutas[i]
```

y:
```c
watch->ruta_central
```

### Solución aplicada
Antes de crear el ID y lanzar el hilo del kernel se agregó una validación que compara el archivo central con cada uno de los archivos vigilados:
```c
for (i = 0; i < cantidad; ++i) {
	if (strcmp(watch->rutas[i],
		   watch->ruta_central) == 0) {

		pr_err(
			"SO2 P3: log central no puede ser archivo vigilado: %s\n",
			watch->ruta_central
		);

		ret = -EINVAL;
		goto error_free;
	}
}
```

Si una ruta coincide con el log central, la syscall termina con:
```c
-EINVAL
```

y el hilo de kernel no es creado.
### Fuentes consultadas
Se revisaron las funciones de manejo de cadenas disponibles en Linux 6.12.69, principalmente:
```text
include/linux/string.h
strcmp()
```

También se utilizó el comportamiento observado durante las pruebas del sistema para identificar que permitir un archivo central dentro de la lista de archivos vigilados producía una configuración inválida.

### Validación de la corrección
Después de aplicar la modificación se volvió a compilar el archivo modificado:
```bash
rm -f kernel/sys.o
make -j1 kernel/sys.o
```

La compilación finalizó correctamente:
```text
CALL    scripts/checksyscalls.sh
DESCEND objtool
INSTALL libsubcmd_headers
CC      kernel/sys.o
```

Posteriormente se recompiló e instaló el kernel y se repitió la prueba:
```bash
./start_log_watch \
ERROR \
"$BASE/loop.log" \
"$BASE/loop.log"
```

Esta vez el programa respondió:
```text
Error start_log_watch: Invalid argument
```

También se comprobó que no se creó el archivo `monitor.id`.
Finalmente, mediante `dmesg` se confirmó que el kernel rechazó la configuración:
```text
SO2 P3: log central no puede ser archivo vigilado: /home/jordiciocio/SO2/Practica_3_2S2026/pruebas_logs/loop.log
```

Por lo tanto, la corrección quedó validada correctamente.