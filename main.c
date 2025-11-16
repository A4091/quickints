#include <exec/execbase.h>
#include <exec/libraries.h>
#include <exec/types.h>
#include <proto/dos.h>     // For Delay(), Printf() if you link against dos.library
#include <stdio.h>
#include <libraries/expansionbase.h>
#include <proto/exec.h>
#include <proto/expansion.h>     // FindConfigDev() prototype (+ ExpansionBase)

#undef ObtainQuickVector
extern ULONG __stdargs ObtainQuickVector(APTR interruptCode);
extern VOID  __stdargs ReleaseQuickVector(ULONG vector);
extern VOID  __stdargs QuickHandler(VOID);

// ----------  A4091 specifics ----------
#define ZORRO_MFG_COMMODORE     0x0202
#define ZORRO_PROD_A4091        0x0054

#define A4091_OFFSET_AUTOCONFIG 0x00000000
#define A4091_OFFSET_ROM        0x00000000
#define A4091_OFFSET_REGISTERS  0x00800000
#define A4091_OFFSET_QUICKINT   0x00880003
#define A4091_OFFSET_SWITCHES   0x008c0003

#define ADDR8(x)      (volatile uint8_t *)(x)
#define BIT(x)        (uint32_t)(1 << (x))

// ----------  NCR 53c710 registers ----------
#define REG_SIEN    0x00  // SCSI interrupt enable
#define REG_SCID    0x07  // SCSI chip ID

#define REG_SSTAT0  0x0e  // SCSI status 0
#define REG_DSTAT   0x0f  // DMA status

#define REG_CTEST4_CDIS BIT(7)  // Cache burst disable

#define REG_CTEST7  0x18  // Chip test 7

#define REG_ISTAT   0x22  // Interrupt status
#define REG_ISTAT_DIP   BIT(0)  // DMA interrupt pending
#define REG_ISTAT_SIP   BIT(1)  // SCSI interrupt pending
#define REG_ISTAT_RST   BIT(6)  // Reset the 53C710
#define REG_ISTAT_ABRT  BIT(7)  // Abort

#define REG_DIEN    0x3a  // DMA interrupt enable
#define REG_DIEN_DFE    BIT(7)  // DMA interrupt on FIFO empty
#define REG_DIEN_BF     BIT(5)  // DMA interrupt on Bus Fault
#define REG_DIEN_ABRT   BIT(4)  // DMA interrupt on Aborted
#define REG_DIEN_SSI    BIT(3)  // DMA interrupt on SCRIPT Step Interrupt
#define REG_DIEN_SIR    BIT(2)  // DMA interrupt on SCRIPT Interrupt Instruction
#define REG_DIEN_WTD    BIT(1)  // DMA interrupt on Watchdog Timeout Detected
#define REG_DIEN_ILD    BIT(0)  // DMA interrupt on Illegal Instruction Detected

#define REG_DWT     0x39  // DMA watchdog timer

#define REG_DCNTL   0x38  // DMA control
#define REG_DCNTL_COM   BIT(0)  // Enable 53C710 mode
#define REG_DCNTL_EA    BIT(5)  // Enable Ack
#define REG_DCNTL_CFD2  0                  // SCLK 37.50-50.00 MHz

#define REG_DMODE   0x3b  // DMA mode
#define REG_DMODE_BLE0  0                  // Burst length 1-transfer
#define REG_DMODE_BLE1  BIT(6)             // Burst length 2-transfers
#define REG_DMODE_BLE2  BIT(7)             // Burst length 4-transfers
#define REG_DMODE_BLE3  (BIT(6) | BIT(7))  // Burst length 8-transfers
#define REG_DMODE_FC2   BIT(5)  // Value driven on FC2 when bus mastering

typedef struct {
	ULONG intcount;
	volatile uint8_t *reg_addr;
	volatile uint8_t *base_addr;
	//
	uint8_t ireg_istat;
	uint8_t ireg_dstat;
	uint8_t ireg_sien;
	uint8_t ireg_sstat0;
} state_t;

state_t g_state = { 0, NULL, 0, 0, 0, 0 };

static void
set_ncrreg8_noglob(volatile uint8_t *a4091_regs, uint reg, uint8_t value)
{
    *ADDR8(a4091_regs + 0x40 + reg) = value;
}

static uint8_t
get_ncrreg8_noglob(volatile uint8_t *a4091_regs, uint reg)
{
    uint8_t value;
    value = *ADDR8(a4091_regs + reg);
    return (value);
}

static void
set_ncrreg8(uint reg, uint8_t value)
{
    set_ncrreg8_noglob(g_state.reg_addr, reg, value);
}

