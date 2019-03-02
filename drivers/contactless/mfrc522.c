/****************************************************************************
 * drivers/contactless/mfrc522.c
 *
 *   Copyright(C) 2016 Uniquix Ltda. All rights reserved.
 *   Author: Alan Carvalho de Assis <acassis@gmail.com>
 *
 * This driver is based on Arduino library for MFRC522 from Miguel
 * Balboa released into the public domain:
 * https://github.com/miguelbalboa/rfid/
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name NuttX nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES(INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <assert.h>

#include <nuttx/kmalloc.h>
#include <nuttx/signal.h>
#include <nuttx/contactless/mfrc522.h>

#include "mfrc522.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#ifdef CONFIG_CL_MFRC522_DEBUG
#  define mfrc522err    _err
#  define mfrc522info   _info
#else
#  ifdef CONFIG_CPP_HAVE_VARARGS
#    define mfrc522err(x...)
#    define mfrc522info(x...)
#  else
#    define mfrc522err  (void)
#    define mfrc522info (void)
#  endif
#endif

#ifdef CONFIG_CL_MFRC522_DEBUG_TX
#  define tracetx errdumpbuffer
#else
#  define tracetx(x...)
#endif

#ifdef CONFIG_CL_MFRC522_DEBUG_RX
#  define tracerx errdumpbuffer
#else
#  define tracerx(x...)
#endif

#define FRAME_SIZE(f) (sizeof(struct mfrc522_frame) + f->len + 2)
#define FRAME_POSTAMBLE(f) (f->data[f->len + 1])

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static inline void mfrc522_configspi(FAR struct spi_dev_s *spi);
static void mfrc522_lock(FAR struct spi_dev_s *spi);
static void mfrc522_unlock(FAR struct spi_dev_s *spi);

/* Character driver methods */

static int mfrc522_open(FAR struct file *filep);
static int mfrc522_close(FAR struct file *filep);
static ssize_t mfrc522_read(FAR struct file *filep, FAR char *buffer,
                            size_t buflen);
static ssize_t mfrc522_write(FAR struct file *filep,
                             FAR const char *buffer, size_t buflen);
static int mfrc522_ioctl(FAR struct file *filep, int cmd,
                         unsigned long arg);

uint8_t mfrc522_readu8(FAR struct mfrc522_dev_s *dev, uint8_t regaddr);
void mfrc522_writeu8(FAR struct mfrc522_dev_s *dev, uint8_t regaddr,
                     FAR uint8_t regval);
void mfrc522_writeblk(FAR struct mfrc522_dev_s *dev, uint8_t regaddr,
                      uint8_t *regval, int length);
void mfrc522_readblk(FAR struct mfrc522_dev_s *dev, uint8_t regaddr,
                     FAR uint8_t *regval, int length, uint8_t rxalign);

void mfrc522_softreset(FAR struct mfrc522_dev_s *dev);

int mfrc522_picc_select(FAR struct mfrc522_dev_s *dev,
                        FAR struct picc_uid_s *uid, uint8_t validbits);

#if 0 /* TODO */
/* IRQ Handling */

static int mfrc522_irqhandler(FAR int irq, FAR void *context, FAR void *dev);
static inline int mfrc522_attachirq(FAR struct mfrc522_dev_s *dev,
                                    xcpt_t isr);
