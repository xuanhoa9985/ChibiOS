/*
    ChibiOS/RT - Copyright (C) 2006-2013 Giovanni Di Sirio

    Licensed under the Apache License, Version 2.0 (the "License");
    you may not use this file except in compliance with the License.
    You may obtain a copy of the License at

        http://www.apache.org/licenses/LICENSE-2.0

    Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS,
    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
    See the License for the specific language governing permissions and
    limitations under the License.
*/
/*
   Concepts and parts of this file have been contributed by Dmytro Milinevskyy <milinevskyy@gmail.com>
 */

/**
 * @file    MIPS-PIC32MX/eic_lld.c
 * @brief   MIPS-PIC32MX low level EIC driver code.
 *
 * @addtogroup EIC
 * @{
 */

#include "ch.h"
#include "hal.h"

#if HAL_USE_EIC || defined(__DOXYGEN__)

/*===========================================================================*/
/* Driver constants.                                                         */
/*===========================================================================*/

/* Max number of IRQs */
#define EIC_NUM_IRQS          75

/* Number of IRQ banks */
#define EIC_IRQ_BANK_QTY      3

/* Number of priority registers */
#define EIC_IRQ_ICP_REGS_QTY  13

/*===========================================================================*/
/* Driver data structures and types.                                         */
/*===========================================================================*/

typedef volatile struct {
  PicReg   intcon;
  PicReg   intstat;
  PicReg   iptmr;
  PicReg   ifs[EIC_IRQ_BANK_QTY];
  PicReg   iec[EIC_IRQ_BANK_QTY];
  PicReg   ipc[EIC_IRQ_ICP_REGS_QTY];
} EicPort;

/**
 * @brief   EIC IRQ handler data.
 */
typedef struct EicIrqInfo {
  /** @brief IRQ handle.*/
  eicIrqHandler handler;
  /** @brief IRQ handler data.*/
  void *data;
} EicIrqInfo;

/*===========================================================================*/
/* Driver exported variables.                                                */
/*===========================================================================*/

/*===========================================================================*/
/* Driver local variables.                                                   */
/*===========================================================================*/

static EicIrqInfo iInfo[EIC_NUM_IRQS];
static EicPort *iPort = (EicPort *)_INT_BASE_ADDRESS;
static uint32_t iMask[EIC_IRQ_BANK_QTY];

/*===========================================================================*/
/* Driver local functions.                                                   */
/*===========================================================================*/

static void eicCoreTimerIsr(uint32_t irq, void *data) {
  (void)data;
  (void)irq;
  
  port_reset_mips_timer();

  chSysLockFromIsr();
  chSysTimerHandlerI();
  chSysUnlockFromIsr();
}

/* Main EIC ISR */
#if defined(MIPS_USE_SHADOW_GPR) || defined(MIPS_USE_VECTORED_IRQ)
CH_IRQ_HANDLER(MIPS_HW_IRQ0) // In PIC32 single-vectored IV mode all interrupts are wired to IRQ0
#else
CH_IRQ_HANDLER(MIPS_HW_IRQ2) // In PIC32 single-vectored compat mode all interrupts are wired to IRQ2
#endif
{
  CH_IRQ_PROLOGUE();

  uint32_t bank;

  for (bank=0;bank<EIC_IRQ_BANK_QTY;++bank) {
    PicReg *ifs = &iPort->ifs[bank];
    uint32_t pending = ifs->reg & iMask[bank];

#if defined(MIPS_USE_MIPS16_ISA)
    uint32_t i;

    for (i=0;i<32;++i) {
      if (pending&1) {
        uint32_t irq = i + bank * 32;
        EicIrqInfo *info = &iInfo[irq];

        chDbgAssert(info->handler, "unhandled EIC IRQ", "");

        ifs->clear = 1 << i;

        info->handler(irq, info->data);
      }

      pending >>= 1;
    }
#else
    while (pending) {
      uint32_t i = 31 - __builtin_clz(pending);
      uint32_t irq = i + bank * 32;
      EicIrqInfo *info = &iInfo[irq];

      chDbgAssert(info->handler, "unhandled EIC IRQ", "");

      ifs->clear = 1 << i;

      info->handler(irq, info->data);

      pending &= ~(1 << i);
    }
#endif
  }
 
  CH_IRQ_EPILOGUE();
}

