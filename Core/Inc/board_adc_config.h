/* B1电压采样基准；只影响AD7124读数换算，不改变DDR目标电压。 */
#ifndef BOARD_ADC_CONFIG_H
#define BOARD_ADC_CONFIG_H

/* 0使用内部2.5V；1使用AVDD。修改后须重编译并重新初始化ADC。 */
#ifndef BOARD_AD_USE_AVDD_REFERENCE
#define BOARD_AD_USE_AVDD_REFERENCE 1U
#endif

/* 用万用表测得的AVDD对AVSS电压，单位mV。换板或供电条件变化时应重新测量。 */
#ifndef BOARD_AD_AVDD_MV
#define BOARD_AD_AVDD_MV 3355U
#endif

#if BOARD_AD_USE_AVDD_REFERENCE != 0U && BOARD_AD_USE_AVDD_REFERENCE != 1U
#error "BOARD_AD_USE_AVDD_REFERENCE must be 0 or 1"
#endif
#if BOARD_AD_USE_AVDD_REFERENCE && (BOARD_AD_AVDD_MV < 2700U || BOARD_AD_AVDD_MV > 3600U)
#error "BOARD_AD_AVDD_MV outside full-power supply range"
#endif

#if BOARD_AD_USE_AVDD_REFERENCE
#define BOARD_AD_REFERENCE_MV BOARD_AD_AVDD_MV
#else
#define BOARD_AD_REFERENCE_MV 2500U
#endif

#endif