static uint8_t
get_ncrreg8(uint reg)
{
    return get_ncrreg8_noglob(g_state.reg_addr, reg);
}


/*
 * a4091_reset
 * -----------
 * Resets the A4091's 53C710 SCSI controller.
 */
static void
a4091_reset(void)
{
    // fake runtime flags
    int runtime_flags=0;
#define FLAG_IS_A4000T 1

    printf("Reset 53C710...");
    if (runtime_flags & FLAG_IS_A4000T)
        set_ncrreg8(REG_DCNTL, REG_DCNTL_EA);   // Enable Ack: allow reg writes
    set_ncrreg8(REG_ISTAT, REG_ISTAT_RST);  // Reset
    (void) get_ncrreg8(REG_ISTAT);          // Push out write

    set_ncrreg8(REG_ISTAT, 0);              // Clear reset
    (void) get_ncrreg8(REG_ISTAT);          // Push out write
    Delay(1);

    /* SCSI Core clock (37.51-50 MHz) */
    if (runtime_flags & FLAG_IS_A4000T)
        set_ncrreg8(REG_DCNTL, REG_DCNTL_COM | REG_DCNTL_CFD2 | REG_DCNTL_EA);
    else
        set_ncrreg8(REG_DCNTL, REG_DCNTL_COM | REG_DCNTL_CFD2);

    set_ncrreg8(REG_SCID, BIT(7));          // Set SCSI ID

    set_ncrreg8(REG_DWT, 60);               // 25MHz DMA timeout: 640ns * 60

    const int burst_mode = 8;
    switch (burst_mode) {
        default:
        case 1:
            /* 1-transfer burst, FC = 101 -- works on A3000 */
            set_ncrreg8(REG_DMODE, REG_DMODE_BLE0 | REG_DMODE_FC2);
            break;
        case 2:
            /* 2-transfer burst, FC = 101 */
            set_ncrreg8(REG_DMODE, REG_DMODE_BLE1 | REG_DMODE_FC2);
            break;
        case 4:
            /* 4-transfer burst, FC = 101 */
            set_ncrreg8(REG_DMODE, REG_DMODE_BLE2 | REG_DMODE_FC2);
            break;
        case 8:
            /* 8-transfer burst, FC = 101 */
          set_ncrreg8(REG_DMODE, REG_DMODE_BLE3 | REG_DMODE_FC2);
            break;
    }

    if ((runtime_flags & FLAG_IS_A4000T) == 0) {
        /* Disable cache line bursts */
        set_ncrreg8(REG_CTEST7, get_ncrreg8(REG_CTEST7) | REG_CTEST4_CDIS);
    }

    /* Disable interrupts */
    set_ncrreg8(REG_DIEN, 0);
    printf("done.\n");
}

static int
dma_clear_istat(void)
{
    uint8_t istat;
    uint    timeout = 30;

    /* Clear pending interrupts */
    while (((istat = get_ncrreg8(REG_ISTAT)) & 0x03) != 0) {
        if (istat & REG_ISTAT_SIP)
            (void) get_ncrreg8(REG_SSTAT0);

        if (istat & REG_ISTAT_DIP)
            (void) get_ncrreg8(REG_DSTAT);

        if (istat & (REG_ISTAT_RST | REG_ISTAT_ABRT))
            set_ncrreg8(REG_ISTAT, 0);

        if (istat & (REG_ISTAT_DIP | REG_ISTAT_SIP))
            Delay(1);

        if (timeout-- == 5) {
            /* Attempt to get out of this state with a 53C710 reset */
            a4091_reset();
        }
        if (timeout == 0) {
            printf("Timeout clearing 53C710 ISTAT %02x\n", istat);
            return (1);
        }
    }
    return (0);
}


LONG
quick_irq_handler(register state_t *save asm("a1"))
{
    uint8_t istat = get_ncrreg8_noglob(save->reg_addr, REG_ISTAT);
    volatile uint8_t dstat;

    if (istat & REG_ISTAT_ABRT)
        set_ncrreg8_noglob(save->reg_addr, REG_ISTAT, 0);

    /* Always read DSTAT to clear DMA interrupt source */
    dstat = get_ncrreg8_noglob(save->reg_addr, REG_DSTAT);
    if ((istat & (REG_ISTAT_DIP | REG_ISTAT_SIP)) != 0) {
        uint prev_istat = save->ireg_istat;
        /*
         * ISTAT_SIP is set, read SSTAT0 register to determine cause
         * If ISTAT_DIP is set, read DSTAT register to determine cause
        */
	save->ireg_istat  = istat;
	save->ireg_sien   = get_ncrreg8_noglob(save->reg_addr, REG_SIEN);
	save->ireg_sstat0 = get_ncrreg8_noglob(save->reg_addr, REG_SSTAT0);
        save->ireg_dstat = dstat;

        save->intcount++;

        if (prev_istat == 0)
	    return 1;
    }

    return 0;
}

