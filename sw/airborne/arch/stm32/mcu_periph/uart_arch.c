/*
 * Copyright (C) 2009 Antoine Drouin <poinix@gmail.com>
 *
 * This file is part of paparazzi.
 *
 * paparazzi is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * paparazzi is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with paparazzi; see the file COPYING.  If not, write to
 * the Free Software Foundation, 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

/**
 * @file arch/stm32/mcu_periph/uart_arch.c
 * @ingroup stm32_arch
 *
 * Handling of UART hardware for STM32.
 */

#include "mcu_periph/uart.h"

#include <libopencm3/stm32/gpio.h>
#include <libopencm3/stm32/rcc.h>
#include <libopencm3/stm32/usart.h>
#include <libopencm3/cm3/nvic.h>

#include "std.h"

#include BOARD_CONFIG

void uart_periph_set_baudrate(struct uart_periph* p, uint32_t baud) {

  /* Configure USART */
  usart_set_baudrate((u32)p->reg_addr, baud);
  usart_set_databits((u32)p->reg_addr, 8);
  usart_set_stopbits((u32)p->reg_addr, USART_STOPBITS_1);
  usart_set_parity((u32)p->reg_addr, USART_PARITY_NONE);

  /* Disable Idle Line interrupt */
  USART_CR1((u32)p->reg_addr) &= ~USART_CR1_IDLEIE;

  /* Disable LIN break detection interrupt */
  USART_CR2((u32)p->reg_addr) &= ~USART_CR2_LBDIE;

  /* Enable USART1 Receive interrupts */
  USART_CR1((u32)p->reg_addr) |= USART_CR1_RXNEIE;

  /* Enable the USART */
  usart_enable((u32)p->reg_addr);

}

void uart_periph_set_mode(struct uart_periph* p, bool_t tx_enabled, bool_t rx_enabled, bool_t hw_flow_control) {
  u32 mode = 0;
  if (tx_enabled)
    mode |= USART_MODE_TX;
  if (rx_enabled)
    mode |= USART_MODE_RX;

  /* set mode to Tx, Rx or TxRx */
  usart_set_mode((u32)p->reg_addr, mode);

  if (hw_flow_control) {
    usart_set_flow_control((u32)p->reg_addr, USART_FLOWCONTROL_RTS_CTS);
  }
  else {
    usart_set_flow_control((u32)p->reg_addr, USART_FLOWCONTROL_NONE);
  }
}

void uart_transmit(struct uart_periph* p, uint8_t data ) {

  uint16_t temp = (p->tx_insert_idx + 1) % UART_TX_BUFFER_SIZE;

  if (temp == p->tx_extract_idx)
    return;                          // no room

  USART_CR1((u32)p->reg_addr) &= ~USART_CR1_TXEIE; // Disable TX interrupt

  // check if in process of sending data
  if (p->tx_running) { // yes, add to queue
    p->tx_buf[p->tx_insert_idx] = data;
    p->tx_insert_idx = temp;
  }
  else { // no, set running flag and write to output register
    p->tx_running = TRUE;
    usart_send((u32)p->reg_addr, data);
  }

  USART_CR1((u32)p->reg_addr) |= USART_CR1_TXEIE; // Enable TX interrupt

}

