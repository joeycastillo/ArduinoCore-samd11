/*
 * Copyright (c) 2017, Alex Taradov <alex@taradov.com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/*- Includes ----------------------------------------------------------------*/
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "samd11.h"
#include "hal_gpio.h"
#include "uart.h"
#include "usb_cdc.h"

/*- Definitions -------------------------------------------------------------*/
#define UART_BUF_SIZE            256

HAL_GPIO_PIN(UART_TX,            A, 4);
HAL_GPIO_PIN(UART_RX,            A, 5);

#define UART_SERCOM              SERCOM0
#define UART_SERCOM_PMUX         PORT_PMUX_PMUXE_D_Val
#define UART_SERCOM_GCLK_ID      SERCOM0_GCLK_ID_CORE
#define UART_SERCOM_APBCMASK     PM_APBCMASK_SERCOM0
#define UART_SERCOM_IRQ_INDEX    SERCOM0_IRQn
#define UART_SERCOM_IRQ_HANDLER  SERCOM0_Handler
#define UART_SERCOM_TXPO         0
#define UART_SERCOM_RXPO         1

/*- Types ------------------------------------------------------------------*/
typedef struct
{
  int       wr;
  int       rd;
  uint8_t   data[UART_BUF_SIZE];
} fifo_buffer_t;

/*- Variables --------------------------------------------------------------*/
static volatile fifo_buffer_t uart_rx_fifo;
static volatile fifo_buffer_t uart_tx_fifo;

/*- Implementations ---------------------------------------------------------*/

//-----------------------------------------------------------------------------
void uart_init(usb_cdc_line_coding_t *line_coding)
{
  int chsize, form, pmode, sbmode, baud, fp;

  HAL_GPIO_UART_TX_pmuxen(UART_SERCOM_PMUX);

  HAL_GPIO_UART_RX_pullup();
  HAL_GPIO_UART_RX_pmuxen(UART_SERCOM_PMUX);

  PM->APBCMASK.reg |= UART_SERCOM_APBCMASK;

  GCLK->CLKCTRL.reg = GCLK_CLKCTRL_ID(UART_SERCOM_GCLK_ID) |
      GCLK_CLKCTRL_CLKEN | GCLK_CLKCTRL_GEN(0);

  UART_SERCOM->USART.CTRLA.reg = SERCOM_USART_CTRLA_SWRST;
  while (UART_SERCOM->USART.CTRLA.bit.SWRST);

  uart_tx_fifo.wr = 0;
  uart_tx_fifo.rd = 0;

  uart_rx_fifo.wr = 0;
  uart_rx_fifo.rd = 0;

  if (USB_CDC_5_DATA_BITS == line_coding->bDataBits)
    chsize = 5;
  else if (USB_CDC_6_DATA_BITS == line_coding->bDataBits)
    chsize = 6;
  else if (USB_CDC_7_DATA_BITS == line_coding->bDataBits)
    chsize = 7;
  else if (USB_CDC_8_DATA_BITS == line_coding->bDataBits)
    chsize = 0;
  else
    chsize = 0;

  if (USB_CDC_NO_PARITY == line_coding->bParityType)
    form = 0;
  else
    form = 1;

  if (USB_CDC_EVEN_PARITY == line_coding->bParityType)
    pmode = 0;
  else
    pmode = SERCOM_USART_CTRLB_PMODE;

  if (USB_CDC_1_STOP_BIT == line_coding->bCharFormat)
    sbmode = 0;
  else
    sbmode = SERCOM_USART_CTRLB_SBMODE;

  baud = F_CPU / (16 * line_coding->dwDTERate);
  fp = (F_CPU / line_coding->dwDTERate - 16 * baud) / 2;

  UART_SERCOM->USART.CTRLA.reg =
      SERCOM_USART_CTRLA_DORD | SERCOM_USART_CTRLA_MODE_USART_INT_CLK |
      SERCOM_USART_CTRLA_FORM(form) | SERCOM_USART_CTRLA_SAMPR(1) |
      SERCOM_USART_CTRLA_RXPO(UART_SERCOM_RXPO) |
      SERCOM_USART_CTRLA_TXPO(UART_SERCOM_TXPO);

  UART_SERCOM->USART.CTRLB.reg = SERCOM_USART_CTRLB_RXEN | SERCOM_USART_CTRLB_TXEN |
      SERCOM_USART_CTRLB_CHSIZE(chsize) | pmode | sbmode;

  UART_SERCOM->USART.BAUD.reg =
      SERCOM_USART_BAUD_FRACFP_BAUD(baud) | SERCOM_USART_BAUD_FRACFP_FP(fp);

  UART_SERCOM->USART.CTRLA.reg |= SERCOM_USART_CTRLA_ENABLE;

  UART_SERCOM->USART.INTENSET.reg = SERCOM_USART_INTENSET_RXC;

  NVIC_EnableIRQ(UART_SERCOM_IRQ_INDEX);
}