#endif

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct file_operations g_mfrc522fops =
{
  mfrc522_open,
  mfrc522_close,
  mfrc522_read,
  mfrc522_write,
  0,
  mfrc522_ioctl
#ifndef CONFIG_DISABLE_POLL
    , 0
#endif
#ifndef CONFIG_DISABLE_PSEUDOFS_OPERATIONS
    , 0
#endif
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void mfrc522_lock(FAR struct spi_dev_s *spi)
{
  (void)SPI_LOCK(spi, true);

  SPI_SETMODE(spi, SPIDEV_MODE0);
  SPI_SETBITS(spi, 8);
  (void)SPI_HWFEATURES(spi, 0);
  (void)SPI_SETFREQUENCY(spi, CONFIG_MFRC522_SPI_FREQ);
}

static void mfrc522_unlock(FAR struct spi_dev_s *spi)
{
  (void)SPI_LOCK(spi, false);
}

static inline void mfrc522_configspi(FAR struct spi_dev_s *spi)
{
  /* Configure SPI for the MFRC522 module. */

  SPI_SETMODE(spi, SPIDEV_MODE0);
  SPI_SETBITS(spi, 8);
  (void)SPI_HWFEATURES(spi, 0);
  (void)SPI_SETFREQUENCY(spi, CONFIG_MFRC522_SPI_FREQ);
}

static inline void mfrc522_select(struct mfrc522_dev_s *dev)
{
  SPI_SELECT(dev->spi, SPIDEV_CONTACTLESS(0), true);
}

static inline void mfrc522_deselect(struct mfrc522_dev_s *dev)
{
  SPI_SELECT(dev->spi, SPIDEV_CONTACTLESS(0), false);
}

/****************************************************************************
 * Name: mfrc522_readu8
 *
 * Description:
 *   Read a byte from a register address.
 *
 * Input Parameters:
 *
 * Returned Value: the read byte from the register
 *
 ****************************************************************************/

uint8_t mfrc522_readu8(FAR struct mfrc522_dev_s *dev, uint8_t regaddr)
{
  uint8_t regval;
  uint8_t address = (0x80 | (regaddr & 0x7E));

  mfrc522_lock(dev->spi);
  mfrc522_select(dev);
  SPI_SEND(dev->spi, address);
  regval = SPI_SEND(dev->spi, 0);
  mfrc522_deselect(dev);
  mfrc522_unlock(dev->spi);

  tracerx("read", regval, 1);
  return regval;
}

/****************************************************************************
 * Name: mfrc522_write8
 *
 * Description:
 *   Write a byte to a register address.
 *
 * Input Parameters:
 *
 * Returned Value:
 *
 ****************************************************************************/

void mfrc522_writeu8(FAR struct mfrc522_dev_s *dev, uint8_t regaddr,
                     FAR uint8_t regval)
{
  mfrc522_lock(dev->spi);
  mfrc522_select(dev);
  SPI_SEND(dev->spi, regaddr & 0x7E);
  SPI_SEND(dev->spi, regval);
  mfrc522_deselect(dev);
  mfrc522_unlock(dev->spi);

  tracerx("write", &regval, 1);
}

/****************************************************************************
 * Name: mfrc522_readblk
 *
 * Description:
 *   Read a block of bytes from a register address. Align the bit positions of
 * regval[0] from rxalign..7.
 *
 * Input Parameters:
 *
 * Returned Value: none
 *
 ****************************************************************************/

void mfrc522_readblk(FAR struct mfrc522_dev_s *dev, uint8_t regaddr,
                     FAR uint8_t *regval, int length, uint8_t rxalign)
{
  uint8_t i = 0;
  uint8_t address = (0x80 | (regaddr & 0x7E));

  mfrc522_lock(dev->spi);
  mfrc522_select(dev);

  /* Inform the MFRC522 the address we want to read */

  SPI_SEND(dev->spi, address);

  while (i < length)
    {
      if (i == 0 && rxalign)
        {
          uint8_t mask = 0;
          uint8_t value;
          uint8_t j;

          for (j = rxalign; j <= 7; j++)
            {
              mask |= (1 << j);
            }

          /* Read the first byte */

          value = SPI_SEND(dev->spi, address);

          /* Apply mask to current regval[0] with the read value */

          regval[0] = (regval[0] & ~mask) | (value & mask);
        }
      else
        {
          /* Read the remaining bytes */

          regval[i] = SPI_SEND(dev->spi, address);
        }
      i++;
    }

  /* Read the last byte. Send 0 to stop reading (it maybe wrong, 1 byte out) */

  regval[i] = SPI_SEND(dev->spi, 0);

  mfrc522_deselect(dev);
  mfrc522_unlock(dev->spi);

  tracerx("readblk", regval, size);
}

/****************************************************************************
 * Name: mfrc522_writeblk
 *
 * Description:
 *   Write a block of bytes to a register address.
 *
 * Input Parameters:
 *
 * Returned Value: none
 *
 ****************************************************************************/

void mfrc522_writeblk(FAR struct mfrc522_dev_s *dev, uint8_t regaddr,
                      uint8_t *regval, int length)
{
  uint8_t address = (regaddr & 0x7E);

  mfrc522_lock(dev->spi);
  mfrc522_select(dev);

  /* Inform the MFRC522 the address we want write to */

  SPI_SEND(dev->spi, address);

  /* Send the block of bytes */

  SPI_SNDBLOCK(dev->spi, regval, length);

  mfrc522_deselect(dev);
  mfrc522_unlock(dev->spi);

  tracerx("writeblk", regval, size);
}

/****************************************************************************
 * Name: mfrc522_calc_crc
 *
 * Description:
 *   Use the CRC coprocessor in the MFRC522 to calculate a CRC_A.
 *
 * Input Parameters:
 *
 * Returned Value: OK or -ETIMEDOUT
 *
 ****************************************************************************/

int mfrc522_calc_crc(FAR struct mfrc522_dev_s *dev, uint8_t *buffer,
                     int length, uint8_t *result)
{
  struct timespec tstart;
  struct timespec tend;

  /* Stop any command in execution */

  mfrc522_writeu8(dev, MFRC522_COMMAND_REG, MFRC522_IDLE_CMD);

  /* Clear the CRCIRq interrupt request bit */

  mfrc522_writeu8(dev, MFRC522_DIV_IRQ_REG, MFRC522_CRC_IRQ);

  /* Flush all bytes in the FIFO */

  mfrc522_writeu8(dev, MFRC522_FIFO_LEVEL_REG, MFRC522_FLUSH_BUFFER);

  /* Write data to the FIFO */

  mfrc522_writeblk(dev, MFRC522_FIFO_DATA_REG, buffer, length);

  /* Start the calculation */

  mfrc522_writeu8(dev, MFRC522_COMMAND_REG, MFRC522_CALC_CRC_CMD);

  /* Wait for CRC completion or 200ms time-out */

  clock_gettime(CLOCK_REALTIME, &tstart);
  tstart.tv_nsec += 200000;
  if (tstart.tv_nsec >= 1000 * 1000 * 1000)
    {
      tstart.tv_sec++;
      tstart.tv_nsec -= 1000 * 1000 * 1000;
    }

  while (1)
    {
      uint8_t irqreg;

      irqreg = mfrc522_readu8(dev, MFRC522_DIV_IRQ_REG);
      if (irqreg & MFRC522_CRC_IRQ)
        {
          break;
        }

      /* Get time now */

      clock_gettime(CLOCK_REALTIME, &tend);

      if ((tend.tv_sec > tstart.tv_sec) && (tend.tv_nsec > tstart.tv_nsec))
        {
          return -ETIMEDOUT;
        }
    }

  /* Stop calculating CRC for new content of FIFO */

  mfrc522_writeu8(dev, MFRC522_COMMAND_REG, MFRC522_IDLE_CMD);

  result[0] = mfrc522_readu8(dev, MFRC522_CRC_RESULT_REGL);
  result[1] = mfrc522_readu8(dev, MFRC522_CRC_RESULT_REGH);

  return OK;
}

/****************************************************************************
 * Name: mfrc522_comm_picc
 *
 * Description:
 *   Transfers data to the MFRC522 FIFO, executes a command, waits for
 * completion and transfers data back from the FIFO.
 * CRC validation can only be done if back_data and back_len are specified.
 *
 * Input Parameters:
 *
 * Returned Value: OK or -ETIMEDOUT
 *
 ****************************************************************************/

int mfrc522_comm_picc(FAR struct mfrc522_dev_s *dev, uint8_t command,
                      uint8_t waitirq, uint8_t *send_data, uint8_t send_len,
                      uint8_t *back_data, uint8_t *back_len,
                      uint8_t *validbits, uint8_t rxalign, bool checkcrc)
{
  int ret;
  uint8_t errors;
  uint8_t vbits;
  uint8_t value;
  struct timespec tstart;
  struct timespec tend;

  /* Prepare values for BitFramingReg */

  uint8_t txlastbits = validbits ? *validbits : 0;
  uint8_t bitframing = (rxalign << 4) + txlastbits;

  /* Stop any active command */

  mfrc522_writeu8(dev, MFRC522_COMMAND_REG, MFRC522_IDLE_CMD);

  /* Clear all seven interrupt request bits */

  value = mfrc522_readu8(dev, MFRC522_COM_IRQ_REG);
  mfrc522_writeu8(dev, MFRC522_COM_IRQ_REG, value | MFRC522_COM_IRQ_MASK);

  /* Flush all bytes in the FIFO */

  mfrc522_writeu8(dev, MFRC522_FIFO_LEVEL_REG, MFRC522_FLUSH_BUFFER);

  /* Write data to FIFO */

  mfrc522_writeblk(dev, MFRC522_FIFO_DATA_REG, send_data, send_len);

  /* Bit adjustments */

  mfrc522_writeu8(dev, MFRC522_BIT_FRAMING_REG, bitframing);

  /* Execute command */

  mfrc522_writeu8(dev, MFRC522_COMMAND_REG, command);

  /* We setup the TAuto flag in the mfrc522_init() then we could use the
   * internal MFC522 Timer to detect timeout, but because there could be some
   * hardware fault, let us to use a NuttX timeout as well.
   */

  clock_gettime(CLOCK_REALTIME, &tstart);
  tstart.tv_nsec += 200000;
  if (tstart.tv_nsec >= 1000 * 1000 * 1000)
    {
      tstart.tv_sec++;
      tstart.tv_nsec -= 1000 * 1000 * 1000;
    }

  /* If it is a Transceive command, then start transmittion */

  if (command == MFRC522_TRANSCV_CMD)
    {
      value = mfrc522_readu8(dev, MFRC522_BIT_FRAMING_REG);
      mfrc522_writeu8(dev, MFRC522_BIT_FRAMING_REG,
                      value | MFRC522_START_SEND);
    }

  /* Wait for the command to complete */

  while (1)
    {
      uint8_t irqsreg;

      irqsreg = mfrc522_readu8(dev, MFRC522_COM_IRQ_REG);

      /* If at least an of selected IRQ happened */

      if (irqsreg & waitirq)
        {
          break;
        }

      /* Timer expired */

      if (irqsreg & MFRC522_TIMER_IRQ)
        {
          return -ETIMEDOUT;
        }

      /* Get time now */

      clock_gettime(CLOCK_REALTIME, &tend);

      if ((tend.tv_sec > tstart.tv_sec) && (tend.tv_nsec > tstart.tv_nsec))
        {
          return -ETIMEDOUT;
        }
    }

  /* Read error register to verify if there are any issue */

  errors = mfrc522_readu8(dev, MFRC522_ERROR_REG);

  /* Check for Protocol error */

  if (errors & (MFRC522_PROTO_ERR))
    {
      return -EPROTO;
    }

  /* Check for Parity and Buffer Overflow errors */

  if (errors & (MFRC522_PARITY_ERR | MFRC522_BUF_OVFL_ERR))
    {
      return -EIO;
    }

  /* Check collision error */

  if (errors & MFRC522_COLL_ERR)
    {
      return -EBUSY;            /* should it be EAGAIN ? */
    }

  /* If the caller wants data back, get it from the MFRC522 */

  if (back_data && back_len)
    {
      uint8_t nbytes;

      /* Number of bytes in the FIFO */

      nbytes = mfrc522_readu8(dev, MFRC522_FIFO_LEVEL_REG);

      /* Returned more bytes than the expected */

      if (nbytes > *back_len)
        {
          return -ENOMEM;
        }

      *back_len = nbytes;

      /* Read the data from FIFO */

      mfrc522_readblk(dev, MFRC522_FIFO_DATA_REG, back_data, nbytes, rxalign);

      /* RxLastBits[2:0] indicates the number of valid bits received */

      vbits = mfrc522_readu8(dev, MFRC522_CONTROL_REG)
              & MFRC522_RX_LAST_BITS_MASK;

      if (validbits)
        {
          *validbits = vbits;
        }
    }

  /* Perform CRC_A validation if requested */

  if (back_data && back_len && checkcrc)
    {
      uint8_t ctrlbuf[2];

      /* In this case a MIFARE Classic NAK is not OK */

      if (*back_len == 1 && vbits == 4)
        {
          return -EACCES;
        }

      /* We need the CRC_A value or all 8 bits of the last byte */

      if (*back_len < 2 || vbits != 0)
        {
          return -EPERM;
        }

      /* Verify CRC_A */

      ret = mfrc522_calc_crc(dev, &back_data[0], *back_len - 2, &ctrlbuf[0]);
      if (ret != OK)
        {
          return ret;
        }

      if ((back_data[*back_len - 2] != ctrlbuf[0]) ||
          (back_data[*back_len - 1] != ctrlbuf[1]))
        {
          return -EFAULT;
        }
    }

  return OK;
}

/****************************************************************************
 * Name: mfrc522_transcv_data
 *
 * Description:
 *   Executes the Transceive command
 *
 * Input Parameters:
 *
 * Returned Value: OK or -ETIMEDOUT
 *
 ****************************************************************************/

int mfrc522_transcv_data(FAR struct mfrc522_dev_s *dev, uint8_t *senddata,
                         uint8_t sendlen, uint8_t *backdata, uint8_t *backlen,
                         uint8_t *validbits, uint8_t rxalign, bool check_crc)
{
  uint8_t waitirq = MFRC522_RX_IRQ | MFRC522_IDLE_IRQ;

  return mfrc522_comm_picc(dev, MFRC522_TRANSCV_CMD, waitirq, senddata,
                           sendlen, backdata, backlen, validbits, rxalign,
                           check_crc);
}

/****************************************************************************
 * Name: mfrc522_picc_reqa_wupa
 *
 * Description:
 *   Transmits REQA or WUPA commands
 *
 * Input Parameters:
 *
 * Returned Value: OK or -ETIMEDOUT
 *
 ****************************************************************************/

int mfrc522_picc_reqa_wupa(FAR struct mfrc522_dev_s *dev, uint8_t command,
                           uint8_t *buffer, uint8_t length)
{
  uint8_t validbits;
  uint8_t value;
  int status;

  if (!buffer || length < 2)
    {
      return -EINVAL;
    }

  /* Force clear of received bits if a collision is detected */

  value = mfrc522_readu8(dev, MFRC522_COLL_REG);
  mfrc522_writeu8(dev, MFRC522_COLL_REG, value & MFRC522_VALUES_AFTER_COLL);

  validbits = 7;
  status = mfrc522_transcv_data(dev, &command, 1, buffer, &length, &validbits,
                                0, false);

  /* For REQA and WUPA we need to transmit only 7 bits */

  if (status != OK)
    {
      return status;
    }

  /* ATQA must be exactly 16 bits */

  if (length != 2 || validbits != 0)
    {
      return -EAGAIN;
    }

  mfrc522info("buffer[0]=0x%02X | buffer[1]=0x%02X\n", buffer[0], buffer[1]);
  return OK;
}

/****************************************************************************
 * Name: mfrc522_picc_request_a
 *
 * Description:
 *   Transmits a REQuest command, Type A. Invites PICCs in state IDLE to go to
 * READY and prepare for anticollision or selection.
 *
 * Input Parameters:
 *
 * Returned Value: OK or -ETIMEDOUT
 *
 ****************************************************************************/

int mfrc522_picc_request_a(FAR struct mfrc522_dev_s *dev, uint8_t *buffer,
                           uint8_t length)
{
  return mfrc522_picc_reqa_wupa(dev, PICC_CMD_REQA, buffer, length);
}

/****************************************************************************
 * Name: mfrc522_picc_detect
 *
 * Description:
 *   Detects if a Contactless Card is near
 *
 * Input Parameters:
 *
 * Returned Value: OK or -ETIMEDOUT
 *
 ****************************************************************************/

int mfrc522_picc_detect(FAR struct mfrc522_dev_s *dev)
{
  int ret;
  uint8_t buffer_atqa[2];
  uint8_t length = sizeof(buffer_atqa);

  /* Send a REQA command */

  ret = mfrc522_picc_request_a(dev, buffer_atqa, length);
  return (ret == OK || ret == -EBUSY);
}

/****************************************************************************
 * Name: mfrc522_picc_select
 *
 * Description:
 *   Selects a near Card and read its UID.
 *
 * Input Parameters:
 *
 * Returned Value: OK or -ETIMEDOUT
 *
 ****************************************************************************/

int mfrc522_picc_select(FAR struct mfrc522_dev_s *dev,
                        FAR struct picc_uid_s *uid, uint8_t validbits)
{
  bool uid_complete;
  bool select_done;
  bool use_cascade_tag;
  uint8_t cascade_level = 1;
  int result;
  uint8_t i;
  uint8_t value;
  uint8_t count;

  /* The first index in uid->data[] that is used in the current Cascade Level */

  uint8_t uid_index;

  /* The number of known UID bits in the current Cascade Level. */

  uint8_t curr_level_known_bits;

  /* The SELECT/ANTICOLLISION uses a 7 byte standard frame + 2 bytes CRC_A */

  uint8_t buffer[9];

  /* The number of bytes used in the buffer, number bytes on FIFO */

  uint8_t buffer_used;

  /* Used to defines the bit position for the first bit received */

  uint8_t rxalign;

  /* The number of valid bits in the last transmitted byte. */

  uint8_t txlastbits;

  uint8_t *resp_buf;
  uint8_t resp_len;

  /* Sanity check */

  if (validbits > 80)
    {
      return -EINVAL;
    }

  /* Force clear of received bits if a collision is detected */

  value = mfrc522_readu8(dev, MFRC522_COLL_REG);
  mfrc522_writeu8(dev, MFRC522_COLL_REG, value & MFRC522_VALUES_AFTER_COLL);

  /* Repeat cascade level loop until we have a complete UID */

  uid_complete = false;
  while (!uid_complete)
    {
      uint8_t bytes_to_copy;

      /* Set the Cascade Level in the SEL byte, find out if we need to use the
       * Cascade Tag in byte 2.
       */

      switch (cascade_level)
        {
        case 1:
          buffer[0] = PICC_CMD_SEL_CL1;
          uid_index = 0;

          /* When we know that the UID has more than 4 bytes */

          use_cascade_tag = validbits && (uid->size > 4);
          break;

        case 2:
          buffer[0] = PICC_CMD_SEL_CL2;
          uid_index = 3;

          /* When we know that the UID has more than 7 bytes */

          use_cascade_tag = validbits && (uid->size > 7);
          break;

        case 3:
          buffer[0] = PICC_CMD_SEL_CL3;
          uid_index = 6;
          use_cascade_tag = false;
          break;

        default:
          return -EIO;          /* Internal error */
        }

      /* How many UID bits are known in this Cascade Level? */

      curr_level_known_bits = validbits - (8 * uid_index);
      if (curr_level_known_bits < 0)
        {
          curr_level_known_bits = 0;
        }

      /* Copy the known bits from uid->uid_data[] to buffer[] */

      i = 2;                    /* destination index in buffer[] */
      if (use_cascade_tag)
        {
          buffer[i++] = PICC_CMD_CT;
        }

      /* Number of bytes needed to represent the known bits for this level */

      bytes_to_copy = curr_level_known_bits / 8 +
                      (curr_level_known_bits % 8 ? 1 : 0);

      if (bytes_to_copy)
        {
          /* Max 4 bytes in each Cascade Level. Only 3 left if we use the
           * Cascade Tag.
           */

          uint8_t max_bytes = use_cascade_tag ? 3 : 4;

          if (bytes_to_copy > max_bytes)
            {
              bytes_to_copy = max_bytes;
            }

          for (count = 0; count < bytes_to_copy; count++)
            {
              buffer[i++] = uid->uid_data[uid_index + count];
            }
        }

      /* Now that the data has been copied we need to include the 8 bits in CT
       * in curr_level_known_bits.
       */

      if (use_cascade_tag)
        {
          curr_level_known_bits += 8;
        }

      /* Repeat anti collision loop until we can transmit all UID bits + BCC
       * and receive a SAK - max 32 iterations.
       */

      select_done = false;
      while (!select_done)
        {
          /* Find out how many bits and bytes to send and receive. */

          if (curr_level_known_bits >= 32)
            {
              /* All UID bits in this Cascade Level are known. This is a
               * SELECT.
               */

              /* NVB - Number of Valid Bits: Seven whole bytes */

              buffer[1] = 0x70;

              /* Calculate BCC - Block Check Character */

              buffer[6] = buffer[2] ^ buffer[3] ^ buffer[4] ^ buffer[5];

              /* Calculate CRC_A */

              result = mfrc522_calc_crc(dev, buffer, 7, &buffer[7]);
              if (result != OK)
                {
                  return result;
                }

              txlastbits = 0;   /* 0 => All 8 bits are valid. */
              buffer_used = 9;

              /* Store response in the last 3 bytes of buffer (BCC and CRC_A -
               * not needed after tx).
               */

              resp_buf = &buffer[6];
              resp_len = 3;
            }
          else
            {
              /* This is an ANTICOLLISION */

              txlastbits = curr_level_known_bits % 8;

              /* Number of whole bytes in the UID part. */

              count = curr_level_known_bits / 8;
              i = 2 + count;

              /* NVB - Number of Valid Bits */

              buffer[1] = (i << 4) + txlastbits;
              buffer_used = i + (txlastbits ? 1 : 0);

              /* Store response in the unused part of buffer */

              resp_buf = &buffer[i];
              resp_len = sizeof(buffer) - i;
            }

          /* Set bit adjustments */

          rxalign = txlastbits;
          mfrc522_writeu8(dev, MFRC522_BIT_FRAMING_REG,
                          (rxalign << 4) + txlastbits);

          /* Transmit the buffer and receive the response */

          result = mfrc522_transcv_data(dev, buffer, buffer_used, resp_buf,
                                        &resp_len, &txlastbits, rxalign,
                                        false);

          /* More than one PICC in the field => collision */

          if (result == -EBUSY)
            {
              uint8_t coll_pos;
              uint8_t coll_reg = mfrc522_readu8(dev, MFRC522_COLL_REG);

              /* CollPosNotValid */

              if (coll_reg & 0x20)
                {
                  /* Without a valid collision position we cannot continue */

                  return -EBUSY;
                }

              coll_pos = coll_reg & 0x1F; /* Values 0-31, 0 means bit 32. */
              if (coll_pos == 0)
                {
                  coll_pos = 32;
                }

              if (coll_pos <= curr_level_known_bits)
                {
                  /* No progress - should not happen */

                  return -EIO;
                }

              /* Choose the PICC with the bit set. */

              curr_level_known_bits = coll_pos;

              /* The bit to modify */

              count = (curr_level_known_bits - 1) % 8;

              /* First byte is index 0. */

              i = 1 + (curr_level_known_bits / 8) + (count ? 1 : 0);
              buffer[i] |= (1 << count);
            }
          else if (result != OK)
            {
              return result;
            }
          else                  /* OK */
            {
              /* This was a SELECT. */

              if (curr_level_known_bits >= 32)
                {
                  /* No more collision */

                  select_done = true;
                }
              else
                {
                  /* This was an ANTICOLLISION. */

                  /* We have all 32 bits of the UID in this Cascade Level */

                  curr_level_known_bits = 32;

                  /* Run loop again to do the SELECT */
                }
            }
        }

      /* We do not check the CBB - it was constructed by us above. */

      /* Copy the found UID bytes from buffer[] to uid->uid_data[] */

      i = (buffer[2] == PICC_CMD_CT) ? 3 : 2;   /* source index in buffer[] */
      bytes_to_copy = (buffer[2] == PICC_CMD_CT) ? 3 : 4;

      for (count = 0; count < bytes_to_copy; count++)
        {
          uid->uid_data[uid_index + count] = buffer[i++];
        }

      /* Check response SAK (Select Acknowledge) */

      if (resp_len != 3 || txlastbits != 0)
        {
          /* SAK must be exactly 24 bits (1 byte + CRC_A). */

          return -EIO;
        }

      /* Verify CRC_A - do our own calculation and store the control in
       * buffer[2..3] - those bytes are not needed anymore.
       */

      result = mfrc522_calc_crc(dev, resp_buf, 1, &buffer[2]);
      if (result != OK)
        {
          return result;
        }

      /* Is it correct */

      if ((buffer[2] != resp_buf[1]) || (buffer[3] != resp_buf[2]))
        {
          return -EINVAL;
        }

      /* Cascade bit set - UID not complete yes */

      if (resp_buf[0] & 0x04)
        {
          cascade_level++;
        }
      else
        {
          uid_complete = true;
          uid->sak = resp_buf[0];
        }
    }

  /* Set correct uid->size */

  uid->size = 3 * cascade_level + 1;

  return OK;
}

/****************************************************************************
 * Name: mfrc522_softreset
 *
 * Description:
 *   Send a software reset command
 *
 * Input Parameters: a pointer to mfrc522_dev_s structure
 *
 * Returned Value: none
 *
 ****************************************************************************/

void mfrc522_softreset(FAR struct mfrc522_dev_s *dev)
{
  /* Send a software reset command */

  mfrc522_writeu8(dev, MFRC522_COMMAND_REG, MFRC522_SOFTRST_CMD);

  /* Wait the internal state machine to initialize */

  nxsig_usleep(50000);

  /* Wait for the PowerDown bit in COMMAND_REG to be cleared */

  while (mfrc522_readu8(dev, MFRC522_COMMAND_REG) & MFRC522_POWER_DOWN);
}

/****************************************************************************
 * Name: mfrc522_enableantenna
 *
 * Description:
 *   Turns the antenna on by enabling the pins TX1 and TX2
 *
 * Input Parameters: a pointer to mfrc522_dev_s structure
 *
 * Returned Value: none
 *
 ****************************************************************************/

void mfrc522_enableantenna(FAR struct mfrc522_dev_s *dev)
{
  uint8_t value = mfrc522_readu8(dev, MFRC522_TX_CTRL_REG);

  if ((value & (MFRC522_TX1_RF_EN | MFRC522_TX2_RF_EN)) != 0x03)
    {
      mfrc522_writeu8(dev, MFRC522_TX_CTRL_REG, value | 0x03);
    }
}

/****************************************************************************
 * Name: mfrc522_disableantenna
 *
 * Description:
 *   Turns the antenna off cutting the signals on TX1 and TX2
 *
 * Input Parameters: a pointer to mfrc522_dev_s structure
 *
 * Returned Value: none
 *
 ****************************************************************************/

void mfrc522_disableantenna(FAR struct mfrc522_dev_s *dev)
{
  uint8_t value = mfrc522_readu8(dev, MFRC522_TX_CTRL_REG);

  value &= ~(MFRC522_TX1_RF_EN | MFRC522_TX2_RF_EN);
  mfrc522_writeu8(dev, MFRC522_TX_CTRL_REG, value);
}

/****************************************************************************
 * Name: mfrc522_getfwversion
 *
 * Description:
 *   Read the MFRC522 firmware version.
 *
 * Input Parameters: a pointer to mfrc522_dev_s structure
 *
 * Returned Value: the firmware version byte
 *
 ****************************************************************************/

uint8_t mfrc522_getfwversion(FAR struct mfrc522_dev_s *dev)
{
  return mfrc522_readu8(dev, MFRC522_VERSION_REG);
}

/****************************************************************************
 * Name: mfrc522_getantennagain
 *
 * Description:
 *   Read the MFRC522 receiver gain (RxGain).
 * See 9.3.3.6 / table 98 in MFRC522 datasheet.
 *
 * Input Parameters: a pointer to mfrc522_dev_s structure
 *
 * Returned Value: none
 *
 ****************************************************************************/

uint8_t mfrc522_getantennagain(FAR struct mfrc522_dev_s *dev)
{
  return mfrc522_readu8(dev, MFRC522_RF_CFG_REG) & MFRC522_RX_GAIN_MASK;
}

/****************************************************************************
 * Name: mfrc522_setantennagain
 *
 * Description:
 *   Set the MFRC522 receiver gain (RxGain) to value value specified in mask.
 * See 9.3.3.6 / table 98 in MFRC522 datasheet.
 *
 * Input Parameters: a pointer to mfrc522_dev_s structure
 *
 * Returned Value: none
 *
 ****************************************************************************/

void mfrc522_setantennagain(FAR struct mfrc522_dev_s *dev, uint8_t mask)
{
  uint8_t value;

  if ((value = mfrc522_getantennagain(dev)) != mask)
    {
      mfrc522_writeu8(dev, MFRC522_RF_CFG_REG, value & ~MFRC522_RX_GAIN_MASK);
      mfrc522_writeu8(dev, MFRC522_RF_CFG_REG, mask & MFRC522_RX_GAIN_MASK);
    }
}

/****************************************************************************
 * Name: mfrc522_init
 *
 * Description:
 *   Initializes the MFRC522 chip
 *
 * Input Parameters: a pointer to mfrc522_dev_s structure
 *
 * Returned Value: none
 *
 ****************************************************************************/

void mfrc522_init(FAR struct mfrc522_dev_s *dev)
{
  /* Force a reset */

  mfrc522_softreset(dev);

  /* We need a timeout if something when communicating with a TAG case
   * something goes wrong. f_timer = 13.56 MHz / (2*TPreScaler+1) where:
   * TPreScaler = [TPrescaler_Hi:Tprescaler_Lo]. Tprescaler_Hi are the four
   * low bits in TmodeReg. Tprescaler_Lo is on TPrescalerReg.
   *
   * TAuto=1; timer starts automatically at the end of the transmission in
   * all communication modes at all speeds.
   */

  mfrc522_writeu8(dev, MFRC522_TMODE_REG, MFRC522_TAUTO);

  /* TPreScaler = TModeReg[3..0]:TPrescalerReg, ie: 0x0A9 = 169 =>
   * f_timer=40kHz, then the timer period will be 25us.
   */

  mfrc522_writeu8(dev, MFRC522_TPRESCALER_REG, 0xA9);

  /* Reload timer with 0x3E8 = 1000, ie 25ms before timeout. */

  mfrc522_writeu8(dev, MFRC522_TRELOAD_REGH, 0x06);
  mfrc522_writeu8(dev, MFRC522_TRELOAD_REGL, 0xE8);

  /* Force 100% ASK modulation independent of the ModGsPReg setting */

  mfrc522_writeu8(dev, MFRC522_TX_ASK_REG, MFRC522_FORCE_100ASK);

  /* Set the preset value for the CRC to 0x6363 (ISO 14443-3 part 6.2.4) */

  mfrc522_writeu8(dev, MFRC522_MODE_REG, 0x3D);

  /* Enable the Antenna pins */

  mfrc522_enableantenna(dev);
}

/****************************************************************************
 * Name: mfrc522_selftest
 *
 * Description:
 *   Executes a self-test of the MFRC522 chip
 *
 * See 16.1.1 in the MFRC522 datasheet
 *
 * Input Parameters: a pointer to mfrc522_dev_s structure
 *
 * Returned Value: none
 *
 ****************************************************************************/

int mfrc522_selftest(FAR struct mfrc522_dev_s *dev)
{
  uint8_t zeros[25] =
  {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0
  };
  char outbuf[3 * 8 + 1];
  uint8_t result[64];
  int i;
  int j;
  int k;

  /* Execute a software reset */

  mfrc522_softreset(dev);

  /* Flush the FIFO buffer */

  mfrc522_writeu8(dev, MFRC522_FIFO_LEVEL_REG, MFRC522_FLUSH_BUFFER);

  /* Clear the internal buffer by writing 25 bytes 0x00 */

  mfrc522_writeblk(dev, MFRC522_FIFO_DATA_REG, zeros, 25);

  /* Transfer to internal buffer */

  mfrc522_writeu8(dev, MFRC522_COMMAND_REG, MFRC522_MEM_CMD);

  /* Enable self-test */

  mfrc522_writeu8(dev, MFRC522_AUTOTEST_REG, MFRC522_SELFTEST_EN);

  /* Write 0x00 to FIFO buffer */

  mfrc522_writeu8(dev, MFRC522_FIFO_DATA_REG, 0x00);

  /* Start self-test by issuing the CalcCRC command */

  mfrc522_writeu8(dev, MFRC522_COMMAND_REG, MFRC522_CALC_CRC_CMD);

  /* Wait for self-test to complete */

  for (i = 0; i < 255; i++)
    {
      uint8_t n;

      n = mfrc522_readu8(dev, MFRC522_DIV_IRQ_REG);
      if (n & MFRC522_CRC_IRQ)
        {
          break;
        }
    }

  /* Stop calculating CRC for new content in the FIFO */

  mfrc522_writeu8(dev, MFRC522_COMMAND_REG, MFRC522_IDLE_CMD);

  /* Read out the 64 bytes result from the FIFO buffer */

  mfrc522_readblk(dev, MFRC522_FIFO_DATA_REG, result, 64, 0);

  /* Self-test done. Reset AutoTestReg register to normal operation */

  mfrc522_writeu8(dev, MFRC522_AUTOTEST_REG, 0x00);

  mfrc522info("Self Test Result:\n");

  for (i = 0; i < 64; i += 8)
    {
      for (j = 0, k = 0; j < 8; j++, k += 3)
        {
          (void)sprintf(&outbuf[k], " %02x", result[i + j]);
        }

      mfrc522info("  %02x:%s\n", i, outbuf);
    }

  mfrc522info("Done!\n");
  return OK;
}

/****************************************************************************
 * Name: mfrc522_mifare_transceive
 *
 * Description:
 *   Wrapper for MIFARE protocol communication.
 *
 * Adds CRC_A, executes the Transceive command and checks that the response is MF_ACK or a timeout.
 *
 ****************************************************************************/

int mfrc522_mifare_transceive(FAR struct mfrc522_dev_s *dev, uint8_t *sendData, uint8_t sendLen, uint8_t acceptTimeout)
{
	int result;
	uint8_t cmdBuffer[18]; // We need room for 16 bytes data and 2 bytes CRC_A.

	// Sanity check
	if (sendData == NULL || sendLen < 16) {
		return EINVAL;
	}

	// Copy sendData[] to cmdBuffer[] and add CRC_A
	memcpy(cmdBuffer, sendData, sendLen);
	result = mfrc522_calc_crc(dev, cmdBuffer, sendLen, &cmdBuffer[sendLen]);
	if (result != OK) {
		return result;
	}
	sendLen += 2;

	// Transceive the data, store the reply in cmdBuffer[]
	uint8_t waitIRq = 0x30; // RxIRq and IdleIRq
	uint8_t cmdBufferSize = sizeof(cmdBuffer);
	uint8_t validBits = 0;
	result = mfrc522_comm_picc(dev, MFRC522_TRANSCV_CMD, waitIRq, cmdBuffer, sendLen, cmdBuffer, &cmdBufferSize, &validBits, 0, false);
	if (acceptTimeout && result == ETIMEDOUT) {
		return OK;
	}
	if (result != OK) {
		return result;
	}
	// The PICC must reply with a 4 bit ACK
	if (cmdBufferSize != 1 || validBits != 4) {
		return EPROTO;
	}
	if (cmdBuffer[0] != 0xA) {
		return EPROTO;
	}
	return OK;
}

/****************************************************************************
 * Name: mfrc522_mifare_read
 *
 * Description:
 *   Reads 16 bytes (+ 2 bytes CRC_A) from the active PICC.
 *
 * For MIFARE Classic the sector containing the block must be authenticated before calling this function.
 *
 * For MIFARE Ultralight only addresses 00h to 0Fh are decoded.
 * The MF0ICU1 returns a NAK for higher addresses.
 * The MF0ICU1 responds to the READ command by sending 16 bytes starting from the page address defined by the command argument.
 * For example; if blockAddr is 03h then pages 03h, 04h, 05h, 06h are returned.
 * A roll-back is implemented: If blockAddr is 0Eh, then the contents of pages 0Eh, 0Fh, 00h and 01h are returned.
 *
 * The buffer must be at least 18 bytes because a CRC_A is also returned.
 * Checks the CRC_A before returning OK.
 *
 ****************************************************************************/

int mfrc522_mifare_read(FAR struct mfrc522_dev_s *dev, uint8_t blockAddr, uint8_t *buffer, uint8_t *bufferSize)
{
	int result;

	// Sanity check
	if (buffer == NULL || *bufferSize < 18) {
		return ENOBUFS;
	}

	// Build command buffer
	buffer[0] = PICC_CMD_MF_READ;
	buffer[1] = blockAddr;
	// Calculate CRC_A
	result = mfrc522_calc_crc(dev, buffer, 2, &buffer[2]);
	if (result != OK) {
		return result;
	}

	// Transmit the buffer and receive the response, validate CRC_A.
	return mfrc522_transcv_data(dev, buffer, 4, buffer, bufferSize, NULL, 0, true);
}

/****************************************************************************
 * Name: mfrc522_mifare_write
 *
 * Description:
 *   Writes 16 bytes to the active PICC.
 *
 * For MIFARE Classic the sector containing the block must be authenticated before calling this function.
 *
 * For MIFARE Ultralight the operation is called "COMPATIBILITY WRITE".
 * Even though 16 bytes are transferred to the Ultralight PICC, only the least significant 4 bytes (bytes 0 to 3)
 * are written to the specified address. It is recommended to set the remaining bytes 04h to 0Fh to all logic 0.
 *
 ****************************************************************************/

int mfrc522_mifare_write(FAR struct mfrc522_dev_s *dev, uint8_t blockAddr, uint8_t *buffer, uint8_t *bufferSize)
{
	int result;

	// Sanity check
	if (buffer == NULL || *bufferSize < 16) {
		return EINVAL;
	}

	// Mifare Classic protocol requires two communications to perform a write.
	// Step 1: Tell the PICC we want to write to block blockAddr.
	uint8_t cmdBuffer[2];
	cmdBuffer[0] = PICC_CMD_MF_WRITE;
	cmdBuffer[1] = blockAddr;
	result = mfrc522_mifare_transceive(dev, cmdBuffer, 2, false); // Adds CRC_A and checks that the response is MF_ACK.
	if (result != OK) {
		return result;
	}

	// Step 2: Transfer the data
	result = mfrc522_mifare_transceive(dev, buffer, *bufferSize, false); // Adds CRC_A and checks that the response is MF_ACK.
	if (result != OK) {
		return result;
	}

	return OK;
}

/****************************************************************************
 * Name: mfrc522_open
 *
 * Description:
 *   This function is called whenever the MFRC522 device is opened.
 *
 ****************************************************************************/

static int mfrc522_open(FAR struct file *filep)
{
  FAR struct inode *inode;
  FAR struct mfrc522_dev_s *dev;

  DEBUGASSERT(filep);
  inode = filep->f_inode;

  DEBUGASSERT(inode && inode->i_private);
  dev = inode->i_private;

  mfrc522_configspi(dev->spi);

  nxsig_usleep(10000);

  mfrc522_getfwversion(dev);

  dev->state = MFRC522_STATE_IDLE;
  return OK;
}

/****************************************************************************
 * Name: mfrc522_close
 *
 * Description:
 *   This routine is called when the MFRC522 device is closed.
 *
 ****************************************************************************/

static int mfrc522_close(FAR struct file *filep)
{
  FAR struct inode *inode;
  FAR struct mfrc522_dev_s *dev;

  DEBUGASSERT(filep);
  inode = filep->f_inode;

  DEBUGASSERT(inode && inode->i_private);
  dev = inode->i_private;

  dev->state = MFRC522_STATE_NOT_INIT;

  return OK;
}

/****************************************************************************
 * Name: mfrc522_read
 *
 * Description:
 *   This routine is called when the device is read.
 *
 * Returns TAG id as string to buffer.
 * or -EIO if no TAG found
 *
 ****************************************************************************/

static ssize_t mfrc522_read(FAR struct file *filep, FAR char *buffer,
                            size_t buflen)
{
  FAR struct inode *inode;
  FAR struct mfrc522_dev_s *dev;
  FAR struct picc_uid_s uid;

  DEBUGASSERT(filep);
  inode = filep->f_inode;

  DEBUGASSERT(inode && inode->i_private);
  dev = inode->i_private;

  /* Is a card near? */

  if (!mfrc522_picc_detect(dev))
    {
      mfrc522err("Card is not present!\n");
      return -EAGAIN;
    }

  /* Now read the UID */

  mfrc522_picc_select(dev, &uid, 0);

  if (uid.sak != 0)
    {
      if (buffer)
        {
          snprintf(buffer, buflen, "0x%02X%02X%02X%02X",
                   uid.uid_data[0], uid.uid_data[1],
                   uid.uid_data[2], uid.uid_data[3]);
          return buflen;
        }
    }

  return OK;
}

/****************************************************************************
 * Name: mfrc522_write
 ****************************************************************************/

static ssize_t mfrc522_write(FAR struct file *filep, FAR const char *buffer,
                             size_t buflen)
{
  FAR struct inode *inode;
  FAR struct mfrc522_dev_s *dev;

  DEBUGASSERT(filep);
  inode = filep->f_inode;

  DEBUGASSERT(inode && inode->i_private);
  dev = inode->i_private;

  (void)dev;

  return -ENOSYS;
}

/****************************************************************************
 * Name: mfrc522_ioctl
 ****************************************************************************/

static int mfrc522_ioctl(FAR struct file *filep, int cmd, unsigned long arg)
{
  FAR struct inode *inode;
  FAR struct mfrc522_dev_s *dev;
  int ret = OK;

  DEBUGASSERT(filep);
  inode = filep->f_inode;

  DEBUGASSERT(inode && inode->i_private);
  dev = inode->i_private;

  switch (cmd)
    {
    case MFRC522IOC_GET_PICC_UID:
      {
        struct picc_uid_s *uid = (struct picc_uid_s *)arg;

        /* Is a card near? */

        if (mfrc522_picc_detect(dev))
          {
            ret = mfrc522_picc_select(dev, uid, 0);
          }
      }
      break;

    case MFRC522IOC_GET_STATE:
      ret = dev->state;
      break;

    default:
      mfrc522err("ERROR: Unrecognized cmd: %d\n", cmd);
      ret = -ENOTTY;
      break;
    }

  return ret;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: mfrc522_register
 *
 * Description:
 *   Register the MFRC522 character device as 'devpath'
 *
 * Input Parameters:
 *   devpath - The full path to the driver to register.
 *             E.g., "/dev/rfid0"
 *   spi     - An instance of the SPI interface to use to communicate with
 *             MFRC522.
 *   config  - chip config
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno value on failure.
 *
 ****************************************************************************/

int mfrc522_register(FAR const char *devpath, FAR struct spi_dev_s *spi)
{
  FAR struct mfrc522_dev_s *dev;
  uint8_t fwver;
  int ret = 0;

  /* Initialize the MFRC522 device structure */

  dev = (FAR struct mfrc522_dev_s *)kmm_malloc(sizeof(struct mfrc522_dev_s));
  if (!dev)
    {
      mfrc522err("ERROR: Failed to allocate instance\n");
      return -ENOMEM;
    }

  dev->spi = spi;

  /* Device is not initialized yet */

  dev->state = MFRC522_STATE_NOT_INIT;

#if defined CONFIG_PM
  dev->pm_level = PM_IDLE;
#endif

  /* mfrc522_attachirq(dev, mfrc522_irqhandler); */

  /* Initialize the MFRC522 */

  mfrc522_init(dev);

  /* Device initialized and idle */

  dev->state = MFRC522_STATE_IDLE;

  /* Read the Firmware Version */

  fwver = mfrc522_getfwversion(dev);

  mfrc522info("MFRC522 Firmware Version: 0x%02X!\n", fwver);

  /* If returned firmware version is unknown don't register the device */

  if (fwver != 0x90 && fwver != 0x91 && fwver != 0x92 && fwver != 0x88)
    {
      mfrc522err("None supported device detected!\n");
      goto firmware_error;
    }

  /* Register the character driver */

  ret = register_driver(devpath, &g_mfrc522fops, 0666, dev);
  if (ret < 0)
    {
      mfrc522err("ERROR: Failed to register driver: %d\n", ret);
      kmm_free(dev);
    }

  return ret;

firmware_error:
  kmm_free(dev);
  return -ENODEV;
}