static inline void usart_isr(struct uart_periph* p) {

  if (((USART_CR1((u32)p->reg_addr) & USART_CR1_TXEIE) != 0) &&
      ((USART_SR((u32)p->reg_addr) & USART_SR_TXE) != 0)) {
    // check if more data to send
    if (p->tx_insert_idx != p->tx_extract_idx) {
      usart_send((u32)p->reg_addr,p->tx_buf[p->tx_extract_idx]);
      p->tx_extract_idx++;
      p->tx_extract_idx %= UART_TX_BUFFER_SIZE;
    }
    else {
      p->tx_running = FALSE;   // clear running flag
      USART_CR1((u32)p->reg_addr) &= ~USART_CR1_TXEIE; // Disable TX interrupt
    }
  }

  if (((USART_CR1((u32)p->reg_addr) & USART_CR1_RXNEIE) != 0) &&
      ((USART_SR((u32)p->reg_addr) & USART_SR_RXNE) != 0) &&
      ((USART_SR((u32)p->reg_addr) & USART_SR_ORE) == 0) &&
      ((USART_SR((u32)p->reg_addr) & USART_SR_NE) == 0) &&
      ((USART_SR((u32)p->reg_addr) & USART_SR_FE) == 0)) {
    uint16_t temp = (p->rx_insert_idx + 1) % UART_RX_BUFFER_SIZE;;
    p->rx_buf[p->rx_insert_idx] = usart_recv((u32)p->reg_addr);
    // check for more room in queue
    if (temp != p->rx_extract_idx)
      p->rx_insert_idx = temp; // update insert index
  }
  else {
    /* ORE, NE or FE error - read USART_DR reg and log the error */
    if (((USART_CR1((u32)p->reg_addr) & USART_CR1_RXNEIE) != 0) &&
        ((USART_SR((u32)p->reg_addr) & USART_SR_ORE) != 0)) {
      usart_recv((u32)p->reg_addr);
      p->ore++;
    }
    if (((USART_CR1((u32)p->reg_addr) & USART_CR1_RXNEIE) != 0) &&
        ((USART_SR((u32)p->reg_addr) & USART_SR_NE) != 0)) {
      usart_recv((u32)p->reg_addr);
      p->ne_err++;
    }
    if (((USART_CR1((u32)p->reg_addr) & USART_CR1_RXNEIE) != 0) &&
        ((USART_SR((u32)p->reg_addr) & USART_SR_FE) != 0)) {
      usart_recv((u32)p->reg_addr);
      p->fe_err++;
    }
  }
}

static inline void usart_enable_irq(u8 IRQn) {
  /* Note:
   * In libstm32 times the priority of this interrupt was set to
   * preemption priority 2 and sub priority 1
   */
  /* Enable USART interrupts */
  nvic_enable_irq(IRQn);
}

/** Set RCC and GPIO mode on the STM32F4
 */
#ifdef STM32F4
static inline void set_uart_pin(u32 gpioport, u16 gpio, u8 alt_func_num) {
  switch (gpioport) {
    case GPIOA:
      rcc_peripheral_enable_clock(&RCC_AHB1ENR, RCC_AHB1ENR_IOPAEN);
      break;
    case GPIOB:
      rcc_peripheral_enable_clock(&RCC_AHB1ENR, RCC_AHB1ENR_IOPBEN);
      break;
    case GPIOC:
      rcc_peripheral_enable_clock(&RCC_AHB1ENR, RCC_AHB1ENR_IOPCEN);
      break;
    case GPIOD:
      rcc_peripheral_enable_clock(&RCC_AHB1ENR, RCC_AHB1ENR_IOPDEN);
      break;
    default:
      break;
  };
  gpio_mode_setup(gpioport, GPIO_MODE_AF, GPIO_PUPD_NONE, gpio);
  gpio_set_af(gpioport, alt_func_num, gpio);
}
#endif /* STM32F4 */

#ifdef USE_UART1

/* by default enable UART Tx and Rx */
#ifndef USE_UART1_TX
#define USE_UART1_TX TRUE
#endif
#ifndef USE_UART1_RX
#define USE_UART1_RX TRUE
#endif

#ifndef UART1_HW_FLOW_CONTROL
#define UART1_HW_FLOW_CONTROL FALSE
#endif