int main(void)
{
    ULONG vec;
    ULONG before, after;

    struct Library        *ExpansionBase;
    struct ConfigDev      *cdev = NULL;

    if ((ExpansionBase = OpenLibrary((CONST_STRPTR)"expansion.library", 0)) == 0) {
        printf("Could not open expansion.library\n");
        return 1;
    }

    cdev = FindConfigDev(cdev, ZORRO_MFG_COMMODORE, ZORRO_PROD_A4091);
    if (cdev == NULL) {
	printf("Could not find A4091/A4092 card\n");
	CloseLibrary(ExpansionBase);
        return 1;
    }

    g_state.base_addr = cdev->cd_BoardAddr;
    g_state.reg_addr = cdev->cd_BoardAddr + A4091_OFFSET_REGISTERS;
    printf("A4091/A4092 card at 0x%08lx\n", (ULONG)g_state.base_addr);

    printf("Installing quick interrupt handler @0x%08lx...\n", (ULONG)QuickHandler);

    /* Naked quick-interrupt handler:
       - Checks for real quick-interrupt
       - Calls C interrupt handler
       - Returns with RTE (required for vectors)
     */
    asm volatile(
	"    bra 1f			\n"
        "   .globl _QuickHandler	\n"
	"_QuickHandler:			\n"
	"    move.l  d0,-(sp)           \n"
	"    move.w  0xdff01c,d0	\n" // Interrupt Enable State
	"    btst.l  #14,d0		\n" // Check if pending disable
	"    bne.s   RealInterrupt	\n"
	"ExitInt:			\n"
	"    move.l  (sp)+,d0		\n"
	"    rte			\n"
        "RealInterrupt:			\n"
	"    movem.l d1-d7/a0-a6,-(sp)  \n"
	"    lea     _g_state,a1        \n"
	"    jsr     _quick_irq_handler \n"
	"    movem.l (sp)+,d1-d7/a0-a6  \n"
        "    bra.s ExitInt		\n"
	"1:				\n"
        :
	:
        : "memory"
    );

    /* Install our handler. For the classic NDK signature, this returns a vector number (>=68). */
    vec = ObtainQuickVector((APTR)QuickHandler);
    if (vec < 68 || vec > 255) {
        printf("ObtainQuickVector failed (got %lu). Aborting.\n", vec);
        return 20;
    }

    printf("Installed at vector %lu. count=%lu\n", vec, g_state.intcount);

    *ADDR8(g_state.base_addr + A4091_OFFSET_QUICKINT) = (uint8_t)vec;

    printf("Informed A4091 of quick interrupt vector %lu.\n", vec);

    before = g_state.intcount;

    printf("Resetting SCSI host controller (slow).\n");
    a4091_reset();
    printf("Clearing pending DMA.\n");
    if (dma_clear_istat()) {
        printf("ERROR: Failed to clear DMA status. Aborting test.\n");
        ReleaseQuickVector(vec);
        return 10;
    }
    printf("Enabling DMA interrupt on Abort.\n");
    set_ncrreg8(REG_DIEN, REG_DIEN_ABRT);  // Set DMA interrupt enable on Abort

    // Verify the interrupt enable was set
    uint8_t dien_check = get_ncrreg8(REG_DIEN);
    printf("DIEN register: 0x%02x (expected: 0x%02x)\n", dien_check, (uint8_t)REG_DIEN_ABRT);

    // Double-check that ISTAT is clear before triggering
    uint8_t istat_before = get_ncrreg8(REG_ISTAT);
    printf("ISTAT before abort: 0x%02x\n", istat_before);

    printf("Triggering Abort.\n");
    set_ncrreg8(REG_ISTAT, REG_ISTAT_ABRT);

    // Immediately check if abort was set
    uint8_t istat_after = get_ncrreg8(REG_ISTAT);
    printf("ISTAT after abort: 0x%02x\n", istat_after);

    printf("Waiting for Quick Interrupts (2s).\n");
    /* Give a little time in case the interrupt is slightly delayed */
    Delay(2);

    after = g_state.intcount;
    printf("After trigger: intcount=%lu (delta=%ld)\n", after, (long)(after - before));

    if (after == before) {
        printf("WARNING: quick interrupt did not fire (counter unchanged).\n");
    } else {
        printf("OK: quick interrupt fired.\n");
    }

    printf("Releasing quick interrupt vector %lu...\n", vec);
    ReleaseQuickVector(vec);
    printf("Released. Final intcount=%lu\n", g_state.intcount);

    return 0;
}