//-----------------------------------------------------------------------------
bool uart_write_byte(int byte)
{
  int wr = (uart_tx_fifo.wr + 1) % UART_BUF_SIZE;
  bool res = false;

  NVIC_DisableIRQ(UART_SERCOM_IRQ_INDEX);

  if (wr != uart_tx_fifo.rd)
  {
    uart_tx_fifo.data[uart_tx_fifo.wr] = byte;
    uart_tx_fifo.wr = wr;
    res = true;

    UART_SERCOM->USART.INTENSET.reg = SERCOM_USART_INTENSET_DRE;
  }

  NVIC_EnableIRQ(UART_SERCOM_IRQ_INDEX);

  return res;
}

//-----------------------------------------------------------------------------
bool uart_read_byte(int *byte)
{
  bool res = false;

  NVIC_DisableIRQ(UART_SERCOM_IRQ_INDEX);

  if (uart_rx_fifo.rd != uart_rx_fifo.wr)
  {
    *byte = uart_rx_fifo.data[uart_rx_fifo.rd];
    uart_rx_fifo.rd = (uart_rx_fifo.rd + 1) % UART_BUF_SIZE;
    res = true;
  }

  NVIC_EnableIRQ(UART_SERCOM_IRQ_INDEX);

  return res;
}

//-----------------------------------------------------------------------------
void UART_SERCOM_IRQ_HANDLER(void)
{
  int flags = UART_SERCOM->USART.INTFLAG.reg;

  if (flags & SERCOM_USART_INTFLAG_RXC)
  {
    int status = UART_SERCOM->USART.STATUS.reg;
    int byte = UART_SERCOM->USART.DATA.reg;
    int wr = (uart_rx_fifo.wr + 1) % UART_BUF_SIZE;

    if (status & SERCOM_USART_STATUS_BUFOVF)
      uart_serial_state_update(USB_CDC_SERIAL_STATE_OVERRUN);

    if (status & SERCOM_USART_STATUS_FERR)
      uart_serial_state_update(USB_CDC_SERIAL_STATE_FRAMING);

    if (status & SERCOM_USART_STATUS_PERR)
      uart_serial_state_update(USB_CDC_SERIAL_STATE_PARITY);

    if (wr == uart_rx_fifo.rd)
    {
      uart_serial_state_update(USB_CDC_SERIAL_STATE_OVERRUN);
    }
    else
    {
      uart_rx_fifo.data[uart_rx_fifo.wr] = byte;
      uart_rx_fifo.wr = wr;
    }
  }

  if (flags & SERCOM_USART_INTFLAG_DRE)
  {
    if (uart_tx_fifo.rd == uart_tx_fifo.wr)
    {
      UART_SERCOM->USART.INTENCLR.reg = SERCOM_USART_INTENCLR_DRE;
    }
    else
    {
      UART_SERCOM->USART.DATA.reg = uart_tx_fifo.data[uart_tx_fifo.rd];
      uart_tx_fifo.rd = (uart_tx_fifo.rd + 1) % UART_BUF_SIZE;
    }
  }
}