void uart1_init( void ) {

  uart_periph_init(&uart1);
  uart1.reg_addr = (void *)USART1;

  /* init RCC and GPIOs */
#if defined(STM32F4)
  rcc_peripheral_enable_clock(&RCC_APB2ENR, RCC_APB2ENR_USART1EN);
  set_uart_pin(UART1_GPIO_PORT_RX, UART1_GPIO_RX, UART1_GPIO_AF);
  set_uart_pin(UART1_GPIO_PORT_TX, UART1_GPIO_TX, UART1_GPIO_AF);

#elif defined(STM32F1)
  rcc_peripheral_enable_clock(&RCC_APB2ENR, RCC_APB2ENR_USART1EN);
  rcc_peripheral_enable_clock(&RCC_APB2ENR, RCC_APB2ENR_IOPAEN);

  gpio_set_mode(GPIO_BANK_USART1_TX, GPIO_MODE_OUTPUT_50_MHZ,
                GPIO_CNF_OUTPUT_ALTFN_PUSHPULL, GPIO_USART1_TX);
  gpio_set_mode(GPIO_BANK_USART1_RX, GPIO_MODE_INPUT,
                GPIO_CNF_INPUT_FLOAT, GPIO_USART1_RX);
#endif

  /* Enable USART interrupts in the interrupt controller */
  usart_enable_irq(NVIC_USART1_IRQ);

#if UART1_HW_FLOW_CONTROL
#warning "USING UART1 FLOW CONTROL. Make sure to pull down CTS if you are not connecting any flow-control-capable hardware."
  /* setup CTS and RTS gpios */
#if defined(STM32F4)
  set_uart_pin(UART1_GPIO_PORT_CTS, UART1_GPIO_CTS, UART1_GPIO_AF);
  set_uart_pin(UART1_GPIO_PORT_RTS, UART1_GPIO_RTS, UART1_GPIO_AF);
#elif defined(STM32F1)
  gpio_set_mode(GPIO_BANK_USART1_RTS, GPIO_MODE_OUTPUT_50_MHZ,
                GPIO_CNF_OUTPUT_ALTFN_PUSHPULL, GPIO_USART1_RTS);
  gpio_set_mode(GPIO_BANK_USART1_CTS, GPIO_MODE_INPUT,
                GPIO_CNF_INPUT_FLOAT, GPIO_USART1_CTS);
#endif
#endif

  /* Configure USART1, enable hardware flow control*/
  uart_periph_set_mode(&uart1, USE_UART1_TX, USE_UART1_RX, UART1_HW_FLOW_CONTROL);

  /* Set USART1 baudrate and enable interrupt */
  uart_periph_set_baudrate(&uart1, UART1_BAUD);
}

void usart1_isr(void) { usart_isr(&uart1); }

#endif /* USE_UART1 */


#ifdef USE_UART2

/* by default enable UART Tx and Rx */
#ifndef USE_UART2_TX
#define USE_UART2_TX TRUE
#endif
#ifndef USE_UART2_RX
#define USE_UART2_RX TRUE
#endif

#ifndef UART2_HW_FLOW_CONTROL
#define UART2_HW_FLOW_CONTROL FALSE
#endif

void uart2_init( void ) {

  uart_periph_init(&uart2);
  uart2.reg_addr = (void *)USART2;

  /* init RCC and GPIOs */
#if defined(STM32F4)
  rcc_peripheral_enable_clock(&RCC_APB1ENR, RCC_APB1ENR_USART2EN);
  set_uart_pin(UART2_GPIO_PORT_RX, UART2_GPIO_RX, UART2_GPIO_AF);
  set_uart_pin(UART2_GPIO_PORT_TX, UART2_GPIO_TX, UART2_GPIO_AF);

#elif defined(STM32F1)
  rcc_peripheral_enable_clock(&RCC_APB1ENR, RCC_APB1ENR_USART2EN);
  rcc_peripheral_enable_clock(&RCC_APB2ENR, RCC_APB2ENR_IOPAEN);
  gpio_set_mode(GPIO_BANK_USART2_TX, GPIO_MODE_OUTPUT_50_MHZ,
                GPIO_CNF_OUTPUT_ALTFN_PUSHPULL, GPIO_USART2_TX);
  gpio_set_mode(GPIO_BANK_USART2_RX, GPIO_MODE_INPUT,
                GPIO_CNF_INPUT_FLOAT, GPIO_USART2_RX);
#endif

  /* Enable USART interrupts in the interrupt controller */
  usart_enable_irq(NVIC_USART2_IRQ);

#if UART2_HW_FLOW_CONTROL && defined(STM32F4)
#warning "USING UART2 FLOW CONTROL. Make sure to pull down CTS if you are not connecting any flow-control-capable hardware."
  /* setup CTS and RTS pins */
  set_uart_pin(UART2_GPIO_PORT_CTS, UART2_GPIO_CTS, UART2_GPIO_AF);
  set_uart_pin(UART2_GPIO_PORT_RTS, UART2_GPIO_RTS, UART2_GPIO_AF);
#endif

  /* Configure USART Tx,Rx, and hardware flow control*/
  uart_periph_set_mode(&uart2, USE_UART2_TX, USE_UART2_RX, UART2_HW_FLOW_CONTROL);

  /* Configure USART */
  uart_periph_set_baudrate(&uart2, UART2_BAUD);
}

