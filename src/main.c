#include <stdio.h>
#include "platform.h"
#include "xparameters.h"
#include "xscugic.h"
#include "xtmrctr.h"
#include "xil_io.h"
#include "xil_exception.h"
#include "xil_printf.h"

// ================================
// btnled_ip（自製 AXI4-Lite IP）
//   0x00 LED         RW  bit[7:0]
//   0x04 BTN         RO  bit[4:0]（去彈跳後）
//   0x08 IRQ_STATUS  R / 寫 1 清除
//   0x0C IRQ_ENABLE  RW  bit0 = 開啟中斷
// ================================
#ifdef XPAR_BTNLED_IP_0_S00_AXI_BASEADDR
#define BTNLED_BASE       XPAR_BTNLED_IP_0_S00_AXI_BASEADDR
#else
#define BTNLED_BASE       0x43C00000U      // Address Editor 分配的位址
#endif

#ifdef XPAR_FABRIC_BTNLED_IP_0_IRQ_INTR
#define INTC_BTN_ID       XPAR_FABRIC_BTNLED_IP_0_IRQ_INTR
#else
#define INTC_BTN_ID       61U              // IRQ_F2P[0]（xlconcat In0）
#endif

#define REG_LED           0x00
#define REG_BTN           0x04
#define REG_IRQ_STATUS    0x08
#define REG_IRQ_ENABLE    0x0C

#define TIMER_ID          XPAR_AXI_TIMER_0_DEVICE_ID
#define INTC_TIMER_ID     XPAR_FABRIC_AXI_TIMER_0_INTERRUPT_INTR   // 62
#define GIC_ID            XPAR_SCUGIC_SINGLE_DEVICE_ID

// Timer 設定：50MHz 下 10,000,000 counts = 0.2 秒
#define TIMER_RESET_VAL   0xFF676980u
#define TICKS_PER_5SEC    25          // 0.2s * 25 = 5s

XScuGic INTCInst;
XTmrCtr TimerInst;

volatile int LED_num   = 0;
volatile int Intr_CTN  = 0;   // 按鈕中斷次數
volatile int Timer_CTN = 0;   // Timer tick 計數（0.2s 一次）
volatile int Sec5_CTN  = 0;   // 5 秒提醒次數

// ================================
// 按鈕中斷處理函式：每次中斷都印
// （IP 只在「按下」時發中斷，且已去彈跳，所以不用再判斷放開）
void Intr_Handler(void *CallbackRef) {
    u32 btn_val = Xil_In32(BTNLED_BASE + REG_BTN);

    // 清除 IP 的中斷狀態（level 訊號，一定要清，不然會一直進來）
    Xil_Out32(BTNLED_BASE + REG_IRQ_STATUS, 0x1);

    Intr_CTN++;
    LED_num = 0;
    xil_printf("[BTN] Interrupt : %d  (btn = 0x%x)\r\n", Intr_CTN, btn_val);
}

// Timer 中斷處理函式：每 0.2 秒進來一次，累積 25 次 = 5 秒才印
void Timer_Intr_Handler(void *CallBackRef, u8 TmrCtrNumber) {
    LED_num++;
    Timer_CTN++;

    if (Timer_CTN >= TICKS_PER_5SEC) {
        Timer_CTN = 0;
        Sec5_CTN++;
        xil_printf("[TIMER] 5 sec reminder #%d  (total %d sec)\r\n",
                   Sec5_CTN, Sec5_CTN * 5);
    }
}

// ================================
// 中斷系統初始化
int Intr_Setup(XScuGic *GicInstancePtr, XTmrCtr *TmrInstancePtr) {
    int Status;
    XScuGic_Config *IntcConfig;

    IntcConfig = XScuGic_LookupConfig(GIC_ID);
    if (NULL == IntcConfig) {
        return XST_FAILURE;
    }

    Status = XScuGic_CfgInitialize(GicInstancePtr, IntcConfig, IntcConfig->CpuBaseAddress);
    if (Status != XST_SUCCESS) {
        return XST_FAILURE;
    }

    Xil_ExceptionInit();
    Xil_ExceptionRegisterHandler(XIL_EXCEPTION_ID_INT,
        (Xil_ExceptionHandler)XScuGic_InterruptHandler, GicInstancePtr);

    // 1. btnled_ip 中斷（level high：trigger type = 0x1）
    XScuGic_SetPriorityTriggerType(GicInstancePtr, INTC_BTN_ID, 0xA0, 0x1);
    Status = XScuGic_Connect(GicInstancePtr, INTC_BTN_ID,
        (Xil_ExceptionHandler)Intr_Handler, NULL);
    if (Status != XST_SUCCESS) return XST_FAILURE;
    XScuGic_Enable(GicInstancePtr, INTC_BTN_ID);

    // 2. Timer 中斷
    Status = XScuGic_Connect(GicInstancePtr, INTC_TIMER_ID,
        (Xil_ExceptionHandler)XTmrCtr_InterruptHandler, (void *)TmrInstancePtr);
    if (Status != XST_SUCCESS) return XST_FAILURE;
    XTmrCtr_SetHandler(TmrInstancePtr, (XTmrCtr_Handler)Timer_Intr_Handler, TmrInstancePtr);
    XScuGic_Enable(GicInstancePtr, INTC_TIMER_ID);

    // 全部設定完才開 CPU 中斷
    Xil_ExceptionEnable();

    return XST_SUCCESS;
}

// Timer 初始化（只設定，不啟動）
int Timer_Setup(void) {
    int Status = XTmrCtr_Initialize(&TimerInst, TIMER_ID);
    if (Status != XST_SUCCESS) return XST_FAILURE;

    XTmrCtr_SetOptions(&TimerInst, 0, XTC_INT_MODE_OPTION | XTC_AUTO_RELOAD_OPTION);
    XTmrCtr_SetResetValue(&TimerInst, 0, TIMER_RESET_VAL);
    return XST_SUCCESS;
}

// ================================
int main() {
    init_platform();

    xil_printf("\r\n=== btnled_ip test (base 0x%08x, irq %d) ===\r\n",
               BTNLED_BASE, INTC_BTN_ID);

    // btnled_ip 初始化：LED 全暗、中斷先關、清舊狀態
    Xil_Out32(BTNLED_BASE + REG_LED, 0x0);
    Xil_Out32(BTNLED_BASE + REG_IRQ_ENABLE, 0x0);
    Xil_Out32(BTNLED_BASE + REG_IRQ_STATUS, 0x1);

    if (Timer_Setup() != XST_SUCCESS) {
        xil_printf("Timer setup failed\r\n");
        return XST_FAILURE;
    }

    if (Intr_Setup(&INTCInst, &TimerInst) != XST_SUCCESS) {
        xil_printf("Interrupt setup failed\r\n");
        return XST_FAILURE;
    }

    // GIC 接好後才開 IP 的中斷輸出、啟動 Timer
    Xil_Out32(BTNLED_BASE + REG_IRQ_ENABLE, 0x1);
    XTmrCtr_Start(&TimerInst, 0);

    xil_printf("Init successful\r\n");

    while (1) {
        // 更新 LED 顯示數值（8 顆）
        Xil_Out32(BTNLED_BASE + REG_LED, LED_num & 0xFF);
    }

    cleanup_platform();
    return 0;
}
