# QuickInts - Standalone Implementation of ObtainQuickVector

An independent implementation of `ObtainQuickVector()` and `ReleaseQuickVector()` for AmigaOS, providing functionality that was removed after AmigaOS V39 and adding the ability to release allocated quick interrupt vectors.

## What are Zorro III Quick Interrupts?

Standard Amiga interrupts use autovectoring: when hardware asserts an interrupt line, the CPU jumps to a fixed vector based on the interrupt priority level. The OS then traverses a chain of interrupt servers to identify which device caused the interrupt and dispatch the appropriate handler.

**Zorro III Quick Interrupts** use the 68k's vectored interrupt capability instead. When a Zorro III card asserts INT2 or INT6, it simultaneously provides an 8-bit vector number (68-255) on the data bus. The CPU fetches the handler address directly from the exception vector table and jumps to it immediately, bypassing the interrupt server chain.

### Why Quick Interrupts Matter

For hardware developers, quick interrupts provide:

- **Lower latency**: Direct vector dispatch eliminates interrupt server chain traversal
- **Reduced CPU overhead**: No need to poll multiple devices to identify the interrupt source
- **Deterministic timing**: No variable overhead from walking interrupt lists

The performance benefit is most noticeable for small block transfers (10-20% improvement), such as loading icons or directory listings where interrupt overhead is significant relative to transfer time. For large transfers, the advantage diminishes as DMA transfer time dominates.

## Background

AmigaOS originally provided `ObtainQuickVector()` in exec.library to allocate quick interrupt vectors (exception vectors 68-255) for hardware that supports vectored interrupts. However:

- **AmigaOS V40+** removed the `ObtainQuickVector()` implementation entirely
- **No release mechanism** ever existed - once a vector was obtained, it could never be freed

This implementation solves both problems by providing a complete, OS-independent solution that works on any 68k Amiga system.

## How It Works

### ObtainQuickVector()

1. **Detects BadQuickInt**: Scans vectors 68-255 to find the most common value (the "bad quick int" handler). This indicates unused vector slots.

2. **Searches for free vector**: Starting from vector 255 and working downward, finds a slot containing the BadQuickInt value.

3. **Installs handler**: Atomically writes the interrupt handler address to the selected vector slot.

4. **VBR-aware**: Properly handles the Vector Base Register on 68010+ processors.

5. **Supervisor mode**: All vector table modifications execute in supervisor mode for safety.

### ReleaseQuickVector()

1. **Validates vector number**: Ensures the vector is in the valid range (68-255).

2. **Restores original value**: Writes the BadQuickInt value back to the vector slot, effectively marking it as unused again.

3. **Atomic operation**: Executes in supervisor mode with interrupts disabled.

## API

```c
ULONG ObtainQuickVector(APTR interruptCode);
```
Allocates a quick interrupt vector and installs the provided interrupt handler.
- **Parameters**: `interruptCode` - Pointer to the interrupt handler routine (must end with RTE)
- **Returns**: Vector number (68-255) on success, 0 on failure

```c
VOID ReleaseQuickVector(ULONG vectorNum);
```
Releases a previously allocated quick interrupt vector.
- **Parameters**: `vectorNum` - The vector number returned by `ObtainQuickVector()`

## Demo Program

The included demo (`main.c`) demonstrates usage with an Amiga 4091 SCSI host controller:

1. Locates the A4091 board via expansion.library
2. Creates a quick interrupt handler that:
   - Increments an interrupt counter
   - Flashes the screen color
   - Clears the A4091's interrupt source
3. Allocates a quick interrupt vector
4. Configures the A4091 to use the allocated vector
5. Triggers a DMA abort to generate an interrupt
6. Verifies the interrupt was received
7. Releases the vector

### Running the Demo

```bash
# Compile (requires m68k-amigaos-gcc cross-compiler)
make

# Run on an Amiga with A4091
quickints
```

Expected output:


![Screenshot](quickints.png?raw=true "Expected output")


```
A4091/A4092 card at 0x40000000
Installing quick interrupt handler @0x070397d6
Installed at vector 255. qcount=0
Informed A4091 of quick interrupt vector 255.
Reset 53C710...done.
Clearing pending DMA.
Enabling DMA interrupt on Abort.
DIEN register: 0x10 (expected: 0x10)
Triggering Abort.
Waiting for Quick Interrupts (2s).
After trigger: qcount=1 (delta=1)
OK: quick interrupt fired.
Releasing quick interrupt vector 255...
Released. Final qcount=1
```

## Building

Requirements:
- m68k-amigaos-gcc cross-compiler
- AmigaOS NDK headers
- clib2 runtime library

```bash
make clean
make
```

## Writing an Interrupt Handler

Quick interrupt handlers must:
- Be naked assembly routines (no C function prologue/epilogue)
- Save and restore all used registers
- End with RTE instruction
- Clear the hardware interrupt source
- Execute as quickly as possible

Example:
```c
asm volatile(
    ".globl _MyHandler         \n"
    "_MyHandler:               \n"
    "    movem.l d0/a0,-(sp)   \n"  // Save registers
    "    ... handler code ...  \n"  // Do work
    "    movem.l (sp)+,d0/a0   \n"  // Restore registers
    "    rte                   \n"  // Return from exception
);
```

## Limitations

- Requires an Amiga capable of Zorro III Quick Interrupts (A3000/A4000)
- Only works with user vectors (68-255)
- Requires supervisor mode access
- The first call scans all vectors to find BadQuickInt (minor overhead)
- No protection against allocating the same vector twice from different code

## License

This code is provided for educational and development purposes for the Amiga community.

## References

- AmigaOS exec.library AutoDocs (V39)
- Amiga Hardware Reference Manual
- Zorro III Specification
- NCR 53C710 SCSI Controller Datasheet