void usart2_isr(void) { usart_isr(&uart2); }

#endif /* USE_UART2 */


#ifdef USE_UART3

/* by default enable UART Tx and Rx */
#ifndef USE_UART3_TX
#define USE_UART3_TX TRUE
#endif
#ifndef USE_UART3_RX
#define USE_UART3_RX TRUE
#endif

#ifndef UART3_HW_FLOW_CONTROL
#define UART3_HW_FLOW_CONTROL FALSE
#endif

void uart3_init( void ) {

  uart_periph_init(&uart3);
  uart3.reg_addr = (void *)USART3;

  /* init RCC */
#if defined(STM32F4)
  rcc_peripheral_enable_clock(&RCC_APB1ENR, RCC_APB1ENR_USART3EN);
  set_uart_pin(UART3_GPIO_PORT_RX, UART3_GPIO_RX, UART3_GPIO_AF);
  set_uart_pin(UART3_GPIO_PORT_TX, UART3_GPIO_TX, UART3_GPIO_AF);

#elif defined(STM32F1)
  rcc_peripheral_enable_clock(&RCC_APB1ENR, RCC_APB1ENR_USART3EN);
  rcc_peripheral_enable_clock(&RCC_APB2ENR, RCC_APB2ENR_IOPCEN);
  rcc_peripheral_enable_clock(&RCC_APB2ENR, RCC_APB2ENR_AFIOEN);

  AFIO_MAPR |= AFIO_MAPR_USART3_REMAP_PARTIAL_REMAP;
  gpio_set_mode(GPIO_BANK_USART3_PR_TX, GPIO_MODE_OUTPUT_50_MHZ,
                GPIO_CNF_OUTPUT_ALTFN_PUSHPULL, GPIO_USART3_PR_TX);
  gpio_set_mode(GPIO_BANK_USART3_PR_RX, GPIO_MODE_INPUT,
                GPIO_CNF_INPUT_FLOAT, GPIO_USART3_PR_RX);
#endif

  /* Enable USART interrupts in the interrupt controller */
  usart_enable_irq(NVIC_USART3_IRQ);

#if UART3_HW_FLOW_CONTROL && defined(STM32F4)
#warning "USING UART3 FLOW CONTROL. Make sure to pull down CTS if you are not connecting any flow-control-capable hardware."
  /* setup CTS and RTS pins */
  set_uart_pin(UART3_GPIO_PORT_CTS, UART3_GPIO_CTS, UART3_GPIO_AF);
  set_uart_pin(UART3_GPIO_PORT_RTS, UART3_GPIO_RTS, UART3_GPIO_AF);
#endif

  /* Configure USART Tx,Rx, and hardware flow control*/
  uart_periph_set_mode(&uart3, USE_UART3_TX, USE_UART3_RX, UART3_HW_FLOW_CONTROL);

  /* Configure USART */
  uart_periph_set_baudrate(&uart3, UART3_BAUD);
}

void usart3_isr(void) { usart_isr(&uart3); }

#endif /* USE_UART3 */


#if defined USE_UART4 && defined STM32F4

/* by default enable UART Tx and Rx */
#ifndef USE_UART4_TX
#define USE_UART4_TX TRUE
#endif
#ifndef USE_UART4_RX
#define USE_UART4_RX TRUE
#endif

void uart4_init( void ) {

  uart_periph_init(&uart4);
  uart4.reg_addr = (void *)UART4;

  /* init RCC and GPIOs */
  rcc_peripheral_enable_clock(&RCC_APB1ENR, RCC_APB1ENR_UART4EN);
  set_uart_pin(UART4_GPIO_PORT_RX, UART4_GPIO_RX, UART4_GPIO_AF);
  set_uart_pin(UART4_GPIO_PORT_TX, UART4_GPIO_TX, UART4_GPIO_AF);

  /* Enable USART interrupts in the interrupt controller */
  usart_enable_irq(NVIC_UART4_IRQ);

  /* Configure USART */
  uart_periph_set_mode(&uart4, USE_UART4_TX, USE_UART4_RX, FALSE);
  uart_periph_set_baudrate(&uart4, UART4_BAUD);
}

