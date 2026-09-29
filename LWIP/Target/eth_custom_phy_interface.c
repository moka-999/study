/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    eth_custom_phy_interface.c
  * @author  MCD Application Team
  * @brief   This file provides a set of functions needed to manage
  *			 the ethernet phy.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2024 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* Private includes ----------------------------------------------------------*/
#include "eth_custom_phy_interface.h"

/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
#define USER_PHY_SW_RESET_TO    ((uint32_t)500U)
#define USER_PHY_MAX_DEV_ADDR   ((uint32_t)31U)
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/**
  * @brief  Register IO functions to component object
  * @param  pObj: device object of user_phy_Object_t.
  * @param  ioctx: holds device IO functions.
  * @retval USER_PHY_STATUS_OK  if OK
  *         USER_PHY_STATUS_ERROR if missing mandatory function
  */
int32_t  USER_PHY_RegisterBusIO(user_phy_Object_t *pObj, user_phy_IOCtx_t *ioctx)
{
  if(!pObj || !ioctx || !ioctx->ReadReg || !ioctx->WriteReg || !ioctx->GetTick)
  {
    return USER_PHY_STATUS_ERROR;
  }

  pObj->IO.Init = ioctx->Init;
  pObj->IO.DeInit = ioctx->DeInit;
  pObj->IO.ReadReg = ioctx->ReadReg;
  pObj->IO.WriteReg = ioctx->WriteReg;
  pObj->IO.GetTick = ioctx->GetTick;

  return USER_PHY_STATUS_OK;
}

/* 扫描0..31，仅接受YT8512C/H完整ID，不把浮空总线当作器件。 */
int32_t USER_PHY_Init(user_phy_Object_t *pObj)
{
  if (!pObj || !pObj->IO.ReadReg || !pObj->IO.WriteReg || !pObj->IO.GetTick)
    return USER_PHY_STATUS_ERROR;
  if (pObj->Is_Initialized) return USER_PHY_STATUS_OK;
  if (pObj->IO.Init && pObj->IO.Init() < 0) return USER_PHY_STATUS_ERROR;

  pObj->DevAddr = USER_PHY_MAX_DEV_ADDR + 1U;
  for (uint32_t addr = 0U; addr <= USER_PHY_MAX_DEV_ADDR; ++addr) {
    uint32_t id1, id2;
    if (pObj->IO.ReadReg(addr, USER_PHY_PHYI1R, &id1) < 0 ||
        pObj->IO.ReadReg(addr, USER_PHY_PHYI2R, &id2) < 0) continue;
    if (id1 == USER_PHY_EXPECTED_ID1 && id2 == USER_PHY_EXPECTED_ID2) {
      pObj->DevAddr = addr;
      pObj->Is_Initialized = 1U;
      return USER_PHY_STATUS_OK;
    }
  }
  return USER_PHY_STATUS_ADDRESS_ERROR;
}

/**
  * @brief  De-Initialize the USER_PHY and it's hardware resources
  * @param  pObj: device object user_phy_Object_t.
  * @retval None
  */
int32_t USER_PHY_DeInit(user_phy_Object_t *pObj)
{
  if(pObj->Is_Initialized)
  {
    if(pObj->IO.DeInit != 0)
    {
      if(pObj->IO.DeInit() < 0)
      {
        return USER_PHY_STATUS_ERROR;
      }
    }

    pObj->Is_Initialized = 0;
  }

  return USER_PHY_STATUS_OK;
}

/**
  * @brief  Perform a software reset of the USER_PHY and wait for its completion
  * @param  pObj: device object user_phy_Object_t.
  * @retval USER_PHY_STATUS_OK           if the reset completed successfully
  *         USER_PHY_STATUS_WRITE_ERROR  if writing the reset command failed
  *         USER_PHY_STATUS_READ_ERROR   if reading the register failed during reset
  *         USER_PHY_STATUS_RESET_TIMEOUT if the reset did not complete within the timeout period
  */
int32_t USER_PHY_SoftwareReset(user_phy_Object_t *pObj)
{
  int32_t status = USER_PHY_STATUS_OK;
  uint32_t tickstart = 0;
  uint32_t regvalue = 0;

  /* Set software reset bit */
  if (pObj->IO.WriteReg(pObj->DevAddr, USER_PHY_BCR, USER_PHY_BCR_SOFT_RESET) < 0)
  {
    return USER_PHY_STATUS_WRITE_ERROR;
  }

  /* Read register to check reset status */
  if (pObj->IO.ReadReg(pObj->DevAddr, USER_PHY_BCR, &regvalue) < 0)
  {
    return USER_PHY_STATUS_READ_ERROR;
  }

  tickstart = pObj->IO.GetTick();

  /* Wait until reset bit is cleared or timeout */
  while (regvalue & USER_PHY_BCR_SOFT_RESET)
  {
    if (((uint32_t)pObj->IO.GetTick() - tickstart) > USER_PHY_SW_RESET_TO)
    {
      status = USER_PHY_STATUS_RESET_TIMEOUT;
      break;
    }

    if (pObj->IO.ReadReg(pObj->DevAddr, USER_PHY_BCR, &regvalue) < 0)
    {
      status = USER_PHY_STATUS_READ_ERROR;
      break;
    }
  }

  return status;
}

/**
 * @brief 读取已解析的真实速率/双工，不从双方能力广播推测工作模式。
 * @return 四种10/100M工作模式、未连接/未解析或负值IO错误。
 */