/*===========================================================================*/
/* Driver exported functions.                                                */
/*===========================================================================*/

/**
 * @brief   EIC LLD initialization.
 * @note    This function is implicitly invoked by @p eicInit(), there is
 *          no need to explicitly initialize the driver.
 *
 * @init
 */
void eic_lld_init(void) {
  int i;

  iPort->intcon.clear = _INTCON_MVEC_MASK; // Single-vectored mode
#if defined(MIPS_USE_SHADOW_GPR) || defined(MIPS_USE_VECTORED_IRQ)
  /* Since we now in IV mode and EIC=1 need to clear IPL(IM) bits */
  {
    uint32_t sr = c0_get_status();
    sr &= ~(0x7f << 10);
    c0_set_status(sr);
  }
#if defined(MIPS_USE_SHADOW_GPR)
  iPort->intcon.set = _INTCON_SS0_MASK; // Use second shadow set on any vectored interrupt
#endif
#endif

  for (i=0;i<EIC_IRQ_BANK_QTY;++i)
    iPort->iec[i].clear = 0xFFFFFFFF;

  /* All interrupts have equal priority ... */
  for (i=0;i<EIC_IRQ_ICP_REGS_QTY;++i) {
    iPort->ipc[i].clear = 0x1F1F1F1F;
    iPort->ipc[i].set = 0x04040404;
  }

  port_init_mips_timer();
  eic_lld_register_irq(EIC_IRQ_CT, eicCoreTimerIsr, NULL);
  eic_lld_enable_irq(EIC_IRQ_CT);
}

/**
 * @brief   EIC IRQ handler registration.
 *
 * @param[in] irq       irq number.
 * @param[in] handler   ponter to irq handler.
 * @param[in] data      opaque data passed to irq handler.
 */
void eic_lld_register_irq(int irq, eicIrqHandler handler, void *data) {
  chDbgAssert(irq < EIC_NUM_IRQS, "IRQ number out of range", "");

  chDbgAssert(!iInfo[irq].handler, "ISR is already registered for this irq", "");

  iInfo[irq].handler = handler;
  iInfo[irq].data = data;
}

/**
 * @brief   EIC IRQ handler unregistration.
 *
 * @param[in] irq       irq number.
 */
void eic_lld_unregister_irq(int irq) {
  chDbgAssert(irq < EIC_NUM_IRQS, "IRQ number out of range", "");

  iInfo[irq].handler = NULL;
}

/**
 * @brief   Enable EIC IRQ.
 *
 * @param[in] irq       irq number.
 */
void eic_lld_enable_irq(int irq) {
  int bank;

  chDbgAssert(irq < EIC_NUM_IRQS, "IRQ number out of range", "");

  if (irq < 32)
    bank = 0;
  else if (irq < 64)
    bank = 1;
  else
    bank = 2;

  iMask[bank] |= 1 << (irq - bank*32);
  iPort->iec[bank].set = 1 << (irq - bank*32);
}

/**
 * @brief   Disable EIC IRQ.
 *
 * @param[in] irq       irq number.
 */
void eic_lld_disable_irq(int irq) {
  int bank;

  chDbgAssert(irq < EIC_NUM_IRQS, "IRQ number out of range", "");

  if (irq < 32)
    bank = 0;
  else if (irq < 64)
    bank = 1;
  else
    bank = 2;

  iMask[bank] &= ~(1 << (irq - bank*32));
  iPort->iec[bank].clear = 1 << (irq - bank*32);
}

/**
 * @brief   Ack EIC IRQ.
 *
 * @param[in] irq       irq number.
 */
void eic_lld_ack_irq(int irq) {
  int bank;

  chDbgAssert(irq < EIC_NUM_IRQS, "IRQ number out of range", "");

  if (irq < 32)
    bank = 0;
  else if (irq < 64)
    bank = 1;
  else
    bank = 2;

  iPort->ifs[bank].clear = 1 << (irq - bank*32);
}

#endif /* HAL_USE_EIC */

/** @} */