void uart4_isr(void) { usart_isr(&uart4); }

#endif /* USE_UART4 */


#ifdef USE_UART5

/* by default enable UART Tx and Rx */
#ifndef USE_UART5_TX
#define USE_UART5_TX TRUE
#endif
#ifndef USE_UART5_RX
#define USE_UART5_RX TRUE
#endif

void uart5_init( void ) {

  uart_periph_init(&uart5);
  uart5.reg_addr = (void *)UART5;

  /* init RCC and GPIOs */
#if defined(STM32F4)
  rcc_peripheral_enable_clock(&RCC_APB1ENR, RCC_APB1ENR_UART5EN);
  set_uart_pin(UART5_GPIO_PORT_RX, UART5_GPIO_RX, UART5_GPIO_AF);
  set_uart_pin(UART5_GPIO_PORT_TX, UART5_GPIO_TX, UART5_GPIO_AF);
#elif defined(STM32F1)
  rcc_peripheral_enable_clock(&RCC_APB1ENR, RCC_APB1ENR_UART5EN);
  rcc_peripheral_enable_clock(&RCC_APB2ENR, RCC_APB2ENR_IOPCEN);
  rcc_peripheral_enable_clock(&RCC_APB2ENR, RCC_APB2ENR_IOPDEN);

  gpio_set_mode(GPIO_BANK_UART5_TX, GPIO_MODE_OUTPUT_50_MHZ,
                GPIO_CNF_OUTPUT_ALTFN_PUSHPULL, GPIO_UART5_TX);
  gpio_set_mode(GPIO_BANK_UART5_RX, GPIO_MODE_INPUT,
                GPIO_CNF_INPUT_FLOAT, GPIO_UART5_RX);
#endif
  /* Enable USART interrupts in the interrupt controller */
  usart_enable_irq(NVIC_UART5_IRQ);

  /* Configure USART */
  uart_periph_set_mode(&uart5, USE_UART5_TX, USE_UART5_RX, FALSE);
  uart_periph_set_baudrate(&uart5, UART5_BAUD);
}

void uart5_isr(void) { usart_isr(&uart5); }

#endif /* USE_UART5 */


#if defined USE_UART6 && defined STM32F4

/* by default enable UART Tx and Rx */
#ifndef USE_UART6_TX
#define USE_UART6_TX TRUE
#endif
#ifndef USE_UART6_RX
#define USE_UART6_RX TRUE
#endif

#ifndef UART6_HW_FLOW_CONTROL
#define UART6_HW_FLOW_CONTROL FALSE
#endif

void uart6_init( void ) {

  uart_periph_init(&uart6);
  uart6.reg_addr = (void *)USART6;

  /* enable uart clock */
  rcc_peripheral_enable_clock(&RCC_APB2ENR, RCC_APB2ENR_USART6EN);

  /* init RCC and GPIOs */
  set_uart_pin(UART6_GPIO_PORT_RX, UART6_GPIO_RX, UART6_GPIO_AF);
  set_uart_pin(UART6_GPIO_PORT_TX, UART6_GPIO_TX, UART6_GPIO_AF);

  /* Enable USART interrupts in the interrupt controller */
  usart_enable_irq(NVIC_USART6_IRQ);

#if UART6_HW_FLOW_CONTROL
#warning "USING UART6 FLOW CONTROL. Make sure to pull down CTS if you are not connecting any flow-control-capable hardware."
  /* setup CTS and RTS pins */
  set_uart_pin(UART6_GPIO_PORT_CTS, UART6_GPIO_CTS, UART6_GPIO_AF);
  set_uart_pin(UART6_GPIO_PORT_RTS, UART6_GPIO_RTS, UART6_GPIO_AF);
#endif

  /* Configure USART Tx,Rx and hardware flow control*/
  uart_periph_set_mode(&uart6, USE_UART6_TX, USE_UART6_RX, UART6_HW_FLOW_CONTROL);

  uart_periph_set_baudrate(&uart6, UART6_BAUD, FALSE);
}

void usart6_isr(void) { usart_isr(&uart6); }

#endif /* USE_UART6 */