int32_t USER_PHY_GetLinkState(user_phy_Object_t *pObj)
{
  uint32_t status;
  if (!pObj || !pObj->Is_Initialized || !pObj->IO.ReadReg)
    return USER_PHY_STATUS_ERROR;
  /* BSR链路位低锁存，读两次排除历史掉线。 */
  if (pObj->IO.ReadReg(pObj->DevAddr, USER_PHY_BSR, &status) < 0 ||
      pObj->IO.ReadReg(pObj->DevAddr, USER_PHY_BSR, &status) < 0)
    return USER_PHY_STATUS_READ_ERROR;
  if ((status & USER_PHY_BSR_LINK_STATUS) == 0U) return USER_PHY_STATUS_LINK_DOWN;
  if (pObj->IO.ReadReg(pObj->DevAddr, USER_PHY_SPECIFIC_STATUS, &status) < 0)
    return USER_PHY_STATUS_READ_ERROR;
  if ((status & USER_PHY_REALTIME_LINK) == 0U) return USER_PHY_STATUS_LINK_DOWN;
  if ((status & USER_PHY_RESOLVED) == 0U) return USER_PHY_STATUS_AUTONEGO_NOTDONE;

  uint32_t speed = status & USER_PHY_RESOLVED_SPEED;
  if (speed != 0U && speed != 0x4000U) return USER_PHY_STATUS_ERROR;
  if (speed == 0x4000U)
    return (status & USER_PHY_RESOLVED_DUPLEX) ? USER_PHY_STATUS_100MBITS_FULLDUPLEX :
                                              USER_PHY_STATUS_100MBITS_HALFDUPLEX;
  return (status & USER_PHY_RESOLVED_DUPLEX) ? USER_PHY_STATUS_10MBITS_FULLDUPLEX :
                                            USER_PHY_STATUS_10MBITS_HALFDUPLEX;
}

/**
 * @brief 强制10/100M速率与双工；参数或读取失败时不写PHY。
 * @param LinkState 仅接受四种USER_PHY_STATUS_xxMBITS_xxDUPLEX。
 * @return 0成功；负值表示参数、MDIO读取或写入失败。
 */
int32_t USER_PHY_SetLinkState(user_phy_Object_t *pObj, uint32_t LinkState)
{
  uint32_t control;
  if (!pObj || !pObj->Is_Initialized || !pObj->IO.ReadReg || !pObj->IO.WriteReg ||
      LinkState < USER_PHY_STATUS_100MBITS_FULLDUPLEX ||
      LinkState > USER_PHY_STATUS_10MBITS_HALFDUPLEX) return USER_PHY_STATUS_ERROR;
  if (pObj->IO.ReadReg(pObj->DevAddr, USER_PHY_BCR, &control) < 0)
    return USER_PHY_STATUS_READ_ERROR;
  control &= ~(USER_PHY_BCR_AUTONEGO_EN | USER_PHY_BCR_SPEED_SELECT | USER_PHY_BCR_DUPLEX_MODE);
  if (LinkState == USER_PHY_STATUS_100MBITS_FULLDUPLEX ||
      LinkState == USER_PHY_STATUS_100MBITS_HALFDUPLEX) control |= USER_PHY_BCR_SPEED_SELECT;
  if (LinkState == USER_PHY_STATUS_100MBITS_FULLDUPLEX ||
      LinkState == USER_PHY_STATUS_10MBITS_FULLDUPLEX) control |= USER_PHY_BCR_DUPLEX_MODE;
  return pObj->IO.WriteReg(pObj->DevAddr, USER_PHY_BCR, control) < 0 ?
         USER_PHY_STATUS_WRITE_ERROR : USER_PHY_STATUS_OK;
}

/**
  * @brief  Enable loopback mode.
  * @param  pObj: Pointer to device object.
  * @retval USER_PHY_STATUS_OK  if OK
  *         USER_PHY_STATUS_READ_ERROR if cannot read register
  *         USER_PHY_STATUS_WRITE_ERROR if cannot write to register
  */
int32_t USER_PHY_EnableLoopbackMode(user_phy_Object_t *pObj)
{
  uint32_t readval = 0;
  int32_t status = USER_PHY_STATUS_OK;

  if(pObj->IO.ReadReg(pObj->DevAddr, USER_PHY_BCR, &readval) >= 0)
  {
    readval |= USER_PHY_BCR_LOOPBACK;

    /* Apply configuration */
    if(pObj->IO.WriteReg(pObj->DevAddr, USER_PHY_BCR, readval) < 0)
    {
      status = USER_PHY_STATUS_WRITE_ERROR;
    }
  }
  else
  {
    status = USER_PHY_STATUS_READ_ERROR;
  }

  return status;
}

/**
  * @brief  Disable loopback mode.
  * @param  pObj: Pointer to device object.
  * @retval USER_PHY_STATUS_OK  if OK
  *         USER_PHY_STATUS_READ_ERROR if cannot read register
  *         USER_PHY_STATUS_WRITE_ERROR if cannot write to register
  */
int32_t USER_PHY_DisableLoopbackMode(user_phy_Object_t *pObj)
{
  uint32_t readval = 0;
  int32_t status = USER_PHY_STATUS_OK;

  if(pObj->IO.ReadReg(pObj->DevAddr, USER_PHY_BCR, &readval) >= 0)
  {
    readval &= ~USER_PHY_BCR_LOOPBACK;

    /* Apply configuration */
    if(pObj->IO.WriteReg(pObj->DevAddr, USER_PHY_BCR, readval) < 0)
    {
      status =  USER_PHY_STATUS_WRITE_ERROR;
    }
  }
  else
  {
    status = USER_PHY_STATUS_READ_ERROR;
  }

  return status;
}

/* USER CODE BEGIN 1 */

/* USER CODE END 1 */
